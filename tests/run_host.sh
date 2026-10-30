#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Run firmware/src/sq1d.c on this computer (tests/host.c): build/host/sq1d.wav and the screens as PNG.
#   tests/run_host.sh [script]       SQ1D_BANK=path/to/esq1.esqw: the owner's bank (else the placeholder waves)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -O2 -Wall -Wno-unused-function -DOBV_HOST -DAPP_VER="\"${APP_VER:-dev}\"" -Ifirmware/src -Ifirmware/gen -o build/host/sq1d firmware/src/sq1d.c
build/host/sq1d build/host/sq1d ${1:+"$1"}
python3 - <<'PY'
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/host").glob("sq1d-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
    print("screen ->", p.with_suffix(".png"))
PY
