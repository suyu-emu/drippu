#!/usr/bin/env python3
"""Validate a recomp_benchmark.json run against portable suite guardrails."""

import argparse
import json
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"benchmark baseline failure: {message}")


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
    for name in representative_required:
        item = workloads[name]
        if int(item.get("iters", 0)) < thresholds["min_iterations"]:
            fail(f"{name}: too few iterations")
        for backend in thresholds["required_backends"]:
            mode = item.get("aot" if backend == "hybrid_aot" else "jit")
            if not mode or mode.get("execution_backend") != backend:
                fail(f"{name}: missing {backend} measurements")
            if int(mode.get("slices", {}).get("count", 0)) < thresholds["min_iterations"]:
                fail(f"{name}: {backend} has no slice samples")
            if int(mode.get("startup_ns", 0)) <= 0:
                fail(f"{name}: {backend} has no startup timing")
            events = mode.get("frame_events", {})
            if int(events.get("count", 0)) < thresholds["min_iterations"]:
                fail(f"{name}: {backend} has no frame-event samples")
    if len(representative_required) + 1 < int(thresholds["required_workloads"]):
        fail("result contains fewer workloads than required_workloads")
    aot = result.get("modes", {}).get("hybrid_aot", {})
    jit = result.get("modes", {}).get("jit", {})
    if not aot or not jit:
        fail("missing primary ALU workload modes")
    if int(aot.get("generated_binary", {}).get("aot_so_bytes", 0)) <= 0:
        fail("missing generated AOT binary size")
    if int(jit.get("generated_binary", {}).get("jit_rss_delta_after_first_slice_bytes", 0)) == 0:
        # RSS can be page-granular and remain unchanged for tiny hosts; retain
        # the field requirement while allowing a zero delta.
        if "jit_rss_delta_after_first_slice_bytes" not in jit.get("generated_binary", {}):
            fail("missing JIT code-size proxy")
    if result.get("workload", {}).get("iters", 0) < thresholds["min_iterations"]:
        fail("primary workload has too few iterations")
    aot_metrics = aot.get("execution_metrics_delta", {})
    aot_blocks = int(aot_metrics.get("backends", {}).get("aot", {}).get("block_executions", 0))
    aot_to_jit = int(aot_metrics.get("transitions", {}).get("aot_to_dynarmic", 0))
    if aot_blocks <= 0:
        fail("primary AOT path did not execute any AOT blocks")
    transition_ratio = aot_to_jit / aot_blocks
    if transition_ratio > float(thresholds["max_aot_to_jit_transition_ratio"]):
        fail(f"primary AOT transition ratio {transition_ratio:.3f} exceeds threshold")
    print(f"validated {len(workloads)} representative workloads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
