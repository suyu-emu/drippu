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


def run_capture(argv: list[str], cwd: Path) -> tuple[int, str]:
    try:
        proc = subprocess.run(argv, cwd=cwd, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, check=False)
    except OSError as exc:
        return 127, f"unable to launch {argv[0]!r}: {exc}\n"
    return proc.returncode, proc.stdout


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", required=True, type=Path,
                        help="path to the built recomp_stack_harness executable")
    parser.add_argument("--output", required=True, type=Path,
                        help="JSON path to write, including on harness failure")
    parser.add_argument("--iterations", type=int, default=32,
                        help="sustained workload iteration setting (default: 32)")
    parser.add_argument("--setting", action="append", default=[],
                        help="record an additional exact setting as KEY=VALUE")
    parser.add_argument("--allow-failures", action="store_true",
                        help="write the baseline but return success when a phase fails")
    args = parser.parse_args()

    cwd = Path.cwd()
    harness = args.harness if args.harness.is_absolute() else cwd / args.harness
    settings = parse_settings(args.setting)
    settings["benchmark_iterations"] = str(max(4, args.iterations))
    env = os.environ.copy()
    env["SUYU_RECOMP_BENCH_ITERS"] = settings["benchmark_iterations"]

    command = [str(harness)]
    try:
        proc = subprocess.run(command, cwd=cwd, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              check=False)
        returncode, output = proc.returncode, proc.stdout
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

    output_path = args.output if args.output.is_absolute() else cwd / args.output
    output_path.parent.mkdir(parents=True, exist_ok=True)
    record = {
        "schema_version": SCHEMA_VERSION,
        "kind": "recompiler_compatibility_baseline",
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "repository": {
            "git_sha": git(cwd, "rev-parse", "HEAD"),
            "git_describe": git(cwd, "describe", "--always", "--dirty"),
            "working_tree_dirty": bool(git(cwd, "status", "--porcelain")),
        },
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
