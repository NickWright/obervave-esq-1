/* ESQ-1 voice engine v2 (see esq_engine.h). Integer control path; float audio path, or (ESQ_FIXED) integer from
 * tables made out of this file's float model; no allocation. */
#include "esq_engine.h"

#if ESQ_HOST
#include <math.h>
#include <string.h>
#endif
#if ESQ_SPLIT && !ESQ_HOST && !defined(ESQ_HAVE_MEMSET)
void *memset(void *d, int c, unsigned n); /* (the host's: Felucca's libc.c) */
#endif

#define ESQ_PI_F 3.14159265359f

#ifndef ESQ_TAP_X
#define ESQ_TAP_X(v, x) ((void)0)                         /* test hook: a voice's DAC signal, band-limited */
#endif

/* (v6.5.8) ESQ_RAMFN, a host's attribute for code that runs from RAM (Felucca: section .ram_text, esq_felucca.h): a
 * second copy of the per-sample loop (esq_voice_run) is built with it, with the hold tables in RAM too, and
 * esq_ram_on picks it (1) or the copy in flash (0). The two are the same code: the same output to the bit */
#ifdef ESQ_RAMFN
#define ESQ_RAM 1
#else
#define ESQ_RAM 0
#endif
#define ESQ_HOTINL static inline __attribute__((always_inline))   /* (into both copies: no calls from the RAM one) */

/* ------------------------------------------------------------- analog constants --- */
#if !ESQ_FIXED
/* From the schematics and the CEM3379 data sheet (docs/HARDWARE.md); the DOC's output level is an estimate. */
#define ESQ_V_DOC 1.2f            /* V at the voice demultiplexer for a full-scale sample at volume 255 */
#define ESQ_HOLD_GAIN 0.0676f     /* hold capacitor, per oscillator: 3 x 0.5 us a 26 us frame into 1.02 k, 4.5 k load */
#define ESQ_HOLD_HZ 2960.0f       /* its time constant: 15 nF over (3 x 0.5/26/1.02 k + 1/4.5 k) */
#define ESQ_VCF_CLIP 0.52f        /* filter input: tanh scale for 1% THD at 360 mVpp */
#define ESQ_VCF_GAIN 7.5f
#define ESQ_VCF_SWING 2.5f        /* 5 Vpp */
#define ESQ_RES_COMP 0.4f         /* input fed to the resonance VCA: passband -6 dB near full resonance */
#define ESQ_VCA_RIN 7500.0f       /* filter -> 3.3 uF + 7.5 k -> final VCA (current input) */
#define ESQ_VCA_IMAX 200e-6f
#define ESQ_PAN_R 470.0f          /* VCA output current -> 470 ohm -> pan OTA inputs */
#define ESQ_OTA_VT 0.048f         /* 3080: 5% THD at 75 mVpp */
#define ESQ_OUT_NORM (0.25f / ESQ_OTA_VT)   /* a saturated voice, panned hard: 0.25 full scale */
#define ESQ_DOC_SCALE (ESQ_V_DOC * ESQ_HOLD_GAIN / (128.0f * 255.0f))   /* DAC units -> V on the hold capacitor */
#define ESQ_ARG_K (ESQ_VCF_SWING / ESQ_VCA_RIN * ESQ_PAN_R / ESQ_OTA_VT)   /* output swing -> the OTA's argument */
#endif
#define ESQ_FC_TUNE 6             /* the autotune's offset at nominal parts: OS tune result (~150) - 0x90 */

/* ------------------------------------------------------------- OS tables --- */
/* Curves from OS 3.5, as piecewise-linear knots (within 1 LSB of the ROM's tables): */
static const uint8_t esq_k_vca[][2] = {    /* final-VCA index (ENV4 x DCA4) -> VCA DAC (0xF01C) */
    {0, 9}, {23, 18}, {37, 28}, {54, 46}, {81, 101}, {101, 162}, {119, 234}, {120, 242}, {123, 254}, {127, 255}};
static const uint8_t esq_k_docvol[][2] = { /* DCA index (2 x LEVEL + mods) -> DOC volume register (0xF09C) */
    {0, 0}, {2, 8}, {6, 12}, {49, 23}, {67, 35}, {79, 50}, {89, 71}, {96, 92}, {103, 121}, {108, 148},
    {114, 190}, {117, 215}, {121, 255}, {127, 255}};
static const uint8_t esq_k_envvel[][2] = { /* velocity -> envelope velocity (0xF11C) */
    {0, 1}, {72, 71}, {108, 100}, {122, 117}, {127, 127}};
static const uint8_t esq_k_vel2[][2] = {   /* velocity -> VEL2 (0xF19C) */
    {0, 0}, {11, 2}, {26, 12}, {43, 32}, {88, 99}, {102, 115}, {117, 125}, {127, 127}};

/* T -> ticks of 10 ms: the Musician's Manual's envelope table (0.01 .. 20.48 s) */
ESQ_SHARED const uint16_t esq_ticks[64] = {
    1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 23, 25, 29, 32, 36, 40, 45, 51,
    57, 64, 72, 81, 91, 102, 114, 128, 144, 161, 181, 203, 228, 256, 287, 323, 362, 406, 456, 512, 575, 645, 724,
    813, 912, 1024, 1149, 1290, 1448, 1625, 1825, 2048};

/* LFO FREQ 1..6: the phase moves on these bits of a 7-tick rotation (FREQ 7.. adds FREQ - 6 a tick) */
static const uint8_t esq_lfo_slow[7] = {0x00, 0x01, 0x11, 0x25, 0x2D, 0x37, 0x77};

#if ESQ_CONST_TABLES                     /* (a host short of RAM: the four curves made once, in flash) */
#include "esq_ktab.h"
#else
static uint8_t esq_vcacurve[128], esq_docvol[128], esq_envvel[128], esq_vel2[128];
#endif
#if ESQ_FIXED
#include "esq_qtab.h"                      /* esq_rate, esq_log6, esq_noise, esq_drift and the audio path's tables */
#else
static int16_t esq_rate[112];              /* segment rate (8.8 per tick) by log index */
static uint8_t esq_log6[128];              /* 64 + 6 log2(n) */
static int8_t esq_noise[256];              /* LFO NOISE: a smooth random cycle (made here, not the ROM's) */
static int8_t esq_drift[256];              /* LFO HUMAN: sparse +-1 rate steps */
static uint32_t esq_q_exp[384];            /* 2^(i / 384), Q31 (the DOC frequency) */
#endif
static int esq_tables_ready;

ESQ_COLD static void esq_knots(uint8_t *out, const uint8_t (*k)[2], int n)
{
    int i, x;
    for (i = 0; i + 1 < n; i++)
        for (x = k[i][0]; x <= k[i + 1][0]; x++)
            out[x] = (uint8_t)((k[i][1] * (k[i + 1][0] - x) + k[i + 1][1] * (x - k[i][0]) +
                                (k[i + 1][0] - k[i][0]) / 2) / (k[i + 1][0] - k[i][0]));
}

static uint32_t esq_xrand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

ESQ_COLD static void esq_tables_init(void)
{
#if !ESQ_FIXED
    uint32_t r = 0x9E3779B9u;
    float acc[256], v = 0.0f, peak = 0.0f;
    int i, sign = 1, gap = 0;
#endif
#if !ESQ_CONST_TABLES
    esq_knots(esq_vcacurve, esq_k_vca, (int)(sizeof esq_k_vca / 2));
    esq_knots(esq_docvol, esq_k_docvol, (int)(sizeof esq_k_docvol / 2));
    esq_knots(esq_envvel, esq_k_envvel, (int)(sizeof esq_k_envvel / 2));
    esq_knots(esq_vel2, esq_k_vel2, (int)(sizeof esq_k_vel2 / 2));
#endif
#if !ESQ_FIXED
    for (i = 0; i < 112; i++) {                           /* 128 x 2^((k - 61) / 6), at least 1 */
        float x = floorf(128.0f * exp2f((float)(i - 61) / 6.0f) + 0.5f);
        esq_rate[i] = (int16_t)(x < 1.0f ? 1.0f : x > 32767.0f ? 32767.0f : x);
    }
    esq_log6[0] = 64;
    for (i = 1; i < 128; i++)
        esq_log6[i] = (uint8_t)(64.0f + floorf(6.0f * log2f((float)i) + 0.5f));
    for (i = 0; i < 256; i++) {                           /* random walk, closed into a loop, smoothed */
        v += (float)((int)(esq_xrand(&r) >> 24) - 128) / 128.0f;
        acc[i] = v;
    }
    for (i = 0; i < 256; i++)
        acc[i] -= v * (float)i / 256.0f;
    for (i = 0; i < 256; i++) {
        float m = (acc[(i + 255) & 255] + 2.0f * acc[i] + acc[(i + 1) & 255]) * 0.25f;
        esq_drift[i] = 0;
        acc[i] = m;
    }
    for (i = 0; i < 256; i++)
        peak = fabsf(acc[i]) > peak ? fabsf(acc[i]) : peak;
    v = 0.0f;
    for (i = 0; i < 256; i++)
        v += acc[i];
    v /= 256.0f;
    for (i = 0; i < 256; i++)
        esq_noise[i] = (int8_t)lrintf((acc[i] - v) / (peak + fabsf(v)) * 120.0f);
    for (i = 0; i < 256; i++) {                           /* alternating +1 / -1 at random gaps of 4..11 ticks */
        if (gap-- > 0)
            continue;
        esq_drift[i] = (int8_t)sign;
        sign = -sign;
        gap = 3 + (int)(esq_xrand(&r) >> 29);
    }
    for (i = 0; i < 384; i++)
        esq_q_exp[i] = (uint32_t)llrint(2147483648.0 * exp2((double)i / 384.0));
#endif
    esq_tables_ready = 1;
}

/* ------------------------------------------------------------- helpers --- */

#if !ESQ_FIXED
static inline float esq_clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
#endif
static inline int esq_clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline int esq_asr8(int x) { return x >= 0 ? x >> 8 : -((-x + 255) >> 8); }     /* floor(x / 256) */

/* (JIANT-ESQ) the 32 x 32 -> 64-bit products through hooks a host may put out of line: JIANT's FM-1 build has each in a
 * helper of its own (eng_esq.c: core.h mul64; felucca-bsp docs/CPU.md, a multiply into a register pair beside a load
 * now and then leaves a register off). The same products to the bit either way. ESQ_MUL64(a, b): the product;
 * ESQ_MSH(a, b, sh): it >> sh (sh 16 17 23 26 28 30 31), as an int32_t; ESQ_UMUL64: unsigned */
#ifndef ESQ_MUL64
#define ESQ_MUL64(a, b) ((int64_t)(a) * (int64_t)(b))
#endif
#ifndef ESQ_UMUL64
#define ESQ_UMUL64(a, b) ((uint64_t)(a) * (uint64_t)(b))
#endif
#ifndef ESQ_MSH
#define ESQ_MSH(a, b, sh) ((int32_t)(ESQ_MUL64(a, b) >> (sh)))
#endif
#if ESQ_FIXED
static inline int32_t esq_m30(int32_t a, int32_t b) { return ESQ_MSH(a, b, 30); }
static inline int32_t esq_m28(int32_t a, int32_t b) { return ESQ_MSH(a, b, 28); }
static inline int32_t esq_m31(int32_t a, int32_t b) { return ESQ_MSH(a, b, 31); }
/* the cubic soft clip below, Q24: x - x^3 / 6.75 up to +-1.5, then +-1 (32-bit, at Q15 inside) */
static inline int32_t esq_tanh_q(int32_t x)
{
    int32_t a, a2;
    if (x >= (3 << 23))
        return 1 << 24;
    if (x <= -(3 << 23))
        return -(1 << 24);
    a = x >> 9;                                           /* Q15, |a| < 49152 */
    a2 = (a * a) >> 15;                                   /* < 73728 */
    return x - ((((a2 * (a >> 1)) >> 14) * 4855) >> 6);   /* 4855: 32768 / 6.75; Q15 -> Q24 */
}
#else
/* cubic soft clip, tanh-like: slope 1 at 0, +-1 with zero slope at +-1.5 (no divide) */
static inline float esq_tanh_fast(float x)
{
    if (x > 1.5f)
        return 1.0f;
    if (x < -1.5f)
        return -1.0f;
    return x - 0.148148148f * x * x * x;
}
#endif

/* the segment rate for a level change d over T (the OS's log / exp tables): about d / ticks(T) a tick */
static inline int16_t esq_seg_rate(int d, int t)
{
    int k = esq_log6[d > 127 ? 127 : d] - t;
    return esq_rate[k < 0 ? 0 : k > 111 ? 111 : k];
}

/* ------------------------------------------------------------- curves --- */

int esq_env_ticks(int t) { return esq_ticks[esq_clampi(t, 0, 63)]; }

/* the phase steps (1/256 cycle) per 7 ticks: FREQ - 6 a tick above 6, the rotation's bits below */
static int esq_lfo_steps7(int f)
{
    int b, n = 0;
    f = esq_clampi(f, 0, 63);
    if (f > 6)
        return (f - 6) * 7;
    for (b = 0; b < 7; b++)
        n += (esq_lfo_slow[f] >> b) & 1;
    return n;
}
int esq_lfo_mhz(int f) { return esq_lfo_steps7(f) * 100000 / (7 * 256); }

#if ESQ_HOST
float esq_env_time_s(int t) { return (float)esq_ticks[esq_clampi(t, 0, 63)] * ESQ_TICK_S; }
float esq_lfo_hz(int f) { return (float)esq_lfo_steps7(f) / 7.0f / 256.0f / ESQ_TICK_S; }
/* FC DAC -> cutoff: AD7524 (19.5 mV/step) -> 20 k into 1 k || 56 k to -5 V -> Vf = 0.913 mV/step - 83.6 mV;
 * Gm = 200 uS exp(Vf / 28.5 mV), fc = Gm / (2 pi 33 nF) */
float esq_fc_hz(int dac) { return 51.3f * expf(0.03204f * (float)esq_clampi(dac, 0, 255)); }
#endif
#if ESQ_FIXED
int esq_fc_hz_i(int dac) { return esq_q_fchz[esq_clampi(dac, 0, 255)]; }
#else
int esq_fc_hz_i(int dac) { return (int)lrintf(esq_fc_hz(dac)); }
#endif

int esq_fc_dac(int freq, int mod)
{
    int f = esq_clampi(freq * 256 + mod, 0, 127 * 256 + 255);
    return esq_clampi((f >> 7) + ESQ_FC_TUNE, 0, 255);
}

#if ESQ_WHOLE
int esq_doc_volume(int index)
{
    if (!esq_tables_ready)
        esq_tables_init();
    return esq_docvol[esq_clampi(index, 0, 127)];
}
#endif

#if ESQ_WHOLE
int esq_vca_dac(int index)
{
    if (!esq_tables_ready)
        esq_tables_init();
    return esq_vcacurve[esq_clampi(index, 0, 127)];
}
#endif

/* ------------------------------------------------------------- envelopes --- */

static inline int esq_env_lv(const esq_env_t *p) { return (p->lv & 63) * 4 + (p->lv ? 2 : 0); }

/* velocity scale of the levels (0..255) and the key / velocity time offsets */
static int esq_env_vscale(const esq_env_t *p, const esq_voice_t *v)
{
    int lv = esq_env_lv(p), v95 = (esq_envvel[v->vel & 127] * 2) & 255;
    return (((lv * v95) >> 8) - lv - 1) & 255;
}

ESQ_SHARED int esq_env_level(const esq_env_t *p, const esq_voice_t *v, int l)
{
    return esq_asr8(esq_clampi(l, -63, 63) * 2 * esq_env_vscale(p, v));
}

static int esq_env_tk(const esq_env_t *p, const esq_voice_t *v)
{
    int key = v->note > 36 ? (v->note - 36) * 2 : 0;
    return ((p->tk & 63) * (key > 255 ? 255 : key)) >> 8;
}

static void esq_env_seg(esq_envst_t *e, int stage, int t, int target)
{
    int cur = e->level >> 8, d = target - cur;
    t = esq_clampi(t, 0, 63);
    e->stage = (uint8_t)stage;
    e->target = (int8_t)target;
    e->count = esq_ticks[t];
    e->rate = d == 0 ? 0 : d > 0 ? esq_seg_rate(d, t) : (int16_t)-esq_seg_rate(-d, t);
}

ESQ_COLD static void esq_env_start(esq_envst_t *e, const esq_env_t *p, const esq_voice_t *v, int from_zero)
{
    int v95 = (esq_envvel[v->vel & 127] * 2) & 255;
    int t1 = (p->t1 & 63) - (((p->t1v & 63) * v95) >> 8);
    if (from_zero)
        e->level = 0;
    esq_env_seg(e, 1, t1 < 0 ? 0 : t1, esq_env_level(p, v, p->l1));
}

static void esq_env_release(esq_envst_t *e, const esq_env_t *p)
{
    esq_env_seg(e, 5, p->t4 & 63, 0);
}

ESQ_COLD static void esq_env_start(esq_envst_t *e, const esq_env_t *p, const esq_voice_t *v, int from_zero);
static void esq_env_tick(esq_envst_t *e, const esq_env_t *p, const esq_voice_t *v, int cycle, int loop)
{
    int lvl, t;
    if (e->stage == 0 || e->stage == 4)
        return;
    lvl = e->level + e->rate;
    if ((e->rate > 0 && lvl > e->target * 256) || (e->rate < 0 && lvl < e->target * 256))
        lvl = e->target * 256;
    e->level = (int16_t)lvl;
    if (--e->count)
        return;
    e->level = (int16_t)(e->target * 256);
    switch (e->stage) {
    case 1:
        t = (p->t2 & 63) - esq_env_tk(p, v);
        esq_env_seg(e, 2, t < 0 ? 0 : t, esq_env_level(p, v, p->l2));
        break;
    case 2:
        t = (p->t3 & 63) - esq_env_tk(p, v);
        esq_env_seg(e, 3, t < 0 ? 0 : t, esq_env_level(p, v, p->l3));
        break;
    case 3:
        if (loop && v->gate)
            esq_env_start(e, p, v, 0);                     /* (R5) LOOP: segment 1 again, from where it is */
        else if (e->target == 0)
            e->stage = 0;                                  /* sustain level 0: done */
        else if (cycle)
            esq_env_release(e, p);                         /* CYC: no sustain */
        else
            e->stage = 4;
        break;
    default:
        e->stage = 0;                                      /* release done */
        break;
    }
}

/* ------------------------------------------------------------- LFOs --- */

ESQ_COLD static void esq_lfo_start(esq_lfost_t *l, const esq_lfo_t *p)
{
    l->level = (int16_t)((p->l1 & 63) * 2);
    l->rate = (int16_t)(p->freq & 63);
    l->mask = (uint8_t)(esq_lfo_slow[l->rate <= 6 ? l->rate : 0] | (p->human ? 0x80 : 0));
    if (p->reset)
        l->phase = 0;
    l->ph16 = (uint16_t)(l->phase << 8);                 /* (R5) SYNC starts where the phase is; a one-shot runs again */
    l->done = 0;
}

/* (R5) SYNC: a note value in twelfths of a beat (4/1 .. 1/16, 1/4T, 1/8T) */
static const uint8_t esq_sync_units[ESQ_NSYNC] = {0, 192, 96, 48, 24, 12, 6, 3, 8, 4};
static int8_t esq_rand8(uint32_t *r) { *r = *r * 1664525u + 1013904223u; return (int8_t)(*r >> 24); }   /* (R5's own:
                                                          * the original's random sequences stay as they were) */
/* sh: ESQ_LSH_* (0: the WAV field's); sync: 0 FREQ, else ESQ_NSYNC's; tempo BPM; one: one-shot; rofs: a rate offset */
static void esq_lfo_tick(esq_lfost_t *l, const esq_lfo_t *p, int src, uint32_t sh, uint32_t sync, uint32_t tempo,
                         uint32_t one, int rofs, uint32_t *rng)
{
    int target = (p->l2 & 63) * 2, step = p->delay & 63, lev, w;
    uint32_t was = l->phase, wrapped;
    /* the L1 -> L2 ramp, then HUMAN's drift */
    if (l->level != target) {
        if (step) {
            int nl = l->level < target ? l->level + step : l->level - step;
            l->level = (int16_t)((l->level < target) == (nl < target) && nl != target ? nl : target);
        }
    } else if (p->human) {
        l->ctr++;
        l->rate = (int16_t)esq_clampi(l->rate + esq_drift[l->ctr], 0, 70);
    }
    lev = esq_clampi(l->level + (src > 0 ? src : 0), 0, 255);
    if (l->done)
        ;                                                 /* (a one-shot that has run: held at its end) */
    else if (sync && sync < ESQ_NSYNC && tempo) {         /* (R5) SYNC: 65536 a cycle, a 10 ms tick's share of it */
        l->ph16 = (uint16_t)(l->ph16 + (tempo * 131072u) / (esq_sync_units[sync] * 1000u));
        l->phase = (uint8_t)(l->ph16 >> 8);
    } else {
        int r = esq_clampi(l->rate + rofs, 0, 70);
        if (r > 6)
            l->phase = (uint8_t)(l->phase + r - 6);
        else {
            int c = l->mask & 1;
            l->mask = (uint8_t)((l->mask & 0x80) | (c << 6) | ((l->mask & 0x7F) >> 1));
            l->phase = (uint8_t)(l->phase + c);
        }
    }
    wrapped = l->phase < was;
    if (wrapped && one) {                                 /* (R5) ONE-SHOT: the cycle's end, held */
        l->done = 1;
        l->phase = 255;
    }
    if (wrapped)
        switch (sh) {                                     /* (R5) the random shapes: a new value a cycle */
        case ESQ_LSH_SH: l->r0 = esq_rand8(rng); break;
        case ESQ_LSH_SMOOTH: l->r0 = l->r1; l->r1 = esq_rand8(rng); break;
        case ESQ_LSH_DRUNK: l->r0 = l->r1; l->r1 = (int8_t)esq_clampi(l->r1 + (esq_rand8(rng) >> 2), -127, 127); break;
        default: break;
        }
    if (sh) {
        int a = (int)l->phase - 128;
        switch (sh) {
        case ESQ_LSH_SINE: w = esq_clampi(-(a * (128 - (a < 0 ? -a : a))) / 32, -127, 127); break;
        case ESQ_LSH_RAMPDN: w = 127 - (int)l->phase; break;
        case ESQ_LSH_SH: w = l->r0; break;
        default: w = l->r0 + (((l->r1 - l->r0) * (int)l->phase) >> 8); break;   /* SMOOTH, DRUNK */
        }
        l->out = (int8_t)esq_asr8(w * lev);
        return;
    }
    switch (p->wave & 3) {
    case ESQ_LFO_TRI: {
        int a = (int8_t)l->phase;
        w = ((a < 0 ? ~a : a) - 64) * 2;
        break;
    }
    case ESQ_LFO_SAW: w = (int8_t)(uint8_t)(l->phase + 128); break;
    case ESQ_LFO_SQR: w = l->phase < 128 ? 0 : 127; break;
    default: w = esq_noise[l->phase]; break;
    }
    l->out = (int8_t)esq_asr8(w * lev);
}

/* ------------------------------------------------------------- the 10 ms update --- */

/* the DOC frequency register for pitch (1/256 semitone; the OS steps it by 1/32) on zone z: integer in both builds,
 * from 2^(i/384) (esq_q_exp, Q31) and 2 x 8.1758 Hz x 65536 / DOC rate (Q20) */
#if ESQ_ZONE_INPLACE
static uint32_t esq_doc_inc(const esq_synth_t *s, int pitch, const esq_zone_t *z, uint32_t zr)
#else
static uint32_t esq_doc_inc(const esq_synth_t *s, int pitch, const esq_zone_t *z)
#endif
{
    int32_t n = pitch >> 3, o, r;
    uint64_t q;
    n = n < 0 ? 0 : n;
    o = n / 384;
    r = n - o * 384;                                      /* octaves and 1/384 octave (1/32 semitone) above 8.18 Hz */
    o = o > 19 ? 19 : o;
#if ESQ_FIXED
    (void)s;
    q = ESQ_UMUL64(esq_q_exp[r], ESQ_Q_INC_K) >> (20 - o);
#else
    q = ((uint64_t)esq_q_exp[r] * s->inc_k) >> (20 - o);
#endif
    {                                                     /* / cycles: x 2^46 / cycles >> 46, no 64-bit divide */
#if ESQ_ZONE_INPLACE
        uint32_t r = zr;
        (void)z;
#else
        uint32_t r = z->rcp ? z->rcp : esq_zone_rcp(z->cycles_q16);   /* (32 bits: esq_zone_rcp) */
#endif
        int sh = 0;
        while (q >> 32) {
            q >>= 1;
            sh++;
        }
        q = ESQ_UMUL64((uint32_t)q, r) >> (46 - sh);
    }
    return q > 0x80000000u ? 0x80000000u : (uint32_t)q;
}

static inline int esq_mod(const esq_mod_t *m, const int *src, int scale)
{
    return src[m->src & 15] * esq_clampi(m->amt, -63, 63) * scale;
}

/* (R5) an extension source (ESQ_XS_*): 0 OFF, 1..15 the ESQ-1's, then RAND, ALT, RR4, ZONE */
static int esq_xsrc(const esq_synth_t *s, const esq_voice_t *v, const int *src, uint32_t x)
{
    if (x == 0 || x >= ESQ_NXS)
        return 0;
    if (x < 16u)
        return src[x - 1u];
    switch (x) {
    case ESQ_XS_RAND: return v->xrnd;
    case ESQ_XS_ALT: return v->xalt;
    case ESQ_XS_RR4: return v->xrr;
    default: return v->note >= s->patch.x.zone_key ? 127 : 0;   /* ZONE */
    }
}

static void esq_voice_tick(esq_synth_t *s, esq_voice_t *v)
{
    const esq_patch_t *p = &s->patch;
    int src[ESQ_NSRC], e, o, d, f, idx, xm[ESQ_NXD], pdrift = 0;
    s->ticks++;                                           /* (v6.6: per synth: two cores may tick two parts at once) */
    /* the key (with the glide still to go), not the host's pitch modulation: the OS picks a wave's zone (and KYBD) from
     * the note + transpose, so vibrato, bend, tune or DRIFT's wander never flip a multisampled wave between zones */
    int note = esq_clampi((v->note * 256 + v->gnote - v->gtarget) >> 8, 0, 127);

    for (e = 0; e < 4; e++)
        esq_env_tick(&v->env[e], &p->env[e], v, p->modes.cycle, (p->x.env_loop >> e) & 1);
    if (v->gnote != v->gtarget) {                         /* glide: linear, a constant time for any interval */
        int g = v->gnote < v->gtarget ? v->gnote + v->grate : v->gnote - v->grate;
        v->gnote = (v->gnote < v->gtarget) == (g < v->gtarget) && g != v->gtarget ? g : v->gtarget;
        note = esq_clampi((v->note * 256 + v->gnote - v->gtarget) >> 8, 0, 127);
    }
    for (e = 0; e < 3; e++)
        src[ESQ_SRC_LFO1 + e] = v->lfo[e].out;            /* last update's, for LFO -> LFO depth */
    for (e = 0; e < 4; e++)
        src[ESQ_SRC_ENV1 + e] = v->env[e].level >> 8;
    src[ESQ_SRC_VEL] = v->vel;
    src[ESQ_SRC_VEL2] = esq_vel2[v->vel & 127];
    src[ESQ_SRC_KYBD] = note;
    src[ESQ_SRC_KYBD2] = esq_clampi(note - 64, -32, 31) * 4;
    src[ESQ_SRC_WHEEL] = s->wheel;
    src[ESQ_SRC_PEDAL] = s->pedal;
    src[ESQ_SRC_PRESS] = s->press;
    src[ESQ_SRC_XCTRL] = s->xctrl;
    src[ESQ_SRC_OFF] = 0;
    for (e = 0; e < 3; e++) {
        esq_lfo_tick(&v->lfo[e], &p->lfo[e], src[p->lfo[e].mod_src & 15], p->x.lfo_shape[e] < ESQ_NLSH ? p->x.lfo_shape[e] : 0u,
                     p->x.lfo_sync[e], s->tempo, (p->x.lfo_oneshot >> e) & 1u, e == 0 ? v->xlfo1 : 0, &s->xr);
        src[ESQ_SRC_LFO1 + e] = v->lfo[e].out;
    }
    {   /* (R5) the two extra modulations, by destination; DRIFT: the card's own tuning and cutoff, a slow wander */
        const esq_ext_t *x = &p->x;
        int k;
        for (k = 0; k < ESQ_NXD; k++)
            xm[k] = 0;
        for (k = 0; k < 2; k++)
            if (x->xm_dst[k] && x->xm_dst[k] < ESQ_NXD)
                xm[x->xm_dst[k]] += esq_xsrc(s, v, src, x->xm_src[k]) * esq_clampi(x->xm_amt[k], -63, 63);
        v->xlfo1 = (int8_t)(esq_asr8(xm[ESQ_XD_LFO1]) / 2);   /* (+-15 rate steps; next update's) */
        if (x->drift) {
            v->wander = (int8_t)esq_clampi(v->wander + (esq_rand8(&s->xr) >> 4), -127, 127);
            pdrift = ((v->card_p * x->drift) >> 8) + ((v->wander * x->drift) >> 9);   /* +-12 cents, +-6 */
        }
        v->fc_drift = (int16_t)((v->card_f * x->drift) >> 5);   /* +- a semitone at 63 */
    }

    /* the DOC: frequency and volume registers */
    for (o = 0; o < 3; o++) {
        const esq_osc_t *op = &p->osc[o];
        const esq_dca_t *dp = &p->dca[o];
        const esq_zone_t *z;
        /* 1/256 semitone: the modulators are source x (2 x amount) / 2 */
        int pitch = v->gnote + op->semi * 256 + (op->fine & 31) * 8 + esq_mod(&op->fm[0], src, 1) +
                    esq_mod(&op->fm[1], src, 1) + s->master_tune * 4 + s->bend +
                    v->detune + pdrift + xm[ESQ_XD_PITCH] + ((s->micro[v->note % 12] * 656) >> 8);   /* (R5) */
        int sc = (esq_xsrc(s, v, src, p->x.scan_src[o]) * esq_clampi(p->x.scan_amt[o], -63, 63) + xm[ESQ_XD_SCAN]) * 2;
        int wv = op->wave + (sc >= 0 ? sc >> 10 : -((-sc) >> 10));   /* (R5) wave scanning: +-15 waves */
        wv = esq_clampi(wv, 0, s->bank->nwaves ? (int)s->bank->nwaves - 1 : 0);
        z = esq_wave_zone(s->bank, (uint32_t)wv, note + op->semi);
        if (z->flags & ESQ_ZONE_ONESHOT)
            v->oneshot |= (uint8_t)(1u << o);
        else
            v->oneshot &= (uint8_t)~(1u << o);
        if (!((v->halted >> o) & 1u)) {                   /* (a one-shot played to its end stays silent) */
            v->tab[o] = s->bank->data + z->offset;
            v->shift[o] = (uint8_t)(32 - z->log2len);
#if ESQ_ZONE_INPLACE                                      /* (the zones read in place: their reciprocal kept here) */
            if (v->rz[o] != z) {
                v->rz[o] = z;
                v->rcp[o] = esq_zone_rcp(z->cycles_q16);
            }
            v->inc[o] = esq_doc_inc(s, pitch, z, v->rcp[o]);
#else
            v->inc[o] = esq_doc_inc(s, pitch, z);
#endif
        }
        d = (esq_mod(&dp->am[0], src, 2) + esq_mod(&dp->am[1], src, 2)) * 2 + xm[ESQ_XD_LEVEL] * 4;
        idx = d > 32767 ? 127 : d < -32768 ? 0 : esq_clampi(dp->level * 2 + esq_asr8(d), 0, 127);
        v->vol[o] = (v->vol[o] + (dp->on ? esq_docvol[idx] : 0u)) >> 1;   /* averaged with the last value */
    }

    /* final VCA: ENV4 x MIDI volume x DCA4 through the OS's curve, a quarter of the way per CV step */
    e = v->env[3].level >> 8;
    idx = ((((e > 0 ? e * 2 : 0) * s->volume) >> 8) * (esq_clampi(p->dca4.level, 0, 63) * 4 + 3)) >> 8;
    v->vca_slope = (esq_vcacurve[idx > 127 ? 127 : idx] * 256 - (v->vca_acc & ~255)) / 4;

    /* filter: FREQ + two modulators + keyboard, x2 + the autotune offset */
    d = esq_mod(&p->flt.mod[0], src, 2) + esq_mod(&p->flt.mod[1], src, 2) + esq_clampi(p->flt.keybd, 0, 63) * 4 * note +
        xm[ESQ_XD_CUTOFF] * 2;
    f = esq_fc_dac(p->flt.freq & 127, d) + (((s->cut_offset + v->fc_drift) * 461) >> 16);   /* 0.555 semitone a DAC step */
    v->fc_slope = (esq_clampi(f, 0, 255) * 256 - (v->fc_acc & ~255)) / 4;

    /* pan: averaged with the last; Q: direct */
    f = esq_clampi((p->dca4.pan & 15) * 16 + 2 * esq_asr8(esq_mod(&p->dca4.panmod, src, 2) + xm[ESQ_XD_PAN] * 2) +
                   v->pan_bias, 0, 255);
    v->pan_dac = (uint8_t)((f + v->pan_new) >> 1);
    v->pan_new = (uint8_t)f;
    v->q_dac = (uint8_t)esq_clampi((p->flt.q & 31) * 4 +   /* (R5) Q modulation */
                                   esq_asr8((esq_xsrc(s, v, src, p->x.q_src) * esq_clampi(p->x.q_amt, -63, 63) + xm[ESQ_XD_Q]) * 2), 0, 127);
}

/* ------------------------------------------------------------- the 2.5 ms CV step --- */

static void esq_voice_cv(esq_synth_t *s, esq_voice_t *v)
{
#if ESQ_FIXED
    int32_t G, G2, G3, G4, ig, k, kin, kn;
    (void)s;
    v->vca_acc = esq_clampi(v->vca_acc + v->vca_slope, 0, 0xFFFF);
    v->fc_acc = esq_clampi(v->fc_acc + v->fc_slope, 0, 0xFFFF);
    v->vg_t = esq_q_vg[v->vca_acc >> 8];
    v->pl_t = esq_q_panl[v->pan_dac];
    v->pr_t = esq_q_panr[v->pan_dac];
    G = esq_q_fcg[v->fc_acc >> 8];                        /* g / (1 + g), Q30 */
    G2 = esq_m30(G, G);
    G3 = esq_m30(G2, G);
    G4 = esq_m30(G2, G2);
    ig = (1 << 30) - G;
    k = esq_q_k[(v->q_dac >> 2) & 31];                    /* resonance VCA: self-oscillation at 2.8 V, Q28 */
    kin = (1 << 28) + esq_m30(k, ESQ_Q_RES_COMP);
    kn = (int32_t)((0xFFFFFFFFu / (uint32_t)(((1 << 30) + (ESQ_MUL64(k, G4) >> 28)) >> 16)) << 12);   /* 1 / (1 + k G4),
                                                          * Q30 (a 32-bit divide: 2^-17 of it) */
    v->G = G;
    v->k = k;
    v->a4 = esq_m30(G4, kn);
    v->c3 = esq_m30(esq_m30(G3, ig), kn);
    v->c2 = esq_m30(esq_m30(G2, ig), kn);
    v->c1 = esq_m30(esq_m30(G, ig), kn);
    v->c0 = esq_m30(ig, kn);
    v->kx = ESQ_MSH(kin, ESQ_Q_KX, 30);   /* DAC x 4 -> the filter's input (Q24 << 16), with kin */
#else
    float g, Vq;
    int fc;
    v->vca_acc = esq_clampi(v->vca_acc + v->vca_slope, 0, 0xFFFF);
    v->fc_acc = esq_clampi(v->fc_acc + v->fc_slope, 0, 0xFFFF);
    v->vg_t = s->vca_gain[v->vca_acc >> 8];
    v->pl_t = s->pan_gl[v->pan_dac];
    v->pr_t = s->pan_gr[v->pan_dac];
    fc = v->fc_acc >> 8;
    g = s->fc_g[fc];
    v->G = g / (1.0f + g);
    v->G2 = v->G * v->G;
    v->G3 = v->G2 * v->G;
    v->G4 = v->G2 * v->G2;
    Vq = (float)v->q_dac * 0.0195f;                       /* resonance VCA: self-oscillation at 2.8 V */
    v->k = 4.0f * Vq / 2.8f;
    v->kin = 1.0f + ESQ_RES_COMP * v->k;
    v->knorm = 1.0f / (1.0f + v->k * v->G4);
#endif
}

/* ------------------------------------------------------------- audio --- */

static const uint8_t esq_silent[2] = {128, 128};   /* a halted oscillator's table (the DAC's centre) */
/* one-shot oscillators that wrapped this frame: halted (silent, frozen) until the next note-on */
ESQ_HOTINL void esq_doc_halt(esq_voice_t *v, const uint32_t *was)
{
    uint32_t o;
    for (o = 0; o < 3u; o++)
        if (((v->oneshot >> o) & 1u) && v->ph[o] < was[o]) {
            v->halted |= (uint8_t)(1u << o);
            v->ph[o] = 0;
            v->inc[o] = 0;
            v->tab[o] = esq_silent;
            v->shift[o] = 31;
        }
}
ESQ_HOTINL void esq_doc_frame(esq_voice_t *v, uint32_t sync, uint32_t am)
{
    uint32_t p0 = v->ph[0] + v->inc[0], wrapped = p0 < v->ph[0];
    int32_t s0, acc;
    uint32_t was[3] = {v->ph[0], v->ph[1], v->ph[2]};
    v->ph[0] = p0;
    if (sync && wrapped)
        v->ph[1] = 0;
    else
        v->ph[1] += v->inc[1];
    v->ph[2] += v->inc[2];
    if (v->oneshot) {                                     /* (OSC2 synced: not its own end) */
        if (sync && wrapped)
            was[1] = 0;
        esq_doc_halt(v, was);
    }
    s0 = v->tab[0][v->ph[0] >> v->shift[0]];
    acc = (s0 - 128) * (int32_t)v->vol[0];
    if (am)                                               /* OSC1's sample is OSC2's volume */
        acc += (v->tab[1][v->ph[1] >> v->shift[1]] - 128) * s0;
    else if (v->vol[1])                                   /* (an oscillator at volume 0: its phase runs on, no read) */
        acc += (v->tab[1][v->ph[1] >> v->shift[1]] - 128) * (int32_t)v->vol[1];
    if (v->vol[2])
        acc += (v->tab[2][v->ph[2] >> v->shift[2]] - 128) * (int32_t)v->vol[2];
    v->doc_out = acc;
}

/* the DOC frames in the next m samples, from nb (Q24 samples to the next frame; period: samples per frame): ticks[j]
 * frames fall in sample j (the interval since sample j - 1), at pos[j][t] / 128 of it (a third or later: the second's) */
static void esq_doc_schedule(uint32_t *nb, uint32_t period, uint32_t m, uint8_t *ticks, uint8_t (*pos)[2])
{
    uint32_t j;
    for (j = 0; j < m; j++) {
        uint8_t t = 0;
        while (*nb <= (1u << 24)) {
            if (t < 2)
                pos[j][t] = (uint8_t)(*nb >> 17);
            t = (uint8_t)(t + 1);
            *nb += period;
        }
        ticks[j] = t;
        *nb -= 1u << 24;
    }
}

#if ESQ_FIXED
/* m samples (at most ESQ_CHUNK) of one voice with fixed CV: adds (x gain / 4, the per-voice API; or panned, Q24,
 * when or_). Three passes over the run (DOC and hold, filter, VCA and OTA): fewer live values each. qhw, qhe: the
 * hold tables (esq_q_hw, esq_q_he, or their RAM copies) */
ESQ_HOTINL void esq_voice_run_in(const esq_synth_t *s, esq_voice_t *v, const uint8_t *ticks, const uint8_t (*pos)[2],
                                 uint32_t m, int32_t *ol, int32_t *or_, int32_t gain, const int16_t *qhw,
                                 const int16_t *qhe)
{
    int32_t buf[ESQ_CHUNK];
    uint32_t j, t;
    {   /* the DOC and the hold capacitor: exact for the DAC's steps, averaged over each sample (DAC x 4); at 44.1 kHz
         * a sample holds at most one DOC frame (more only for a host's faster DOC) */
        uint32_t sync = s->patch.modes.sync, am = s->patch.modes.am;
        int32_t h = v->hold, x = v->hold_x, kx = v->kx;
        /* (SQ1D R5.1) the oscillators' state in locals: the wave tables are bytes, which the compiler must assume
         * alias the voice, so through v every frame reloaded it. One-shot zones keep the frame as it was (halts) */
        const int fast = !v->oneshot;
        uint32_t p0 = v->ph[0], p1 = v->ph[1], p2 = v->ph[2];
        const uint32_t i0 = v->inc[0], i1 = v->inc[1], i2 = v->inc[2];
        const uint8_t *t0 = v->tab[0], *t1 = v->tab[1], *t2 = v->tab[2];
        const uint32_t s0h = v->shift[0], s1h = v->shift[1], s2h = v->shift[2];
        const int32_t v0 = (int32_t)v->vol[0], v1 = (int32_t)v->vol[1], v2 = (int32_t)v->vol[2];
        for (j = 0; j < m; j++) {
            int32_t c = 0, d, e, al = 0, ae = 0;
            for (t = 0; t < ticks[j]; t++) {
                c = pos[j][t ? 1 : 0];
                d = t == 0 ? c : t == 1 ? c - pos[j][0] : 0;
                e = h - x;
                al += d * x;
                ae += (qhw[d] * e) >> 11;
                h = x + ((qhe[d] * e) >> 11);
                if (fast) {                       /* esq_doc_frame, without one-shots */
                    uint32_t n0 = p0 + i0;
                    int32_t a, sm0;
                    p1 = sync && n0 < p0 ? 0u : p1 + i1;
                    p0 = n0;
                    p2 += i2;
                    sm0 = t0[p0 >> s0h];
                    a = (sm0 - 128) * v0;
                    if (am)
                        a += (t1[p1 >> s1h] - 128) * sm0;
                    else if (v1)
                        a += (t1[p1 >> s1h] - 128) * v1;
                    if (v2)
                        a += (t2[p2 >> s2h] - 128) * v2;
                    x = a * 4;
                } else {
                    esq_doc_frame(v, sync, am);
                    x = v->doc_out * 4;
                }
            }
            d = ESQ_HOLD_SUB - c;
            e = h - x;
            al += d * x;
            ae += (qhw[d] * e) >> 11;
            h = x + ((qhe[d] * e) >> 11);
            buf[j] = ESQ_MSH((al >> 7) + ae, kx, 16);   /* -> the CEM3379's input, Q24 of its clip */
            ESQ_TAP_X(v, buf[j]);
        }
        v->hold = h, v->hold_x = x;
        if (fast) {
            v->ph[0] = p0, v->ph[1] = p1, v->ph[2] = p2;
            v->doc_out = x / 4;
        }
    }
    {   /* CEM3379: compensated resonance, input clip, 4 poles, x7.5 into its 5 Vpp swing */
        int32_t f0 = v->fs[0], f1 = v->fs[1], f2 = v->fs[2], f3 = v->fs[3];
        int32_t G = v->G, k = v->k, a4 = v->a4, c3 = v->c3, c2 = v->c2, c1 = v->c1, c0 = v->c0;
        for (j = 0; j < m; j++) {
            int32_t xin = buf[j], y4, u, w, y;
            y4 = esq_m30(a4, xin) + esq_m30(c3, f0) + esq_m30(c2, f1) + esq_m30(c1, f2) + esq_m30(c0, f3);
            u = esq_tanh_q(xin - esq_m28(k, y4));
            w = esq_m30(u - f0, G); y = w + f0; f0 = y + w;
            w = esq_m30(y - f1, G); y = w + f1; f1 = y + w;
            w = esq_m30(y - f2, G); y = w + f2; f2 = y + w;
            w = esq_m30(y - f3, G); y = w + f3; f3 = y + w;
            buf[j] = esq_tanh_q(esq_m28(y, ESQ_Q_YGAIN));
        }
        v->fs[0] = f0, v->fs[1] = f1, v->fs[2] = f2, v->fs[3] = f3;
    }
    {   /* 3.3 uF into the VCA (8 bits more: it settles), its input limit, the pan OTA's input saturation */
        int64_t hp = v->hp;
        int32_t vg = v->vg, vg_t = v->vg_t;
        if (or_) {
            int32_t pl = v->pl, pr = v->pr, pl_t = v->pl_t, pr_t = v->pr_t;
            if (vg == vg_t && pl == pl_t && pr == pr_t) {   /* (R5.1) settled: their steps are all 0, same output */
                for (j = 0; j < m; j++) {
                    int32_t i = buf[j] - (int32_t)(hp >> 8), o;
                    hp += ESQ_MUL64(i, ESQ_Q_HA) >> 23;
                    i = buf[j] - (int32_t)(hp >> 8);
                    i = i > ESQ_Q_IMAX ? ESQ_Q_IMAX : i < -ESQ_Q_IMAX ? -ESQ_Q_IMAX : i;
                    o = esq_tanh_q(esq_m28(i, vg));
                    ol[j] += ESQ_MSH(o, pl, 17);
                    or_[j] += ESQ_MSH(o, pr, 17);
                }
            } else
            for (j = 0; j < m; j++) {
                int32_t i = buf[j] - (int32_t)(hp >> 8), o;
                hp += ESQ_MUL64(i, ESQ_Q_HA) >> 23;
                i = buf[j] - (int32_t)(hp >> 8);
                i = i > ESQ_Q_IMAX ? ESQ_Q_IMAX : i < -ESQ_Q_IMAX ? -ESQ_Q_IMAX : i;
                vg += esq_m31(vg_t - vg, ESQ_Q_VA);
                o = esq_tanh_q(esq_m28(i, vg));
                pl += esq_m31(pl_t - pl, ESQ_Q_PA);
                pr += esq_m31(pr_t - pr, ESQ_Q_PA);
                ol[j] += ESQ_MSH(o, pl, 17);   /* 0.25 a saturated voice, panned hard */
                or_[j] += ESQ_MSH(o, pr, 17);
            }
            v->pl = pl, v->pr = pr;
        } else {
            /* mono; with a host's side buffer, also its pan: (L - R) / 2 = out x (gl - gr) / (gl + gr), the pan OTA's
             * gains (their sum is constant: L + R stays the mono level) */
            int32_t *sd = s->side_at, sg = sd ? esq_q_panl[v->pan_dac] - esq_q_panr[v->pan_dac] : 0;
            sg = sg > -256 && sg < 256 ? 0 : sg;          /* (the middle, PAN 8: 0.3 % apart in the table: mono) */
            for (j = 0; j < m; j++) {
                int32_t i = buf[j] - (int32_t)(hp >> 8), o;
                hp += ESQ_MUL64(i, ESQ_Q_HA) >> 23;
                i = buf[j] - (int32_t)(hp >> 8);
                i = i > ESQ_Q_IMAX ? ESQ_Q_IMAX : i < -ESQ_Q_IMAX ? -ESQ_Q_IMAX : i;
                vg += esq_m31(vg_t - vg, ESQ_Q_VA);
                o = esq_tanh_q(esq_m28(i, vg));
                o = ESQ_MSH(o, gain, 26);
                ol[j] += o;
                if (sg)
                    sd[j] += (o * sg) >> 15;
            }
        }
        v->hp = hp, v->vg = vg;
    }
}

static void esq_voice_run(const esq_synth_t *s, esq_voice_t *v, const uint8_t *ticks, const uint8_t (*pos)[2],
                          uint32_t m, int32_t *ol, int32_t *or_, int32_t gain)
{
    esq_voice_run_in(s, v, ticks, pos, m, ol, or_, gain, esq_q_hw, esq_q_he);
}
#if ESQ_RAM
static int16_t esq_q_hwr[ESQ_HOLD_SUB + 1], esq_q_her[ESQ_HOLD_SUB + 1];   /* (RAM copies: esq_init) */
ESQ_RAMFN void esq_voice_run_ram(const esq_synth_t *s, esq_voice_t *v, const uint8_t *ticks,
                                 const uint8_t (*pos)[2], uint32_t m, int32_t *ol, int32_t *or_, int32_t gain)
{
    esq_voice_run_in(s, v, ticks, pos, m, ol, or_, gain, esq_q_hwr, esq_q_her);
}
typedef void esq_run_fn(const esq_synth_t *, esq_voice_t *, const uint8_t *, const uint8_t (*)[2], uint32_t,
                        int32_t *, int32_t *, int32_t);
/* called through a pointer the compiler cannot fold into a direct call: flash code reaches RAM (0x01C0xxxx) only so on
 * the FM-1 (a direct call's 23-bit range ends short of it; Felucca's fm1_flash.h FL_FAR does the same) */
static esq_run_fn *volatile esq_run_ram = esq_voice_run_ram;
int esq_ram_on = 1;
#endif

/* a voice through m samples, stepping its CVs on time; ends it once ENV4 and the VCA are done */
static void esq_voice_block(esq_synth_t *s, esq_voice_t *v, const uint8_t *ticks, const uint8_t (*pos)[2], uint32_t m,
                            int32_t *ol, int32_t *or_, int32_t gain)
{
    uint32_t done = 0;
    while (done < m) {
        uint32_t c;
        if (v->cv_wait <= 0) {
            if (++v->cv_step >= ESQ_CV_STEPS) {
                v->cv_step = 0;
                esq_voice_tick(s, v);
            }
            esq_voice_cv(s, v);
            v->cv_wait += ESQ_Q_FAST_LEN;
        }
        c = (uint32_t)(v->cv_wait + 255) >> 8;
        if (c > m - done)
            c = m - done;
        if (c == 0)
            c = 1;
        {
            int32_t *sa = s->side_at;
            if (sa)
                s->side_at = sa + done;                   /* (esq_voice_run's mono output: its side at this run) */
#if ESQ_RAM
            if (esq_ram_on)
                esq_run_ram(s, v, ticks + done, pos + done, c, ol + done, or_ ? or_ + done : 0, gain);
            else
#endif
            esq_voice_run(s, v, ticks + done, pos + done, c, ol + done, or_ ? or_ + done : 0, gain);
            s->side_at = sa;
            if (sa && (esq_q_panl[v->pan_dac] - esq_q_panr[v->pan_dac] >= 256 ||
                       esq_q_panr[v->pan_dac] - esq_q_panl[v->pan_dac] >= 256))
                s->side_used = 1;
        }
        v->cv_wait -= (int32_t)(c << 8);
        done += c;
    }
    if (!v->gate && v->env[3].stage == 0 && (v->vca_acc >> 8) <= esq_vcacurve[0] && v->vg <= ESQ_Q_VG_END)
        v->active = 0;
}

#if ESQ_WHOLE
void esq_render_q(esq_synth_t *s, int32_t *out_l, int32_t *out_r, uint32_t n)
{
    uint8_t ticks[ESQ_CHUNK], pos[ESQ_CHUNK][2];
    uint32_t i = 0, j;
    int vi;
    while (i < n) {
        uint32_t m = n - i < ESQ_CHUNK ? n - i : ESQ_CHUNK;
        int32_t *ol = out_l + i, *or_ = out_r + i;
        esq_doc_schedule(&s->doc_nb, s->doc_period, m, ticks, pos);
        for (j = 0; j < m; j++)
            ol[j] = or_[j] = 0;
        for (vi = 0; vi < ESQ_NVOICE; vi++)
            if (s->v[vi].active)
                esq_voice_block(s, &s->v[vi], ticks, (const uint8_t(*)[2])pos, m, ol, or_, 0);
        for (j = 0; j < m; j++) {                         /* output stage: 20.8 kHz, 15 Hz high-pass, 15.4 kHz */
            int32_t c[2] = {ol[j], or_[j]};
            int ch;
            for (ch = 0; ch < 2; ch++) {
                int32_t x = c[ch], w;
                w = esq_m30(x - s->o_s1[ch], ESQ_Q_OG1); x = w + s->o_s1[ch]; s->o_s1[ch] = x + w;
                s->o_h[ch] += ((int64_t)(x - (int32_t)(s->o_h[ch] >> 8)) * ESQ_Q_OHP) >> 23;
                x -= (int32_t)(s->o_h[ch] >> 8);
                w = esq_m30(x - s->o_s2[ch], ESQ_Q_OG2); x = w + s->o_s2[ch]; s->o_s2[ch] = x + w;
                c[ch] = s->out_clean ? x * 2 : esq_tanh_q(x * 2);   /* x out_gain 2 */
            }
            ol[j] = c[0];
            or_[j] = c[1];
        }
        i += m;
    }
}

#endif /* ESQ_WHOLE */

#if ESQ_HOST && ESQ_WHOLE
void esq_render(esq_synth_t *s, float *out_l, float *out_r, uint32_t n)
{
    int32_t l[ESQ_CHUNK], r[ESQ_CHUNK];
    uint32_t i = 0, j;
    while (i < n) {
        uint32_t m = n - i < ESQ_CHUNK ? n - i : ESQ_CHUNK;
        esq_render_q(s, l, r, m);
        for (j = 0; j < m; j++) {
            out_l[i + j] = (float)l[j] * (1.0f / 16777216.0f);
            out_r[i + j] = (float)r[j] * (1.0f / 16777216.0f);
        }
        i += m;
    }
}
#endif

#else /* float */

/* m samples of one voice with fixed CV: adds (x gain, panned when or_) to ol / or_ */
static void esq_voice_run(const esq_synth_t *s, esq_voice_t *v, const uint8_t *ticks, const uint8_t (*pos)[2],
                          uint32_t m, float *ol, float *or_, float gain)
{
    uint32_t j, t, sync = s->patch.modes.sync, am = s->patch.modes.am;
    float f0 = v->fs[0], f1 = v->fs[1], f2 = v->fs[2], f3 = v->fs[3], h = v->hold, x = v->hold_x, hp = v->hp;
    float G = v->G, G2 = v->G2, G3 = v->G3, G4 = v->G4, k = v->k, kin = v->kin, kn = v->knorm, ig = 1.0f - G;
    float va = s->vca_a, pa = s->pan_a, ha = s->hp_a;
    float vg = v->vg, vg_t = v->vg_t, pl = v->pl, pr = v->pr, pl_t = v->pl_t, pr_t = v->pr_t;
    for (j = 0; j < m; j++) {
        float al = 0.0f, ae = 0.0f, e, xin, y4, u, w, y, i, o;
        int c = 0, d;
        /* the hold capacitor (a 3 kHz one-pole): exact for the DAC's steps, averaged over the sample */
        for (t = 0; t < ticks[j]; t++) {
            c = pos[j][t ? 1 : 0];
            d = t == 0 ? c : t == 1 ? c - pos[j][0] : 0;
            e = h - x;
            al += (float)d * x;
            ae += s->hold_w[d] * e;
            h = x + s->hold_e[d] * e;
            esq_doc_frame(v, sync, am);
            x = (float)v->doc_out * ESQ_DOC_SCALE;
        }
        d = ESQ_HOLD_SUB - c;
        e = h - x;
        al += (float)d * x;
        ae += s->hold_w[d] * e;
        h = x + s->hold_e[d] * e;
        xin = al * (1.0f / ESQ_HOLD_SUB) + ae;
        ESQ_TAP_X(v, xin);
        xin *= kin;                                       /* CEM3379: compensated resonance, input clip */
        y4 = (G4 * xin + (G3 * f0 + G2 * f1 + G * f2 + f3) * ig) * kn;
        u = ESQ_VCF_CLIP * esq_tanh_fast((xin - k * y4) * (1.0f / ESQ_VCF_CLIP));
        w = (u - f0) * G; y = w + f0; f0 = y + w;
        w = (y - f1) * G; y = w + f1; f1 = y + w;
        w = (y - f2) * G; y = w + f2; f2 = y + w;
        w = (y - f3) * G; y = w + f3; f3 = y + w;
        y = ESQ_VCF_SWING * esq_tanh_fast(y * (ESQ_VCF_GAIN / ESQ_VCF_SWING));
        hp += (y - hp) * ha;                              /* 3.3 uF into the VCA */
        i = esq_clampf((y - hp) * (1.0f / ESQ_VCA_RIN), -ESQ_VCA_IMAX, ESQ_VCA_IMAX);
        vg += (vg_t - vg) * va;
        o = ESQ_OTA_VT * esq_tanh_fast(i * vg * (ESQ_PAN_R / ESQ_OTA_VT)) * ESQ_OUT_NORM * gain;
        if (or_) {
            pl += (pl_t - pl) * pa;
            pr += (pr_t - pr) * pa;
            ol[j] += o * pl;
            or_[j] += o * pr;
        } else
            ol[j] += o;
    }
    v->fs[0] = f0, v->fs[1] = f1, v->fs[2] = f2, v->fs[3] = f3;
    v->hold = h, v->hold_x = x, v->hp = hp;
    v->vg = vg, v->pl = pl, v->pr = pr;
}

/* a voice through m samples, stepping its CVs on time; ends it once ENV4 and the VCA are done */
static void esq_voice_block(esq_synth_t *s, esq_voice_t *v, const uint8_t *ticks, const uint8_t (*pos)[2], uint32_t m,
                            float *ol, float *or_, float gain)
{
    uint32_t done = 0;
    while (done < m) {
        uint32_t c;
        if (v->cv_wait <= 0.0f) {
            if (++v->cv_step >= ESQ_CV_STEPS) {
                v->cv_step = 0;
                esq_voice_tick(s, v);
            }
            esq_voice_cv(s, v);
            v->cv_wait += s->fast_len;
        }
        c = (uint32_t)ceilf(v->cv_wait);
        if (c > m - done)
            c = m - done;
        if (c == 0)
            c = 1;
        esq_voice_run(s, v, ticks + done, pos + done, c, ol + done, or_ ? or_ + done : 0, gain);
        v->cv_wait -= (float)c;
        done += c;
    }
    if (!v->gate && v->env[3].stage == 0 && (v->vca_acc >> 8) <= esq_vcacurve[0] &&
        v->vg <= s->vca_gain[esq_vcacurve[0]] * 1.05f)
        v->active = 0;
}

void esq_render(esq_synth_t *s, float *out_l, float *out_r, uint32_t n)
{
    uint8_t ticks[ESQ_CHUNK], pos[ESQ_CHUNK][2];
    uint32_t i = 0, j;
    int vi;
    while (i < n) {
        uint32_t m = n - i < ESQ_CHUNK ? n - i : ESQ_CHUNK;
        float *ol = out_l + i, *or_ = out_r + i;
        esq_doc_schedule(&s->doc_nb, s->doc_period, m, ticks, pos);
        for (j = 0; j < m; j++)
            ol[j] = or_[j] = 0.0f;
        for (vi = 0; vi < ESQ_NVOICE; vi++)
            if (s->v[vi].active)
                esq_voice_block(s, &s->v[vi], ticks, (const uint8_t(*)[2])pos, m, ol, or_, 1.0f);
        for (j = 0; j < m; j++) {                         /* output stage: 20.8 kHz, 15 Hz high-pass, 15.4 kHz */
            float c[2] = {ol[j], or_[j]};
            int ch;
            for (ch = 0; ch < 2; ch++) {
                float x = c[ch], w;
                w = (x - s->o_s1[ch]) * s->o_g1; x = w + s->o_s1[ch]; s->o_s1[ch] = x + w;
                s->o_h[ch] += (x - s->o_h[ch]) * s->o_hp; x -= s->o_h[ch];
                w = (x - s->o_s2[ch]) * s->o_g2; x = w + s->o_s2[ch]; s->o_s2[ch] = x + w;
                c[ch] = esq_tanh_fast(x * s->out_gain);
            }
            ol[j] = c[0];
            or_[j] = c[1];
        }
        i += m;
    }
}
#endif

/* ------------------------------------------------------------- notes --- */

ESQ_COLD static void esq_voice_start(esq_synth_t *s, esq_voice_t *v, int note, int vel, int legato)
{
    const esq_patch_t *p = &s->patch;
    int was = v->active, e, from;
    from = p->modes.glide ? (p->modes.mono ? (legato ? v->gnote >> 8 : note) : (s->last_note >= 0 ? s->last_note : note))
                          : note;
    v->gtarget = note * 256;
    if (legato && p->modes.glide)
        from = -1;                                        /* glide on from where the pitch is */
    if (from >= 0)
        v->gnote = from * 256;
    if (v->gnote != v->gtarget) {
        int d = (v->gtarget - v->gnote) / 256;
        v->grate = esq_seg_rate(d < 0 ? -d : d, p->modes.glide & 63);
    }
    s->last_note = note;
    v->note = (uint8_t)note;
    if (legato)
        return;
    v->active = 1;
    v->gate = 1;
    v->sustained = 0;
    v->vel = (uint8_t)vel;
    v->age = ++s->age_counter;
    if (!was) {
        memset(v->fs, 0, sizeof(v->fs));
        v->hold = v->hold_x = v->hp = v->vg = 0;
        v->vca_acc = esq_vcacurve[0] * 256;
        v->fc_acc = esq_fc_dac(p->flt.freq & 127, 0) * 256;
        v->doc_out = 0;
        v->vol[0] = v->vol[1] = v->vol[2] = 0;
        for (e = 0; e < 4; e++)
            v->env[e].level = 0;
        for (e = 0; e < 3; e++)
            v->lfo[e].phase = (uint8_t)(esq_xrand(&s->rng) >> 24);   /* free-running before this note */
        v->pan_new = (uint8_t)((p->dca4.pan & 15) * 16);
#if ESQ_FIXED
        v->pl = v->pl_t = esq_q_panl[v->pan_new];
        v->pr = v->pr_t = esq_q_panr[v->pan_new];
#else
        v->pl = v->pl_t = s->pan_gl[v->pan_new];
        v->pr = v->pr_t = s->pan_gr[v->pan_new];
#endif
    }
    if (p->modes.osc_reset)
        v->ph[0] = v->ph[1] = v->ph[2] = 0;               /* halted, restarted in phase */
    else if (!was) {
        v->ph[0] = esq_xrand(&s->rng);
        v->ph[1] = esq_xrand(&s->rng);
        v->ph[2] = esq_xrand(&s->rng);
    }
    for (e = 0; e < 3; e++)
        esq_lfo_start(&v->lfo[e], &p->lfo[e]);
    for (e = 0; e < 4; e++)
        esq_env_start(&v->env[e], &p->env[e], v, !(was && !p->modes.env_reset));   /* (ENV reset off: any retrigger
                                                          * of a sounding voice goes on from its level, as the OS does;
                                                          * up to v6.5.3 a new key restarted from 0: a click) */
    {   /* (R5) this note's RAND, ALT, RR4 */
        uint32_t k = s->xcount++;
        v->xrnd = esq_rand8(&s->xr);
        v->xalt = (uint8_t)(k & 1u ? 127 : 0);
        v->xrr = (uint8_t)((k & 3u) * 127u / 3u);
    }
    v->cv_step = 0;                                       /* update now, then every 10 ms */
    v->halted = 0;                                        /* (one-shots play again) */
    esq_voice_tick(s, v);
    for (e = 0; e < 3; e++)                               /* a one-shot starts at its start, whatever OSC reset says */
        if ((v->oneshot >> e) & 1u)
            v->ph[e] = 0;
    esq_voice_cv(s, v);
#if ESQ_FIXED
    v->cv_wait = ESQ_Q_FAST_LEN;
#else
    v->cv_wait = s->fast_len;
#endif
}

ESQ_COLD static void esq_voice_release(esq_synth_t *s, esq_voice_t *v)
{
    int e;
    v->gate = 0;
    if (s->patch.modes.cycle)
        return;                                           /* CYC: the envelopes ignore the key */
    for (e = 0; e < 4; e++)
        if (v->env[e].stage)
            esq_env_release(&v->env[e], &s->patch.env[e]);
}

#if ESQ_WHOLE                             /* (the whole-synth API: a host of the per-voice API has no use for it) */
/* (R6) the voices it may use: VOICES (esq_synth_t.nvoice, 0: all) */
static int esq_nv(const esq_synth_t *s) { return s->nvoice && s->nvoice < ESQ_NVOICE ? s->nvoice : ESQ_NVOICE; }
static esq_voice_t *esq_voice_pick(esq_synth_t *s, int note)
{
    esq_voice_t *best = 0;
    int i, same = 0, nv = esq_nv(s);
    if (s->patch.modes.vc)                                /* VC: the key takes its voice back */
        for (i = 0; i < nv; i++)
            if (s->v[i].active && s->v[i].note == note)
                return &s->v[i];
    for (i = 0; i < nv; i++)                      /* VC off: at most two voices on one key */
        if (s->v[i].active && s->v[i].note == note && ++same >= 2)
            break;
    if (same >= 2) {
        for (i = 0; i < nv; i++)
            if (s->v[i].active && s->v[i].note == note && (!best || s->v[i].age < best->age))
                best = &s->v[i];
        return best;
    }
    for (i = 0; i < nv; i++)
        if (!s->v[i].active)
            return &s->v[i];
    for (i = 0; i < nv; i++)                      /* oldest released */
        if (!s->v[i].gate && (!best || s->v[i].age < best->age))
            best = &s->v[i];
    if (best)
        return best;
    best = &s->v[0];
    for (i = 1; i < nv; i++)                      /* oldest */
        if (s->v[i].age < best->age)
            best = &s->v[i];
    return best;
}

/* (R5) unison: voices a note (1..4), and voice k of n's detune (1/256 semitone) and pan spread */
static uint32_t esq_uni(const esq_synth_t *s)
{
    uint32_t n = s->patch.x.uni, nv = (uint32_t)esq_nv(s);
    n = n < 2u ? 1u : n > 4u ? 4u : n;
    return n > nv ? nv : n;
}
static void esq_uni_set(const esq_synth_t *s, esq_voice_t *v, uint32_t k, uint32_t n)
{
    int sp = 2 * (int)k - (int)(n - 1u), den = n > 1u ? (int)(n - 1u) : 1;
    v->detune = (int16_t)(sp * (int)s->patch.x.uni_det * 256 / (100 * den));
    v->pan_bias = (int16_t)(n > 1u ? sp * 32 / den : 0);
}
/* (R5) a voice for unison voice k: a free one, else the oldest released, else the oldest, none already taken */
static esq_voice_t *esq_voice_pick_uni(esq_synth_t *s, esq_voice_t *const *used, uint32_t nused)
{
    esq_voice_t *best = 0;
    int i, pass;
    for (pass = 0; pass < 3 && !best; pass++)
        for (i = 0; i < esq_nv(s); i++) {
            esq_voice_t *v = &s->v[i];
            uint32_t u, taken = 0;
            for (u = 0; u < nused; u++) taken |= used[u] == v;
            if (taken || (pass == 0 && v->active) || (pass == 1 && v->gate))
                continue;
            if (!best || v->age < best->age)
                best = v;
            if (pass == 0)
                break;
        }
    return best;
}

void esq_note_on(esq_synth_t *s, int note, int vel)
{
    uint32_t n = esq_uni(s), k;
    if (note < 0 || note > 127)
        return;
    if (vel <= 0) {
        esq_note_off(s, note);
        return;
    }
    if (vel > 127)
        vel = 127;
    if (s->patch.modes.mono) {
        esq_voice_t *v = &s->v[0];
        int legato = v->active && v->gate && s->nheld > 0;
        if (s->nheld < ESQ_HELD_MAX)
            s->held[s->nheld++] = (uint8_t)note;
        for (k = 0; k < n; k++) {                         /* (unison: voices 0 .. n - 1 together) */
            esq_uni_set(s, &s->v[k], k, n);
            esq_voice_start(s, &s->v[k], note, vel, legato && s->v[k].active && s->v[k].gate);
        }
        return;
    }
    if (n == 1u) {
        esq_voice_t *v = esq_voice_pick(s, note);
        v->detune = 0;
        v->pan_bias = 0;
        esq_voice_start(s, v, note, vel, 0);
        return;
    }
    {
        esq_voice_t *used[4];
        for (k = 0; k < n; k++) {
            used[k] = esq_voice_pick_uni(s, used, k);
            if (!used[k])
                break;
            esq_uni_set(s, used[k], k, n);
            esq_voice_start(s, used[k], note, vel, 0);
        }
    }
}

void esq_note_off(esq_synth_t *s, int note)
{
    int i;
    if (s->patch.modes.mono) {
        int k, w = 0, u, n = (int)esq_uni(s);
        for (k = 0; k < s->nheld; k++)
            if (s->held[k] != note)
                s->held[w++] = s->held[k];
        s->nheld = (uint8_t)w;
        for (u = 0; u < n; u++) {                         /* (unison: voices 0 .. n - 1) */
            esq_voice_t *v = &s->v[u];
            if (v->active && v->gate && v->note == note) {
                if (s->nheld)
                    esq_voice_start(s, v, s->held[s->nheld - 1], v->vel, 1);
                else if (s->sustain)
                    v->sustained = 1;
                else
                    esq_voice_release(s, v);
            }
        }
        return;
    }
    for (i = 0; i < ESQ_NVOICE; i++)
        if (s->v[i].active && s->v[i].gate && s->v[i].note == note && !s->v[i].sustained) {
            if (s->sustain)
                s->v[i].sustained = 1;
            else
                esq_voice_release(s, &s->v[i]);
        }
}

void esq_all_off(esq_synth_t *s)
{
    int i;
    s->nheld = 0;
    s->sustain = 0;
    for (i = 0; i < ESQ_NVOICE; i++)
        if (s->v[i].active && s->v[i].gate)
            esq_voice_release(s, &s->v[i]);
}
#endif

/* ------------------------------------------------------------- setup, controls --- */

/* (R5) each voice card's own tuning and cutoff offsets (DRIFT scales them), the tempo LFO SYNC starts at */
ESQ_COLD static void esq_cards(esq_synth_t *s)
{
#if ESQ_WHOLE || !ESQ_FIXED
    uint32_t r = 0x51D1u;
    int i;
    for (i = 0; i < ESQ_NVOICE; i++) {
        s->v[i].card_p = esq_rand8(&r);
        s->v[i].card_f = esq_rand8(&r);
    }
#endif
    s->tempo = 120;
}

#if ESQ_FIXED
ESQ_COLD void esq_init(esq_synth_t *s, uint32_t sample_rate, const esq_wavebank_t *bank)
{
    (void)sample_rate;                                    /* (the tables are 44.1 kHz's) */
    if (!esq_tables_ready)
        esq_tables_init();
#if ESQ_RAM
    {
        uint32_t i;
        for (i = 0; i <= ESQ_HOLD_SUB; i++)
            esq_q_hwr[i] = esq_q_hw[i], esq_q_her[i] = esq_q_he[i];
    }
#endif
    memset(s, 0, sizeof(*s));
    s->bank = bank;
    s->rng = 0x2545F491u;
    s->bend_range = 2;
    s->volume = 127;
    s->last_note = -1;
    s->doc_period = ESQ_Q_DOC_PERIOD;
    esq_cards(s);
}
#else
static float esq_onepole_g(float hz, float fs)
{
    float g = tanf(ESQ_PI_F * esq_clampf(hz, 1.0f, 0.45f * fs) / fs);
    return g / (1.0f + g);
}

void esq_set_doc_rate(esq_synth_t *s, float doc_rate)
{
    s->doc_rate = doc_rate;
    s->doc_period = (uint32_t)((double)s->fs / (double)doc_rate * 16777216.0 + 0.5);
    s->inc_k = (uint32_t)llrint(2.0 * 8.17579891564 * 65536.0 / (double)doc_rate * 1048576.0);
}

ESQ_COLD void esq_init(esq_synth_t *s, uint32_t sample_rate, const esq_wavebank_t *bank)
{
    int i;
    float fst;
    if (!esq_tables_ready)
        esq_tables_init();
    memset(s, 0, sizeof(*s));
    s->fs = (float)sample_rate;
    s->bank = bank;
    s->rng = 0x2545F491u;
    s->bend_range = 2;
    s->volume = 127;
    s->out_gain = 2.0f;
    s->last_note = -1;
    s->fast_len = ESQ_TICK_S / ESQ_CV_STEPS * s->fs;
    s->vca_a = 1.0f - expf(-1.0f / (0.002f * s->fs));         /* 20 k / 0.1 uF */
    s->pan_a = 1.0f - expf(-1.0f / (0.0047f * s->fs));        /* 47 k / 0.1 uF */
    s->hp_a = 1.0f - expf(-2.0f * ESQ_PI_F * 6.4f / s->fs);   /* 3.3 uF / 7.5 k */
    s->o_g1 = esq_onepole_g(20800.0f, s->fs);                 /* 5.1 k / 1500 pF */
    s->o_g2 = esq_onepole_g(15400.0f, s->fs);                 /* 47 k / 220 pF */
    s->o_hp = 1.0f - expf(-2.0f * ESQ_PI_F * 15.4f / s->fs);  /* 0.22 uF / 47 k */
    fst = s->fs / (2.0f * ESQ_PI_F * ESQ_HOLD_HZ);            /* the hold's time constant, in samples */
    for (i = 0; i <= ESQ_HOLD_SUB; i++) {                     /* over i / 128 sample: what is left, the average */
        float e = expf(-(float)i / ESQ_HOLD_SUB / fst);
        s->hold_e[i] = e;
        s->hold_w[i] = fst * (1.0f - e);
    }
    for (i = 0; i < 256; i++) {
        float fc = esq_fc_hz(i), V = (float)i * 0.0195f, x;
        s->fc_g[i] = tanf(ESQ_PI_F * esq_clampf(fc, 5.0f, 0.45f * s->fs) / s->fs);
        /* CEM3379 final VCA: linear above 0.2 V (0.93 at 5 V, zero at 85 mV), an exponential knee below */
        s->vca_gain[i] = V >= 0.2f ? 0.93f * (V - 0.085f) / 4.915f
                                   : 0.93f * 0.115f / 4.915f * expf((V - 0.2f) / 0.0105f);
        /* pan OTAs: complementary, a linear crossfade over 1.25..3.75 V */
        x = esq_clampf((V - 1.25f) / 2.5f, 0.0f, 1.0f);
        s->pan_gl[i] = 1.0f - x;
        s->pan_gr[i] = x;
    }
    esq_set_doc_rate(s, ESQ_DOC_RATE_DEFAULT);
    esq_cards(s);
}
#endif

ESQ_COLD void esq_set_patch(esq_synth_t *s, const esq_patch_t *p)
{
    s->patch = *p;
    s->nheld = 0;
}

#if ESQ_WHOLE
void esq_cc(esq_synth_t *s, int cc, int val)
{
    int i, v = esq_clampi(val, 0, 127);
    switch (cc) {
    case 1: s->wheel = v; break;
    case 4: s->pedal = v; break;
    case 7: s->volume = v; break;
    case 11: s->xctrl = v; break;
    case 64:
        s->sustain = v >= 64;
        if (!s->sustain)
            for (i = 0; i < ESQ_NVOICE; i++)
                if (s->v[i].active && s->v[i].sustained) {
                    s->v[i].sustained = 0;
                    esq_voice_release(s, &s->v[i]);
                }
        break;
#if ESQ_WHOLE
    case 123: esq_all_off(s); break;
#endif
    default: break;
    }
}
#endif

void esq_pressure(esq_synth_t *s, int val) { s->press = esq_clampi(val, 0, 127); }

void esq_bend(esq_synth_t *s, int val) { s->bend = esq_clampi(val, -8192, 8192) * s->bend_range / 32; }

#if ESQ_WHOLE
int esq_active_voices(const esq_synth_t *s)
{
    int i, n = 0;
    for (i = 0; i < ESQ_NVOICE; i++)
        n += s->v[i].active;
    return n;
}
#endif

/* ------------------------------------------------------------- per-voice API --- */

ESQ_COLD void esq_voice_on(esq_synth_t *s, esq_voice_t *v, int note, int vel)
{
    if (!v->active)
        v->doc_nb = 0;                                    /* (state may be all zeros: a host's fresh memory) */
    esq_voice_start(s, v, esq_clampi(note, 0, 127), esq_clampi(vel, 1, 127), 0);
}

ESQ_COLD void esq_voice_off(esq_synth_t *s, esq_voice_t *v)
{
    if (v->active && v->gate)
        esq_voice_release(s, v);
}

/* A legato note (the host kept the voice and its envelopes, the key changed): the ESQ-1's fingered glide from where
 * the pitch is, at GLIDE's rate, or (GLIDE 0) straight there */
ESQ_COLD void esq_voice_legato(esq_synth_t *s, esq_voice_t *v, int note)
{
    int d;
    note = esq_clampi(note, 0, 127);
    if (!v->active || note == v->note)
        return;
    d = (note - v->note) * 256;
    v->gtarget += d;                                      /* (the host's pitch moves by d too: esq_voice_render) */
    if (!s->patch.modes.glide)
        v->gnote = v->gtarget;
    else
        esq_voice_glide_from(s, v, v->gnote);
    v->note = (uint8_t)note;
    s->last_note = note;
}

/* glide from pitch `from` (8.8 semitones, where the voice's own target is gtarget) at GLIDE's rate */
ESQ_COLD void esq_voice_glide_from(esq_synth_t *s, esq_voice_t *v, int32_t from)
{
    int d;
    if (!s->patch.modes.glide) {
        v->gnote = v->gtarget;
        return;
    }
    v->gnote = from;
    d = (v->gtarget - v->gnote) / 256;
    v->grate = esq_seg_rate(d < 0 ? -d : d, s->patch.modes.glide & 63);
}

#if ESQ_FIXED
int esq_voice_render_q(esq_synth_t *s, esq_voice_t *v, int32_t pitch, int32_t *out, int32_t gain, uint32_t n)
{
    uint8_t ticks[ESQ_CHUNK], pos[ESQ_CHUNK][2];
    uint32_t i = 0;
    int32_t base = esq_clampi(pitch, 0, 127 * 256);
#else
int esq_voice_render(esq_synth_t *s, esq_voice_t *v, float pitch, float *out, float gain, uint32_t n)
{
    uint8_t ticks[ESQ_CHUNK], pos[ESQ_CHUNK][2];
    uint32_t i = 0;
    int32_t base = (int32_t)lrintf(esq_clampf(pitch, 0.0f, 127.0f) * 256.0f);
#endif
    v->gnote = base + (v->gnote - v->gtarget);           /* the glide still to go, around the host's pitch */
    v->gtarget = base;
    while (i < n && v->active) {
        uint32_t m = n - i < ESQ_CHUNK ? n - i : ESQ_CHUNK;
        esq_doc_schedule(&v->doc_nb, s->doc_period, m, ticks, pos);
#if ESQ_FIXED
        s->side_at = s->side ? s->side + i : 0;
#endif
        esq_voice_block(s, v, ticks, (const uint8_t(*)[2])pos, m, out + i, 0, gain);
        i += m;
    }
    return v->active;

}
