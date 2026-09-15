# Game library

The Gamer library is the default home: a card grid of local (and optionally owned) titles, a search field, and buttons to add a scan directory or load a file. On a clean isolate it shows an empty collection, not a crash or a keys modal.

## Sub-features

- `library-open` shows the Gamer library view from launch and from the Library nav button.
- `library-empty` reports zero playable titles and the empty-state copy.
- `library-add-dir` adds a folder to the scan list without installing a ROM.
- `library-search` filters visible cards by title text and can be cleared.
- `library-refresh` rebuilds the grid from the current scan list.

## How to get to it (user POV)

- Launch the Qt app and choose **Gamer** (or pass `-gamer`).
- Choose the **Library** button in the left Gamer sidebar.
- Choose **Add a Game** to pick a directory, or **Load a Game** to pick a single file.
- Type into **Search your games...**
- Double-click the empty library pane (legacy list) to add a folder; that path is hidden under Gamer chrome.

## Driving it with control-suyu

Preconditions:

- `control-suyu doctor` reports `ok` and `ui_state.mode` is `gamer`.
- No Nintendo dumps are required.
- A disposable empty directory exists, e.g. `/tmp/suyu-verify-$RUN_ID/games`.

- **Open library.** Arrive on Gamer home, or choose Library. Run `control-suyu call navigate_gamer_view '{"view":"library"}'` then `control-suyu call get_ui_state`. `current_view` is `library` and `mode` is `gamer`.
- **Empty state.** With no playable titles, `game_count` is `0`. Run `control-suyu screenshot --path "$SUYU_VERIFY_EVIDENCE_DIR/library-empty.png"`. The PNG shows heading `Library`, subtitle `Your collection, curated.`, stats `0 games in your library`, and empty copy starting with `No games found.` (full line also tells the user to click Add a Game). Sidebar brand is primarily **suyu** with a smaller **drippu** mark; window title contains both names.
- **Add directory.** Create an empty folder and add it. Run `control-suyu call add_game_directory '{"path":"/tmp/suyu-verify-'"$SUYU_VERIFY_RUN_ID"'/games"}'`. `success` is true and `already_present` is false on first add.
- **Confirm scan list.** Run `control-suyu call list_configured_game_dirs`. `directories` contains that absolute path with `deep_scan` true (NAND/SDMC rows may also appear).
- **Search filter.** Run `control-suyu call set_gamer_search_filter '{"filter":"zelda"}'` then `control-suyu call get_ui_state`. `search_filter` is `zelda`. Capture `library-search.png`.
- **Clear filter.** Run `control-suyu call set_gamer_search_filter '{"filter":""}'` then `get_ui_state`. `search_filter` is empty.
- **Refresh.** Run `control-suyu call refresh_game_library`. `success` is true; `get_ui_state` still reports `current_view` `library`.
- **Proof.** Write `$SUYU_VERIFY_EVIDENCE_DIR/library.meta.json` with the last `get_ui_state` and `list_configured_game_dirs` bodies plus feature id `library-add-dir`.

## Gotchas

- `list_game_directories` reads a stale generic data path. Use `list_configured_game_dirs` for the live scan list.
- `add_game_directory` does not create games. An empty folder stays an empty library; that is success.
- `Load a Game` / `launch_game_path` needs a ROM (and usually keys). Do not call them on the empty-library recipe.
- First-run **Welcome to drippu** blocks the event loop if `first_run_done` was not seeded. Doctor then times out on MCP.
- `set_gamer_search_filter` forces Gamer mode. Call it after mode-switch tests, not in the middle of a Programmer assertion.
- That tool invokes `OnSearchChanged` directly, so `get_ui_state.search_filter` updates while the search box can still show the placeholder `Search your games...`. Assert the MCP field, not the placeholder text.
