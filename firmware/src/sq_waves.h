/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D: the owner's wave bank in the FM-1's flash. The firmware holds no ROM data: the .esqw image (the two wave ROMs,
 * the OS's zone map and its 40 programs, made on the owner's computer by the engine's tools/esq1rom.py) comes over
 * USB-MIDI from tools/esq_syx.py waves, the same protocol the Felucca and JIANT ESQ builds take:
 *   F0 7D 46 4C cmd args F7, the reply F0 7D 46 4C cmd bytes F7 (each byte 7 bit)
 *   96 BEGIN  -> rc: the placeholder plays, the header's sector is erased (the bank is gone from then on)
 *   97 WRITE  offset (3 x 7 bit: >= 256, a multiple of 256), pack7 data (<= 256 bytes) -> offset (3 x 7 bit), rc;
 *             in increasing order: a write at a 4 KiB boundary erases that sector first
 *   98 END    pack7 of u32 length, u32 CRC32 of the image -> rc
 *   99 INFO   -> in flash (0/1), waves, programs, length (3 x 7 bit), USER slots (0: none here)
 *   rc: 0 ok, 1 arguments / not now, 2 erase, 3 CRC / write, 4 flash, 5 not a bank (the placeholder plays)
 * The store (platform.c wv_*: WAVES_AT, WV_SECT sectors): a 16-byte header at 0, the image from 256, read in place
 * through the XIP window. The header is written last: an image that did not arrive whole or fails its CRC is never
 * taken. It stays through firmware updates (the update loader writes only the app area). */
#define WV_HDR 256u
#define WV_MAX (WV_SECT * 4096u - WV_HDR)
#define WV_MAGIC 0x42575145u                    /* "EQWB", as the Felucca build's */
typedef struct { uint32_t magic, len, crc; uint8_t ver, rsv[3]; } wv_hdr_t;
static uint8_t wv_on, wv_tx;                    /* the flash's bank plays; a transfer is open (BEGIN taken) */

#ifdef OBV_HOST
#define SQ_CLI() 0u
#define SQ_STI(f) (void)(f)
#else
#define SQ_CLI() irq_save()
#define SQ_STI(f) irq_restore(f)
#endif

static uint32_t wv_crc32(const uint8_t *p, uint32_t n)   /* (bitwise: no table) */
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* the image in flash (len bytes) or, 0, the placeholder: under the audio */
static int wv_take(uint32_t len)
{
    uint32_t f = SQ_CLI();
    int rc = sq_set_bank(len ? wv_ptr(WV_HDR) : 0, len);
    SQ_STI(f);
    wv_on = len && !rc;
    return rc;
}

/* once at boot, after engine_init: the bank in flash, if it is there and whole */
static void sq_waves_boot(void)
{
    const wv_hdr_t *h;
    wv_map();
    h = (const wv_hdr_t *)wv_ptr(0);
    if (h->magic == WV_MAGIC && h->ver == 1u && h->len && h->len <= WV_MAX && wv_crc32(wv_ptr(WV_HDR), h->len) == h->crc)
        (void)wv_take(h->len);
}

/* the image is in place: checked, taken, its header written last */
static int wv_commit(uint32_t len, uint32_t want)
{
    wv_hdr_t h = {WV_MAGIC, 0, 0, 1, {0, 0, 0}};
    h.len = len;
    if (!len || len > WV_MAX)
        return 1;
    wv_inval();
    h.crc = wv_crc32(wv_ptr(WV_HDR), len);
    if (h.crc != want)
        return 3;
    if (wv_take(len))
        return 5;
    if (wv_prog(0, &h, sizeof h)) {
        (void)wv_take(0);
        return 4;
    }
    wv_inval();
    return 0;
}

static uint32_t wv_unpack7(const uint8_t *a, uint32_t na, uint8_t *out, uint32_t max)
{
    uint32_t n = 0, i, j;
    for (i = 0; i < na; i += 8u) {
        uint32_t hi = a[i];
        for (j = 1; j < 8u && i + j < na && n < max; j++)
            out[n++] = (uint8_t)(a[i + j] | ((hi >> (j - 1u)) & 1u) << 7);
    }
    return n;
}

static uint8_t ed_out[24];
static uint32_t ed_n;
static void ed_b(uint32_t v) { if (ed_n < sizeof ed_out - 1u) ed_out[ed_n++] = (uint8_t)(v & 127u); }
static void ed_send(void);                      /* (below: the device's USB, the host's printout) */

/* one editor command (p: the bytes after F0, without F7). 1: it was ours */
static int sq_ed_handle(const uint8_t *p, uint32_t n)
{
    uint8_t d[256];
    uint32_t cmd, off, len, rc = 0, k;
    const uint8_t *a = p + 4;
    uint32_t na = n - 4u;
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x46u || p[2] != 0x4Cu)
        return 0;
    cmd = p[3];
    if (cmd < 96u || cmd > 99u)
        return 1;                               /* (another editor command: not this firmware's) */
    ed_n = 0;
    ed_out[ed_n++] = 0xF0u;
    ed_b(0x7Du); ed_b(0x46u); ed_b(0x4Cu); ed_b(cmd);
    switch (cmd) {
    case 96:
        ctl_panic = 1;                          /* (the erases stall the audio a moment: nothing should hang on) */
        (void)wv_take(0);
        rc = wv_erase(0) ? 2u : 0u;
        wv_tx = !rc;
        ed_b(rc);
        break;
    case 97:
        if (na < 4u)
            return 1;                           /* (no reply: as the editor's fixed lengths) */
        off = (uint32_t)a[0] | (uint32_t)a[1] << 7 | (uint32_t)a[2] << 14;
        len = wv_unpack7(a + 3, na - 3u, d, sizeof d);
        if (!wv_tx || off < WV_HDR || (off & 0xFFu) || !len || off + len > WV_SECT * 4096u)
            rc = 1;
        else if (!(off & 0xFFFu) && wv_erase(off))
            rc = 2;
        else if (wv_prog(off, d, len))
            rc = 3;
        ed_b(off); ed_b(off >> 7); ed_b(off >> 14); ed_b(rc);
        break;
    case 98:
        if (wv_unpack7(a, na, d, 8u) != 8u || !wv_tx)
            rc = 1;
        else {
            memcpy(&len, d, 4);
            memcpy(&k, d + 4, 4);
            rc = (uint32_t)wv_commit(len, k);
            wv_tx = 0;
        }
        ed_b(rc);
        break;
    default:
        len = wv_on ? ((const wv_hdr_t *)wv_ptr(0))->len : 0u;
        ed_b(wv_on); ed_b(sq_bank.nwaves); ed_b(sq_bank.npatches);
        ed_b(len); ed_b(len >> 7); ed_b(len >> 14);
        ed_b(0);
        break;
    }
    ed_out[ed_n++] = 0xF7u;
    ed_send();
    return 1;
}

#ifdef OBV_HOST
static void ed_send(void) { memcpy(host_ed_last, ed_out, ed_n); host_ed_last_n = ed_n; }
#else
static void ed_send(void) { (void)ota_wire_send(ed_out, ed_n); }
/* platform_poll, before ota_service: an editor frame is ours, the rest stays for the updater */
static void sq_ed_service(void)
{
    const uint8_t *p;
    uint32_t n;
    if (!ota_frame_get(&p, &n))
        return;
    if (n >= 4u && p[0] == 0x7Du && p[1] == 0x46u && p[2] == 0x4Cu) {
        uint8_t f[320];                         /* (a copy: the next frame may arrive while this one is handled) */
        if (n > sizeof f) { ota_frame_done(); return; }
        memcpy(f, p, n);
        ota_frame_done();
        (void)sq_ed_handle(f, n);
    }
}
#endif
