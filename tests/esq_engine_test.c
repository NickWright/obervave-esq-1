/* Engine tests. Includes the engine sources directly to reach voice internals. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../firmware/src/esq/esq_engine.c"
#include "../firmware/src/esq/esq_patch.c"
#include "../firmware/src/esq/esq_patches.c"
#include "../firmware/src/esq/esq_waves.c"

#define RATE 44100.0f
#define TICK ((uint32_t)(RATE * ESQ_TICK_S))

static int fails, checks;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        checks++;                                              \
        if (!(cond)) {                                         \
            fails++;                                           \
            printf("  FAIL %s:%d: ", __func__, __LINE__);      \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

static esq_wavebank_t bank;

#if ESQ_FIXED                                              /* (the integer build: the same tests through its API) */
static int esq_voice_render(esq_synth_t *s, esq_voice_t *v, float pitch, float *out, float gain, uint32_t n)
{
    static int32_t q[1 << 16];
    uint32_t i;
    int alive;
    for (i = 0; i < n; i++)
        q[i] = 0;
    alive = esq_voice_render_q(s, v, (int32_t)lrintf(pitch * 256.0f), q, (int32_t)(gain * 65536.0f), n);
    for (i = 0; i < n; i++)
        out[i] += (float)q[i] / 65536.0f;
    return alive;
}
#endif
static esq_synth_t syn;

/* a single-oscillator patch: wave w on OSC1, filter open, organ envelope */
static esq_patch_t one_osc(int w)
{
    esq_patch_t p = ESQ_DEMO_PATCHES[0];
    p.osc[0].wave = (uint8_t)w;
    p.flt.freq = 110;
    p.flt.q = 0;
    p.env[3] = (esq_env_t){.l1 = 63, .l2 = 63, .l3 = 63, .t4 = 10};
    return p;
}

static void setup(const esq_patch_t *p)
{
    esq_init(&syn, RATE, &bank);
    esq_set_patch(&syn, p);
}

static void run(uint32_t n)
{
    static float l[ESQ_CHUNK], r[ESQ_CHUNK];
    while (n) {
        uint32_t m = n < ESQ_CHUNK ? n : ESQ_CHUNK;
        esq_render(&syn, l, r, m);
        n -= m;
    }
}

/* frequency from rising zero crossings (interpolated) */
static double measure_hz(const float *x, uint32_t n)
{
    double first = -1, last = -1;
    int count = 0;
    uint32_t i;
    for (i = 1; i < n; i++)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            double t = (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0)
                first = t;
            last = t;
            count++;
        }
    return count > 1 ? (count - 1) / ((last - first) / RATE) : 0.0;
}

static void test_pitch(void)
{
    static float l[88200], r[88200];
    static const int notes[] = {45, 57, 69, 81, 93};
    esq_patch_t p = one_osc(2);                            /* SINE */
    int i;
    for (i = 0; i < 5; i++) {
        double want = 440.0 * pow(2.0, (notes[i] - 69) / 12.0), got;
        setup(&p);
        esq_note_on(&syn, notes[i], 100);
        esq_render(&syn, l, r, 4410);                      /* settle */
        esq_render(&syn, l, r, 88200);
        got = measure_hz(l, 88200);
        CHECK(fabs(got / want - 1.0) < 0.002, "note %d: %.3f Hz, want %.3f", notes[i], got, want);
    }
    p.osc[0].fine = 16;                                    /* FINE 16 = half a semitone */
    setup(&p);
    esq_note_on(&syn, 69, 100);
    esq_render(&syn, l, r, 4410);
    esq_render(&syn, l, r, 88200);
    CHECK(fabs(measure_hz(l, 88200) / (440.0 * pow(2.0, 0.5 / 12.0)) - 1.0) < 0.002, "FINE 16: %.3f Hz",
          measure_hz(l, 88200));
}

static void test_doc(void)
{
    esq_patch_t p = one_osc(0);                            /* SAW: the index moves every DOC frame at note 84 */
    uint32_t i, changes = 0, n = 44100;
    int32_t prev;
    int multiple = 1;
    CHECK(fabsf(ESQ_DOC_RATE_DEFAULT - 38461.5f) < 1.0f, "DOC rate %.1f", ESQ_DOC_RATE_DEFAULT);
    setup(&p);
    esq_note_on(&syn, 84, 100);
    run(TICK * 8);                                         /* the volume register settles (averaged per update) */
    prev = syn.v[0].doc_out;
    for (i = 0; i < n; i++) {
        run(1);
        if (syn.v[0].doc_out != prev)
            changes++;
        if (syn.v[0].doc_out % (int32_t)syn.v[0].vol[0] != 0)
            multiple = 0;
        prev = syn.v[0].doc_out;
    }
    CHECK(fabs(changes / (double)ESQ_DOC_RATE_DEFAULT - 1.0) < 0.02, "%u DOC value changes in 1 s, DOC rate %.0f",
          changes, 44100.0 * 16777216.0 / syn.doc_period);
    CHECK(multiple, "DOC output not (8-bit sample - 128) x volume");
    CHECK(syn.v[0].vol[0] >= 254, "LEVEL 63: volume register %u", syn.v[0].vol[0]);
    CHECK(esq_doc_volume(0) == 0 && esq_doc_volume(126) == 255 && esq_doc_volume(64) > 25 && esq_doc_volume(64) < 40,
          "DCA curve: %d %d %d", esq_doc_volume(0), esq_doc_volume(64), esq_doc_volume(126));
}

static void test_env(void)
{
    esq_patch_t p = one_osc(2);
    esq_voice_t *v = &syn.v[0];
    uint32_t i, attack_end = 0, gone = 0, half = 0;
    p.env[3].t1 = 30;                                      /* 0.45 s */
    p.env[3].t4 = 34;                                      /* 0.72 s */
    CHECK(fabsf(esq_env_time_s(0) - 0.01f) < 1e-6f && fabsf(esq_env_time_s(63) - 20.48f) < 1e-4f &&
              fabsf(esq_env_time_s(39) - 1.28f) < 1e-5f, "time table");
    setup(&p);
    esq_note_on(&syn, 60, 127);
    for (i = 0; i < 44100 && !attack_end; i++) {
        run(1);
        if (v->env[3].stage != 1)
            attack_end = i;
        if (!half && (v->env[3].level >> 8) >= 63)
            half = i;
    }
    CHECK(fabs(attack_end / RATE - 0.45) < 0.015, "attack %.4f s, want 0.45", attack_end / RATE);
    CHECK(fabs(half / RATE - 0.225) < 0.02, "attack half way at %.4f s: not linear", half / RATE);
    run(22050);
    esq_note_off(&syn, 60);
    for (i = 0; i < 4 * 44100 && !gone; i++) {
        run(1);
        if (!v->active)
            gone = i;
    }
    CHECK(gone && fabs(gone / RATE - 0.72) < 0.03, "release %.4f s, want 0.72", gone / RATE);

    /* LV, T1V, TK */
    p = one_osc(2);
    p.env[0] = (esq_env_t){.l1 = 63, .l2 = 40, .l3 = 20, .t1 = 30, .t2 = 30, .t3 = 30, .t4 = 30, .lv = 63, .t1v = 63, .tk = 63};
    setup(&p);
    v->vel = 127;
    v->note = 60;
    esq_env_start(&v->env[0], &p.env[0], v, 1);
    CHECK(v->env[0].target >= 124, "LV at velocity 127: L1 %d", v->env[0].target);
    CHECK(v->env[0].count <= 2, "T1V at velocity 127: %u ticks", v->env[0].count);
    v->vel = 10;
    esq_env_start(&v->env[0], &p.env[0], v, 1);
    CHECK(v->env[0].target < 30, "LV at velocity 10: L1 %d", v->env[0].target);
    CHECK(v->env[0].count > 20, "T1V at velocity 10: %u ticks", v->env[0].count);
    v->note = 96;
    CHECK(esq_env_tk(&p.env[0], v) == 29, "TK 63 at note 96: T2, T3 shortened by %d", esq_env_tk(&p.env[0], v));
    v->note = 36;
    CHECK(esq_env_tk(&p.env[0], v) == 0, "TK at note 36: %d", esq_env_tk(&p.env[0], v));

    /* CYC: no sustain, the key ignored */
    p = one_osc(2);
    p.env[3] = (esq_env_t){.l1 = 63, .l2 = 50, .l3 = 40, .t1 = 5, .t2 = 5, .t3 = 5, .t4 = 10};
    p.modes.cycle = 1;
    setup(&p);
    esq_note_on(&syn, 60, 100);
    run(TICK * 20);
    CHECK(v->env[3].stage == 5 || v->env[3].stage == 0, "CYC: stage %d with the key held", v->env[3].stage);

    /* ENV restart off: a restruck key's voice (VC) starts its envelopes from their level */
    p = one_osc(2);
    p.env[3] = (esq_env_t){.l1 = 63, .l2 = 63, .l3 = 63, .t1 = 40, .t4 = 40};
    p.modes.vc = 1;
    p.modes.env_reset = 0;
    setup(&p);
    esq_note_on(&syn, 60, 127);
    run(TICK * 40);
    i = (uint32_t)(v->env[3].level >> 8);
    esq_note_off(&syn, 60);
    esq_note_on(&syn, 60, 127);
    CHECK(esq_active_voices(&syn) == 1 && (v->env[3].level >> 8) >= (int)i - 2, "ENV off: restart from %d, was %u",
          v->env[3].level >> 8, i);
    p.modes.env_reset = 1;
    setup(&p);
    esq_note_on(&syn, 60, 127);
    run(TICK * 40);
    esq_note_on(&syn, 60, 127);
    CHECK((v->env[3].level >> 8) < 10, "ENV on: restart from %d", v->env[3].level >> 8);
}

static void test_lfo(void)
{
    esq_lfost_t l;
    esq_lfo_t p = {.freq = 7, .wave = ESQ_LFO_SQR, .l1 = 63, .l2 = 63};
    int i, neg = 0, wraps = 0, prev;
    CHECK(fabsf(esq_lfo_hz(63) - 57.0f / 2.56f) < 0.01f && fabsf(esq_lfo_hz(7) - 1.0f / 2.56f) < 0.001f,
          "LFO rates %.3f %.3f", esq_lfo_hz(63), esq_lfo_hz(7));
    memset(&l, 0, sizeof l);
    esq_lfo_start(&l, &p);
    prev = l.phase;
    for (i = 0; i < 512; i++) {
        esq_lfo_tick(&l, &p, 0);
        neg |= l.out < 0;
        wraps += l.phase < prev;
        prev = l.phase;
    }
    CHECK(!neg, "square LFO went negative (it is unipolar)");
    CHECK(wraps == 2, "FREQ 7: %d cycles in 512 ticks, want 2", wraps);
    p = (esq_lfo_t){.freq = 3, .wave = ESQ_LFO_TRI, .l1 = 63, .l2 = 63};
    memset(&l, 0, sizeof l);
    esq_lfo_start(&l, &p);
    for (i = 0; i < 7 * 256; i++)
        esq_lfo_tick(&l, &p, 0);
    CHECK(l.phase == 0, "FREQ 3: phase %d after 7 x 256 ticks (3/7 a tick)", l.phase);
    p = (esq_lfo_t){.freq = 20, .wave = ESQ_LFO_TRI, .l1 = 0, .l2 = 40, .delay = 4};
    memset(&l, 0, sizeof l);
    esq_lfo_start(&l, &p);
    for (i = 0; i < 10; i++)
        esq_lfo_tick(&l, &p, 0);
    CHECK(l.level == 40, "DELAY 4: depth %d after 10 ticks, want 40", l.level);
    p.delay = 0;
    esq_lfo_start(&l, &p);
    for (i = 0; i < 100; i++)
        esq_lfo_tick(&l, &p, 0);
    CHECK(l.level == 0 && l.out == 0, "DELAY 0 left L1");
    esq_lfo_tick(&l, &p, 100);
    CHECK(l.out != 0 || l.phase == 64 || l.phase == 192, "the MOD source did not add to the depth");
}

static void test_cv(void)
{
    esq_patch_t p = one_osc(2);
    esq_voice_t *v = &syn.v[0];
    int fc48, fc60;
    CHECK(fabsf(esq_fc_hz(150) / 6262.0f - 1.0f) < 0.01f, "FC DAC 150: %.0f Hz", esq_fc_hz(150));
    CHECK(esq_fc_dac(72, 0) == 150 && esq_fc_dac(0, 0) == 6 && esq_fc_dac(127, 0) == 255, "FC DAC from FREQ");
    CHECK(esq_vca_dac(0) == 9 && esq_vca_dac(126) == 255, "VCA curve ends %d %d", esq_vca_dac(0), esq_vca_dac(126));
    /* the VCA CV: a quarter of the way per 2.5 ms step, at the target after the 4th */
    setup(&p);
    esq_note_on(&syn, 60, 100);
    run(TICK * 4 - 4);
    CHECK((v->vca_acc >> 8) == esq_vca_dac(123), "VCA DAC %d after the attack ramp, want %d", v->vca_acc >> 8,
          esq_vca_dac(123));
    /* keyboard tracking 63: about 1:1 (2 FC DAC steps of 0.555 semitone per key) */
    p.flt.freq = 20;
    p.flt.keybd = 63;
    setup(&p);
    esq_note_on(&syn, 48, 100);
    run(TICK * 2);
    fc48 = v->fc_acc >> 8;
    setup(&p);
    esq_note_on(&syn, 60, 100);
    run(TICK * 2);
    fc60 = v->fc_acc >> 8;
    CHECK(abs((fc60 - fc48) - 24) <= 1, "KEYBD 63: %d DAC steps an octave, want ~24 (1:1)", fc60 - fc48);
    CHECK(v->q_dac == 0, "Q DAC");
}

static void test_voices(void)
{
    static float l[3 * 44100], r[3 * 44100];
    esq_patch_t p = one_osc(0);
    int i, silent = 1, n;
    setup(&p);
    for (i = 0; i < 9; i++)
        esq_note_on(&syn, 48 + i * 3, 100);
    esq_render(&syn, l, r, 512);
    CHECK(esq_active_voices(&syn) == ESQ_NVOICE, "%d voices sounding, want %d", esq_active_voices(&syn), ESQ_NVOICE);
    for (i = 0; i < 9; i++)
        esq_note_off(&syn, 48 + i * 3);
    esq_render(&syn, l, r, 3 * 44100);
    CHECK(esq_active_voices(&syn) == 0, "%d voices left after release", esq_active_voices(&syn));
    for (i = 3 * 44100 - 4410; i < 3 * 44100; i++)
        if (fabsf(l[i]) > 1e-5f || fabsf(r[i]) > 1e-5f)
            silent = 0;
    CHECK(silent, "output not silent after every voice ended");
    /* VC on: a restruck key takes its voice back; off: a new voice, two at most */
    p.env[3].t4 = 40;
    p.modes.vc = 1;
    setup(&p);
    for (i = 0; i < 3; i++) {
        esq_note_on(&syn, 60, 100);
        esq_note_off(&syn, 60);
    }
    CHECK(esq_active_voices(&syn) == 1, "VC on: %d voices on one key", esq_active_voices(&syn));
    p.modes.vc = 0;
    setup(&p);
    for (i = 0; i < 3; i++) {
        esq_note_on(&syn, 60, 100);
        esq_note_off(&syn, 60);
    }
    n = esq_active_voices(&syn);
    CHECK(n == 2, "VC off: %d voices on one key, want 2", n);
    /* sustain pedal */
    setup(&p);
    esq_cc(&syn, 64, 127);
    esq_note_on(&syn, 60, 100);
    esq_note_off(&syn, 60);
    CHECK(syn.v[0].gate, "sustain pedal: released");
    esq_cc(&syn, 64, 0);
    CHECK(!syn.v[0].gate, "sustain pedal up: still held");
}

static void test_health(void)
{
    static float l[2 * 44100], r[2 * 44100];
    static const int chord[] = {36, 43, 48, 55, 60, 64, 67, 71};
    int p, i;
    for (p = 0; p < ESQ_NDEMO_PATCHES; p++) {
        float peak = 0.0f, sum = 0.0f;
        int finite = 1;
        setup(&ESQ_DEMO_PATCHES[p]);
        for (i = 0; i < 8; i++)
            esq_note_on(&syn, chord[i], 127);
        esq_cc(&syn, 1, 127);
        esq_render(&syn, l, r, 2 * 44100);
        for (i = 0; i < 2 * 44100; i++) {
            if (!isfinite(l[i]) || !isfinite(r[i]))
                finite = 0;
            peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(r[i])));
            sum += l[i];
        }
        CHECK(finite, "%s: non-finite output", ESQ_DEMO_PATCHES[p].name);
        CHECK(peak <= 1.0f, "%s: peak %.3f", ESQ_DEMO_PATCHES[p].name, peak);
        CHECK(peak > 0.01f, "%s: silent (peak %.4f)", ESQ_DEMO_PATCHES[p].name, peak);
        CHECK(fabsf(sum / (2 * 44100)) < 0.02f, "%s: DC %.4f", ESQ_DEMO_PATCHES[p].name, sum / (2 * 44100));
    }
}

static void test_sync(void)
{
    esq_patch_t p = one_osc(0);
    esq_voice_t *v;
    int i, resets = 0, wraps = 0, bad = 0;
    p.modes.sync = 1;
    p.osc[1] = (esq_osc_t){.semi = 19, .wave = 0, .fm = {{ESQ_SRC_OFF, 0}, {ESQ_SRC_OFF, 0}}};
    p.dca[1] = (esq_dca_t){.on = 1, .level = 63, .am = {{ESQ_SRC_OFF, 0}, {ESQ_SRC_OFF, 0}}};
    setup(&p);
    esq_note_on(&syn, 48, 100);
    v = &syn.v[0];
    for (i = 0; i < 20000; i++) {
        uint32_t before = v->ph[0];
        esq_doc_frame(v, 1, 0);
        if (v->ph[0] < before) {
            wraps++;
            if (v->ph[1] == 0)
                resets++;
            else
                bad++;
        }
    }
    CHECK(wraps > 10 && bad == 0 && resets == wraps, "OSC1 wrapped %d times, OSC2 reset %d", wraps, resets);
}

static void test_am(void)
{
    esq_patch_t p = one_osc(2);
    esq_voice_t *v;
    int i, mismatch = 0;
    p.modes.am = 1;
    p.osc[1] = (esq_osc_t){.wave = 0, .fm = {{ESQ_SRC_OFF, 0}, {ESQ_SRC_OFF, 0}}};
    p.dca[1] = (esq_dca_t){.on = 1, .level = 20, .am = {{ESQ_SRC_OFF, 0}, {ESQ_SRC_OFF, 0}}};
    setup(&p);
    esq_note_on(&syn, 60, 100);
    run(TICK * 4);
    v = &syn.v[0];
    for (i = 0; i < 5000; i++) {
        int32_t s0, s1, want;
        esq_doc_frame(v, 0, 1);
        s0 = v->tab[0][v->ph[0] >> v->shift[0]];
        s1 = v->tab[1][v->ph[1] >> v->shift[1]];
        want = (s0 - 128) * (int32_t)v->vol[0] + (s1 - 128) * s0;   /* OSC1 heard; its sample is OSC2's volume */
        if (v->doc_out != want)
            mismatch++;
    }
    CHECK(mismatch == 0, "%d AM frames differ from OSC1 + OSC2 x OSC1 sample", mismatch);
}

static void test_filter(void)
{
    static float l[44100], r[44100];
    double hf[3];
    int k, i;
    for (k = 0; k < 3; k++) {
        esq_patch_t p = one_osc(0);
        p.flt.freq = k == 0 ? 15 : 110;
        p.flt.q = k == 2 ? 31 : 0;
        setup(&p);
        esq_note_on(&syn, 48, 100);
        esq_render(&syn, l, r, 44100);
        hf[k] = 0;
        for (i = 22050; i < 44100; i++)
            hf[k] += (double)(l[i] - l[i - 1]) * (l[i] - l[i - 1]);
    }
    CHECK(hf[0] < hf[1] * 0.01, "closed filter HF energy %.3g vs open %.3g", hf[0], hf[1]);
    {   /* resonance just below self-oscillation at Q 31: a ringing peak, but no oscillation without input */
        esq_patch_t p = one_osc(0);
        float peak = 0.0f;
        p.flt.freq = 60;
        p.flt.q = 31;
        p.dca[0].on = 0;
        setup(&p);
        esq_note_on(&syn, 48, 100);
        syn.v[0].fs[0] = 0.1f;
        esq_render(&syn, l, r, 44100);
        for (i = 22050; i < 44100; i++)
            peak = fmaxf(peak, fabsf(l[i]));
        CHECK(peak < 1e-4f, "Q 31 self-oscillates (%.5f)", peak);
    }
}

static void test_bank_io(void)
{
    static esq_wavebank_t b2, src;
    src = bank;
    uint8_t pcb[2 * ESQ_PCB_LEN];
    long need;
    uint8_t *buf, *bad;
    int w, same = 1, err;
    esq_patch_to_pcb(&ESQ_DEMO_PATCHES[1], pcb);
    esq_patch_to_pcb(&ESQ_DEMO_PATCHES[2], pcb + ESQ_PCB_LEN);
    src.patches = pcb;
    src.npatches = 2;
    need = esq_wavebank_write(&src, 0, 0);
    buf = malloc((size_t)need);
    bad = malloc((size_t)need);
    CHECK(esq_wavebank_write(&src, buf, (size_t)need) == need, "write size");
    memcpy(bad, buf, (size_t)need);
    err = esq_wavebank_load(&b2, buf, (size_t)need);
    CHECK(err == 0, "round trip load: error %d", err);
    if (!err) {
        CHECK(b2.nwaves == bank.nwaves, "nwaves %d", b2.nwaves);
        for (w = 0; w < bank.nwaves; w++) {
            const esq_zone_t *a = esq_zone_of(&bank, (uint32_t)w, 0), *b = esq_zone_of(&b2, (uint32_t)w, 0);
            if (strcmp(bank.wave[w].name, b2.wave[w].name) || a->log2len != b->log2len || a->cycles_q16 != b->cycles_q16 ||
                memcmp(bank.data + a->offset, b2.data + b->offset, 1u << a->log2len))
                same = 0;
        }
        CHECK(same, "round-tripped bank differs");
        CHECK(b2.npatches == 2 && !memcmp(b2.patches, pcb, sizeof pcb), "PTCH chunk lost");
    }
    memcpy(buf, bad, (size_t)need);
    buf[0] = 'X';
    CHECK(esq_wavebank_load(&b2, buf, (size_t)need) == -1, "bad magic accepted");
    memcpy(buf, bad, (size_t)need);
    buf[12 + 12 + 8 + 3] = 0x7F;                           /* wave 0 zone 0: offset far past the data */
    CHECK(esq_wavebank_load(&b2, buf, (size_t)need) == -8, "out-of-range zone accepted");
    free(buf);
    free(bad);
}

static void test_patch_format(void)
{
    uint8_t pcb[ESQ_PCB_LEN], pcb2[ESQ_PCB_LEN], syx[ESQ_SYSEX_SINGLE_LEN + 3];
    esq_patch_t a, b[4];
    int i, ok = 1, n;
    for (i = 0; i < ESQ_NDEMO_PATCHES; i++) {
        esq_patch_to_pcb(&ESQ_DEMO_PATCHES[i], pcb);
        esq_patch_from_pcb(&a, pcb);
        esq_patch_to_pcb(&a, pcb2);
        if (memcmp(pcb, pcb2, ESQ_PCB_LEN) || strcmp(a.name, ESQ_DEMO_PATCHES[i].name) ||
            memcmp(&a.env, &ESQ_DEMO_PATCHES[i].env, sizeof a.env) || memcmp(&a.osc, &ESQ_DEMO_PATCHES[i].osc, sizeof a.osc))
            ok = 0;
    }
    CHECK(ok, "PCB round trip differs");
    esq_patch_to_pcb(&ESQ_DEMO_PATCHES[1], pcb);
    CHECK(pcb[58] == 36 && pcb[68] == 48 && pcb[89] == (0x80 | ESQ_DEMO_PATCHES[1].flt.freq) && (pcb[93] & 0x80),
          "PCB bytes: OSC1 SEMI %d, OSC2 SEMI %d, SYNC|FREQ %02x, MONO %02x", pcb[58], pcb[68], pcb[89], pcb[93]);
    syx[0] = 0xFE;                                         /* junk around the message */
    esq_sysex_single(&ESQ_DEMO_PATCHES[2], 3, syx + 1);
    syx[ESQ_SYSEX_SINGLE_LEN + 1] = 0xF8;
    syx[ESQ_SYSEX_SINGLE_LEN + 2] = 0x00;
    CHECK(syx[1] == 0xF0 && syx[4] == 0x03 && syx[5] == 0x01 && syx[ESQ_SYSEX_SINGLE_LEN] == 0xF7, "SysEx frame");
    n = esq_sysex_parse(syx, sizeof syx, b, 4);
    CHECK(n == 1 && !strcmp(b[0].name, "BELLPD") && b[0].flt.freq == ESQ_DEMO_PATCHES[2].flt.freq, "SysEx parse: %d", n);
    CHECK(esq_sysex_parse(pcb, sizeof pcb, b, 4) == -1, "SysEx parse found a program in junk");
}

static void test_zones(void)
{
    static uint8_t data[512];
    static esq_wavebank_t b;
    esq_patch_t p = one_osc(0);
    memset(&b, 0, sizeof(b));
    memset(data, 0x40, 256);
    memset(data + 256, 0xC0, 256);
    b.nwaves = 1;
    b.data = data;
    b.data_len = 512;
    b.wave[0] = (esq_wave_t){.name = "SPLIT", .nzones = 2, .zone0 = 0};
    b.nzone = 2;
    b.zone[0] = (esq_zone_t){.top_note = 59, .log2len = 8, .cycles_q16 = 1u << 16, .offset = 0};
    b.zone[1] = (esq_zone_t){.top_note = 127, .log2len = 8, .cycles_q16 = 1u << 16, .offset = 256};
    esq_init(&syn, RATE, &b);
    esq_set_patch(&syn, &p);
    esq_note_on(&syn, 50, 100);
    esq_note_on(&syn, 70, 100);
    CHECK(syn.v[0].tab[0] == data && syn.v[1].tab[0] == data + 256, "keymap zones not selected by note");
}

static void test_glide(void)
{
    esq_patch_t p = one_osc(0);
    int mid;
    p.modes.mono = 1;
    p.modes.glide = 30;                                    /* 0.45 s, any interval */
    setup(&p);
    esq_note_on(&syn, 60, 100);
    run(TICK * 3);
    CHECK(syn.v[0].gnote == 60 * 256, "mono, first key: glided (%d)", syn.v[0].gnote);
    esq_note_on(&syn, 72, 100);
    run((uint32_t)(RATE * 0.225f));
    mid = syn.v[0].gnote;
    CHECK(mid > 64 * 256 && mid < 68 * 256, "fingered glide at half time %.2f, want ~66", mid / 256.0);
    CHECK(esq_active_voices(&syn) == 1, "mono uses %d voices", esq_active_voices(&syn));
    run((uint32_t)(RATE * 0.3f));
    CHECK(syn.v[0].gnote == 72 * 256, "glide did not land (%.2f)", syn.v[0].gnote / 256.0);
    esq_note_off(&syn, 72);
    CHECK(syn.v[0].gate && syn.v[0].gtarget == 60 * 256, "release of the top note did not return to 60");
    esq_note_off(&syn, 60);
    CHECK(!syn.v[0].gate, "last release did not close the gate");
    /* poly: glides from the last note played */
    p.modes.mono = 0;
    setup(&p);
    esq_note_on(&syn, 48, 100);
    esq_note_on(&syn, 60, 100);
    run(TICK * 5);
    CHECK(syn.v[1].gnote > 48 * 256 && syn.v[1].gnote < 60 * 256, "poly glide: %.2f", syn.v[1].gnote / 256.0);
}

/* the per-voice API (the Felucca adapter's path): pitch from the host, release, end */
static void test_voice_api(void)
{
    static float out[88200 + ESQ_CHUNK];
    static esq_voice_t vv;
    esq_patch_t p = one_osc(2);
    uint32_t i;
    int alive = 1;
    double got;
    setup(&p);
    memset(&vv, 0, sizeof(vv));
    esq_voice_on(&syn, &vv, 60, 100);
    for (i = 0; i < 88200; i += ESQ_CHUNK)
        esq_voice_render(&syn, &vv, 69.0f, out + i, 1.0f, ESQ_CHUNK);   /* host says A4 whatever the note */
    got = measure_hz(out + 4410, 88200 - 4410);
    CHECK(fabs(got / 440.0 - 1.0) < 0.002, "per-voice pitch %.3f Hz, want 440", got);
    CHECK(esq_active_voices(&syn) == 0, "per-voice API used the synth's own voices");
    esq_voice_off(&syn, &vv);
    for (i = 0; i < 2 * 44100 / ESQ_CHUNK && alive; i++)
        alive = esq_voice_render(&syn, &vv, 69.0f, out, 1.0f, ESQ_CHUNK);
    CHECK(!alive, "voice still sounding 2 s after release");
}

/* the per-voice API's glide: the host's pitch is the note's, the program's glide rides on it */
static void test_voice_glide(void)
{
    static float out[ESQ_CHUNK];
    static esq_voice_t a, b;
    esq_patch_t p = one_osc(2);
    uint32_t i;
    int mid;
    p.modes.glide = 30;                                    /* 0.45 s */
    setup(&p);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    esq_voice_on(&syn, &a, 48, 100);
    esq_voice_render(&syn, &a, 48.0f, out, 1.0f, ESQ_CHUNK);
    esq_voice_on(&syn, &b, 60, 100);                       /* poly: from the last note played (48) */
    for (i = 0; i < (uint32_t)(RATE * 0.225f) / ESQ_CHUNK; i++)
        esq_voice_render(&syn, &b, 60.0f, out, 1.0f, ESQ_CHUNK);
    mid = b.gnote;
    CHECK(mid > 52 * 256 && mid < 56 * 256, "per-voice poly glide at half time %.2f, want ~54", mid / 256.0);
    for (i = 0; i < (uint32_t)(RATE * 0.3f) / ESQ_CHUNK; i++)
        esq_voice_render(&syn, &b, 60.5f, out, 1.0f, ESQ_CHUNK);   /* (a bend on top) */
    CHECK(b.gnote == 60 * 256 + 128, "per-voice glide did not land on the host's pitch (%.2f)", b.gnote / 256.0);
    /* mono legato: the host keeps the voice, the key moves 12 up */
    p.modes.mono = 1;
    setup(&p);
    memset(&a, 0, sizeof(a));
    esq_voice_on(&syn, &a, 60, 100);
    for (i = 0; i < 20; i++)
        esq_voice_render(&syn, &a, 60.0f, out, 1.0f, ESQ_CHUNK);
    CHECK(a.gnote == 60 * 256, "mono first key glided (%.2f)", a.gnote / 256.0);
    esq_voice_legato(&syn, &a, 72);
    for (i = 0; i < (uint32_t)(RATE * 0.225f) / ESQ_CHUNK; i++)
        esq_voice_render(&syn, &a, 72.0f, out, 1.0f, ESQ_CHUNK);
    CHECK(a.gnote > 64 * 256 && a.gnote < 68 * 256, "per-voice fingered glide at half time %.2f", a.gnote / 256.0);
    CHECK(a.env[3].stage != 1 || a.env[3].count > 0, "legato restarted the envelopes");
    p.modes.glide = 0;                                     /* GLIDE 0: straight there */
    setup(&p);
    memset(&a, 0, sizeof(a));
    esq_voice_on(&syn, &a, 60, 100);
    esq_voice_render(&syn, &a, 60.0f, out, 1.0f, ESQ_CHUNK);
    esq_voice_legato(&syn, &a, 67);
    esq_voice_render(&syn, &a, 67.0f, out, 1.0f, ESQ_CHUNK);
    CHECK(a.gnote == 67 * 256, "legato without GLIDE: %.2f", a.gnote / 256.0);
}

int main(void)
{
    esq_wavebank_placeholder(&bank);
    test_pitch();
    test_doc();
    test_env();
    test_lfo();
    test_cv();
    test_voices();
    test_health();
    test_sync();
    test_am();
    test_filter();
    test_bank_io();
    test_patch_format();
    test_zones();
    test_glide();
    test_voice_api();
    test_voice_glide();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
