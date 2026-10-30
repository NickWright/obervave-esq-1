/* Demo programs for the host harness and Felucca (not ESQ-1 factory sounds: those load from your own OS ROM, as the
 * PTCH chunk tools/esq1rom.py puts in the wave bank). Values are front-panel values, as on the ESQ-1's pages. */
#include "esq_patches.h"

#define EP_OFF {ESQ_SRC_OFF, 0}
#define EP_M(src, amt) {ESQ_SRC_##src, (amt)}
#define EP_LFO_NONE {.freq = 0, .wave = ESQ_LFO_TRI, .mod_src = ESQ_SRC_OFF}
#define EP_OSC(semi_, f, w, m0, m1) {.semi = (semi_), .fine = (f), .wave = (w), .fm = {m0, m1}}
#define EP_DCA(on_, lvl, m0, m1) {.on = (on_), .level = (lvl), .am = {m0, m1}}
/* L1 L2 L3, T1 T2 T3 T4, LV T1V TK */
#define EP_ENV(a1, a2, a3, b1, b2, b3, b4, lv_, t1v_, tk_) \
    {.l1 = (a1), .l2 = (a2), .l3 = (a3), .t1 = (b1), .t2 = (b2), .t3 = (b3), .t4 = (b4), .lv = (lv_), .t1v = (t1v_), .tk = (tk_)}
#define EP_ENV_ZERO EP_ENV(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)

/* wave numbers (the OS's list, esq_waves.c ESQ_ROM_WAVE_NAMES) */
enum { EPW_SAW, EPW_BELL, EPW_SINE, EPW_SQUARE, EPW_PULSE, EPW_NOISE1, EPW_NOISE2, EPW_NOISE3, EPW_BASS, EPW_PIANO, EPW_ELPNO,
       EPW_VOICE1, EPW_VOICE2, EPW_KICK, EPW_REED, EPW_ORGAN, EPW_SYNTH1, EPW_SYNTH2, EPW_SYNTH3, EPW_FORMT1, EPW_FORMT2,
       EPW_FORMT3, EPW_FORMT4, EPW_FORMT5, EPW_PULSE2, EPW_SQR2, EPW_4OCTS, EPW_PRIME, EPW_BASS2, EPW_EPNO2, EPW_OCTAVE, EPW_OCT5 };

const esq_patch_t ESQ_DEMO_PATCHES[] = {
    {
        .name = "INIT",
        .osc = {EP_OSC(0, 0, EPW_SAW, EP_OFF, EP_OFF), EP_OSC(0, 0, EPW_SAW, EP_OFF, EP_OFF), EP_OSC(0, 0, EPW_SAW, EP_OFF, EP_OFF)},
        .dca = {EP_DCA(1, 63, EP_OFF, EP_OFF), EP_DCA(0, 63, EP_OFF, EP_OFF), EP_DCA(0, 63, EP_OFF, EP_OFF)},
        .flt = {.freq = 100, .q = 0, .keybd = 0, .mod = {EP_OFF, EP_OFF}},
        .dca4 = {.level = 63, .pan = 8, .panmod = EP_OFF},
        .env = {EP_ENV_ZERO, EP_ENV_ZERO, EP_ENV_ZERO, EP_ENV(63, 63, 63, 0, 0, 0, 20, 0, 0, 0)},
        .lfo = {EP_LFO_NONE, EP_LFO_NONE, EP_LFO_NONE},
    },
    {
        .name = "SYNCLD",
        .osc = {EP_OSC(0, 0, EPW_SAW, EP_M(LFO1, 8), EP_OFF), EP_OSC(12, 0, EPW_SAW, EP_M(ENV2, 24), EP_M(LFO1, 8)),
                EP_OSC(-12, 3, EPW_SQUARE, EP_M(LFO1, 8), EP_OFF)},
        .dca = {EP_DCA(0, 0, EP_OFF, EP_OFF), EP_DCA(1, 60, EP_OFF, EP_OFF), EP_DCA(1, 44, EP_OFF, EP_OFF)},
        .flt = {.freq = 30, .q = 10, .keybd = 40, .mod = {EP_M(ENV1, 28), EP_M(WHEEL, 20)}},
        .dca4 = {.level = 63, .pan = 8, .panmod = EP_OFF},
        .env = {EP_ENV(63, 36, 24, 0, 35, 40, 24, 16, 0, 0), EP_ENV(63, 0, 0, 0, 38, 0, 20, 0, 0, 0), EP_ENV_ZERO,
                EP_ENV(63, 58, 54, 0, 30, 30, 22, 16, 0, 0)},
        .lfo = {{.freq = 36, .wave = ESQ_LFO_TRI, .l1 = 0, .l2 = 14, .delay = 2, .human = 1, .mod_src = ESQ_SRC_WHEEL},
                EP_LFO_NONE, EP_LFO_NONE},
        .modes = {.sync = 1, .mono = 1, .glide = 22},
    },
    {
        .name = "BELLPD",
        .osc = {EP_OSC(0, 0, EPW_BELL, EP_OFF, EP_OFF), EP_OSC(12, 3, EPW_SINE, EP_OFF, EP_OFF),
                EP_OSC(0, 2, EPW_SYNTH3, EP_M(LFO2, 6), EP_OFF)},
        .dca = {EP_DCA(1, 58, EP_OFF, EP_OFF), EP_DCA(1, 52, EP_M(ENV2, 20), EP_OFF), EP_DCA(1, 54, EP_OFF, EP_OFF)},
        .flt = {.freq = 38, .q = 6, .keybd = 40, .mod = {EP_M(LFO2, 6), EP_M(ENV1, 20)}},
        .dca4 = {.level = 63, .pan = 8, .panmod = EP_M(LFO3, 30)},
        .env = {EP_ENV(63, 40, 20, 10, 40, 50, 40, 30, 0, 0), EP_ENV(-63, 0, 0, 0, 30, 0, 30, 0, 0, 0), EP_ENV_ZERO,
                EP_ENV(63, 50, 46, 22, 48, 50, 44, 25, 20, 10)},
        .lfo = {EP_LFO_NONE,
                {.freq = 24, .wave = ESQ_LFO_TRI, .l1 = 0, .l2 = 12, .delay = 1, .mod_src = ESQ_SRC_OFF},
                {.freq = 10, .wave = ESQ_LFO_TRI, .l1 = 50, .l2 = 50, .mod_src = ESQ_SRC_OFF}},
        .modes = {.env_reset = 1},
    },
    {
        .name = "BASS",
        .osc = {EP_OSC(-12, 0, EPW_BASS, EP_OFF, EP_OFF), EP_OSC(-12, 2, EPW_SQUARE, EP_OFF, EP_OFF),
                EP_OSC(-24, 0, EPW_SINE, EP_OFF, EP_OFF)},
        .dca = {EP_DCA(1, 63, EP_OFF, EP_OFF), EP_DCA(1, 56, EP_OFF, EP_OFF), EP_DCA(1, 54, EP_OFF, EP_OFF)},
        .flt = {.freq = 30, .q = 18, .keybd = 30, .mod = {EP_M(ENV1, 50), EP_M(VEL, 14)}},
        .dca4 = {.level = 63, .pan = 8, .panmod = EP_OFF},
        .env = {EP_ENV(63, 14, 0, 0, 26, 32, 10, 30, 0, 0), EP_ENV_ZERO, EP_ENV_ZERO, EP_ENV(63, 54, 50, 0, 30, 20, 8, 30, 0, 0)},
        .lfo = {EP_LFO_NONE, EP_LFO_NONE, EP_LFO_NONE},
        .modes = {.osc_reset = 1, .env_reset = 1, .vc = 1},
    },
    {
        .name = "AMVOX",
        .osc = {EP_OSC(19, 0, EPW_SINE, EP_M(LFO1, 6), EP_OFF), EP_OSC(0, 0, EPW_VOICE1, EP_M(LFO1, 6), EP_OFF),
                EP_OSC(0, 4, EPW_FORMT2, EP_M(LFO1, 6), EP_OFF)},
        .dca = {EP_DCA(1, 30, EP_OFF, EP_OFF), EP_DCA(1, 63, EP_OFF, EP_OFF), EP_DCA(1, 50, EP_OFF, EP_OFF)},
        .flt = {.freq = 46, .q = 6, .keybd = 30, .mod = {EP_M(ENV1, 12), EP_OFF}},
        .dca4 = {.level = 63, .pan = 8, .panmod = EP_OFF},
        .env = {EP_ENV(63, 30, 30, 30, 40, 0, 40, 0, 0, 0), EP_ENV_ZERO, EP_ENV_ZERO, EP_ENV(63, 60, 60, 34, 30, 30, 40, 20, 0, 0)},
        .lfo = {{.freq = 26, .wave = ESQ_LFO_TRI, .l1 = 0, .l2 = 16, .delay = 3, .mod_src = ESQ_SRC_OFF}, EP_LFO_NONE,
                EP_LFO_NONE},
        .modes = {.am = 1},
    },
    {
        .name = "ORGAN",
        .osc = {EP_OSC(0, 0, EPW_ORGAN, EP_M(LFO1, 5), EP_OFF), EP_OSC(12, 1, EPW_OCTAVE, EP_M(LFO1, 5), EP_OFF),
                EP_OSC(0, 31, EPW_4OCTS, EP_M(LFO1, 5), EP_OFF)},
        .dca = {EP_DCA(1, 60, EP_OFF, EP_OFF), EP_DCA(1, 54, EP_OFF, EP_OFF), EP_DCA(1, 52, EP_OFF, EP_OFF)},
        .flt = {.freq = 40, .q = 2, .keybd = 40, .mod = {EP_OFF, EP_OFF}},
        .dca4 = {.level = 63, .pan = 8, .panmod = EP_OFF},
        .env = {EP_ENV_ZERO, EP_ENV_ZERO, EP_ENV_ZERO, EP_ENV(63, 63, 63, 0, 0, 0, 6, 0, 0, 0)},
        .lfo = {{.freq = 44, .wave = ESQ_LFO_TRI, .l1 = 10, .l2 = 10, .mod_src = ESQ_SRC_OFF}, EP_LFO_NONE, EP_LFO_NONE},
    },
    {
        .name = "WIND",
        .osc = {EP_OSC(0, 0, EPW_NOISE2, EP_OFF, EP_OFF), EP_OSC(0, 0, EPW_NOISE3, EP_OFF, EP_OFF),
                EP_OSC(0, 0, EPW_SAW, EP_OFF, EP_OFF)},
        .dca = {EP_DCA(1, 63, EP_OFF, EP_OFF), EP_DCA(1, 58, EP_OFF, EP_OFF), EP_DCA(0, 0, EP_OFF, EP_OFF)},
        .flt = {.freq = 18, .q = 26, .keybd = 60, .mod = {EP_M(LFO1, 24), EP_M(LFO2, 8)}},
        .dca4 = {.level = 63, .pan = 8, .panmod = EP_M(LFO2, 40)},
        .env = {EP_ENV_ZERO, EP_ENV_ZERO, EP_ENV_ZERO, EP_ENV(63, 63, 63, 44, 0, 0, 46, 0, 0, 0)},
        .lfo = {{.freq = 8, .wave = ESQ_LFO_TRI, .l1 = 63, .l2 = 63, .human = 1, .mod_src = ESQ_SRC_OFF},
                {.freq = 12, .wave = ESQ_LFO_NOISE, .l1 = 50, .l2 = 50, .mod_src = ESQ_SRC_OFF}, EP_LFO_NONE},
    },
};

const int ESQ_NDEMO_PATCHES = (int)(sizeof(ESQ_DEMO_PATCHES) / sizeof(ESQ_DEMO_PATCHES[0]));

#undef EP_OFF
#undef EP_M
#undef EP_LFO_NONE
#undef EP_OSC
#undef EP_DCA
#undef EP_ENV
#undef EP_ENV_ZERO
