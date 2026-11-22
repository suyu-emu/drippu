# SPDX-License-Identifier: GPL-3.0-or-later
"""Run with the configured NxNative Python and DRIPPU_TEST_NXNATIVE set to its checkout."""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/export_integration"))
import make_fixture


@unittest.skipUnless(os.environ.get("DRIPPU_TEST_NXNATIVE"), "requires the pinned Mac toolchain")
class RealPackerTest(unittest.TestCase):
    def test_metadata_preflight_and_packer_failure(self):
        # A legitimate synthetic content meta, deliberately without a program.
        # This reaches the real extractor, compatibility check and packing CLI.
        title = 0x01000000E5700000
        meta = make_fixture.make_meta_nca(title, 0x80, title)
        name = hashlib.sha256(meta).hexdigest()[:32] + ".cnmt.nca"
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            base = folder / "original fixture [v999].nsp"
            base.write_bytes(make_fixture.make_pfs0([(name, meta)]))
            keys = folder / "prod.keys"
            keys.write_text("key_area_key_application_00 = " + "7a" * 16 +
                            "\nheader_key = " + bytes(range(32)).hex() + "\n")
            for target in ("mac-app", "ios"):
                with self.subTest(target=target):
                    result = subprocess.run(
                        [sys.executable, str(ROOT / "dist/macos/nxnative/export.py"),
                         "--checkout", os.environ["DRIPPU_TEST_NXNATIVE"], "--keys", str(keys),
                         "--output", tmp, "--target", target, str(base)],
                        input="build\n", capture_output=True, text=True, timeout=90)
                    events = [json.loads(s) for s in result.stdout.splitlines() if s.startswith("{")]
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("v0", next(e["msg"] for e in events if e["ev"] == "drippu_ready"))
                    self.assertTrue(any(e["ev"] == "stage" for e in events))
                    self.assertIn("no Program NCA", result.stdout)
                    self.assertFalse(any(e["ev"] == "drippu_product" for e in events))
                    work = folder / ".drippu-nxnative" / f"{title:016X}" / target
                    self.assertTrue((work / "survey/logs/exefs.log").is_file())
                    self.assertEqual(list(folder.glob(".drippu-nx-*")), [])
