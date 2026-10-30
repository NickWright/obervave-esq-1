/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D: the ESQ-1 voice in OberVave's eight cards. Included by obv_engine.h after its voice table: each card keeps
 * OberVave's allocation (gate, note, part, MONO / UNISON groups) and gets an esq_voice_t that makes the sound. Each
 * track (part) is an esq_synth_t holding its ESQ-1 program and controls; its voices render through the per-voice API.
 *
 * The programs: the engine's 7 demos (0..6), then the bank's own (the OS's 40 when the owner's bank is loaded).
 * The waves: the generated placeholder set (esq_phbank.h, not ROM data) until the owner's bank is loaded. The host
 * build reads one from the file in SQ1D_BANK (tests); the device's comes with Phase 2's install. */
#include "esq/esq_sq1d.h"
#include "esq/esq_engine.c"
#include "esq/esq_waves.c"
#include "esq/esq_patches.c"
#include "esq/esq_patch.c"
#include "esq/esq_phbank.h"

#define SQ_NDEMO 7u
#define SQ_GAIN_K 16                            /* VOLUME^2 / 4 (Q12) x this: a saturated voice peaks near OberVave's */

static esq_wavebank_t sq_bank;
static esq_synth_t sq_part[NPART];
static esq_voice_t sq_v[NV];
static uint32_t sq_nprog = SQ_NDEMO;
static const uint32_t *sq_prog_src;            /* the app's trk_prog[]: the program each track wants (Phase 1: the
                                                 * track's program number picks the ESQ-1 program) */
static uint32_t sq_prog_cur[NPART] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
static int32_t sq_tmp[CTL], sq_side[CTL];

static void sq_program(uint32_t n, esq_patch_t *out)
{
    n %= sq_nprog;
    if (n < SQ_NDEMO) *out = ESQ_DEMO_PATCHES[n];
    else esq_patch_from_pcb(out, sq_bank.patches + 102u * (n - SQ_NDEMO));
}

static void sq_program_name(uint32_t n, char *out7)   /* 6 characters + NUL */
{
    esq_patch_t p;
    uint32_t i;
    sq_program(n, &p);
    for (i = 0; i < 6u && p.name[i]; i++) out7[i] = p.name[i];
    for (; i < 6u; i++) out7[i] = ' ';
    out7[6] = 0;
}

#ifdef OBV_HOST
#include <stdio.h>
#include <stdlib.h>
static uint8_t *sq_host_bank;                   /* (host) the owner's bank, read from the file in SQ1D_BANK */
#endif

static void sq_init(void)
{
    uint32_t p;
    int loaded = 0;
#ifdef OBV_HOST
    const char *path = getenv("SQ1D_BANK");
    if (path && *path) {
        FILE *f = fopen(path, "rb");
        long len;
        if (f && !fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET)) {
            sq_host_bank = malloc((size_t)len);
            if (sq_host_bank && fread(sq_host_bank, 1, (size_t)len, f) == (size_t)len
                && !esq_wavebank_load(&sq_bank, sq_host_bank, (size_t)len))
                loaded = 1;
        }
        if (f) fclose(f);
        printf("SQ1D bank %s: %s\n", path, loaded ? "loaded" : "NOT loaded, placeholder waves");
    }
#endif
    if (!loaded) esq_wavebank_load_const(&sq_bank, ESQ_PHBANK, sizeof ESQ_PHBANK_U.b);
    sq_nprog = SQ_NDEMO + (sq_bank.patches ? sq_bank.npatches : 0u);
    for (p = 0; p < NPART; p++) {
        esq_init(&sq_part[p], FS, &sq_bank);
        sq_part[p].volume = 127;
        sq_part[p].side = sq_side;
    }
}

/* per block (ISR): a track's new program, its controllers */
static void sq_block(uint32_t p, const part_t *g)
{
    esq_synth_t *s = &sq_part[p];
    uint32_t want = sq_prog_src ? sq_prog_src[p] % sq_nprog : 0u;   /* (one core: a word the main loop wrote) */
    if (want != sq_prog_cur[p]) {               /* sounding notes go on with the new program, as the OS's do */
        sq_program(want, &s->patch);
        sq_prog_cur[p] = want;
    }
    s->wheel = (int)g->wheel_q;                 /* the knob and the mod wheel (CC 1) are one value */
}

static int sq_part_mono(uint32_t p) { return sq_prog_cur[p] != 0xFFFFFFFFu && sq_part[p].patch.modes.mono; }

/* a card starts (or restarts) a note */
static void sq_voice_start(uint32_t card, uint32_t part, uint32_t note, uint32_t vel, int32_t pan)
{
    esq_voice_t *e = &sq_v[card];
    esq_voice_on(&sq_part[part], e, (int)note, (int)vel);
    e->pan_bias = (int16_t)(pan * 2);
}

/* one block of a card: its gate to the engine, its pitch (1/256 semitone), its samples into its track's bus.
 * Returns 0 once the voice has ended (the card is free) */
static int sq_voice_block(uint32_t card, voice_t *v, int32_t *out)
{
    esq_voice_t *e = &sq_v[card];
    const part_t *g = &pt[v->part];
    esq_synth_t *s = &sq_part[v->part];
    int32_t pitch, gain, lv;
    uint32_t i;
    int alive;
    if (!v->gate && e->gate) esq_voice_off(s, e);
    pitch = (int32_t)v->note * 256 + g->bend8 + ctl_tune8 + v->det8;
    gain = g->gain_q12 * SQ_GAIN_K;
    for (i = 0; i < CTL; i++) sq_tmp[i] = sq_side[i] = 0;
    s->side = sq_side;
    alive = esq_voice_render_q(s, e, pitch, sq_tmp, gain, CTL);
    for (i = 0; i < CTL; i++) {
        out[2u * i] += sq_tmp[i] + sq_side[i];
        out[2u * i + 1u] += sq_tmp[i] - sq_side[i];
    }
    s->side_used = 0;
    lv = e->env[3].level;                       /* ENV4, 8.8: the LED's glow */
    ui_vlevel[card] = lv > 0 ? (lv > 32767 ? 32767 : lv) : 0;
    ui_vnote[card] = v->note;
    return alive;
}
