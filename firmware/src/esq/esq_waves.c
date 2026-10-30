/* Wave bank: placeholder ROM set, .esqw loader and writer. */
#include "esq_waves.h"

#if !defined(ESQ_HOST) || ESQ_HOST
#include <math.h>
#include <string.h>
#define esqw_PLACEHOLDER 1
#else
#define esqw_PLACEHOLDER 0
#endif

/* ESQ-1 wave list (the OS 3.5 names table order). */
const char *const ESQ_ROM_WAVE_NAMES[ESQ_NROM_WAVES] = {
    "SAW",    "BELL",   "SINE",   "SQUARE", "PULSE",  "NOISE 1", "NOISE 2", "NOISE 3",
    "BASS",   "PIANO",  "EL PNO", "VOICE 1", "VOICE 2", "KICK",   "REED",    "ORGAN",
    "SYNTH 1", "SYNTH 2", "SYNTH 3", "FORMT 1", "FORMT 2", "FORMT 3", "FORMT 4", "FORMT 5",
    "PULSE 2", "SQR 2",  "4 OCTS", "PRIME",  "BASS 2", "E PNO 2", "OCTAVE", "OCT+5",
};

uint32_t esq_zone_rcp(uint32_t c)
{
    uint64_t rem = 0, q = 0;
    int i;
    if (!c)
        c = 65536u;
    for (i = 46; i >= 0; i--) {                        /* restoring division of 2^46 */
        rem = (rem << 1) | (i == 46);
        if (rem >= c) {
            rem -= c;
            q |= (uint64_t)1 << i;
        }
    }
    return q > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)q;
}

/* ------------------------------------------------------------- placeholders --- */
#if esqw_PLACEHOLDER

#define esqw_PH_LEN 256
#define esqw_NOISE_LOG2 12
#define esqw_NOISE_LEN (1 << esqw_NOISE_LOG2)
#define esqw_NOISE_CYCLES 16
#define esqw_POOL_LEN (29 * esqw_PH_LEN + 3 * esqw_NOISE_LEN)

static uint8_t esqw_ph_pool[esqw_POOL_LEN];
static float esqw_scratch[esqw_NOISE_LEN];

typedef struct {
    uint8_t h;
    float a;
} esqw_harm_t;

static uint8_t esqw_q8(float x)
{
    long v = lrintf(128.0f + x);
    if (v < 1)
        v = 1;
    if (v > 255)
        v = 255;
    return (uint8_t)v;
}

/* normalise esqw_scratch[0..len) to +-126 and esqw_quantise */
static void esqw_quantise(uint8_t *t, int len)
{
    float peak = 1e-9f, mean = 0.0f;
    int i;
    for (i = 0; i < len; i++)
        mean += esqw_scratch[i];
    mean /= (float)len;
    for (i = 0; i < len; i++) {
        esqw_scratch[i] -= mean;
        if (fabsf(esqw_scratch[i]) > peak)
            peak = fabsf(esqw_scratch[i]);
    }
    for (i = 0; i < len; i++)
        t[i] = esqw_q8(esqw_scratch[i] * 126.0f / peak);
}

static void esqw_additive(uint8_t *t, const esqw_harm_t *hp, int n)
{
    const float w = 6.28318530718f / esqw_PH_LEN;
    int i, k;
    for (i = 0; i < esqw_PH_LEN; i++) {
        float s = 0.0f;
        for (k = 0; k < n; k++)
            s += hp[k].a * sinf(w * (float)hp[k].h * (float)i);
        esqw_scratch[i] = s;
    }
    esqw_quantise(t, esqw_PH_LEN);
}

/* harmonic esqw_series 1..n with amplitude n^-p, odd-only when `odd` */
static void esqw_series(uint8_t *t, int n, float p, int odd)
{
    esqw_harm_t hp[64];
    int k, m = 0;
    for (k = 1; k <= n && m < 64; k++) {
        if (odd && !(k & 1))
            continue;
        hp[m].h = (uint8_t)k;
        hp[m].a = powf((float)k, -p);
        m++;
    }
    esqw_additive(t, hp, m);
}

/* esqw_formant: gaussian partial envelope centred on harmonic c, plus a fundamental */
static void esqw_formant(uint8_t *t, float c, float width, float fund)
{
    esqw_harm_t hp[32];
    int k;
    for (k = 1; k <= 32; k++) {
        float d = ((float)k - c) / width;
        hp[k - 1].h = (uint8_t)k;
        hp[k - 1].a = expf(-0.5f * d * d) + (k == 1 ? fund : 0.0f);
    }
    esqw_additive(t, hp, 32);
}

static void esqw_naive_pulse(uint8_t *t, float duty)
{
    int i;
    for (i = 0; i < esqw_PH_LEN; i++)
        esqw_scratch[i] = ((float)i < duty * esqw_PH_LEN) ? 1.0f : -1.0f;
    esqw_quantise(t, esqw_PH_LEN);
}

static void esqw_naive_saw(uint8_t *t)
{
    int i;
    for (i = 0; i < esqw_PH_LEN; i++)
        esqw_scratch[i] = (float)i / esqw_PH_LEN * 2.0f - 1.0f;
    esqw_quantise(t, esqw_PH_LEN);
}

static void esqw_noise(uint8_t *t, uint32_t seed, float lp)
{
    uint32_t x = seed;
    float y = 0.0f;
    int i, pass;
    for (pass = 0; pass < 2; pass++)          /* second pass: the loop point is continuous */
        for (i = 0; i < esqw_NOISE_LEN; i++) {
            float w;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            w = (float)(x >> 8) / 8388608.0f - 1.0f;
            y += lp * (w - y);
            esqw_scratch[i] = y;
        }
    esqw_quantise(t, esqw_NOISE_LEN);
}

#define esqw_H(...) ((const esqw_harm_t[]){__VA_ARGS__}), (int)(sizeof((const esqw_harm_t[]){__VA_ARGS__}) / sizeof(esqw_harm_t))

void esq_wavebank_placeholder(esq_wavebank_t *b)
{
    uint32_t off = 0;
    int w;
    memset(b, 0, sizeof(*b));
    b->nwaves = ESQ_NROM_WAVES;
    b->nzone = ESQ_NROM_WAVES;
    b->data = esqw_ph_pool;
    b->data_len = esqw_POOL_LEN;
    for (w = 0; w < ESQ_NROM_WAVES; w++) {
        esq_wave_t *wv = &b->wave[w];
        esq_zone_t *zn = &b->zone[w];
        int is_noise = (w >= 5 && w <= 7);
        uint8_t *t = esqw_ph_pool + off;
        strncpy(wv->name, ESQ_ROM_WAVE_NAMES[w], 8);
        wv->nzones = 1;
        wv->zone0 = (uint16_t)w;
        zn->top_note = 127;
        zn->log2len = is_noise ? esqw_NOISE_LOG2 : 8;
        zn->cycles_q16 = (is_noise ? esqw_NOISE_CYCLES : 1u) << 16;
        zn->rcp = esq_zone_rcp(zn->cycles_q16);
        zn->offset = off;
        off += is_noise ? esqw_NOISE_LEN : esqw_PH_LEN;
        switch (w) {
        case 0: esqw_naive_saw(t); break;
        case 1: esqw_additive(t, esqw_H({1, 1}, {2, .6f}, {4, .5f}, {5, .35f}, {7, .3f}, {9, .2f}, {11, .25f})); break;
        case 2: esqw_additive(t, esqw_H({1, 1})); break;
        case 3: esqw_naive_pulse(t, 0.5f); break;
        case 4: esqw_naive_pulse(t, 0.25f); break;
        case 5: esqw_noise(t, 0x9E3779B9u, 1.0f); break;
        case 6: esqw_noise(t, 0x85EBCA6Bu, 0.25f); break;
        case 7: esqw_noise(t, 0xC2B2AE35u, 0.06f); break;
        case 8: esqw_additive(t, esqw_H({1, 1}, {2, .8f}, {3, .6f}, {4, .5f}, {5, .4f}, {6, .3f}, {7, .25f}, {8, .2f})); break;
        case 9: esqw_additive(t, esqw_H({1, 1}, {2, .5f}, {3, .35f}, {4, .25f}, {5, .15f}, {6, .12f}, {7, .08f}, {8, .05f})); break;
        case 10: esqw_additive(t, esqw_H({1, 1}, {2, .15f}, {3, .08f}, {5, .05f}, {15, .12f})); break;
        case 11: esqw_formant(t, 4.0f, 1.5f, 0.6f); break;
        case 12: esqw_formant(t, 9.0f, 2.0f, 0.8f); break;
        case 13: esqw_additive(t, esqw_H({1, 1}, {2, .4f}, {3, .1f})); break;
        case 14: esqw_additive(t, esqw_H({1, 1}, {2, .2f}, {3, .7f}, {4, .15f}, {5, .5f}, {6, .1f}, {7, .35f}, {9, .25f})); break;
        case 15: esqw_additive(t, esqw_H({1, 1}, {2, 1}, {3, .7f}, {4, .6f}, {6, .4f}, {8, .4f})); break;
        case 16: esqw_series(t, 32, 1.3f, 0); break;
        case 17: esqw_series(t, 31, 1.5f, 1); break;
        case 18: esqw_additive(t, esqw_H({1, 1}, {3, .8f}, {6, .6f}, {9, .4f}, {12, .3f})); break;
        case 19: esqw_formant(t, 3.0f, 1.5f, 0.3f); break;
        case 20: esqw_formant(t, 5.0f, 1.5f, 0.3f); break;
        case 21: esqw_formant(t, 8.0f, 1.5f, 0.3f); break;
        case 22: esqw_formant(t, 12.0f, 1.5f, 0.3f); break;
        case 23: esqw_formant(t, 18.0f, 1.5f, 0.3f); break;
        case 24: esqw_naive_pulse(t, 0.125f); break;
        case 25: esqw_series(t, 15, 1.0f, 1); break;
        case 26: esqw_additive(t, esqw_H({1, 1}, {2, 1}, {4, 1}, {8, 1})); break;
        case 27: esqw_additive(t, esqw_H({2, 1}, {3, .8f}, {5, .6f}, {7, .5f}, {11, .4f}, {13, .3f})); break;
        case 28: esqw_additive(t, esqw_H({1, 1}, {2, .9f}, {3, .2f}, {4, .5f}, {5, .1f}, {6, .3f})); break;
        case 29: esqw_additive(t, esqw_H({1, 1}, {2, .3f}, {4, .1f}, {9, .12f}, {17, .08f})); break;
        case 30: esqw_additive(t, esqw_H({1, 1}, {2, 1})); break;
        default: esqw_additive(t, esqw_H({1, 1}, {2, .8f}, {3, .8f})); break;
        }
    }
}

#endif

/* ------------------------------------------------------------- .esqw I/O --- */

static uint32_t esqw_rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t esqw_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
#if esqw_PLACEHOLDER
static void esqw_wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
#endif

#define esqw_HDR_LEN 12
#define esqw_WAVE_HDR_LEN 12
#define esqw_ZONE_LEN 12
#define esqw_ZONE_LEN_V1 8

#if ESQ_ZONE_INPLACE                                   /* (the image's v2 zone record is this struct, little-endian) */
typedef char esqw_zone_layout[sizeof(esq_zone_t) == esqw_ZONE_LEN && __builtin_offsetof(esq_zone_t, cycles_q16) == 4 &&
                              __builtin_offsetof(esq_zone_t, offset) == 8 ? 1 : -1];
#endif
static int esqw_load(esq_wavebank_t *b, const uint8_t *buf, uint8_t *wbuf, size_t len)
{
    size_t pos = esqw_HDR_LEN, i, zl;
    uint32_t data_len;
    int w, z;
    memset(b, 0, sizeof(*b));
    if (len < esqw_HDR_LEN || memcmp(buf, "ESQW", 4) != 0)
        return -1;                                     /* not a bank */
    if (buf[4] != 1 && buf[4] != 2)
        return -2;                                     /* version */
    zl = buf[4] == 1 ? esqw_ZONE_LEN_V1 : esqw_ZONE_LEN;
    b->nwaves = buf[5];
    data_len = esqw_rd32(buf + 8);
    if (b->nwaves == 0 || b->nwaves > ESQ_MAX_WAVES)
        return -3;
    for (w = 0; w < b->nwaves; w++) {
        esq_wave_t *wv = &b->wave[w];
        if (pos + esqw_WAVE_HDR_LEN > len)
            return -4;                                 /* truncated */
        memcpy(wv->name, buf + pos, 8);
        wv->name[8] = 0;
        wv->nzones = buf[pos + 8];
#if !ESQ_ZONE_INPLACE
        wv->zone0 = b->nzone;
#endif
        pos += esqw_WAVE_HDR_LEN;
        if (wv->nzones == 0 || wv->nzones > ESQ_MAX_ZONES)
            return -5;
#if ESQ_ZONE_INPLACE
        if (zl != esqw_ZONE_LEN || ((uintptr_t)(buf + pos) & 3u))
            return -11;                                /* (in place: v2 records, 4-aligned) */
        wv->z = (const esq_zone_t *)(const void *)(buf + pos);
#else
        if (b->nzone + wv->nzones > ESQ_ZONE_POOL)
            return -10;                                /* more zones than this build holds */
#endif
        if (pos + (size_t)wv->nzones * zl > len)
            return -4;
#if ESQ_ZONE_INPLACE
        for (z = 0; z < wv->nzones; z++) {             /* (checked as they are: read only) */
            const esq_zone_t *zn = &wv->z[z];
            pos += zl;
            if (zn->log2len < ESQ_LOG2LEN_MIN || zn->log2len > ESQ_LOG2LEN_MAX || !zn->cycles_q16)
                return -6;
            if (zn->top_note > 127 || (z > 0 && zn->top_note <= zn[-1].top_note))
                return -7;
            if ((uint64_t)zn->offset + (1u << zn->log2len) > data_len)
                return -8;
        }
        b->nzone = (uint16_t)(b->nzone + wv->nzones);
#else
        for (z = 0; z < wv->nzones; z++) {
            esq_zone_t *zn = &b->zone[b->nzone + z];
            zn->top_note = buf[pos];
            zn->log2len = buf[pos + 1];
            if (zl == esqw_ZONE_LEN_V1) {
                zn->flags = 0;
                zn->cycles_q16 = (uint32_t)esqw_rd16(buf + pos + 2) << 16;
                zn->offset = esqw_rd32(buf + pos + 4);
            } else {
                zn->flags = buf[pos + 2];
                zn->cycles_q16 = esqw_rd32(buf + pos + 4);
                zn->offset = esqw_rd32(buf + pos + 8);
            }
            zn->rcp = esq_zone_rcp(zn->cycles_q16);
            pos += zl;
            if (zn->log2len < ESQ_LOG2LEN_MIN || zn->log2len > ESQ_LOG2LEN_MAX || !zn->cycles_q16)
                return -6;
            if (zn->top_note > 127 || (z > 0 && zn->top_note <= zn[-1].top_note))
                return -7;                             /* zones out of order */
            if ((uint64_t)zn->offset + (1u << zn->log2len) > data_len)
                return -8;                             /* table outside the data */
        }
        b->zone[b->nzone + wv->nzones - 1].top_note = 127;
        b->nzone = (uint16_t)(b->nzone + wv->nzones);
#endif
    }
    if (pos + data_len > len)
        return -4;
    b->data = buf + pos;
    b->data_len = data_len;
    for (i = 0; i < data_len; i++)                     /* 0x00 would halt a DOC oscillator */
        if (buf[pos + i] == 0) {
            if (!wbuf)
                return -9;
            wbuf[pos + i] = 1;
        }
    pos += data_len;
    while (pos + 8 <= len) {                           /* optional chunks */
        uint32_t cl = esqw_rd32(buf + pos + 4);
        if ((uint64_t)pos + 8 + cl > len)
            break;
        if (!memcmp(buf + pos, "PTCH", 4) && cl >= 102) {
            b->patches = buf + pos + 8;
            b->npatches = (uint8_t)(cl / 102 > 255 ? 255 : cl / 102);
        }
        pos += 8 + cl;
    }
    return 0;
}

int esq_wavebank_load(esq_wavebank_t *b, uint8_t *buf, size_t len) { return esqw_load(b, buf, buf, len); }
int esq_wavebank_load_const(esq_wavebank_t *b, const uint8_t *buf, size_t len) { return esqw_load(b, buf, 0, len); }

#if esqw_PLACEHOLDER
long esq_wavebank_write(const esq_wavebank_t *b, uint8_t *out, size_t cap)
{
    size_t need = esqw_HDR_LEN, pos;
    int w, z;
    for (w = 0; w < b->nwaves; w++)
        need += esqw_WAVE_HDR_LEN + (size_t)b->wave[w].nzones * esqw_ZONE_LEN;
    need += b->data_len;
    if (b->patches && b->npatches)
        need += 8 + (size_t)b->npatches * 102;
    if (!out)
        return (long)need;
    if (cap < need)
        return -1;
    memset(out, 0, need);
    memcpy(out, "ESQW", 4);
    out[4] = 2;
    out[5] = b->nwaves;
    esqw_wr32(out + 8, b->data_len);
    pos = esqw_HDR_LEN;
    for (w = 0; w < b->nwaves; w++) {
        const esq_wave_t *wv = &b->wave[w];
        memcpy(out + pos, wv->name, strnlen(wv->name, 8));
        out[pos + 8] = wv->nzones;
        pos += esqw_WAVE_HDR_LEN;
        for (z = 0; z < wv->nzones; z++) {
            const esq_zone_t *zn = esq_zone_of(b, (uint32_t)w, (uint32_t)z);
            out[pos] = zn->top_note;
            out[pos + 1] = zn->log2len;
            out[pos + 2] = zn->flags;
            esqw_wr32(out + pos + 4, zn->cycles_q16);
            esqw_wr32(out + pos + 8, zn->offset);
            pos += esqw_ZONE_LEN;
        }
    }
    memcpy(out + pos, b->data, b->data_len);
    pos += b->data_len;
    if (b->patches && b->npatches) {
        memcpy(out + pos, "PTCH", 4);
        esqw_wr32(out + pos + 4, (uint32_t)b->npatches * 102);
        memcpy(out + pos + 8, b->patches, (size_t)b->npatches * 102);
    }
    return (long)need;
}
#endif

const esq_zone_t *esq_wave_zone(const esq_wavebank_t *b, uint32_t wave, int note)
{
    const esq_wave_t *wv = &b->wave[wave < b->nwaves ? wave : 0];
#if ESQ_ZONE_INPLACE
    const esq_zone_t *zn = wv->z;                       /* (the last plays every note above it: 127 or not) */
#else
    const esq_zone_t *zn = &b->zone[wv->zone0];
#endif
    int z;
    for (z = 0; z < wv->nzones - 1; z++)
        if (note <= zn[z].top_note)
            return &zn[z];
    return &zn[wv->nzones - 1];
}

#undef esqw_H
#undef esqw_PLACEHOLDER
#undef esqw_PH_LEN
#undef esqw_NOISE_LOG2
#undef esqw_NOISE_LEN
#undef esqw_NOISE_CYCLES
#undef esqw_POOL_LEN
#undef esqw_HDR_LEN
#undef esqw_WAVE_HDR_LEN
#undef esqw_ZONE_LEN
#undef esqw_ZONE_LEN_V1
