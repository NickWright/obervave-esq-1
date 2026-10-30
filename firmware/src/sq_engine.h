/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D: the ESQ-1 engine as one instrument. One esq_synth_t with its eight voices, allocated by the engine as OS 3.5
 * does (VC, MONO with fingered glide, the sustain pedal). The main loop talks to the audio ISR through a ring of
 * events (notes, controllers, program changes); the ISR owns the synth.
 *
 * The programs (sq_prog.h): INT, CART A, CART B in flash, and ROM: the bank's own 40, else the engine's 7 demos.
 * The waves: the generated placeholder set (esq_phbank.h, not ROM data) until the owner's bank is in flash
 * (sq_waves.h). The host build can read a bank from the file in SQ1D_BANK (tests). */
#include "esq/esq_sq1d.h"
#include "esq/esq_engine.c"
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

/* ---- main loop -> ISR: events */
enum { EV_ON, EV_OFF, EV_CC, EV_BEND, EV_PRESS, EV_PROG, EV_PANIC };
typedef struct { uint8_t type, a, b, pad; int16_t v; } sq_ev_t;
#define SQ_NQ 128u
static sq_ev_t sq_q[SQ_NQ];
static volatile uint32_t sq_qw, sq_qr;
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

/* ---- the ISR side */
static volatile uint32_t ui_nvoices, ui_render_us;   /* (ISR -> UI) voices sounding, the render's time */
static volatile uint8_t ui_vlevel[ESQ_NVOICE];  /* each voice's ENV4, 0..127 (the voice LEDs) */

static void sq_take_events(void)
{
    while (sq_qr != sq_qw) {
        sq_ev_t e;
        RING_PUBLISH();
        e = sq_q[sq_qr % SQ_NQ];
        sq_qr++;
        switch (e.type) {
        case EV_ON: if (e.b) esq_note_on(&sq, e.a, e.b); else esq_note_off(&sq, e.a); break;
        case EV_OFF: esq_note_off(&sq, e.a); break;
        case EV_CC:
            if (e.a == 120u) { int i; for (i = 0; i < ESQ_NVOICE; i++) sq.v[i].active = 0; }   /* all sound off */
            else esq_cc(&sq, e.a, e.b);
            break;
        case EV_BEND: esq_bend(&sq, e.v); break;
        case EV_PRESS: esq_pressure(&sq, e.b); break;
        case EV_PROG:
            sq.patch = sq_pend;                 /* sounding notes go on with the new program, as on the ESQ-1 */
            break;
        default: esq_all_off(&sq); break;
        }
    }
}

static int32_t sq_l[HALF_FRAMES], sq_r[HALF_FRAMES];
static volatile uint32_t sq_ready;              /* sq_engine_init has run: the audio starts before app_main does, and a
                                                 * synth not set up yet (DOC period 0) would loop in the ISR for ever */
static void sq_render(int32_t *out, uint32_t n)
{
    uint32_t i, nv = 0;
    if (!sq_ready) return;
    sq_take_events();
    esq_render_q(&sq, sq_l, sq_r, n);
    for (i = 0; i < n; i++) {                   /* the output stage's full scale (1 << 24) to the platform's Q15 */
        out[2u * i] += sq_l[i] >> 9;
        out[2u * i + 1u] += sq_r[i] >> 9;
    }
    for (i = 0; i < ESQ_NVOICE; i++) {
        int32_t lv = sq.v[i].active ? sq.v[i].env[3].level >> 8 : 0;
        ui_vlevel[i] = (uint8_t)(lv < 0 ? 0 : lv > 127 ? 127 : lv);
        nv += sq.v[i].active;
    }
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
static int sq_set_bank(const uint8_t *img, uint32_t len)
{
    int rc = -1, i;
    if (img && len) rc = esq_wavebank_load_const(&sq_bank, img, len);
    if (rc) (void)esq_wavebank_load_const(&sq_bank, ESQ_PHBANK, sizeof ESQ_PHBANK_U.b);
    for (i = 0; i < ESQ_NVOICE; i++) sq.v[i].active = 0;
    sq.nheld = 0;
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
    if (!loaded) esq_wavebank_load_const(&sq_bank, ESQ_PHBANK, sizeof ESQ_PHBANK_U.b);
    esq_init(&sq, FS, &sq_bank);
    sq.volume = 127;
    sq_rom_program(0, &p);
    esq_set_patch(&sq, &p);
    RING_PUBLISH();
    sq_ready = 1;
}
