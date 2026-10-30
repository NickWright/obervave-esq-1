#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# SQ1D's host tests (no hardware). Run from the repo root; the update-path tests need a device build first (sh build.sh).
#   sh tests/run_tests.sh
set -e
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"   # (fm1pkg_make.py: the SDK files of a package)
cd "$(dirname "$0")/.."
OUT=build/host
mkdir -p "$OUT"
CC="${CC:-cc} -O1 -Wall -Wno-unused-function"
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }

# the ESQ-1 engine's own tests (pitch, DOC, envelopes, LFOs, CVs, voices, SYNC, AM, filter, programs, glide): the float
# model and the integer build the firmware runs, the integer one checked against the float one
for q in 0 1; do
    cc -O2 -std=c99 -Wall -Wno-unused-function -D_DEFAULT_SOURCE -DESQ_FIXED=$q -o "$OUT/esq_engine_test$q" tests/esq_engine_test.c -lm \
        && run "ESQ-1 engine (ESQ_FIXED=$q)" sh -c "cd tests && ../$OUT/esq_engine_test$q"
done

run "the app on the host: the default script plays, screens and sound" sh tests/run_host.sh
run "R1: programs, keys, octave (tests/sq1d_r1.txt): sound comes out" sh -c \
    "sh tests/run_host.sh tests/sq1d_r1.txt >/dev/null && python3 -c \"
import wave, array; w = wave.open('$OUT/sq1d.wav'); a = array.array('h'); a.frombytes(w.readframes(w.getnframes()))
assert max(abs(x) for x in a) > 1000, 'silent'\""

# the wave bank's install over SysEx (sq_waves.h) with the placeholder image: taken, CRC checked
python3 tools/sq_waves_syx.py --placeholder "$OUT/sq_ph.syx" >/dev/null \
    && run "wave bank install (SysEx 96-99, the placeholder image)" sh -c \
       "sh tests/run_host.sh tests/sq1d_waves.txt | grep -q 'sysex reply: F0 7D 46 4C 63 01'"

if [ -f build/sq1d.fwsc ]; then                  # the update path (ota.c, the loader, tools/fm1_install.py)
    $CC -o "$OUT/ota_test" tests/ota_test.c
    run "M-UPGRADE entry (FM-1)" "$OUT/ota_test" build/sq1d.fwsc
    head -c 60000 build/sq1d.bin > "$OUT/old_app.bin"
    python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" >/dev/null
    $CC -o "$OUT/ldr_screen_test" tests/ldr_screen_test.c
    run "update loader's screen: UPDATE MODE, WRITING and its bar, UPDATE FAILED" "$OUT/ldr_screen_test" "$OUT"
    $CC -o "$OUT/ldr_test" tests/ldr_test.c
    run "update loader (FM-1): other app -> this build" "$OUT/ldr_test" "$OUT/old.fwsc" build/sq1d.fwsc
    run "installer (tools/fm1_install.py)" python3 tests/install_test.py
else
    echo "(no build/sq1d.fwsc: the update-path tests are skipped; sh build.sh first)"
fi

[ "$fail" = 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit "$fail"
