#!/usr/bin/env python3
"""Small regression tests for verify_release_artifacts.py."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile


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
            executable = Path(directory) / "drippu.app" / "Contents" / "MacOS" / "suyu"
            executable.parent.mkdir(parents=True)
            executable.touch()
            executable.chmod(0o755)
            self.assertEqual(verify.desktop_binaries(Path(directory), "macos")[0], executable)

    def test_rejects_zip_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "bad.zip"
            with zipfile.ZipFile(archive, "w") as package:
                package.writestr("../outside", "must not extract")
            with self.assertRaises(RuntimeError):
                verify.verify(archive, "structure", "generic")


if __name__ == "__main__":
    unittest.main()
