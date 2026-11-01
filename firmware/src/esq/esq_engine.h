/* ESQ-1 voice engine, v2: the OS 3.5 control path and the voice board's analog path.
 *
 * Control (integer, as the 6809 OS computes it; see docs/HARDWARE.md for the addresses it was read from):
 *   every 10 ms per voice: envelopes (linear segments of the manual's 0.01..20.48 s), LFOs (8-bit phase), glide,
 *     the mod matrix, then the DOC registers (frequency, volume averaged with the last value) and the CV targets
 *   every 2.5 ms: VCA and filter CVs step a quarter of the way to their targets (8-bit DAC values), pan and Q follow
 * Audio (per voice; float, or with ESQ_FIXED=1 in integers, as the FM-1 needs it):
 *   3 DOC oscillators at clock / 8 / 26 = 38.46 kHz (8-bit tables, no interpolation, SYNC, AM)
 *     -> DAC, held on the voice's 15 nF capacitor (3 x 0.5 us strobes a frame into 4.5 k: 0.068 per oscillator,
 *        a 3 kHz one-pole), solved exactly for the DAC's staircase and averaged over each output sample (which
 *        band-limits it: the folded images stay 40 dB under the signal in band, docs/HARDWARE.md)
 *     -> CEM3379 filter: 4 equal poles, exponential CV (19.75 mV/octave behind the OS's FC divider), input clip
 *        (1% THD at 360 mVpp), resonance through a VCA with input compensation, x7.5, 5 Vpp output swing
 *     -> AC-coupled current into the final VCA (linear above 0.2 V CV, exponential knee below; 2 ms CV smoothing)
 *     -> pan OTAs (3080 type, driven through 470 ohm: they saturate on loud voices), 4.7 ms CV smoothing
 *   output stage (esq_render): 20.8 kHz and 15.4 kHz poles, 15 Hz high-pass, clip. */
#ifndef ESQ_ENGINE_H
#define ESQ_ENGINE_H

#include <stdint.h>

#include "esq_types.h"
#include "esq_waves.h"

/* Build options:
 *   ESQ_FIXED  1: the audio and control paths in integers only, from tables tools/esq_qgen.c makes out of the float
 *              model (engine/esq_qtab.h); 44.1 kHz only. 0: float, the reference
 *   ESQ_HOST   0: no C library headers and no float in the API (the Felucca device build, with ESQ_FIXED)
 *   ESQ_NVOICE the whole-synth API's voices (esq_note_on, esq_render); a host of the per-voice API can set 1 */
#ifndef ESQ_FIXED
#define ESQ_FIXED 0
#endif
#ifndef ESQ_CONST_TABLES
#define ESQ_CONST_TABLES 0         /* 1: the four knot curves in flash (esq_ktab.h), not made in RAM */
#endif
#ifndef ESQ_HOST
#define ESQ_HOST 1
#endif
#if !ESQ_HOST && !ESQ_FIXED
#error "ESQ_HOST 0 needs ESQ_FIXED 1"
#endif
#ifndef ESQ_NVOICE
#define ESQ_NVOICE 8
#endif
/*   ESQ_SPLIT  1: esq_engine.c is compiled as its own object (the Felucca device build, at -O2 while the rest is
 *              built for size): the tables and helpers the host's pages read get linkage (ESQ_SHARED), declared
 *              below. 0: they stay static (a host that includes esq_engine.c) */
#ifndef ESQ_SPLIT
#define ESQ_SPLIT 0
#endif
#if ESQ_SPLIT
#define ESQ_SHARED
#else
#define ESQ_SHARED static
#endif
/* the whole-synth API (esq_note_on, esq_render ..): with more than one voice of its own; a host of the per-voice API
 * (ESQ_NVOICE 1, Felucca) leaves it out (about 1.5 KB of code) */
#define ESQ_WHOLE (ESQ_NVOICE > 1)
/* code off the audio path (set-up, note-on, note-off) built for size in a split build at -O2 (clang's minsize) */
#if ESQ_SPLIT && defined(__clang__)
#define ESQ_COLD __attribute__((minsize, noinline))
#else
#define ESQ_COLD
#endif
#define ESQ_CHUNK 32                /* audio chunk (samples): the DOC frame schedule */
#define ESQ_CTRL_DIV ESQ_CHUNK      /* (the largest block esq_voice_render takes in one call is unlimited; kept for hosts) */
#define ESQ_HELD_MAX 16
#define ESQ_HOLD_SUB 128            /* a DOC frame's time within an output sample: 1/128 sample */
#define ESQ_FIXED_FS 44100          /* the integer build's sample rate */

/* DOC frame rate = clock / 8 / (enabled oscillators + 2): the ESQ-1 clocks the ES5503 at 8 MHz and OS 3.5 enables
 * 24 oscillators (it writes 0x2E to register 0xE1 at reset), so 38.46 kHz. */
#define ESQ_DOC_CLOCK_HZ 8000000.0f
#define ESQ_DOC_OSCS 24
#define ESQ_DOC_RATE_DEFAULT (ESQ_DOC_CLOCK_HZ / 8.0f / (float)(ESQ_DOC_OSCS + 2))

#define ESQ_TICK_S 0.010f           /* the OS's per-voice update (envelope times are counts of it) */
#define ESQ_CV_STEPS 4              /* CV ramp steps per update */

typedef struct {
    int16_t level;               /* 8.8, -126..126 */
    int16_t rate;                /* 8.8 per tick */
    int8_t target;
    uint8_t stage;               /* 0 idle, 1..3 segments to L1 L2 L3, 4 sustain, 5 release */
    uint16_t count;              /* ticks left in the segment */
} esq_envst_t;

typedef struct {
    uint8_t phase, mask, ctr;
    int8_t out;                  /* the modulation value: wave x depth / 256 */
    int16_t rate;                /* FREQ, plus HUMAN's drift */
    int16_t level;               /* depth x 2 */
    uint16_t ph16;               /* (R5) SYNC's phase, 1/256 of a step */
    int8_t r0, r1;               /* (R5) S&H / SMOOTH / DRUNK: this cycle's value, the next one's */
    uint8_t done;                /* (R5) one-shot: played its cycle */
} esq_lfost_t;

typedef struct {
    uint8_t active, gate, note, vel;
    uint8_t sustained;           /* note-off held by the sustain pedal */
    uint32_t age;
    /* control (OS units) */
    int32_t gnote, gtarget, grate;   /* glide, 8.8 semitones */
    esq_envst_t env[4];
    esq_lfost_t lfo[3];
    int32_t vca_acc, vca_slope, fc_acc, fc_slope;   /* 8.8 DAC values */
    uint8_t cv_step, q_dac, pan_dac, pan_new;
    /* DOC */
    uint32_t ph[3], inc[3];
    uint32_t vol[3];             /* volume registers, 0..255 */
    uint8_t shift[3];
    const uint8_t *tab[3];
#if ESQ_ZONE_INPLACE
    const esq_zone_t *rz[3];     /* the zone each oscillator plays, its 2^46 / cycles (esq_doc_inc) */
    uint32_t rcp[3];
#endif
    int32_t doc_out;             /* the DAC's value for this voice (sum of the three) */
    uint8_t oneshot, halted;     /* bit o: oscillator o plays a one-shot zone / has played it to its end (silent) */
    int16_t fc_drift;            /* 1/256 semitone on this voice's cutoff (a host's voice-card drift), with cut_offset */
    int16_t pan_bias;            /* added to the pan DAC (0..255): a host's spread of its voices across the field */
    int16_t detune;              /* (R5) 1/256 semitone: unison */
    int8_t xrnd, card_p, card_f, wander;   /* (R5) RAND this note; the voice card's own pitch / cutoff; DRIFT's walk */
    uint8_t xalt, xrr;           /* (R5) ALT, RR4 this note */
    int8_t xlfo1;                /* (R5) an extra modulation of LFO1's rate (the last update's) */
    uint32_t doc_nb;             /* per-voice API: samples to the voice's next DOC frame (Q24) */
#if ESQ_FIXED
    int32_t cv_wait;             /* samples to the next CV step (Q8) */
    int32_t hold, hold_x;        /* the hold capacitor and the DAC level it charges to (DOC units x 4) */
    int32_t fs[4];               /* filter stages (Q24, in units of the filter's input clip) */
    int32_t kx, k, a4, c3, c2, c1, c0, G;   /* the CV step's filter: kx DAC -> input, k Q28, the rest Q30 */
    int64_t hp;                  /* VCA input coupling (Q32, in units of the filter's output swing) */
    int32_t vg, vg_t;            /* VCA gain into the pan OTA's argument, smoothed and target (Q28) */
    int32_t pl, pr, pl_t, pr_t;  /* pan gains (Q15) */
#else
    float cv_wait;               /* samples to the next CV step */
    float hold, hold_x;          /* the hold capacitor and the DAC level it charges to (V) */
    float fs[4];                 /* filter stages */
    float G, G2, G3, G4, k, kin, knorm;
    float hp;                    /* VCA input coupling */
    float vg, vg_t;              /* VCA gain, smoothed and target */
    float pl, pr, pl_t, pr_t;    /* pan gains, smoothed and target */
#endif
} esq_voice_t;

typedef struct {
    uint32_t doc_period;         /* samples per DOC frame (Q24) */
    uint32_t doc_nb;             /* samples to the next DOC frame (Q24; esq_render's) */
    const esq_wavebank_t *bank;
    esq_patch_t patch;
#if ESQ_WHOLE || !ESQ_FIXED
    esq_voice_t v[ESQ_NVOICE];
#endif
    uint32_t age_counter;
    uint32_t rng;
    /* performance: 0..127 as the OS holds them */
    int wheel, pedal, press, xctrl;
    int volume;                  /* MIDI volume, 0..127: scales ENV4 into the final VCA */
    uint8_t sustain;
    int bend;                    /* 1/256 semitone (esq_bend: +-bend_range) */
    int bend_range;              /* semitones */
    int master_tune;             /* -31..31, 1/64 semitone */
    int8_t micro[12];            /* (R5) cents on each note of the octave (C ..): microtuning */
    uint8_t tempo;               /* (R5) BPM, for LFO SYNC */
    uint32_t xcount, xr;         /* (R5) notes started: ALT, RR4; R5's random generator */
    uint8_t nvoice;              /* (R6) VOICES: how many of v[] it uses (0: all) */
    int last_note;               /* poly glide starts from it (-1: none yet) */
    uint8_t held[ESQ_HELD_MAX], nheld;
    int cut_offset;              /* 1/256 semitone added to every voice's cutoff (a host's filter modulation) */
    int32_t *side;               /* (fixed, per-voice API) a host's side buffer: a mono voice also adds (L - R) / 2 of
                                  * its pan here (L = out + side, R = out - side); 0: none, mono as ever */
    int32_t *side_at;            /* .. at the chunk / run being rendered (internal) */
    uint8_t side_used;           /* a voice wrote to side (the host clears it) */
    uint32_t ticks;              /* (v6.6) the 10 ms control ticks its voices ran (a host's profile) */
#if ESQ_FIXED
    int32_t o_s1[2], o_s2[2];    /* output stage (Q24) */
    int64_t o_h[2];              /* .. its high-pass (Q32) */
    uint8_t out_clean;           /* (SQ1D) 1: the output stage's filters without its saturation (x 2, linear: the host
                                  * clips), as a per-voice host hears the voices */
#else
    float fs, doc_rate, out_gain;
    uint32_t inc_k;              /* 2 x 8.1758 Hz x 65536 / DOC rate, Q20 (esq_doc_inc) */
    /* derived from fs */
    float fast_len;              /* samples per CV step */
    float vca_a, pan_a, hp_a;
    float hold_e[ESQ_HOLD_SUB + 1], hold_w[ESQ_HOLD_SUB + 1];   /* the hold over d/128 sample: decay, average */
    float fc_g[256];             /* tan(pi fc / fs) for each FC DAC value */
    float vca_gain[256], pan_gl[256], pan_gr[256];
    /* output stage */
    float o_g1, o_g2, o_hp;
    float o_s1[2], o_s2[2], o_h[2];
#endif
} esq_synth_t;

void esq_init(esq_synth_t *s, uint32_t sample_rate, const esq_wavebank_t *bank);   /* (ESQ_FIXED: 44100 only) */
#if !ESQ_FIXED
void esq_set_doc_rate(esq_synth_t *s, float doc_rate);
#endif
void esq_set_patch(esq_synth_t *s, const esq_patch_t *p);
#if ESQ_WHOLE
void esq_note_on(esq_synth_t *s, int note, int vel);
void esq_note_off(esq_synth_t *s, int note);
void esq_all_off(esq_synth_t *s);
#endif
/* MIDI-style controls: cc 1 wheel, 4 pedal, 7 volume, 11 XCTRL, 64 sustain, 123 all notes off; pressure 0..127;
 * bend -8192..8191 */
#if ESQ_WHOLE
void esq_cc(esq_synth_t *s, int cc, int val);
#endif
void esq_pressure(esq_synth_t *s, int val);
void esq_bend(esq_synth_t *s, int val);
/* Render n stereo samples (overwrites), through the output stage (esq_render_q: full scale 1 << 24). */
#if ESQ_WHOLE
#if ESQ_HOST
void esq_render(esq_synth_t *s, float *out_l, float *out_r, uint32_t n);
#endif
#if ESQ_FIXED
void esq_render_q(esq_synth_t *s, int32_t *out_l, int32_t *out_r, uint32_t n);
#endif
int esq_active_voices(const esq_synth_t *s);
#endif

/* Per-voice API, for hosts that allocate voices themselves (the Felucca engine): `s` carries the patch, bank,
 * DOC rate and controls, its own s->v[] stay unused. The render adds n samples of the voice (after the final VCA and
 * the pan OTA's saturation, no pan, no output stage) x gain to out: a saturated voice peaks at gain / 4. `pitch` is
 * the note's (MIDI note plus bend and tune: semitones as a float, esq_voice_render_q 1/256 semitone), and the
 * program's glide still to go rides on it (polyphonic glide from the last note, as esq_note_on). esq_voice_render_q
 * takes gain up to 1 << 17. Returns 0 once the voice has ended.
 * esq_voice_legato: the host moved a held voice to another key without a new note (mono legato): the fingered glide.
 * esq_voice_glide_from: glide from pitch `from` (8.8 semitones, in the frame where the voice's note is gtarget), e.g.
 * a retriggered mono voice from where it was. */
void esq_voice_on(esq_synth_t *s, esq_voice_t *v, int note, int vel);
void esq_voice_off(esq_synth_t *s, esq_voice_t *v);
void esq_voice_legato(esq_synth_t *s, esq_voice_t *v, int note);
void esq_voice_glide_from(esq_synth_t *s, esq_voice_t *v, int32_t from);
#if ESQ_FIXED
int esq_voice_render_q(esq_synth_t *s, esq_voice_t *v, int32_t pitch, int32_t *out, int32_t gain, uint32_t n);
#else
int esq_voice_render(esq_synth_t *s, esq_voice_t *v, float pitch, float *out, float gain, uint32_t n);
#endif

/* Response curves, for tests, docs and the pages. */
#if ESQ_HOST
float esq_env_time_s(int t);              /* segment time for T (the manual's table) */
float esq_lfo_hz(int f);                  /* LFO rate for FREQ */
float esq_fc_hz(int dac);                 /* filter cutoff for an FC DAC value */
#endif
int esq_env_ticks(int t);                 /* segment time for T, 10 ms ticks */
int esq_lfo_mhz(int f);                   /* LFO rate for FREQ, mHz */
int esq_fc_hz_i(int dac);                 /* filter cutoff for an FC DAC value, Hz */
int esq_fc_dac(int freq, int mod);        /* FC DAC value for FREQ plus a modulation sum (8.8, OS units) */
#if ESQ_WHOLE
int esq_doc_volume(int index);            /* DOC volume register for a DCA index 0..127 (2 x LEVEL + mods) */
#endif
#if ESQ_WHOLE
int esq_vca_dac(int index);               /* VCA DAC value for a final-VCA index 0..127 */
#endif

#if ESQ_SPLIT && ESQ_FIXED                /* what the host's pages read of the engine's own (ESQ_SHARED) */
extern const uint16_t esq_ticks[64];      /* segment ticks by T */
extern const int8_t esq_noise[256];       /* LFO NOISE's cycle */
extern const uint32_t esq_q_exp[384];     /* 2^(i / 384), Q31 */
extern const int32_t esq_q_k[32];         /* the resonance by Q, Q28 */
int esq_env_level(const esq_env_t *p, const esq_voice_t *v, int l);
#endif

#if ESQ_FIXED && defined(ESQ_RAMFN)      /* (v6.5.8) the per-sample loop: 1 its RAM copy, 0 the one in flash */
extern int esq_ram_on;
#endif

#endif
