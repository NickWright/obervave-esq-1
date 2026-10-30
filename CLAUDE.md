# SQ1D

A firmware for the M-VAVE FM-1: ONE 8-voice synth with the ESQ-1's voice architecture (3 DOC-style wave
oscillators, 4-pole low-pass, DCA4, 4 envelopes, 3 LFOs), OS 3.5 control arithmetic, programmed page by page as the
ESQ-1, plus the user's list of enhancements. Standalone since 2026-10-30 (branch `sq1d`): nothing of OberVave's synth,
sequencer, mixer or effects is left; the FM-1 platform (Dinghy's, through OberVave) is. GPL-3.0. Repo:
NickWright/obervave-esq-1 (the name is historical). Branch `esq1` keeps the earlier OberVave-based build.

## Rules

- **No bulk ROM content in the repo or in any package:** ESQ-1 wave ROMs, the zone map, the OS's programs, OS images,
  dumps, disassembly listings. `.gitignore`: `waves/`, `private/`, `*.rom`, `*.esqw`. The firmware ships the generated
  placeholder waves (`esq/esq_phbank.h`, not ROM data); the owner's bank (`.esqw`, made locally by `tools/esq1rom.py`
  from the two wave ROMs and `3p5lo.bin`) goes to flash with `tools/esq_syx.py waves`.
  Allowed (the user, 2026-10-30): the engine's small control constants read from OS 3.5 (envelope / LFO rate knots,
  VCA / filter / DCA curves: `esq_ktab.h`, `esq_qtab.h`, constants in `esq_engine.c`).
- **No trademarks on screen:** never "Ensoniq" or "ESQ-1" on screen, in program names or in the release name; "inspired
  by" in docs only. The name is SQ1D (the user's choice, knowing "SQ-1" was also an Ensoniq model).
- Package identity `FM-1_97XY` (`FM-1_9700` a dev build). Before a release check the hub: Dinghy's list says 97N was
  zp12 (`FM-1_971`).
- FM-1 only (the platform keeps its SMK-37 paths, unused).
- Target: JieLi AC791N, no FPU, no libgcc: integers only, **no 64-bit division** (int64 multiply is fine), no constant
  in 0xFFC00000-0xFFD00000, no volatile pointer casts / register literals outside hal/ (`tools/build.py` checks).
  RAM <= 96 KB, big buffers in `.pool`. Built with -Os (OberVave found -O2 broke USB sends).
- Audio ISR: 256 frames per call at 44.1 kHz (5.8 ms). Stay under half of it; the screen's foot shows the render time.
- Style: C99, 4 spaces, short comments that say why, unity build. Commit after each tested step, push, host test before
  every device build (`tests/run_host.sh`, look at `build/host/*`).

## The plan (agreed 2026-10-30)

| | Work |
| --- | --- |
| R1 (done) | Standalone app: whole-synth engine, keys / MIDI, wave bank, program select of the bank's 40 as banks of ten |
| R2 (done) | Program memory: ROM 40 (read only), INT 40, CART A, CART B (40 each, flash); WRITE; ESQ-1 SysEx single / bank dumps in and out, requests |
| R3 | The page editor (below), VFD look, cursor + data entry, COMPARE, undo, INIT, copy OSC / ENV / LFO |
| R4 | MODERN look (the same fields + envelope / LFO / wave graphics), system pages MASTER, MIDI, CONTROL, STORAGE |
| R5 | Enhancements, each in the engine + the program's extension block + page fields: AM + SYNC together, Q as a mod destination, LFO shapes (sine, ramps, S&H, smooth random, drunk), LFO tempo sync and one-shot, looping envelopes, wave scanning, microtuning, unison, DRIFT, more velocity destinations, sources random / round-robin / key zones, SPLIT / LAYER (two programs), arpeggiator, latch, chord memory, CC / NRPN for every parameter, velocity / pressure curves, MUTATE / RANDOM |
| R6 | CPU on the device, factory programs, 1.0 |

Not planned before 1.0: the ESQ-1's sequencer, effects.

### Controls (agreed)

- Page buttons: EDIT taps OSC 1 > 2 > 3; FX taps DCA 1 > 2 > 3 > DCA 4; ENV taps ENV 1 > 4; LFO taps LFO 1 > 3; SEQ taps
  FILTER > MODES > SPLIT/LAYER; EDIT held + a key = any page in the ESQ-1's page-button order (OSC 1-3, DCA 1-3,
  FILTER, DCA 4, ENV 1-4, LFO 1-3, MODES, SPLIT/LAYER); GLO taps MASTER, MIDI, CONTROL, STORAGE.
- SELECT turns the cursor over the page's fields (the ESQ-1's soft buttons); KNOB 1-4 edit the four fields of the
  cursor's row; ALGORITHM is the data slider on the cursor's field; SEL pressed = COMPARE.
- HOME = the program select page (a bank of ten); PRESETS = program from any page; SAVE = WRITE (SAVE, PRESETS picks
  the slot, SAVE). ARP = the arp page, PLAY = latch, REC = chord memory. OCT- / OCT+ octave.
- Pages keep the ESQ-1's two rows of fields; each field drawn as a large label + value (a literal 2 x 40 line is
  unreadable on 240 px).

## Where things are

- `firmware/src/sq1d.c`: the app (keys, MIDI, panel, the main loop). Includes `platform.c` first, then
  `sq_engine.h` (the engine as one instrument: `esq_synth_t sq`, the event ring main loop -> ISR, `sq_render`,
  `sq_set_bank`, the programs: the bank's PTCH 40 else the 7 demos), `sq_waves.h` (the bank in flash and its SysEx
  install), `sq_ui.h` (the screen: VFD cyan, header / program / bank of ten / voice meters).
- `firmware/src/esq/`: the user's ESQ-1 engine (esq1-fm1, v6.6), integer build (`esq/esq_sq1d.h`: ESQ_FIXED,
  ESQ_HOST 0, ESQ_NVOICE 8 = the whole-synth API, the engine's own voice allocation; zones and knot curves read in
  place). `esq_render_q` output full scale 1 << 24, the app takes >> 9 to Q15.
- `tests/host.c` + `tests/run_host.sh [script]`: the app on the PC (WAV + screen PNGs, scripted keys / buttons /
  encoders / MIDI / `sysex FILE`). `SQ1D_BANK=path` loads the owner's bank on the host. `tests/run_tests.sh`: engine
  tests, the host app, the bank install; the update path when `build/sq1d.fwsc` exists. Scripts: `tests/sq1d_r1.txt`,
  `tests/sq1d_waves.txt`.
- `tools/build.py` / `./build.sh` -> `build/sq1d.fwsc`; CI (`.github/workflows/release.yml`, workflow_dispatch)
  builds, tests and uploads the artifact `sq1d-fwsc`.

## Program memory (R2, 2026-10-30)

- `firmware/src/sq_prog.h`: four sets of 40 (0 INT, 1 CART A, 2 CART B: RAM copies in `.pool`, flash via platform.c
  `bank_load / bank_save` at `BANKS_AT` 0xD3000, 2 sectors a set (the second kept for R5's extension block); 3 ROM:
  the bank's PTCH 40, else the 7 demos then INIT). A program number is set * 40 + slot; on screen "INT BANK 2 PROG 3".
  A set never stored shows defaults (INT the ROM's programs, the carts INIT) and follows a new wave bank (`banks_follow`).
- Programs are kept as their raw 102 bytes everywhere (`prog_pcb`, `sq_edit_pcb`): the engine's decode drops a few bits
  the ESQ-1 keeps, so nothing is re-encoded (the ROM set's dump is byte-identical to the bank's PTCH, checked). R3's
  editor must merge its changes into the raw bytes (keep the bits `esq_patch_to_pcb(from_pcb(x))` loses).
- The edit buffer `sq_edit` + `sq_edit_pcb`, `sq_edited`; `prog_load`, `edit_apply` (EV_PROG with `sq_pend`).
  WRITE (`prog_write`): SAVE, PRESETS / ALGORITHM pick a writable slot, SAVE stores at once (~50 ms audio stall).
- SysEx in (`sq_sx_byte`, called for every SysEx byte by usb.c's `sysex_byte` through `APP_SYSEX_BYTE`, defined in
  sq1d.c; host: the "sysex" script command too): one program -> the edit buffer, EDITED; all 40 -> INT, stored.
  Out on request (editor cmds 100 set, 101 edit buffer: `sq_ed_more`, sent by `ota_wire_send`):
  `tools/esq_syx.py dump --set int|carta|cartb|rom -o F.syx`, `--edit`. The ESQ-1's own request messages: not
  implemented (their codes unverified).
- Settings (`sq1d.c`): the program and the octave, saved 3 s after a change once no voice sounds.
- Panel now: PRESETS program (all 160, wrapping), ALGORITHM a bank of ten, SAVE = WRITE, OCT- / OCT+.
- Host: `tests/sq1d_r2.txt` (WRITE, the dumps into build/host/sq1d-out.syx), `tests/sq1d_req_*.syx`.

## The wave bank in flash (Phase 2, 2026-10-30)

- `firmware/src/sq_waves.h`: the owner's `.esqw` in the FM-1's flash, `WAVES_AT` 0xC0000, 19 sectors (platform.c
  `wv_*`, added to `FL_RANGE_OK`; FL_DATA after the pattern slots). 16-byte header (magic "EQWB", len, CRC32, ver 1)
  at 0, the image from 256, read in place through XIP (`wv_map` = `fl_plain_window_init`, called once in
  `sq_waves_boot` after `engine_init`; OberVave never mapped it before). The header is written last; boot takes the
  bank only if its CRC holds. Firmware updates leave it alone (the loader writes the app area only).
- Install: the Felucca / JIANT ESQ editor protocol (F0 7D 46 4C cmd .. F7, cmds 96 BEGIN, 97 WRITE, 98 END, 99 INFO),
  so `tools/esq_syx.py waves BANK.esqw` (copied from esq1-fm1; it waits 0.3 s after opening the port and retries INFO:
  CoreMIDI drops a message sent right after opening) works unchanged. `sq_ed_service` runs in
  platform_poll before `ota_service` and takes only editor frames from usb.c's SysEx frame. `sq_set_bank` swaps the
  bank with the interrupts off, stops every voice and takes the program again.
- The bank: `tools/esq1rom.py --wavelo LOW --wavehi HIGH --oslo 3p5lo.bin OUT.esqw` on the owner's computer.
- Host: "sysex FILE" in a script feeds F0..F7 messages to the editor (replies printed); `SQ1D_FLASH=path` keeps the
  store in a file between runs; `tools/sq_waves_syx.py` writes an install as a .syx (`--placeholder`: the generated
  waves, for `tests/sq1d_waves.txt` in run_tests.sh). Checked on the host with the owner's bank: 75 waves, 40
  programs, kept through a restart, a corrupted transfer refused (rc 3) and not taken.
- SMK-37: no store (stubs, the placeholder plays).

## The platform (from OberVave's notes, still true)

- Installing: `python3 tools/fm1_install.py build/sq1d.fwsc --yes` (USB-MIDI, ~3 min, `--info` shows the identity).
- The way back if an install leaves the FM-1 unreachable (OberVave, 2026-10-13): the UBOOT key (F0 22 24 35 7D F7)
  -> the mask ROM's USB mass storage -> jl-uboot-tool's loader -> compare and rewrite the app area 0x4000..0x93000.
  The Firmware Hub's Rescue page and OCT- held at power-on are the first things to try.
- OberVave's history and its own notes are in the `esq1` branch's CLAUDE.md and upstream zvenson/obervave.
