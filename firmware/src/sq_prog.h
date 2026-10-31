/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's program memory (R2), as the ESQ-1 keeps it: banks of 40 in four sets, numbered on screen as the ESQ-1 does
 * (bank 1-4 of ten, program 1-10).
 *   INT, CART A, CART B  40 programs each in the FM-1's flash (platform.c bank_load / bank_save: one sector each, the
 *                        ESQ-1's 102-byte program format), writable. A set never stored holds the ROM's programs (INT)
 *                        or INIT (the carts) until the first WRITE into it.
 *   ROM                  the owner's bank's 40 (the OS's factory programs), read only; without a bank the engine's demos.
 * The edit buffer (sq_edit) is the program playing. A program change loads it; a received one-program dump lands in
 * it (EDITED, as on the ESQ-1); WRITE stores it into a slot. A received 40-program dump replaces INT, as on the ESQ-1.
 *
 * SysEx (the ESQ-1's format, esq/esq_patch.h): F0 0F 02 0c 01 <204 nibbles> F7 one program, F0 0F 02 0c 02 <8160
 * nibbles> F7 all 40, any channel. Received a byte at a time (sq_sx_byte, from usb.c's sysex_byte through
 * APP_SYSEX_BYTE: an all-40 dump is longer than the platform's SysEx frame) into rx.pcb; the main loop takes it when
 * it is whole (sq_sx_poll). Sent on request through the editor protocol (sq_waves.h): 100 a set as an all-40 dump,
 * 101 the edit buffer as a one-program dump (tools/esq_syx.py dump / get). */
#define NSET 4u
#define SET_ROM 3u
#define NSLOT 40u
#define NPROG_ALL (NSET * NSLOT)
static const char *const SET_NAME[NSET] = {"INT", "CART A", "CART B", "ROM"};

static uint8_t sq_pcb[NBANK][NSLOT][ESQ_PCB_LEN] __attribute__((section(".pool")));
static uint32_t sq_stored;                      /* bit i: set i is in flash (else it shows its defaults) */
static uint32_t sq_cur;                         /* the program number playing: set * 40 + slot */
static esq_patch_t sq_edit;                     /* the edit buffer, decoded */
static uint8_t sq_edit_pcb[ESQ_PCB_LEN];        /* .. and as its 102 bytes: the program as stored or received, every bit
                                                 * (the engine's decoding drops a few the ESQ-1 keeps: stored, sent and
                                                 * written from here, never re-encoded) */
static uint8_t sq_edited;                       /* the edit buffer differs from its slot */

/* a program's 102 bytes as stored: a set's from RAM, ROM's from the bank (else the demos, encoded) */
static void prog_pcb(uint32_t g, uint8_t *pcb)
{
    uint32_t set = g / NSLOT % NSET, k = g % NSLOT;
    if (set != SET_ROM) memcpy(pcb, sq_pcb[set][k], ESQ_PCB_LEN);
    else if (sq_bank.patches && k < sq_bank.npatches) memcpy(pcb, sq_bank.patches + ESQ_PCB_LEN * k, ESQ_PCB_LEN);
    else esq_patch_to_pcb(&ESQ_DEMO_PATCHES[k < SQ_NDEMO ? k : 0u], pcb);   /* (no bank: the demos, then INIT) */
}
static void prog_get(uint32_t g, esq_patch_t *p)
{
    uint8_t pcb[ESQ_PCB_LEN];
    prog_pcb(g, pcb);
    esq_patch_from_pcb(p, pcb);
}
static void prog_name(uint32_t g, char *out7)  /* 6 characters + NUL */
{
    esq_patch_t p;
    uint32_t i;
    prog_get(g, &p);
    for (i = 0; i < 6u && p.name[i]; i++) out7[i] = p.name[i];
    for (; i < 6u; i++) out7[i] = ' ';
    out7[6] = 0;
}

static void ed_track_lost(void);                /* (sq_edit.h) */
static void edit_apply(void)                    /* the edit buffer to the engine */
{
    uint32_t f = SQ_CLI();
    sq_pend = sq_edit;
    SQ_STI(f);
    sq_event(EV_PROG, 0, 0, 0);
}
static void prog_load(uint32_t g)
{
    sq_cur = g % NPROG_ALL;
    prog_pcb(sq_cur, sq_edit_pcb);
    esq_patch_from_pcb(&sq_edit, sq_edit_pcb);
    ed_track_lost();
    sq_edited = 0;
    edit_apply();
}

/* a set's defaults, while it was never stored: INT the ROM's programs, a cart INIT throughout */
static void set_defaults(uint32_t set)
{
    uint32_t k;
    for (k = 0; k < NSLOT; k++)
        if (set == 0u) prog_pcb(SET_ROM * NSLOT + k, sq_pcb[set][k]);
        else esq_patch_to_pcb(&ESQ_DEMO_PATCHES[0], sq_pcb[set][k]);
}
static int set_store(uint32_t set)            /* 0: in flash (the erase stalls the audio ~50 ms) */
{
    int rc = bank_save(set, sq_pcb[set], NSLOT * ESQ_PCB_LEN);
    if (!rc) sq_stored |= 1u << set;
    return rc;
}

static void banks_boot(void)
{
    uint32_t set;
    for (set = 0; set < NBANK; set++)
        if (!bank_load(set, sq_pcb[set], NSLOT * ESQ_PCB_LEN)) sq_stored |= 1u << set;
        else set_defaults(set);
}
/* the wave bank changed (installed, or gone): the sets never stored follow it, and the program playing is loaded
 * again (a ROM one is another program now) */
static void banks_follow(void)
{
    uint32_t set;
    for (set = 0; set < NBANK; set++)
        if (!(sq_stored >> set & 1u)) set_defaults(set);
    prog_load(sq_cur);
}

/* WRITE: the edit buffer into a writable slot, at once in flash. 0: done */
static int prog_write(uint32_t g)
{
    uint32_t set = g / NSLOT % NSET, k = g % NSLOT;
    if (set == SET_ROM) return -1;
    memcpy(sq_pcb[set][k], sq_edit_pcb, ESQ_PCB_LEN);
    if (set_store(set)) return -2;
    sq_cur = g;
    sq_edited = 0;
    return 0;
}

/* ---- SysEx in: the ESQ-1's dumps, a byte at a time (context: the USB poll, in TIMER5 / the audio ISR) */
enum { SX_IDLE, SX_HDR, SX_DATA, SX_SKIP };
/* (after Felucca-ESQ v6.5.2) a dump survives another sender's whole SysEx message inside it (F0 .. F7 from another app
 * on the computer: CoreMIDI interleaves them between a long message's packets): stepped over. A broken dump says why */
enum { SXW_NONE, SXW_LENGTH, SXW_BYTE, SXW_CUT };
static struct {
    uint8_t pcb[NSLOT][ESQ_PCB_LEN];            /* the programs as received */
    uint16_t nib, want, at;                     /* nibbles so far / the message's; (broken) where */
    uint8_t st, hdr, lo, code, nest, why;
    uint32_t t_ms;                              /* the last byte's time */
    volatile uint8_t ready;                     /* 1 one program, 2 all 40, 3 a broken ESQ-1 dump: the main loop's turn */
} rx __attribute__((section(".pool")));

static void sq_sx_byte(uint32_t b)
{
    uint32_t gap;
    if (b >= 0xF8u) return;                     /* realtime may come anywhere */
    gap = fm1_ms - rx.t_ms;
    rx.t_ms = fm1_ms;
    if (rx.st == SX_DATA && (b == 0xF0u || rx.nest) && (b != 0xF0u || gap < 1000u)) {
        if (b == 0xF0u || b == 0xF7u) rx.nest = b == 0xF0u;   /* another sender's message inside the dump */
        return;                                 /* (an F0 after a second's silence: a new message, this one abandoned) */
    }
    if (b == 0xF0u) { rx.st = rx.ready ? SX_SKIP : SX_HDR; rx.hdr = 0; rx.nest = 0; return; }
    if (b & 0x80u) {                            /* F7 or any status: the message ends */
        if (rx.st == SX_DATA) {
            int whole = b == 0xF7u && rx.nib == rx.want;
            if (!whole) {
                if (rx.nib == 0xFFFFu) rx.why = SXW_BYTE;
                else { rx.why = b == 0xF7u ? SXW_LENGTH : SXW_CUT; rx.at = rx.nib; }
            }
            RING_PUBLISH();
            rx.ready = whole ? rx.code : 3u;
        }
        rx.st = SX_IDLE;
        return;
    }
    switch (rx.st) {
    case SX_HDR:
        if ((rx.hdr == 0 && b != 0x0Fu) || (rx.hdr == 1u && b != 0x02u) || (rx.hdr == 2u && b > 0x0Fu) ||
            (rx.hdr == 3u && b != 0x01u && b != 0x02u)) { rx.st = SX_SKIP; return; }   /* another maker's, another message */
        if (++rx.hdr == 4u) {
            rx.code = (uint8_t)b;
            rx.want = (uint16_t)(b == 0x01u ? 2u * ESQ_PCB_LEN : 2u * ESQ_PCB_LEN * NSLOT);
            rx.nib = 0;
            rx.st = SX_DATA;
        }
        return;
    case SX_DATA:
        if (b > 0x0Fu || rx.nib >= rx.want) {   /* not a nibble, or one too many */
            if (rx.nib != 0xFFFFu) rx.at = rx.nib;
            rx.nib = 0xFFFFu;
            return;
        }
        if (!(rx.nib & 1u)) rx.lo = (uint8_t)b;
        else rx.pcb[rx.nib / (2u * ESQ_PCB_LEN)][(rx.nib >> 1) % ESQ_PCB_LEN] = (uint8_t)(rx.lo | b << 4);
        rx.nib++;
        return;
    default:
        return;
    }
}

static void ui_say(const char *a, const char *b); /* (sq_ui.h) */
static int imp_dest(void);                      /* (sq_sys.h) IMPORT: the set a 40-program dump goes into, -1: ask */
static uint8_t sx_dest_once;                    /* (editor command 102) the next 40-program dump: into this set + 1 */
static uint32_t sx_import;                      /* a 40-program dump waiting for an answer: 1 + the set proposed */

/* a waiting 40-program dump into a set (0..2), stored */
static void sx_import_to(uint32_t set)
{
    static const char *const INTO[3] = {"INTO INT", "INTO CART A", "INTO CART B"};
    set %= NBANK;
    memcpy(sq_pcb[set], rx.pcb, sizeof rx.pcb);
    ui_say("40 PROGRAMS", set_store(set) ? "NOT STORED" : INTO[set]);
    if (sq_cur / NSLOT == set) prog_load(sq_cur);
    sx_import = 0;
    RING_PUBLISH();
    rx.ready = 0;
}
static void sx_import_cancel(void)
{
    sx_import = 0;
    ui_say("IMPORT", "CANCELLED");
    RING_PUBLISH();
    rx.ready = 0;
}

/* main loop: a whole dump that arrived */
static void sq_sx_poll(void)
{
    uint32_t r = rx.ready;
    if (!r) return;
    RING_PUBLISH();
    if (r == 1u) {                              /* one program: the edit buffer, EDITED */
        memcpy(sq_edit_pcb, rx.pcb[0], ESQ_PCB_LEN);
        esq_patch_from_pcb(&sq_edit, sq_edit_pcb);
        ed_track_lost();
        sq_edited = 1;
        edit_apply();
        ui_say("PROGRAM RECEIVED", "SAVE TO KEEP IT");
    } else if (r == 2u) {                       /* all 40: into the set IMPORT says, or ask (it waits meanwhile) */
        int d = sx_dest_once ? (int)sx_dest_once - 1 : imp_dest();
        if (sx_import) return;
        sx_dest_once = 0;
        if (d < 0) { sx_import = 1; return; }
        sx_import_to((uint32_t)d);
        return;
    } else {                                    /* why, and where: "LENGTH 8100/8160", "BAD BYTE AT 512", "CUT AT 300" */
        char t[20];
        uint32_t i = 0, v, k;
        const char *w = rx.why == SXW_LENGTH ? "LENGTH " : rx.why == SXW_BYTE ? "BAD BYTE AT " : "CUT AT ";
        while (*w) t[i++] = *w++;
        for (k = 0; k < 2u; k++) {
            char d[6];
            uint32_t n = 0;
            v = k ? rx.want : rx.at;
            do { d[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
            while (n && i < sizeof t - 1u) t[i++] = d[--n];
            if (k || rx.why != SXW_LENGTH) break;
            t[i++] = '/';
        }
        t[i] = 0;
        ui_say("DUMP BROKEN", t);
    }
    RING_PUBLISH();
    rx.ready = 0;
}

/* ---- SysEx out (editor commands 100, 101; sq_waves.h hands them over) */
static uint8_t sx_out[5u + 2u * ESQ_PCB_LEN * NSLOT + 1u] __attribute__((section(".pool")));
static void sq_send_raw(const uint8_t *p, uint32_t n);   /* (sq1d.c: the device's USB, the host's file) */
static uint32_t sx_put(uint32_t o, const uint8_t *pcb)
{
    uint32_t i;
    for (i = 0; i < ESQ_PCB_LEN; i++) { sx_out[o++] = pcb[i] & 15u; sx_out[o++] = pcb[i] >> 4; }
    return o;
}
static int sq_ed_more(uint32_t cmd, const uint8_t *a, uint32_t na)
{
    uint32_t o = 5, k;
    sx_out[0] = 0xF0u; sx_out[1] = 0x0Fu; sx_out[2] = 0x02u; sx_out[3] = 0;
    if (cmd == 100u && na >= 1u) {              /* a set (0 INT, 1 CART A, 2 CART B, 3 ROM): all 40 */
        uint32_t set = a[0] % NSET;
        sx_out[4] = 0x02u;
        for (k = 0; k < NSLOT; k++) {
            uint8_t pcb[ESQ_PCB_LEN];
            prog_pcb(set * NSLOT + k, pcb);
            o = sx_put(o, pcb);
        }
    } else if (cmd == 101u) {                   /* the edit buffer */
        sx_out[4] = 0x01u;
        o = sx_put(o, sq_edit_pcb);
    } else if (cmd == 102u && na >= 1u) {       /* (R4) the next 40-program dump into set a[0] (0..2), not asked */
        sx_dest_once = (uint8_t)(a[0] % NBANK + 1u);
        return 1;
    } else
        return 0;
    sx_out[o++] = 0xF7u;
    sq_send_raw(sx_out, o);
    return 1;
}
