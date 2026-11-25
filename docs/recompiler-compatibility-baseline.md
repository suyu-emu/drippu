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

## Real-game comparison

Use the [game baseline runner](../scripts/recompiler_game_baseline.py) to
compare Dynarmic JIT, Hybrid AOT + JIT, and strict static AOT at the same
checkpoint. It runs all three backends against the same resolved ExeFS and
RomFS, SDL configuration, starting profile, and frame-indexed replay. The
first reference workload is the opening of Mario Kart Stadium in Mario Kart
8 Deluxe 4.0.0 using the [checked-in replay](../scripts/mk8d-tas/README.md).
Its 6,001 commands cover the opening part of the race.

### Prepare the inputs

1. Use your own game and update to prepare a clean, resolved `exefs/main`
   with its matching `main.npdm`, modules, and `romfs.bin`. The CLI can
   resolve a base and update pair with
   `--content-base BASE --content-update UPDATE --content-probe --content-dump OUTPUT`.
   Use a dedicated content directory so the runner can fingerprint the game.
2. Build all three CLI executables from the same Drippu revision. Use
   `suyu-cmd` for JIT. For the other two, generate matching ABI 5 or 6
   images and build `suyu-cmd-static` against those images. Use
   `SUYU_RECOMP_HYBRID=ON` and `SUYU_NO_JIT=OFF` for Hybrid, and
   `SUYU_RECOMP_HYBRID=OFF` and `SUYU_NO_JIT=ON` for strict static.
   Keep each executable with its runtime libraries. Put the corresponding
   `aot_manifest.json` beside each AOT executable.
3. Prepare a private seed user directory with the required keys and
   firmware and a save that already has Player 1's Mii selected. Use the
   same seed and SDL configuration for every trial. The runner copies the
   profile for each launch. Leave mods and cheats disabled. On the target
   machine, choose a checkpoint where the JIT screenshot shows Mario
   driving on Mario Kart Stadium with the race HUD visible.

### Run and review

Use Python 3.11 or later. For example, try frame 5900 with the bundled replay
and confirm the image against the intended milestone.

```sh
python3 scripts/recompiler_game_baseline.py run \
  --jit /work/jit/suyu-cmd \
  --hybrid /work/hybrid/suyu-cmd-static \
  --static /work/static/suyu-cmd-static \
  --game /work/content/exefs/main --profile /work/seed-user \
  --config /work/sdl2-config.ini --revision "$(git rev-parse HEAD)" \
  --title-id 0100152000022000 --version 1441792:4.0.0 \
  --frame 5900 --milestone 'Driving on Mario Kart Stadium with the race HUD' \
  --output /work/mk8d-baseline
```

Inspect each run's `checkpoint.png`. Pass `--reached` once for each
capture that shows the stated gameplay milestone. For a successful single
trial, the command is:

```sh
python3 scripts/recompiler_game_baseline.py review --output /work/mk8d-baseline \
  --reached jit-1 --reached hybrid-1 --reached static-1
```

The runner separately checks the loaded CPU backend, exact replay
completion, and AOT coverage. Strict static requires a no-JIT build,
positive static block counts, and zero recorded fallback attempts.
The comparison uses elapsed time from replay start to the chosen frame.
The screenshot is requested at that frame and saved by the next render.
The report compares median elapsed time only after visual review accepts
every run. For performance measurements, use `--repeat 3` or more on the
same idle machine. Keep the report, screenshots, and logs local with the
game test results.
