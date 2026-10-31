/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's program editor (R3): the ESQ-1's voice pages as a table, each page two rows of fields as the original's
 * display shows them over its ten soft buttons. A field is a value in the edit buffer (sq_edit, front-panel units,
 * esq/esq_types.h) with its range and how it reads. An edit goes into sq_edit, then into the raw 102 bytes
 * (sq_edit_pcb) through edit_commit, which writes only the bytes the edit changed (the rest stays as stored), and on to the
 * engine at once: a sounding note follows it, as on the ESQ-1.
 *
 * COMPARE swaps the program as stored in for the edits, sound and pages, and back; UNDO swaps the buffer with the one before the last edit
 * gesture (a gesture: edits of one field with no pause over 1.5 s); INIT; COPY puts this OSC / DCA / ENV / LFO into
 * the next one. */
#include <stddef.h>

/* the pages: the program's (OSC .. SPLIT, PERFORM: fields of the edit buffer), then (R4) the system's (ARP .. STORAGE:
 * fields of the settings, sq_sys.h; SEQ: of the pattern and its step at the cursor, through seq_view) */
enum { PG_PROG, PG_OSC, PG_DCA, PG_FILT, PG_DCA4, PG_ENV, PG_LFO, PG_MODES, PG_SPLIT, PG_XMOD, PG_PERF, PG_ARP, PG_SEQ,
       PG_MASTER, PG_MIDI, PG_STORE, PG_TUNE, NPG };
#define PG_SYS(pg) ((pg) >= PG_ARP)
enum { K_U8, K_S8, K_ONOFF, K_WAVE, K_SRC, K_LFOW, K_OCT, K_SEMI, K_PROG, K_KEY, K_DIR, K_ENUM, K_NOTE, K_Q4, K_PCT,
       K_CHAN, K_STEP };
/* x: 0 a field of the page's slot; (R5) the extension block (sq_edit.x): XS one byte, XA the slot's in an array,
 * XB the slot's bit of a byte */
enum { FX_NO, FX_XS, FX_XA, FX_XB };
typedef struct { const char *lab, *name; uint8_t kind; int16_t lo, hi; uint16_t off; const char *const *nm; uint8_t x; } fld_t;
#define F_X(m) offsetof(esq_ext_t, m)
static const char *const XSRC_NAME[] = {"OFF", "LFO1", "LFO2", "LFO3", "ENV1", "ENV2", "ENV3", "ENV4", "VEL", "VEL2",
                                        "KYBD", "KYBD2", "WHEEL", "PEDAL", "PRESS", "XCTRL", "RAND", "ALT", "RR4", "ZONE"};
static const char *const XDST_NAME[] = {"OFF", "PITCH", "CUTOF", "Q", "LEVEL", "PAN", "LFO1", "SCAN"};
static const char *const LSH_NAME[] = {"WAV", "SINE", "RAMP-", "S&H", "SMTH", "DRUNK"};
static const char *const SYNC_NAME[] = {"FREQ", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8", "1/16", "1/4T", "1/8T"};
static const char *const SCALE_NAME[] = {"EQUAL", "JUST", "PYTH", "MEANT", "WERCK"};
static const char *const KEYN_NAME[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const LOOK_NAME[] = {"VFD", "MODRN"};
typedef struct { const char *title; uint8_t nslot, n1, n2; const fld_t *f; } page_t;   /* n1 + n2: its fields (n1 the
                                                 * ESQ-1's top row; R5's extension fields at the end of n2) */

#define F_OSC(m) offsetof(esq_osc_t, m)
static const fld_t F_OSC_[] = {
    {"OCT", "OCTAVE", K_OCT, -3, 3, F_OSC(semi)}, {"SEMI", "SEMITONE", K_SEMI, 0, 11, F_OSC(semi)},
    {"FINE", "FINE TUNE", K_U8, 0, 31, F_OSC(fine)}, {"WAVE", "WAVEFORM", K_WAVE, 0, 74, F_OSC(wave)},
    {"MOD#1", "FM SOURCE 1", K_SRC, 0, 15, F_OSC(fm[0].src)}, {"DEPTH", "FM DEPTH 1", K_S8, -63, 63, F_OSC(fm[0].amt)},
    {"MOD#2", "FM SOURCE 2", K_SRC, 0, 15, F_OSC(fm[1].src)}, {"DEPTH", "FM DEPTH 2", K_S8, -63, 63, F_OSC(fm[1].amt)},
    {"SCAN", "WAVE SCAN SOURCE", K_ENUM, 0, ESQ_NXS - 1, F_X(scan_src), XSRC_NAME, FX_XA},
    {"DEPTH", "WAVE SCAN DEPTH", K_S8, -63, 63, F_X(scan_amt), 0, FX_XA}};
#define F_DCA(m) offsetof(esq_dca_t, m)
static const fld_t F_DCA_[] = {
    {"LEVEL", "LEVEL", K_U8, 0, 63, F_DCA(level)}, {"OUTPUT", "OUTPUT", K_ONOFF, 0, 1, F_DCA(on)},
    {"MOD#1", "AM SOURCE 1", K_SRC, 0, 15, F_DCA(am[0].src)}, {"DEPTH", "AM DEPTH 1", K_S8, -63, 63, F_DCA(am[0].amt)},
    {"MOD#2", "AM SOURCE 2", K_SRC, 0, 15, F_DCA(am[1].src)}, {"DEPTH", "AM DEPTH 2", K_S8, -63, 63, F_DCA(am[1].amt)}};
#define F_FLT(m) offsetof(esq_filter_t, m)
static const fld_t F_FLT_[] = {
    {"FREQ", "CUTOFF", K_U8, 0, 127, F_FLT(freq)}, {"Q", "RESONANCE", K_U8, 0, 31, F_FLT(q)},
    {"KEYBD", "KEY TRACK", K_U8, 0, 63, F_FLT(keybd)},
    {"MOD#1", "MOD SOURCE 1", K_SRC, 0, 15, F_FLT(mod[0].src)}, {"DEPTH", "MOD DEPTH 1", K_S8, -63, 63, F_FLT(mod[0].amt)},
    {"MOD#2", "MOD SOURCE 2", K_SRC, 0, 15, F_FLT(mod[1].src)}, {"DEPTH", "MOD DEPTH 2", K_S8, -63, 63, F_FLT(mod[1].amt)},
    {"QMOD", "Q MOD SOURCE", K_ENUM, 0, ESQ_NXS - 1, F_X(q_src), XSRC_NAME, FX_XS},
    {"DEPTH", "Q MOD DEPTH", K_S8, -63, 63, F_X(q_amt), 0, FX_XS}};
#define F_D4(m) offsetof(esq_dca4_t, m)
static const fld_t F_D4_[] = {
    {"LEVEL", "FINAL LEVEL", K_U8, 0, 63, F_D4(level)}, {"PAN", "PAN", K_U8, 0, 15, F_D4(pan)},
    {"MOD", "PAN SOURCE", K_SRC, 0, 15, F_D4(panmod.src)}, {"DEPTH", "PAN DEPTH", K_S8, -63, 63, F_D4(panmod.amt)}};
#define F_ENV(m) offsetof(esq_env_t, m)
static const fld_t F_ENV_[] = {
    {"L1", "LEVEL 1", K_S8, -63, 63, F_ENV(l1)}, {"L2", "LEVEL 2", K_S8, -63, 63, F_ENV(l2)},
    {"L3", "LEVEL 3", K_S8, -63, 63, F_ENV(l3)}, {"LV", "VEL > LEVEL", K_U8, 0, 63, F_ENV(lv)},
    {"T1V", "VEL > T1", K_U8, 0, 63, F_ENV(t1v)},
    {"T1", "TIME 1", K_U8, 0, 63, F_ENV(t1)}, {"T2", "TIME 2", K_U8, 0, 63, F_ENV(t2)},
    {"T3", "TIME 3", K_U8, 0, 63, F_ENV(t3)}, {"T4", "RELEASE", K_U8, 0, 63, F_ENV(t4)},
    {"TK", "KEY > TIME", K_U8, 0, 63, F_ENV(tk)},
    {"LOOP", "LOOP SEGMENTS 1-3", K_ONOFF, 0, 1, F_X(env_loop), 0, FX_XB}};
#define F_LFO(m) offsetof(esq_lfo_t, m)
static const fld_t F_LFO_[] = {
    {"FREQ", "RATE", K_U8, 0, 63, F_LFO(freq)}, {"RESET", "RESET", K_ONOFF, 0, 1, F_LFO(reset)},
    {"HUMAN", "HUMANIZE", K_ONOFF, 0, 1, F_LFO(human)}, {"WAV", "WAVE", K_LFOW, 0, 3, F_LFO(wave)},
    {"L1", "LEVEL 1", K_U8, 0, 63, F_LFO(l1)}, {"DELAY", "DELAY", K_U8, 0, 63, F_LFO(delay)},
    {"L2", "LEVEL 2", K_U8, 0, 63, F_LFO(l2)}, {"MOD", "DEPTH SOURCE", K_SRC, 0, 15, F_LFO(mod_src)},
    {"SHAPE", "SHAPE (WAV: AS WAV)", K_ENUM, 0, ESQ_NLSH - 1, F_X(lfo_shape), LSH_NAME, FX_XA},
    {"SYNC", "SYNC TO TEMPO", K_ENUM, 0, ESQ_NSYNC - 1, F_X(lfo_sync), SYNC_NAME, FX_XA},
    {"1SHOT", "ONE-SHOT", K_ONOFF, 0, 1, F_X(lfo_oneshot), 0, FX_XB}};
#define F_MD(m) offsetof(esq_modes_t, m)
static const fld_t F_MD_[] = {
    {"SYNC", "OSC SYNC", K_ONOFF, 0, 1, F_MD(sync)}, {"AM", "OSC AM", K_ONOFF, 0, 1, F_MD(am)},
    {"MONO", "MONO", K_ONOFF, 0, 1, F_MD(mono)}, {"GLIDE", "GLIDE", K_U8, 0, 63, F_MD(glide)},
    {"VC", "VOICE RESTRIKE", K_ONOFF, 0, 1, F_MD(vc)},
    {"ENV", "ENV RESET", K_ONOFF, 0, 1, F_MD(env_reset)}, {"OSC", "OSC RESET", K_ONOFF, 0, 1, F_MD(osc_reset)},
    {"CYC", "CYCLE", K_ONOFF, 0, 1, F_MD(cycle)},
    {"UNI", "UNISON VOICES", K_U8, 1, 4, F_X(uni), 0, FX_XS}, {"DETUN", "UNISON DETUNE (CENTS)", K_U8, 0, 63, F_X(uni_det), 0, FX_XS},
    {"DRIFT", "DRIFT", K_U8, 0, 63, F_X(drift), 0, FX_XS}};
static const fld_t F_XM_[] = {                  /* MOD+: two more modulations, any source to more destinations */
    {"SRC1", "MOD+ 1 SOURCE", K_ENUM, 0, ESQ_NXS - 1, F_X(xm_src[0]), XSRC_NAME, FX_XS},
    {"DEST1", "MOD+ 1 DESTINATION", K_ENUM, 0, ESQ_NXD - 1, F_X(xm_dst[0]), XDST_NAME, FX_XS},
    {"AMT1", "MOD+ 1 DEPTH", K_S8, -63, 63, F_X(xm_amt[0]), 0, FX_XS},
    {"SRC2", "MOD+ 2 SOURCE", K_ENUM, 0, ESQ_NXS - 1, F_X(xm_src[1]), XSRC_NAME, FX_XS},
    {"DEST2", "MOD+ 2 DESTINATION", K_ENUM, 0, ESQ_NXD - 1, F_X(xm_dst[1]), XDST_NAME, FX_XS},
    {"AMT2", "MOD+ 2 DEPTH", K_S8, -63, 63, F_X(xm_amt[1]), 0, FX_XS},
    {"ZONE", "ZONE KEY (THE ZONE SOURCE)", K_KEY, 0, 127, F_X(zone_key), 0, FX_XS}};
#define F_SP(m) offsetof(esq_split_t, m)
static const fld_t F_SP_[] = {
    {"LAYER", "LAYER", K_ONOFF, 0, 1, F_SP(layer)}, {"PRG", "LAYER PROGRAM", K_PROG, 0, 119, F_SP(layer_prog)},
    {"SPLIT", "SPLIT", K_ONOFF, 0, 1, F_SP(split)}, {"PRG", "SPLIT PROGRAM", K_PROG, 0, 119, F_SP(split_prog)},
    {"S/L", "SPLIT/LAYER", K_ONOFF, 0, 1, F_SP(split_layer)}, {"PRG", "S/L PROGRAM", K_PROG, 0, 119, F_SP(split_layer_prog)},
    {"KEY", "SPLIT KEY", K_KEY, 0, 127, F_SP(split_point)}, {"DIR", "SPLIT SIDE", K_DIR, 0, 1, F_SP(split_dir)}};

#define F_P(m) offsetof(esq_patch_t, m)
static const fld_t F_PERF_[] = {                /* PERFORM: the program's most played-with fields on one page */
    {"FREQ", "FILTER CUTOFF", K_U8, 0, 127, F_P(flt.freq)}, {"Q", "RESONANCE", K_U8, 0, 31, F_P(flt.q)},
    {"ATK", "ENV 4 ATTACK", K_U8, 0, 63, F_P(env[3].t1)}, {"REL", "ENV 4 RELEASE", K_U8, 0, 63, F_P(env[3].t4)},
    {"LFO1", "LFO 1 RATE", K_U8, 0, 63, F_P(lfo[0].freq)}, {"GLIDE", "GLIDE", K_U8, 0, 63, F_P(modes.glide)},
    {"LEVEL", "FINAL LEVEL", K_U8, 0, 63, F_P(dca4.level)}, {"PAN", "PAN", K_U8, 0, 15, F_P(dca4.pan)}};
static const char *const ARPM_NAME[] = {"UP", "DOWN", "UP/DN", "RAND", "ORDER"};
static const char *const RATE_NAME[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char *const KEYS_NAME[] = {"PLAY", "TRNSP"};
static const char *const CRV_NAME[] = {"LIN", "SOFT", "HARD", "FIXED"};
static const char *const PCRV_NAME[] = {"LIN", "SOFT", "HARD", "OFF"};
static const char *const CLK_NAME[] = {"INT", "MIDI"};
static const char *const USB_NAME[] = {"ON", "OFF"};
static const char *const OUT_NAME[] = {"ORIG", "CLEAN"};   /* the original's output stage saturating, or not */
static const char *const IMP_NAME[] = {"ASK", "INT", "CRT A", "CRT B"};
static const char *const DMP_NAME[] = {"INT", "CRT A", "CRT B", "ROM", "EDIT"};
#define F_S(m) offsetof(sq_sys_t, m)
static const fld_t F_ARP_[] = {
    {"ARP", "ARPEGGIATOR", K_ONOFF, 0, 1, F_S(arp)}, {"MODE", "ARP MODE", K_ENUM, 0, 4, F_S(amode), ARPM_NAME},
    {"OCT", "ARP OCTAVES", K_U8, 1, 4, F_S(aoct)}, {"RATE", "ARP RATE", K_ENUM, 0, 5, F_S(arate), RATE_NAME},
    {"GATE", "ARP GATE", K_PCT, 5, 100, F_S(agate)},
    {"LATCH", "LATCH", K_ONOFF, 0, 1, F_S(latch)}, {"CHORD", "CHORD MEMORY", K_ONOFF, 0, 1, F_S(chord)},
    {"TEMPO", "TEMPO (BPM)", K_U8, 40, 240, F_S(tempo)}, {"SWING", "SWING", K_PCT, 50, 75, F_S(swing)}};
typedef struct { uint8_t patt, len, rate, tempo, swing, step, note, vel, gate, keys; } seq_view_t;
#define F_V(m) offsetof(seq_view_t, m)
static const fld_t F_SEQ_[] = {
    {"PATT", "PATTERN", K_U8, 1, 8, F_V(patt)}, {"LEN", "LENGTH (STEPS)", K_U8, 1, 64, F_V(len)},
    {"RATE", "STEP RATE", K_ENUM, 0, 5, F_V(rate), RATE_NAME}, {"TEMPO", "TEMPO (BPM)", K_U8, 40, 240, F_V(tempo)},
    {"SWING", "SWING", K_PCT, 50, 75, F_V(swing)},
    {"STEP", "STEP", K_STEP, 1, 64, F_V(step)}, {"NOTE", "STEP NOTE", K_NOTE, 0, 127, F_V(note)},
    {"VEL", "STEP VELOCITY", K_U8, 1, 127, F_V(vel)}, {"GATE", "STEP GATE", K_Q4, 1, 16, F_V(gate)},
    {"KEYS", "KEYS WHILE PLAYING", K_ENUM, 0, 1, F_V(keys), KEYS_NAME}};
static const fld_t F_MST_[] = {
    {"TUNE", "MASTER TUNE", K_S8, -31, 31, F_S(tune)}, {"BEND", "BEND RANGE", K_U8, 0, 12, F_S(bend)},
    {"VOL", "VOLUME", K_U8, 0, 127, F_S(volume)},
    {"KVEL", "KEY VELOCITY", K_U8, 1, 127, F_S(keyvel)}, {"CURVE", "VELOCITY CURVE", K_ENUM, 0, 3, F_S(velcrv), CRV_NAME},
    {"PRESS", "PRESSURE CURVE", K_ENUM, 0, 3, F_S(prscrv), PCRV_NAME},
    {"USB", "AUDIO TO THE COMPUTER", K_ENUM, 0, 1, F_S(usboff), USB_NAME},
    {"OUT", "OUTPUT STAGE", K_ENUM, 0, 1, F_S(outclean), OUT_NAME},
    {"LOOK", "SCREEN LOOK", K_ENUM, 0, 1, F_S(look), LOOK_NAME}};
static const fld_t F_TUNE_[] = {                /* TUNING: a scale, on a key */
    {"SCALE", "SCALE", K_ENUM, 0, 4, F_S(scale), SCALE_NAME}, {"KEY", "ITS KEY", K_ENUM, 0, 11, F_S(skey), KEYN_NAME}};
static const fld_t F_MIDI_[] = {
    {"CHAN", "MIDI CHANNEL", K_CHAN, 0, 16, F_S(chan)}, {"PROG", "PROGRAM CHANGE IN", K_ONOFF, 0, 1, F_S(rxprog)},
    {"SEND", "NOTES OUT", K_ONOFF, 0, 1, F_S(send)},
    {"CLOCK", "CLOCK SOURCE", K_ENUM, 0, 1, F_S(clock), CLK_NAME}, {"CKOUT", "CLOCK OUT", K_ONOFF, 0, 1, F_S(clkout)}};
static const fld_t F_STO_[] = {
    {"IN 40", "40-PROGRAM DUMP IN", K_ENUM, 0, 3, F_S(imp), IMP_NAME},
    {"OUT", "DUMP OUT: SAVE SENDS", K_ENUM, 0, 4, F_S(dump), DMP_NAME}};

static const page_t PAGES[NPG] = {
    [PG_PROG] = {"PROGRAM", 1, 0, 0, 0},
    [PG_OSC] = {"OSC", 3, 4, 6, F_OSC_}, [PG_DCA] = {"DCA", 3, 2, 4, F_DCA_}, [PG_FILT] = {"FILTER", 1, 3, 6, F_FLT_},
    [PG_DCA4] = {"DCA 4", 1, 2, 2, F_D4_}, [PG_ENV] = {"ENV", 4, 5, 6, F_ENV_}, [PG_LFO] = {"LFO", 3, 4, 7, F_LFO_},
    [PG_MODES] = {"MODES", 1, 5, 6, F_MD_}, [PG_SPLIT] = {"SPLIT/LAYER", 1, 4, 4, F_SP_},
    [PG_XMOD] = {"MOD+", 1, 3, 4, F_XM_}, [PG_TUNE] = {"TUNING", 1, 2, 0, F_TUNE_},
    [PG_PERF] = {"PERFORM", 1, 4, 4, F_PERF_}, [PG_ARP] = {"ARP", 1, 5, 4, F_ARP_}, [PG_SEQ] = {"SEQ", 1, 5, 5, F_SEQ_},
    [PG_MASTER] = {"MASTER", 1, 3, 6, F_MST_}, [PG_MIDI] = {"MIDI", 1, 3, 2, F_MIDI_},
    [PG_STORE] = {"STORAGE", 1, 2, 0, F_STO_}};

/* (R4.1) screens: each page in screens of at most four fields, one under each knob (KNOB 1-4 left to right); SELECT
 * steps through them. Field indices in hex, screens apart by '/' (ENV: the levels, the times, the two scalings) */
static const char *const SCR[NPG] = {
    [PG_PROG] = "", [PG_OSC] = "0123/4567/89", [PG_DCA] = "01/2345", [PG_FILT] = "012/3456/78", [PG_DCA4] = "0123",
    [PG_ENV] = "0123/5678/49a", [PG_LFO] = "0123/4567/89a", [PG_MODES] = "0123/4567/89a", [PG_SPLIT] = "0123/4567",
    [PG_XMOD] = "012/345/6", [PG_PERF] = "0123/4567", [PG_ARP] = "0123/4567/8", [PG_SEQ] = "0123/5678/49",
    [PG_MASTER] = "012/3456/78", [PG_MIDI] = "012/34", [PG_STORE] = "01", [PG_TUNE] = "01"};
static uint32_t ed_scr_n(uint32_t pg)           /* the page's screens */
{
    const char *c = SCR[pg];
    uint32_t n = *c ? 1u : 0u;
    while (*c) n += *c++ == '/';
    return n;
}
static uint32_t ed_scr_fields(uint32_t pg, uint32_t sc, uint8_t *f)   /* screen sc's fields -> f[0..3]; their count */
{
    const char *c = SCR[pg];
    uint32_t n = 0;
    while (*c && sc) sc -= *c++ == '/';
    while (*c && *c != '/' && n < 4u) { f[n++] = (uint8_t)(*c <= '9' ? *c - '0' : *c - 'a' + 10); c++; }
    return n;
}

static const char *const SRC_NAME[16] = {"LFO1", "LFO2", "LFO3", "ENV1", "ENV2", "ENV3", "ENV4", "VEL", "VEL2", "KYBD",
                                         "KYBD2", "WHEEL", "PEDAL", "PRESS", "XCTRL", "OFF"};
static const char *const LFOW_NAME[4] = {"TRI", "SAW", "SQR", "NOI"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* the editor's place: the page, its slot (OSC 2 = PG_OSC, 1), the cursor (a field) */
static uint32_t ed_pg = PG_PROG, ed_slot[NPG], ed_cur[NPG], ed_scr[NPG];   /* ed_cur: the field ALGORITHM turns (the
                                                 * last one a knob turned on this screen) */
static uint8_t sq_edit_enc[ESQ_PCB_LEN];        /* sq_edit as the engine encodes it: a commit takes only the bytes an
                                                 * edit changed into the raw program, the rest stays as stored */
static uint8_t ed_undo_pcb[ESQ_PCB_LEN], ed_has_undo, ed_comparing;
static esq_ext_t ed_undo_x, ed_keep_x;           /* (R5) the extension blocks of the undo copy and of the kept edit */
static esq_patch_t ed_cmp_edit;                 /* COMPARE: the edits, kept aside while the stored program is shown */
static uint8_t ed_cmp_pcb[ESQ_PCB_LEN], ed_cmp_enc[ESQ_PCB_LEN], ed_cmp_edited;
static uint32_t ed_last_fld = 0xFFFFFFFFu, ed_last_ms;
/* the edit buffer outlives a program change, as on the ESQ-1: the edited program waits here (and where it came from)
 * until another program is edited or it is written; COMPARE on a program not edited brings it back */
static uint8_t ed_keep_pcb[ESQ_PCB_LEN], ed_keep;
static uint32_t ed_keep_from;

static uint32_t ed_pg_now(void) { return ed_pg; }
static uint32_t ed_cmp_now(void) { return ed_comparing; }

/* SEQ's fields: the pattern's and its step at the cursor, copied in before they are read, back after a turn */
static seq_view_t seq_view;
static void seq_view_fill(seq_view_t *v)
{
    const sq_pat_t *p = seq_p();
    const sq_step_t *s = &p->s[seq_cur % SEQ_MAXLEN];
    v->patt = sys.patt; v->len = p->len; v->rate = p->rate; v->tempo = sys.tempo; v->swing = sys.swing;
    v->step = (uint8_t)(seq_cur + 1u); v->note = s->note[0]; v->vel = s->vel ? s->vel : 100u;
    v->gate = s->gate ? s->gate : 3u; v->keys = sys.keys;
}
static void seq_view_apply(void)
{
    seq_view_t o, *v = &seq_view;
    sq_pat_t *p;
    sq_step_t *s;
    seq_view_fill(&o);
    seq_gen++;
    if (v->patt != o.patt) { sys.patt = v->patt; if (seq_cur >= seq_p()->len) seq_cur = 0; return; }
    p = seq_p();
    if (v->len != o.len) {
        p->len = v->len;
        if (seq_cur >= p->len) seq_cur = (uint8_t)(p->len - 1u);
        if (seq_pos >= p->len) seq_pos = 0;
    }
    if (v->rate != o.rate) p->rate = v->rate;
    if (v->tempo != o.tempo) sys.tempo = v->tempo;
    if (v->swing != o.swing) sys.swing = v->swing;
    if (v->step != o.step) seq_cur = (uint8_t)(v->step - 1u);
    if (v->keys != o.keys) { sys.keys = v->keys; seq_transp = 0; }
    s = &p->s[seq_cur % SEQ_MAXLEN];
    if (v->note != o.note) {                    /* "--": the step empties; from empty: C5 */
        if (!v->note) memset(s, 0, sizeof *s);
        else { s->note[0] = o.note ? v->note : 60u; if (!s->vel) s->vel = 100; if (!s->gate) s->gate = 3; }
    }
    if (v->vel != o.vel && s->note[0]) s->vel = v->vel;
    if (v->gate != o.gate && s->note[0]) s->gate = v->gate;
}

static int ed_compare(void);

static uint8_t *ed_slot_base(uint32_t pg, uint32_t slot)
{
    switch (pg) {
    case PG_OSC: return (uint8_t *)&sq_edit.osc[slot % 3u];
    case PG_DCA: return (uint8_t *)&sq_edit.dca[slot % 3u];
    case PG_FILT: return (uint8_t *)&sq_edit.flt;
    case PG_DCA4: return (uint8_t *)&sq_edit.dca4;
    case PG_ENV: return (uint8_t *)&sq_edit.env[slot % 4u];
    case PG_LFO: return (uint8_t *)&sq_edit.lfo[slot % 3u];
    case PG_MODES: return (uint8_t *)&sq_edit.modes;
    case PG_SPLIT: return (uint8_t *)&sq_edit.split;
    case PG_XMOD: return (uint8_t *)&sq_edit.x;
    case PG_PERF: return (uint8_t *)&sq_edit;
    case PG_SEQ: seq_view_fill(&seq_view); return (uint8_t *)&seq_view;
    default: return (uint8_t *)&sys;
    }
}
static uint32_t ed_nfld(uint32_t pg) { return (uint32_t)PAGES[pg].n1 + PAGES[pg].n2; }
static const fld_t *ed_fld(uint32_t pg, uint32_t i) { return &PAGES[pg].f[i % ed_nfld(pg)]; }

static int32_t fld_hi(const fld_t *f)           /* (WAVE: the bank's waves; STEP: the pattern's length) */
{
    return f->kind == K_WAVE ? (int32_t)sq_bank.nwaves - 1 : f->kind == K_STEP ? (int32_t)seq_p()->len : f->hi;
}
static uint8_t *fld_ptr(uint32_t pg, uint32_t slot, const fld_t *f)
{
    if (f->x) return (uint8_t *)&sq_edit.x + f->off + (f->x == FX_XA ? slot : 0u);
    return ed_slot_base(pg, slot) + f->off;
}
static int32_t fld_get(uint32_t pg, uint32_t slot, const fld_t *f)
{
    const uint8_t *b = fld_ptr(pg, slot, f);
    int32_t semi;
    if (f->x == FX_XB) return *b >> slot & 1u;
    switch (f->kind) {
    case K_S8: return *(const int8_t *)b;
    case K_OCT: semi = *(const int8_t *)b; return semi >= 0 ? semi / 12 : -((11 - semi) / 12);
    case K_SEMI: semi = *(const int8_t *)b; return ((semi % 12) + 12) % 12;
    default: return *b;
    }
}
static void fld_set(uint32_t pg, uint32_t slot, const fld_t *f, int32_t v)
{
    uint8_t *b = fld_ptr(pg, slot, f);
    int32_t semi = *(const int8_t *)b, oct = semi >= 0 ? semi / 12 : -((11 - semi) / 12), st = semi - oct * 12;
    int32_t hi = fld_hi(f);
    v = v < f->lo ? f->lo : v > hi ? hi : v;
    if (f->x == FX_XB) { *b = (uint8_t)(v ? *b | 1u << slot : *b & ~(1u << slot)); return; }
    switch (f->kind) {
    case K_S8: *(int8_t *)b = (int8_t)v; break;
    case K_OCT: semi = v * 12 + st; *(int8_t *)b = (int8_t)(semi < -36 ? -36 : semi > 36 ? 36 : semi); break;
    case K_SEMI: semi = oct * 12 + v; *(int8_t *)b = (int8_t)(semi < -36 ? -36 : semi > 36 ? 36 : semi); break;
    default: *b = (uint8_t)v; break;
    }
}

/* a field's value as it reads (at most 7 characters) */
static void fld_text(uint32_t pg, uint32_t slot, const fld_t *f, char *t)
{
    int32_t v = fld_get(pg, slot, f);
    t[0] = 0;
    switch (f->kind) {
    case K_ONOFF: ui_cat(t, v ? "ON" : "OFF"); break;
    case K_SRC: ui_cat(t, SRC_NAME[v & 15]); break;
    case K_LFOW: ui_cat(t, LFOW_NAME[v & 3]); break;
    case K_DIR: ui_cat(t, v ? "UPPER" : "LOWER"); break;
    case K_WAVE:
        if ((uint32_t)v < sq_bank.nwaves) ui_cpy(t, sq_bank.wave[v].name, 7);
        else ui_fmt_u(t, (uint32_t)v);
        break;
    case K_KEY: ui_cat(t, NOTE_NAME[v % 12]); ui_fmt_u(t, (uint32_t)(v / 12)); break;   /* (C5 = 60, as the ESQ-1) */
    case K_NOTE: if (!v) ui_cat(t, "--"); else { ui_cat(t, NOTE_NAME[v % 12]); ui_fmt_u(t, (uint32_t)(v / 12)); } break;
    case K_ENUM: ui_cat(t, f->nm[v < f->lo ? f->lo : v > f->hi ? f->hi : v]); break;
    case K_Q4: ui_fmt_u(t, (uint32_t)v * 25u); ui_cat(t, "%"); break;
    case K_PCT: ui_fmt_u(t, (uint32_t)v); ui_cat(t, "%"); break;
    case K_CHAN: if (!v) ui_cat(t, "OMNI"); else ui_fmt_u(t, (uint32_t)v); break;
    case K_S8: case K_OCT:
        if (v < 0) { ui_cat(t, "-"); ui_fmt_u(t, (uint32_t)-v); }
        else { ui_cat(t, v ? "+" : ""); ui_fmt_u(t, (uint32_t)v); }
        break;
    default: ui_fmt_u(t, (uint32_t)v); break;
    }
}

static void ed_keep_drop(void) { ed_keep = 0; }
static void ed_keep_aside(void)
{
    if (ed_comparing) {                         /* (the edits wait in the COMPARE copy) */
        if (ed_cmp_edited) { memcpy(ed_keep_pcb, ed_cmp_pcb, ESQ_PCB_LEN); ed_keep_x = ed_cmp_edit.x; ed_keep = 1; ed_keep_from = sq_cur; }
    } else if (sq_edited) {
        memcpy(ed_keep_pcb, sq_edit_pcb, ESQ_PCB_LEN);
        ed_keep_x = sq_edit.x;
        ed_keep = 1;
        ed_keep_from = sq_cur;
    }
}
/* after a load or a receive: the engine's encoding of what was loaded (edit_commit compares against it) */
static void ed_track_lost(void)
{
    esq_patch_to_pcb(&sq_edit, sq_edit_enc);
    ed_comparing = 0;
}
/* sq_edit changed: the bytes whose encoding changed go into the raw program whole (so a field always lands, even in
 * a byte with bits the decoding drops); every other byte stays as stored. Then the engine, EDITED */
static void edit_commit(void)
{
    uint8_t n[ESQ_PCB_LEN];
    uint32_t i;
    esq_patch_to_pcb(&sq_edit, n);
    for (i = 0; i < ESQ_PCB_LEN; i++)
        if (n[i] != sq_edit_enc[i]) sq_edit_pcb[i] = n[i];
    memcpy(sq_edit_enc, n, ESQ_PCB_LEN);
    sq_edited = 1;
    ed_keep = 0;                                /* (this program is the edit buffer now) */
    edit_apply();
}
static void ed_snapshot(void) { memcpy(ed_undo_pcb, sq_edit_pcb, ESQ_PCB_LEN); ed_undo_x = sq_edit.x; ed_has_undo = 1; }

/* turn field i of the page by d: a new gesture (another field, or 1.5 s since the last turn) keeps an undo first */
static void ed_turn(uint32_t i, int32_t d)
{
    const fld_t *f = ed_fld(ed_pg, i);
    uint32_t key = ed_pg << 16 | ed_slot[ed_pg] << 8 | i;
    int32_t v;
    if (ed_pg == PG_PROG || !d) return;
    if (PG_SYS(ed_pg)) {                        /* a setting: no undo, no COMPARE, no program */
        sq_sys_t old = sys;
        fld_set(ed_pg, 0, f, fld_get(ed_pg, 0, f) + d);
        if (ed_pg == PG_SEQ) seq_view_apply();
        if (old.arp != sys.arp || old.latch != sys.latch || old.chord != sys.chord) pl_reset();
        if (old.clock != sys.clock) seq_stop();
        if (old.tune != sys.tune || old.bend != sys.bend || old.volume != sys.volume || old.usboff != sys.usboff ||
            old.outclean != sys.outclean || old.tempo != sys.tempo || old.scale != sys.scale || old.skey != sys.skey)
            sys_apply();
        if (old.look != sys.look) ui_set_look(sys.look);
        return;
    }
    if (ed_comparing) { ed_compare(); return; }   /* (a turn ends COMPARE first, as on the ESQ-1) */
    if (key != ed_last_fld || fm1_ms - ed_last_ms > 1500u) ed_snapshot();
    ed_last_fld = key;
    ed_last_ms = fm1_ms;
    v = fld_get(ed_pg, ed_slot[ed_pg], f) + d;
    if (f->kind == K_SEMI) {                    /* SEMI past 11 or under 0 carries into OCT (the transpose, -36..+36) */
        int8_t *sm = (int8_t *)(ed_slot_base(ed_pg, ed_slot[ed_pg]) + f->off);
        int32_t t = *sm + d;
        *sm = (int8_t)(t < -36 ? -36 : t > 36 ? 36 : t);
        edit_commit();
        return;
    }
    if (f->kind == K_WAVE)                      /* (the rows with no zones of their own play SAW: stepped over) */
        while (v > 0 && v < (int32_t)sq_bank.nwaves && sq_bank.wave[v].nzones &&
               (esq_zone_of(&sq_bank, (uint32_t)v, 0)->flags & 4u))
            v += d > 0 ? 1 : -1;
    fld_set(ed_pg, ed_slot[ed_pg], f, v);
    edit_commit();
}

/* COMPARE: the program as stored in its slot against the edit buffer, sound and pages both; again to swap back. The
 * edits wait aside meanwhile, untouched */
static int ed_compare(void)                      /* 0: nothing to compare (not edited, no edit kept) */
{
    if (!ed_comparing && !sq_edited) {          /* not edited: the edit buffer kept from another program comes back */
        if (!ed_keep) return 0;
        sq_cur = ed_keep_from;
        memcpy(sq_edit_pcb, ed_keep_pcb, ESQ_PCB_LEN);
        edit_decode(&ed_keep_x);
        esq_patch_to_pcb(&sq_edit, sq_edit_enc);
        sq_edited = 1;
        ed_keep = 0;
        ed_last_fld = 0xFFFFFFFFu;
        edit_apply();
        return 2;
    }
    if (!ed_comparing) {
        ed_cmp_edit = sq_edit;
        memcpy(ed_cmp_pcb, sq_edit_pcb, ESQ_PCB_LEN);
        memcpy(ed_cmp_enc, sq_edit_enc, ESQ_PCB_LEN);
        ed_cmp_edited = sq_edited;
        esq_ext_t x;
        prog_pcb(sq_cur, sq_edit_pcb);
        prog_ext(sq_cur, &x);
        edit_decode(&x);
        esq_patch_to_pcb(&sq_edit, sq_edit_enc);
        sq_edited = 0;
        ed_comparing = 1;
    } else {
        sq_edit = ed_cmp_edit;
        memcpy(sq_edit_pcb, ed_cmp_pcb, ESQ_PCB_LEN);
        memcpy(sq_edit_enc, ed_cmp_enc, ESQ_PCB_LEN);
        sq_edited = ed_cmp_edited;
        ed_comparing = 0;
    }
    ed_last_fld = 0xFFFFFFFFu;
    edit_apply();
    return 1;
}
static void ed_undo(void)                       /* swaps: UNDO again is REDO */
{
    uint8_t t[ESQ_PCB_LEN];
    esq_ext_t x;
    if (!ed_has_undo) return;
    memcpy(t, sq_edit_pcb, ESQ_PCB_LEN);
    memcpy(sq_edit_pcb, ed_undo_pcb, ESQ_PCB_LEN);
    memcpy(ed_undo_pcb, t, ESQ_PCB_LEN);
    x = ed_undo_x;
    ed_undo_x = sq_edit.x;
    edit_decode(&x);
    esq_patch_to_pcb(&sq_edit, sq_edit_enc);
    sq_edited = 1;
    ed_keep = 0;
    ed_last_fld = 0xFFFFFFFFu;
    edit_apply();
}
static void ed_init(void)                       /* INIT: the plain program (kept undoable) */
{
    ed_snapshot();
    sq_edit = ESQ_DEMO_PATCHES[0];
    esq_patch_to_pcb(&sq_edit, sq_edit_pcb);
    memcpy(sq_edit_enc, sq_edit_pcb, ESQ_PCB_LEN);
    sq_edited = 1;
    ed_keep = 0;
    ed_last_fld = 0xFFFFFFFFu;
    edit_apply();
}
/* COPY: this OSC / DCA / ENV / LFO into the next of its kind (OSC 3 into OSC 1). 0: not a page with several */
static int ed_copy(void)
{
    uint32_t n = PAGES[ed_pg].nslot, s = ed_slot[ed_pg], t = (s + 1u) % n;
    size_t sz = ed_pg == PG_OSC ? sizeof(esq_osc_t) : ed_pg == PG_DCA ? sizeof(esq_dca_t) :
                ed_pg == PG_ENV ? sizeof(esq_env_t) : sizeof(esq_lfo_t);
    if (n < 2u) return 0;
    ed_snapshot();
    memcpy(ed_slot_base(ed_pg, t), ed_slot_base(ed_pg, s), sz);
    ed_last_fld = 0xFFFFFFFFu;
    edit_commit();
    return 1;
}

/* (R5) a field set to an absolute value (NRPN, CC): as a turn would (an undo first, the program EDITED) */
static void ed_set_abs(uint32_t pg, uint32_t slot, uint32_t fi, int32_t v)
{
    uint32_t keep_pg = ed_pg, keep_slot = ed_slot[pg];
    const fld_t *f;
    if (pg == PG_PROG || PG_SYS(pg) || fi >= ed_nfld(pg)) return;
    f = ed_fld(pg, fi);
    ed_pg = pg;
    ed_slot[pg] = slot % PAGES[pg].nslot;
    if (ed_comparing) ed_compare();
    ed_turn(fi, v - fld_get(pg, ed_slot[pg], f));
    ed_pg = keep_pg;
    ed_slot[pg] = keep_slot;
}

/* (R5) MUTATE: about a third of the program's numbers move by up to an eighth of their range; RANDOM: a new program,
 * within what sounds (the final envelope opens, DCA 4 up, a filter not shut). Both undoable */
static uint32_t ed_rng = 0x7A3Bu;
static int32_t ed_rand(int32_t n) { ed_rng = ed_rng * 1664525u + 1013904223u; return n > 0 ? (int32_t)((ed_rng >> 8) % (uint32_t)n) : 0; }
static void ed_shake(int full)
{
    static const uint8_t PGS[] = {PG_OSC, PG_DCA, PG_FILT, PG_DCA4, PG_ENV, PG_LFO};
    uint32_t i, sl, k;
    ed_snapshot();
    for (i = 0; i < sizeof PGS; i++)
        for (sl = 0; sl < PAGES[PGS[i]].nslot; sl++)
            for (k = 0; k < ed_nfld(PGS[i]); k++) {
                const fld_t *f = ed_fld(PGS[i], k);
                int32_t lo = f->lo, hi = fld_hi(f), v;
                if (f->x || f->kind == K_OCT || f->kind == K_SEMI || f->kind == K_ONOFF) continue;
                if (f->kind == K_WAVE) hi = hi > 31 ? 31 : hi;   /* (the 32 everyday waves) */
                if (full) v = lo + ed_rand(hi - lo + 1);
                else {
                    if (ed_rand(3)) continue;
                    v = fld_get(PGS[i], sl, f) + ed_rand((hi - lo) / 4 + 1) - (hi - lo) / 8;
                }
                fld_set(PGS[i], sl, f, v);
            }
    if (full) {                                 /* (what keeps it a sound) */
        esq_env_t *e4 = &sq_edit.env[3];
        e4->l1 = (int8_t)(40 + ed_rand(24)); e4->l2 = (int8_t)(30 + ed_rand(34)); e4->l3 = (int8_t)(20 + ed_rand(44));
        e4->t1 = (uint8_t)ed_rand(30); e4->t4 = (uint8_t)(10 + ed_rand(30));
        sq_edit.dca4.level = 63;
        sq_edit.dca[0].on = 1;
        if (sq_edit.dca[0].level < 40) sq_edit.dca[0].level = 50;
        if (sq_edit.flt.freq < 40) sq_edit.flt.freq = (uint8_t)(40 + ed_rand(80));
        sq_edit.flt.q = (uint8_t)ed_rand(20);
    }
    ed_last_fld = 0xFFFFFFFFu;
    edit_commit();
}
