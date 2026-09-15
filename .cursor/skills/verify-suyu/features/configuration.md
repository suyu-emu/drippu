# Configuration

Users change emulator and UI settings from a modal **suyu Configuration** dialog (General, System, CPU, Graphics, Audio, Controls) and can switch light/dark theme immediately via MCP. The in-app user manual is a separate window titled **User Manual**.

## Sub-features

- `config-open` opens the configuration dialog from Gamer Settings and from Emulation → Configure.
- `config-tabs` shows the six selector rows: General, System, CPU, Graphics, Audio, Controls.
- `config-theme` switches light, dark, or auto and applies it without OK (MCP path).
- `config-manual` opens the user manual from Help or Gamer Manual.

## How to get to it (user POV)

- Choose **Settings** in the Gamer sidebar.
- Choose **Emulation → Configure**.
- Choose **Help → User Manual**, or **Manual** in the Gamer sidebar.
- Theme is also reachable from UI settings inside the dialog; MCP `set_theme_mode` is the unattended equivalent of that control (applies immediately). Dialog theme edits still need Apply/OK.
- **More Options** in the Gamer sidebar is *not* Configure — it offers refresh / keys / firmware / verify shortcuts.

## Driving it with control-suyu

Preconditions:

- Doctor is healthy.
- Start from Gamer library (`navigate_gamer_view '{"view":"library"}'`).

- **Open from Gamer.** Run `control-suyu call navigate_gamer_view '{"view":"settings"}'`. Expect `success` when the reply arrives; a blocked/empty MCP body while the modal is up is normal. Run `control-suyu screenshot --target active_modal --path "$SUYU_VERIFY_EVIDENCE_DIR/config-dialog.png"`. The modal title is `suyu Configuration` (often `suyu Configuration — drippu`).
- **Open from action.** If the dialog was closed, run `control-suyu call trigger_ui_action '{"action":"configure"}'`. Same modal title.
- **Theme dark.** Run `control-suyu call set_theme_mode '{"mode":"dark"}'`. `success` is true and `is_dark_mode` is true. Screenshot `theme-dark.png` of `main_window`.
- **Theme light.** Run `control-suyu call set_theme_mode '{"mode":"light"}'`. `is_dark_mode` is false. Screenshot `theme-light.png`.
- **Manual.** Run `control-suyu call trigger_ui_action '{"action":"open_user_manual"}'` or `navigate_gamer_view '{"view":"manual"}'`. Screenshot `manual.png` via `active_window` or `main_window` showing **User Manual**.
- **Proof.** `config.meta.json` stores both theme results. PNGs show the configuration title or the themed Gamer chrome, not an unrelated desktop.

## Gotchas

- `navigate_gamer_view settings` and `trigger_ui_action configure` both call `OnConfigure()`, which is modal. Capture `active_modal` before trying another tool that also spins a dialog.
- Closing the dialog is not exposed as an MCP tool. If a later tool hangs, cleanup the instance and relaunch.
- `set_theme_mode auto` follows the desktop palette; assert `success` and record `is_dark_mode` rather than assuming dark.
- Web / telemetry tabs exist only when web services were compiled in. This skill's default cmake turns `ENABLE_WEB_SERVICE` off, so do not require a Web page.
- Do not assert a `drippu Configuration` title — the dialog string is still `suyu Configuration` after the user-facing rebrand.
