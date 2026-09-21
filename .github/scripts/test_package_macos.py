#!/usr/bin/env python3
"""Regression tests for macOS release packaging path checks."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("package_macos.py")
spec = importlib.util.spec_from_file_location("package_macos", SCRIPT)
assert spec and spec.loader
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageMacOSTest(unittest.TestCase):
    def test_otool_listing_drops_install_id(self) -> None:
        install_name = "@executable_path/../Frameworks/libSDL3.0.dylib"
        entries = [
            install_name,
            "@executable_path/../Frameworks/libfoo.dylib",
            "/usr/lib/libSystem.B.dylib",
        ]
        self.assertEqual(
            package.dependent_libraries(entries, install_name),
            [
                "@executable_path/../Frameworks/libfoo.dylib",
                "/usr/lib/libSystem.B.dylib",
            ],
        )

    def test_executables_keep_every_load_command(self) -> None:
        entries = ["/usr/lib/libSystem.B.dylib", "@rpath/QtCore.framework/Versions/A/QtCore"]
        self.assertEqual(package.dependent_libraries(entries, None), entries)

    def test_framework_executable_path_id_is_not_a_cli_dependency(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / "drippu.app"
            frameworks = app / "Contents" / "Frameworks"
            binary = frameworks / "libSDL3.0.dylib"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"")
            cli = root / "drippu-cmd"
            cli.write_bytes(b"")
            app_executable = app / "Contents" / "MacOS" / "suyu"
            app_executable.parent.mkdir(parents=True)
            app_executable.write_bytes(b"")
            install_name = "@executable_path/../Frameworks/libSDL3.0.dylib"
            # Resolves for the .app executable. The same string on a Frameworks
            # dylib is its install name, not a load command the CLI must satisfy.
            self.assertTrue(package.reference_exists(install_name, app_executable, app, cli))
            self.assertFalse(package.reference_exists(install_name, binary, app, cli))
            self.assertTrue(
                package.reference_exists("@loader_path/libSDL3.0.dylib", binary, app, cli)
            )

    @unittest.skipUnless(sys.platform == "darwin", "needs otool")
    def test_real_dylib_install_id_is_excluded(self) -> None:
        clang = "clang"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "dep.c"
            source.write_text("void dep(void) {}\n", encoding="utf-8")
            binary = root / "libdep.dylib"
            install_name = "@executable_path/../Frameworks/libdep.dylib"
            subprocess.run(
                [
                    clang,
                    "-dynamiclib",
                    "-o",
                    str(binary),
                    "-install_name",
                    install_name,
                    str(source),
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(package.install_id(binary), install_name)
            self.assertNotIn(install_name, package.dependencies(binary))


if __name__ == "__main__":
    unittest.main()
