# Multiplayer

Users browse public rooms, host a room, or connect directly from the Multiplayer menu and from the Gamer sidebar. The lobby is a separate window titled like **Public Room Browser**.

## Sub-features

- `mp-open` opens the lobby from Gamer Multiplayer and from Multiplayer → View Lobby.
- `mp-counts` reports visible vs unfiltered room rows after the list fetch.
- `mp-filters` records search / hide-empty / hide-full / games-owned checkbox state.

## How to get to it (user POV)

- Choose **Multiplayer** in the Gamer sidebar.
- Choose **Multiplayer → View Lobby**.
- Choose **Multiplayer → Start Room** or **Connect to Room** for host/direct flows (not covered by the smoke tool).

## Driving it with control-suyu

Preconditions:

- Doctor is healthy.
- Outbound network is allowed if you need a non-zero room list. A zero-row list after a completed fetch is still a valid empty lobby.
- Start from Gamer library.

- **Open from Gamer.** Run `control-suyu call navigate_gamer_view '{"view":"multiplayer"}'`. `success` is true.
- **Row counts.** Run `control-suyu call get_lobby_row_counts`. This waits up to ~15s for the room list. `success` is true, `visible_rows` and `unfiltered_rows` are integers ≥ 0, and `filters` includes `search`, `games_owned`, `hide_empty`, `hide_full`.
- **Screenshot.** Run `control-suyu screenshot --target active_window --path "$SUYU_VERIFY_EVIDENCE_DIR/lobby.png"`. The window is the lobby (`Lobby` objectName or title containing `Public Room Browser`).
- **Proof.** `multiplayer.meta.json` stores the counts JSON. Do not claim rooms exist if `unfiltered_rows` is 0.

## Gotchas

- `get_lobby_row_counts` spins the GUI thread with `QThread::msleep` while waiting. Do not overlap other MCP calls until it returns.
- An empty lobby is not a harness failure. A `Lobby window is not available` error is.
- Host/join with a password is out of scope for this map; there is no MCP tool for filling the host-room form.
- Returning to the library: `navigate_gamer_view '{"view":"library"}'` after closing the lobby. If the lobby stays open, cleanup the instance.
