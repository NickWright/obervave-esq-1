/* ESQ-1 programs: the 102-byte PCB format and SysEx (Musician's Manual, Appendix 6).
 *
 * PCB program, 102 bytes:
 *   0   name, 6 ASCII characters
 *   6   ENV1..ENV4, 10 bytes each: L1 L2 L3 (signed, 2 x level) T1 T2 T3 T4 LV (4 x value) T1V TK
 *   46  LFO1..LFO3, 4 bytes each: [WAVE:2 | FREQ:6] [MOD hi:2 | L1:6] [MOD lo:2 | L2:6] [RESET HUMAN | DELAY:6]
 *   58  OSC1..OSC3, 10 bytes each: SEMI (0..72, 36 = 0) FINE (x 8) [FM2:4 | FM1:4] FM1AMT FM2AMT (signed, 2 x value)
 *       WAVE [ON | 2 x LEVEL] [AM2:4 | AM1:4] AM1AMT AM2AMT (signed, 2 x value)
 *   88  [AM | 2 x DCA4]      89 [SYNC | FREQ]      90 Q      91 [FMOD2:4 | FMOD1:4]
 *   92  [VC | FMOD1AMT:7 signed]   93 [MONO | FMOD2AMT:7 signed]   94 [ENV | 2 x KEYBD]   95 [OSC | GLIDE:6]
 *   96  [SPLIT DIR | SPLIT POINT]  97 [LAYER | PROG]  98 [SPLIT | PROG]  99 [SPLIT/LAYER | PROG]
 *   100 [PAN:4 | PANMOD:4]    101 [CYC | PANMODAMT:7 signed]
 * The byte layout was checked against the 40 programs in OS 3.5 and the OS code that reads them (the LFO byte that
 * carries FREQ also carries the wave; the modulator nibbles are split over L1 and L2).
 *
 * SysEx: F0 0F 02 0c <code> <data> F7, c the MIDI channel; each byte sent as two nibbles, low first.
 *   code 01: one program (204 nibbles)   code 02: all 40 programs (8160 nibbles). */
#ifndef ESQ_PATCH_H
#define ESQ_PATCH_H

#include <stddef.h>
#include <stdint.h>

#include "esq_types.h"

#define ESQ_PCB_LEN 102

void esq_patch_from_pcb(esq_patch_t *p, const uint8_t *pcb);
void esq_patch_to_pcb(const esq_patch_t *p, uint8_t *pcb);

/* Programs in a SysEx stream (any number of messages): returns how many were stored (up to max), or -1 if no
 * ESQ-1 program message was found. */
int esq_sysex_parse(const uint8_t *buf, size_t len, esq_patch_t *out, int max);

/* One-program dump (code 01) on MIDI channel ch (0..15): writes ESQ_SYSEX_SINGLE_LEN bytes. */
#define ESQ_SYSEX_SINGLE_LEN (5 + 2 * ESQ_PCB_LEN + 1)
void esq_sysex_single(const esq_patch_t *p, int ch, uint8_t *out);

#endif
