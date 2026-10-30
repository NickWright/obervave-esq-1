/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D: one 8-voice synth for the M-VAVE FM-1 after the 1985 digital-analog hybrid with three wave oscillators
 * a voice, a 4-pole filter, four envelopes and three LFOs. The voice is the user's engine (esq/, OS 3.5's control
 * arithmetic, the voice board's audio path); the waves and the factory programs come from the owner's own ROM dumps
 * into flash (sq_waves.h), never from this firmware.
 *
 * R1: the standalone app on Dinghy's platform: the keys and MIDI play the engine, OCT- / OCT+ the octave.
 * R2: program memory (sq_prog.h): INT, CART A, CART B, ROM, as banks of ten; PRESETS a program, ALGORITHM a bank of
 *     ten; SAVE = WRITE (SAVE, PRESETS / ALGORITHM pick the slot, SAVE; any other button: no); the ESQ-1's SysEx
 *     dumps in (one program: the edit buffer; all 40: INT) and out on request. The last program and the octave are
 *     kept (settings).
 * R3: the voice pages (sq_edit.h, sq_ui_page.h), programmed as the ESQ-1: a page button, the cursor on a field
 *     (SELECT, the ESQ-1's soft buttons), the value (ALGORITHM: the data slider; KNOB 1-4: the cursor row's fields).
 *     EDIT taps OSC 1 > 2 > 3, FX DCA 1 > 2 > 3 > DCA 4, ENV 1 > 4, LFO 1 > 3, SEQ FILTER > MODES > SPLIT/LAYER,
 *     HOME the program page; EDIT held + a key: any page in the ESQ-1's page-button order. SEL: COMPARE; EDIT held +
 *     SEL: UNDO (again: REDO), + HOME: INIT, + SAVE: COPY this OSC / DCA / ENV / LFO into the next. */
#include <stdint.h>
static void sq_sx_byte(uint32_t b);
#define APP_SYSEX_BYTE(b) sq_sx_byte(b)         /* (usb.c: every SysEx byte, for the ESQ-1's long dumps) */
#define APP_NAME "SQ1D"
#include "platform.c"
#include "sq_engine.h"
#include "sq_waves.h"
#include "sq_prog.h"
#include "sq_ui.h"
#include "sq_edit.h"
#include "sq_ui_page.h"

static void app_audio(int32_t *out, uint32_t n)
{
#ifndef OBV_HOST
    uint32_t t0 = fm1_ticks();
#endif
    sq_render(out, n);
#ifndef OBV_HOST
    ui_render_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
#endif
}

#ifdef OBV_HOST
static void sq_send_raw(const uint8_t *p, uint32_t n)   /* (host) a dump the FM-1 sends: into build/host/sq1d-out.syx */
{
    FILE *f = fopen("build/host/sq1d-out.syx", "ab");
    if (f) { fwrite(p, 1, n, f); fclose(f); }
    printf("%6u ms  sysex out: %u B, code %02X\n", fm1_ms, n, n > 4u ? p[4] : 0u);
}
#else
static void sq_send_raw(const uint8_t *p, uint32_t n) { (void)ota_wire_send(p, n); }
#endif

/* ---- settings: the program playing and the octave, kept 3 s after a change once nothing sounds (the erase stalls
 * the audio) */
#define SET_MAGIC 0x51314431u                   /* "1D1Q" */
typedef struct { uint32_t magic; uint16_t prog; int8_t oct; uint8_t pad; } sq_settings_t;
static sq_settings_t set_saved;
static uint32_t set_due;

static void settings_boot(void)
{
    sq_settings_t s;
    if (!settings_load(&s, sizeof s) && s.magic == SET_MAGIC) {
        set_saved = s;
        sq_cur = s.prog % NPROG_ALL;
        ui_oct = s.oct < -3 ? -3 : s.oct > 3 ? 3 : s.oct;
    }
}
static void settings_poll(void)
{
    sq_settings_t s = {SET_MAGIC, (uint16_t)sq_cur, (int8_t)ui_oct, 0};
    if (s.prog == set_saved.prog && s.oct == set_saved.oct && set_saved.magic == SET_MAGIC) { set_due = 0; return; }
    if (!set_due) { set_due = fm1_ms + 3000u; return; }
    if ((int32_t)(fm1_ms - set_due) < 0 || ui_nvoices) return;
    if (!settings_save(&s, sizeof s)) set_saved = s;
    set_due = 0;
}

/* ---- the keys */
#define KEY_NOTE0 53u                           /* the lowest key, F3 */
#define NKEY 27u
static uint32_t keys_prev;
static uint8_t key_note[NKEY];                  /* the note each key started (an octave change while held) */

/* EDIT held + a key: a page, in the ESQ-1's page-button order */
static const uint8_t KEY_PG[] = {PG_OSC, PG_OSC, PG_OSC, PG_DCA, PG_DCA, PG_DCA, PG_FILT, PG_DCA4, PG_ENV, PG_ENV, PG_ENV,
                                 PG_ENV, PG_LFO, PG_LFO, PG_LFO, PG_MODES, PG_SPLIT};
static const uint8_t KEY_SLOT[] = {0, 1, 2, 0, 1, 2, 0, 0, 0, 1, 2, 3, 0, 1, 2, 0, 0};
static uint32_t edit_used;                      /* EDIT was a modifier while held: its release is no tap */
#define KEY_NONE 255u

static void page_go(uint32_t pg, uint32_t slot) { ed_pg = pg; ed_slot[pg] = slot % PAGES[pg].nslot; }

static void keys_in(void)
{
    uint32_t now = fm1_in.notes & ((1u << NKEY) - 1u), ch = now ^ keys_prev, k;
    for (k = 0; k < NKEY; k++) {
        if (!(ch >> k & 1u)) continue;
        if (now >> k & 1u) {
            int32_t n = (int32_t)KEY_NOTE0 + (int32_t)k + 12 * ui_oct;
            if (fm1_in.buttons >> BTN_EDIT & 1u) {   /* a page, no note */
                if (k < sizeof KEY_PG) page_go(KEY_PG[k], KEY_SLOT[k]);
                edit_used = 1;
                key_note[k] = KEY_NONE;
                continue;
            }
            if (n < 0 || n > 127) { key_note[k] = KEY_NONE; continue; }
            key_note[k] = (uint8_t)n;
            sq_note(1, (uint32_t)n, 100);
            fm1_led_key(KEY_LED(k), 1);
        } else {
            if (key_note[k] != KEY_NONE) sq_note(0, key_note[k], 0);
            fm1_led_key(KEY_LED(k), 0);
        }
    }
    keys_prev = now;
}

static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        switch (st & 0xF0u) {
        case 0x90: sq_note(d2 != 0, d1, d2); break;
        case 0x80: sq_note(0, d1, 0); break;
        case 0xB0:
            if (d1 == 123u) sq_event(EV_PANIC, d1, d2, 0);
            else sq_event(EV_CC, d1, d2, 0);
            break;
        case 0xC0: prog_load(sq_cur / NSLOT * NSLOT + d1 % NSLOT); break;   /* within the set, as the ESQ-1's 0-39 */
        case 0xD0: sq_event(EV_PRESS, 0, d1, 0); break;
        case 0xE0: sq_event(EV_BEND, 0, 0, (int32_t)((uint32_t)d1 | (uint32_t)d2 << 7) - 8192); break;
        default: break;
        }
    }
}

/* a program number moved by d (wrapping over all four sets); WRITE: over the writable ones only */
static uint32_t prog_step(uint32_t g, int32_t d, uint32_t n)
{
    return (uint32_t)(((int32_t)g + d) % (int32_t)n + (int32_t)n) % n;
}

/* a page button tapped: its group's pages in turn */
static void page_button(uint32_t b)
{
    uint32_t pg = ed_pg, sl = ed_slot[pg];
    switch (b) {
    case BTN_EDIT: if (pg == PG_OSC) page_go(PG_OSC, sl + 1u); else ed_pg = PG_OSC; break;
    case BTN_FX:
        if (pg == PG_DCA) { if (sl < 2u) page_go(PG_DCA, sl + 1u); else ed_pg = PG_DCA4; }
        else if (pg == PG_DCA4) page_go(PG_DCA, 0);
        else ed_pg = PG_DCA;
        break;
    case BTN_ENV: if (pg == PG_ENV) page_go(PG_ENV, sl + 1u); else ed_pg = PG_ENV; break;
    case BTN_LFO: if (pg == PG_LFO) page_go(PG_LFO, sl + 1u); else ed_pg = PG_LFO; break;
    case BTN_SEQ: ed_pg = pg == PG_FILT ? PG_MODES : pg == PG_MODES ? PG_SPLIT : PG_FILT; break;
    case BTN_HOME: ed_pg = PG_PROG; break;
    default: break;
    }
}

static void panel_in(void)
{
    static const uint8_t PAGE_BTN[] = {BTN_FX, BTN_ENV, BTN_LFO, BTN_SEQ, BTN_HOME};
    uint32_t rel, press = fm1_input_edges(&rel), held = fm1_in.buttons, i;
    int32_t dp = fm1_enc_take(ENC_PRESETS), db = fm1_enc_take(ENC_ALGORITHM), ds = fm1_enc_take(ENC_SELECT);
    int32_t dk[4];
    for (i = 0; i < 4u; i++) dk[i] = fm1_enc_take(ENC_KNOB1 + i);
    if (ui_write) {                             /* WRITE: the slot, then SAVE again (or anything else: no) */
        uint32_t g = ui_write - 1u, nw = NBANK * NSLOT;
        if (dp) g = prog_step(g, dp, nw);
        if (db) g = prog_step(g, 10 * db, nw);
        ui_write = g + 1u;
        if (press >> BTN_SAVE & 1u) {
            int rc = prog_write(g);
            ui_write = 0;
            ui_say(rc ? "WRITE FAILED" : "WRITTEN", rc ? "FLASH" : SET_NAME[g / NSLOT % NSET]);
        } else if (press)
            ui_write = 0;
        press = 0;
    } else if (held >> BTN_EDIT & 1u) {         /* EDIT held: the modifiers */
        if (press >> BTN_SEL & 1u) { ed_undo(); ui_say("UNDO", ""); edit_used = 1; }
        if (press >> BTN_HOME & 1u) { ed_init(); ui_say("INIT", "THE PLAIN PROGRAM"); edit_used = 1; }
        if (press >> BTN_SAVE & 1u) {
            char t[20];
            if (ed_copy()) {
                pg_title(t);
                ui_cat(t, " > NEXT");
                ui_say("COPY", t);
            }
            edit_used = 1;
        }
        press &= ~(1u << BTN_SEL | 1u << BTN_HOME | 1u << BTN_SAVE);
    } else {
        if (dp) prog_load(prog_step(sq_cur, dp, NPROG_ALL));
        if (press >> BTN_SAVE & 1u)             /* the slot it came from if writable, else INT's same slot */
            ui_write = 1u + (sq_cur / NSLOT == SET_ROM ? sq_cur % NSLOT : sq_cur);
        if (press >> BTN_SEL & 1u) ed_compare();
    }
    if (ed_pg == PG_PROG) {
        if (db && !ui_write) prog_load(prog_step(sq_cur, 10 * db, NPROG_ALL));
    } else if (!ui_write) {                     /* a voice page: the cursor, the data slider, the row's knobs */
        const page_t *p = &PAGES[ed_pg];
        uint32_t n = ed_nfld(ed_pg), cur = ed_cur[ed_pg] % n, row0 = cur >= p->n1 ? p->n1 : 0u,
                 rown = cur >= p->n1 ? p->n2 : p->n1;
        if (ds) ed_cur[ed_pg] = cur = (uint32_t)(((int32_t)cur + ds) % (int32_t)n + (int32_t)n) % n;
        if (db) ed_turn(cur, db);
        for (i = 0; i < 4u; i++)
            if (dk[i] && i < rown) { ed_turn(row0 + i, dk[i]); ed_cur[ed_pg] = row0 + i; }
    }
    if (press >> BTN_EDIT & 1u) edit_used = 0;
    if (rel >> BTN_EDIT & 1u && !edit_used && !ui_write) page_button(BTN_EDIT);   /* a tap: on its release */
    for (i = 0; i < sizeof PAGE_BTN; i++)
        if (press >> PAGE_BTN[i] & 1u) page_button(PAGE_BTN[i]);
    if (press >> BTN_OCT_DOWN & 1u && ui_oct > -3) ui_oct--;
    if (press >> BTN_OCT_UP & 1u && ui_oct < 3) ui_oct++;
    if ((press >> BTN_OCT_DOWN & 1u) && (held >> BTN_OCT_UP & 1u)) ui_oct = 0;   /* both: back to 0 */
    fm1_led_key(BTN_OCT_DOWN, ui_oct < 0);
    fm1_led_key(BTN_OCT_UP, ui_oct > 0);
    fm1_led_key(BTN_SAVE, ui_write != 0);
    fm1_led_key(BTN_SEL, ed_comparing);
    fm1_led_key(BTN_EDIT, ed_pg == PG_OSC);
    fm1_led_key(BTN_FX, ed_pg == PG_DCA || ed_pg == PG_DCA4);
    fm1_led_key(BTN_ENV, ed_pg == PG_ENV);
    fm1_led_key(BTN_LFO, ed_pg == PG_LFO);
    fm1_led_key(BTN_SEQ, ed_pg == PG_FILT || ed_pg == PG_MODES || ed_pg == PG_SPLIT);
    fm1_led_key(BTN_HOME, ed_pg == PG_PROG);
}

static void app_main(void)
{
    uint32_t t_meter = 0;
    sq_engine_init();
    sq_waves_boot();                            /* the owner's bank, if it is in flash */
    banks_boot();
    settings_boot();
    sq_bank_changed = 0;
    prog_load(sq_cur);
    ui_draw(1);
    for (;;) {
        platform_poll();
        keys_in();
        midi_in();
        panel_in();
        sq_sx_poll();
        if (sq_bank_changed) { sq_bank_changed = 0; banks_follow(); }   /* a wave bank arrived (or went) */
        settings_poll();
        if (fm1_ms - t_meter >= 40u) {          /* the screen at 25 frames a second, each strip only when it changed */
            t_meter = fm1_ms;
            ui_draw(0);
        }
    }
}
