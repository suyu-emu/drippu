#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate a synthetic, bootable extracted-ExeFS fixture for suyu (stdlib only).

Writes `main` (NSO), `main.npdm` and `romfs.bin` into a directory. Everything is
synthesised here: no Nintendo keys, firmware, code or game data are used.
The title/program ID 0x0100000000E57A00 is SYNTHETIC and belongs to no real title.

Usage: python make_fixture.py OUTDIR
"""
import struct
import sys
from pathlib import Path

TITLE_ID = 0x0100000000E57A00  # synthetic; not a real title
MARKER = "SUYU_EXPORT_FIXTURE_OK"
ROMFS_FILE_NAME = "fixture.txt"
ROMFS_FILE_DATA = b"suyu synthetic export fixture\n"
PAGE = 0x1000
FILES = ["main", "main.npdm", "romfs.bin"]

# Load layout (module-relative): .text @0, .rodata @0x1000, .data @0x2000.
TEXT_LOC, RODATA_LOC, DATA_LOC = 0x0000, 0x1000, 0x2000
MOD0_OFF = 0x8              # text[4] holds this; MOD0 header lives at text+8
CODE_OFF = MOD0_OFF + 0x1C  # first instruction after MOD0 (0x24)
MSG_OFF = 0x40              # the marker's offset in .rodata, after the module path


def _msg() -> bytes:
    return MARKER.encode() + b"\n"


def _code_words() -> list:
    assert len(_msg()) == 23
    # Each entry is one little-endian AArch64 instruction word (hand-assembled).
    return [
        0xB0000000,  # adrp x0, #0x1000         ; x0 = page(pc)+0x1000 -> .rodata
        0x91010000,  # add  x0, x0, #0x40       ; the message, after the module path
        0xD28002E1,  # movz x1, #23             ; length of "SUYU_EXPORT_FIXTURE_OK\n"
        0xD40004E1,  # svc  #0x27               ; svcOutputDebugString(x0=str, x1=len)
        0xD29C2000,  # movz x0, #0xe100         ; 100,000,000 ns = 0x05F5E100 (low half)
        0xF2A0BEA0,  # movk x0, #0x5f5, lsl 16  ; (high half)
        0xD4000161,  # svc  #0xb                ; svcSleepThread(100 ms): gives suyu's debug-string
                     #                          ; flusher thread time to start; the buffered
                     #                          ; marker is logged when the process shuts down.
                     #                          ; (Keep this < 250 ms: see README.)
        0xD40000E1,  # svc  #0x7                ; svcExitProcess()
        0x14000000,  # b    .                   ; unreachable safety net
    ]


def make_nso() -> bytes:
    # --- .text: b <code>; MOD0 offset; MOD0; code; zero pad to a page ---
    text = bytearray()
    text += struct.pack("<I", 0x14000000 | (CODE_OFF >> 2))  # b #CODE_OFF (entry at text+0)
    text += struct.pack("<I", MOD0_OFF)                       # offset of MOD0 (read by exporter)
    text += struct.pack(
        "<7I",
        0x30444F4D,                  # "MOD0"
        DATA_LOC - MOD0_OFF,         # dynamic_offset (rel. to MOD0): lone DT_NULL in .data
        DATA_LOC + 0x10 - MOD0_OFF,  # bss_start (empty bss)
        DATA_LOC + 0x10 - MOD0_OFF,  # bss_end
        0, 0,                        # eh_frame_hdr start/end (none)
        0)                           # runtime module object offset
    assert len(text) == CODE_OFF
    for w in _code_words():
        text += struct.pack("<I", w)
    text += b"\x00" * (PAGE - len(text))
    assert len(text) % 4 == 0 and len(text) % PAGE == 0

    # .rodata starts with the module path every NSO carries ({u32 0, s32 length,
    # path}). FindModules (src/core/arm/debug.cpp) needs it to see the module, and
    # recompiled images are bound to modules through that list. The message follows.
    path = b"synthetic/main.nss"
    rodata = bytearray(struct.pack("<Ii", 0, len(path)) + path)
    rodata += b"\x00" * (MSG_OFF - len(rodata))
    rodata += _msg()
    rodata += b"\x00" * (PAGE - len(rodata))
    data = bytearray(PAGE)  # zero-filled: .dynamic is a lone DT_NULL

    file_off = 0x100
    offs = [file_off, file_off + len(text), file_off + len(text) + len(rodata)]
    build_id = b"SYNTHETIC-FIXTURE".ljust(0x20, b"\x00")
    hdr = bytearray()
    hdr += b"NSO0"
    hdr += struct.pack("<III", 0, 0, 0)  # version, reserved, flags=0 (no LZ4, no hash check)
    # (file offset, location, segment, alignment / bss_size)
    for off, loc, seg, extra in ((offs[0], TEXT_LOC, text, 1),
                                 (offs[1], RODATA_LOC, rodata, 1),
                                 (offs[2], DATA_LOC, data, 0)):
        hdr += struct.pack("<IIII", off, loc, len(seg), extra)
    hdr += build_id
    hdr += struct.pack("<III", len(text), len(rodata), len(data))  # "compressed" == raw sizes
    hdr += b"\x00" * 0x1C
    hdr += b"\x00" * 24   # api_info / dynstr / dynsym extents
    hdr += b"\x00" * 96   # segment hashes (unused: hash-check flags clear)
    assert len(hdr) == 0x100, len(hdr)
    return bytes(hdr) + bytes(text) + bytes(rodata) + bytes(data)


def _svc_mask_cap(svcs) -> list:
    """SyscallMask caps: id bits[0:5]=0b01111, mask bits[5:29], index bits[29:32] (24 SVCs each)."""
    groups = {}
    for s in svcs:
        groups[s // 24] = groups.get(s // 24, 0) | (1 << (s % 24))
    return [0xF | (mask << 5) | (idx << 29) for idx, mask in sorted(groups.items())]


def make_npdm() -> bytes:
    caps = [
        0x030043F7,                          # CorePriority: cores 0-3, prio 16-63 (suyu default)
        *_svc_mask_cap([0x07, 0x0B, 0x27]),  # ExitProcess, SleepThread, OutputDebugString
        0x3FFF | (0 << 15) | (3 << 19),      # KernelVersion 3.0
        0x7FFF | (128 << 16),                # HandleTable size 128
        0xFFFF | (1 << 17),                  # DebugFlags: allow_debug
    ]
    kac = struct.pack("<%dI" % len(caps), *caps)

    fah = struct.pack("<B3xQ4I", 1, 0xFFFFFFFFFFFFFFFF, 0, 0, 0, 0)     # FileAccessHeader
    fac = struct.pack("<B3xQ", 1, 0xFFFFFFFFFFFFFFFF) + b"\x00" * 0x20  # FileAccessControl
    sac = b""

    aci_off = 0x80
    fah_o = 0x40
    sac_o = fah_o + len(fah)
    kac_o = sac_o + len(sac)
    aci_hdr = (b"ACI0" + b"\x00" * 0xC + struct.pack("<Q", TITLE_ID) + b"\x00" * 8 +
               struct.pack("<6I", fah_o, len(fah), sac_o, len(sac), kac_o, len(kac)) +
               b"\x00" * 8)
    assert len(aci_hdr) == 0x40, len(aci_hdr)
    aci = aci_hdr + fah + sac + kac
    aci += b"\x00" * (-len(aci) % 0x10)

    acid_off = aci_off + len(aci)
    fac_o = 0x240
    sac_o2 = fac_o + len(fac)
    kac_o2 = sac_o2 + len(sac)
    acid_hdr = (b"\x00" * 0x200 + b"ACID" + struct.pack("<I", 0) + b"\x00" * 4 +
                struct.pack("<I", 0) +  # flags: Application pool, non-production
                struct.pack("<QQ", TITLE_ID, TITLE_ID) +
                struct.pack("<6I", fac_o, len(fac), sac_o2, len(sac), kac_o2, len(kac)) +
                b"\x00" * 8)
    assert len(acid_hdr) == 0x240, len(acid_hdr)
    acid = acid_hdr + fac + sac + kac

    hdr = bytearray()
    hdr += b"META" + b"\x00" * 8
    hdr += bytes([0x07,   # flags: bit0 = 64-bit, bits1-3 = 3 (39-bit address space)
                  0,      # reserved
                  0x2C,   # main thread priority (within 16-63)
                  0])     # main thread core
    hdr += b"\x00" * 4
    hdr += struct.pack("<III", 0, 0, 0x100000)  # system resource size, category, stack size
    hdr += b"SyntheticFixture".ljust(0x10, b"\x00")
    hdr += b"\x00" * 0x40
    hdr += struct.pack("<4I", aci_off, len(aci), acid_off, len(acid))
    assert len(hdr) == 0x80, len(hdr)
    return bytes(hdr) + aci + acid


def _romfs_hash(parent: int, name: bytes) -> int:
    h = parent ^ 123456789
    for c in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h


def make_romfs() -> bytes:
    empty = 0xFFFFFFFF
    name = ROMFS_FILE_NAME.encode()
    dir_hash = struct.pack("<I", 0)  # 1 bucket -> root dir at dir-meta offset 0
    root = struct.pack("<6I", 0, empty, empty, 0, _romfs_hash(0, b""), 0)  # child_file = offset 0
    file_hash = struct.pack("<I", 0)  # 1 bucket -> file at file-meta offset 0
    fent = struct.pack("<IIQQII", 0, empty, 0, len(ROMFS_FILE_DATA),
                       _romfs_hash(0, name), len(name))
    fent += name + b"\x00" * (-len(name) % 4)
    off = 0x50
    dh_o = off
    off += len(dir_hash)
    dm_o = off
    off += len(root)
    fh_o = off
    off += len(file_hash)
    fm_o = off
    off += len(fent)
    data_o = (off + 0x1FF) & ~0x1FF
    hdr = struct.pack("<Q8QQ", 0x50, dh_o, len(dir_hash), dm_o, len(root),
                      fh_o, len(file_hash), fm_o, len(fent), data_o)
    body = hdr + dir_hash + root + file_hash + fent
    body += b"\x00" * (data_o - len(body)) + ROMFS_FILE_DATA
    return body


def write_fixture(directory) -> dict:
    d = Path(directory)
    d.mkdir(parents=True, exist_ok=True)
    (d / "main").write_bytes(make_nso())
    (d / "main.npdm").write_bytes(make_npdm())
    (d / "romfs.bin").write_bytes(make_romfs())
    return {"title_id": TITLE_ID, "marker": MARKER, "files": list(FILES)}


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: make_fixture.py OUTDIR")
    info = write_fixture(sys.argv[1])
    for f in info["files"]:
        print(f, (Path(sys.argv[1]) / f).stat().st_size)
    print("title_id=0x%016X marker=%s" % (info["title_id"], info["marker"]))
