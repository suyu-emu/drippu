# drippu

<h1 align="center">
  <br>
  <img src="dist/drippu.svg" alt="drippu" height="128">
  <br>
  <b>drippu</b>
  <br>
</h1>

<h4 align="center">
Nintendo Switch emulator and native recompiler — a continued fork of suyu, based on <a href="https://git.eden-emu.dev/eden-emu/eden">Eden</a>, which itself descends from yuzu.
</h4>

<p align="center">
  <a href="#status">Status</a> |
  <a href="#building">Building</a> |
  <a href="#license">License</a>
</p>

---

> **drippu is an active fork continuation of the suyu codebase** (formerly published as suyu v0.04).
>
> Development continues here under GPL-3.0-or-later. This repository is not a final archive — builds, fixes, and platform work may continue from this tree.
>
> The project inherits suyu’s codebase and history (itself based on Eden / yuzu). Binary and internal target names may still say `suyu` in places; the product presents as **drippu**.

## About

drippu is a Nintendo Switch emulator and AArch64 native recompiler written in C++. It can run decrypted Switch titles using either:

- **HLE/emulation mode** — full hardware-level emulation via the drippu core (GPU, CPU, audio, services)
- **Recompiler mode** — ahead-of-time static recompilation of Switch AArch64 game code to native x86-64 executables, bundled with drippu's HLE backend

Based on [Eden](https://git.eden-emu.dev/eden-emu/eden), with improvements to UI, recompiler, and platform support carried forward from suyu and continued in drippu.

## Status

Continued development under the drippu name. Automated builds are published to the [releases page](../../releases) by GitHub Actions (Windows, Linux, Android) when configured.

Platforms: Windows, Linux, Android. macOS/iOS support depends on the build configuration in use.

## Legal Notice

drippu is a GPLv3 program, which allows fully free redistribution of its source code and releases liability of its authors for how this software is used as stated in Section 15 and 16.

The drippu Emulator program does not circumvent Nintendo's technological protection measures (TPMs) as the user is required to provide both the Nintendo Switch software & the encryption keys for these games, and the drippu Emulator uses a mode of the Advanced Encryption Standard (AES), an open encryption standard established by the US NIST, along with the encryption keys that the user themselves must lawfully acquire, to decrypt the software. As the standard is public and available to use by all, it does not constitute as the Digital Market Copyright Act's (DMCA) definition of "circumventing a technological measure" as defined in Section 1201(a)(3).

The drippu Emulator also falls under the exemptions stated in Section 1201(f) of the DMCA as this software was created for the purposes of reverse engineering the Nintendo Switch software (known as Horizon OS) to create interoperability with Nintendo Switch games and software with the Windows, macOS, and GNU/Linux operating systems.

Any aggressive DMCA claims or takedown notices against projects that explicitly disclaim piracy support, require user-provided keys, and limit functionality to interoperability (such as drippu) could constitute overreach or misuse of the DMCA.

As derived from §512(f), if Nintendo (or an affiliated entity) knowingly materially misrepresents that a project like drippu is infringing (or circumvents TPMs) when it does not, especially if they fail to consider fair use, interoperability exemptions under §1201(f), or the fact that the emulator requires user-provided keys and does not itself contain proprietary Nintendo code, they can be made liable for any Damages against drippu.

## Building

### Dependencies

- CMake 3.15+, Ninja
- Qt 6.4+ (without bundled Qt: `-DYUZU_USE_BUNDLED_QT=OFF`)
- Vulkan SDK, libusb, OpenSSL

### Windows

```bat
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF -GNinja
cmake --build build --target suyu suyu-cmd
```

### Linux

```sh
sudo apt-get install ninja-build qt6-base-dev libqt6svg6-dev libusb-1.0-0-dev libssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF -GNinja
cmake --build build --target suyu suyu-cmd
```

### Android

```sh
cd src/android && ./gradlew assembleMainlineRelease
```

Build targets remain `suyu` / `suyu-cmd` for now (deep path and target renames are deferred).

## License

GPL-3.0-or-later. See [LICENSE.txt](LICENSE.txt).
