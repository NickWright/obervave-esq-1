/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's screen (R1): the program select page as the ESQ-1 shows a bank, in a vacuum-fluorescent cyan. The
 * header, the program playing in large letters, its bank of ten in two columns of five (the ESQ-1's ten soft
 * buttons), the voices at the foot. Drawn in strips, each only when what it shows changed. */
#define VFD RGB(110, 236, 255)                  /* lit */
#define VFD_DIM RGB(26, 70, 82)                 /* unlit segments, rules */
#define VFD_BG RGB(2, 10, 14)
#define VFD_SEL_BG RGB(110, 236, 255)

static uint32_t ui_prog, ui_oct_shown = 99, ui_prog_shown = 0xFFFFFFFFu;
static uint32_t ui_nprog_shown, ui_flash_shown = 2;
static int32_t ui_oct;
static uint8_t ui_lv_shown[ESQ_NVOICE];
static uint32_t ui_us_shown = 0xFFFFFFFFu;

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

/* the header: name, where the programs come from, the octave */
static void ui_header(void)
{
    char t[24];
    cv_begin(240, 24, VFD_BG);
    cv_text(6, 4, &FONT_S, "SQ1D", VFD);
    t[0] = 0;
    ui_cat(t, sq_bank.npatches ? "ROM BANK" : "DEMO");
    cv_text(64, 4, &FONT_S, t, VFD_DIM);
    t[0] = 0;
    ui_cat(t, "OCT ");
    if (ui_oct < 0) { ui_cat(t, "-"); ui_fmt_u(t, (uint32_t)-ui_oct); }
    else { ui_cat(t, ui_oct ? "+" : " "); ui_fmt_u(t, (uint32_t)ui_oct); }
    cv_text(240 - 6 - text_w(&FONT_S, t), 4, &FONT_S, t, VFD);
    cv_rect(0, 23, 240, 1, VFD_DIM);
    cv_blit(0, 0);
}

/* the program playing: its number as the ESQ-1 counts (bank 1-4, slot 1-10) and its name */
static void ui_program(void)
{
    char nm[7], t[16];
    sq_program_name(ui_prog, nm);
    cv_begin(240, 76, VFD_BG);
    t[0] = 0;
    ui_cat(t, "BANK ");
    ui_fmt_u(t, ui_prog / 10u + 1u);
    ui_cat(t, "  PROG ");
    ui_fmt_u(t, ui_prog % 10u + 1u);
    cv_text(8, 6, &FONT_S, t, VFD_DIM);
    cv_text((240 - text_w(&FONT_L, nm)) / 2, 30, &FONT_L, nm, VFD);
    cv_blit(0, 24);
}

/* the program's bank of ten: two columns of five, the program playing lit as the ESQ-1's cursor */
static void ui_bank(void)
{
    uint32_t b = ui_prog / 10u, k;
    cv_begin(240, 110, VFD_BG);
    cv_rect(0, 0, 240, 1, VFD_DIM);
    for (k = 0; k < 10u; k++) {
        uint32_t n = b * 10u + k, sel = n == ui_prog;
        int32_t x = k < 5u ? 4 : 124, y = 6 + (int32_t)(k % 5u) * 20;
        char nm[7], t[12];
        if (n >= sq_nprog) continue;
        sq_program_name(n, nm);
        t[0] = 0;
        ui_fmt_u(t, k + 1u);
        if (sel) cv_rect(x - 2, y - 1, 114, 18, VFD_SEL_BG);
        cv_text(x + (k + 1u < 10u ? 8 : 0), y, &FONT_S, t, sel ? VFD_BG : VFD_DIM);
        cv_text(x + 28, y, &FONT_S, nm, sel ? VFD_BG : VFD);
    }
    cv_blit(0, 100);
}

/* the foot: the eight voices' ENV4 as bars (the ESQ-1 has eight voice cards), the render time */
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
    t[0] = 0;
    ui_fmt_u(t, ui_us_shown);
    ui_cat(t, " us");
    cv_text(240 - 6 - text_w(&FONT_S, t), 8, &FONT_S, t, VFD_DIM);
    cv_blit(0, 210);
}

static void ui_draw(int full)
{
    uint32_t i, changed = 0;
    if (full) { lcd_fill(0, 0, 240, 240, VFD_BG); ui_oct_shown = 99; ui_prog_shown = 0xFFFFFFFFu; }
    if ((uint32_t)(ui_oct + 50) != ui_oct_shown || ui_flash_shown != sq_bank.npatches) {
        ui_oct_shown = (uint32_t)(ui_oct + 50);
        ui_flash_shown = sq_bank.npatches;
        ui_header();
    }
    if (ui_prog != ui_prog_shown || ui_nprog_shown != sq_nprog) {   /* (the bank's cursor moves with it) */
        ui_prog_shown = ui_prog;
        ui_nprog_shown = sq_nprog;
        ui_program();
        ui_bank();
    }
    for (i = 0; i < ESQ_NVOICE; i++)
        if (ui_vlevel[i] != ui_lv_shown[i]) { ui_lv_shown[i] = ui_vlevel[i]; changed = 1; }
    if (ui_render_us != ui_us_shown) { ui_us_shown = ui_render_us; changed = 1; }
    if (changed || full) ui_voices();
}
