---
name: verify-suyu
description: "Drive the suyu Qt desktop emulator the way a user does — launch an isolated instance, talk to its localhost MCP JSON-RPC, and prove UI behavior. Use when verifying suyu frontend changes, game-library/settings/mode flows, or any Qt UI work in this repo."
---

# Verify suyu

suyu is a Qt 6 desktop Nintendo Switch emulator. The user-facing surface is the `suyu` window (Gamer / Programmer / Hacker layouts). `suyu-cmd` is a separate SDL CLI for booting a game; Android is a separate Gradle app. This skill covers the **Qt desktop frontend only**.

Do not boot copyrighted titles, dump keys, or install firmware you do not already have. Library, mode, settings, and status tools work without dumps.

## Launch

Build (Ubuntu/Debian, from the repo root). System libfmt 9 on Ubuntu 24.04 cannot compile this tree (`format.get()` needs fmt 10+); SDL3 is also not packaged. Bundled FFmpeg 8 needs `vaMapBuffer2` from a newer libva than Ubuntu 24.04 ships, so leave `YUZU_USE_BUNDLED_FFMPEG=OFF` and use distro libav*. Use GCC — `/usr/bin/c++` is often Clang and fails to link `libstdc++`. CMake must be 3.31+ (CPMUtil).

```sh
cmake -B build-verify -GNinja \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT=ON \
  -DYUZU_USE_BUNDLED_QT=OFF \
  -DYUZU_USE_BUNDLED_SDL3=ON \
  -DYUZU_USE_BUNDLED_FFMPEG=OFF \
  -Dfmt_FORCE_BUNDLED=ON \
  -DYUZU_TESTS=OFF \
  -DENABLE_QT_TRANSLATION=OFF \
  -DUSE_DISCORD_PRESENCE=OFF \
  -DENABLE_UPDATE_CHECKER=OFF \
  -DENABLE_WEB_SERVICE=OFF
cmake --build build-verify --target suyu
```

Ready binary: `build-verify/bin/suyu` (or `build/bin/suyu` if that is what you configured).

Needs a working X11 display (`DISPLAY=:1` in this cloud VM). Wayland is unused.

Start one isolated instance:

```sh
export SUYU_VERIFY_RUN_ID="${SUYU_VERIFY_RUN_ID:-$RANDOM}"
export SUYU_VERIFY_BIN="${SUYU_VERIFY_BIN:-$PWD/build-verify/bin/suyu}"
# Optional: send proof PNGs/JSON somewhere durable (cleanup will not delete it)
export SUYU_VERIFY_EVIDENCE_DIR="${SUYU_VERIFY_EVIDENCE_DIR:-/tmp/suyu-verify-$SUYU_VERIFY_RUN_ID/evidence}"
.cursor/skills/verify-suyu/scripts/control-suyu launch
```

`launch` is ready when TCP `127.0.0.1:9742` answers MCP `ping`. It prints `instance.json` (pid, XDG dirs, log, evidence dir). The log line `MCP Server started on port 9742` also appears in `$XDG_DATA_HOME/suyu/log/suyu_log.txt`.

What launch actually does:

- Refuses if port **9742** is already listening for another PID.
- Sets `XDG_DATA_HOME` / `XDG_CONFIG_HOME` / `XDG_CACHE_HOME` under `/tmp/suyu-verify-$RUN_ID/`.
- Runs from an empty workdir so a portable `user/` directory cannot steal paths (cwd `user/` beats XDG).
- Seeds `$XDG_CONFIG_HOME/suyu/suyu.conf` with `first_run_done=true` so the modal **Welcome to suyu** dialog does not block MCP.
- Seeds `$XDG_CONFIG_HOME/suyu team/suyu.conf` with `RememberMode=true` and starts `suyu -gamer` so the **suyu | Setup Profile** mode selector never appears.
- Starts a new process group; teardown kills that PID only.

Teardown: `.cursor/skills/verify-suyu/scripts/control-suyu cleanup`

## Doctor

Run this first whenever anything looks off:

```sh
.cursor/skills/verify-suyu/scripts/control-suyu doctor
```

A healthy report has `"ok": true` and:

- `pid_alive` true for the PID in `instance.json`
- `port_owned_by_us` true (ss listeners on 9742 include that PID)
- `system.emulator` is `"suyu"`
- `system.app_dir` is the directory of the launched binary
- `ui_state.mode` is `gamer`, `programmer`, or `hacker`

If doctor fails, stop. Do not click through some other suyu window on the same display.

## Drive

Harness: **control-suyu** over suyu's JSON-RPC MCP on `127.0.0.1:9742`. There is no Playwright surface. Qt widgets have almost no accessible names; do not drive by coordinates unless MCP cannot reach the control.

```sh
.cursor/skills/verify-suyu/scripts/control-suyu call get_ui_state
.cursor/skills/verify-suyu/scripts/control-suyu call set_app_mode '{"mode":"gamer"}'
.cursor/skills/verify-suyu/scripts/control-suyu call navigate_gamer_view '{"view":"library"}'
.cursor/skills/verify-suyu/scripts/control-suyu screenshot --path "$SUYU_VERIFY_EVIDENCE_DIR/library.png"
```

Stable handles (tool names and string arguments, not CSS):

| Tool | Arguments | Observes |
| --- | --- | --- |
| `get_ui_state` / `get_emulator_state` | `{}` | `mode`, `current_view` (`library`/`social`), `search_filter`, `game_count`, `emu_running` |
| `get_system_info` | `{}` | `emulator=suyu`, `qt_version`, `app_dir` |
| `set_app_mode` | `mode`: `gamer` \| `programmer` \| `hacker` | `success`, `mode` |
| `navigate_gamer_view` | `view`: `library`, `social`, `settings`, `multiplayer`, `manual`, `website`, `more_options` | `success`; `settings` opens **suyu Configuration** |
| `set_gamer_search_filter` | `filter` string | `search_filter` on next `get_ui_state` |
| `add_game_directory` | `path` absolute dir | `list_configured_game_dirs` |
| `list_configured_game_dirs` | `{}` | `directories[].path` |
| `refresh_game_library` | `{}` | empty overlay / card grid |
| `set_theme_mode` | `mode`: `light` \| `dark` \| `auto` | `is_dark_mode` |
| `trigger_ui_action` | `action`: `configure`, `open_user_manual`, `install_keys_dialog`, `install_firmware_dialog`, `export_game`, `nintendo_account`, `steam_integration`, `toggle_fullscreen` | named dialog/window |
| `capture_ui_screenshot` | `path`, `target`: `main_window` \| `active_modal` \| `active_window` \| `central_widget` | PNG on disk |
| `get_firmware_status` / `get_keys_status` | `{}` | `prod_keys_present`, `nand_path` (expect false on a clean isolate) |
| `get_lobby_row_counts` | `{}` | public room browser; needs network; blocks ~15s |
| `launch_game_path` / `stop_emulation` | ROM path | needs keys + a dump; skip unless the run has both |

User-visible strings to assert in screenshots: window title `suyu`; Gamer nav `Library`, `Settings`, `Multiplayer`, `Social`; library hero `Library` / `Your collection, curated.`; empty copy `No games found.`; search placeholder `Search your games...`; buttons `Add a Game`, `Load a Game`; config dialog title `suyu Configuration`; welcome dialog `Welcome to suyu` (must not appear after launch seeding).

Gamer sidebar objectName `gamerSidebar`; grid objectName `gameGrid`. Prefer MCP over picking those by pixel.

Read `features/` before driving. Use the matching feature file as the recipe.

## Evidence

Proof standards:

- Exercise the real Qt path (MCP tools call the same slots as the buttons). Do not write `qt-config.ini` by hand and call that "the user changed settings."
- Capture the action **and** the resulting state: MCP JSON plus a PNG of the window (and the modal if you opened one).
- Side effects: after `add_game_directory`, `list_configured_game_dirs` must contain the path. After `set_gamer_search_filter`, `get_ui_state.search_filter` must match. After `set_theme_mode`, `is_dark_mode` must match.
- Do not install keys/firmware or launch a title unless those files already exist on the machine. Status tools on a clean isolate (`prod_keys_present: false`) are the proof for that feature.
- `social_debug_navigate` and `aot_test_export` are test-only; do not treat them as user paths.

Where files go:

- Default: `$SUYU_VERIFY_EVIDENCE_DIR` (launch creates `/tmp/suyu-verify-$RUN_ID/evidence` if unset).
- Set `SUYU_VERIFY_EVIDENCE_DIR` to a durable store (for example a Project `media/` directory) **before** launch when the proof must survive the VM.
- Record the feature id and entry point next to the PNG (`*.meta.json` with `feature`, `entry`, `ui_state`).

Cleanup deletes XDG scratch and the instance file. It never deletes `evidence_dir`.

## Cleanup

```sh
.cursor/skills/verify-suyu/scripts/control-suyu cleanup
```

Kills the PID (process group) from `instance.json` with SIGTERM then SIGKILL. Does not `pkill suyu`. After a failed iteration, run cleanup before launching again — port 9742 is exclusive.

## Isolate

Port **9742 is hardcoded** in `GMainWindow::ApplyAppMode`. Two suyu Qt instances cannot be driven on one host. Refuse to attach to a PID you did not start.

XDG isolation is enough for settings/NAND/keys. Never create a `user/` directory in the launch cwd unless you intend portable mode.

## Helpers

`scripts/control-suyu` is executable. Commands:

```sh
.cursor/skills/verify-suyu/scripts/control-suyu launch --bin build-verify/bin/suyu
.cursor/skills/verify-suyu/scripts/control-suyu doctor
.cursor/skills/verify-suyu/scripts/control-suyu call get_ui_state
.cursor/skills/verify-suyu/scripts/control-suyu call add_game_directory '{"path":"/tmp/suyu-verify-games"}'
.cursor/skills/verify-suyu/scripts/control-suyu screenshot --path "$SUYU_VERIFY_EVIDENCE_DIR/library.png"
.cursor/skills/verify-suyu/scripts/control-suyu cleanup
```

Keep `--run-id` / `SUYU_VERIFY_RUN_ID` stable across launch, doctor, drive, and cleanup.
