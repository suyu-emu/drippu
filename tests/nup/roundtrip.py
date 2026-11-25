#!/usr/bin/env python3
"""Pack a synthetic game with the Python writer and read it through the C++ loader."""

import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "nup"))
from nup import pack  # noqa: E402
from nup.read import NupReader  # noqa: E402

TITLE_ID = 0x0100123456780000


def create_exefs(exefs, title_id):
    exefs.mkdir(parents=True)
    npdm = bytearray(0x340)
    npdm[:4] = b"META"
    struct.pack_into("<4I", npdm, 0x70, 0x80, 0x40, 0xC0, 0x240)
    npdm[0x80:0x84] = b"ACI0"
    npdm[0x2C0:0x2C4] = b"ACID"
    struct.pack_into("<Q", npdm, 0x90, title_id)
    (exefs / "main.npdm").write_bytes(npdm)
    (exefs / "main").write_bytes(b"NSO0" + bytes(0xFC))


def run(probe, root, compressed):
    path = root / ("compressed.nup" if compressed else "raw.nup")
    with mock.patch.object(pack, "have_zstd", return_value=compressed):
        pack.pack(str(path), str(root / "dump"), names={None: "NUP round-trip fixture"})
    with NupReader(str(path)) as nup:
        compressed_members = sum(info[-1] == 1 for info in nup.files)
        assert (compressed_members > 0) == compressed
    subprocess.run([probe, path, "valid"], check=True)

    corrupted = bytearray(path.read_bytes())
    toc_size = struct.unpack_from("<Q", corrupted, 0x10)[0]
    toc_end = 0x80 + toc_size
    # Make the root point to itself and recalculate the digest so the tree checks
    # are exercised independently of the TOC hash.
    package_count, dirs, files = struct.unpack_from("<IQQ", corrupted, 0x84)
    slots = 0x80 + 0x30 + package_count * 0x40 + dirs * 0x18 + files * 0x50
    struct.pack_into("<I", corrupted, slots, 0)
    corrupted[0x18:0x38] = hashlib.sha256(corrupted[0x80:toc_end]).digest()
    bad = root / "bad-tree.nup"
    bad.write_bytes(corrupted)
    subprocess.run([probe, bad, "invalid"], check=True)

    corrupted[0x18] ^= 1
    bad = root / "bad-hash.nup"
    bad.write_bytes(corrupted)
    subprocess.run([probe, bad, "invalid"], check=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="drippu-nup-") as temp:
        root = Path(temp)
        dump = root / "dump"
        create_exefs(dump / "base" / "exefs", TITLE_ID)
        create_exefs(dump / "updates" / "65536" / "exefs", TITLE_ID | 0x800)
        nested = dump / "base" / "romfs" / "nested"
        nested.mkdir(parents=True)
        (nested / "small.bin").write_bytes(bytes((1, 2, 3)))
        (nested / "empty.bin").touch()
        (nested / "compressible.bin").write_bytes(b"x" * (3 * 128 * 1024))
        update = dump / "updates" / "65536" / "romfs"
        update.mkdir()
        (update / "updated.bin").write_bytes(bytes((4, 5, 6)))
        run(sys.argv[1], root, False)
        if pack.have_zstd():
            run(sys.argv[1], root, True)
        else:
            print("zstandard is unavailable, so only the raw fixture was exercised")
