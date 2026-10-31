/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's settings (R4): what the system pages (MASTER, MIDI, STORAGE) set, the arpeggiator's and the sequencer's
 * globals, the program playing and the octave. One record in the settings sector, kept 3 s after a change once no
 * voice sounds (the erase stalls the audio ~50 ms). R3's record (program, octave) is read once and carried over. */
#define SYS_MAGIC 0x52314431u                   /* "1D1R" */
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
    uint8_t pad1[3];                                                       /* (44 bytes, no hidden padding: memcmp) */
} sq_sys_t;
static const sq_sys_t SYS_DEFAULT = {SYS_MAGIC, 0, 0, 0, 0, 2, 100, CRV_LIN, CRV_LIN, 127, 0, 1, 0, 0, 0,
                                     IMP_ASK, DMP_INT, 0, AM_UP, 1, RT_16, 50, 0, 0, 0, {0}, 120, 50, 1, 0, {0}};
static sq_sys_t sys, sys_saved;
static uint32_t sys_due;

static void sys_apply(void)                     /* the engine's part, at boot and after a change */
{
    sq_event(EV_SET, SET_TUNE, 0, sys.tune);
    sq_event(EV_SET, SET_BENDR, 0, sys.bend);
    sq_event(EV_CC, 7, sys.volume, 0);
}

static void sys_boot(void)
{
    struct { uint32_t magic; uint16_t prog; int8_t oct; uint8_t pad; } r3;
    sys = SYS_DEFAULT;
    if (!settings_load(&sys_saved, sizeof sys_saved) && sys_saved.magic == SYS_MAGIC) sys = sys_saved;
    else if (!settings_load(&r3, sizeof r3) && r3.magic == SET_MAGIC_R3) { sys.prog = r3.prog; sys.oct = r3.oct; }
    sys.magic = SYS_MAGIC;
    if (sys.tempo < 40u || sys.tempo > 240u) sys.tempo = 120;
    if (sys.patt < 1u || sys.patt > 8u) sys.patt = 1;
    if (sys.swing < 50u || sys.swing > 75u) sys.swing = 50;
    if (sys.nchord > 8u) sys.nchord = 0;
    if (sys.arate >= NRATE) sys.arate = RT_16;
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
