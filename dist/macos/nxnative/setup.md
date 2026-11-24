# Build with NxNative

Drippu runs the packer locally on an Apple silicon Mac with macOS 26.6 or later.
Both toolchains build a Mac app or a standalone iPhone/iPad IPA. Select an
Xcode 27.2 app in the export dialog or leave the field empty to follow the
system selection. Install its Metal toolchain and, for iOS exports, the iOS
platform in Xcode Settings → Components.

## NX2Apple release

Install the [NX2Apple 1.5.0 Mac release](https://github.com/LNPANDY/NX2Apple/releases/tag/v1.5.0)
and open it once to complete setup. Choose **NX2Apple 1.5.0 for Mac** in
Drippu and select the installed app. Drippu uses its bundled Python and tools
and the matching writable engine in your user's NX2Apple Application Support
folder. The chosen Xcode applies only to the export process.

## NxNative checkout

Choose **NxNative checkout** for a dedicated, writable checkout. Run these commands once from the directory
where you want to keep NxNative.

```sh
git clone https://github.com/seedds/nxnative.git
cd nxnative
git checkout --detach 692c8047a940c581d6dade58829a2a754f0ca00b
xcodebuild -downloadComponent MetalToolchain
brew install python@3.14 xcodegen zstd glslang
"$(brew --prefix python@3.14)/bin/python3.14" -m venv .venv
.venv/bin/python -m pip install lz4 'capstone>=5,<6' fonttools nsz zbic
```

Select the checkout folder and its `.venv/bin/python` executable.

## Check and build

Select your console's `prod.keys`. Supply the base package and any update or DLC package files that
belong with it. NxNative reads their content metadata to choose the newest
selected version. Installed Drippu NAND updates and saves are separate.

Choose an export folder, then click **Check compatibility** to see its free space.
Drippu budgets the selected package sizes, with a 3.5× allowance for compressed
NSZ/XCZ files. It warns below the first-build estimate of twice this budget plus
10 GB and stops below the package-space minimum. A matching, completed
extraction reduces both figures. Choose a folder on another drive if needed.
Intermediate files and detailed logs
stay under `.drippu-nxnative` in that folder. Close the build before removing
that cache. Repeat builds keep earlier exports and add a number to the new filename.

A Mac export opens from Finder. Before publishing it, Drippu verifies the
bundle and runs the launcher's startup check when the launcher supports it.
That check exits before loading game code. The result appears in the dialog.
An iOS export produces an unsigned IPA for an
A17 Pro or later iPhone or M1 or later iPad running iOS/iPadOS 26 or later.
Sign and install the IPA with your sideloading tool. Upstream's game notes
describe the devices, versions, and parts of gameplay it has tested.

[Upstream build and installation guide](https://github.com/seedds/nxnative/tree/692c8047a940c581d6dade58829a2a754f0ca00b#how-to-play)

### Experimental Nextendo matchmaking

The Mac exporter can build Mario Kart 8 Deluxe 4.0.1 (v1507328) with a
Drippu-owned runtime patch for either target. Select **Enable experimental
Nextendo matchmaking**. The public client ID is prefilled and can be edited. The exporter
verifies the signed game catalog before building. Each player signs in on
the device running their export.

Drippu's bundled public client (`nxc_evYdYRXj_MWa`) is registered for
`game.matchmaking` and the Apple callback `drippu-nextendo://oauth/callback`.
Players can sign in with their Nextendo account. Only maintainers using the
separate desktop loopback flow need to set `DRIPPU_NEXTENDO_LOOPBACK_CLIENT_ID`
to a client registered with a loopback redirect.

In the Mac emulator, use **Multiplayer > Nextendo** to sign in and enable
game traffic routing before launch. Maintainers can set
`-DDRIPPU_NEXTENDO_CLIENT_ID=nxc_…` when configuring Drippu, or set the
environment variable of the same name when launching the Mac app.
[Nextendo developer portal](https://nextendo.network/developers) ·
[Integration guide](https://wiki.nextendo.network/integration)

On iOS, open **Nextendo Account** in the exported game's launcher. On Mac,
use **Nextendo Account** in the running game's application menu. The same
control can revoke the session. Refresh tokens stay in the local Keychain.

Online racing is unverified. The runtime checks the game server's TLS
certificate and hostname. On November 22, 2026 the documented Mario Kart
endpoint presented a self-signed certificate with a different hostname.
