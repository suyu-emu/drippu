# Build with NxNative

Drippu runs the packer locally on an Apple silicon Mac with macOS 26.6 or later.
Select Xcode 27.2 and install its Metal toolchain. For iPhone and iPad exports,
also install the iOS platform in Xcode Settings → Components.

Use a dedicated, writable checkout. Run these commands once from the directory
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

Select the checkout folder, its `.venv/bin/python` executable and your console's
`prod.keys`. Supply the base package and any update or DLC package files that
belong with it. NxNative reads their content metadata to choose the newest
selected version. Installed Drippu NAND updates and saves are separate.

Choose an export folder with room for the build. Upstream estimates twice the
game size plus 10 GB for a first build. Intermediate files and detailed logs
stay under `.drippu-nxnative` in that folder. Close the build before removing
that cache. Repeat builds keep earlier exports and add a number to the new filename.

A Mac export opens from Finder. An iOS export produces an unsigned IPA for an
A17 Pro or later iPhone or M1 or later iPad running iOS/iPadOS 26 or later.
Sign and install the IPA with your sideloading tool. Upstream's game notes
describe the devices, versions, and parts of gameplay it has tested.

[Upstream build and installation guide](https://github.com/seedds/nxnative/tree/692c8047a940c581d6dade58829a2a754f0ca00b#how-to-play)
