/* ESQ-1 programs: PCB format and SysEx (see esq_patch.h). */
#include "esq_patch.h"

#if !defined(ESQ_HOST) || ESQ_HOST
#include <string.h>
#endif

static int esqp_s8(uint8_t b) { return b >= 128 ? (int)b - 256 : (int)b; }
static int esqp_s7(uint8_t b) { b &= 0x7F; return b >= 64 ? (int)b - 128 : (int)b; }
static int esqp_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int8_t esqp_amt2(uint8_t b) { return (int8_t)esqp_clamp(esqp_s8(b) / 2, -63, 63); }   /* signed, 2 x value */
static uint8_t esqp_put2(int v) { return (uint8_t)(esqp_clamp(v, -63, 63) * 2); }
static uint8_t esqp_put7(int v) { return (uint8_t)(esqp_clamp(v, -63, 63) & 0x7F); }

void esq_patch_from_pcb(esq_patch_t *p, const uint8_t *b)
{
    int i;
    memset(p, 0, sizeof(*p));
    for (i = 0; i < 6; i++)
        p->name[i] = b[i] >= 32 && b[i] < 127 ? (char)b[i] : ' ';
    for (i = 5; i >= 0 && p->name[i] == ' '; i--)
        p->name[i] = 0;
    for (i = 0; i < 4; i++) {
        const uint8_t *e = b + 6 + 10 * i;
        esq_env_t *d = &p->env[i];
        d->l1 = esqp_amt2(e[0]);
        d->l2 = esqp_amt2(e[1]);
        d->l3 = esqp_amt2(e[2]);
        d->t1 = e[3] & 63;
        d->t2 = e[4] & 63;
        d->t3 = e[5] & 63;
        d->t4 = e[6] & 63;
        d->lv = e[7] >> 2;
        d->t1v = e[8] & 63;
        d->tk = e[9] & 63;
    }
    for (i = 0; i < 3; i++) {
        const uint8_t *l = b + 46 + 4 * i;
        esq_lfo_t *d = &p->lfo[i];
        d->wave = l[0] >> 6;
        d->freq = l[0] & 63;
        d->mod_src = (uint8_t)((l[1] >> 6) << 2 | l[2] >> 6);
        d->l1 = l[1] & 63;
        d->l2 = l[2] & 63;
        d->reset = l[3] >> 7;
        d->human = (l[3] >> 6) & 1;
        d->delay = l[3] & 63;
    }
    for (i = 0; i < 3; i++) {
        const uint8_t *o = b + 58 + 10 * i;
        p->osc[i].semi = (int8_t)(esqp_clamp(o[0], 0, 72) - 36);
        p->osc[i].fine = o[1] >> 3;
        p->osc[i].fm[0].src = o[2] & 15;
        p->osc[i].fm[1].src = o[2] >> 4;
        p->osc[i].fm[0].amt = esqp_amt2(o[3]);
        p->osc[i].fm[1].amt = esqp_amt2(o[4]);
        p->osc[i].wave = o[5];
        p->dca[i].on = o[6] >> 7;
        p->dca[i].level = (o[6] & 0x7F) >> 1;
        p->dca[i].am[0].src = o[7] & 15;
        p->dca[i].am[1].src = o[7] >> 4;
        p->dca[i].am[0].amt = esqp_amt2(o[8]);
        p->dca[i].am[1].amt = esqp_amt2(o[9]);
    }
    p->modes.am = b[88] >> 7;
    p->dca4.level = (b[88] & 0x7F) >> 1;
    p->modes.sync = b[89] >> 7;
    p->flt.freq = b[89] & 0x7F;
    p->flt.q = b[90] & 31;
    p->flt.mod[0].src = b[91] & 15;
    p->flt.mod[1].src = b[91] >> 4;
    p->modes.vc = b[92] >> 7;
    p->flt.mod[0].amt = (int8_t)esqp_s7(b[92]);
    p->modes.mono = b[93] >> 7;
    p->flt.mod[1].amt = (int8_t)esqp_s7(b[93]);
    p->modes.env_reset = b[94] >> 7;
    p->flt.keybd = (b[94] & 0x7F) >> 1;
    p->modes.osc_reset = b[95] >> 7;
    p->modes.glide = b[95] & 63;
    p->split.split_dir = b[96] >> 7;
    p->split.split_point = b[96] & 0x7F;
    p->split.layer = b[97] >> 7;
    p->split.layer_prog = b[97] & 0x7F;
    p->split.split = b[98] >> 7;
    p->split.split_prog = b[98] & 0x7F;
    p->split.split_layer = b[99] >> 7;
    p->split.split_layer_prog = b[99] & 0x7F;
    p->dca4.pan = b[100] >> 4;
    p->dca4.panmod.src = b[100] & 15;
    p->modes.cycle = b[101] >> 7;
    p->dca4.panmod.amt = (int8_t)esqp_s7(b[101]);
}

void esq_patch_to_pcb(const esq_patch_t *p, uint8_t *b)
{
    int i;
    memset(b, 0, ESQ_PCB_LEN);
    for (i = 0; i < 6; i++)
        b[i] = p->name[i] >= 32 && p->name[i] < 127 ? (uint8_t)p->name[i] : ' ';
    for (i = 0; i < 6 && p->name[i]; i++)
        ;
    for (; i < 6; i++)
        b[i] = ' ';
    for (i = 0; i < 4; i++) {
        uint8_t *e = b + 6 + 10 * i;
        const esq_env_t *d = &p->env[i];
        e[0] = esqp_put2(d->l1);
        e[1] = esqp_put2(d->l2);
        e[2] = esqp_put2(d->l3);
        e[3] = d->t1 & 63;
        e[4] = d->t2 & 63;
        e[5] = d->t3 & 63;
        e[6] = d->t4 & 63;
        e[7] = (uint8_t)((d->lv & 63) << 2 | (d->lv ? 2 : 0));
        e[8] = d->t1v & 63;
        e[9] = d->tk & 63;
    }
    for (i = 0; i < 3; i++) {
        uint8_t *l = b + 46 + 4 * i;
        const esq_lfo_t *d = &p->lfo[i];
        l[0] = (uint8_t)((d->wave & 3) << 6 | (d->freq & 63));
        l[1] = (uint8_t)(((d->mod_src >> 2) & 3) << 6 | (d->l1 & 63));
        l[2] = (uint8_t)((d->mod_src & 3) << 6 | (d->l2 & 63));
        l[3] = (uint8_t)((d->reset ? 0x80 : 0) | (d->human ? 0x40 : 0) | (d->delay & 63));
    }
    for (i = 0; i < 3; i++) {
        uint8_t *o = b + 58 + 10 * i;
        o[0] = (uint8_t)esqp_clamp(p->osc[i].semi + 36, 0, 72);
        o[1] = (uint8_t)((p->osc[i].fine & 31) << 3);
        o[2] = (uint8_t)((p->osc[i].fm[1].src & 15) << 4 | (p->osc[i].fm[0].src & 15));
        o[3] = esqp_put2(p->osc[i].fm[0].amt);
        o[4] = esqp_put2(p->osc[i].fm[1].amt);
        o[5] = p->osc[i].wave;
        o[6] = (uint8_t)((p->dca[i].on ? 0x80 : 0) | (esqp_clamp(p->dca[i].level, 0, 63) << 1));
        o[7] = (uint8_t)((p->dca[i].am[1].src & 15) << 4 | (p->dca[i].am[0].src & 15));
        o[8] = esqp_put2(p->dca[i].am[0].amt);
        o[9] = esqp_put2(p->dca[i].am[1].amt);
    }
    b[88] = (uint8_t)((p->modes.am ? 0x80 : 0) | (esqp_clamp(p->dca4.level, 0, 63) << 1));
    b[89] = (uint8_t)((p->modes.sync ? 0x80 : 0) | (p->flt.freq & 0x7F));
    b[90] = p->flt.q & 31;
    b[91] = (uint8_t)((p->flt.mod[1].src & 15) << 4 | (p->flt.mod[0].src & 15));
    b[92] = (uint8_t)((p->modes.vc ? 0x80 : 0) | esqp_put7(p->flt.mod[0].amt));
    b[93] = (uint8_t)((p->modes.mono ? 0x80 : 0) | esqp_put7(p->flt.mod[1].amt));
    b[94] = (uint8_t)((p->modes.env_reset ? 0x80 : 0) | (esqp_clamp(p->flt.keybd, 0, 63) << 1));
    b[95] = (uint8_t)((p->modes.osc_reset ? 0x80 : 0) | (p->modes.glide & 63));
    b[96] = (uint8_t)((p->split.split_dir ? 0x80 : 0) | (p->split.split_point & 0x7F));
    b[97] = (uint8_t)((p->split.layer ? 0x80 : 0) | (p->split.layer_prog & 0x7F));
    b[98] = (uint8_t)((p->split.split ? 0x80 : 0) | (p->split.split_prog & 0x7F));
    b[99] = (uint8_t)((p->split.split_layer ? 0x80 : 0) | (p->split.split_layer_prog & 0x7F));
    b[100] = (uint8_t)((p->dca4.pan & 15) << 4 | (p->dca4.panmod.src & 15));
    b[101] = (uint8_t)((p->modes.cycle ? 0x80 : 0) | esqp_put7(p->dca4.panmod.amt));
}

/* n bytes from 2n nibbles, low nibble first */
static void esqp_denib(const uint8_t *src, uint8_t *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        dst[i] = (uint8_t)((src[2 * i] & 15) | (src[2 * i + 1] & 15) << 4);
}

int esq_sysex_parse(const uint8_t *buf, size_t len, esq_patch_t *out, int max)
{
    size_t i = 0;
    int n = 0, found = 0;
    while (i + 6 <= len) {
        size_t end, body, nprog, k;
        if (buf[i] != 0xF0 || buf[i + 1] != 0x0F || buf[i + 2] != 0x02 || (buf[i + 3] & 0xF0) != 0) {
            i++;
            continue;
        }
        for (end = i + 1; end < len && buf[end] != 0xF7; end++)
            ;
        body = end - (i + 5);                            /* nibbles between the code and F7 */
        nprog = buf[i + 4] == 0x01 ? 1 : buf[i + 4] == 0x02 ? 40 : 0;
        if (nprog && body >= nprog * 2 * ESQ_PCB_LEN) {
            found = 1;
            for (k = 0; k < nprog && n < max; k++) {
                uint8_t pcb[ESQ_PCB_LEN];
                esqp_denib(buf + i + 5 + k * 2 * ESQ_PCB_LEN, pcb, ESQ_PCB_LEN);
                esq_patch_from_pcb(&out[n++], pcb);
            }
        }
        i = end + 1;
    }
    return found ? n : -1;
}

void esq_sysex_single(const esq_patch_t *p, int ch, uint8_t *out)
{
    uint8_t pcb[ESQ_PCB_LEN];
    int i;
    esq_patch_to_pcb(p, pcb);
    out[0] = 0xF0;
    out[1] = 0x0F;
    out[2] = 0x02;
    out[3] = (uint8_t)(ch & 15);
    out[4] = 0x01;
    for (i = 0; i < ESQ_PCB_LEN; i++) {
        out[5 + 2 * i] = pcb[i] & 15;
        out[6 + 2 * i] = pcb[i] >> 4;
    }
    out[5 + 2 * ESQ_PCB_LEN] = 0xF7;
}
