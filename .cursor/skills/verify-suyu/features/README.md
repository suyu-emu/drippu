# suyu verification map

This directory is the maintained source for verifying the Qt desktop frontend of suyu. Read the index before driving the app, then use the matching feature file as the recipe.

## Baseline preconditions

- Build `suyu` with the cmake flags in the skill Launch section. The binary is `build-verify/bin/suyu` (or `build/bin/suyu`).
- `DISPLAY` points at a live X11 server. `control-suyu launch` starts `suyu -gamer` with isolated `XDG_*_HOME`.
- Run `control-suyu doctor` and require `ok`, `emulator=suyu`, `port_owned_by_us`, and `ui_state.mode` in `{gamer,programmer,hacker}`.
- Never drive an instance that was not started by this verification run. Port 9742 is exclusive.
- Do not supply Nintendo dumps or `prod.keys` unless the recipe explicitly needs them. Clean isolates report keys/firmware absent.

## Driving conventions

- Start every recipe from the baseline isolate unless its preconditions say otherwise.
- Drive through `control-suyu call` / `control-suyu screenshot`. Treat tool names and JSON keys as literal.
- After a mutation, read state back with a second MCP tool (`get_ui_state`, `list_configured_game_dirs`, `get_firmware_status`). A `success: true` alone is not proof.
- Restore the gamer library view (`navigate_gamer_view {"view":"library"}`) after opening Settings, Multiplayer, or Manual.
- Cleanup removes the isolate. Leave proof under `$SUYU_VERIFY_EVIDENCE_DIR`.

## Proof and skip reporting

- Capture the user action and the resulting state, not only the final screen.
- UI proof includes MCP JSON and a PNG that shows the suyu window identity (`suyu` title / Gamer chrome).
- Mutation proof includes a second read of the stored value.
- Record the feature ID and entry point used with every artifact.
- Report an unreachable path with the attempted command and the unmet precondition (missing keys, no network, no ROM).
- Do not report a skipped entry point as verified through a different path.

## Feature entry contract

Each feature file starts with an H1 title and one paragraph describing the user-visible behavior. It then uses exactly four H2 sections in this order.

1. `Sub-features` lists short IDs with one line for each behavior.
2. `How to get to it (user POV)` lists every user entry point.
3. `Driving it with control-suyu` starts with `Preconditions:` and uses labeled bullets that pair each user action with an exact command and observable result.
4. `Gotchas` lists traps that can waste or invalidate a verification run.

Keep implementation details out of the map. Name only user paths, stable handles, required state, commands, and observable proof.

## Features

- [Game library](./game-library.md) covers the Gamer library grid, empty state, directory add, search filter, and refresh.
- [App modes](./app-modes.md) covers Gamer, Programmer, and Hacker layouts and the first-launch profile selector.
- [Configuration](./configuration.md) covers the settings dialog, theme, and user manual.
- [Keys and firmware](./keys-and-firmware.md) covers decryption-key and firmware status plus install dialogs without requiring dumps.
- [Multiplayer](./multiplayer.md) covers the public room browser and related Gamer nav.
