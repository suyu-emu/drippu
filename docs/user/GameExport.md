# Exporting a Game

Use **File > Export Game...** to turn a game into its own package. On Windows the
package is a program you can double-click or launch from Steam, without opening suyu.
Exporting reads the game on this computer and writes the package to the output folder
you choose.

> **Re-export builds made with v0.0.10 or earlier.** This release uses a new format
> for pre-compiled code (ABI 5) and refuses older (ABI 4) builds. Re-exporting also
> fixes the version number, shader cache, firmware and controller handling.

## Quick steps (Windows)

1. Open **File > Export Game...** and choose the game with **From Library**.
2. Check the **Update** row. Use **Install Update File...** if the update you want is
   not installed.
3. Leave **CPU Backend** on **suyu Dynarmic JIT (Baseline)** and **Export Format** on
   **Build**.
4. Tick **Include transferable shader cache** if you have played the game in suyu.
5. Optional: tick **Add to Steam library when the export finishes**.
6. Click export and wait for the progress bar to finish.
7. Open the package folder and run the game's `.exe` (or `launch.bat`).

## Choose the target and backend

The dialog can make Windows, Linux and macOS packages. Only a Windows export can
produce a ready-to-run program. Linux and macOS exports are **Source** only: they
contain generated code to compile yourself and no bundled runtime.

The CPU backends, in the order the dialog lists them:

- **suyu Dynarmic JIT (Baseline)** is the default. It is the normal emulator packaged on
  its own, and the most compatible choice. Windows only.
- **suyu Hybrid JIT + AOT** runs pre-compiled game code first and falls back to the
  Dynarmic JIT for anything it doesn't cover. Performance varies by game; compare it
  with the Dynarmic JIT export.
- **suyu static AOT (Experimental)** runs pre-compiled code only, with no JIT fallback. It
  stops when it reaches code it doesn't cover. Use it for testing.

Static and Hybrid exports are now much faster. For Mario Kart 8 Deluxe v4.0.0, a race
runs at about 60 fps on the Dynarmic JIT export and about 56 fps on Windows for a
static export (about 60 on macOS and Linux). A Hybrid export with Clang reached about
51 fps on Windows; Hybrid performance still varies by game. On Windows, reaching that speed needs Clang/LLVM
installed (see **Select an export format** below); without it, static and Hybrid
exports still work but run slower. **Re-export a game you already exported to get the
speed-up.** Static is still tested on Mario Kart 8 Deluxe only; the Dynarmic JIT stays
the default and the most compatible choice for other games.

The dialog also shows a coverage line for the selected game, for example whether static
AOT looks safe to try, or how much code still needs Hybrid's JIT fallback. Playing a
Hybrid export records any code that still needed the JIT, so re-exporting the same game
later can cover more of it. **Import / Export coverage file** lets players share this
information; the file only holds module IDs, code offsets and counts, no game code.
Hybrid remains the choice when static won't run a game.

## Select an export format

- **Build** is the default on Windows. It produces a standalone program. The Hybrid
  and static backends compile the generated code, which needs CMake and a C compiler
  and can take a long time for large games.

  On Windows, the game code runs faster when LLVM/Clang is installed, because the
  export then compiles it with clang-cl instead of Microsoft's compiler (it also
  compiles faster). To get it, run `winget install LLVM.LLVM`, or add **C++ Clang
  tools for Windows** in the Visual Studio Installer. The export uses Microsoft's
  compiler when Clang is missing or does not work, and the export log and the
  package's `README_NATIVE_EXPORT.txt` say which compiler it used. To choose a
  particular `clang-cl.exe`, set `SUYU_CLANG_CL` to its path; to use Microsoft's
  compiler anyway, set `SUYU_RECOMP_COMPILER=msvc`.
- **Source** writes the generated C project, its `CMakeLists.txt` and a build script,
  and stops there. It does not include a compiled program. This is the only format for
  Linux and macOS.

While exporting, the progress bar follows the real work. The status line shows what
is happening, for example "Compiling 312/940 files (main)" or "Copying game data:
X of Y MB". During some setup steps the bar pauses while the status line keeps
updating.

The **Full code scan** option is hidden; it would not change the generated code.
For Hybrid, **Allow Dynarmic fallback if a module fails to recompile** lets the
export continue on the JIT for that module. Clear it to stop the export instead.

## Game updates

The **Update** row shows which update the export will use:

| Message | Meaning |
|---|---|
| Update X is available and turned on | The export uses that update. |
| Update included in this game file | The game file carries its own update, and the export uses it. |
| Updates are turned off for this game | Turn them on in the game's **Properties > Add-Ons**. |
| The installed update cannot be read | Reinstall it, or check your keys. Otherwise the export uses the base game. |
| No update installed | The export uses the base game version. |

When an update is used, a **From:** line shows where it comes from.

**Install Update File...** installs an update `.nsp` into suyu. It first checks the
file really is an update for the selected game. There is no Cancel while it installs.

If no update is installed when you export, suyu asks whether to install one or
**Export Without Update**. If the game file includes an update but updates are turned
off for that game, the export stops before it starts; turn updates on and try again.

An extracted ExeFS/RomFS folder carries no version information. Set the application
version overrides in suyu's configuration before exporting one, and check the version
on the game's title screen.

## Output folder

The default output folder is:

- `exports` inside the source folder, when suyu runs from a source checkout. Git
  ignores this folder.
- Your **Downloads** folder otherwise.

Keep the path short. Very long output paths can break the build; the dialog warns you.

## What goes into the package

**Always:** the game data needed to run, and a copy of your suyu settings: graphics,
CPU, audio, system and applet settings. Debug and unsafe settings, folder paths, input
devices and personal data are left out.

**Optional** (needs a game whose title ID suyu can read, such as one chosen **From
Library**):

- **Include save data for this game**.
- **Include transferable shader cache**. Shaders your GPU has already built are
  reused, so the game stutters less the first time you play. On the Mario Kart 8
  Deluxe JIT package this removed a 5-second freeze at race start.
- **Include custom game configuration**. The game's own settings from suyu are added
  on top of your global settings.

Ticking these boxes now reliably includes save data, shader cache and per-game settings
in the package. Older exports could leave them out even when the box was ticked;
re-export if that happened to you.

**Never:** keys or firmware. See the next section.

## Keys and firmware

Exported games read keys and firmware from the suyu installed on the same computer:

- Windows: `%APPDATA%\suyu\keys` and `%APPDATA%\suyu\nand`.
- A portable suyu: its `user\` folder.
- A custom NAND folder set in suyu is honoured.

Before exporting, suyu warns if no firmware is installed.

When an exported Windows game starts, it checks for them:

- **Missing keys:** a dialog shows the exact keys folder. Choose **Install keys in
  suyu** (shown when the suyu that made the export is found), **Open folder** or
  **Quit**. Copying `prod.keys` into that folder works just as well.
- **Missing firmware:** a warning with the firmware folder. You can install firmware
  in suyu, open the folder, or choose **Continue anyway**. Mii screens and some menus
  may fail without firmware.

## Run the result

For a Windows Build export, open the package folder and run the game's `.exe`, or
`launch.bat`. The package keeps its own `user` folder for settings, saves and logs.

The title bar shows FPS, frame time, the CPU backend, and "Building N shaders"
while shaders are being built. A stutter at the same moment as a shader build means
the shader cache didn't have that shader yet.

Each launch, an exported game first prepares the shaders in its cache: you'll see a
progress bar ("Preparing shaders: N of M") and "Building shaders N/M" in the title
bar. This is normal and usually takes a few seconds (about 3 for Mario Kart 8 Deluxe).

The package reports the game's real update version. Packages made before v0.0.11
showed "Ver. 1.0.0"; re-export them.

Controllers are assigned automatically. Press **F12** for the controls panel, which now
also has a **Resolution Scale** choice — it's saved in the package and takes effect the
next time the game starts. See
[Controllers in exported games](./Controllers.md#controllers-in-exported-games).

## Steam

Tick **Add to Steam library when the export finishes** to add the export to Steam as a
non-Steam game. It needs a Windows export with a program: **Build**, or the Dynarmic
JIT backend.

- The shortcut runs the export's own program. It is named after the game and backend:
  `<Game> (suyu Dynarmic JIT)`, `<Game> (suyu Hybrid JIT + AOT)` or
  `<Game> (suyu static AOT)`.
- **...and replace an existing shortcut for the same game** removes only suyu's own
  shortcut that launches this game.
- Artwork (cover, banner, hero, logo and icon) is made from the game's icon.
- **...and fetch cover art from Wikipedia** uses Wikipedia cover art instead. It sends
  the game's title to English Wikipedia.
- Your other Steam shortcuts are kept exactly as they were. If suyu can't fully read
  Steam's shortcut file, it leaves the file untouched.
- **Restart Steam** to see the new shortcut.

Steam shows no description for non-Steam games; that is a Steam limitation.

## Discord

Exported games show up in Discord as playing suyu, with the game's cover art (from
Wikipedia, which receives the game's title). This is on by default: to turn it off for
a specific export, untick **Show this game in Discord (cover art from Wikipedia)**
before exporting. To turn it off later, set `enabled=0` in the `discord.ini` file next
to the game's `.exe`.

## Known limitations

- Portable settings, the keys/firmware check and automatic controller assignment
  apply to Windows packages only.
- Steam shortcuts go to the first Steam account found on the computer.
- A game that isn't in your suyu library is named in Steam after its file name.
- Static exports on Windows can crash now and then when closing. A fix is included
  but not yet confirmed on Windows.
- Linux static exports start slowly.
- Static and Hybrid compatibility is per game. A successful launch only shows that
  the parts of the game you played work.
- Exports check that the game code and game data come from the same version, but this
  can't catch every mismatch. Test a package before relying on it.
- Standalone `.nca` exports stop when an installed update changes RomFS.
