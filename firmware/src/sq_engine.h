/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D: the ESQ-1 engine as one instrument. One esq_synth_t with its eight voices, allocated by the engine as OS 3.5
 * does (VC, MONO with fingered glide, the sustain pedal). The main loop talks to the audio ISR through a ring of
 * events (notes, controllers, program changes); the ISR owns the synth.
 *
 * The programs (sq_prog.h): INT, CART A, CART B in flash, and ROM: the bank's own 40, else the engine's 7 demos.
 * The waves: the generated placeholder set (esq_phbank.h, not ROM data) until the owner's bank is in flash
 * (sq_waves.h). The host build can read a bank from the file in SQ1D_BANK (tests). */
#include "esq/esq_sq1d.h"
#if ESQ_SPLIT
#include "esq/esq_engine.h"                     /* (the engine: its own object, -O2: tools/build.py) */
#else
#include "esq/esq_engine.c"
#endif
#include "esq/esq_waves.c"
#include "esq/esq_patches.c"
#include "esq/esq_patch.c"
#include "esq/esq_phbank.h"

#define SQ_NDEMO 7u
#ifndef RING_PUBLISH                            /* (one core: the ISR runs whole between two main-loop instructions) */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif

static esq_wavebank_t sq_bank;
static esq_synth_t sq;

/* the ROM programs: the bank's own (the OS's 40), else the engine's demos (sq_prog.h numbers them) */
static uint32_t sq_rom_count(void) { return sq_bank.patches && sq_bank.npatches ? sq_bank.npatches : SQ_NDEMO; }
static void sq_rom_program(uint32_t n, esq_patch_t *out)
{
    if (sq_bank.patches && sq_bank.npatches) esq_patch_from_pcb(out, sq_bank.patches + ESQ_PCB_LEN * (n % sq_bank.npatches));
    else *out = ESQ_DEMO_PATCHES[n % SQ_NDEMO];
}
static esq_patch_t sq_pend;                     /* (main loop writes) the program EV_PROG hands the ISR */
static volatile uint32_t sq_bank_changed;      /* (set by sq_set_bank) the main loop loads the program again */

/* ---- main loop -> ISR: events. Two rings: the immediate one (keys, MIDI, program changes: taken at the next 32
 * samples) and (R4) the timed one: the sequencer's and the arpeggiator's notes, each with the sample time it is due
 * at (sq_clock's count), put in time order by sq_play.h's scheduler a few ms ahead and taken within 32 samples of it */
enum { EV_ON, EV_OFF, EV_CC, EV_BEND, EV_PRESS, EV_PROG, EV_PANIC, EV_SET };
enum { SET_TUNE, SET_BENDR, SET_OUT, SET_TEMPO, SET_MICRO, SET_VOICES };   /* (SET_MICRO: b the note of the octave) */                   /* EV_SET's a: an engine setting (MASTER), v its value */
typedef struct { uint8_t type, a, b, pad; int16_t v; } sq_ev_t;
#define SQ_NQ 128u
#define SQ_NTQ 256u
static sq_ev_t sq_q[SQ_NQ], sq_tq[SQ_NTQ] __attribute__((section(".pool")));
static uint32_t sq_tat[SQ_NTQ] __attribute__((section(".pool")));
static volatile uint32_t sq_qw, sq_qr, sq_tqw, sq_tqr, sq_tdrop_to, sq_tdrop;
static volatile uint32_t sq_clock;              /* (ISR) the samples rendered so far: the timed ring's time */
static void sq_event(uint32_t type, uint32_t a, uint32_t b, int32_t v)
{
    sq_ev_t *e;
    if (sq_qw - sq_qr >= SQ_NQ) return;         /* a flood: dropped (the ISR drains it every 5.8 ms) */
    e = &sq_q[sq_qw % SQ_NQ];
    e->type = (uint8_t)type; e->a = (uint8_t)a; e->b = (uint8_t)b; e->v = (int16_t)v;
    RING_PUBLISH();
    sq_qw++;
}
static void sq_note(uint32_t on, uint32_t note, uint32_t vel) { sq_event(on ? EV_ON : EV_OFF, note & 127u, vel & 127u, 0); }
static int sq_tevent(uint32_t at, uint32_t type, uint32_t a, uint32_t b)   /* 0: queued (at: no earlier than the last) */
{
    sq_ev_t *e;
    if (sq_tqw - sq_tqr >= SQ_NTQ) return -1;
    e = &sq_tq[sq_tqw % SQ_NTQ];
    e->type = (uint8_t)type; e->a = (uint8_t)a; e->b = (uint8_t)b; e->v = 0;
    sq_tat[sq_tqw % SQ_NTQ] = at;
    RING_PUBLISH();
    sq_tqw++;
    return 0;
}
static void sq_tdrop_all(void)                  /* (main loop) the timed notes not yet due are dropped (a stop) */
{
    sq_tdrop_to = sq_tqw;
    RING_PUBLISH();
    sq_tdrop = 1;
}

/* ---- the ISR side */
static volatile uint32_t ui_nvoices, ui_render_us, ui_irq_us;   /* (ISR -> UI) voices sounding, the render's time,
                                                 * the timer interrupt's inside it */
static volatile uint8_t ui_vlevel[ESQ_NVOICE];  /* each voice's ENV4, 0..127 (the voice LEDs) */

/* (R5) SPLIT / LAYER: up to three synths, the program (sq), its LAYER program (all the keys the program plays) and its
 * SPLIT program (the keys on the split's side, instead of the program), eight voices among them: a note that would
 * make more stops the oldest voice of the busiest one. sq_route, set with the programs (EV_PROG), says who plays */
static esq_synth_t sq_lay, sq_spl;
static esq_synth_t *const SQ_SYN[3] = {&sq, &sq_lay, &sq_spl};
typedef struct { uint8_t layer, split, key, upper; } sq_route_t;   /* upper: the split program plays from key up */
static sq_route_t sq_route, sq_route_pend;
static int sq_vmax = ESQ_NVOICE;                /* (R6) VOICES: all three synths' together */
static esq_patch_t sq_pend_lay, sq_pend_spl;
static int sq_nactive(const esq_synth_t *s) { int i, n = 0; for (i = 0; i < ESQ_NVOICE; i++) n += s->v[i].active; return n; }
static void sq_cap(esq_synth_t *to)              /* room for to's next note (its unison) within eight voices */
{
    int need = to->patch.x.uni > 1u ? (to->patch.x.uni > 4u ? 4 : to->patch.x.uni) : 1, k;
    for (k = 0; k < 8; k++) {
        int tot = sq_nactive(&sq) + sq_nactive(&sq_lay) + sq_nactive(&sq_spl), most = -1, mi = 0, i, j;
        esq_voice_t *old = 0;
        if (tot + need <= sq_vmax) return;
        for (j = 0; j < 3; j++) { int n = sq_nactive(SQ_SYN[j]); if (n > most) { most = n; mi = j; } }
        for (i = 0; i < ESQ_NVOICE; i++) {      /* its oldest released voice, else its oldest */
            esq_voice_t *v = &SQ_SYN[mi]->v[i];
            if (v->active && (!old || (!v->gate && old->gate) || (v->gate == old->gate && v->age < old->age))) old = v;
        }
        if (!old) return;
        old->active = 0;
    }
}
static void sq_on(esq_synth_t *s, uint32_t note, uint32_t vel)
{
    if (sq_route.layer || sq_route.split) sq_cap(s);
    esq_note_on(s, (int)note, (int)vel);
}
static void sq_do(const sq_ev_t *e)
{
    uint32_t j;
    switch (e->type) {
    case EV_ON:
        if (!e->b) { for (j = 0; j < 3u; j++) esq_note_off(SQ_SYN[j], e->a); break; }
        if (sq_route.split && (sq_route.upper ? e->a >= sq_route.key : e->a < sq_route.key)) sq_on(&sq_spl, e->a, e->b);
        else {
            sq_on(&sq, e->a, e->b);
            if (sq_route.layer) sq_on(&sq_lay, e->a, e->b);
        }
        break;
    case EV_OFF: for (j = 0; j < 3u; j++) esq_note_off(SQ_SYN[j], e->a); break;   /* (all: the routing may have changed) */
    case EV_CC:
        for (j = 0; j < 3u; j++) {
            if (e->a == 120u) { int i; for (i = 0; i < ESQ_NVOICE; i++) SQ_SYN[j]->v[i].active = 0; }   /* all sound off */
            else esq_cc(SQ_SYN[j], e->a, e->b);
        }
        break;
    case EV_BEND: for (j = 0; j < 3u; j++) esq_bend(SQ_SYN[j], e->v); break;
    case EV_PRESS: for (j = 0; j < 3u; j++) esq_pressure(SQ_SYN[j], e->b); break;
    case EV_PROG:                               /* sounding notes go on with the new program, as on the ESQ-1 */
        sq.patch = sq_pend;
        sq_lay.patch = sq_pend_lay;
        sq_spl.patch = sq_pend_spl;
        if (!sq_route_pend.layer && sq_route.layer) esq_all_off(&sq_lay);
        if (!sq_route_pend.split && sq_route.split) esq_all_off(&sq_spl);
        sq_route = sq_route_pend;
        break;
    case EV_SET:
        for (j = 0; j < 3u; j++) {
            esq_synth_t *s = SQ_SYN[j];
            if (e->a == SET_TUNE) s->master_tune = e->v;
            else if (e->a == SET_BENDR) s->bend_range = e->v;
            else if (e->a == SET_OUT) s->out_clean = (uint8_t)e->v;
            else if (e->a == SET_TEMPO) s->tempo = (uint8_t)e->v;
            else if (e->a == SET_MICRO) s->micro[e->b % 12u] = (int8_t)e->v;
            else if (e->a == SET_VOICES) {      /* (R6) VOICES: the ones past it stop */
                int i;
                s->nvoice = (uint8_t)e->v;
                sq_vmax = e->v > 0 && e->v < ESQ_NVOICE ? e->v : ESQ_NVOICE;
                for (i = sq_vmax; i < ESQ_NVOICE; i++) s->v[i].active = 0;
            }
        }
        break;
    default: for (j = 0; j < 3u; j++) esq_all_off(SQ_SYN[j]); break;
    }
}
static void sq_take_events(uint32_t until)
{
    while (sq_qr != sq_qw) {
        sq_ev_t e;
        RING_PUBLISH();
        e = sq_q[sq_qr % SQ_NQ];
        sq_qr++;
        sq_do(&e);
    }
    while (sq_tqr != sq_tqw && (int32_t)(sq_tat[sq_tqr % SQ_NTQ] - until) < 0) {
        sq_ev_t e;
        RING_PUBLISH();
        e = sq_tq[sq_tqr % SQ_NTQ];
        sq_tqr++;
        sq_do(&e);
    }
}

static int32_t sq_l[HALF_FRAMES], sq_r[HALF_FRAMES];
static volatile uint32_t sq_ready;              /* sq_engine_init has run: the audio starts before app_main does, and a
                                                 * synth not set up yet (DOC period 0) would loop in the ISR for ever */
static void sq_mix(esq_synth_t *s, uint32_t o, uint32_t m)   /* another synth's chunk, added */
{
    static int32_t l[ESQ_CHUNK], r[ESQ_CHUNK];
    uint32_t i;
    esq_render_q(s, l, r, m);
    for (i = 0; i < m; i++) { sq_l[o + i] += l[i]; sq_r[o + i] += r[i]; }
}
static void sq_render(int32_t *out, uint32_t n)
{
    uint32_t i, nv = 0, o;
    if (!sq_ready) return;
    if (sq_tdrop) {                             /* (never back over what was already taken) */
        RING_PUBLISH();
        if ((int32_t)(sq_tdrop_to - sq_tqr) > 0) sq_tqr = sq_tdrop_to;
        sq_tdrop = 0;
    }
    for (o = 0; o < n; o += ESQ_CHUNK) {        /* in the engine's own chunks: an event lands within 32 samples */
        uint32_t m = n - o < ESQ_CHUNK ? n - o : ESQ_CHUNK;
        sq_take_events(sq_clock + o + m);
        esq_render_q(&sq, sq_l + o, sq_r + o, m);
        if (sq_route.layer || sq_nactive(&sq_lay)) sq_mix(&sq_lay, o, m);   /* (R5) LAYER, SPLIT */
        if (sq_route.split || sq_nactive(&sq_spl)) sq_mix(&sq_spl, o, m);
    }
    sq_clock += n;
    for (i = 0; i < n; i++) {                   /* the output stage's full scale (1 << 24) to the platform's Q15 */
        int32_t l = sq_l[i] >> 9, r = sq_r[i] >> 9;   /* (OUTPUT CLEAN can pass full scale: clipped here) */
        out[2u * i] += l > 65535 ? 65535 : l < -65535 ? -65535 : l;
        out[2u * i + 1u] += r > 65535 ? 65535 : r < -65535 ? -65535 : r;
    }
    for (i = 0; i < ESQ_NVOICE; i++) {
        int32_t lv = sq.v[i].active ? sq.v[i].env[3].level >> 8 : 0;
        ui_vlevel[i] = (uint8_t)(lv < 0 ? 0 : lv > 127 ? 127 : lv);
        nv += sq.v[i].active;
    }
    nv += (uint32_t)(sq_nactive(&sq_lay) + sq_nactive(&sq_spl));
    ui_nvoices = nv;
}

/* ---- setup */
#ifdef OBV_HOST
#include <stdio.h>
#include <stdlib.h>
static uint8_t *sq_host_bank;                   /* (host) the owner's bank, read from the file in SQ1D_BANK */
#endif

/* the bank: an .esqw image read in place (img, len), or (img 0) the placeholder. Every voice stops (their tables
 * pointed into the old bank), the program is taken again from the new one. 0: img was taken. (Main loop, with the
 * interrupts off: the ISR never sees half a bank) */
/* (R4.1) the bank is copied into RAM and played from there: the voices' wave reads (8 voices x 3 oscillators at the
 * DOC's 38 kHz, all over 75 KB) missed the flash's XIP cache; the pool holds it (19 sectors, as the store) */
#define SQ_WRAM (19u * 4096u - 256u)
static uint8_t sq_wram[SQ_WRAM] __attribute__((section(".pool"), aligned(4)));
static int sq_bank_ram(const uint8_t *img, uint32_t len)
{
    if (len > SQ_WRAM) return esq_wavebank_load_const(&sq_bank, img, len);   /* (too big: in place) */
    memcpy(sq_wram, img, len);
    return esq_wavebank_load_const(&sq_bank, sq_wram, len);
}
static int sq_set_bank(const uint8_t *img, uint32_t len)
{
    int rc = -1, i;
    if (img && len) rc = sq_bank_ram(img, len);
    if (rc) (void)sq_bank_ram(ESQ_PHBANK, sizeof ESQ_PHBANK_U.b);
    for (i = 0; i < ESQ_NVOICE; i++) sq.v[i].active = sq_lay.v[i].active = sq_spl.v[i].active = 0;
    sq.nheld = sq_lay.nheld = sq_spl.nheld = 0;
    sq_bank_changed = 1;
    return rc;
}

static void sq_engine_init(void)
{
    int loaded = 0;
    esq_patch_t p;
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
    if (!loaded) (void)sq_bank_ram(ESQ_PHBANK, sizeof ESQ_PHBANK_U.b);
    esq_init(&sq, FS, &sq_bank);
    esq_init(&sq_lay, FS, &sq_bank);
    esq_init(&sq_spl, FS, &sq_bank);
    sq.volume = 127;
    sq_rom_program(0, &p);
    esq_set_patch(&sq, &p);
    RING_PUBLISH();
    sq_ready = 1;
}
