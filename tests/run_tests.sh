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
run() {                                         # (on GitHub a failure is an annotation, readable without the log)
    name="$1"; shift
    echo "== $name"
    if ! "$@" > "$OUT/last.log" 2>&1; then
        fail=1; cat "$OUT/last.log"
        if [ -n "$GITHUB_ACTIONS" ]; then echo "::error title=$name::$(tail -c 900 "$OUT/last.log" | tr '\n' '|')"; fi
    else cat "$OUT/last.log"; fi
}

# the ESQ-1 engine's own tests (pitch, DOC, envelopes, LFOs, CVs, voices, SYNC, AM, filter, programs, glide): the float
# model and the integer build the firmware runs, the integer one checked against the float one
for q in 0 1; do                                # (a build that fails is a failure too)
    run "ESQ-1 engine (ESQ_FIXED=$q)" sh -c "cc -O2 -std=c99 -Wall -Wno-unused-function -D_DEFAULT_SOURCE -DESQ_FIXED=$q \
        -o $OUT/esq_engine_test$q tests/esq_engine_test.c -lm && cd tests && ../$OUT/esq_engine_test$q"
done

run "the app on the host: the default script plays, screens and sound" sh tests/run_host.sh
run "R1: programs, keys, octave (tests/sq1d_r1.txt): sound comes out" sh -c \
    "sh tests/run_host.sh tests/sq1d_r1.txt >/dev/null && python3 -c \"
import wave, array; w = wave.open('$OUT/sq1d.wav'); a = array.array('h'); a.frombytes(w.readframes(w.getnframes()))
assert max(abs(x) for x in a) > 1000, 'silent'\""

# R2: WRITE into INT (flash), the ROM set and the edit buffer as ESQ-1 dumps on request, both parse
run "R2: WRITE, dumps out (tests/sq1d_r2.txt)" sh -c "rm -f $OUT/sq1d-out.syx; sh tests/run_host.sh tests/sq1d_r2.txt | grep -q 'bank 0 saved' \
    && python3 tools/esq_syx.py list $OUT/sq1d-out.syx | grep -c . | grep -qx 41"

# R3: the voice pages: EDIT / FX / ENV / EDIT + key, cursor, data slider, knobs, COMPARE, UNDO, WRITE of the edit
run "R3: the voice pages (tests/sq1d_r3.txt)" sh -c "sh tests/run_host.sh tests/sq1d_r3.txt | grep -q 'bank 0 saved'"

# R4: system pages, arpeggiator, latch, chord memory, step entry, PLAY, the pattern stored, IMPORT asks (into CART A);
# FX / GLO page order, STORAGE's dump out, real-time recording, transpose; MIDI clock in
python3 tools/esq_syx.py bank "$OUT/sq1d-out.syx" -o "$OUT/bank40.syx" >/dev/null
run "R4: ARP, LATCH, CHORD, SEQ step entry + PLAY + SAVE, IMPORT (tests/sq1d_r4.txt)" sh -c "sh tests/run_host.sh tests/sq1d_r4.txt > $OUT/r4.log; \
    grep -q 'blob 0 saved' $OUT/r4.log && grep -q 'bank 1 saved' $OUT/r4.log && \
    [ \$(awk '\$1 < 2800 && /MIDI out 90/' $OUT/r4.log | wc -l) -ge 8 ] && grep -q '7419 ms  MIDI out 90 60' $OUT/r4.log"
run "R4: pages, dump out, real-time recording, transpose (tests/sq1d_r4b.txt)" sh -c "sh tests/run_host.sh tests/sq1d_r4b.txt > $OUT/r4b.log; \
    grep -q 'sysex out: 8166 B' $OUT/r4b.log && grep -q 'MIDI out 90 67' $OUT/r4b.log"
run "R4: MIDI clock in drives the sequencer (tests/sq1d_r4clk.txt)" sh -c "sh tests/run_host.sh tests/sq1d_r4clk.txt > $OUT/clk.log; \
    awk '\$1 > 2100 && /MIDI out 90 64/' $OUT/clk.log | grep -q ."

python3 -c "import sys; open(sys.argv[1], 'wb').write(bytes([0xF0, 0x7D, 0x46, 0x4C, 102, 2, 0xF7]) + open(sys.argv[2], 'rb').read())" \
    "$OUT/to_cartb.syx" "$OUT/bank40.syx"
printf '100 sysex %s\n600 end\n' "$OUT/to_cartb.syx" > "$OUT/to_cartb.txt"
run "R4.1: COMPARE as the original: C*, the edit kept over a program change and recalled (tests/sq1d_r41.txt)" sh -c \
    "sh tests/run_host.sh tests/sq1d_r41.txt >/dev/null"
python3 -c "import sys; d = open(sys.argv[2], 'rb').read(); open(sys.argv[1], 'wb').write(d[:3000] + bytes([0xF0, 0x7D, 0x46, 0x4C, 0x63, 0xF7]) + d[3000:])" \
    "$OUT/nested.syx" "$OUT/bank40.syx"
printf '100 sysex %s\n500 btn 1\n800 end\n' "$OUT/nested.syx" > "$OUT/nested.txt"
run "R4.1: a 40-program dump with another sender's SysEx inside it is still taken" sh -c \
    "sh tests/run_host.sh $OUT/nested.txt | grep -q 'bank 0 saved'"
run "R4: editor command 102 (esq_syx.py send --to cartb): a 40-program dump straight into CART B" sh -c \
    "sh tests/run_host.sh $OUT/to_cartb.txt | grep -q 'bank 2 saved'"

# R5: the extension pages (scan, LFO shape / sync, unison, drift, MOD+), LAYER, MODERN look, NRPN, MUTATE, WRITE with
# the extension block, TUNING, RANDOM: sound, and the extension sector stored
run "R5: extension fields, LAYER, MODERN, NRPN, MUTATE / RANDOM, WRITE (tests/sq1d_r5.txt)" sh -c "sh tests/run_host.sh tests/sq1d_r5.txt > $OUT/r5.log; \
    grep -q 'bank 3 saved (1280 B)' $OUT/r5.log && python3 -c \"
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
