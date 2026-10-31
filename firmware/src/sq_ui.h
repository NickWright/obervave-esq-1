/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's screen: the program select page as the ESQ-1 shows a bank, in a vacuum-fluorescent cyan. The header (the
 * set, the octave), the program playing in large letters, its bank of ten in two columns of five (the ESQ-1's ten
 * soft buttons), the voices at the foot. WRITE takes the program block and the bank for the slot it writes to; a
 * message (a received dump) takes the program block for 2 s. Drawn in strips, each only when what it shows changed. */
/* the colours: VFD (cyan, the original's display) or (R5) MODERN; the names stay the VFD's */
static uint16_t ui_pal[5] = {RGB(110, 236, 255), RGB(26, 70, 82), RGB(2, 10, 14), RGB(110, 236, 255), RGB(110, 236, 255)};
#define VFD ui_pal[0]                           /* lit */
#define VFD_DIM ui_pal[1]                       /* unlit segments, rules */
#define VFD_BG ui_pal[2]
#define VFD_SEL_BG ui_pal[3]
#define UI_ACC ui_pal[4]                        /* (MODERN) the graphs' colour */
static uint32_t ui_modern;
static uint32_t ui_mode_shown = 99u;
static void ui_set_look(uint32_t modern)
{
    static const uint16_t P[2][5] = {{RGB(110, 236, 255), RGB(26, 70, 82), RGB(2, 10, 14), RGB(110, 236, 255), RGB(110, 236, 255)},
                                     {RGB(236, 238, 242), RGB(70, 76, 90), RGB(16, 18, 24), RGB(255, 150, 50), RGB(255, 150, 50)}};
    uint32_t i;
    ui_modern = modern != 0;
    for (i = 0; i < 5u; i++) ui_pal[i] = P[ui_modern][i];
    ui_mode_shown = 99u;                        /* (everything again) */
}

static int32_t ui_oct;
static uint32_t ui_write;                       /* WRITE: 0 off, else 1 + the target program number */
static uint32_t ui_shown_key = 0xFFFFFFFFu, ui_hdr_key = 0xFFFFFFFFu;
static uint8_t ui_lv_shown[ESQ_NVOICE];
static uint32_t ui_us_shown = 0xFFFFFFFFu, ui_irq_shown, ui_redraw;
static char ui_msg[2][20];
static uint32_t ui_msg_until;

static void ui_fmt_u(char *s, uint32_t v)       /* v in decimal, appended */
{
    char t[11];
    uint32_t n = 0;
    while (*s) s++;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < sizeof t);
    while (n) *s++ = t[--n];
    *s = 0;
}
static void ui_cat(char *s, const char *a) { while (*s) s++; while (*a) *s++ = *a++; *s = 0; }
static void ui_cpy(char *d, const char *a, uint32_t n) { while (*a && n > 1u) { *d++ = *a++; n--; } *d = 0; }

static void ui_say(const char *a, const char *b)
{
    ui_cpy(ui_msg[0], a, sizeof ui_msg[0]);
    ui_cpy(ui_msg[1], b, sizeof ui_msg[1]);
    ui_msg_until = fm1_ms + 2000u;
    ui_redraw = 1;
}

/* "INT  BANK 2  PROG 3" for a program number */
static void ui_where(char *t, uint32_t g)
{
    t[0] = 0;
    ui_cat(t, SET_NAME[g / NSLOT % NSET]);
    ui_cat(t, "  BANK ");
    ui_fmt_u(t, g % NSLOT / 10u + 1u);
    ui_cat(t, "  PROG ");
    ui_fmt_u(t, g % 10u + 1u);
}

static uint32_t pl_state(void);                 /* (sq_play.h) bits: ARP, LATCH, CHORD, PLAY, REC */
static uint32_t ed_cmp_now(void);               /* (sq_edit.h) COMPARE is on */

static void ui_header(void)
{
    char t[24];
    uint32_t st = pl_state(), i;
    cv_begin(240, 24, VFD_BG);
    cv_text(6, 4, &FONT_S, "SQ1D", VFD);
    cv_text(64, 4, &FONT_S, ui_write ? "WRITE" : sx_import ? "IMPORT" : SET_NAME[sq_cur / NSLOT % NSET],
            ui_write || sx_import ? VFD : VFD_DIM);
    t[0] = 0;                                   /* what plays besides the keys: A arp, L latch, C chord, > seq, R rec */
    for (i = 0; i < 5u; i++) if (st >> i & 1u) { char c[2] = {"ALC>R"[i], 0}; ui_cat(t, c); }
    cv_text(136, 4, &FONT_S, t, VFD);
    t[0] = 0;
    ui_cat(t, "OCT ");
    if (ui_oct < 0) { ui_cat(t, "-"); ui_fmt_u(t, (uint32_t)-ui_oct); }
    else { ui_cat(t, ui_oct ? "+" : " "); ui_fmt_u(t, (uint32_t)ui_oct); }
    cv_text(240 - 6 - text_w(&FONT_S, t), 4, &FONT_S, t, VFD);
    cv_rect(0, 23, 240, 1, VFD_DIM);
    cv_blit(0, 0);
}

/* the program block: the program playing (EDITED marked), WRITE's question, or a message */
static void ui_program(int msg)
{
    char nm[7], t[32];
    cv_begin(240, 76, VFD_BG);
    if (msg && !sx_import && !ui_write) {      /* (a question outranks a message) */
        cv_text((240 - text_w(&FONT_S, ui_msg[0])) / 2, 16, &FONT_S, ui_msg[0], VFD);
        cv_text((240 - text_w(&FONT_S, ui_msg[1])) / 2, 42, &FONT_S, ui_msg[1], VFD_DIM);
    } else if (sx_import) {                    /* a 40-program dump in: where to (IMPORT ASK) */
        cv_text(8, 4, &FONT_S, "IMPORT 40 PROGRAMS", VFD);
        cv_text(8, 26, &FONT_S, "INTO", VFD_DIM);
        cv_text(56, 26, &FONT_S, SET_NAME[(sx_import - 1u) % 3u], VFD);
        cv_text(8, 50, &FONT_S, "OCT+: YES    OCT-: NO", VFD_DIM);
    } else if (ui_write) {
        uint32_t g = ui_write - 1u;
        cv_text(8, 4, &FONT_S, "WRITE", VFD);
        cv_text(64, 4, &FONT_S, sq_edit.name, VFD);
        cv_text(8, 26, &FONT_S, "TO", VFD_DIM);
        ui_where(t, g);
        cv_text(40, 26, &FONT_S, t, VFD);
        cv_text(8, 50, &FONT_S, "OCT+: YES    OCT-: NO", VFD_DIM);
    } else {
        ui_where(t, sq_cur);
        if (ed_cmp_now()) ui_cat(t, "  COMPARE");
        else if (sq_edited) ui_cat(t, "  C*");   /* (the ESQ-1's change marker) */
        cv_text(8, 6, &FONT_S, t, VFD_DIM);
        ui_cpy(nm, sq_edit.name, sizeof nm);
        cv_text((240 - text_w(&FONT_L, nm)) / 2, 30, &FONT_L, nm, VFD);
    }
    cv_blit(0, 24);
}

/* a bank of ten: two columns of five, the cursor (the program playing, or WRITE's slot) lit */
static void ui_bank(uint32_t cur)
{
    uint32_t b0 = cur / 10u * 10u, k;
    cv_begin(240, 110, VFD_BG);
    cv_rect(0, 0, 240, 1, VFD_DIM);
    for (k = 0; k < 10u; k++) {
        uint32_t g = b0 + k, sel = g == cur;
        int32_t x = k < 5u ? 4 : 124, y = 6 + (int32_t)(k % 5u) * 20;
        char nm[7], t[12];
        if (sx_import) {                        /* (the dump's own programs) */
            esq_patch_t pp;
            uint32_t j;
            esq_patch_from_pcb(&pp, rx.pcb[g % NSLOT]);
            for (j = 0; j < 6u && pp.name[j]; j++) nm[j] = pp.name[j];
            for (; j < 6u; j++) nm[j] = ' ';
            nm[6] = 0;
        } else
            prog_name(g, nm);
        t[0] = 0;
        ui_fmt_u(t, k + 1u);
        if (sel) cv_rect(x - 2, y - 1, 114, 18, VFD_SEL_BG);
        cv_text(x + (k + 1u < 10u ? 8 : 0), y, &FONT_S, t, sel ? VFD_BG : VFD_DIM);
        cv_text(x + 28, y, &FONT_S, nm, sel ? VFD_BG : VFD);
    }
    cv_blit(0, 100);
}

/* the foot: the eight voices' ENV4 as bars (the eight voice cards), the render time */
static void ui_voices(void)
{
    uint32_t i;
    char t[16];
    cv_begin(240, 30, VFD_BG);
    cv_rect(0, 0, 240, 1, VFD_DIM);
    for (i = 0; i < ESQ_NVOICE; i++) {
        int32_t h = (ui_lv_shown[i] * 18) / 127;
        cv_rect(6 + (int32_t)i * 16, 6, 12, 20, VFD_DIM);
        if (h) cv_rect(6 + (int32_t)i * 16, 26 - h, 12, h, VFD);
    }
    t[0] = 0;                                   /* the render + the interrupts inside it, us (of 5805) */
    ui_fmt_u(t, ui_us_shown);
    ui_cat(t, "+");
    ui_fmt_u(t, ui_irq_us);
    ui_cat(t, "us");
    cv_text(240 - 6 - text_w(&FONT_S, t), 8, &FONT_S, t, VFD_DIM);
    cv_blit(0, 210);
}

static uint32_t ed_pg_now(void);                /* (sq_edit.h: the page, PG_PROG = 0 the program page) */
static int ui_page_draw(int full);              /* (sq_ui_page.h) */
static void ui_draw(int full)
{
    uint32_t i, changed = 0, msg = fm1_ms < ui_msg_until, cur = ui_write ? ui_write - 1u : sq_cur;
    uint32_t key = cur | (uint32_t)msg << 9 | (uint32_t)(ui_write != 0) << 10 | (uint32_t)sq_edited << 11 | sx_import << 12 |
                   ed_cmp_now() << 13;
    uint32_t hkey = (uint32_t)(ui_oct + 8) | (cur / NSLOT) << 4 | (uint32_t)(ui_write != 0) << 8 | pl_state() << 9 |
                    (uint32_t)(sx_import != 0) << 14;
    uint32_t mode = ed_pg_now() && !ui_write && !sx_import;   /* a voice page (a message in its focus area), or the program page */
    if (mode != ui_mode_shown) { ui_mode_shown = mode; full = 1; }
    if (full) { lcd_fill(0, 0, 240, 240, VFD_BG); ui_hdr_key = ui_shown_key = 0xFFFFFFFFu; }
    if (mode) (void)ui_page_draw(full);
    else {
        if (hkey != ui_hdr_key) { ui_hdr_key = hkey; ui_header(); }
        if (ui_redraw || key != ui_shown_key) {
            ui_shown_key = key;
            ui_redraw = 0;
            ui_program((int)msg);
            ui_bank(cur);
        }
    }
    for (i = 0; i < ESQ_NVOICE; i++)
        if (ui_vlevel[i] != ui_lv_shown[i]) { ui_lv_shown[i] = ui_vlevel[i]; changed = 1; }
    if (ui_render_us != ui_us_shown || ui_irq_us != ui_irq_shown) { ui_us_shown = ui_render_us; ui_irq_shown = ui_irq_us; changed = 1; }
    if (changed || full) ui_voices();
}
