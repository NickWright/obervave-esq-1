# SQ1D

**An 8-voice synth for the M-VAVE FM-1 after a 1985 digital-analog hybrid: three wave oscillators a voice, a 4-pole
filter, four envelopes, three LFOs, programmed page by page as the original.**

The voice is a model of the original's voice hardware running its OS 3.5 control arithmetic. The waves and the 40
factory programs come from **your own ROM dumps**, sent to the FM-1's flash once; the firmware holds no ROM data and
plays generated stand-in waves without them.

> **Status: R2 (development).** It plays: the keys and MIDI; programs in INT, CART A, CART B (yours, in flash) and
> ROM (your bank's 40), shown as banks of ten; WRITE; the original's SysEx dumps in and out. The page-by-page
> editor comes next (R3).

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
python3 tools/esq_syx.py send bank.syx              # an all-40 dump: into INT;  a one-program dump: the edit buffer
python3 tools/esq_syx.py dump --set int -o int.syx  # a set from the FM-1 (int, carta, cartb, rom)
python3 tools/esq_syx.py dump --edit -o prog.syx    # the program playing
```

## Playing (R2)

| Control | Does |
| --- | --- |
| Keys | Play (F3 to G5), velocity 100 |
| PRESETS | Program: INT, CART A, CART B, ROM, 40 each; the screen shows its bank of ten |
| ALGORITHM | A bank of ten up or down |
| SAVE | WRITE: SAVE, pick the slot with PRESETS / ALGORITHM, SAVE again (any other button: cancel) |
| OCT- / OCT+ | Octave, -3..+3 (both: 0) |
| MIDI in (USB, TRS) | Notes, velocity, pitch bend, pressure, CC 1 wheel, 4 pedal, 7 volume, 11 XCTRL, 64 sustain, 120 / 123, program change |

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
