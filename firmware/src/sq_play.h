/* SPDX-License-Identifier: GPL-3.0-only */
/* SQ1D's playing layer (R4). Every note from the keys and from MIDI in passes through here on its way to the engine:
 *   transpose   (SEQ KEYS TRANSP, while the sequencer plays) a key sets the sequence's transposition, nothing sounds
 *   chord       chord memory: each note plays the stored chord on it (ARP held + a chord stores one)
 *   record      the sequencer's step entry (stopped) or real-time recording (playing), overdubbed
 *   arpeggiator the notes held (or latched) in turn: UP, DOWN, UP/DN, RANDOM, ORDER over 1-4 octaves
 *   latch       (arpeggiator off) the last chord sounds on after the keys let go, until the next one
 * and the scheduler, which times the arpeggiator's and the sequencer's notes against the audio's own sample clock
 * (sq_clock) at 24 ticks a quarter note, from the tempo or from MIDI clock: a few ms ahead of the audio, in time
 * order, into the timed ring (sq_engine.h), so a note lands within 32 samples of its time whatever the main loop is
 * doing. MIDI out (SEND) and the clock out go the same way, sent when their time comes. */
#define PL_LOOK 1024u                           /* the scheduler works this far ahead of the audio (23 ms) */
static const uint8_t RATE_TICKS[NRATE] = {24, 12, 8, 6, 4, 3};
enum { SRC_ARP = 1, SRC_SEQ = 2 };
enum { PD_OFF, PD_ON, PD_CLOCK };

/* ---- the notes timed but not yet handed to the ISR, in time order (an off before an on at the same time) */
typedef struct { uint32_t at; uint8_t kind, note, vel, src; } pend_t;
#define NPEND 128u
static pend_t pend[NPEND] __attribute__((section(".pool")));
static uint32_t npend, sch_floor;               /* sch_floor: what went to the ISR ends there; nothing goes earlier */
static uint8_t snd[128];                        /* notes the direct path holds on (latch, release all) */

/* MIDI out, sent when its time comes (SEND, clock out) */
typedef struct { uint32_t at; uint8_t st, d1, d2, pad; } mo_t;
#define NMO 128u
static mo_t mo[NMO] __attribute__((section(".pool")));
static uint32_t mo_w, mo_r;
static void mo_put(uint32_t at, uint32_t st, uint32_t d1, uint32_t d2)
{
    mo_t *m;
    if (mo_w - mo_r >= NMO) return;
    m = &mo[mo_w++ % NMO];
    m->at = at; m->st = (uint8_t)st; m->d1 = (uint8_t)d1; m->d2 = (uint8_t)d2;
}
static void mo_poll(void)
{
    while (mo_r != mo_w && (int32_t)(mo[mo_r % NMO].at - sq_clock) <= 0) {
        const mo_t *m = &mo[mo_r++ % NMO];
        midi_send(m->st, m->d1, m->d2);
    }
}

static void pend_add(uint32_t at, uint32_t kind, uint32_t note, uint32_t vel, uint32_t src)
{
    uint32_t i;
    if ((int32_t)(at - sch_floor) < 0) at = sch_floor;
    if (npend >= NPEND) {                       /* (full: an off goes now, an on is dropped) */
        if (kind == PD_OFF) sq_note(0, note, 0);
        return;
    }
    for (i = npend; i > 0u; i--) {              /* from the end: most go there */
        const pend_t *q = &pend[i - 1u];
        int32_t d = (int32_t)(q->at - at);
        if (d < 0 || (d == 0 && (q->kind == PD_OFF || kind != PD_OFF))) break;
        pend[i] = pend[i - 1u];
    }
    pend[i].at = at; pend[i].kind = (uint8_t)kind; pend[i].note = (uint8_t)note; pend[i].vel = (uint8_t)vel;
    pend[i].src = (uint8_t)src;
    npend++;
}
static void pend_flush(uint32_t limit)          /* everything due before limit: to the ISR (and MIDI out) */
{
    uint32_t k = 0, ch = sys_ochan();
    while (k < npend && (int32_t)(pend[k].at - limit) < 0) {
        const pend_t *q = &pend[k++];
        if (q->kind == PD_CLOCK) { mo_put(q->at, 0xF8u, 0, 0); continue; }
        if (sq_tevent(q->at, q->kind == PD_ON ? EV_ON : EV_OFF, q->note, q->kind == PD_ON ? q->vel : 0u)) {
            if (q->kind == PD_OFF) sq_note(0, q->note, 0);   /* (the ring full: an off now rather than never) */
            continue;
        }
        if (sys.send) mo_put(q->at, (q->kind == PD_ON ? 0x90u : 0x80u) | ch, q->note, q->kind == PD_ON ? q->vel : 0u);
    }
    if (k) { uint32_t i; for (i = k; i < npend; i++) pend[i - k] = pend[i]; npend -= k; }
    if ((int32_t)(limit - sch_floor) > 0) sch_floor = limit;
}
/* stop what src (0: any) has timed: the timed ring's notes not yet due go, and every off pending is sent now */
static void pend_cut(uint32_t src)
{
    uint32_t i, k = 0;
    sq_tdrop_all();                             /* (both sources' not-yet-due notes go; the arpeggiator just goes on) */
    for (i = 0; i < npend; i++) {
        const pend_t *q = &pend[i];
        if (q->kind == PD_OFF) { sq_note(0, q->note, 0); if (sys.send) midi_send(0x80u | sys_ochan(), q->note, 0); }
        else if (q->kind == PD_ON && src && q->src != src) pend[k++] = *q;   /* (the other source's ons stay) */
        else if (q->kind == PD_CLOCK) pend[k++] = *q;
    }
    npend = k;
}

/* ---- the direct path: a note now */
static void direct(uint32_t on, uint32_t n, uint32_t vel)
{
    if (on) {
        if (snd[n] < 255u) snd[n]++;
        sq_note(1, n, vel);
    } else {
        if (!snd[n]) return;
        snd[n]--;
        sq_note(0, n, 0);
    }
    if (sys.send) midi_send((on ? 0x90u : 0x80u) | sys_ochan(), n, on ? vel : 0u);
}
static void direct_all_off(void)
{
    uint32_t n;
    for (n = 0; n < 128u; n++)
        while (snd[n]) direct(0, n, 0);
}

/* ---- the clock: internal (tempo) or MIDI clock in; 24 ticks a quarter note */
static uint32_t clk_next, clk_frac, ext_last, ext_period = 918u, ext_seen;
static uint32_t pl_spt_q8(void) { return 28224000u / (sys.tempo ? sys.tempo : 120u); }   /* samples a tick, Q8 */
static uint32_t pl_tick_samples(void) { return sys.clock ? ext_period : pl_spt_q8() >> 8; }
static void clk_restart(uint32_t at)           /* the next internal tick at 'at' (no earlier than sch_floor) */
{
    clk_next = (int32_t)(at - sch_floor) < 0 ? sch_floor : at;
    clk_frac = 0;
}

/* ---- the sequencer's state (sq_seq: the patterns, below) */
#define SEQ_NPAT 8u
#define SEQ_MAXLEN 64u
#define PAT_MAGIC 0x31544150u                   /* "PAT1" */
typedef struct { uint8_t note[4], vel, gate, pad[2]; } sq_step_t;   /* gate: quarters of a step, 1..16 (over 4: a tie) */
typedef struct { uint32_t magic; uint8_t len, rate, pad[2]; sq_step_t s[SEQ_MAXLEN]; } sq_pat_t;
static sq_pat_t sq_pat[SEQ_NPAT] __attribute__((section(".pool")));
static uint8_t seq_play, seq_rec, seq_pos, seq_cur, seq_shown_pos;
static int8_t seq_transp;
static uint32_t seq_t, seq_gen;                 /* ticks since the start; a count of changes (the screen redraws) */
static struct { uint32_t at; uint8_t pos; } seq_hist[4];   /* the last steps' times (real-time recording) */
static uint32_t seq_nhist;
static uint8_t seq_skip_pos = 0xFFu, seq_skip_note;   /* a note just recorded ahead of its step: not played again */
static sq_pat_t *seq_p(void) { return &sq_pat[(sys.patt - 1u) % SEQ_NPAT]; }
static uint32_t seq_steplen(void) { return RATE_TICKS[seq_p()->rate % NRATE] * pl_tick_samples(); }

static void seq_tick(uint32_t t)
{
    sq_pat_t *p = seq_p();
    uint32_t rate = RATE_TICKS[p->rate % NRATE], len, sl, on_at, i;
    const sq_step_t *s;
    if (seq_t++ % rate) return;
    len = p->len ? p->len : 16u;
    if (seq_pos >= len) seq_pos = 0;
    s = &p->s[seq_pos];
    sl = rate * pl_tick_samples();
    on_at = t + (seq_pos & 1u ? sl * (uint32_t)(sys.swing - 50u) / 50u : 0u);
    seq_hist[seq_nhist++ % 4u].at = t, seq_hist[(seq_nhist - 1u) % 4u].pos = seq_pos;
    for (i = 0; i < 4u; i++) {
        int32_t n = s->note[i];
        if (!n) continue;
        if (seq_pos == seq_skip_pos && (uint32_t)n == seq_skip_note) { seq_skip_pos = 0xFFu; continue; }
        n += seq_transp;
        if (n < 0 || n > 127) continue;
        pend_add(on_at, PD_ON, (uint32_t)n, s->vel ? s->vel : 100u, SRC_SEQ);
        pend_add(on_at + sl * (s->gate ? s->gate : 3u) / 4u - 1u, PD_OFF, (uint32_t)n, 0, SRC_SEQ);
    }
    seq_pos = (uint8_t)((seq_pos + 1u) % len);
}

/* ---- the arpeggiator: the notes held (or latched) in the order played */
static uint8_t arp_n[16], arp_v[16], arp_cnt;
static uint32_t arp_t, arp_step, arp_rng = 0x2545F491u;
static int arp_note(uint32_t step, uint32_t *vel)   /* the note for step 'step' */
{
    uint8_t so[16];
    uint32_t i, j, n = arp_cnt, tot = n * (sys.aoct ? sys.aoct : 1u), k, per;
    if (!n) return -1;
    for (i = 0; i < n; i++) so[i] = arp_n[i];
    if (sys.amode != AM_ORDER)                  /* (sorted low to high) */
        for (i = 1; i < n; i++)
            for (j = i; j > 0u && so[j - 1u] > so[j]; j--) { uint8_t t = so[j]; so[j] = so[j - 1u]; so[j - 1u] = t; }
    switch (sys.amode) {
    case AM_DOWN: k = tot - 1u - step % tot; break;
    case AM_UPDN: per = tot > 1u ? 2u * tot - 2u : 1u; k = step % per; if (k >= tot) k = per - k; break;
    case AM_RAND: arp_rng ^= arp_rng << 13; arp_rng ^= arp_rng >> 17; arp_rng ^= arp_rng << 5; k = arp_rng % tot; break;
    default: k = step % tot; break;
    }
    for (i = 0; i < n && arp_n[i] != so[k % n]; i++) {}
    *vel = i < n ? arp_v[i] : 100u;
    k = so[k % n] + 12u * (k / n);
    return k > 127u ? (int)so[0] : (int)k;
}
static uint32_t arp_dur(void)
{
    uint32_t d = RATE_TICKS[sys.arate % NRATE] * pl_tick_samples() * sys.agate / 100u;
    return d < 64u ? 64u : d;
}
static void arp_tick(uint32_t t, uint32_t st)   /* st: the sequencer's tick count before this tick */
{
    uint32_t rate = RATE_TICKS[sys.arate % NRATE], tk = seq_play ? st : arp_t++, vel, on_at;
    int n;
    if (!sys.arp || !arp_cnt || tk % rate) return;
    n = arp_note(arp_step++, &vel);
    if (n < 0) return;
    on_at = t + ((tk / rate) & 1u ? rate * pl_tick_samples() * (uint32_t)(sys.swing - 50u) / 50u : 0u);
    pend_add(on_at, PD_ON, (uint32_t)n, vel, SRC_ARP);
    pend_add(on_at + arp_dur(), PD_OFF, (uint32_t)n, 0, SRC_ARP);
}
static void arp_clear(void) { arp_cnt = 0; }
static void arp_add(uint32_t n, uint32_t vel)
{
    uint32_t i, was = arp_cnt;
    int v;
    for (i = 0; i < arp_cnt; i++) if (arp_n[i] == n) return;
    if (arp_cnt >= 16u) return;
    arp_n[arp_cnt] = (uint8_t)n; arp_v[arp_cnt] = (uint8_t)vel; arp_cnt++;
    if (was || seq_play) return;
    arp_step = 0;                               /* a new run: its first note now, the clock restarted under it */
    if (sys.clock) { arp_t = 0; return; }       /* (MIDI clock: from the next tick) */
    v = arp_note(arp_step++, &vel);
    if (v >= 0) {
        uint32_t now = sq_clock;
        sq_note(1, (uint32_t)v, vel);
        if (sys.send) midi_send(0x90u | sys_ochan(), (uint32_t)v, vel);
        pend_add(now + arp_dur(), PD_OFF, (uint32_t)v, 0, SRC_ARP);
    }
    arp_t = 1;
    clk_restart(sq_clock + pl_tick_samples());
}
static void arp_remove(uint32_t n)
{
    uint32_t i;
    for (i = 0; i < arp_cnt && arp_n[i] != n; i++) {}
    if (i == arp_cnt) return;
    for (; i + 1u < arp_cnt; i++) { arp_n[i] = arp_n[i + 1u]; arp_v[i] = arp_v[i + 1u]; }
    arp_cnt--;
}

/* ---- the scheduler: a tick */
static void pl_tick(uint32_t t)
{
    uint32_t st = seq_t;
    if ((int32_t)(t - sch_floor) < 0) t = sch_floor;   /* (MIDI clock arrives "now": its notes a few ms on, whole) */
    if (sys.clkout && !sys.clock) pend_add(t, PD_CLOCK, 0, 0, 0);
    if (seq_play) seq_tick(t);
    arp_tick(t, st);
}
static void pl_ext_clock(void)                  /* MIDI clock in (CLOCK EXT): a tick now */
{
    uint32_t now = sq_clock, d = now - ext_last;
    if (ext_seen && d < 44100u) ext_period += (uint32_t)((int32_t)(d - ext_period) / 8);
    ext_last = now;
    ext_seen = 1;
    pl_tick(now);
}

/* ---- the sequencer: transport, recording */
static void seq_stop(void)
{
    if (!seq_play) return;
    seq_play = 0;
    pend_cut(SRC_SEQ);
    if (sys.clkout && !sys.clock) midi_send(0xFCu, 0, 0);
    seq_gen++;
}
static void seq_start(uint32_t from0)
{
    if (seq_play) return;
    if (from0) { seq_pos = 0; seq_t = 0; }
    seq_nhist = 0;
    seq_play = 1;
    if (!sys.clock) {
        clk_restart(sq_clock);
        if (sys.clkout) midi_send(from0 ? 0xFAu : 0xFBu, 0, 0);
    }
    seq_gen++;
}
static uint8_t ent_note[4], ent_n, ent_vel, ent_ties;   /* step entry: the chord being entered */
static struct { uint8_t note, step; uint32_t at; } live[8];   /* real-time recording: notes held, their steps */
static void step_put(sq_step_t *s, uint32_t n, uint32_t vel)   /* (overdub: a free place, else the last) */
{
    uint32_t i;
    for (i = 0; i < 4u; i++) if (s->note[i] == n) return;
    for (i = 0; i < 4u && s->note[i]; i++) {}
    s->note[i < 4u ? i : 3u] = (uint8_t)n;
    s->vel = (uint8_t)vel;
    if (!s->gate) s->gate = 3;
}
static void seq_rec_in(uint32_t on, uint32_t n, uint32_t vel)
{
    sq_pat_t *p = seq_p();
    uint32_t len = p->len ? p->len : 16u, i;
    if (!seq_play) {                            /* step entry: the chord gathers until every key is up */
        if (on && ent_n < 4u) {
            for (i = 0; i < ent_n && ent_note[i] != n; i++) {}
            if (i == ent_n) ent_note[ent_n++] = (uint8_t)n;
            ent_vel = (uint8_t)vel;
        }
        return;
    }
    if (on) {                                   /* real time: the nearest step */
        uint32_t now = sq_clock, sl = seq_steplen(), k, h = 0xFFFFFFFFu, s;
        for (k = 0; k < 4u && k < seq_nhist; k++) {   /* the latest step already sounding */
            uint32_t j = (seq_nhist - 1u - k) % 4u;
            if ((int32_t)(seq_hist[j].at - now) <= 0) { h = j; break; }
        }
        if (h == 0xFFFFFFFFu) return;
        k = h;
        s = seq_hist[k].pos;
        if (now - seq_hist[k].at > sl / 2u) { s = (s + 1u) % len; seq_skip_pos = (uint8_t)s; seq_skip_note = (uint8_t)n; }
        step_put(&p->s[s], n, vel);
        p->s[s].gate = 1;
        for (i = 0; i < 8u && live[i].note; i++) {}
        if (i < 8u) { live[i].note = (uint8_t)n; live[i].step = (uint8_t)s; live[i].at = now; }
        seq_gen++;
    } else {
        for (i = 0; i < 8u; i++)
            if (live[i].note == n) {
                uint32_t sl = seq_steplen(), g = sl ? ((sq_clock - live[i].at) * 4u + sl / 2u) / sl : 1u;
                p->s[live[i].step].gate = (uint8_t)(g < 1u ? 1u : g > 16u ? 16u : g);
                live[i].note = 0;
            }
    }
}
static void seq_entry_commit(void)             /* every key up: the chord into the step at the cursor, on to the next */
{
    sq_pat_t *p = seq_p();
    uint32_t len = p->len ? p->len : 16u, i;
    sq_step_t *s = &p->s[seq_cur % len];
    if (!ent_n) return;
    for (i = 0; i < 4u; i++) s->note[i] = i < ent_n ? ent_note[i] : 0u;
    s->vel = ent_vel ? ent_vel : 100u;
    s->gate = (uint8_t)(3u + 4u * ent_ties > 16u ? 16u : 3u + 4u * ent_ties);
    seq_cur = (uint8_t)((seq_cur + 1u + ent_ties) % len);
    ent_n = 0; ent_ties = 0;
    seq_gen++;
}
static void seq_rest(void)                      /* step entry: SEL with no key down, an empty step */
{
    sq_pat_t *p = seq_p();
    uint32_t len = p->len ? p->len : 16u;
    memset(&p->s[seq_cur % len], 0, sizeof p->s[0]);
    seq_cur = (uint8_t)((seq_cur + 1u) % len);
    seq_gen++;
}
static void pat_init(sq_pat_t *p) { memset(p, 0, sizeof *p); p->magic = PAT_MAGIC; p->len = 16; p->rate = RT_16; }
static void seq_boot(void)
{
    uint32_t i;
    for (i = 0; i < SEQ_NPAT; i++)
        if (blob_load(i, &sq_pat[i], sizeof sq_pat[i]) || sq_pat[i].magic != PAT_MAGIC ||
            sq_pat[i].len < 1u || sq_pat[i].len > SEQ_MAXLEN || sq_pat[i].rate >= NRATE)
            pat_init(&sq_pat[i]);
}
static int seq_store(void) { return blob_save((sys.patt - 1u) % SEQ_NPAT, seq_p(), sizeof(sq_pat_t)); }

/* ---- the way in: a note from the keys or MIDI */
static uint32_t pl_held[4], pl_down;           /* the input notes held (before chord memory) */
static void pl_stage(uint32_t on, uint32_t n, uint32_t vel, uint32_t fresh)
{
    if (seq_rec) seq_rec_in(on, n, vel);
    if (sys.arp) {
        if (on) { if (sys.latch && fresh) arp_clear(); arp_add(n, vel); }
        else if (!sys.latch) arp_remove(n);
    } else if (sys.latch) {
        if (on) { if (fresh) direct_all_off(); if (!snd[n]) direct(1, n, vel); }
    } else
        direct(on, n, vel);
}
static void pl_input(uint32_t on, uint32_t n, uint32_t vel)
{
    uint32_t bit = 1u << (n & 31u), fresh = 0, i;
    n &= 127u;
    if (on) {
        if (pl_held[n >> 5] & bit) return;
        pl_held[n >> 5] |= bit;
        fresh = pl_down == 0;
        pl_down++;
    } else {
        if (!(pl_held[n >> 5] & bit)) return;
        pl_held[n >> 5] &= ~bit;
        pl_down--;
    }
    if (sys.keys && seq_play && !seq_rec) {     /* KEYS TRANSP: the sequence follows the key (C5 plays as written) */
        if (on) { seq_transp = (int8_t)((int32_t)n - 60); seq_gen++; }
        return;
    }
    if (sys.chord && sys.nchord) {
        for (i = 0; i < sys.nchord; i++) {
            int32_t x = (int32_t)n + sys.chordi[i];
            if (x >= 0 && x <= 127) pl_stage(on, (uint32_t)x, vel, fresh && i == 0u);
        }
    } else
        pl_stage(on, n, vel, fresh);
    if (!on && !pl_down && seq_rec && !seq_play) seq_entry_commit();
}
/* the playing layer's settings changed (ARP, LATCH, CHORD): everything it holds lets go */
static void pl_reset(void)
{
    direct_all_off();
    arp_clear();
    pend_cut(SRC_ARP);
}
/* chord memory: the notes held while ARP was held (one note: chord memory off) */
static void pl_chord_set(const uint8_t *n, uint32_t cnt)
{
    uint32_t i, j, lo = 127;
    pl_reset();
    if (cnt < 2u) { sys.chord = 0; return; }
    for (i = 0; i < cnt; i++) if (n[i] < lo) lo = n[i];
    sys.nchord = (uint8_t)(cnt > 8u ? 8u : cnt);
    for (i = 0; i < sys.nchord; i++) sys.chordi[i] = (int8_t)(n[i] - lo);
    for (i = 1; i < sys.nchord; i++)            /* (low to high) */
        for (j = i; j > 0u && sys.chordi[j - 1u] > sys.chordi[j]; j--) { int8_t t = sys.chordi[j]; sys.chordi[j] = sys.chordi[j - 1u]; sys.chordi[j - 1u] = t; }
    sys.chord = 1;
}

/* ---- main loop: the scheduler's turn */
static void pl_poll(void)
{
    uint32_t now = sq_clock, horizon = now + PL_LOOK;
    if ((int32_t)(sch_floor - now) < 0) sch_floor = now;   /* (after a stall: from now) */
    if (!sys.clock) {
        uint32_t spt = pl_spt_q8();
        if ((int32_t)(clk_next - now) < 0) clk_restart(now);
        while ((int32_t)(clk_next - horizon) < 0) {
            pl_tick(clk_next);
            clk_frac += spt;
            clk_next += clk_frac >> 8;
            clk_frac &= 255u;
        }
    }
    pend_flush(horizon);
    mo_poll();
    if (seq_play) {                             /* the step the audio is on, for the screen */
        uint32_t k;
        for (k = 0; k < 4u && k < seq_nhist; k++) {
            uint32_t h = (seq_nhist - 1u - k) % 4u;
            if ((int32_t)(seq_hist[h].at - now) <= 0) {
                if (seq_hist[h].pos != seq_shown_pos) { seq_shown_pos = seq_hist[h].pos; seq_gen++; }
                break;
            }
        }
    }
}
static uint32_t pl_state(void)
{
    return (uint32_t)sys.arp | (uint32_t)sys.latch << 1 | (uint32_t)(sys.chord && sys.nchord) << 2 |
           (uint32_t)seq_play << 3 | (uint32_t)seq_rec << 4;
}
