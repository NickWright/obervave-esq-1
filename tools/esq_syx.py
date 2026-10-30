#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""ESQ-1 SysEx program dumps and the wave bank for the FM-1's ESQ-1 engine (esq1-fm1 v6.5).

  esq_syx.py list FILE.syx ...                  the programs in the files (one-program and all-40 dumps)
  esq_syx.py send FILE.syx ... [--port NAME]    send them to the FM-1 over USB-MIDI (mido + python-rtmidi):
                                                  one program  -> the selected ESQ track (its edit buffer)
                                                  more         -> the USER bank, U01..U40, as one all-40 dump
                                                  --single N   -> only program N (1-based), as a one-program dump
                                                  --bank       -> an all-40 dump even for one program
  esq_syx.py bank FILE.syx ... -o OUT.syx       write the programs as one all-40 dump (for SysEx Librarian etc.)
  esq_syx.py waves BANK.esqw [--port NAME]      put your wave bank (build/esq1.esqw: install.sh makes it from your
                                                  ROMs) into the FM-1's flash, once; it stays through firmware updates
  esq_syx.py info [--port NAME]                 what the FM-1 holds: the wave bank, the USER slots used
  esq_syx.py receive -o OUT.syx [--port NAME]   wait for dumps from the FM-1 (SAVE > ESQ BANK: SEND / BANK) and keep
                                                  them in a .syx file (Ctrl-C or --timeout ends it)
A received all-40 dump is kept in the FM-1's flash (v6.5): the USER bank is there after a power cycle.

Format (Musician's Manual, Appendix 6; engine/esq_patch.h): F0 0F 02 0c 01 <204 nibbles> F7 for one program,
F0 0F 02 0c 02 <8160 nibbles> F7 for all 40; c the MIDI channel, each byte low nibble first. A bank with fewer than
40 programs is filled with INIT, named U01.. by its slot. Files may hold any number of messages; anything that is
not an ESQ-1 program dump is skipped. Programs past the 40th are left out (with a warning).
"""
import argparse
import os
import re
import sys
import time

PCB = 102
NBANK = 40
# engine/esq_patches.c INIT as a PCB program (esq_patch_to_pcb); its name is replaced by the slot's
INIT_PCB = bytes.fromhex(
    "494e495420200000000000000000000000000000000000000000000000000000000000007e7e7e0000001400000000c0c00000c0c00000c0"
    "c0002400ff000000feff00002400ff0000007eff00002400ff0000007eff00007e6400ff00000000000000008f00")
assert len(INIT_PCB) == PCB
PORT_RE = re.compile(r"fm-1|felucca|jiant|composite|sinco|usb-midi", re.I)   # as tools/fm1_install.py (never other gear; JIANT-ESQ: "JIANT")


def name_of(pcb):
    return "".join(chr(b) if 32 <= b < 127 else " " for b in pcb[:6]).rstrip() or "?"


def programs(data, where=""):
    """[(name, pcb, where)] from the bytes of a .syx file"""
    out, i = [], 0
    while True:
        i = data.find(b"\xf0", i)
        if i < 0:
            break
        end = data.find(b"\xf7", i + 1)
        if end < 0:
            break
        m = data[i:end]
        i = end + 1
        if len(m) < 5 or m[1] != 0x0F or m[2] != 0x02 or m[3] > 0x0F or m[4] not in (1, 2):
            continue
        n = 1 if m[4] == 1 else NBANK
        body = m[5:]
        if len(body) < 2 * PCB * n or any(b > 0x0F for b in body[:2 * PCB * n]):
            print(f"esq_syx: {where}: a broken ESQ-1 dump at byte {i - len(m) - 1} skipped", file=sys.stderr)
            continue
        for k in range(n):
            nib = body[2 * PCB * k:2 * PCB * (k + 1)]
            pcb = bytes((nib[2 * j] & 15) | (nib[2 * j + 1] & 15) << 4 for j in range(PCB))
            out.append((name_of(pcb), pcb, f"{where} #{len(out) + 1}"))
    return out


def read_all(paths):
    progs = []
    for p in paths:
        with open(p, "rb") as f:
            got = programs(f.read(), os.path.basename(p))
        if not got:
            print(f"esq_syx: {p}: no ESQ-1 program dump", file=sys.stderr)
        progs += got
    if not progs:
        sys.exit("esq_syx: no programs")
    return progs


def nibbles(pcb):
    return bytes(x for b in pcb for x in (b & 15, b >> 4))


def single(pcb, ch=0):
    return bytes([0xF0, 0x0F, 0x02, ch & 15, 0x01]) + nibbles(pcb) + b"\xf7"


def bank_pcbs(progs):
    if len(progs) > NBANK:
        print(f"esq_syx: {len(progs)} programs, the first {NBANK} kept", file=sys.stderr)
    pcbs = [p[1] for p in progs[:NBANK]]
    for k in range(len(pcbs), NBANK):
        pcbs.append(f"U{k + 1:02d}   ".encode()[:6] + INIT_PCB[6:])
    return pcbs


def bank(progs, ch=0):
    return bytes([0xF0, 0x0F, 0x02, ch & 15, 0x02]) + b"".join(nibbles(p) for p in bank_pcbs(progs)) + b"\xf7"


def cmd_list(a):
    for k, (name, _, where) in enumerate(read_all(a.files)):
        slot = f"U{k + 1:02d}" if k < NBANK else "  - "
        print(f"{k + 1:3d}  {slot}  {name:<6}  {where}")


def cmd_bank(a):
    with open(a.o, "wb") as f:
        f.write(bank(read_all(a.files)))
    print(f"esq_syx: {a.o} ({os.path.getsize(a.o)} B, one all-40 dump)")


def cmd_header(a):
    progs = read_all(a.files)[:NBANK]
    rows = ",\n".join("    {" + ", ".join(f"0x{b:02x}" for b in p[1]) + "}  /* " + f"U{k + 1:02d} {p[0]}" + " */"
                      for k, p in enumerate(progs))
    with open(a.o, "w") as f:
        f.write("/* made by tools/esq_syx.py from your .syx files: the USER bank's programs at power-on (never "
                "commit) */\n")
        f.write(f"#define ESQ_USERBANK_N {len(progs)}\n")
        f.write(f"static const uint8_t ESQ_USERBANK[{len(progs)}][{PCB}] = {{\n{rows}\n}};\n")
    print(f"esq_syx: {a.o}: {len(progs)} programs ({', '.join(p[0] for p in progs[:6])}"
          f"{', ...' if len(progs) > 6 else ''})")


def open_ports(a, need_in=False):
    try:
        import mido
    except ImportError:
        sys.exit("esq_syx: this needs mido and python-rtmidi (pip install mido python-rtmidi)")
    names = list(dict.fromkeys(mido.get_output_names()))
    want = [n for n in names if (a.port.lower() in n.lower() if a.port else PORT_RE.search(n))]
    if not want:
        sys.exit("esq_syx: no FM-1 MIDI port" + (f" matching '{a.port}'" if a.port else "") +
                 (": " + ", ".join(names) if names else " (is it connected? close other MIDI apps)"))
    ins = [n for n in dict.fromkeys(mido.get_input_names())
           if (a.port.lower() in n.lower() if a.port else PORT_RE.search(n))]
    if need_in and not ins:
        sys.exit("esq_syx: no FM-1 MIDI input port")
    return mido, want[0], (ins[0] if ins else None)


ED = [0x7D, 0x46, 0x4C]          # Felucca's editor protocol (web/EDITOR_PROTOCOL.md): F0 7D 46 4C cmd args F7


def pack7(b):
    out = []
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << j for j, x in enumerate(g)))
        out += [x & 127 for x in g]
    return out


class Editor:
    def __init__(self, mido, out_name, in_name):
        self.mido, self.out, self.inp = mido, mido.open_output(out_name), mido.open_input(in_name)
        time.sleep(0.3)   # (SQ1D) CoreMIDI may drop a message sent just after opening (as tools/fm1_install.py)

    def call(self, cmd, args=(), timeout=3.0, tries=1):
        for _ in range(tries):
            for m in self.inp.iter_pending():
                pass
            self.out.send(self.mido.Message("sysex", data=ED + [cmd] + list(args)))
            end = time.time() + timeout
            while time.time() < end:
                m = self.inp.poll()
                if m is None:
                    time.sleep(0.001)
                    continue
                d = list(m.data) if m.type == "sysex" else []
                if d[:4] == ED + [cmd]:
                    return d[4:]
        sys.exit(f"esq_syx: no answer from the FM-1 to command {cmd} (an ESQ firmware of v6.5 or later?)")

    def close(self):
        self.out.close()
        self.inp.close()


RC = {1: "refused (arguments, or not now)", 2: "flash erase failed", 3: "CRC / write failed", 4: "flash write failed",
      5: "not a wave bank (the placeholder plays)"}


def cmd_waves(a):
    import struct
    import zlib
    img = open(a.file, "rb").read()
    if img[:4] != b"ESQW":
        sys.exit(f"esq_syx: {a.file} is not an .esqw wave bank")
    if len(img) > 19 * 4096 - 256:
        sys.exit(f"esq_syx: {a.file}: {len(img)} bytes, more than the 77568 the FM-1 keeps for it")
    mido, o, i = open_ports(a, True)
    ed = Editor(mido, o, i)
    ed.call(99, timeout=1.0, tries=3)   # (SQ1D) the FM-1 answers first: a lost first message costs nothing
    r = ed.call(96)
    if r[:1] != [0]:
        sys.exit("esq_syx: BEGIN " + RC.get(r[0] if r else 1, "failed"))
    t0, n = time.time(), (len(img) + 255) // 256
    for k in range(n):
        off = 256 + 256 * k
        r = ed.call(97, [off & 127, (off >> 7) & 127, off >> 14] + pack7(img[256 * k:256 * k + 256]))
        if len(r) < 4 or r[3]:
            sys.exit(f"esq_syx: WRITE at {off}: " + RC.get(r[3] if len(r) > 3 else 1, "failed"))
        if k % 16 == 15 or k == n - 1:
            print(f"\resq_syx: {100 * (k + 1) // n:3d} %", end="", flush=True)
    r = ed.call(98, pack7(struct.pack("<II", len(img), zlib.crc32(img) & 0xFFFFFFFF)), timeout=10)
    print()
    if r[:1] != [0]:
        sys.exit("esq_syx: END " + RC.get(r[0] if r else 1, "failed"))
    r = ed.call(99)
    ed.close()
    print(f"esq_syx: wave bank in the FM-1's flash: {r[1]} waves, {r[2]} programs ({len(img)} B, "
          f"{time.time() - t0:.1f} s) via {o}")


def cmd_info(a):
    mido, o, i = open_ports(a, True)
    ed = Editor(mido, o, i)
    r = ed.call(99, timeout=1.0, tries=3)
    ed.close()
    if r[0]:
        print(f"wave bank: {r[1]} waves, {r[2]} programs, {r[3] | r[4] << 7 | r[5] << 14} B")
    else:
        print(f"wave bank: none in flash (the placeholder plays: {r[1]} waves)")
    print(f"USER bank: {r[6]} of 40 slots hold a program")


def cmd_receive(a):
    mido, o, i = open_ports(a, True)
    got, end = [], (time.time() + a.timeout if a.timeout else None)
    print(f"esq_syx: waiting for ESQ-1 dumps on {i} (SAVE > ESQ BANK: SEND or BANK, OCT+); Ctrl-C ends")
    with mido.open_input(i) as inp:
        try:
            while end is None or time.time() < end:
                m = inp.poll()
                if m is None:
                    time.sleep(0.005)
                    continue
                if m.type == "sysex" and list(m.data[:2]) == [0x0F, 0x02]:
                    raw = bytes([0xF0] + list(m.data) + [0xF7])
                    got.append(raw)
                    for name, _, _ in programs(raw, "FM-1"):
                        print(f"  {name}")
                    print(f"esq_syx: a {'one-program' if m.data[3] == 1 else 'all-40'} dump ({len(raw)} B)")
        except KeyboardInterrupt:
            pass
    if got:
        with open(a.o, "wb") as f:
            f.write(b"".join(got))
        print(f"esq_syx: {a.o}: {len(got)} dump(s)")
    else:
        print("esq_syx: nothing received")


def cmd_send(a):
    progs = read_all(a.files)
    if a.single is not None:
        if not 1 <= a.single <= len(progs):
            sys.exit(f"esq_syx: --single {a.single}: the files hold {len(progs)} programs")
        msgs, what = [single(progs[a.single - 1][1], a.channel)], f"program {progs[a.single - 1][0]}"
    elif len(progs) == 1 and not a.bank:
        msgs, what = [single(progs[0][1], a.channel)], f"program {progs[0][0]} (to the selected ESQ track)"
    else:
        msgs, what = [bank(progs, a.channel)], f"{min(len(progs), NBANK)} programs (to U01..U40)"
    mido, port, _ = open_ports(a)
    with mido.open_output(port) as out:
        for m in msgs:
            out.send(mido.Message("sysex", data=list(m[1:-1])))
            time.sleep(a.delay / 1000)
    print(f"esq_syx: sent {what} to {port}" + (" (kept in its flash once the transport stops)" if len(msgs[0]) > 300 else ""))


def main():
    ap = argparse.ArgumentParser(description="ESQ-1 SysEx program dumps for the FM-1's ESQ-1 engine")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("send")
    p.add_argument("files", nargs="+")
    p.add_argument("--port", help="a part of the MIDI port's name (default: the FM-1's)")
    p.add_argument("--single", type=int, metavar="N", help="send only program N (1-based) as a one-program dump")
    p.add_argument("--bank", action="store_true", help="an all-40 dump even for one program")
    p.add_argument("--channel", type=int, default=0, help="the channel nibble in the header (ignored by the FM-1)")
    p.add_argument("--delay", type=int, default=200, help="ms after each message (default 200)")
    p.set_defaults(fn=cmd_send)
    p = sub.add_parser("bank")
    p.add_argument("files", nargs="+")
    p.add_argument("-o", required=True)
    p.set_defaults(fn=cmd_bank)
    p = sub.add_parser("header")             # (up to v6.4: the compiled-in USER bank)
    p.add_argument("files", nargs="+")
    p.add_argument("-o", required=True)
    p.set_defaults(fn=cmd_header)
    p = sub.add_parser("waves")
    p.add_argument("file")
    p.add_argument("--port", help="a part of the MIDI port's name (default: the FM-1's)")
    p.set_defaults(fn=cmd_waves)
    p = sub.add_parser("info")
    p.add_argument("--port")
    p.set_defaults(fn=cmd_info)
    p = sub.add_parser("receive")
    p.add_argument("-o", required=True)
    p.add_argument("--port")
    p.add_argument("--timeout", type=float, default=0, help="seconds (default: until Ctrl-C)")
    p.set_defaults(fn=cmd_receive)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
