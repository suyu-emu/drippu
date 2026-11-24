# SPDX-License-Identifier: GPL-3.0-or-later
"""Run with python3 -m unittest discover -s tests/nxnative_export on Apple Silicon."""
import os
import platform
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parents[1] / "dist/nxnative/nextendo"
ENGINE = Path(os.environ.get("DRIPPU_TEST_ENGINE", "/Applications/NX2Apple.app/Contents/Resources/engine"))


def run(*command, cwd=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f"{command[0]} exited with {result.returncode}\n{result.stdout}{result.stderr}")


@unittest.skipUnless(platform.system() == "Darwin" and platform.machine() == "arm64", "requires Apple SDKs")
class AppleRuntime(unittest.TestCase):
    def test_account_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "session"
            run("clang", "-fobjc-arc", "-Wall", "-Werror", "-Wno-deprecated-declarations",
                HERE / "test_nextendo_session.m", "-framework", "AuthenticationServices",
                "-framework", "AppKit", "-framework", "Security", "-framework", "Foundation", "-o", binary)
            run(binary)

    @unittest.skipUnless((ENGINE / "dm/host/Makefile").is_file(), "requires a pinned native engine")
    def test_guest_udp_and_verified_tls(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            host = root / "dm/host"
            host.mkdir(parents=True)
            patches = ["mk8-online.patch", "account-menu.patch"]
            if (ENGINE / ".engine-id").is_file():
                patches.insert(0, "nx2apple-teardown.patch")
            for patch in patches:
                for line in (ASSETS / patch).read_text().splitlines():
                    if line.startswith("--- a/"):
                        path = Path(line[6:])
                        (root / path).parent.mkdir(parents=True, exist_ok=True)
                        if not (root / path).exists():
                            shutil.copyfile(ENGINE / path, root / path)
                run("/usr/bin/patch", "-s", "-f", "-F", "0", "-p1", "-i", ASSETS / patch, cwd=root)
            for path in (ENGINE / "dm/host").glob("*.h"):
                shutil.copyfile(path, host / path.name)
            for path in ASSETS.iterdir():
                if path.suffix in {".h", ".inc", ".c", ".m"}:
                    shutil.copyfile(path, host / path.name)
            run("python3", ENGINE / "dm/games/game.py", "header", "0100152000022000", host / "game_profile.h")
            (host / "drippu_nextendo_config.h").write_text('#define DRIPPU_NEXTENDO_CLIENT_ID "nxc_fixture"\n')
            # The fixture trust root and its private key only exist in this temp directory.
            run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=Test Root",
                "-addext", "basicConstraints=critical,CA:TRUE", "-out", "ca.pem", "-keyout", "ca.key", cwd=root)
            run("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=fixture.nintendo.net",
                "-keyout", "leaf.key", "-out", "leaf.req", cwd=root)
            (root / "ext.cnf").write_text("subjectAltName=DNS:fixture.nintendo.net\nextendedKeyUsage=serverAuth\n")
            run("openssl", "x509", "-req", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-in", "leaf.req",
                "-out", "leaf.pem", "-days", "1", "-extfile", "ext.cnf", cwd=root)
            cert = ssl.PEM_cert_to_DER_cert((root / "ca.pem").read_text())
            (host / "drippu_nextendo_ca.h").write_text("static const unsigned char drippu_ca[] = {" +
                                                       ",".join(map(str, cert)) + "};")
            flags = ["-fobjc-arc", "-Wno-deprecated-declarations", "-I", str(host)]
            for sdk, target in [("iphoneos", "arm64-apple-ios17.0"), ("macosx", "arm64-apple-macos14")]:
                sdkroot = subprocess.check_output(["xcrun", "--sdk", sdk, "--show-sdk-path"], text=True).strip()
                run("xcrun", "--sdk", sdk, "swiftc", "-parse-as-library", "-sdk", sdkroot,
                    "-target", target, "-emit-object", ASSETS / "CatalogSignature.swift", "-o", root / f"{sdk}.o")
                run("clang", "-target", target, "-isysroot", sdkroot, *flags, "-fsyntax-only",
                    *(host / name for name in ("hle_net.c", "hle_offline.c", "hle_account.c", "network.c", "tls.m", "session.m")))
            binary = root / "transport"
            run("clang", *flags, HERE / "nextendo_transport.c", host / "tls.m", host / "network.c",
                "-framework", "Foundation", "-framework", "Security", "-o", binary)

            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(root / "leaf.pem", root / "leaf.key")
            for hostname in ("fixture.nintendo.net", "mismatch.nintendo.net"):
                with socket.socket() as listener:
                    listener.bind(("127.0.0.1", 0))
                    listener.listen()
                    listener.settimeout(8)
                    def serve():
                        with listener.accept()[0] as raw:
                            raw.settimeout(5)
                            try:
                                with context.wrap_socket(raw, server_side=True) as tls:
                                    if tls.recv(5) == b"hello":
                                        tls.sendall(b"world")
                            except (ConnectionError, ssl.SSLError):
                                pass  # expected when the guest rejects the wrong hostname
                    thread = threading.Thread(target=serve, daemon=True)
                    thread.start()
                    run(binary, str(listener.getsockname()[1]), hostname)
                    thread.join(6)
                    self.assertFalse(thread.is_alive())
