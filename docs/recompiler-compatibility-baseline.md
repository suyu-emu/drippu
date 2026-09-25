# Recompiler compatibility testing

The old `recomp_stack_harness` baseline was written for drippu's ABI 1 image
loader, cache rejection policy, and execution metrics API. Doug's `mk8-recomp`
merge replaced those contracts with guarded ABI 5/6 images. The old harness
source and baseline scripts remain in the tree for a future port, but the
target is no longer built or used as a release gate. Its old benchmark JSON
must not be treated as a current performance result.

## Current checks

Run the active generated-image suite without game files, firmware, or keys:

```text
python3 tests/recompiler_smoke/run.py
```

It builds and executes ABI 5, ABI 6 fastmem, GG1 generation guard, FPX1, and
combined GG1/FPX1 fixtures. It checks code mutation and mapping changes,
cross-page memory, static multi-module linkage, and the coverage-gap loop.

The standalone exporter and instruction probes are available with:

```text
cmake -S src/tests/recompiler -B build-recompiler-tests
cmake --build build-recompiler-tests
ctest --test-dir build-recompiler-tests --output-on-failure
```

The pull request workflow also builds the emulator and runs its Catch2 tests.
The active synthetic suites do not exercise a complete game boot or provide a
game compatibility matrix. Test Hybrid and static exports on target hardware
before using their results to judge game performance or compatibility.
