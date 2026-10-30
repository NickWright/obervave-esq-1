#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The SysEx a wave-bank install sends (the editor protocol's 96-99, as tools/esq_syx.py waves sends it), into a .syx
file, for the host build's "sysex" script command (tests/sq1d_waves.txt). Not for the device: there,
tools/esq_syx.py waves BANK.esqw does it and waits for each reply.

  tools/sq_waves_syx.py BANK.esqw OUT.syx          the owner's bank (made by esq1rom.py from his ROM dumps)
  tools/sq_waves_syx.py --placeholder OUT.syx      the placeholder waves (firmware/src/esq/esq_phbank.h: no ROM data)
"""
import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

ED = bytes([0x7D, 0x46, 0x4C])


def pack7(b):
    out = []
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << j for j, x in enumerate(g)))
        out += [x & 127 for x in g]
    return bytes(out)


def msg(cmd, args=b""):
    return b"\xf0" + ED + bytes([cmd]) + bytes(args) + b"\xf7"


def placeholder():
    h = (Path(__file__).resolve().parent.parent / "firmware/src/esq/esq_phbank.h").read_text()
    body = h[h.index("{.b = {") + 7:h.index("}}")]
    return bytes(int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", body))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bank", nargs="?")
    ap.add_argument("out")
    ap.add_argument("--placeholder", action="store_true")
    a = ap.parse_args()
    img = placeholder() if a.placeholder else Path(a.bank).read_bytes()
    if img[:4] != b"ESQW":
        sys.exit("not an .esqw wave bank")
    if len(img) > 19 * 4096 - 256:
        sys.exit(f"{len(img)} bytes: more than the 77568 the FM-1 keeps")
    out = [msg(99), msg(96)]
    for k in range((len(img) + 255) // 256):
        off = 256 + 256 * k
        out.append(msg(97, [off & 127, (off >> 7) & 127, off >> 14] + list(pack7(img[256 * k:256 * k + 256]))))
    out.append(msg(98, pack7(struct.pack("<II", len(img), zlib.crc32(img) & 0xFFFFFFFF))))
    out.append(msg(99))
    Path(a.out).write_bytes(b"".join(out))
    print(f"{a.out}: {len(img)} B in {len(out)} messages")


if __name__ == "__main__":
    main()
