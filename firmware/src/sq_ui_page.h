/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's pages, VFD look (R4.1): the screen's fields (at most four) in four columns, one over each knob, the field
 * ALGORITHM turns lit; under them that field again in large letters with its full name and, for a number, where it
 * sits in its range (SEQ: the pattern's steps). The header: the page, C* when edited, which screen of how many. */
static uint32_t pg_key_shown = 0xFFFFFFFFu, pg_hash_shown;

static uint32_t pg_hash(void)                  /* a cheap sum of what the page shows: redraw when it changes */
{
    uint32_t i, h = 2166136261u, n = ed_nfld(ed_pg);
    for (i = 0; i < n; i++) h = (h ^ (uint32_t)(fld_get(ed_pg, ed_slot[ed_pg], ed_fld(ed_pg, i)) + 1000)) * 16777619u;
    if (ed_pg == PG_SEQ) h = (h ^ seq_gen) * 16777619u ^ (uint32_t)seq_play << 5 ^ (uint32_t)seq_rec << 6;
    return h ^ sq_edited << 1 ^ ed_comparing ^ (uint32_t)(fm1_ms < ui_msg_until) << 2 ^ ui_redraw << 3;
}

static void pg_title(char *t)                   /* "OSC 2" */
{
    t[0] = 0;
    ui_cat(t, PAGES[ed_pg].title);
    if (PAGES[ed_pg].nslot > 1u) { ui_cat(t, " "); ui_fmt_u(t, ed_slot[ed_pg] + 1u); }
}

static void pg_header(void)
{
    char t[24], nm[8];
    cv_begin(240, 24, VFD_BG);
    pg_title(t);
    if (ed_scr_n(ed_pg) > 1u) {                 /* which screen: "OSC 2 1/2" */
        ui_cat(t, " ");
        ui_fmt_u(t, ed_scr[ed_pg] % ed_scr_n(ed_pg) + 1u);
        ui_cat(t, "/");
        ui_fmt_u(t, ed_scr_n(ed_pg));
    }
    if (sq_edited && !ed_comparing && !PG_SYS(ed_pg)) ui_cat(t, " C*");   /* (the ESQ-1's change marker, every page) */
    cv_text(6, 4, &FONT_S, t, VFD);
    ui_cpy(nm, sq_edit.name, 7);
    if (ed_pg == PG_SEQ) {                      /* the transport, the transposition */
        t[0] = 0;
        ui_cat(t, seq_rec ? "REC " : "");
        ui_cat(t, seq_play ? "PLAY" : "STOP");
        if (seq_transp) {
            ui_cat(t, seq_transp > 0 ? " +" : " -");
            ui_fmt_u(t, (uint32_t)(seq_transp > 0 ? seq_transp : -seq_transp));
        }
    } else if (ed_comparing) { t[0] = 0; ui_cat(t, "COMPARE"); }
    else { t[0] = 0; ui_cat(t, nm); }
    cv_text(240 - 6 - text_w(&FONT_S, t), 4, &FONT_S, t, ed_comparing || (ed_pg == PG_SEQ && (seq_play || seq_rec)) ? VFD : VFD_DIM);
    cv_rect(0, 23, 240, 1, VFD_DIM);
    cv_blit(0, 0);
}

/* the screen's fields: four columns of 60, one over each knob; ALGORITHM's field lit */
static void pg_fields(void)
{
    uint8_t fi[4];
    uint32_t i, n = ed_scr_fields(ed_pg, ed_scr[ed_pg] % ed_scr_n(ed_pg), fi);
    cv_begin(240, 80, VFD_BG);
    for (i = 0; i < 4u; i++) {
        int32_t x = (int32_t)i * 60;
        cv_rect(x + 29, 74, 2, 4, VFD_DIM);    /* (the knob under it) */
        if (i < n) {
            const fld_t *f = ed_fld(ed_pg, fi[i]);
            uint32_t sel = fi[i] == ed_cur[ed_pg];
            char v[12];
            const felucca_font_t *ft;
            fld_text(ed_pg, ed_slot[ed_pg], f, v);
            ft = text_w(&FONT_L, v) <= 58 ? &FONT_L : &FONT_S;
            if (sel) cv_rect(x + 1, 2, 58, 70, VFD_SEL_BG);
            cv_text(x + (60 - text_w(&FONT_S, f->lab)) / 2, 8, &FONT_S, f->lab, sel ? VFD_BG : VFD_DIM);
            cv_text(x + (60 - text_w(ft, v)) / 2, ft == &FONT_L ? 30 : 38, ft, v, sel ? VFD_BG : VFD);
        }
    }
    cv_rect(0, 79, 240, 1, VFD_DIM);
    cv_blit(0, 24);
}

/* (R5 MODERN) the graphs: in the focus area's canvas, y 26..100 (0 at 63) */
static void pg_graph_env(const esq_env_t *e)
{
    int32_t t[4] = {e->t1, e->t2, e->t3, e->t4}, l[5] = {0, e->l1, e->l2, e->l3, 0}, x = 8, i, sum = 0, w;
    for (i = 0; i < 4; i++) sum += t[i] + 6;
    for (i = 0; i < 4; i++) {
        int32_t x1;
        w = (196 * (t[i] + 6)) / sum;
        x1 = x + w;
        if (i == 3) { cv_line(x, 63 - l[3] * 36 / 63, x + 28, 63 - l[3] * 36 / 63, UI_ACC); x += 28; x1 = x + w; }
        cv_line(x, 63 - l[i] * 36 / 63, x1, 63 - l[i + 1] * 36 / 63, UI_ACC);
        x = x1;
    }
}
static void pg_graph_lfo(uint32_t k)
{
    const esq_lfo_t *p = &sq_edit.lfo[k];
    uint32_t sh = sq_edit.x.lfo_shape[k], i, r = 0x1234u;
    int32_t px = 8, py = 63, rv[4];
    for (i = 0; i < 4u; i++) { r = r * 1664525u + 1013904223u; rv[i] = (int32_t)(int8_t)(r >> 24); }
    for (i = 0; i <= 224u; i += 2u) {
        uint32_t ph = (i * 256u / 224u) & 255u, cyc = 0;
        int32_t a = (int32_t)ph - 128, w;
        if (sh == ESQ_LSH_SINE) w = -(a * (128 - (a < 0 ? -a : a))) / 32;
        else if (sh == ESQ_LSH_RAMPDN) w = 127 - (int32_t)ph;
        else if (sh >= ESQ_LSH_SH) {            /* (random: four made-up cycles' worth, a quarter each) */
            cyc = i * 4u / 225u;
            ph = (i * 1024u / 224u) & 255u;
            w = sh == ESQ_LSH_SH ? rv[cyc] : rv[cyc] + (((rv[(cyc + 1u) & 3u] - rv[cyc]) * (int32_t)ph) >> 8);
        } else if ((p->wave & 3u) == ESQ_LFO_TRI) { int32_t b = (int8_t)ph; w = ((b < 0 ? ~b : b) - 64) * 2; }
        else if ((p->wave & 3u) == ESQ_LFO_SAW) w = (int8_t)(uint8_t)(ph + 128u);
        else if ((p->wave & 3u) == ESQ_LFO_SQR) w = ph < 128u ? 0 : 127;
        else { r = r * 1664525u + 1013904223u; w = (int32_t)(int8_t)(r >> 24); }
        w = 63 - w * 36 / 128;
        if (i) cv_line(px, py, 8 + (int32_t)i, w, UI_ACC);
        px = 8 + (int32_t)i; py = w;
    }
}
static void pg_graph_wave(uint32_t wave)
{
    const esq_zone_t *z;
    const uint8_t *d;
    uint32_t n, i;
    int32_t py = 63;
    if (wave >= sq_bank.nwaves || !sq_bank.wave[wave].nzones) return;
    z = esq_wave_zone(&sq_bank, wave, 60);
    d = sq_bank.data + z->offset;
    n = 1u << z->log2len;
    for (i = 0; i < 224u; i++) {                /* (the table, 224 columns of it) */
        int32_t y = 63 - ((int32_t)d[i * n / 224u] - 128) * 36 / 128;
        if (i) cv_line(8 + (int32_t)i - 1, py, 8 + (int32_t)i, y, UI_ACC);
        py = y;
    }
}

/* the cursor's field, large: its full name, its value, its place in the range */
static void pg_focus(void)
{
    uint32_t n = ed_nfld(ed_pg), cur = ed_cur[ed_pg] % n;
    const fld_t *f = ed_fld(ed_pg, cur);
    int32_t v = fld_get(ed_pg, ed_slot[ed_pg], f), hi = fld_hi(f);
    char t[12];
    cv_begin(240, 106, VFD_BG);
    if (fm1_ms < ui_msg_until) {                /* a message (UNDO, COPY, a received program) for 2 s */
        cv_text(8, 24, &FONT_L, ui_msg[0], VFD);
        cv_text(8, 66, &FONT_S, ui_msg[1], VFD_DIM);
        cv_blit(0, 104);
        return;
    }
    if (ui_modern && (ed_pg == PG_ENV || ed_pg == PG_LFO || ed_pg == PG_OSC)) {   /* (R5 MODERN) the field on a line,
                                                 * under it the envelope, the LFO's cycle or the wave */
        fld_text(ed_pg, ed_slot[ed_pg], f, t);
        cv_text(8, 4, &FONT_S, f->name, VFD_DIM);
        cv_text(232 - text_w(&FONT_S, t), 4, &FONT_S, t, VFD);
        cv_rect(8, 63, 224, 1, VFD_DIM);
        if (ed_pg == PG_ENV) pg_graph_env(&sq_edit.env[ed_slot[ed_pg] % 4u]);
        else if (ed_pg == PG_LFO) pg_graph_lfo(ed_slot[ed_pg] % 3u);
        else pg_graph_wave(sq_edit.osc[ed_slot[ed_pg] % 3u].wave);
        cv_blit(0, 104);
        return;
    }
    if (ed_pg == PG_SEQ) {                      /* the cursor's field on a line, under it the pattern's steps */
        const sq_pat_t *p = seq_p();
        uint32_t k;
        fld_text(ed_pg, 0, f, t);
        cv_text(8, 4, &FONT_S, f->name, VFD_DIM);
        cv_text(232 - text_w(&FONT_S, t), 4, &FONT_S, t, VFD);
        for (k = 0; k < SEQ_MAXLEN; k++) {     /* 4 rows of 16: a note lit, a rest a dash, the playing step hollow,
                                                 * the cursor underlined, a tie reaching on */
            int32_t x = 8 + (int32_t)(k % 16u) * 14, y = 26 + (int32_t)(k / 16u) * 15;
            const sq_step_t *st = &p->s[k];
            if (k >= p->len) { cv_rect(x + 5, y + 5, 2, 2, VFD_DIM); continue; }
            if (st->note[0]) {
                cv_rect(x, y, 12, 11, VFD);
                if (st->gate > 4u) cv_rect(x + 12, y + 4, 2, 3, VFD);
            } else
                cv_rect(x, y + 8, 12, 3, VFD_DIM);
            if (seq_play && k == seq_shown_pos) cv_rect(x + 3, y + 3, 6, 5, st->note[0] ? VFD_BG : VFD);
            if (k == seq_cur) cv_rect(x, y + 12, 12, 2, VFD);
        }
        cv_blit(0, 104);
        return;
    }
    cv_text(8, 14, &FONT_S, f->name, VFD_DIM);
    fld_text(ed_pg, ed_slot[ed_pg], f, t);
    cv_text(8, 38, &FONT_L, t, VFD);
    if (hi - f->lo >= 7) {                      /* a number: the bar of its range (0 marked when it is signed) */
        int32_t w = 224 * (v - f->lo) / (hi - f->lo);
        cv_rect(8, 82, 224, 6, VFD_DIM);
        if (f->lo < 0) {
            int32_t z = 224 * -f->lo / (hi - f->lo);
            if (w >= z) cv_rect(8 + z, 82, w - z + 1, 6, VFD);
            else cv_rect(8 + w, 82, z - w, 6, VFD);
            cv_rect(8 + z, 79, 1, 12, VFD);
        } else
            cv_rect(8, 82, w + 1, 6, VFD);
    }
    cv_blit(0, 104);
}

/* the page's part of the screen; 1: it is a voice page (the program page is ui_draw's own) */
static int ui_page_draw(int full)
{
    uint32_t key, h;
    if (ed_pg == PG_PROG) { pg_key_shown = 0xFFFFFFFFu; return 0; }
    key = ed_pg << 16 | ed_slot[ed_pg] << 8 | ed_cur[ed_pg] | ed_scr[ed_pg] << 24;
    h = pg_hash();
    if (full || key != pg_key_shown) { pg_header(); pg_fields(); pg_focus(); }
    else if (h != pg_hash_shown) { pg_header(); pg_fields(); pg_focus(); }
    pg_key_shown = key;
    pg_hash_shown = h;
    ui_redraw = 0;
    return 1;
}
