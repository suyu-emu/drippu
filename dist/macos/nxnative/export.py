# SPDX-License-Identifier: GPL-3.0-or-later
"""Mac export dialog worker. Stdout carries NxNative's JSON lines and two Drippu events."""
import argparse
import contextlib
import ctypes
import fcntl
import json
import os
import platform
import plistlib
import re
import signal
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REVISION = "692c8047a940c581d6dade58829a2a754f0ca00b"


def event(kind, **fields):
    print(json.dumps({"ev": kind, **fields}), flush=True)


@contextlib.contextmanager
def lock_directory(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError(f"Another Drippu export is using {path}.") from None
        yield
    finally:
        os.close(fd)


def stage_inputs(files, folder):
    staged = []
    for i, file in enumerate(files):
        if not file.is_file() or file.suffix.lower() not in (".nsp", ".xci", ".nsz", ".xcz"):
            raise ValueError(f"Select a readable NSP, XCI, NSZ or XCZ file: {file}")
        source = file.resolve()
        link = folder / source.name
        if link.exists():
            # nsz names output from the resolved source. Its uppercase-extension
            # path lets NxNative assign a distinct basename for colliding inputs.
            link = folder / f"{i}-{source.stem}{source.suffix.upper()}"
            while link.exists():
                link = link.with_name("_" + link.name)
        link.symlink_to(source)
        staged.append(link)
    return staged


def platform_evidence(notes, version, display, target):
    device = (r"\b(?:Mac|macOS|MacBook|iMac)\b" if target == "mac-app"
              else r"\b(?:iOS|iPadOS|iPhone|iPad)\b")
    versions = [re.escape(version)]
    if display:
        versions.append(re.escape(display))
    match_version = re.compile(r"(?<![\w.])(?:" + "|".join(versions) + r")(?!\w|\.\d)")
    if notes and notes.is_file():
        # The upstream reports are prose. Show their wording, including negative
        # results and limited tests, rather than inferring playability from a mention.
        for paragraph in notes.read_text(encoding="utf-8").split("\n\n"):
            if (match_version.search(paragraph) and re.search(device, paragraph, re.I)
                    and not paragraph.startswith(("#", "```", "|"))):
                return "Upstream notes mentioning this version and target:\n" + paragraph.strip()
    return "Device test status for this version is unconfirmed. See the upstream game notes if available."


def validate_product(product, target):
    if target == "ios":
        with zipfile.ZipFile(product) as archive:
            plists = [n for n in archive.namelist() if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n)]
            if len(plists) != 1:
                raise ValueError("The IPA has no unique application bundle.")
            info = plistlib.loads(archive.read(plists[0]))
            executable = info["CFBundleExecutable"]
            valid = archive.getinfo(plists[0].rsplit("/", 1)[0] + "/" + executable).file_size > 0
    else:
        contents = product / "Contents"
        info = plistlib.loads((contents / "Info.plist").read_bytes())
        binary = contents / "MacOS" / info["CFBundleExecutable"]
        valid = binary.is_file() and binary.stat().st_size > 0 and os.access(binary, os.X_OK)
    if not valid:
        raise ValueError("The exported bundle has no runnable executable.")


def publish(product, destination):
    # Atomic same-volume commit, even if another process created the destination
    # during the build. os.rename would replace an existing IPA on macOS.
    rename = ctypes.CDLL(None, use_errno=True).renamex_np
    if rename(os.fsencode(product), os.fsencode(destination), 4):  # Darwin RENAME_EXCL
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err), str(destination))


def build(args, nx, game):
    repo = args.checkout
    env = nx.tool_env()
    nx.check_native_python()
    nx.check_prereqs(env, repo / "dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate", args.target == "mac-app")
    nx.check_metal(sys.executable, repo, env)
    if not args.keys.is_file():
        raise ValueError("Select the prod.keys file for these packages.")
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".drippu-nx-", dir=args.output) as tmp:
        inputs = Path(tmp) / "inputs"
        inputs.mkdir()
        staged = stage_inputs(args.files, inputs)
        with nx.package_reader(repo, args.keys, env) as reader:
            title, family = nx.scan_nsps(inputs, reader.contents, args.title)
            reader.check_keys(family)
        included = {family["base"], *(p for _, p in family["updates"]), *(p for _, _, p in family["dlc"])}
        if family["base"] != staged[0] or set(staged) != included:
            raise ValueError("Select the base game first and only update or DLC packages for that game.")
        if any(p.suffix.lower() in nx.COMPRESSED for p in staged):
            nx.nsz_command(env)
        version = max(nx.nsp_versions(family), key=nx.vnum)
        title_id = f"{title:016X}"
        profile = game.by_title(title)
        builds = profile.builds() if profile else {}
        registered = version in builds
        name = profile.get("name", title_id) if profile else args.files[0].stem
        safe_name = re.sub(r"[/:\x00-\x1f]", "_", name).strip(" .")[:64] or title_id
        ext = ".ipa" if args.target == "ios" else ".app"
        filename = f"{safe_name} [{title_id}] [{version}]{ext}"
        destination = args.output / filename
        copy = 1
        while os.path.lexists(destination):
            copy += 1
            destination = args.output / f"{Path(filename).stem} ({copy}){ext}"
        label = f"{builds[version]['display']} ({version})" if registered else version
        status = "Registered in the pinned profile." if registered else "Experimental build outside the registered versions."
        notes = profile.dir / "README.md" if profile else None
        evidence = platform_evidence(notes, version, builds.get(version, {}).get("display"), args.target)
        event("drippu_ready", msg=f"{name} · {label}\n{status}\n{evidence}",
              notes=str(notes) if notes else "")
        if sys.stdin.readline().strip() != "build":
            return 1
        work = args.output / ".drippu-nxnative" / title_id / args.target
        work.mkdir(parents=True, exist_ok=True)
        product = Path(tmp) / filename
        event("say", msg=f"Build files and detailed logs: {work}")
        with lock_directory(repo), lock_directory(work):
            # The packer writes an unknown title's survey beside the input packages,
            # even with --work-dir. Keep its logs after the input links are removed.
            survey = work / "survey"
            survey.mkdir(exist_ok=True)
            (inputs / f"{title_id}_survey").symlink_to(survey, target_is_directory=True)
            rc = nx.cli([str(inputs), "--keys", str(args.keys), "--title-id", title_id,
                         "--package", version, "--platform", args.target, "--progress", "json",
                         "--work-dir", str(work), "--out", str(product)])
            if rc:
                return rc
            validate_product(product, args.target)
            publish(product, destination)
        event("drippu_product", path=str(destination))
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkout", required=True, type=Path)
    parser.add_argument("--keys", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=("ios", "mac-app"))
    parser.add_argument("--title", type=lambda s: int(s, 16))
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    if sys.version_info < (3, 12) or platform.machine() != "arm64":
        raise ValueError("Choose an arm64 Python 3.12 or later with the NxNative dependencies.")
    if tuple(map(int, platform.mac_ver()[0].split(".")[:2])) < (26, 6):
        raise ValueError("This NxNative revision requires macOS 26.6 or later.")
    args.checkout = args.checkout.expanduser().resolve()
    args.output = args.output.expanduser().resolve()
    args.keys = args.keys.expanduser().resolve()
    head = subprocess.run(["/usr/bin/git", "-C", str(args.checkout), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=False)
    if head.returncode or head.stdout.strip() != REVISION:
        raise ValueError(f"Select the NxNative checkout at revision {REVISION}. See Setup.")
    sys.path.insert(0, str(args.checkout / "scripts"))
    sys.path.insert(0, str(args.checkout / "dm/games"))
    import build_ipa_from_nsp as nx
    import game

    try:
        return build(args, nx, game)
    except nx.BuildError as exc:
        raise ValueError(str(exc)) from exc


if __name__ == "__main__":
    # Unwind our temporary directory on cancellation during preflight or review.
    # NxNative's cli installs its own child-process cleanup during the actual build.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, ImportError, zipfile.BadZipFile, plistlib.InvalidFileException) as exc:
        event("say", msg=str(exc))
        sys.exit(1)
