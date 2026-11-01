/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's settings (R4): what the system pages (MASTER, MIDI, STORAGE) set, the arpeggiator's and the sequencer's
 * globals, the program playing and the octave. One record in the settings sector, kept 3 s after a change once no
 * voice sounds (the erase stalls the audio ~50 ms). R3's record (program, octave) is read once and carried over. */
#define SYS_MAGIC 0x53314431u                   /* "1D1S" (R5: 48 bytes) */
#define SYS_MAGIC_R4 0x52314431u                /* R4's 44-byte record: carried over */
#define SET_MAGIC_R3 0x51314431u                /* R3's record: { magic, prog, oct } */
enum { CRV_LIN, CRV_SOFT, CRV_HARD, CRV_FIXED };   /* velocity: FIXED plays every note at KEY VEL; pressure: OFF */
enum { IMP_ASK, IMP_INT, IMP_CARTA, IMP_CARTB };   /* where a received 40-program dump goes */
enum { DMP_INT, DMP_CARTA, DMP_CARTB, DMP_ROM, DMP_EDIT };
enum { AM_UP, AM_DOWN, AM_UPDN, AM_RAND, AM_ORDER };
enum { RT_4, RT_8, RT_8T, RT_16, RT_16T, RT_32, NRATE };
typedef struct {
    uint32_t magic;
    uint16_t prog; int8_t oct; uint8_t pad0;
    int8_t tune; uint8_t bend, keyvel, velcrv, prscrv, volume;              /* MASTER */
    uint8_t chan, rxprog, send, clock, clkout;                             /* MIDI (chan 0: omni) */
    uint8_t imp, dump;                                                     /* STORAGE */
    uint8_t arp, amode, aoct, arate, agate, latch, chord, nchord;          /* ARP, chord memory */
    int8_t chordi[8];                                                      /* .. its notes over the lowest */
    uint8_t tempo, swing, patt, keys;                                      /* SEQ (patt 1..8; keys 1: transpose) */
    uint8_t usboff, outclean;                                              /* (R4.1) MASTER USB AUDIO OFF, OUTPUT CLEAN */
    uint8_t voices;                                                        /* (R6) MASTER VOICES 6..8 */
    uint8_t scale, skey, look, pad2;                                       /* (R5) TUNING scale and its key; MODERN */
} sq_sys_t;
typedef char sq_sys_size_check[sizeof(sq_sys_t) == 48u ? 1 : -1];   /* (memcmp: no hidden padding) */
static const sq_sys_t SYS_DEFAULT = {SYS_MAGIC, 0, 0, 0, 0, 2, 100, CRV_LIN, CRV_LIN, 127, 0, 1, 0, 0, 0,
                                     IMP_ASK, DMP_INT, 0, AM_UP, 1, RT_16, 50, 0, 0, 0, {0}, 120, 50, 1, 0, 0, 0, 8, 0, 0, 0, 0};
static sq_sys_t sys, sys_saved;
static uint32_t sys_due;

/* (R5) TUNING: cents off equal temperament, C .. B, for a scale on C (KEY turns it) */
#define NSCALE 5
static const int8_t SCALE_CENTS[NSCALE][12] = {
    {0},                                                            /* EQUAL */
    {0, 12, 4, 16, -14, -2, -10, 2, 14, -16, 18, -12},              /* JUST (5-limit) */
    {0, -10, 4, -6, 8, -2, 12, 2, -8, 6, -4, 10},                   /* PYTHAGOREAN */
    {0, -24, -7, 10, -14, 3, -21, -3, -27, -10, 7, -17},            /* 1/4-COMMA MEANTONE */
    {0, -10, -8, -6, -10, -2, -12, -4, -8, -12, -4, -8}};           /* WERCKMEISTER III */
static void sys_apply(void)                     /* the engine's part, at boot and after a change */
{
    uint32_t k;
    for (k = 0; k < 12u; k++)
        sq_event(EV_SET, SET_MICRO, k, SCALE_CENTS[sys.scale % NSCALE][(k + 12u - sys.skey % 12u) % 12u]);
    sq_event(EV_SET, SET_TEMPO, 0, sys.tempo);
    sq_event(EV_SET, SET_TUNE, 0, sys.tune);
    sq_event(EV_SET, SET_BENDR, 0, sys.bend);
    sq_event(EV_CC, 7, sys.volume, 0);
    sq_event(EV_SET, SET_OUT, 0, sys.outclean);
    sq_event(EV_SET, SET_VOICES, 0, sys.voices);
#ifndef OBV_HOST
    uac_off = sys.usboff;
#endif
}

static void sys_boot(void)
{
    struct { uint32_t magic; uint16_t prog; int8_t oct; uint8_t pad; } r3;
    sys = SYS_DEFAULT;
    if (!settings_load(&sys_saved, sizeof sys_saved) && sys_saved.magic == SYS_MAGIC) sys = sys_saved;
    else if (!settings_load(&sys_saved, 44u) && sys_saved.magic == SYS_MAGIC_R4) {
        memcpy(&sys, &sys_saved, 44u);           /* (R4's: the R5 fields at their defaults) */
        memset(&sys_saved, 0, sizeof sys_saved);
    } else if (!settings_load(&r3, sizeof r3) && r3.magic == SET_MAGIC_R3) { sys.prog = r3.prog; sys.oct = r3.oct; }
    sys.magic = SYS_MAGIC;
    if (sys.tempo < 40u || sys.tempo > 240u) sys.tempo = 120;
    if (sys.patt < 1u || sys.patt > 8u) sys.patt = 1;
    if (sys.swing < 50u || sys.swing > 75u) sys.swing = 50;
    if (sys.nchord > 8u) sys.nchord = 0;
    if (sys.arate >= NRATE) sys.arate = RT_16;
    if (sys.scale >= NSCALE) sys.scale = 0;
    if (sys.voices < 6u || sys.voices > 8u) sys.voices = 8;
    sq_cur = sys.prog % NPROG_ALL;
    ui_oct = sys.oct < -3 ? -3 : sys.oct > 3 ? 3 : sys.oct;
    sys_apply();
}
static void sys_poll(void)
{
    sys.prog = (uint16_t)sq_cur;
    sys.oct = (int8_t)ui_oct;
    if (!memcmp(&sys, &sys_saved, sizeof sys)) { sys_due = 0; return; }
    if (!sys_due) { sys_due = fm1_ms + 3000u; return; }
    if ((int32_t)(fm1_ms - sys_due) < 0 || ui_nvoices) return;
    if (!settings_save(&sys, sizeof sys)) sys_saved = sys;
    sys_due = 0;
}

/* velocity and pressure through their curves (MASTER) */
static uint32_t isqrt_u(uint32_t x) { uint32_t r = 0, b = 1u << 14; while (b) { if ((r + b) * (r + b) <= x) r += b; b >>= 1; } return r; }
static uint32_t crv(uint32_t v, uint32_t c)
{
    switch (c) {
    case CRV_SOFT: return isqrt_u(v * 127u);
    case CRV_HARD: return (v * v + 126u) / 127u;
    default: return v;
    }
}
static uint32_t sys_vel(uint32_t v) { v = sys.velcrv == CRV_FIXED ? sys.keyvel : crv(v, sys.velcrv); return v < 1u ? 1u : v > 127u ? 127u : v; }
static uint32_t sys_press(uint32_t v) { return sys.prscrv == CRV_FIXED ? 0u : crv(v, sys.prscrv); }
static uint32_t sys_ochan(void) { return sys.chan ? sys.chan - 1u : 0u; }
static int imp_dest(void) { return sys.imp == IMP_ASK ? -1 : (int)sys.imp - 1; }
