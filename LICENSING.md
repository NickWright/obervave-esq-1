# Licensing

SQ1D is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `midi_uart.c` (the TRS MIDI IN; its fix from Felucca Salt by ChanceTheMaker, found by keremimo), `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The ESQ-1 voice engine: `firmware/src/esq/`, `tests/esq_engine_test.c`; the bank tools `tools/esq1rom.py`, `tools/esq_syx.py` | Copyright (C) 2026 Nick Wright (esq1-fm1); GPL-3.0-only. Its placeholder waves are generated, not ROM data |
| SQ1D, the app: `firmware/src/sq1d.c`, `sq_engine.h`, `sq_waves.h`, `sq_ui.h`, `tools/sq_waves_syx.py`, `tests/sq1d_*.txt` | Copyright (C) 2026 Nick Wright |
| The platform's wiring `platform.c`, the host mode `tests/`, the docs, the build (`build.sh`, `tools/build.py`) | zvenson, as [Dinghy](https://github.com/zvenson/dinghy) |
| The update loader's screen: `firmware/loader/ldr_screen.c`, `tests/ldr_screen_test.c` | Copyright (C) 2026 zvenson (the Firmware Hub's look); the idea, a loader that draws, its 5 x 7 letters and the order of things from [Skiff](https://github.com/illuminus5/skiff) by illuminus5, GPL-3.0 |
| The base SQ1D grew from: [OberVave](https://github.com/zvenson/obervave) (its platform wiring, the wave store hooks in `platform.c`, the host harness) | Copyright (C) 2026 zvenson |
| The SMK-37 board layer: `firmware/hal/smk_input.h`, `fm1_flash_map.h`, the SMK-37 parts of `fm1_cc.h`, `fm1_flash.h`, `fm1_lcd_hw.h`, `lcd.c`, `ota.c`, `bootguard.h`, `firmware/loader/`, `tools/fm1pkg_make.py`, `tools/fm1_install.py`, `tools/build.py`; `firmware/src/smk_wheel.h`; `tests/ota_test.c`, `ldr_test.c`, `install_test.py`, `smk_wheel_test.c`, `smk_pkg_test.py` | Copyright (C) 2026 M. Geisler (genkigenki/makkuro), from [genkigenki/sloop-fm1](https://github.com/genkigenki/sloop-fm1) branch `smk37` (the reference port of the [fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) kit), on the platform above |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE", "FM-1", "SMK-37", "MKE-P37" and
"Starrykey" belong to their owners. SQ1D is inspired by a 1985 digital-analog hybrid synthesizer; "Ensoniq" and "ESQ-1" are
trademarks of their owner. SQ1D is not made or endorsed by any of them, holds none of their ROM data, and does not
show those marks on screen.
