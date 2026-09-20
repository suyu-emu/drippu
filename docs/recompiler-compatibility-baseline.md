# Recompiler compatibility baseline

The release and test workflow records a repeatable compatibility baseline for
the experimental ArmRecomp path. It uses the redistributable
`recomp_stack_harness` fixture and does not require firmware, keys, or a game
dump. The fixture is deliberately named `recompiler_synthetic_homebrew`: it
is a generated `Kernel::CodeSet` image, not a claim that an NSO or NRO loader
has loaded a real title.

## Covered workloads

The harness must provide evidence for all four workloads:

- `boot`: construct an application `KProcess`, load the fixture, and install
  ArmRecomp through `SetRecompLookup`.
- `sustained_execution`: run the identical JIT/hybrid-AOT workload with the
  configured iteration count.
- `save_load`: save and restore the kernel `Svc::ThreadContext`, then verify
  the restored PC/TLS/AOT result.
- `restart`: create a fresh process and instruction cache, then rerun the
  fixture through ArmRecomp.

The same run also exercises the real `PhysicalCore::RunThread` to
`Kernel::Svc::Call` path with `GetCurrentProcessorNumber`, and feeds the two
fixture threads through the kernel priority queue. The standalone harness does
not own a `CpuManager` guest fiber, so it stops at deterministic scheduler
priority selection rather than yielding into the production fiber loop.

## Running locally

Build the full-tree target, then run:

```text
python3 scripts/recompiler_compat_baseline.py \
  --harness build/bin/recomp_stack_harness \
  --output recomp-compat-baseline.json \
  --iterations 32 \
  --timeout-seconds 60
```

The runner captures repository provenance before starting the harness. It
launches the harness in a separate process group, sends a graceful termination
on timeout, then kills the full group if needed. It always writes JSON,
including when the harness times out; timeout is recorded in
`command.timed_out` and `failures`. A non-zero exit status means a workload
marker was missing, the harness returned a failure, or the timeout fired. Use
`--setting key=value` for any run-specific setting that must be recorded. The workflow uploads the JSON as the
`recompiler-compatibility-baseline` artifact.

## JSON record

The record is schema version 1 and includes the UTC timestamp, repository SHA
and dirty state, host architecture, compiler version, exact command, explicit
settings, fixture provenance, each workload's status/evidence, failure reasons,
and the tail of the harness log. The tail is diagnostic only; workload status
and failures are the machine-readable gate.

This baseline is an emulator/recompiler compatibility gate for a known,
self-contained workload. It must not be presented as a game compatibility
matrix.
