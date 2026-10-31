/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D: one 8-voice synth for the M-VAVE FM-1 after the 1985 digital-analog hybrid with three wave oscillators
 * a voice, a 4-pole filter, four envelopes and three LFOs. The voice is the user's engine (esq/, OS 3.5's control
 * arithmetic, the voice board's audio path); the waves and the factory programs come from the owner's own ROM dumps
 * into flash (sq_waves.h), never from this firmware.
 *
 * R1: the standalone app on Dinghy's platform: the keys and MIDI play the engine, OCT- / OCT+ the octave.
 * R2: program memory (sq_prog.h): INT, CART A, CART B, ROM, as banks of ten; PRESETS a program, ALGORITHM a bank of
 *     ten; SAVE = WRITE (SAVE, PRESETS / ALGORITHM pick the slot, OCT+ yes, OCT- no); the ESQ-1's SysEx
 *     dumps in (one program: the edit buffer; all 40: INT) and out on request. The last program and the octave are
 *     kept (settings).
 * R3: the voice pages (sq_edit.h, sq_ui_page.h), programmed as the ESQ-1: a page button, the cursor on a field
 *     (SELECT, the ESQ-1's soft buttons), the value (ALGORITHM: the data slider; KNOB 1-4: the cursor row's fields).
 *     EDIT taps OSC 1 > 2 > 3, FX DCA 1 > 2 > 3 > DCA 4, ENV 1 > 4, LFO 1 > 3, SEQ FILTER > MODES > SPLIT/LAYER,
 *     HOME the program page; EDIT held + a key: any page in the ESQ-1's page-button order. SEL: COMPARE; EDIT held +
 *     SEL: UNDO (again: REDO), + HOME: INIT, + SAVE: COPY this OSC / DCA / ENV / LFO into the next.
 * R4: the engine at -O2 with its sample loop in RAM; the system pages (sq_sys.h: MASTER, MIDI, STORAGE; GLO held),
 *     MODES and SPLIT/LAYER on GLO, FILTER on FX, HOME's PERFORM page; the playing layer (sq_play.h): arpeggiator,
 *     latch, chord memory, a sequencer of 8 patterns (step entry, real-time recording, transpose, swing, MIDI clock
 *     in and out); a received 40-program dump asks where it goes (IMPORT). */
#include <stdint.h>
static void sq_sx_byte(uint32_t b);
#define APP_SYSEX_BYTE(b) sq_sx_byte(b)         /* (usb.c: every SysEx byte, for the ESQ-1's long dumps) */
#define APP_NAME "SQ1D"
#include "platform.c"
#include "sq_engine.h"
#include "sq_waves.h"
#include "sq_prog.h"
#include "sq_ui.h"
#include "sq_sys.h"
#include "sq_play.h"
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

/* ---- the keys */
#define KEY_NOTE0 53u                           /* the lowest key, F3 */
#define NKEY 27u
static uint32_t keys_prev;
static uint8_t key_note[NKEY];                  /* the note each key started (an octave change while held) */

/* EDIT held + a key: a page, in the ESQ-1's page-button order, then PERFORM, ARP, SEQ, MASTER, MIDI, STORAGE */
static const uint8_t KEY_PG[] = {PG_OSC, PG_OSC, PG_OSC, PG_DCA, PG_DCA, PG_DCA, PG_FILT, PG_DCA4, PG_ENV, PG_ENV, PG_ENV,
                                 PG_ENV, PG_LFO, PG_LFO, PG_LFO, PG_MODES, PG_SPLIT, PG_PERF, PG_ARP, PG_SEQ, PG_MASTER,
                                 PG_MIDI, PG_STORE};
static const uint8_t KEY_SLOT[] = {0, 1, 2, 0, 1, 2, 0, 0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
static uint32_t edit_used;                      /* EDIT was a modifier while held: its release is no tap */
#define KEY_NONE 255u
#define HOLD_MS 600u                            /* GLO / ARP held this long: their second meaning */
static uint32_t arp_down_ms, arp_fired, arp_ncap;
static uint8_t arp_cap[8];                      /* ARP held: the keys pressed meanwhile (chord memory) */

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
            if (fm1_in.buttons >> BTN_ARP & 1u) {    /* chord memory: the keys gathered, no note */
                if (arp_ncap < sizeof arp_cap && n >= 0 && n <= 127) arp_cap[arp_ncap++] = (uint8_t)n;
                key_note[k] = KEY_NONE;
                continue;
            }
            if (n < 0 || n > 127) { key_note[k] = KEY_NONE; continue; }
            key_note[k] = (uint8_t)n;
            pl_input(1, (uint32_t)n, sys.keyvel);   /* (the keys have no velocity: KEY VEL) */
            fm1_led_key(KEY_LED(k), 1);
        } else {
            if (key_note[k] != KEY_NONE) pl_input(0, key_note[k], 0);
            fm1_led_key(KEY_LED(k), 0);
        }
    }
    keys_prev = now;
}

static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        if (st >= 0xF8u) {                      /* clock and transport: CLOCK MIDI only */
            if (!sys.clock) continue;
            if (st == 0xF8u) pl_ext_clock();
            else if (st == 0xFAu) { seq_stop(); seq_start(1); }
            else if (st == 0xFBu) seq_start(0);
            else if (st == 0xFCu) seq_stop();
            continue;
        }
        if (st < 0xF0u && sys.chan && (st & 15u) != sys.chan - 1u) continue;   /* another channel */
        switch (st & 0xF0u) {
        case 0x90: pl_input(d2 != 0, d1, d2 ? sys_vel(d2) : 0u); break;
        case 0x80: pl_input(0, d1, 0); break;
        case 0xB0:
            if (d1 == 123u) { sq_event(EV_PANIC, d1, d2, 0); direct_all_off(); arp_clear(); }
            else sq_event(EV_CC, d1, d2, 0);
            break;
        case 0xC0: if (sys.rxprog) prog_load(sq_cur / NSLOT * NSLOT + d1 % NSLOT); break;   /* within the set, as the ESQ-1's 0-39 */
        case 0xD0: sq_event(EV_PRESS, 0, sys_press(d1), 0); break;
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
    case BTN_FX:                                /* DCA 1 > 2 > 3 > FILTER > DCA 4 (the signal's way) */
        if (pg == PG_DCA) { if (sl < 2u) page_go(PG_DCA, sl + 1u); else ed_pg = PG_FILT; }
        else if (pg == PG_FILT) ed_pg = PG_DCA4;
        else page_go(PG_DCA, 0);
        break;
    case BTN_ENV: if (pg == PG_ENV) page_go(PG_ENV, sl + 1u); else ed_pg = PG_ENV; break;
    case BTN_LFO: if (pg == PG_LFO) page_go(PG_LFO, sl + 1u); else ed_pg = PG_LFO; break;
    case BTN_GLO:                               /* MODES > SPLIT/LAYER; on a system page the next one */
        if (pg == PG_MASTER) ed_pg = PG_MIDI;
        else if (pg == PG_MIDI) ed_pg = PG_STORE;
        else if (pg == PG_STORE) ed_pg = PG_MASTER;
        else ed_pg = pg == PG_MODES ? PG_SPLIT : PG_MODES;
        break;
    case BTN_SEQ: ed_pg = PG_SEQ; break;
    case BTN_HOME: ed_pg = pg == PG_PROG ? PG_PERF : PG_PROG; break;
    default: break;
    }
}

/* a two-way question (WRITE, IMPORT): PRESETS / ALGORITHM move the answer, OCT+ yes, OCT- no; every other button
 * waits. 1: yes, -1: no, 0: not yet */
static int ask(uint32_t press)
{
    if (press >> BTN_OCT_UP & 1u) return 1;
    if (press >> BTN_OCT_DOWN & 1u) return -1;
    return 0;
}

static void panel_in(void)
{
    static const uint8_t PAGE_BTN[] = {BTN_FX, BTN_ENV, BTN_LFO, BTN_SEQ, BTN_HOME};
    static uint32_t glo_down_ms, glo_fired;
    uint32_t rel, press = fm1_input_edges(&rel), held = fm1_in.buttons, i;
    int32_t dp = fm1_enc_take(ENC_PRESETS), db = fm1_enc_take(ENC_ALGORITHM), ds = fm1_enc_take(ENC_SELECT);
    int32_t dk[4];
    for (i = 0; i < 4u; i++) dk[i] = fm1_enc_take(ENC_KNOB1 + i);
    if (sx_import) {                            /* IMPORT: the set, then OCT+ / OCT- */
        uint32_t d = sx_import - 1u;
        int a = ask(press);
        if (dp || db) d = prog_step(d, dp + db, NBANK);
        sx_import = d + 1u;
        if (a > 0) sx_import_to(d);
        else if (a < 0) sx_import_cancel();
        press = 0;
        dp = db = 0;
    } else if (ui_write) {                      /* WRITE: the slot, then OCT+ yes, OCT- no */
        uint32_t g = ui_write - 1u, nw = NBANK * NSLOT;
        int a = ask(press);
        if (dp) g = prog_step(g, dp, nw);
        if (db) g = prog_step(g, 10 * db, nw);
        ui_write = g + 1u;
        if (a > 0) {
            int rc = prog_write(g);
            ui_write = 0;
            ui_say(rc ? "WRITE FAILED" : "WRITTEN", rc ? "FLASH" : SET_NAME[g / NSLOT % NSET]);
        } else if (a < 0) {
            ui_write = 0;
            ui_say("WRITE", "CANCELLED");
        }
        press = 0;                              /* (every other button waits: a stray press neither writes nor cancels) */
        dp = db = 0;
    } else if (held >> BTN_EDIT & 1u) {         /* EDIT held: the modifiers (on SEQ: CLEAR, COPY the pattern) */
        if (press >> BTN_SEL & 1u) { ed_undo(); ui_say("UNDO", ""); edit_used = 1; }
        if (press >> BTN_HOME & 1u) {
            if (ed_pg == PG_SEQ) { pat_init(seq_p()); seq_cur = 0; seq_gen++; ui_say("CLEAR", "THE PATTERN"); }
            else { ed_init(); ui_say("INIT", "THE PLAIN PROGRAM"); }
            edit_used = 1;
        }
        if (press >> BTN_SAVE & 1u) {
            char t[20];
            if (ed_pg == PG_SEQ) {
                uint32_t to = sys.patt % SEQ_NPAT;
                sq_pat[to] = *seq_p();
                t[0] = 0; ui_cat(t, "INTO PATTERN "); ui_fmt_u(t, to + 1u);
                ui_say("COPY", t);
            } else if (ed_copy()) {
                pg_title(t);
                ui_cat(t, " > NEXT");
                ui_say("COPY", t);
            }
            edit_used = 1;
        }
        press &= ~(1u << BTN_SEL | 1u << BTN_HOME | 1u << BTN_SAVE);
    } else {
        if (dp) prog_load(prog_step(sq_cur, dp, NPROG_ALL));
        if (press >> BTN_SAVE & 1u) {
            if (ed_pg == PG_SEQ) {              /* SEQ: the pattern into flash */
                char t[20] = "PATTERN ";
                ui_fmt_u(t, sys.patt);
                ui_say(seq_store() ? "NOT STORED" : "STORED", t);
            } else if (ed_pg == PG_STORE) {     /* STORAGE: the dump SEND names, out over USB */
                static const uint8_t A[5] = {0, 1, 2, 3, 0};
                (void)sq_ed_more(sys.dump == DMP_EDIT ? 101u : 100u, &A[sys.dump % 5u], 1);
                ui_say("SENT", DMP_NAME[sys.dump % 5u]);
            } else if (!PG_SYS(ed_pg)) {        /* the slot it came from if writable, else INT's same slot */
                if (ed_comparing) ed_compare(); /* (WRITE stores the edits, never the compared program) */
                ui_write = 1u + (sq_cur / NSLOT == SET_ROM ? sq_cur % NSLOT : sq_cur);
            }
        }
        if (press >> BTN_SEL & 1u) {
            if (seq_rec && !seq_play) {         /* step entry: a rest, or (keys down) the chord one step longer */
                if (pl_down && ent_n) ent_ties++;
                else seq_rest();
            } else {
                int r = ed_compare();
                if (!r) ui_say("COMPARE", "NOT EDITED");
                else if (r == 2) ui_say("EDIT RECALLED", "C* AGAIN");
            }
        }
        if (press >> BTN_PLAY & 1u) { if (seq_play) seq_stop(); else seq_start(1); }
        if (press >> BTN_REC & 1u) {
            seq_rec ^= 1u;
            ent_n = 0; ent_ties = 0;
            memset(live, 0, sizeof live);
            if (seq_rec) ed_pg = PG_SEQ;
            seq_gen++;
        }
    }
    if (ed_pg == PG_PROG) {
        if (db && !ui_write) prog_load(prog_step(sq_cur, 10 * db, NPROG_ALL));
    } else if (!ui_write && !sx_import) {       /* a page: the cursor, the data slider, the row's knobs */
        const page_t *p = &PAGES[ed_pg];
        uint32_t n = ed_nfld(ed_pg), cur = ed_cur[ed_pg] % n, row0 = cur >= p->n1 ? p->n1 : 0u,
                 rown = cur >= p->n1 ? p->n2 : p->n1;
        if (ds) ed_cur[ed_pg] = cur = (uint32_t)(((int32_t)cur + ds) % (int32_t)n + (int32_t)n) % n;
        if (db) ed_turn(cur, db);
        for (i = 0; i < 4u; i++)
            if (dk[i] && i < rown) { ed_turn(row0 + i, dk[i]); ed_cur[ed_pg] = row0 + i; }
    }
    /* EDIT: a tap on its release; GLO: a tap on its release, held: MASTER; ARP: a tap on its release (the ARP page;
     * there: ARP on / off), held alone: LATCH on / off, held + keys: chord memory */
    if (press >> BTN_EDIT & 1u) edit_used = 0;
    if (rel >> BTN_EDIT & 1u && !edit_used && !ui_write && !sx_import) page_button(BTN_EDIT);
    if (press >> BTN_GLO & 1u) { glo_down_ms = fm1_ms; glo_fired = 0; }
    if (held >> BTN_GLO & 1u && !glo_fired && fm1_ms - glo_down_ms >= HOLD_MS) { glo_fired = 1; ed_pg = PG_MASTER; }
    if (rel >> BTN_GLO & 1u && !glo_fired && !ui_write && !sx_import) page_button(BTN_GLO);
    if (press >> BTN_ARP & 1u) { arp_down_ms = fm1_ms; arp_fired = 0; arp_ncap = 0; }
    if (held >> BTN_ARP & 1u && !arp_fired && !arp_ncap && fm1_ms - arp_down_ms >= HOLD_MS) {
        arp_fired = 1;
        sys.latch ^= 1u;
        pl_reset();
        ui_say("LATCH", sys.latch ? "ON" : "OFF");
    }
    if (rel >> BTN_ARP & 1u && !ui_write && !sx_import) {
        if (arp_ncap) {
            pl_chord_set(arp_cap, arp_ncap);
            ui_say("CHORD MEMORY", sys.chord ? "ON" : "OFF");
        } else if (!arp_fired) {
            if (ed_pg == PG_ARP) { sys.arp ^= 1u; pl_reset(); }
            else ed_pg = PG_ARP;
        }
        arp_ncap = 0;
    }
    for (i = 0; i < sizeof PAGE_BTN; i++)
        if (press >> PAGE_BTN[i] & 1u) page_button(PAGE_BTN[i]);
    if (press >> BTN_OCT_DOWN & 1u && ui_oct > -3) ui_oct--;
    if (press >> BTN_OCT_UP & 1u && ui_oct < 3) ui_oct++;
    if ((press >> BTN_OCT_DOWN & 1u) && (held >> BTN_OCT_UP & 1u)) ui_oct = 0;   /* both: back to 0 */
    fm1_led_key(BTN_OCT_DOWN, ui_oct < 0);
    fm1_led_key(BTN_OCT_UP, ui_oct > 0);
    fm1_led_key(BTN_SAVE, ui_write != 0 || sx_import != 0);
    fm1_led_key(BTN_SEL, ed_comparing);
    fm1_led_key(BTN_EDIT, ed_pg == PG_OSC);
    fm1_led_key(BTN_FX, ed_pg == PG_DCA || ed_pg == PG_FILT || ed_pg == PG_DCA4);
    fm1_led_key(BTN_ENV, ed_pg == PG_ENV);
    fm1_led_key(BTN_LFO, ed_pg == PG_LFO);
    fm1_led_key(BTN_GLO, ed_pg == PG_MODES || ed_pg == PG_SPLIT || ed_pg == PG_MASTER || ed_pg == PG_MIDI || ed_pg == PG_STORE);
    fm1_led_key(BTN_SEQ, ed_pg == PG_SEQ);
    fm1_led_key(BTN_HOME, ed_pg == PG_PROG || ed_pg == PG_PERF);
    fm1_led_key(BTN_ARP, sys.arp);
    fm1_led_key(BTN_PLAY, seq_play);
    fm1_led_key(BTN_REC, seq_rec);
}

static void app_main(void)
{
    uint32_t t_meter = 0;
    sq_engine_init();
    sq_waves_boot();                            /* the owner's bank, if it is in flash */
    banks_boot();
    sys_boot();
    seq_boot();
    sys_saved = sys;                            /* (sys_boot's carry-over of R3's record is saved at the next change) */
    sq_bank_changed = 0;
    prog_load(sq_cur);
    ui_draw(1);
    for (;;) {
        platform_poll();
        keys_in();
        midi_in();
        panel_in();
        pl_poll();
        sq_sx_poll();
        if (sq_bank_changed) { sq_bank_changed = 0; banks_follow(); }   /* a wave bank arrived (or went) */
        sys_poll();
        if (fm1_ms - t_meter >= 40u) {          /* the screen at 25 frames a second, each strip only when it changed */
            t_meter = fm1_ms;
            ui_draw(0);
        }
    }
}
