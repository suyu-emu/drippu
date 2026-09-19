#!/usr/bin/env python3
"""Validate a recomp_benchmark.json run against portable suite guardrails."""

import argparse
import json
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"benchmark baseline failure: {message}")


def validate_mode(mode: dict, name: str, backend: str, minimum_iterations: int) -> None:
    if mode.get("execution_backend") != backend:
        fail(f"{name}: expected backend {backend}")
    slices = mode.get("slices", {})
    if int(slices.get("count", 0)) < minimum_iterations:
        fail(f"{name}: too few slice samples")
    if int(mode.get("startup_ns", 0)) <= 0:
        fail(f"{name}: missing startup timing")
    events = mode.get("frame_events", {})
    if int(events.get("count", 0)) < minimum_iterations or int(events.get("wall_time_ns", 0)) <= 0:
        fail(f"{name}: missing frame-event timing")
    correctness = mode.get("correctness", {})
    if not correctness.get("halt_supervisor_call"):
        fail(f"{name}: workload did not halt on SVC")
    if correctness.get("x0") != correctness.get("expected_x0"):
        fail(f"{name}: x0 correctness mismatch")
    if correctness.get("svc") != correctness.get("expected_svc"):
        fail(f"{name}: SVC correctness mismatch")
    metrics = mode.get("execution_metrics_delta", {})
    if "backends" not in metrics or "transitions" not in metrics or "fallback_reasons" not in metrics:
        fail(f"{name}: incomplete execution metrics")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("result", type=Path)
    parser.add_argument("--baseline", type=Path, required=True)
    args = parser.parse_args()
    result = json.loads(args.result.read_text(encoding="utf-8"))
    baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    if result.get("kind") != "recomp_benchmark" or result.get("schema_version") != 1:
        fail("unexpected result kind/schema")
    thresholds = baseline["thresholds"]
    if args.result.stat().st_size > int(thresholds["max_invalid_json_size_bytes"]):
        fail("result JSON exceeds max_invalid_json_size_bytes")
    workloads = result.get("representative_workloads", {})
    required = set(baseline["workloads"])
    representative_required = required - {"alu_add_reload_svc"}
    if not representative_required <= set(workloads):
        fail(f"missing workloads: {sorted(representative_required - set(workloads))}")
    primary = result.get("workload", {})
    if primary.get("name") != "alu_add_reload_svc" or not primary.get("identical_across_backends"):
        fail("primary workload identity/identical_across_backends is invalid")
    primary_iters = int(primary.get("iters", 0))
    if primary_iters < thresholds["min_iterations"]:
        fail("primary workload has too few iterations")
    primary_modes = result.get("modes", {})
    if set(("hybrid_aot", "jit")) - set(primary_modes):
        fail("primary workload is missing a backend mode")
    validate_mode(primary_modes["hybrid_aot"], "primary AOT", "hybrid_aot", primary_iters)
    validate_mode(primary_modes["jit"], "primary JIT", "jit", primary_iters)
    for name in representative_required:
        item = workloads[name]
        iters = int(item.get("iters", 0))
        if iters < thresholds["min_iterations"]:
            fail(f"{name}: too few iterations")
        for backend in thresholds["required_backends"]:
            mode = item.get("aot" if backend == "hybrid_aot" else "jit")
            if not mode:
                fail(f"{name}: missing {backend} measurements")
            validate_mode(mode, f"{name} {backend}", backend, iters)
    if len(representative_required) + 1 < int(thresholds["required_workloads"]):
        fail("result contains fewer workloads than required_workloads")
    aot = primary_modes["hybrid_aot"]
    jit = primary_modes["jit"]
    if not aot or not jit:
        fail("missing primary ALU workload modes")
    if int(aot.get("generated_binary", {}).get("aot_so_bytes", 0)) <= 0:
        fail("missing generated AOT binary size")
    if int(jit.get("generated_binary", {}).get("jit_rss_delta_after_first_slice_bytes", 0)) == 0:
        # RSS can be page-granular and remain unchanged for tiny hosts; retain
        # the field requirement while allowing a zero delta.
        if "jit_rss_delta_after_first_slice_bytes" not in jit.get("generated_binary", {}):
            fail("missing JIT code-size proxy")
    aot_metrics = aot.get("execution_metrics_delta", {})
    aot_blocks = int(aot_metrics.get("backends", {}).get("aot", {}).get("block_executions", 0))
    aot_to_jit = int(aot_metrics.get("transitions", {}).get("aot_to_dynarmic", 0))
    if aot_blocks <= 0:
        fail("primary AOT path did not execute any AOT blocks")
    transition_ratio = aot_to_jit / aot_blocks
    if transition_ratio > float(thresholds["max_aot_to_jit_transition_ratio"]):
        fail(f"primary AOT transition ratio {transition_ratio:.3f} exceeds threshold")
    aggregate = result.get("aot_compile", {})
    if aggregate.get("scope") != "shared_aot_image" or aggregate.get("per_workload") != "unavailable":
        fail("AOT compile scope must be an explicit shared aggregate")
    for key in ("translate_ns", "cmake_configure_ns", "cmake_build_ns", "dlopen_ns", "so_bytes"):
        if int(aggregate.get(key, 0)) <= 0:
            fail(f"missing shared AOT aggregate {key}")
    print(f"validated {len(workloads)} representative workloads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
