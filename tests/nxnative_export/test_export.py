# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise selection, build-result validation and publication without a commercial title."""
import contextlib
import importlib.util
import io
import json
import plistlib
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

WORKER = Path(__file__).resolve().parents[2] / "dist/nxnative/export.py"
spec = importlib.util.spec_from_file_location("nxnative_export", WORKER)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


@unittest.skipUnless(sys.platform in ("darwin", "win32"), "Apple export build host")
class ExportTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        base = self.root / "game [base].nsp"
        update = self.root / "second folder" / base.name
        update.parent.mkdir()
        base.write_bytes(b"base")
        update.write_bytes(b"update")
        keys = self.root / "prod.keys"
        keys.touch()
        self.args = SimpleNamespace(checkout=self.root, keys=keys, output=self.root / "exports",
                                    files=[base, update], title=0x010093801237C000, target="ios")
        self.nx = mock.Mock(COMPRESSED={".nsz": ".nsp", ".xcz": ".xci"})
        self.nx.package_reader.return_value = contextlib.nullcontext(mock.Mock())
        self.nx.scan_nsps.side_effect = self.scan
        self.nx.nsp_versions.return_value = ["v0", "v327680"]
        self.nx.vnum.side_effect = lambda v: int(v[1:])
        self.nx.cli.side_effect = self.pack
        (self.root / "README.md").write_text("Version 2.1.0 played on iPhone. Audio remains untested.")
        self.profile = mock.Mock(dir=self.root)
        self.profile.get.return_value = "Fixture"
        self.profile.builds.return_value = {"v327680": {"display": "2.1.0"}}
        self.games = mock.Mock()
        self.games.by_title.return_value = self.profile

    def scan(self, folder, reader, title):
        by_content = {p.read_bytes(): p for p in folder.iterdir()}
        self.assertEqual(set(by_content), {b"base", b"update"})
        return title, {"base": by_content[b"base"], "updates": [(327680, by_content[b"update"])],
                       "dlc": []}

    def pack(self, argv):
        values = dict(zip(argv[1::2], argv[2::2]))
        self.assertEqual(values["--platform"], self.args.target)
        self.assertEqual(values["--package"], "v327680")
        self.assertEqual(values["--keys"], str(self.args.keys))
        self.assertEqual(values["--title-id"], f"{self.args.title:016X}")
        product = Path(values["--out"])
        info = plistlib.dumps({"CFBundleExecutable": "fixture"})
        if self.args.target == "ios":
            with zipfile.ZipFile(product, "w") as archive:
                archive.writestr("Payload/Fixture.app/Info.plist", info)
                archive.writestr("Payload/Fixture.app/fixture", b"fixture executable")
                archive.writestr("Payload/Fixture.app/romfs.img", b"fixture RomFS")
        else:
            exe = product / "Contents/MacOS/fixture"
            exe.parent.mkdir(parents=True)
            exe.write_bytes(b"fixture executable")
            exe.chmod(0o755)
            (product / "Contents/Info.plist").write_bytes(info)
        return 0

    def run_worker(self, reply="build\n", windows_driver=None):
        log = io.StringIO()
        with contextlib.redirect_stdout(log), mock.patch.object(sys, "stdin", io.StringIO(reply)):
            if sys.platform == "win32" and windows_driver is None:
                windows_driver = self.nx.cli
            rc = worker.build(self.args, self.nx, self.games, windows_driver=windows_driver)
        events = [json.loads(line) for line in log.getvalue().splitlines()]
        self.assertEqual(list(self.args.output.glob(".drippu-nx-*")), [])
        return rc, events

    def test_platform_specific_evidence_and_artifacts(self):
        targets = [("ios", "iPhone")]
        if sys.platform == "darwin":
            targets.append(("mac-app", "unconfirmed"))
        for target, evidence in targets:
            with self.subTest(target=target):
                self.args.target = target
                rc, events = self.run_worker()
                self.assertEqual(rc, 0)
                self.assertIn(evidence, events[0]["msg"])
                self.assertIn("2.1.0 (v327680)", events[0]["msg"])
                artifact = Path(events[-1]["path"])
                self.assertEqual(artifact.parent, self.args.output)
                worker.validate_product(artifact, target)
                _, repeated = self.run_worker()
                self.assertNotEqual(repeated[-1]["path"], str(artifact))
                self.assertTrue(artifact.exists())

    def test_windows_one_game_route(self):
        def windows_pack(argv):
            self.assertEqual(argv[-2:], ["--romfs", "bundle"])
            return self.pack(argv)

        rc, events = self.run_worker(windows_driver=windows_pack)
        self.assertEqual(rc, 0)
        self.assertEqual(events[-1]["ev"], "drippu_product")
        self.nx.check_metal.assert_not_called()
        self.nx.cli.assert_not_called()

    @unittest.skipUnless(sys.platform == "darwin", "Mac export checks")
    def test_storage_checked_before_and_after_build_confirmation(self):
        with mock.patch.object(worker.shutil, "disk_usage", return_value=SimpleNamespace(free=9)):
            with self.assertRaisesRegex(ValueError, "10 bytes.*Choose an export folder"):
                self.run_worker()
        with mock.patch.object(worker.shutil, "disk_usage", side_effect=[
                SimpleNamespace(free=10**12), SimpleNamespace(free=1)]):
            with self.assertRaisesRegex(ValueError, "Choose an export folder"):
                self.run_worker()
        self.nx.cli.assert_not_called()

    def test_compressed_space_and_matching_cache(self):
        compressed = self.root / "fixture.XCZ"
        compressed.write_bytes(b"1234")
        extraction = self.root / "extract/v327680"
        (extraction / "ExeFS").mkdir(parents=True)
        (extraction / "ExeFS/main").write_bytes(b"program")
        (extraction / "RomFS.img").write_bytes(b"image")
        (extraction / "extract.key").write_text("stamp")
        family = {"base": self.args.files[0]}
        self.nx.nsp_versions.return_value = {"v327680": self.args.files[1]}
        self.nx.reused.return_value = ""
        def cached():
            return worker.cached_extraction(self.nx, self.root, extraction, family, "v327680", self.args.title)
        self.assertEqual(cached(), 5)
        self.nx.extraction_key.assert_called_once_with(self.root, "program", tuple(self.args.files),
                                                       self.nx.extract_flags.return_value)
        with mock.patch.object(worker.shutil, "disk_usage", return_value=SimpleNamespace(free=13)):
            msg = worker.check_space(self.root, [compressed, self.args.files[0]], extraction, cached())
            self.assertIn("minimum: 13 bytes", msg)
            self.assertIn("5 bytes of reusable", msg)
            self.assertIn("Below the estimate", msg)
        self.nx.reused.return_value = None
        self.assertEqual(cached(), 0)
        self.nx.reused.return_value = ""
        (extraction / "ExeFS/main").unlink()
        self.assertEqual(cached(), 0)

    @unittest.skipUnless(sys.platform == "darwin", "Mac launcher")
    def test_launcher_check_failure_prevents_publication(self):
        self.args.target = "mac-app"
        def pack_launcher(argv):
            self.pack(argv)
            binary = Path(argv[argv.index("--out") + 1]) / "Contents/MacOS/fixture"
            binary.write_bytes(b"fixture with DM_PREFLIGHT\0")
            return 0
        self.nx.cli.side_effect = pack_launcher
        with mock.patch.object(worker.subprocess, "run", return_value=SimpleNamespace(returncode=4)) as launch:
            with self.assertRaisesRegex(ValueError, "verification failed"):
                self.run_worker()
            self.assertEqual(launch.call_args.kwargs["env"]["DM_PREFLIGHT"], "1")
        self.assertFalse(list(self.args.output.glob("*.app")))
        self.nx.cli.side_effect = self.pack
        with mock.patch.object(worker.subprocess, "run") as launch:
            _, events = self.run_worker()
            self.assertIn("does not support", events[-1]["verification"])
            launch.assert_not_called()

    def test_mismatched_mac_engine_and_invalid_xcode(self):
        app = self.root / "NX2Apple.app"
        with self.assertRaisesRegex(ValueError, "NX2Apple 1.5.0"):
            worker.mac_release(app, self.root)
        contents = app / "Contents"
        contents.mkdir(parents=True)
        (contents / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleIdentifier": "org.nxnative.NSPortBuilder", "CFBundleShortVersionString": "1.5.0"}))
        with self.assertRaisesRegex(ValueError, "engine missing or mismatched"):
            worker.mac_release(app, self.root)
        with self.assertRaisesRegex(ValueError, "Xcode application"):
            worker.select_xcode(app)

    def test_windows_rejects_slim_ipa(self):
        def slim(argv):
            product = Path(argv[argv.index("--out") + 1])
            with zipfile.ZipFile(product, "w") as ipa:
                ipa.writestr("Payload/Test.app/Info.plist", plistlib.dumps({"CFBundleExecutable": "game"}))
                ipa.writestr("Payload/Test.app/game", b"fixture executable")
                ipa.writestr("Payload/Test.app/romfs.id", b"external game data")
            return 0

        with self.assertRaisesRegex(ValueError, "bundled RomFS"):
            self.run_worker(windows_driver=slim)
        self.assertFalse(list(self.args.output.glob("*.ipa")))

    @unittest.skipUnless(sys.platform == "win32", "Windows volume fallback")
    def test_copy_when_hard_links_are_unavailable(self):
        inputs = self.root / "inputs"
        inputs.mkdir()
        with mock.patch.object(worker.os, "link", side_effect=OSError("different volumes")):
            with contextlib.redirect_stdout(io.StringIO()):
                staged = worker.stage_inputs(self.args.files, inputs)
        self.assertEqual([p.read_bytes() for p in staged], [b"base", b"update"])
        staged[0].write_bytes(b"test copy")
        self.assertEqual(self.args.files[0].read_bytes(), b"base")

    def test_windows_rejects_wrong_release_before_launch(self):
        with self.assertRaisesRegex(ValueError, "NX2Apple Windows"):
            worker.windows_environment(self.root)

    def test_decline_after_check(self):
        rc, events = self.run_worker("")
        self.assertNotEqual(rc, 0)
        self.assertEqual([e["ev"] for e in events], ["drippu_ready"])
        self.nx.cli.assert_not_called()

    def test_error_or_missing_product_never_publishes(self):
        self.nx.cli.side_effect = None
        self.nx.cli.return_value = 1
        self.assertEqual(self.run_worker()[0], 1)
        self.nx.cli.return_value = 0
        with self.assertRaises(FileNotFoundError):
            self.run_worker()
        self.assertFalse(list(self.args.output.glob("*.ipa")))

    def test_wrong_addon_refused(self):
        self.nx.scan_nsps.side_effect = lambda folder, reader, title: (
            title, {"base": folder / self.args.files[0].name, "updates": [], "dlc": []})
        with self.assertRaisesRegex(ValueError, "only update or DLC"):
            self.run_worker()
        self.nx.cli.assert_not_called()

    def test_concurrent_destination_preserved(self):
        def competing_pack(argv):
            self.pack(argv)
            product = Path(argv[argv.index("--out") + 1])
            (self.args.output / product.name).write_bytes(b"existing")
            return 0

        self.nx.cli.side_effect = competing_pack
        with self.assertRaises(FileExistsError):
            self.run_worker()
        self.assertEqual(next(self.args.output.glob("*.ipa")).read_bytes(), b"existing")

    def test_malformed_ipa(self):
        def missing_executable(argv):
            archive = Path(argv[argv.index("--out") + 1])
            with zipfile.ZipFile(archive, "w") as ipa:
                ipa.writestr("Payload/Fixture.app/Info.plist",
                             plistlib.dumps({"CFBundleExecutable": "absent"}))
            return 0

        self.nx.cli.side_effect = missing_executable
        with self.assertRaises(KeyError):
            self.run_worker()
        self.assertFalse(list(self.args.output.glob("*.ipa")))

    def test_checkout_busy(self):
        with worker.lock_directory(self.root), self.assertRaisesRegex(ValueError, "Another Drippu"):
            self.run_worker()
        self.nx.cli.assert_not_called()


if __name__ == "__main__":
    unittest.main()
