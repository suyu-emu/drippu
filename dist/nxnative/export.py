# SPDX-License-Identifier: GPL-3.0-or-later
"""Apple game export dialog worker. Stdout carries NxNative's JSON lines and two Drippu events."""
import argparse
import contextlib
import ctypes
import importlib.util
import json
import mmap
import os
import platform
import plistlib
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

NX2APPLE_RELEASE = "1.5.0"
MAC_ENGINE_ID = "99224562186a8fdb7e97afb872319a21941ec317-ebcbf51f244f"
REVISION = "692c8047a940c581d6dade58829a2a754f0ca00b"


def event(kind, **fields):
    print(json.dumps({"ev": kind, **fields}), flush=True)


@contextlib.contextmanager
def lock_directory(path):
    if os.name == "nt":
        import msvcrt

        # Closing the handle releases the byte-range lock, including on cancellation.
        with (path / ".drippu-export.lock").open("a+b") as lock:
            lock.seek(0)
            try:
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as exc:
                raise ValueError(f"Cannot lock {path} for this export: {exc}") from exc
            yield
        return

    import fcntl
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
        if os.name == "nt":
            # No Developer Mode or administrator rights required. Do not hard-link a
            # read-only file: clearing its read-only attribute on cleanup changes the source.
            try:
                if not os.access(source, os.W_OK):
                    raise PermissionError("read-only package")
                os.link(source, link)
            except OSError:
                event("say", msg=f"Copying {source.name} into the temporary build folder…")
                shutil.copyfile(source, link)
        else:
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


def check_space(output, files, extraction, cached_bytes):
    # NX2Apple's input-size floor gives compressed inputs a 3.5x expansion allowance.
    # NxNative's first-build planning estimate also covers the final product and build tools.
    expanded = sum(p.stat().st_size * (7 if p.suffix.lower() in (".nsz", ".xcz") else 2) // 2
                   for p in files)
    credit = min(expanded, cached_bytes)
    minimum = expanded - credit
    estimate = 2 * expanded + 10_000_000_000 - credit
    available = shutil.disk_usage(output).free
    def size(n):
        if n >= 2**30:
            return f"{n / 2**30:.2f} GiB"
        return f"{n / 2**20:.1f} MiB" if n >= 2**20 else f"{n:,} bytes"
    report = (f"Export volume ({output}): {size(available)} free. Estimated additional space: "
              f"{size(estimate)}. Package-space minimum: {size(minimum)}.")
    if credit:
        report += f" Includes {size(credit)} of reusable extraction data for {extraction.name}."
    if available < minimum:
        raise ValueError(report + " Choose an export folder on a volume with more space.")
    if available < estimate:
        report += " Below the estimate. The build may run out of space."
    return report


def cached_extraction(nx, repo, extraction, family, version, title):
    image = extraction / "RomFS.img"
    if not (image.is_file() and (extraction / "ExeFS/main").is_file() and
            (extraction / "extract.key").is_file()) or (extraction / "RomFS").exists():
        return 0
    packages = (family["base"], nx.nsp_versions(family)[version])
    # The packer stamps converted NSZ/XCZ files after decompression. Their
    # original input metadata cannot establish that the cached image matches.
    if any(p and p.suffix.lower() in nx.COMPRESSED for p in packages):
        return 0
    key = nx.extraction_key(repo, "program", packages, nx.extract_flags(title))
    if nx.reused(extraction / "extract.key", key) is None:
        return 0
    try:
        if not nx.rfs.source(extraction).is_dir():
            return 0
    except (OSError, ValueError, struct.error):
        return 0
    return image.stat().st_size


def check_mac_launcher(binary):
    # Both supported engines compile this switch into launchers that exit before
    # loading game code. Older launchers must never be started by this check.
    with binary.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data.find(b"DM_PREFLIGHT\0") == -1:
            return "Bundle verified. This launcher does not support the startup check."
    event("say", msg="Checking the Mac launcher before publishing the app…")
    try:
        result = subprocess.run([str(binary)], env={**os.environ, "DM_PREFLIGHT": "1"},
                                stdin=subprocess.DEVNULL, timeout=600)
    except subprocess.TimeoutExpired as exc:
        raise ValueError("Mac launcher verification timed out.") from exc
    if result.returncode:
        raise ValueError(f"Mac launcher verification failed (exit {result.returncode}).")
    return "Mac launcher startup check passed."


def validate_product(product, target, require_romfs=False):
    if target == "ios":
        with zipfile.ZipFile(product) as archive:
            plists = [n for n in archive.namelist() if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n)]
            if len(plists) != 1:
                raise ValueError("The IPA has no unique application bundle.")
            info = plistlib.loads(archive.read(plists[0]))
            executable = info["CFBundleExecutable"]
            prefix = plists[0].rsplit("/", 1)[0] + "/"
            valid = archive.getinfo(prefix + executable).file_size > 0
            if require_romfs and not any(
                (entry.filename == prefix + "romfs.img" and entry.file_size > 0)
                or (entry.filename.startswith(prefix + "romfs/") and not entry.is_dir())
                for entry in archive.infolist()
            ):
                raise ValueError("The single-game IPA is missing its bundled RomFS.")
    else:
        contents = product / "Contents"
        info = plistlib.loads((contents / "Info.plist").read_bytes())
        name = info["CFBundleExecutable"]
        if not isinstance(name, str) or not name or Path(name).name != name:
            raise ValueError("Invalid Mac launcher name in Info.plist.")
        binary = contents / "MacOS" / name
        if not binary.resolve().is_relative_to(product.resolve()):
            raise ValueError("The Mac launcher is outside the exported bundle.")
        valid = binary.is_file() and binary.stat().st_size > 0 and os.access(binary, os.X_OK)
    if not valid:
        raise ValueError("The exported bundle has no runnable executable.")
    if target == "mac-app":
        return binary


def publish(product, destination):
    # Atomic same-volume commit, even if another process created the destination
    # during the build. os.rename would replace an existing IPA on macOS.
    if os.name == "nt":
        os.rename(product, destination)  # MoveFileEx without REPLACE_EXISTING
        return
    rename = ctypes.CDLL(None, use_errno=True).renamex_np
    if rename(os.fsencode(product), os.fsencode(destination), 4):  # Darwin RENAME_EXCL
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err), str(destination))


def build(args, nx, game, windows_driver=None, staging=None):
    repo = args.checkout
    env = nx.tool_env()
    nx.check_native_python()
    nx.check_prereqs(env, repo / "dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate", args.target == "mac-app")
    if windows_driver is None:
        nx.check_metal(sys.executable, repo, env)
    if not args.keys.is_file():
        raise ValueError("Select the prod.keys file for these packages.")
    args.output.mkdir(parents=True, exist_ok=True)
    # On Windows the dialog owns the staging directory and can clean it even if
    # the complete worker/tool process tree is terminated.
    scratch = contextlib.nullcontext(staging) if staging else tempfile.TemporaryDirectory(
        prefix=".drippu-nx-", dir=args.output)
    with scratch as tmp:
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
        invalid = r'[<>:"/\\|?*\x00-\x1f]' if windows_driver else r"[/:\x00-\x1f]"
        safe_name = re.sub(invalid, "_", name).strip(" .")[:64] or title_id
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
        work = args.output / ".drippu-nxnative" / title_id / args.target
        extraction = work / "extract" / version
        def storage_report():
            cached = cached_extraction(nx, repo, extraction, family, version, title)
            return check_space(args.output, staged, extraction, cached)
        report = "\n" + storage_report() if windows_driver is None else ""
        event("drippu_ready", msg=f"{name} · {label}\n{status}\n{evidence}{report}",
              notes=str(notes) if notes else "")
        if sys.stdin.readline().strip() != "build":
            return 1
        work.mkdir(parents=True, exist_ok=True)
        product = Path(tmp) / filename
        event("say", msg=f"Build files and detailed logs: {work}")
        with lock_directory(repo), lock_directory(work):
            if windows_driver is None:
                storage_report()  # The volume or cache can change while the user reviews.
            # The packer writes an unknown title's survey beside the input packages,
            # even with --work-dir. Keep its logs after the input links are removed.
            command = [str(inputs), "--keys", str(args.keys), "--title-id", title_id,
                       "--package", version, "--platform", args.target, "--progress", "json",
                       "--work-dir", str(work), "--out", str(product)]
            survey_path = inputs / f"{title_id}_survey"
            if windows_driver is None:
                if getattr(args, "nx2apple", False) and args.target == "ios":
                    command.extend(["--romfs", "bundle"])
                survey = work / "survey"
                survey.mkdir(exist_ok=True)
                survey_path.symlink_to(survey, target_is_directory=True)
                rc = nx.cli(command)
            else:
                # NX2Apple's one-game IPA includes the RomFS in the application.
                try:
                    rc = windows_driver(command + ["--romfs", "bundle"])
                finally:
                    if (survey_path / "logs").is_dir():
                        shutil.copytree(survey_path / "logs", work / "survey/logs", dirs_exist_ok=True)
            if rc:
                return rc
            binary = validate_product(product, args.target, require_romfs=(
                windows_driver is not None or getattr(args, "nx2apple", False)))
            verification = check_mac_launcher(binary) if binary else ""
            publish(product, destination)
        event("drippu_product", path=str(destination), verification=verification)
    return 0


def windows_environment(installation):
    package = installation / "_internal/nspb_qt"
    version_file = package / "app_update.py"
    marker = f"APP_VERSION = '{NX2APPLE_RELEASE}'"
    if (not version_file.is_file() or marker not in version_file.read_text(encoding="utf-8").splitlines()
            or not (installation / "engine/scripts/build_ipa_from_nsp.py").is_file()):
        raise ValueError(f"Select the extracted NX2Apple Windows {NX2APPLE_RELEASE} release folder. See Setup.")
    # Import only the pure Python package, never the frozen launcher's _internal
    # directory. Its Qt and Python DLLs may belong to a different interpreter.
    spec = importlib.util.spec_from_file_location("nspb_qt", package / "__init__.py",
                                                  submodule_search_locations=[str(package)])
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    from nspb_qt import platform_util
    from nspb_qt.win import shim

    # shim.build_env also consults the process environment to locate the release's
    # bundled Windows binaries. The actual driver runs in a separate interpreter.
    os.environ["NSPB_REPO"] = str(installation / "engine")
    env = platform_util.child_env({"DM_FULL_ROMFS": "1"})
    return shim.build_env(env, python_exe=sys.executable)


def windows_build(args):
    if args.target != "ios":
        raise ValueError("The NX2Apple Windows driver builds iPhone and iPad IPAs.")
    if sys.version_info < (3, 12) or sys.maxsize <= 2**32 or platform.machine().lower() not in ("amd64", "x86_64"):
        raise ValueError("Select an x64 Python 3.12 or later with the NX2Apple build packages.")
    if not args.windows_prepared:
        env = windows_environment(args.checkout)
        # Start in the official shim environment so sitecustomize also applies
        # to this interpreter, the extractor, and compiler subprocesses.
        return subprocess.call([sys.executable, "-u", "-P", __file__, "--windows-prepared", *sys.argv[1:]],
                               env=env, stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)
    from nspb_qt.win import driver

    args.checkout = args.checkout / "engine"
    nx = driver.load_upstream(args.checkout)
    driver.patch(nx, args.checkout)
    sys.path.insert(0, str(args.checkout / "dm/games"))
    import game

    def run_driver(command):
        return subprocess.call([sys.executable, "-u", "-P", "-m", "nspb_qt.win.driver", *command],
                               stdin=subprocess.DEVNULL, stdout=sys.stdout, stderr=sys.stderr, cwd=args.checkout)

    try:
        return build(args, nx, game, windows_driver=run_driver, staging=args.staging)
    except nx.BuildError as exc:
        raise ValueError(str(exc)) from exc


def mac_release(app, engine_store):
    resources = app / "Contents/Resources"
    info = app / "Contents/Info.plist"
    metadata = plistlib.loads(info.read_bytes()) if info.is_file() else {}
    if (metadata.get("CFBundleIdentifier") != "org.nxnative.NSPortBuilder" or
            metadata.get("CFBundleShortVersionString") != NX2APPLE_RELEASE):
        raise ValueError(f"Select the NX2Apple {NX2APPLE_RELEASE} macOS release app.")
    engine = engine_store / MAC_ENGINE_ID
    for root in (resources / "engine", engine):
        stamp = root / ".engine-id"
        if (not stamp.is_file() or stamp.read_text().strip() != MAC_ENGINE_ID or
                not (root / "scripts/build_ipa_from_nsp.py").is_file()):
            raise ValueError(f"NX2Apple {NX2APPLE_RELEASE} engine missing or mismatched at {root}. "
                             "Open the release app once and complete its engine setup.")
    python = resources / "python/bin/python3"
    if not python.is_file() or not os.access(python, os.X_OK):
        raise ValueError(f"Bundled Python is missing from {app}.")
    return engine, python


def select_xcode(app):
    developer = app.expanduser().resolve() / "Contents/Developer"
    if not os.access(developer / "usr/bin/xcodebuild", os.X_OK):
        raise ValueError(f"Choose an Xcode application containing xcodebuild: {app}")
    os.environ["DEVELOPER_DIR"] = str(developer)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--staging", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--windows-prepared", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--nx2apple", action="store_true", help="use the installed NX2Apple macOS release")
    parser.add_argument("--xcode", type=Path, help="Xcode app to use for this export")
    parser.add_argument("--checkout", required=True, type=Path)
    parser.add_argument("--keys", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=("ios", "mac-app"))
    parser.add_argument("--title", type=lambda s: int(s, 16))
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args()
    args.checkout = args.checkout.expanduser().resolve()
    args.output = args.output.expanduser().resolve()
    args.keys = args.keys.expanduser().resolve()
    if os.name == "nt":
        if args.staging and not args.windows_prepared and sys.stdin.readline().strip() != "start":
            return 1
        return windows_build(args)
    if args.xcode:
        select_xcode(args.xcode)
    if args.nx2apple:
        store = Path.home() / "Library/Application Support/NX2Apple/engine"
        engine, python = mac_release(args.checkout, store)
        os.environ.update(PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1")
        if Path(sys.executable).resolve() != python.resolve() or not sys.flags.isolated:
            os.execv(str(python), [str(python), "-I", "-B", "-u", __file__, *sys.argv[1:]])
        tools = args.checkout / "Contents/Resources/tools/bin"
        os.environ["PATH"] = os.pathsep.join([str(tools), str(python.parent), os.environ.get("PATH", os.defpath)])
        if args.target == "ios":
            os.environ.update(DM_FULL_ROMFS="1", DM_COPY_ROMFS="YES")
        args.checkout = engine  # Build tools write into this installed copy, outside the .app.
    if sys.version_info < (3, 12) or platform.machine() != "arm64":
        raise ValueError("Choose an arm64 Python 3.12 or later with the NxNative dependencies.")
    if tuple(map(int, platform.mac_ver()[0].split(".")[:2])) < (26, 6):
        raise ValueError("This NxNative revision requires macOS 26.6 or later.")
    if not args.nx2apple:
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
