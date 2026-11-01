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
| R3 (done) | The page editor (below), VFD look, cursor + data entry, COMPARE, undo, INIT, copy OSC / ENV / LFO |
| R4 (built) | CPU (the engine at -O2, its sample loop in RAM); the button layout below; PERFORM; system pages MASTER, MIDI, STORAGE; IMPORT asks; arpeggiator, latch, chord memory; a sequencer (8 patterns) |
| R4.1 (built) | COMPARE as the original (C*, the edit buffer kept); screens of four fields; knob acceleration; CPU meter split, waves in RAM, USB AUDIO OFF; OUT ORIG / CLEAN; SysEx dumps survive other senders |
| R5 (built) | MODERN look (graphs); the extension block: LFO shapes, sync, one-shot, looping envelopes, Q mod, wave scan, unison, DRIFT, MOD+ (sources RAND / ALT / RR4 / ZONE to pitch, cutoff, Q, level, pan, LFO1 rate, scan), TUNING, SPLIT / LAYER played, NRPN / CC, MUTATE / RANDOM |
| R6 (built) | CPU: measured on the device 180 us + ~380 us a voice (the engine's own cost, as Felucca's), lossless trims, MASTER VOICES 6 / 7 / 8; SQ1D's own factory programs (CART B); 1.0 waits for device testing and the hub's identity check (1.0 = FM-1_9710) |

Not planned before 1.0: the ESQ-1's sequencer, effects.

### Controls (agreed)

- Page buttons (R4): EDIT taps OSC 1 > 2 > 3; FX taps DCA 1 > 2 > 3 > FILTER > DCA 4; ENV taps ENV 1 > 4; LFO taps
  LFO 1 > 3; GLO taps MODES > SPLIT/LAYER (on a system page: MASTER > MIDI > STORAGE), GLO held 0.6 s = MASTER; SEQ =
  the sequencer page; HOME taps PROGRAM > PERFORM; ARP tap = the ARP page (there: ARP on / off), ARP held alone 0.6 s =
  LATCH on / off, ARP held + keys = chord memory (one key: off); PLAY = sequencer play / stop; REC = record (step entry
  when stopped, real time when playing). EDIT held + a key = any page: the ESQ-1's page-button order (OSC 1-3, DCA 1-3,
  FILTER, DCA 4, ENV 1-4, LFO 1-3, MODES, SPLIT/LAYER), then PERFORM, ARP, SEQ, MASTER, MIDI, STORAGE.
- (R4.1) Pages are screens of at most four fields, one over each knob (`SCR[]` in sq_edit.h); SELECT steps the
  screens (on the program's pages on through OSC 1 .. MOD+); KNOB 1-4 edit the screen's fields; ALGORITHM the last
  turned (lit). Fast turns step 2 / 4 / 8 (fields of 32 values or more). SEL pressed = COMPARE (step entry: a rest;
  with keys down: the chord one step longer). GLO taps MODES > SPLIT/LAYER > MOD+ (system: MASTER > MIDI > STORAGE >
  TUNING). EDIT held + ENV / LFO = MUTATE / RANDOM.
- HOME = the program select page (a bank of ten); PRESETS = program from any page; SAVE = WRITE (SAVE, PRESETS picks
  the slot, OCT+ yes, OCT- no); on SEQ SAVE stores the pattern, on STORAGE it sends the dump OUT names. EDIT held +
  SEL / HOME / SAVE = UNDO / INIT / COPY (on SEQ: -, CLEAR the pattern, COPY it into the next). OCT- / OCT+ octave.
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

## The voice pages (R3, 2026-10-30)

- `firmware/src/sq_edit.h`: the pages as tables (`PAGES[]`, `fld_t`: label, full name, kind, range, offset in the
  slot's struct of `sq_edit`); OSC (OCT SEMI FINE WAVE / MOD#1 DEPTH MOD#2 DEPTH), DCA (LEVEL OUTPUT / 2 mods),
  FILTER (FREQ Q KEYBD / 2 mods), DCA 4 (LEVEL PAN / MOD DEPTH), ENV (L1 L2 L3 LV T1V / T1 T2 T3 T4 TK), LFO (FREQ
  RESET HUMAN WAV / L1 DELAY L2 MOD), MODES (SYNC AM MONO GLIDE VC / ENV OSC CYC), SPLIT/LAYER (stored and edited;
  the engine plays one program until R5). WAVE runs over all the bank's waves, the hidden ones too (the alias rows
  39-41 stepped over). `edit_commit` re-encodes and keeps the bits the decoding loses (`sq_lost`, from
  `ed_track_lost` at every load); COMPARE (`ed_compare`), UNDO / REDO (`ed_undo`, a gesture = one field, < 1.5 s
  pauses), INIT (`ed_init`), COPY (`ed_copy`: this OSC / DCA / ENV / LFO into the next).
- `firmware/src/sq_ui_page.h`: header (page, program name, * EDITED / COMPARE), the fields on a grid of five (rows as
  the ESQ-1's), the cursor's field large with its full name and a range bar; messages in that area for 2 s.
- Panel (sq1d.c `panel_in`, `page_button`, `keys_in`): as agreed; EDIT is a tap on its release unless it was used as
  a modifier (`edit_used`); keys pressed with EDIT held choose a page and play nothing (`KEY_NONE`). Page LEDs lit.
- Host: `tests/sq1d_r3.txt`.

## R4 (2026-10-30)

- CPU: `tools/build.py` compiles `esq/esq_engine.c` as its own object at -O2 (`-DSQ1D_ESQ_SPLIT=1`, `-include
  esq/esq_sq1d.h`: ESQ_SPLIT, ESQ_RAMFN = section .ram_text.esq, the per-sample loop's RAM copy, build.py refuses
  calls in it); sq1d.c stays -Os. `SQ1D_ESQ_SPLIT=0` / `SQ1D_ESQ_RAM=0` at build go back. The host keeps the unity
  build; split and RAM copy checked bit-identical on the host (`-DSQ1D_ESQ_RAM_TEST`).
- `sq_sys.h`: the settings record `sq_sys_t` (44 B, magic "1D1R"; R3's record carried over once): MASTER (TUNE, BEND,
  VOL, KEY VEL, velocity / pressure curves), MIDI (channel, program change in, SEND notes out, CLOCK INT / MIDI, clock
  out), STORAGE (IMPORT: ASK / INT / CART A / CART B; OUT: the dump SAVE sends), ARP, chord memory, tempo, swing,
  pattern, KEYS. `sys_apply` hands TUNE / BEND / VOL to the engine (EV_SET, CC 7).
- `sq_engine.h`: a second, timed event ring (sq_tq, sample times in `sq_clock`); the render runs in 32-sample chunks
  and takes due events before each, so scheduled notes land within 32 samples; `sq_tdrop_all` drops the not-yet-due.
- `sq_play.h`: the playing layer, every key / MIDI note through `pl_input`: KEYS TRNSP (transpose while playing),
  chord memory, recording, arpeggiator / latch / direct. The scheduler (`pl_poll`, main loop) runs 1024 samples ahead
  at 24 PPQN (tempo, or MIDI clock F8 when CLOCK MIDI; FA / FB / FC transport), keeps pending notes in time order
  (`pend`), hands them to the timed ring; SEND and clock out are sent from `po` when due. Patterns `sq_pat[8]` (.pool,
  520 B each: len, rate, 64 steps of 4 notes + velocity + gate in quarter steps, > 4 a tie) in the blob sectors
  (platform.c `blob_load / blob_save`, 0xF4000); SAVE on SEQ stores the current one.
- `sq_edit.h`: pages PERFORM (program fields), ARP, SEQ (through `seq_view`), MASTER, MIDI, STORAGE (`PG_SYS`: no undo,
  no COMPARE); field kinds ENUM / NOTE / Q4 / PCT / CHAN / STEP; labels and values at most 5 characters (the grid).
- IMPORT (`sq_prog.h`): an all-40 dump goes where IMPORT says or (ASK) waits on a prompt (PRESETS / ALGORITHM the set,
  OCT+ / OCT-); editor command 102 [set] sends the next one straight there (`esq_syx.py send --to carta`).
- Host: `tests/sq1d_r4.txt`, `sq1d_r4b.txt`, `sq1d_r4clk.txt` (MIDI clock); the 102 check in run_tests.sh.

## R4.1 and R5 (2026-10-31)

- COMPARE (sq_edit.h `ed_compare`, `ed_keep_*`): C* on every page once edited; the edit buffer kept over a program
  change (`ed_keep_aside` from `prog_load`) and recalled by COMPARE on a program not edited; WRITE / a new edit drop it.
- CPU: the wave bank is copied into the pool (`sq_wram`, sq_engine.h `sq_bank_ram`) and played from RAM; the meter
  (`app_audio`) shows the render and, apart, TIMER5's time inside it (platform.c `t5_nested`), the most of each over
  64 renders. MASTER USB OFF (`uac_off`: no ring, resampler or packets). MASTER OUT CLEAN (`esq_synth_t.out_clean`).
- SysEx in (sq_prog.h): another sender's F0..F7 inside a dump is stepped over (Felucca-ESQ v6.5.2); a broken dump
  shows LENGTH n/8160, BAD BYTE AT n or CUT AT n. The owner's cleanBanks2026.syx imports on the host.
- The extension block (`esq_ext_t`, esq/esq_types.h, 32 bytes, `esq_patch_t.x`; zero = the original, bit-identical
  on the host): per program in `sq_ext[set][slot]`, the set's second sector (`bank_save(set + NBANK)`); ROM's zero; a
  received dump's zero. The edit buffer's is `sq_edit.x` (`edit_decode` after every raw decode). Engine: LFO shapes /
  SYNC (`esq_synth_t.tempo`) / one-shot (`esq_lfo_tick`), ENV loop, Q mod, wave scan, unison (`esq_note_on`,
  `esq_voice_pick_uni`), DRIFT (`card_p / card_f / wander`), sources RAND / ALT / RR4 / ZONE (`esq_xsrc`), two MOD+
  routes, `micro[12]`; R5's randomness has its own generator (`xr`).
- Fields of the block: `fld_t.x` (FX_XS one byte, FX_XA the slot's in an array, FX_XB the slot's bit); pages OSC
  (SCAN), FILTER (QMOD), ENV (LOOP), LFO (SHAPE SYNC 1SHOT), MODES (UNI DETUN DRIFT), MOD+ (PG_XMOD), TUNING (PG_TUNE).
- SPLIT/LAYER (sq_engine.h): `sq`, `sq_lay`, `sq_spl`; `sq_route` with the programs (`edit_apply`: PRG 0..119 =
  INT, CART A, CART B); `sq_cap` keeps eight voices among them. S/L is not played.
- NRPN 99 / 98 / 6 (sq1d.c `cc_param`: MSB the page 1..18, LSB the field, value from the field's lowest); CC 74 71
  73 72 5. MUTATE / RANDOM `ed_shake`. MODERN: `ui_set_look` palette, graphs in `sq_ui_page.h` (ENV, LFO, OSC).
- Settings 48 bytes ("1D1S"; R4's 44 carried over): + scale, skey, look.
- Host: `tests/sq1d_r41.txt`, `tests/sq1d_r5.txt`.

## R6 (2026-11-01)

- CPU on the device (the owner, R4.1): 0 keys 180 + 13 us, 1 key 575 + 36, 4 keys 1696 + 107, 8 keys 3200 + 200: about
  380 us a voice a 256-frame render (Felucca's 169 us is per 128 frames: the same engine cost; Felucca capped its ESQ
  voices at 6). Lossless trims in the per-sample loop (oscillator state in locals, settled VCA / pan smoothing
  skipped: bit-identical on the host). MASTER VOICES 6 / 7 / 8 (`esq_synth_t.nvoice`, `esq_nv`; `sq_vmax` for all three
  synths; settings byte `voices`, 0 = 8).
- `sq_factory.h`: SQ1D's own programs (23: SQ1D, UNISAW, SCANWV, DRUNKF, S&HBLP, LOOPEV, RRPLUK, RNDPAN, ZONEKB, QSWEEP,
  AMSYNC, ZAPPER, VELBRS, TAPEKY, CHORGN, STRNGS, VOXSCN, SUBBAS, ALTBEL, WINDS, GATSEQ, BENDLD, WIDEPD), CART B's
  defaults (`set_defaults(2)`); STORAGE FACT (SAVE twice within 3 s) writes them into CART B (`factory_restore`).
- Host: `tests/sq1d_r6.txt` (each program's level measured).

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
