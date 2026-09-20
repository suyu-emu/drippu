#!/usr/bin/env python3
"""Run the self-contained recompiler compatibility baseline.

The baseline intentionally uses the synthetic CodeSet fixture in
``recomp_stack_harness``.  It is not a title compatibility claim: it records
the emulator/backend behavior that can be reproduced without firmware, keys,
or copyrighted game data.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import signal
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


SCHEMA_VERSION = 1
WORKLOADS = {
    "boot": "bootstrap SetRecompLookup + application KProcess ArmRecomp",
    "sustained_execution": "identical JIT vs hybrid AOT workload (slice/startup/compile/memory/size)",
    "save_load": "ThreadContext save/load restores AOT execution and TLS",
    "restart": "new process ArmRecomp (fresh icache) re-runs Translate AOT",
}


def git(cwd: Path, *args: str) -> str:
    try:
        return subprocess.run(["git", *args], cwd=cwd, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                              check=False).stdout.strip()
    except OSError:
        return ""


def tool_version(name: str) -> str:
    tool = shutil.which(name)
    if not tool:
        return ""
    try:
        return subprocess.run([tool, "--version"], text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, check=False).stdout.splitlines()[0]
    except (OSError, IndexError):
        return ""


def parse_settings(values: list[str]) -> dict[str, str]:
    settings: dict[str, str] = {
        "cpu_backend": "ArmRecomp with Dynarmic fallback",
        "fixture_format": "synthetic CodeSet (not NSO/NRO)",
        "firmware": "not required",
        "keys": "not required",
    }
    for value in values:
        if "=" not in value:
            raise ValueError(f"--setting requires KEY=VALUE: {value}")
        key, setting = value.split("=", 1)
        if not key:
            raise ValueError(f"--setting has an empty key: {value}")
        settings[key] = setting
    return settings


def terminate_process_group(proc: subprocess.Popen[str]) -> None:
    """Terminate the harness and every child it started, on both host OSes."""
    if proc.poll() is not None:
        return
    if os.name == "nt":
        # CREATE_NEW_PROCESS_GROUP lets CTRL_BREAK_EVENT reach the harness and
        # its console-group children. Use taskkill /T /F only if the group did
        # not exit during the graceful window; unlike proc.terminate(), this
        # cannot leave a compiler/helper child behind after the parent exits.
        try:
            proc.send_signal(signal.CTRL_BREAK_EVENT)
        except (AttributeError, OSError):
            # Some Windows hosts do not expose a console for CTRL_BREAK. The
            # tree kill below remains the hard cleanup path.
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           check=False)
            proc.wait()
        return
    try:
        process_group = os.getpgid(proc.pid)
    except ProcessLookupError:
        return
    # TERM first gives the harness a bounded, graceful shutdown window.
    os.killpg(process_group, signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    else:
        # The parent exited, but descendants may still be attached to the
        # process group. We still kill the complete group below.
        pass
    try:
        os.killpg(process_group, signal.SIGKILL)
    except ProcessLookupError:
        pass
    if proc.poll() is None:
        proc.kill()
    proc.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", required=True, type=Path,
                        help="path to the built recomp_stack_harness executable")
    parser.add_argument("--output", required=True, type=Path,
                        help="JSON path to write, including on harness failure")
    parser.add_argument("--iterations", type=int, default=32,
                        help="sustained workload iteration setting (default: 32)")
    parser.add_argument("--timeout-seconds", type=float, default=60.0,
                        help="maximum harness runtime (default: 60 seconds)")
    parser.add_argument("--setting", action="append", default=[],
                        help="record an additional exact setting as KEY=VALUE")
    parser.add_argument("--allow-failures", action="store_true",
                        help="write the baseline but return success when a phase fails")
    args = parser.parse_args()

    cwd = Path.cwd()
    harness = args.harness if args.harness.is_absolute() else cwd / args.harness
    settings = parse_settings(args.setting)
    settings["benchmark_iterations"] = str(max(4, args.iterations))
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    settings["timeout_seconds"] = str(args.timeout_seconds)
    env = os.environ.copy()
    env["SUYU_RECOMP_BENCH_ITERS"] = settings["benchmark_iterations"]

    # Snapshot provenance before running user code. In particular, a harness
    # that writes generated files must not change the SHA/dirty state recorded
    # for the workload it is about to measure.
    repository = {
        "git_sha": git(cwd, "rev-parse", "HEAD"),
        "git_describe": git(cwd, "describe", "--always", "--dirty"),
        "working_tree_dirty": bool(git(cwd, "status", "--porcelain")),
    }

    command = [str(harness)]
    popen_kwargs: dict[str, object] = {
        "cwd": cwd,
        "env": env,
        "text": True,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.STDOUT,
    }
    if os.name == "nt":
        popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        popen_kwargs["start_new_session"] = True
    timed_out = False
    try:
        proc = subprocess.Popen(command, **popen_kwargs)
        try:
            output, _ = proc.communicate(timeout=args.timeout_seconds)
            returncode = proc.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            terminate_process_group(proc)
            output, _ = proc.communicate()
            returncode = proc.returncode
    except OSError as exc:
        returncode, output = 127, f"unable to launch {harness}: {exc}\n"

    # Keep the normal CI log useful while preserving a complete copy in JSON.
    print(output, end="")
    failures: list[dict[str, str]] = []
    results = []
    for name, marker in WORKLOADS.items():
        passed = marker in output
        result = {
            "name": name,
            "status": "pass" if passed else "fail",
            "evidence": marker,
        }
        if not passed:
            reason = "required harness evidence was not found"
            failures.append({"workload": name, "reason": reason})
            result["failure"] = reason
        results.append(result)
    if returncode != 0:
        failures.append({"workload": "harness", "reason": f"exit code {returncode}"})
    if timed_out:
        failures.append({"workload": "harness",
                         "reason": f"timeout after {args.timeout_seconds:g} seconds"})

    output_path = args.output if args.output.is_absolute() else cwd / args.output
    output_path.parent.mkdir(parents=True, exist_ok=True)
    record = {
        "schema_version": SCHEMA_VERSION,
        "kind": "recompiler_compatibility_baseline",
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "repository": repository,
        "host": {
            "os": platform.platform(),
            "architecture": platform.machine(),
            "python": platform.python_version(),
            "compiler": tool_version(os.environ.get("CXX", "c++")),
        },
        "command": {
            "argv": command,
            "cwd": str(cwd),
            "return_code": returncode,
            "timed_out": timed_out,
        },
        "settings": settings,
        "fixture": {
            "name": "recompiler_synthetic_homebrew",
            "description": "Generated AArch64 CodeSet image exercising ArmRecomp, Dynarmic, kernel SVCs, TLS, memory, and restart.",
            "redistributable": True,
            "external_firmware_or_keys": False,
        },
        "workloads": results,
        "failures": failures,
        "harness_output_tail": output[-12000:],
    }
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=output_path.parent,
                                    prefix=output_path.name + ".", suffix=".tmp",
                                    delete=False) as tmp:
        json.dump(record, tmp, indent=2, sort_keys=True)
        tmp.write("\n")
        temporary = Path(tmp.name)
    temporary.replace(output_path)
    print(f"wrote compatibility baseline: {output_path}")
    if failures:
        print(f"compatibility baseline failures: {len(failures)}", file=sys.stderr)
    return 0 if (not failures or args.allow_failures) else 1


if __name__ == "__main__":
    raise SystemExit(main())
