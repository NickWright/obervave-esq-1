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

enum { PG_PROG, PG_OSC, PG_DCA, PG_FILT, PG_DCA4, PG_ENV, PG_LFO, PG_MODES, PG_SPLIT, NPG };
enum { K_U8, K_S8, K_ONOFF, K_WAVE, K_SRC, K_LFOW, K_OCT, K_SEMI, K_PROG, K_KEY, K_DIR };
typedef struct { const char *lab, *name; uint8_t kind; int16_t lo, hi; uint16_t off; } fld_t;
typedef struct { const char *title; uint8_t nslot, n1, n2; const fld_t *f; } page_t;   /* n1 fields on the top row, n2 under */

#define F_OSC(m) offsetof(esq_osc_t, m)
static const fld_t F_OSC_[] = {
    {"OCT", "OCTAVE", K_OCT, -3, 3, F_OSC(semi)}, {"SEMI", "SEMITONE", K_SEMI, 0, 11, F_OSC(semi)},
    {"FINE", "FINE TUNE", K_U8, 0, 31, F_OSC(fine)}, {"WAVE", "WAVEFORM", K_WAVE, 0, 74, F_OSC(wave)},
    {"MOD#1", "FM SOURCE 1", K_SRC, 0, 15, F_OSC(fm[0].src)}, {"DEPTH", "FM DEPTH 1", K_S8, -63, 63, F_OSC(fm[0].amt)},
    {"MOD#2", "FM SOURCE 2", K_SRC, 0, 15, F_OSC(fm[1].src)}, {"DEPTH", "FM DEPTH 2", K_S8, -63, 63, F_OSC(fm[1].amt)}};
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
    {"MOD#2", "MOD SOURCE 2", K_SRC, 0, 15, F_FLT(mod[1].src)}, {"DEPTH", "MOD DEPTH 2", K_S8, -63, 63, F_FLT(mod[1].amt)}};
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
    {"TK", "KEY > TIME", K_U8, 0, 63, F_ENV(tk)}};
#define F_LFO(m) offsetof(esq_lfo_t, m)
static const fld_t F_LFO_[] = {
    {"FREQ", "RATE", K_U8, 0, 63, F_LFO(freq)}, {"RESET", "RESET", K_ONOFF, 0, 1, F_LFO(reset)},
    {"HUMAN", "HUMANIZE", K_ONOFF, 0, 1, F_LFO(human)}, {"WAV", "WAVE", K_LFOW, 0, 3, F_LFO(wave)},
    {"L1", "LEVEL 1", K_U8, 0, 63, F_LFO(l1)}, {"DELAY", "DELAY", K_U8, 0, 63, F_LFO(delay)},
    {"L2", "LEVEL 2", K_U8, 0, 63, F_LFO(l2)}, {"MOD", "DEPTH SOURCE", K_SRC, 0, 15, F_LFO(mod_src)}};
#define F_MD(m) offsetof(esq_modes_t, m)
static const fld_t F_MD_[] = {
    {"SYNC", "OSC SYNC", K_ONOFF, 0, 1, F_MD(sync)}, {"AM", "OSC AM", K_ONOFF, 0, 1, F_MD(am)},
    {"MONO", "MONO", K_ONOFF, 0, 1, F_MD(mono)}, {"GLIDE", "GLIDE", K_U8, 0, 63, F_MD(glide)},
    {"VC", "VOICE RESTRIKE", K_ONOFF, 0, 1, F_MD(vc)},
    {"ENV", "ENV RESET", K_ONOFF, 0, 1, F_MD(env_reset)}, {"OSC", "OSC RESET", K_ONOFF, 0, 1, F_MD(osc_reset)},
    {"CYC", "CYCLE", K_ONOFF, 0, 1, F_MD(cycle)}};
#define F_SP(m) offsetof(esq_split_t, m)
static const fld_t F_SP_[] = {
    {"LAYER", "LAYER", K_ONOFF, 0, 1, F_SP(layer)}, {"PRG", "LAYER PROGRAM", K_PROG, 0, 119, F_SP(layer_prog)},
    {"SPLIT", "SPLIT", K_ONOFF, 0, 1, F_SP(split)}, {"PRG", "SPLIT PROGRAM", K_PROG, 0, 119, F_SP(split_prog)},
    {"S/L", "SPLIT/LAYER", K_ONOFF, 0, 1, F_SP(split_layer)}, {"PRG", "S/L PROGRAM", K_PROG, 0, 119, F_SP(split_layer_prog)},
    {"KEY", "SPLIT KEY", K_KEY, 0, 127, F_SP(split_point)}, {"DIR", "SPLIT SIDE", K_DIR, 0, 1, F_SP(split_dir)}};

static const page_t PAGES[NPG] = {
    [PG_PROG] = {"PROGRAM", 1, 0, 0, 0},
    [PG_OSC] = {"OSC", 3, 4, 4, F_OSC_}, [PG_DCA] = {"DCA", 3, 2, 4, F_DCA_}, [PG_FILT] = {"FILTER", 1, 3, 4, F_FLT_},
    [PG_DCA4] = {"DCA 4", 1, 2, 2, F_D4_}, [PG_ENV] = {"ENV", 4, 5, 5, F_ENV_}, [PG_LFO] = {"LFO", 3, 4, 4, F_LFO_},
    [PG_MODES] = {"MODES", 1, 5, 3, F_MD_}, [PG_SPLIT] = {"SPLIT/LAYER", 1, 4, 4, F_SP_}};

static const char *const SRC_NAME[16] = {"LFO1", "LFO2", "LFO3", "ENV1", "ENV2", "ENV3", "ENV4", "VEL", "VEL2", "KYBD",
                                         "KYBD2", "WHEEL", "PEDAL", "PRESS", "XCTRL", "OFF"};
static const char *const LFOW_NAME[4] = {"TRI", "SAW", "SQR", "NOI"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* the editor's place: the page, its slot (OSC 2 = PG_OSC, 1), the cursor (a field) */
static uint32_t ed_pg = PG_PROG, ed_slot[NPG], ed_cur[NPG];
static uint8_t sq_edit_enc[ESQ_PCB_LEN];        /* sq_edit as the engine encodes it: a commit takes only the bytes an
                                                 * edit changed into the raw program, the rest stays as stored */
static uint8_t ed_undo_pcb[ESQ_PCB_LEN], ed_has_undo, ed_comparing;
static esq_patch_t ed_cmp_edit;                 /* COMPARE: the edits, kept aside while the stored program is shown */
static uint8_t ed_cmp_pcb[ESQ_PCB_LEN], ed_cmp_enc[ESQ_PCB_LEN], ed_cmp_edited;
static uint32_t ed_last_fld = 0xFFFFFFFFu, ed_last_ms;

static uint32_t ed_pg_now(void) { return ed_pg; }

static void ed_compare(void);

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
    default: return (uint8_t *)&sq_edit.split;
    }
}
static uint32_t ed_nfld(uint32_t pg) { return (uint32_t)PAGES[pg].n1 + PAGES[pg].n2; }
static const fld_t *ed_fld(uint32_t pg, uint32_t i) { return &PAGES[pg].f[i % ed_nfld(pg)]; }

static int32_t fld_get(uint32_t pg, uint32_t slot, const fld_t *f)
{
    const uint8_t *b = ed_slot_base(pg, slot) + f->off;
    int32_t semi;
    switch (f->kind) {
    case K_S8: return *(const int8_t *)b;
    case K_OCT: semi = *(const int8_t *)b; return semi >= 0 ? semi / 12 : -((11 - semi) / 12);
    case K_SEMI: semi = *(const int8_t *)b; return ((semi % 12) + 12) % 12;
    default: return *b;
    }
}
static void fld_set(uint32_t pg, uint32_t slot, const fld_t *f, int32_t v)
{
    uint8_t *b = ed_slot_base(pg, slot) + f->off;
    int32_t semi = *(const int8_t *)b, oct = semi >= 0 ? semi / 12 : -((11 - semi) / 12), st = semi - oct * 12;
    int32_t hi = f->kind == K_WAVE ? (int32_t)sq_bank.nwaves - 1 : f->hi;
    v = v < f->lo ? f->lo : v > hi ? hi : v;
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
    case K_S8: case K_OCT:
        if (v < 0) { ui_cat(t, "-"); ui_fmt_u(t, (uint32_t)-v); }
        else { ui_cat(t, v ? "+" : ""); ui_fmt_u(t, (uint32_t)v); }
        break;
    default: ui_fmt_u(t, (uint32_t)v); break;
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
    edit_apply();
}
static void ed_snapshot(void) { memcpy(ed_undo_pcb, sq_edit_pcb, ESQ_PCB_LEN); ed_has_undo = 1; }

/* turn field i of the page by d: a new gesture (another field, or 1.5 s since the last turn) keeps an undo first */
static void ed_turn(uint32_t i, int32_t d)
{
    const fld_t *f = ed_fld(ed_pg, i);
    uint32_t key = ed_pg << 16 | ed_slot[ed_pg] << 8 | i;
    int32_t v;
    if (ed_pg == PG_PROG || !d) return;
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
static void ed_compare(void)
{
    if (!ed_comparing) {
        ed_cmp_edit = sq_edit;
        memcpy(ed_cmp_pcb, sq_edit_pcb, ESQ_PCB_LEN);
        memcpy(ed_cmp_enc, sq_edit_enc, ESQ_PCB_LEN);
        ed_cmp_edited = sq_edited;
        prog_pcb(sq_cur, sq_edit_pcb);
        esq_patch_from_pcb(&sq_edit, sq_edit_pcb);
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
}
static void ed_undo(void)                       /* swaps: UNDO again is REDO */
{
    uint8_t t[ESQ_PCB_LEN];
    if (!ed_has_undo) return;
    memcpy(t, sq_edit_pcb, ESQ_PCB_LEN);
    memcpy(sq_edit_pcb, ed_undo_pcb, ESQ_PCB_LEN);
    memcpy(ed_undo_pcb, t, ESQ_PCB_LEN);
    esq_patch_from_pcb(&sq_edit, sq_edit_pcb);
    esq_patch_to_pcb(&sq_edit, sq_edit_enc);
    sq_edited = 1;
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
