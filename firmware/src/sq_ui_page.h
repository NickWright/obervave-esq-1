/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's voice pages (R3), VFD look: the page's two rows of fields on a grid of five, as the ESQ-1 lays them over
 * its soft buttons, the cursor's field lit; under them the cursor's field again in large letters with its full name
 * and, for a number, where it sits in its range. */
static uint32_t pg_key_shown = 0xFFFFFFFFu, pg_hash_shown;

static uint32_t pg_hash(void)                  /* a cheap sum of what the page shows: redraw when it changes */
{
    uint32_t i, h = 2166136261u, n = ed_nfld(ed_pg);
    for (i = 0; i < n; i++) h = (h ^ (uint32_t)(fld_get(ed_pg, ed_slot[ed_pg], ed_fld(ed_pg, i)) + 1000)) * 16777619u;
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
    cv_text(6, 4, &FONT_S, t, VFD);
    ui_cpy(nm, sq_edit.name, 7);
    if (ed_comparing) ui_cpy(t, "COMPARE", sizeof t);
    else { t[0] = 0; ui_cat(t, nm); if (sq_edited) ui_cat(t, "*"); }
    cv_text(240 - 6 - text_w(&FONT_S, t), 4, &FONT_S, t, ed_comparing ? VFD : VFD_DIM);
    cv_rect(0, 23, 240, 1, VFD_DIM);
    cv_blit(0, 0);
}

/* the fields: two rows on the grid of five; the cursor lit */
static void pg_fields(void)
{
    const page_t *p = &PAGES[ed_pg];
    uint32_t i, n = ed_nfld(ed_pg), cur = ed_cur[ed_pg] % n;
    cv_begin(240, 100, VFD_BG);
    for (i = 0; i < n; i++) {
        const fld_t *f = ed_fld(ed_pg, i);
        uint32_t row = i >= p->n1, col = row ? i - p->n1 : i;
        int32_t x = (int32_t)col * 48, y = 6 + (int32_t)row * 48;
        char v[12];
        fld_text(ed_pg, ed_slot[ed_pg], f, v);
        v[6] = 0;
        if (i == cur) cv_rect(x + 1, y - 2, 46, 42, VFD_SEL_BG);
        cv_text(x + (48 - text_w(&FONT_S, f->lab)) / 2, y, &FONT_S, f->lab, i == cur ? VFD_BG : VFD_DIM);
        cv_text(x + (48 - text_w(&FONT_S, v)) / 2, y + 20, &FONT_S, v, i == cur ? VFD_BG : VFD);
    }
    cv_rect(0, 99, 240, 1, VFD_DIM);
    cv_blit(0, 24);
}

/* the cursor's field, large: its full name, its value, its place in the range */
static void pg_focus(void)
{
    uint32_t n = ed_nfld(ed_pg), cur = ed_cur[ed_pg] % n;
    const fld_t *f = ed_fld(ed_pg, cur);
    int32_t v = fld_get(ed_pg, ed_slot[ed_pg], f), hi = f->kind == K_WAVE ? (int32_t)sq_bank.nwaves - 1 : f->hi;
    char t[12];
    cv_begin(240, 86, VFD_BG);
    if (fm1_ms < ui_msg_until) {                /* a message (UNDO, COPY, a received program) for 2 s */
        cv_text(8, 14, &FONT_L, ui_msg[0], VFD);
        cv_text(8, 56, &FONT_S, ui_msg[1], VFD_DIM);
        cv_blit(0, 124);
        return;
    }
    cv_text(8, 6, &FONT_S, f->name, VFD_DIM);
    fld_text(ed_pg, ed_slot[ed_pg], f, t);
    cv_text(8, 28, &FONT_L, t, VFD);
    if (hi - f->lo >= 7) {                      /* a number: the bar of its range (0 marked when it is signed) */
        int32_t w = 224 * (v - f->lo) / (hi - f->lo);
        cv_rect(8, 70, 224, 6, VFD_DIM);
        if (f->lo < 0) {
            int32_t z = 224 * -f->lo / (hi - f->lo);
            if (w >= z) cv_rect(8 + z, 70, w - z + 1, 6, VFD);
            else cv_rect(8 + w, 70, z - w, 6, VFD);
            cv_rect(8 + z, 67, 1, 12, VFD);
        } else
            cv_rect(8, 70, w + 1, 6, VFD);
    }
    cv_blit(0, 124);
}

/* the page's part of the screen; 1: it is a voice page (the program page is ui_draw's own) */
static int ui_page_draw(int full)
{
    uint32_t key, h;
    if (ed_pg == PG_PROG) { pg_key_shown = 0xFFFFFFFFu; return 0; }
    key = ed_pg << 16 | ed_slot[ed_pg] << 8 | ed_cur[ed_pg];
    h = pg_hash();
    if (full || key != pg_key_shown) { pg_header(); pg_fields(); pg_focus(); }
    else if (h != pg_hash_shown) { pg_header(); pg_fields(); pg_focus(); }
    pg_key_shown = key;
    pg_hash_shown = h;
    ui_redraw = 0;
    return 1;
}
