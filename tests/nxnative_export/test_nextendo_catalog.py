# SPDX-License-Identifier: GPL-3.0-or-later
import base64
import datetime
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

try:
    from Crypto.PublicKey import ECC
    from Crypto.Signature import eddsa
except ImportError:
    eddsa = None

path = Path(__file__).resolve().parents[2] / "dist/nxnative/nextendo/engine.py"
spec = importlib.util.spec_from_file_location("nextendo_engine", path)
engine = importlib.util.module_from_spec(spec)
spec.loader.exec_module(engine)


@unittest.skipUnless(eddsa, "requires the NxNative Python crypto dependency")
class CatalogVerification(unittest.TestCase):
    def setUp(self):
        key = ECC.generate(curve="Ed25519")
        override = patch.object(engine, "CATALOG_KEY", base64.b64encode(key.public_key().export_key(format="raw")))
        override.start()
        self.addCleanup(override.stop)
        self.signer = eddsa.new(key, "rfc8032")
        self.game = dict(enabled=True, title_ids=["0100152000022000"],
                         versions=dict(policy="exact", accepted=["4.0.1"]),
                         required_capabilities=["versions.exact.v1"])
        self.catalog = dict(document_type="game_catalog", revision=1,
                            expires_at=(datetime.datetime.now(datetime.timezone.utc) +
                                        datetime.timedelta(days=1)).isoformat(), games=[self.game])

    def envelope(self):
        payload = json.dumps(self.catalog).encode()
        return dict(key_id="catalog-key-1", payload=base64.b64encode(payload),
                    signature=base64.b64encode(self.signer.sign(payload)))

    def test_accept_exact_version(self):
        engine.check_catalog(self.envelope(), "4.0.1")

    def test_reject_tampering(self):
        envelope = self.envelope()
        envelope["payload"] = base64.b64encode(b'{}')
        with self.assertRaises(ValueError):
            engine.check_catalog(envelope, "4.0.1")

    def test_reject_expired_catalog(self):
        self.catalog["expires_at"] = "2020-01-01T00:00:00Z"
        with self.assertRaises(ValueError):
            engine.check_catalog(self.envelope(), "4.0.1")

    def test_reject_unavailable_game(self):
        for change in [dict(enabled=False), dict(title_ids=[]), dict(required_capabilities=["other"]),
                       dict(versions=dict(policy="exact", accepted=["4.0.2"]))]:
            with self.subTest(change=change), patch.dict(self.game, change), self.assertRaises(ValueError):
                engine.check_catalog(self.envelope(), "4.0.1")
