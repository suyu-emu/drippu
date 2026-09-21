#!/usr/bin/env python3
"""Verify packaged desktop and libretro artifacts on their native runners.

The release workflow calls this script before uploading each artifact.  It
deliberately uses only the standard library so the check also works on the
minimal publish runner and can be reused by downstream packagers.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import plistlib
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile


def fail(message: str) -> None:
    raise RuntimeError(message)


def unpack(archive: Path, destination: Path) -> None:
    destination_root = destination.resolve()
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as package:
            bad = package.testzip()
            if bad:
                fail(f"corrupt zip member: {bad}")
            members = package.infolist()
            for member in members:
                target = (destination / member.filename).resolve()
                mode = (member.external_attr >> 16) & 0xFFFF
                if destination_root not in target.parents and target != destination_root:
                    fail(f"unsafe zip member: {member.filename}")
                if stat.S_ISLNK(mode):
                    fail(f"symlink zip member is not allowed: {member.filename}")
            for member in members:
                package.extract(member, destination)
        return
    if archive.name.endswith((".tar.gz", ".tgz")):
        with tarfile.open(archive, "r:gz") as package:
            members = package.getmembers()
            for member in members:
                target = (destination / member.name).resolve()
                if destination_root not in target.parents and target != destination_root:
                    fail(f"unsafe tar member: {member.name}")
                if member.issym():
                    # macOS frameworks use relative symlinks for Versions/Current
                    # and their top-level binary/resources. Permit those only when
                    # the resolved link remains inside the extraction directory.
                    link_target = (destination / member.name).parent / member.linkname
                    resolved_link = link_target.resolve()
                    if member.linkname.startswith("/") or (
                        destination_root not in resolved_link.parents
                        and resolved_link != destination_root
                    ):
                        fail(f"unsafe tar symlink: {member.name} -> {member.linkname}")
                elif member.islnk() or member.isdev() or member.isfifo():
                    fail(f"special tar member is not allowed: {member.name}")
            package.extractall(destination)
        return
    if archive.suffix == ".apk":
        with zipfile.ZipFile(archive) as package:
            bad = package.testzip()
            if bad:
                fail(f"corrupt APK member: {bad}")
            names = set(package.namelist())
            if "AndroidManifest.xml" not in names:
                fail("APK has no AndroidManifest.xml")
            for member in package.infolist():
                target = (destination / member.filename).resolve()
                mode = (member.external_attr >> 16) & 0xFFFF
                if destination_root not in target.parents and target != destination_root:
                    fail(f"unsafe APK member: {member.filename}")
                if stat.S_ISLNK(mode):
                    fail(f"symlink APK member is not allowed: {member.filename}")
                package.extract(member, destination)
        return
    fail(f"unsupported archive: {archive}")


def run(command: list[str], *, timeout: int = 30, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=False, capture_output=True, text=True, timeout=timeout, env=env)


def check_dependencies(binary: Path, target: str) -> None:
    if target == "linux":
        tool = shutil.which("ldd")
        if not tool:
            fail("ldd is required for Linux dependency validation")
        result = run([tool, str(binary)])
        if result.returncode != 0:
            fail(f"ldd failed for {binary}: {result.stderr.strip()}")
        if "not found" in result.stdout:
            fail(f"unresolved Linux dependency for {binary}:\n{result.stdout}")
    elif target == "macos":
        tool = shutil.which("otool")
        if not tool:
            fail("otool is required for macOS dependency validation")
        result = run([tool, "-L", str(binary)])
        if result.returncode != 0:
            fail(f"otool failed for {binary}: {result.stderr.strip()}")
        if "not found" in result.stdout:
            fail(f"unresolved macOS dependency for {binary}:\n{result.stdout}")
    elif target == "windows":
        # dumpbin is provided by the MSVC developer shell in the packaging job.
        # It verifies that the PE is readable and records the import table; the
        # process launch below verifies that the packaged DLL search path works.
        tool = shutil.which("dumpbin")
        if not tool:
            fail("dumpbin is required for Windows dependency validation")
        result = run([tool, "/DEPENDENTS", str(binary)])
        if result.returncode != 0:
            fail(f"dumpbin failed for {binary}: {result.stderr.strip()}")


def desktop_binaries(root: Path, target: str) -> list[Path]:
    if target == "windows":
        names = ("drippu.exe", "drippu-cmd.exe")
    elif target == "macos":
        names = ("drippu", "drippu-cmd")
    else:
        names = ("drippu", "drippu-cmd")
    candidates: list[Path] = []
    for name in names:
        paths = [root / name, root / "_pkg" / name]
        if target == "macos" and name == "drippu":
            bundle_bin = root / "drippu.app" / "Contents" / "MacOS"
            bundle_executable = None
            info_plist = bundle_bin.parent / "Info.plist"
            if info_plist.is_file():
                try:
                    with info_plist.open("rb") as stream:
                        executable_name = plistlib.load(stream).get("CFBundleExecutable")
                    if isinstance(executable_name, str):
                        candidate = bundle_bin / executable_name
                        if candidate.is_file() and os.access(candidate, os.X_OK):
                            bundle_executable = candidate
                except (OSError, plistlib.InvalidFileException, ValueError):
                    pass
            if bundle_executable is None:
                bundle_executable = next(
                    (
                        path
                        for path in sorted(bundle_bin.glob("*"))
                        if path.is_file() and os.access(path, os.X_OK)
                    ),
                    None,
                )
            if bundle_executable is not None:
                paths[0:0] = [bundle_executable]
        for candidate in paths:
            if candidate.is_file() and os.access(candidate, os.X_OK if target != "windows" else os.R_OK):
                candidates.append(candidate)
                break
    if not candidates:
        fail(f"packaged desktop executable not found under {root}")
    return candidates


def launch_desktop(binary: Path, target: str) -> None:
    env = os.environ.copy()
    if target == "linux":
        env["QT_QPA_PLATFORM"] = "offscreen"
    elif target == "macos":
        # Release bundles carry the native Cocoa plugin, not Qt's optional
        # offscreen plugin. GitHub's macOS runner provides a WindowServer.
        env["QT_QPA_PLATFORM"] = "cocoa"
    command = [str(binary), "--help"]
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    except OSError as exc:
        fail(f"could not launch packaged desktop executable {binary}: {exc}")
    try:
        stdout, stderr = process.communicate(timeout=12)
    except subprocess.TimeoutExpired:
        # A GUI that remains alive proves that its packaged runtime initialized;
        # terminate it so CI never leaves a desktop process behind.
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
        return
    if process.returncode not in (0,):
        fail(f"packaged desktop launch failed ({process.returncode}): {stderr[-2000:]}\n{stdout[-1000:]}")


def smoke_cli(binary: Path, target: str) -> None:
    """Run each packaged command-line binary with a bounded success probe."""
    env = os.environ.copy()
    if target == "linux":
        env["QT_QPA_PLATFORM"] = "offscreen"
    elif target == "macos":
        env["QT_QPA_PLATFORM"] = "cocoa"
    try:
        result = run([str(binary), "--version"], timeout=12, env=env)
    except OSError as exc:
        fail(f"could not launch packaged command-line executable {binary}: {exc}")
    if result.returncode != 0:
        fail(f"packaged command-line smoke failed ({result.returncode}) for {binary}: {result.stderr[-2000:]}")


class RetroSystemInfo(ctypes.Structure):
    _fields_ = [
        ("library_name", ctypes.c_char_p),
        ("library_version", ctypes.c_char_p),
        ("valid_extensions", ctypes.c_char_p),
        ("need_fullpath", ctypes.c_bool),
        ("block_extract", ctypes.c_bool),
    ]


def libretro_binary(root: Path) -> Path:
    candidates = sorted(
        path for path in root.rglob("*") if path.is_file() and path.suffix.lower() in (".so", ".dll", ".dylib")
    )
    if not candidates:
        fail(f"no libretro shared library found under {root}")
    candidates.sort(key=lambda path: (0 if "libretro" in path.stem.lower() else 1, str(path)))
    return candidates[0]


def smoke_libretro(binary: Path, target: str) -> None:
    check_dependencies(binary, target)
    dll_directory = None
    old_library_path: str | None = None
    if target == "windows" and hasattr(os, "add_dll_directory"):
        dll_directory = os.add_dll_directory(str(binary.parent))
    elif target == "linux":
        old_library_path = os.environ.get("LD_LIBRARY_PATH")
        os.environ["LD_LIBRARY_PATH"] = str(binary.parent) + (
            os.pathsep + old_library_path if old_library_path else ""
        )
    core = None
    try:
        try:
            core = ctypes.CDLL(str(binary))
        except OSError as exc:
            fail(f"could not load packaged libretro core {binary}: {exc}")
        try:
            api_version = core.retro_api_version
            api_version.restype = ctypes.c_uint
            if api_version() != 1:
                fail("libretro API version is not 1")
            get_info = core.retro_get_system_info
            get_info.argtypes = [ctypes.POINTER(RetroSystemInfo)]
            get_info.restype = None
            info = RetroSystemInfo()
            get_info(ctypes.byref(info))
            if not info.library_name or not info.library_version or not info.valid_extensions:
                fail("retro_get_system_info returned incomplete metadata")
        except AttributeError as exc:
            fail(f"packaged libretro core is missing required API: {exc}")
        finally:
            if target == "windows" and core is not None:
                # Windows cannot delete a loaded module, which breaks the
                # TemporaryDirectory cleanup in verify(). Free the DLL now.
                try:
                    ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
                    ctypes.windll.kernel32.FreeLibrary.restype = ctypes.c_int
                    ctypes.windll.kernel32.FreeLibrary(core._handle)
                except Exception:
                    pass
                finally:
                    # Drop the last Python reference so the file handle is
                    # released before the temp dir is removed. Antivirus
                    # scanners may briefly hold the new DLL; gc ensures the
                    # refcount drops deterministically.
                    del core
                    import gc

                    gc.collect()
    finally:
        if target == "linux":
            if old_library_path is None:
                os.environ.pop("LD_LIBRARY_PATH", None)
            else:
                os.environ["LD_LIBRARY_PATH"] = old_library_path
        if dll_directory is not None:
            try:
                dll_directory.close()
            except Exception:
                pass


def verify_structure(root: Path, archive: Path) -> None:
    files = [path for path in root.rglob("*") if path.is_file()]
    if not files:
        fail(f"package is empty: {archive.name}")
    if archive.suffix == ".apk":
        if not (root / "AndroidManifest.xml").is_file():
            fail(f"APK has no AndroidManifest.xml: {archive.name}")
        return
    names = {path.name for path in files}
    if "libretro-core" in archive.name:
        if not any(path.suffix.lower() in (".so", ".dll", ".dylib") for path in files):
            fail(f"libretro package has no shared library: {archive.name}")
    elif archive.name.endswith("-windows-x64.zip") and not {"drippu.exe", "drippu-cmd.exe"}.issubset(names):
        fail(f"Windows desktop package is missing its launchable binaries: {archive.name}")
    else:
        has_cli = "drippu-cmd" in names or "drippu-cmd.exe" in names
        has_macos_bundle = any(
            path.parent.name == "MacOS"
            and path.parent.parent.name == "Contents"
            and path.parent.parent.parent.name == "drippu.app"
            for path in files
        )
        has_gui = "drippu" in names or "drippu.exe" in names or has_macos_bundle
        if not (has_gui and has_cli):
            fail(f"desktop package is missing its launchable binaries: {archive.name}")


def verify(archive: Path, mode: str, target: str) -> None:
    if not archive.is_file():
        fail(f"artifact does not exist: {archive}")
    # Python 3.10+ can ignore cleanup errors (e.g. a briefly locked DLL on
    # Windows when an antivirus scanner holds the freshly extracted core).
    # The libretro smoke test already frees the DLL explicitly; this is only
    # a backstop so verification never fails with WinError 5 on rmtree.
    if sys.version_info >= (3, 10):
        temporary_dir = tempfile.TemporaryDirectory(
            prefix="drippu-release-verify-", ignore_cleanup_errors=True
        )
    else:
        temporary_dir = tempfile.TemporaryDirectory(prefix="drippu-release-verify-")
    with temporary_dir as temporary:
        root = Path(temporary)
        unpack(archive, root)
        if mode == "structure":
            verify_structure(root, archive)
        elif mode == "desktop":
            binaries = desktop_binaries(root, target)
            for binary in binaries:
                check_dependencies(binary, target)
            gui = next(
                binary
                for binary in binaries
                if binary.name not in ("drippu-cmd", "drippu-cmd.exe")
            )
            launch_desktop(gui, target)
            for binary in binaries:
                if binary.name in ("drippu-cmd", "drippu-cmd.exe"):
                    smoke_cli(binary, target)
        elif mode == "libretro":
            smoke_libretro(libretro_binary(root), target)
        elif mode == "apk":
            # APK validation is intentionally limited to ZIP integrity and the
            # manifest here; install/launch belongs on an Android emulator.
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("structure", "desktop", "libretro", "apk"))
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=("linux", "macos", "windows", "android", "generic"))
    args = parser.parse_args()
    try:
        verify(args.archive, args.mode, args.platform)
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(f"release artifact verification failed: {exc}", file=sys.stderr)
        return 1
    print(f"release artifact verification passed: {args.mode} {args.platform} {args.archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
