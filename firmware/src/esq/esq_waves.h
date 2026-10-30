/* Wave bank: DOC-style 8-bit tables, optionally keymapped (multisampled).
 *
 * Samples are unsigned 8-bit, 0x80 = zero. 0x00 halts a DOC oscillator, so the loader maps 0x00 -> 0x01.
 * A zone's table holds `cycles` periods of the fundamental (fractional: a table tuned as the ESQ-1 OS tunes
 * it, by semitone and fine offsets, need not hold a whole number): single-cycle waves use cycles = 1.
 *
 * .esqw file layout (little-endian):
 *   "ESQW" u8 version(2) u8 nwaves u16 reserved u32 data_len
 *   nwaves x { char name[8]; u8 nzones; u8 pad[3];
 *              nzones x { u8 top_note; u8 log2len; u8 flags; u8 pad; u32 cycles_q16; u32 offset; } }
 *   data[data_len]
 *   optional chunks: char tag[4]; u32 len; u8 body[len]
 *     "PTCH": programs, 102 bytes each in the PCB format (esq_patch.h); tools/esq1rom.py stores the OS's 40 here
 * Version 1 (still read): zones of 8 bytes { u8 top_note; u8 log2len; u16 cycles; u32 offset; }.
 * flags bit 0: one-shot (the ESQ-1's sampled waves 54..74: the oscillator plays the table once, then halts, as the
 * ES5503's one-shot mode); bit 2: an alias row (the ROM has no zones for it; it plays SAW: hosts may skip it).
 * Zones are in ascending top_note order; the last zone covers up to note 127. */
#ifndef ESQ_WAVES_H
#define ESQ_WAVES_H

#include <stddef.h>
#include <stdint.h>

#ifndef ESQ_MAX_WAVES
#define ESQ_MAX_WAVES 128
#endif
#define ESQ_MAX_ZONES 16           /* the ESQ-1 OS splits a wave into up to 16 key ranges */
#ifndef ESQ_ZONE_POOL
#define ESQ_ZONE_POOL 1024         /* a bank's zones, all its waves together (the ESQ-1's 75 waves: 340) */
#endif
#define ESQ_NROM_WAVES 32
#define ESQ_LOG2LEN_MIN 4
#define ESQ_LOG2LEN_MAX 15        /* DOC tables top out at 32 KB */

#ifndef ESQ_ZONE_INPLACE
#define ESQ_ZONE_INPLACE 0         /* 1: a bank's zones read in place from its image (flash), not copied: no rcp (the voice
                                    * keeps it), the image's zone records 4-aligned (v2, 12 bytes: this struct's layout) */
#endif

#define ESQ_ZONE_ONESHOT 1u
#define ESQ_ZONE_ALIAS 4u

typedef struct {
    uint8_t top_note;             /* highest MIDI note this zone plays */
    uint8_t log2len;              /* table length = 1 << log2len */
    uint8_t flags;                /* bit 0: one-shot, bit 2: an alias row (ESQ_ZONE_*) */
    uint8_t pad;
    uint32_t cycles_q16;          /* fundamental periods in the table, 16.16 */
    uint32_t offset;              /* into data */
#if !ESQ_ZONE_INPLACE
    uint32_t rcp;                 /* 2^46 / cycles_q16 (the loaders set it; 0: the engine works it out) */
#endif
} esq_zone_t;

/* 2^46 / c (c: cycles, 16.16), by shifts: no 64-bit divide, which the FM-1's toolchain does not link */
uint32_t esq_zone_rcp(uint32_t c);

typedef struct {
    char name[9];
    uint8_t nzones;
#if ESQ_ZONE_INPLACE
    const esq_zone_t *z;          /* its zones, in the image */
#else
    uint16_t zone0;               /* its first zone in the bank's zone[] */
#endif
} esq_wave_t;

typedef struct {
    uint8_t nwaves;
    uint16_t nzone;
    esq_wave_t wave[ESQ_MAX_WAVES];
#if !ESQ_ZONE_INPLACE
    esq_zone_t zone[ESQ_ZONE_POOL];
#endif
    const uint8_t *data;
    uint32_t data_len;
    const uint8_t *patches;       /* "PTCH" chunk: npatches x 102-byte PCB programs, or 0 */
    uint8_t npatches;
} esq_wavebank_t;

/* Fill `b` with procedurally generated stand-ins for the 32 ESQ-1 ROM waves (names, rough timbres).
 * Not the ROM content: load a bank built from your own ROM dump for the real waves. (Float: host builds only; the
 * integer build has them as a ready image, engine/esq_phbank.h.) */
#if !defined(ESQ_HOST) || ESQ_HOST
void esq_wavebank_placeholder(esq_wavebank_t *b);
#endif

/* Parse an .esqw image. `buf` must outlive the bank (tables point into it) and be writable
 * (0x00 samples are rewritten to 0x01). Returns 0, or a negative error code. */
int esq_wavebank_load(esq_wavebank_t *b, uint8_t *buf, size_t len);
/* The same for an image in read-only memory, whose tables hold no 0x00 (tools write them so): -9 if one does. */
int esq_wavebank_load_const(esq_wavebank_t *b, const uint8_t *buf, size_t len);

/* Serialize to .esqw (with the PTCH chunk when b->patches is set). Returns bytes written (or needed, if out is NULL), negative on error. */
#if !defined(ESQ_HOST) || ESQ_HOST
long esq_wavebank_write(const esq_wavebank_t *b, uint8_t *out, size_t cap);
#endif

/* Zone z of wave w (no checks). */
#if ESQ_ZONE_INPLACE
static inline const esq_zone_t *esq_zone_of(const esq_wavebank_t *b, uint32_t w, uint32_t z) { return &b->wave[w].z[z]; }
#else
static inline const esq_zone_t *esq_zone_of(const esq_wavebank_t *b, uint32_t w, uint32_t z) { return &b->zone[b->wave[w].zone0 + z]; }
#endif

/* The zone that plays `note` (wave index clamped to the bank). */
const esq_zone_t *esq_wave_zone(const esq_wavebank_t *b, uint32_t wave, int note);

extern const char *const ESQ_ROM_WAVE_NAMES[ESQ_NROM_WAVES];

#endif
