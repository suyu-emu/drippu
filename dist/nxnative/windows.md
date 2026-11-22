# Build an iPhone or iPad IPA on Windows

Extract the complete **NX2Apple-windows-x64.zip** from the
[NX2Apple v1.5.0 release](https://github.com/LNPANDY/NX2Apple/releases/tag/v1.5.0)
into a writable folder. In Drippu, select the folder containing
`NX2Apple.exe`, `engine`, and `_internal`.

Open NX2Apple once and use its Dependencies panel to configure its Windows
build tools, including the iPhoneOS SDK, LLVM-MinGW, .NET, and Apple's Metal
Developer Tools for Windows. Select the same x64 Python 3.12 or later executable
with the build packages installed in Drippu. Its path can be entered directly.

Select your console's `prod.keys`, the game's base package, and any update
and DLC packages for that title. Choose an export folder with space for the
extracted game, build cache, and IPA. Staging can require an additional copy
of the selected packages when building on a different drive.

**Check compatibility** reads the package metadata and checks the tools.
**Build** runs NX2Apple's experimental Windows driver and writes a standalone
IPA with the game's RomFS included. Sign and install the IPA with your
sideloading tool. Support varies by game, particularly for games that capture
shaders at runtime. Detailed tool logs and reusable build files are under
`.drippu-nxnative` in the export folder.

[Windows toolchain setup and limitations](https://github.com/LNPANDY/NX2Apple/blob/v1.5.0/frontend-qt/README.md#windows-build)
