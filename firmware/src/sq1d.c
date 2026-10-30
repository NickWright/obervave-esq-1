/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D: one 8-voice synth for the M-VAVE FM-1 after the 1985 digital-analog hybrid with three wave oscillators
 * a voice, a 4-pole filter, four envelopes and three LFOs. The voice is the user's engine (esq/, OS 3.5's control
 * arithmetic, the voice board's audio path); the waves and the factory programs come from the owner's own ROM dumps
 * into flash (sq_waves.h), never from this firmware.
 *
 * R1 (2026-10-30): the standalone app on Dinghy's platform. The keys and MIDI play the engine; PRESETS picks the
 * program, shown as a bank of ten; OCT- / OCT+ the octave. The ESQ-1-style pages (R3), program memory and SysEx
 * dumps (R2) come next. */
#define APP_NAME "SQ1D"
#include "platform.c"
#include "sq_engine.h"
#include "sq_waves.h"
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

static void prog_set(uint32_t n)
{
    ui_prog = n % sq_nprog;
    sq_event(EV_PROG, ui_prog, 0, 0);
}

static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        switch (st & 0xF0u) {
        case 0x90: sq_note(d2 != 0, d1, d2); break;
        case 0x80: sq_note(0, d1, 0); break;
        case 0xB0:
            if (d1 == 123u || d1 == 120u) sq_event(d1 == 120u ? EV_CC : EV_PANIC, d1, d2, 0);
            else sq_event(EV_CC, d1, d2, 0);
            break;
        case 0xC0: prog_set(d1); break;
        case 0xD0: sq_event(EV_PRESS, 0, d1, 0); break;
        case 0xE0: sq_event(EV_BEND, 0, 0, (int32_t)((uint32_t)d1 | (uint32_t)d2 << 7) - 8192); break;
        default: break;
        }
    }
}

static void panel_in(void)
{
    uint32_t rel, press = fm1_input_edges(&rel);
    int32_t d = fm1_enc_take(ENC_PRESETS);
    (void)rel;
    if (d) prog_set((uint32_t)((int32_t)ui_prog + d + (int32_t)sq_nprog * 64) % sq_nprog);
    if (press >> BTN_OCT_DOWN & 1u && ui_oct > -3) ui_oct--;
    if (press >> BTN_OCT_UP & 1u && ui_oct < 3) ui_oct++;
    if ((press >> BTN_OCT_DOWN & 1u) && (fm1_in.buttons >> BTN_OCT_UP & 1u)) ui_oct = 0;   /* both: back to 0 */
    fm1_led_key(BTN_OCT_DOWN, ui_oct < 0);
    fm1_led_key(BTN_OCT_UP, ui_oct > 0);
}

static void app_main(void)
{
    uint32_t t_meter = 0, nprog_seen;
    sq_engine_init();
    sq_waves_boot();                            /* the owner's bank, if it is in flash */
    nprog_seen = sq_nprog;
    ui_draw(1);
    for (;;) {
        platform_poll();
        keys_in();
        midi_in();
        panel_in();
        if (sq_nprog != nprog_seen) { nprog_seen = sq_nprog; if (ui_prog >= sq_nprog) ui_prog = 0; }   /* a bank arrived */
        if (fm1_ms - t_meter >= 40u) {          /* the voices and the meter at 25 frames a second; the rest at once */
            t_meter = fm1_ms;
            ui_draw(0);
        }
    }
}
