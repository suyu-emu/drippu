# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the opt-in Mario Kart runtime inside the game's build directory."""
import base64
import datetime
import hashlib
import json
import re
import shutil
import ssl
import subprocess
import sys
import tempfile
from pathlib import Path

ASSETS = Path(__file__).resolve().parent
TITLE = 0x0100152000022000
SOURCE_FILES = ("session.h", "session.m", "network.h", "network.c", "sockets.inc", "tls.h", "tls.m")
CATALOG_URL = "https://nextendo.network/service-catalog/v1/game-catalog"
CATALOG_KEY = "xI+atR6VesrCH89ZDZD4t3WQLcYOFEhk5F0HLMzgWk4="
ROOT_SHA256 = "a654c0eff8aa20a72280ea0ef2b5c2a17392922dd78ffbb113862e1ad2c66d49"


def check_catalog(envelope, display):
    from Crypto.Signature import eddsa
    if envelope.get("key_id") != "catalog-key-1":
        raise ValueError("Unrecognized Nextendo catalog signing key.")
    payload = base64.b64decode(envelope["payload"], validate=True)
    key = eddsa.import_public_key(base64.b64decode(CATALOG_KEY))
    eddsa.new(key, "rfc8032").verify(payload, base64.b64decode(envelope["signature"], validate=True))
    catalog = json.loads(payload)
    now = datetime.datetime.now(datetime.timezone.utc)
    if (catalog["document_type"] != "game_catalog" or catalog["revision"] < 1 or
            datetime.datetime.fromisoformat(catalog["expires_at"]) <= now):
        raise ValueError("Invalid or expired Nextendo game catalog.")
    for game in catalog["games"]:
        if (game["enabled"] and f"{TITLE:016x}" in [t.lower() for t in game["title_ids"]]
                and game["versions"]["policy"] == "exact" and display in game["versions"]["accepted"]
                and set(game["required_capabilities"]) <= {"versions.exact.v1"}):
            return
    raise ValueError("Nextendo does not currently accept this Mario Kart version or runtime.")


def preflight(title, version, display, client):
    if (title, version, display) != (TITLE, "v1507328", "4.0.1"):
        raise ValueError("The Nextendo runtime requires Mario Kart 8 Deluxe 4.0.1 (v1507328).")
    if not re.fullmatch(r"nxc_[A-Za-z0-9_-]{1,128}", client):
        raise ValueError("Enter the Drippu public client ID from the Nextendo developer portal.")
    result = subprocess.run(["/usr/bin/curl", "--fail", "--silent", "--show-error", "--max-time", "20",
                             "--max-filesize", "1048576", CATALOG_URL], capture_output=True, check=False)
    if result.returncode:
        raise ValueError("Could not download the signed Nextendo game catalog.")
    check_catalog(json.loads(result.stdout), display)


def build_id(identity, client, release):
    if not re.fullmatch(r"nxc_[A-Za-z0-9_-]{1,128}", client):
        raise ValueError("Invalid public Nextendo client ID.")
    patches = ["mk8-online.patch", "account-menu.patch"]
    if release:
        patches.insert(0, "nx2apple-teardown.patch")
    digest = hashlib.sha256((identity + client).encode())
    for filename in [*patches, *SOURCE_FILES, "engine.py", "CatalogSignature.swift", "nextendo-network-ca.pem"]:
        digest.update((ASSETS / filename).read_bytes())
    return digest.hexdigest()[:24]


def prepare(source, work, identity, client, release):
    root = work / ("engine-" + build_id(identity, client, release))
    if root.is_dir():
        return root
    cert = ssl.PEM_cert_to_DER_cert((ASSETS / "nextendo-network-ca.pem").read_text())
    if hashlib.sha256(cert).hexdigest() != ROOT_SHA256:
        raise ValueError("Unexpected Nextendo TLS root fingerprint.")
    patches = ["mk8-online.patch", "account-menu.patch"]
    if release:
        patches.insert(0, "nx2apple-teardown.patch")
    with tempfile.TemporaryDirectory(dir=work, prefix=".engine-") as tmp:
        copy = Path(tmp) / "engine"
        copy.mkdir()
        if release:
            for directory in ("dm", "scripts", "docs"):
                shutil.copytree(source / directory, copy / directory, symlinks=True)
        else:
            with subprocess.Popen(["/usr/bin/git", "-C", str(source), "archive", identity,
                                   "dm", "scripts", "docs"], stdout=subprocess.PIPE) as archive:
                subprocess.run(["/usr/bin/tar", "-xf", "-", "-C", str(copy)], stdin=archive.stdout, check=True)
                if archive.wait():
                    raise ValueError("Could not extract the pinned NxNative sources.")
        for name in patches:
            subprocess.run(["/usr/bin/patch", "-s", "-f", "-F", "0", "-p1", "-i", str(ASSETS / name)],
                           cwd=copy, check=True)
        host = copy / "dm/host"
        shutil.copyfile(ASSETS / "CatalogSignature.swift", copy / "dm/ios/Sources/CatalogSignature.swift")
        for name in SOURCE_FILES:
            shutil.copyfile(ASSETS / name, host / name)
        (host / "drippu_nextendo_config.h").write_text(f'#define DRIPPU_NEXTENDO_CLIENT_ID "{client}"\n')
        (host / "drippu_nextendo_ca.h").write_text(
            "static const unsigned char drippu_ca[] = {" + ",".join(map(str, cert)) + "};\n")
        copy.rename(root)
    return root


def run_cli(repo, command):
    with subprocess.Popen([sys.executable, "-B", "-u", str(repo / "scripts/build_ipa_from_nsp.py"),
                           *command], cwd=repo, stdin=subprocess.DEVNULL) as child:
        try:
            return child.wait()
        finally:
            if child.poll() is None:
                child.terminate()
