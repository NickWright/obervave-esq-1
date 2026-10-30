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
 *     kept (settings). The ESQ-1-style pages come in R3. */
#include <stdint.h>
static void sq_sx_byte(uint32_t b);
#define APP_SYSEX_BYTE(b) sq_sx_byte(b)         /* (usb.c: every SysEx byte, for the ESQ-1's long dumps) */
#define APP_NAME "SQ1D"
#include "platform.c"
#include "sq_engine.h"
#include "sq_waves.h"
#include "sq_prog.h"
#include "sq_ui.h"

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

static void keys_in(void)
{
    uint32_t now = fm1_in.notes & ((1u << NKEY) - 1u), ch = now ^ keys_prev, k;
    for (k = 0; k < NKEY; k++) {
        if (!(ch >> k & 1u)) continue;
        if (now >> k & 1u) {
            int32_t n = (int32_t)KEY_NOTE0 + (int32_t)k + 12 * ui_oct;
            if (n < 0 || n > 127) continue;
            key_note[k] = (uint8_t)n;
            sq_note(1, (uint32_t)n, 100);
            fm1_led_key(KEY_LED(k), 1);
        } else {
            sq_note(0, key_note[k], 0);
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

static void panel_in(void)
{
    uint32_t rel, press = fm1_input_edges(&rel);
    int32_t dp = fm1_enc_take(ENC_PRESETS), db = fm1_enc_take(ENC_ALGORITHM);
    (void)rel;
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
    } else {
        if (dp) prog_load(prog_step(sq_cur, dp, NPROG_ALL));
        if (db) prog_load(prog_step(sq_cur, 10 * db, NPROG_ALL));
        if (press >> BTN_SAVE & 1u)             /* the slot it came from if writable, else INT's same slot */
            ui_write = 1u + (sq_cur / NSLOT == SET_ROM ? sq_cur % NSLOT : sq_cur);
    }
    if (press >> BTN_OCT_DOWN & 1u && ui_oct > -3) ui_oct--;
    if (press >> BTN_OCT_UP & 1u && ui_oct < 3) ui_oct++;
    if ((press >> BTN_OCT_DOWN & 1u) && (fm1_in.buttons >> BTN_OCT_UP & 1u)) ui_oct = 0;   /* both: back to 0 */
    fm1_led_key(BTN_OCT_DOWN, ui_oct < 0);
    fm1_led_key(BTN_OCT_UP, ui_oct > 0);
    fm1_led_key(BTN_SAVE, ui_write != 0);
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
