#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build an .esqw wave bank from the ESQ-1's own ROMs: the two wave ROMs and the OS 3.5 low ROM.

  tools/esq1rom.py --wavelo esq1wavlo.bin --wavehi esq1wavhi.bin --oslo 3p5lo.bin OUT.esqw [--header OUT.h]
  tools/esq1rom.py --roms roms/ OUT.esqw            (finds the three files by name in roms/)

What the OS holds (3p5lo.bin, banked into the 6809's 0x7000 window; found from the code at 0xDF20 of 3p5hi):
  bank 1, 0x1000  75 rows of 16 bytes: a row per wave number, a byte per pitch range (note + osc transpose) >> 3,
                  holding the index of a zone record
  bank 1, 0x14B0  150 zone records of 4 bytes: DOC page, DOC wavetable register (bit 6 bank, 5..3 size,
                  2..0 resolution), semitone offset (signed), fine offset (1/256 semitone)
  bank 6, 0x23B8  the 32 wave names, 6 characters each
The OS plays a zone with its resolution and the pitch note + transpose + semitone + fine/256; a 512-byte table at
resolution 1 and offset 0 holds one cycle (the SAW, 0xC400). So a zone holds 2^(res-1) * 2^(-(semi + fine/256)/12)
cycles of the note it plays.
Waves 0..31 are the ESQ-1's. 32..74 are the rows past them (the "hidden" waves, reachable on the hardware by
SysEx only), with 54.. marked one-shot as the OS marks them: named HID32..HID53 (they loop) and HIT54..HIT74 (they
play once); rows 39..41 have no zones of their own and play SAW, named -39..-41 and flagged as aliases (bit 2). The ESQ-1 leaves the DOC's bank-select pin unconnected,
so a zone that asks for bank 1 plays the same page of bank 0: the bank bit is ignored, as the hardware ignores it.
Wave n of the bank is row n, so a program's WAVE byte indexes the bank directly.
  bank 3, 0x3000  the 40 internal programs, 102 bytes each (esq_patch.h): stored as the bank's PTCH chunk

The ROMs are Ensoniq's: keep them, and the bank and header made from them, out of any public repository."""
import argparse
import hashlib
import os
import struct
import sys

ROW_BASE, NROWS = 0x1000, 75
REC_BASE, NRECS = 0x14B0, 150
NAMES = 0x63B8
PATCHES, NPATCHES, PCB_LEN = 0x3000, 40, 102
ONESHOT_FROM = 54
KNOWN = {  # sha1 of the dumps this was worked out from
    "esq1wavlo.bin": "867b51229b0a82c886bf3b216aa8893748236d8b",
    "esq1wavhi.bin": "ed0318e5253637585559e8cf24c06d6115bd18f6",
    "3p5lo.bin": "14d1150bccdbc15d90567cf1812aacdb3b6ee882",
}


def s8(v):
    return v - 256 if v > 127 else v


def read(path, name):
    data = open(path, "rb").read()
    if len(data) != 32768:
        sys.exit(f"esq1rom: {path}: {len(data)} bytes, want 32768")
    h = hashlib.sha1(data).hexdigest()
    if h != KNOWN[name]:
        print(f"esq1rom: warning: {path} is not the dump this was made from ({h})", file=sys.stderr)
    return data


def build(wave, os_lo, hidden=True):
    names = [os_lo[NAMES + 6 * k:NAMES + 6 * k + 6].decode("ascii", "replace").strip() for k in range(32)]
    if names[0] != "SAW" or names[31] != "OCT+5":
        sys.exit("esq1rom: the OS ROM has no wave names where 3.5 keeps them (not 3.5?)")
    recs = [os_lo[REC_BASE + 4 * k:REC_BASE + 4 * k + 4] for k in range(NRECS)]
    blob, waves = bytes(b if b else 1 for b in wave), []    # the whole ROM once; zones point into it

    def zone_of(ri):
        page, wsr, semi, fine = recs[ri]
        size, res = (wsr >> 3) & 7, wsr & 7            # bit 6 (bank) aliases to bank 0 on the ESQ-1
        start, length = page << 8, 256 << size
        if start + length > len(wave):
            return None
        cycles = 2.0 ** (res - 1) * 2.0 ** (-(s8(semi) + fine / 256.0) / 12.0)
        return {"log2len": 8 + size, "cycles": cycles, "offset": start}

    for row in range(NROWS if hidden else 32):
        idx = os_lo[ROW_BASE + 16 * row:ROW_BASE + 16 * row + 16]
        zones, ok = [], not any(i >= NRECS for i in idx)   # (rows 39..41 hold no zone records)
        for rng, ri in enumerate(idx if ok else []):   # runs of one record -> one zone
            z = zone_of(ri)
            if z is None:
                ok = False
                break
            top = min(127, rng * 8 + 7)
            if zones and zones[-1]["ri"] == ri:
                zones[-1]["top"] = top
            else:
                zones.append(dict(z, ri=ri, top=top, flags=1 if row >= ONESHOT_FROM else 0))
        if not ok:
            if row < 32:
                sys.exit(f"esq1rom: wave {row} reaches past the 64 KB wave ROM")
            zones = [dict(waves[0][2][0], top=127, flags=4)]   # keeps the numbering: plays SAW, marked an alias
        zones[-1]["top"] = 127
        # the hidden rows: HID32.. loop (multi-cycle and layered tables), HIT54.. play once (the sampled attacks and
        # hits), "-" the three rows with no zones of their own (SAW again: the pages skip them)
        hname = names[row] if row < 32 else ("-%02d" % row if not ok else
                                             ("HIT%d" if row >= ONESHOT_FROM else "HID%d") % row)
        waves.append((hname, row, zones))
    patches = os_lo[PATCHES:PATCHES + NPATCHES * PCB_LEN]
    return waves, blob, patches


def write_esqw(waves, blob, patches=b""):
    out = bytearray(b"ESQW" + bytes([2, len(waves)]) + b"\0\0" + struct.pack("<I", len(blob)))
    for name, _, zones in waves:
        out += name.encode("ascii")[:8].ljust(8, b"\0") + bytes([len(zones), 0, 0, 0])
        for z in zones:
            out += struct.pack("<BBBBII", z["top"], z["log2len"], z["flags"], 0, int(round(z["cycles"] * 65536)),
                               z["offset"])
    out += blob
    if patches:
        out += b"PTCH" + struct.pack("<I", len(patches)) + patches
    return bytes(out)


def write_header(path, image, waves):
    lines = ["/* ESQ-1 wave bank, made by tools/esq1rom.py from the ESQ-1 wave ROMs and OS 3.5 (Ensoniq's data:",
             " * local builds only, never committed or published). */",
             "#define ESQ_ROMBANK_WAVES %d" % len(waves),
             "#define ESQ_ROMBANK_PATCHES %d" % NPATCHES,
             "static const uint8_t ESQ_ROMBANK[%d] = {   /* (in flash: its tables hold no 0x00) */" % len(image)]
    for i in range(0, len(image), 24):
        lines.append("    " + ",".join(str(b) for b in image[i:i + 24]) + ",")
    lines.append("};")
    open(path, "w").write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--roms", help="a folder holding esq1wavlo.bin, esq1wavhi.bin and 3p5lo.bin")
    ap.add_argument("--wavelo")
    ap.add_argument("--wavehi")
    ap.add_argument("--oslo")
    ap.add_argument("--header", help="also write the bank as a C array (for the Felucca build)")
    ap.add_argument("--no-hidden", action="store_true", help="waves 0..31 only")
    ap.add_argument("--list", action="store_true", help="print every wave's zones")
    a = ap.parse_args()
    if a.roms:
        a.wavelo = a.wavelo or os.path.join(a.roms, "esq1wavlo.bin")
        a.wavehi = a.wavehi or os.path.join(a.roms, "esq1wavhi.bin")
        a.oslo = a.oslo or os.path.join(a.roms, "3p5lo.bin")
    if not (a.wavelo and a.wavehi and a.oslo):
        ap.error("give --roms or all of --wavelo --wavehi --oslo")
    wave = read(a.wavelo, "esq1wavlo.bin") + read(a.wavehi, "esq1wavhi.bin")
    waves, blob, patches = build(wave, read(a.oslo, "3p5lo.bin"), hidden=not a.no_hidden)
    image = write_esqw(waves, blob, patches)
    open(a.out, "wb").write(image)
    if a.header:
        write_header(a.header, image, waves)
    if a.list:
        for k in range(NPATCHES):
            print(f"P{k + 1:02d} {patches[k * PCB_LEN:k * PCB_LEN + 6].decode('ascii', 'replace')}")
        for k, (name, row, zones) in enumerate(waves):
            zs = " ".join(f"<={z['top']}:{256 << (z['log2len'] - 8)}/{z['cycles']:.3g}" for z in zones)
            print(f"{k:3d} {name:<7} row{row:3d} {zs}")
    print(f"{a.out}: {len(waves)} waves ({sum(1 for w in waves if w[1] >= 32)} hidden), "
          f"{len(blob)} bytes of tables, {len(patches) // PCB_LEN} programs, {len(image)} bytes")


if __name__ == "__main__":
    main()
