The kit contains release-specific host objects and link libraries, not emulator
sources. It needs the Windows x64 MSVC toolchain, Windows SDK, CMake 3.22+, and
the selected generator. clang-cl may compile generated modules separately using
`src/suyu_cmd/recomp_modules`; Python is only needed when packaging the release.
The host uses MSVC, defaults to Release and rejects Debug. Its compiler must be
at least as recent as the producer version recorded in `manifest.json`.
Release CI uses the updated VS 2022 toolset 14.44 baseline; newer MSVC versions
can consume that kit. This follows Microsoft's forward binary compatibility
contract and avoids newer hosted-runner STL helpers silently raising the minimum.

Integrate `include(ExportBuildKit)` in `src/suyu_cmd/CMakeLists.txt`, call
`suyu_export_build_kit_add_probes()` before the shared `SUYU_CMD_TARGETS` loop,
and `suyu_export_build_kit_add_package()` after it. Configure a single-config
Ninja Release build with `SUYU_EXPORT_BUILD_KIT=ON`, then build target
`export-build-kit` and ship `bin/export-build-kit` beside the application.

Point the exporter host configuration at that directory, retaining
`SUYU_CMD_RECOMP_DIR`, `SUYU_RECOMP_HYBRID`, and optional
`SUYU_CMD_RECOMP_PREBUILT_DIR`. Pass
`SUYU_EXPORT_BUILD_KIT_REVISION=suyu-aot-kit-abi6-fm1-gg1-fpx1-control-r3`.
The resulting target is `suyu-cmd-static`, output in `bin`. All ABI 6 handshake
sidecars and a nonempty `recomp_modules.cmake` are required. Changing the registry
or host layout requires changing the revision on both sides.

The probe registry is only used to complete the producer link. The packager
explicitly excludes its object. Both strict and hybrid frontend objects are
compiled with GV2, FM1, feature, GG1 and FPX1 handshakes. SDK libraries remain
bare linker names; every other object/library is copied and referenced relative
to the kit. Unknown path-dependent flags and missing inputs abort packaging.
The consumer checks every copied input against the package SHA256 manifest.

Packaging is currently limited to Windows MSVC single-config Ninja Release.
Objects and libraries may contain debugging source-path strings; they do not
require those paths to exist. Retain GPL source/license distribution obligations
through the corresponding release source archive and license bundle.
