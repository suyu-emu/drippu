# Keys and firmware

Switch titles need user-provided `prod.keys` (and usually dumped firmware). The Qt UI reports whether those are present and offers Tools menu installers. A clean verification isolate has neither, and that absence is the observable.

## Sub-features

- `keys-status` reports whether `prod.keys` / `title.keys` exist in the isolate keys dir.
- `firmware-status` reports NAND path existence without claiming a firmware version.
- `keys-dialog` opens **Tools → Install Decryption Keys**.
- `firmware-dialog` opens **Tools → Install Firmware**.

## How to get to it (user POV)

- Choose **Tools → Install Decryption Keys** and pick a `prod.keys` file.
- Choose **Tools → Install Firmware** and pick a directory of dumped firmware NCAs (directory picker — not a ZIP chooser).
- Choose **File → Setup** for the checklist (keys, firmware, games, optional recompile).
- First-run **Welcome to drippu** also points at profile / Nintendo / Steam setup; launch seeding skips that window.

## Driving it with control-suyu

Preconditions:

- Doctor is healthy on a fresh isolate (no keys copied in).
- Do not pass Nintendo key material or firmware dumps.

- **Keys absent.** Run `control-suyu call get_keys_status`. `prod_keys_present` is false, `title_keys_present` is false, and `keys_directory` is under the isolate `XDG_DATA_HOME` (`.../xdg-data/suyu/keys`).
- **Firmware absent.** Run `control-suyu call get_firmware_status`. `prod_keys_present` is false; `nand_path` is under the isolate (`.../xdg-data/suyu/nand`).
- **Keys dialog.** Run `control-suyu call trigger_ui_action '{"action":"install_keys_dialog"}'`. MCP may return empty while the native picker is up. Screenshot `keys-dialog.png` with `--target active_modal`; title contains `Select prod.keys`.
- **Firmware dialog.** After dismissing or on a fresh launch, run `control-suyu call trigger_ui_action '{"action":"install_firmware_dialog"}'`. Screenshot `firmware-dialog.png`; title contains `Select Dumped Firmware Source Location`.
- **Proof.** Save `keys.meta.json` with both status payloads. Paths must sit under the isolate XDG data home, not `~/.local/share/suyu`.

## Gotchas

- `install_keys_from_path` / `install_firmware_from_path` copy real dumps into the isolate. Do not call them in CI or on a shared machine unless the operator already has lawful files and asked for that recipe.
- Missing keys do **not** block the Gamer window. They only warn when launching a non-NRO title.
- `get_keys_status` and `get_firmware_status` are the user-visible status; do not stat files in `/home` and call that verification.
- File pickers are native Qt dialogs. If a picker is still open, later MCP tools that grab `active_modal` will screenshot the picker, not the main window — cleanup/relaunch between picker recipes.
- XDG directory names remain `suyu` even though the welcome dialog says drippu.
