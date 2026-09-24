#!/usr/bin/env python3
"""Small regression tests for verify_release_artifacts.py."""

from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock


SCRIPT = Path(__file__).with_name("verify_release_artifacts.py")
spec = importlib.util.spec_from_file_location("verify_release_artifacts", SCRIPT)
assert spec and spec.loader
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class VerifyReleaseArtifactsTest(unittest.TestCase):
    def test_prefers_libretro_binary_over_dependency_dll(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Qt6Core.dll").touch()
            (root / "suyu_libretro.dll").touch()
            self.assertEqual(verify.libretro_binary(root).name, "suyu_libretro.dll")

    def test_discovers_suyu_inside_renamed_macos_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory) / "drippu.app"
            executable = bundle / "Contents" / "MacOS" / "suyu"
            executable.parent.mkdir(parents=True)
            executable.touch()
            executable.chmod(0o755)
            (bundle / "Contents" / "Info.plist").write_bytes(
                b"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                b"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
                b" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
                b"<plist version=\"1.0\"><dict><key>CFBundleExecutable</key>"
                b"<string>suyu</string></dict></plist>"
            )
            (Path(directory) / "drippu-cmd").touch()
            (Path(directory) / "drippu-cmd").chmod(0o755)
            self.assertEqual(verify.desktop_binaries(Path(directory), "macos")[0], executable)
            verify.verify_structure(Path(directory), Path(directory) / "drippu-macos-arm64.tar.gz")

    def test_structure_requires_desktop_cli_binary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "drippu"
            executable.touch()
            with self.assertRaises(RuntimeError):
                verify.verify_structure(root, root / "drippu-linux-x64.tar.gz")

    def test_libretro_load_restores_linux_library_path_on_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "suyu_libretro.so"
            binary.touch()
            with mock.patch.object(verify, "check_dependencies"), mock.patch.object(
                verify.ctypes, "CDLL", side_effect=OSError("load failed")
            ):
                with mock.patch.dict(verify.os.environ, {"LD_LIBRARY_PATH": "before"}, clear=False):
                    with self.assertRaises(RuntimeError):
                        verify.smoke_libretro(binary, "linux")
                    self.assertEqual(verify.os.environ["LD_LIBRARY_PATH"], "before")

    def test_rejects_zip_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "bad.zip"
            with zipfile.ZipFile(archive, "w") as package:
                package.writestr("../outside", "must not extract")
            with self.assertRaises(RuntimeError):
                verify.verify(archive, "structure", "generic")

    def test_allows_relative_macos_framework_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "framework.tar.gz"
            framework_binary = "drippu.app/Contents/Frameworks/QtCore.framework/Versions/A/QtCore"
            with tarfile.open(archive, "w:gz") as package:
                binary = tarfile.TarInfo(framework_binary)
                binary.size = 4
                package.addfile(binary, io.BytesIO(b"mach"))
                current = tarfile.TarInfo(
                    "drippu.app/Contents/Frameworks/QtCore.framework/Versions/Current"
                )
                current.type = tarfile.SYMTYPE
                current.linkname = "A"
                package.addfile(current)
                top_level = tarfile.TarInfo(
                    "drippu.app/Contents/Frameworks/QtCore.framework/QtCore"
                )
                top_level.type = tarfile.SYMTYPE
                top_level.linkname = "Versions/Current/QtCore"
                package.addfile(top_level)

            extracted = root / "extracted"
            extracted.mkdir()
            verify.unpack(archive, extracted)
            link = extracted / "drippu.app/Contents/Frameworks/QtCore.framework/QtCore"
            self.assertTrue(link.is_symlink())
            self.assertEqual(link.read_bytes(), b"mach")

    def test_appimage_extract_returns_squashfs_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "drippu-linux-x86_64.AppImage"
            archive.write_bytes(b"\x7fELF")
            destination = root / "extracted"
            destination.mkdir()

            def fake_run(command, **kwargs):
                self.assertIn("--appimage-extract", command)
                (kwargs["cwd"] / "squashfs-root").mkdir()
                return subprocess.CompletedProcess(command, 0, "", "")

            with mock.patch.object(verify.subprocess, "run", side_effect=fake_run):
                extracted = verify.unpack(archive, destination)
            self.assertEqual(extracted, destination / "squashfs-root")

    def test_rejects_tar_symlink_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "bad.tar.gz"
            with tarfile.open(archive, "w:gz") as package:
                link = tarfile.TarInfo("escape")
                link.type = tarfile.SYMTYPE
                link.linkname = "../outside"
                package.addfile(link)
            extracted = root / "extracted"
            extracted.mkdir()
            with self.assertRaises(RuntimeError):
                verify.unpack(archive, extracted)


if __name__ == "__main__":
    unittest.main()
