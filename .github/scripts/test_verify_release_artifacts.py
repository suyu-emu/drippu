#!/usr/bin/env python3
"""Small regression tests for verify_release_artifacts.py."""

from __future__ import annotations

import importlib.util
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
