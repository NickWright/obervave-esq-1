# SQ1D

**An 8-voice synth for the M-VAVE FM-1 after a 1985 digital-analog hybrid: three wave oscillators a voice, a 4-pole
filter, four envelopes, three LFOs, programmed page by page as the original.**

The voice is a model of the original's voice hardware running its OS 3.5 control arithmetic. The waves and the 40
factory programs come from **your own ROM dumps**, sent to the FM-1's flash once; the firmware holds no ROM data and
plays generated stand-in waves without them.

> **Status: R6 (release candidate).** Programs in INT, CART A, CART B and ROM; every voice page of the original, edited as
> on it, plus extension fields (LFO shapes and tempo sync, looping envelopes, Q modulation, wave scanning, unison,
> drift, MOD+, tuning); SPLIT / LAYER; COMPARE as the original; an arpeggiator, latch, chord memory and an 8-pattern
> step sequencer; system pages; VFD or MODERN look; NRPN for every program field; 23 factory programs of its own
> in CART B; MASTER VOICES 6 / 7 / 8.

## Install

```sh
python3 -m venv ~/.venvs/fm1 && source ~/.venvs/fm1/bin/activate
pip install mido python-rtmidi
python3 tools/fm1_install.py sq1d.fwsc --yes      # the package: this repo's Actions, artifact sq1d-fwsc
```

## Your wave bank

```sh
python3 tools/esq1rom.py --wavelo LOW.bin --wavehi HIGH.bin --oslo 3p5lo.bin esq1.esqw   # from your dumps
python3 tools/esq_syx.py waves esq1.esqw                                                 # into the FM-1, once
python3 tools/esq_syx.py info                                                            # 75 waves, 40 programs
```

The bank stays in its own flash area through firmware updates. Keep it, and your dumps, out of any repository.

## Programs over SysEx

```sh
python3 tools/esq_syx.py send bank.syx              # an all-40 dump: the FM-1 asks where; a one-program dump: the edit buffer
python3 tools/esq_syx.py send bank.syx --to carta   # an all-40 dump straight into CART A (int, carta, cartb)
python3 tools/esq_syx.py dump --set int -o int.syx  # a set from the FM-1 (int, carta, cartb, rom)
python3 tools/esq_syx.py dump --edit -o prog.syx    # the program playing
```

## Playing and programming (R4)

| Control | Does |
| --- | --- |
| Keys | Play (F3 to G5) at KEY VEL (MASTER) |
| PRESETS | Program: INT, CART A, CART B, ROM, 40 each (any page) |
| HOME | The program page (ALGORITHM a bank of ten), again: PERFORM (cutoff, Q, attack, release, LFO 1, glide, level, pan) |
| EDIT | OSC 1 > 2 > 3 |
| FX | DCA 1 > 2 > 3 > FILTER > DCA 4 |
| ENV / LFO | ENV 1 > 4 / LFO 1 > 3 |
| GLO | MODES > SPLIT/LAYER > MOD+; held: MASTER, then GLO: MIDI > STORAGE > TUNING > MASTER |
| ARP | The ARP page; there: arpeggiator on / off. Held: LATCH on / off. Held + a chord: chord memory (+ one key: off) |
| SEQ, PLAY, REC | The sequencer page; play / stop; record (stopped: step entry, SEL a rest; playing: real time, overdub) |
| EDIT held + a key | Any page: OSC 1-3, DCA 1-3, FILTER, DCA 4, ENV 1-4, LFO 1-3, MODES, SPLIT/LAYER, PERFORM, ARP, SEQ, MASTER, MIDI, STORAGE |
| SELECT / KNOB 1-4 / ALGORITHM | The screens (at most four fields each) / the field over each knob / the field last turned |
| EDIT held + ENV / LFO | MUTATE / RANDOM (undoable) |
| SEL (press) | COMPARE: the program as stored, against your edits |
| EDIT held + SEL / HOME / SAVE | UNDO (again: REDO) / INIT / COPY into the next OSC, DCA, ENV, LFO (on SEQ: clear / copy the pattern) |
| SAVE | WRITE: PRESETS / ALGORITHM pick the slot, OCT+ yes, OCT- no. On SEQ: store the pattern; on STORAGE: send the dump |
| OCT- / OCT+ | Octave, -3..+3 (both: 0) |
| MIDI in (USB, TRS) | Notes, velocity, pitch bend, pressure, CC 1 wheel, 4 pedal, 7 volume, 11 XCTRL, 64 sustain, 120 / 123, program change, clock and transport (CLOCK MIDI) |

## Build

As Dinghy: Linux x86-64 (or Docker), Python 3 with Pillow, the JieLi toolchain (`tools/get_toolchain.sh`), the AC79
SDK. The GitHub workflow builds and tests on every run.

```sh
tests/run_host.sh [script]      # on your computer: build/host/sq1d.wav and the screens (SQ1D_BANK=bank.esqw)
sh tests/run_tests.sh           # the engine's tests, the app on the host, the bank install
./build.sh                      # build/sq1d.fwsc
```

## Credits and licence

GPL-3.0-only, see `LICENSE` and `LICENSING.md`. SQ1D stands on [OberVave](https://github.com/zvenson/obervave) and
[Dinghy](https://github.com/zvenson/dinghy) (zvenson), which carry the FM-1 platform of
[Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (Hügelton Instruments) with the changes of
[SLOOP](https://github.com/isod89/sloop-fm1) (isod89).

Inspired by the Ensoniq ESQ-1; "Ensoniq" and "ESQ-1" are trademarks of their owner. SQ1D is not made or endorsed by
them. "M-VAVE" and "FM-1" belong to their owners.
