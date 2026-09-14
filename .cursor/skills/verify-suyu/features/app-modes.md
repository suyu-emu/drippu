# App modes

suyu presents three full-window layouts: Gamer (library), Programmer (IDE-style), and Hacker (debug dock plus MCP tools). The first launch asks which profile to use unless a flag or a remembered choice is set.

## Sub-features

- `mode-gamer` applies the Gamer library chrome (sidebar + grid).
- `mode-programmer` applies the Programmer layout.
- `mode-hacker` applies the Hacker layout and shows the Hacker Tools dock.
- `mode-selector-skipped` confirms unattended `-gamer`/`-hacker`/`-programmer` does not show **suyu | Setup Profile**.

## How to get to it (user POV)

- On a first launch without a remembered profile, pick **Gamer**, **Programmer**, or **Hacker** in **suyu | Setup Profile**.
- Choose **View → Change Interface Mode** (action `action_Change_Interface_Mode`).
- Start the process with `-gamer`, `-hacker`, or `-programmer`.

## Driving it with control-suyu

Preconditions:

- `control-suyu launch` used `-gamer` (the helper always does).
- `control-suyu doctor` reports `ok`.

- **Baseline Gamer.** Run `control-suyu call get_ui_state`. `mode` is `gamer`. Screenshot `mode-gamer.png` shows the Library sidebar labels `Library`, `Settings`, `Multiplayer`, `Social`.
- **Switch to Programmer.** Run `control-suyu call set_app_mode '{"mode":"programmer"}'` then `get_ui_state`. `success` is true and `mode` is `programmer`. Screenshot `mode-programmer.png`.
- **Switch to Hacker.** Run `control-suyu call set_app_mode '{"mode":"hacker"}'` then `get_ui_state`. `mode` is `hacker`. Screenshot `mode-hacker.png` includes the **Hacker Tools** dock.
- **Return to Gamer.** Run `control-suyu call set_app_mode '{"mode":"gamer"}'` then `get_ui_state`. `mode` is `gamer` and `current_view` is `library` or becomes so after `navigate_gamer_view '{"view":"library"}'`.
- **Selector skipped.** The screenshot must not show window title `suyu | Setup Profile`. If it does, launch seeding failed; cleanup and relaunch.
- **Proof.** Keep the three mode PNGs and a `modes.meta.json` with the three `get_ui_state` results.

## Gotchas

- `set_app_mode` rejects anything other than `gamer`, `programmer`, `hacker` (case-insensitive). Unknown values return `success: false`.
- MCP is started from `ApplyAppMode`. Calling tools during a doomed launch (mode selector still up) will hang — that is why launch passes `-gamer`.
- Hacker debug panels only show when emulation is not running.
- Do not treat `RememberMode` in the real `~/.config` as in-scope. Isolated XDG is mandatory.
