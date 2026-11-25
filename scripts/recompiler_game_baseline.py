#!/usr/bin/env python3
"""Run and review a local real-game JIT, Hybrid, and strict-static baseline."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

MODES = ("jit", "hybrid", "static")
DEFAULT_REPLAY = Path(__file__).parent / "mk8d-tas" / "script0-1.txt"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def tree_sha256(root):
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Use a self-contained input directory: {path}")
        if path.is_file():
            relative = path.relative_to(root).as_posix().encode()
            digest.update(len(relative).to_bytes(4, "little"))
            digest.update(relative)
            digest.update(bytes.fromhex(sha256(path)))
    return digest.hexdigest()


def exefs_sha256(directory):
    # Keep the fingerprint identical to HashExeFsFiles in the artifact exporter.
    digest = hashlib.sha256()
    for path in sorted(directory.iterdir(), key=lambda path: path.name.encode()):
        if path.is_file() and path.name.lower() != "romfs.bin":
            name = path.name.encode()
            digest.update(str(len(name)).encode() + b":" + name + b":" +
                          str(path.stat().st_size).encode() + b":")
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
                    digest.update(chunk)
    return digest.hexdigest()


def replay_length(replay):
    frame = -1
    for line in replay.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        command = line.split(" ")
        if len(command) != 4 or not command[0].isascii() or not command[0].isdigit():
            raise ValueError(f"Invalid TAS command: {line!r}")
        next_frame = int(command[0])
        if next_frame <= frame:
            raise ValueError(f"TAS frame {next_frame} is out of order")
        frame = next_frame
    return frame + 1


def one_match(pattern, log):
    found = re.findall(pattern, log)
    return found[0] if len(found) == 1 else None


def coverage_stats(path):
    coverage = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
    if not coverage.rstrip().endswith("=== END RECOMP EXECUTION COVERAGE ==="):
        return None
    blocks = one_match(r"static blocks executed\s*:\s*(\d+)", coverage)
    transitions = one_match(r"static -> JIT\s*:\s*(\d+)", coverage)
    killed = one_match(r"threads killed with no JIT fallback:\s*(\d+)", coverage)
    if blocks is None or transitions is None:
        return None
    return {
        "static_blocks": int(blocks),
        "jit_fallbacks": int(transitions),
        "threads_killed_without_fallback": int(killed or 0),
    }


def evaluate(log, mode, revision, title_id, frame, length, coverage, result):
    failures = []
    backend = one_match(r"TAS baseline backend=(\w+) revision=(\S+) program_id=([0-9a-f]+)", log)
    checkpoint = one_match(r"TAS baseline checkpoint frame=(\d+) elapsed_us=(\d+)", log)
    completion = one_match(r"TAS playback finished, (\d+) of (\d+) frames, exiting", log)
    if backend != (mode, revision, title_id):
        failures.append(f"Unexpected backend, build revision, or title ID: {backend}")
    elapsed = int(checkpoint[1]) if checkpoint and int(checkpoint[0]) == frame else 0
    if elapsed <= 0:
        failures.append("Missing the requested timed checkpoint")
    if completion != (str(length), str(length)):
        failures.append(f"Replay did not finish exactly: {completion}")
    if result != 0:
        failures.append(f"Game process failed: {result}")
    if mode != "jit" and (not coverage or coverage["static_blocks"] <= 0):
        failures.append("No evidence of executed recompiled blocks")
    if mode == "jit" and coverage and coverage["static_blocks"] > 0:
        failures.append("JIT reference executed recompiled blocks")
    if mode == "static" and coverage and (
        coverage["jit_fallbacks"] or coverage["threads_killed_without_fallback"]
        or "falling back to JIT" in log or "created JIT fallback" in log
    ):
        failures.append("Strict-static run attempted JIT fallback or killed a guest thread")
    return elapsed, failures


def save_report(path, report):
    pending = path.with_suffix(".tmp")
    pending.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    pending.replace(path)


def run_backend(args, paths, claims, mode, output):
    output.mkdir()
    capture = output / "checkpoint.png"
    coverage = output / "coverage.txt"
    env = {key: value for key, value in os.environ.items()
           if not key.upper().startswith(("SUYU_RECOMP_", "SUYU_CMD_"))}
    env.update(SUYU_CMD_TAS_BASELINE_FRAME=str(args.frame),
               SUYU_CMD_TAS_BASELINE_CAPTURE=str(capture),
               SUYU_CMD_PERF_SAMPLE="1",
               SUYU_RECOMP_COVERAGE_PATH=str(coverage))

    launcher = paths[mode]
    with tempfile.TemporaryDirectory(prefix="drippu-baseline-") as temporary:
        stage = Path(temporary) / "run"
        def ignore_user(folder, names):
            return {"user"} if Path(folder) == launcher.parent and "user" in names else set()

        shutil.copytree(launcher.parent, stage, symlinks=True, ignore=ignore_user)
        profile = stage / "user"
        shutil.copytree(paths["profile"], profile)
        tas = profile / "tas"
        tas.mkdir(exist_ok=True)
        for old_replay in tas.glob("script0-*.txt"):
            old_replay.unlink()
        shutil.copyfile(paths["replay"], tas / "script0-1.txt")

        config = profile / "config" / "baseline.ini"
        config.parent.mkdir(exist_ok=True)
        shutil.copyfile(paths["config"], config)
        cmd = [str(stage / launcher.name), "-t", "-g", str(paths["game"]), "-c", str(config),
               "-V", args.version]
        with (output / "console.log").open("wb") as console:
            try:
                result = subprocess.run(cmd, cwd=stage, env=env, stdin=subprocess.DEVNULL,
                                        stdout=console, stderr=subprocess.STDOUT,
                                        timeout=args.timeout, check=False).returncode
            except subprocess.TimeoutExpired:
                result = "timeout"
        app_log = profile / "log" / "suyu_log.txt"
        log_path = output / "console.log"
        if app_log.is_file():
            log_path = output / "suyu_log.txt"
            shutil.copyfile(app_log, log_path)
        log = log_path.read_text(encoding="utf-8", errors="replace")

    stats = coverage_stats(coverage)
    elapsed, failures = evaluate(log, mode, args.revision, args.title_id, args.frame,
                                 claims["replay_frames"], stats, result)
    if capture.is_file():
        with capture.open("rb") as image:
            valid_capture = image.read(8) == b"\x89PNG\r\n\x1a\n"
    else:
        valid_capture = False
    if not valid_capture:
        failures.append("Missing the checkpoint PNG")
    return {"backend": mode, "directory": output.name, "process_result": result,
            "elapsed_us": elapsed, "coverage": stats, "failures": failures,
            "capture_sha256": sha256(capture) if valid_capture else None}


def run(args):
    output = args.output.resolve()
    paths = {key: getattr(args, key).resolve()
             for key in (*MODES, "game", "profile", "config", "replay")}
    for key, path in paths.items():
        if key == "profile" and path.is_dir():
            continue
        if key == "profile" or not path.is_file():
            raise ValueError(f"Missing {key} input: {path}")
    for key in (*MODES, "profile"):
        root = paths[key] if key == "profile" else paths[key].parent
        if output.is_relative_to(root):
            raise ValueError(f"Output directory must be outside {key} inputs")
    if paths["game"].name != "main" or paths["game"].parent.name != "exefs":
        raise ValueError("Use the resolved content's exefs/main as --game")
    if not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        raise ValueError("Use the full Git commit ID as --revision")
    if not re.fullmatch(r"[0-9a-f]{16}", args.title_id):
        raise ValueError("Use the lower-case 16-digit hexadecimal program ID")
    version = re.fullmatch(r"([0-9]+):[^:]+", args.version)
    if not version or int(version[1]) > 0xffffffff:
        raise ValueError("Use a numeric and display application version, for example 1441792:4.0.0")
    length = replay_length(paths["replay"])
    if not 1 <= args.frame <= length - 2:
        raise ValueError(f"Choose a checkpoint from frame 1 through {length - 2}")

    for root in (paths["profile"] / "load", *(paths[mode].parent / "mods" for mode in MODES)):
        if root.exists() and any(path.is_file() for path in root.rglob("*")):
            raise ValueError(f"Use an unmodified game for the baseline: {root}")
    exefs_hash = exefs_sha256(paths["game"].parent)
    manifests = {}
    for mode in ("hybrid", "static"):
        manifest = paths[mode].with_name("aot_manifest.json")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        if data["image_abi"] not in (5, 6):
            raise ValueError(f"Re-export the {mode} build as image ABI 5 or 6")
        if data["source_exefs_sha256"] != exefs_hash or data.get("baked_patches"):
            raise ValueError(f"The {mode} build must match the unmodified game ExeFS")
        manifests[mode] = {"sha256": sha256(manifest), "image_abi": data["image_abi"],
                           "image_features": data.get("image_features"),
                           "source_exefs_sha256": data["source_exefs_sha256"]}

    claims = {
        "revision": args.revision,
        "title_id": args.title_id,
        "version": args.version,
        "milestone": args.milestone,
        "frame": args.frame,
        "replay_frames": length,
        "replay_sha256": sha256(paths["replay"]),
        "config_sha256": sha256(paths["config"]),
        "profile_sha256": tree_sha256(paths["profile"]),
        "content_sha256": tree_sha256(paths["game"].parent.parent),
        "exefs_sha256": exefs_hash,
        "executables": {mode: sha256(paths[mode]) for mode in MODES},
        "aot_manifests": manifests,
        "host": platform.platform(),
    }
    output.mkdir(parents=True)
    report = {"schema": 1, "status": "running", "inputs": claims, "runs": []}
    save_report(output / "baseline.json", report)
    for trial in range(args.repeat):
        order = MODES[trial % len(MODES):] + MODES[:trial % len(MODES)]
        for mode in order:
            sample = run_backend(args, paths, claims, mode, output / f"{mode}-{trial + 1}")
            report["runs"].append(sample)
            print(f"{sample['directory']}: " + ("; ".join(sample["failures"]) or "awaiting review"),
                  flush=True)
            save_report(output / "baseline.json", report)
    failed = any(item["failures"] for item in report["runs"])
    report["status"] = "failed" if failed else "awaiting_review"
    save_report(output / "baseline.json", report)
    return int(failed)


def review(args):
    path = args.output / "baseline.json"
    report = json.loads(path.read_text(encoding="utf-8"))
    if report["status"] != "awaiting_review" or any(run["failures"] for run in report["runs"]):
        raise ValueError("Only completed runs without automated failures can be reviewed")
    confirmed = set(args.reached)
    captures = {run["directory"] for run in report["runs"]}
    if not confirmed <= captures:
        raise ValueError(f"Unknown run(s): {', '.join(sorted(confirmed - captures))}")
    for item in report["runs"]:
        name = item["directory"]
        if sha256(args.output / name / "checkpoint.png") != item["capture_sha256"]:
            raise ValueError(f"Checkpoint image changed after the run: {name}")
        item["milestone_reached"] = name in confirmed

    report["status"] = "passed" if confirmed == captures else "milestone_mismatch"
    if report["status"] == "passed":
        medians = {mode: statistics.median(run["elapsed_us"] for run in report["runs"]
                                          if run["backend"] == mode) for mode in MODES}
        report["comparison"] = {
            mode: {"median_elapsed_ms": round(medians[mode] / 1000, 3),
                   "relative_to_jit": round(medians["jit"] / medians[mode], 4)} for mode in MODES
        }
        print(json.dumps(report["comparison"], indent=2))
    save_report(path, report)
    return int(report["status"] != "passed")


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    launch = commands.add_parser("run")
    for mode in MODES:
        launch.add_argument(f"--{mode}", type=Path, required=True, help="path to the CLI executable")
    launch.add_argument("--game", type=Path, required=True, help="resolved ExeFS main")
    launch.add_argument("--profile", type=Path, required=True, help="private seed user directory")
    launch.add_argument("--config", type=Path, required=True, help="SDL config shared across modes")
    launch.add_argument("--replay", type=Path, default=DEFAULT_REPLAY)
    launch.add_argument("--revision", required=True)
    launch.add_argument("--title-id", required=True)
    launch.add_argument("--version", required=True, help="numeric:display")
    launch.add_argument("--frame", type=positive, required=True)
    launch.add_argument("--milestone", required=True)
    launch.add_argument("--repeat", type=positive, default=1)
    launch.add_argument("--timeout", type=positive, default=900, help="seconds per launch")
    launch.add_argument("--output", type=Path, required=True, help="new result directory")
    verdict = commands.add_parser("review")
    verdict.add_argument("--output", type=Path, required=True)
    verdict.add_argument("--reached", action="append", default=[],
                         help="run directory whose screenshot shows the milestone (repeatable)")
    args = parser.parse_args()
    try:
        return run(args) if args.command == "run" else review(args)
    except (OSError, ValueError, KeyError) as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
