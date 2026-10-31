/* ESQ-1 patch model.
 * Fields hold front-panel values in front-panel units (Musician's Manual, Appendix 3). esq_patch.c converts them to
 * and from the 102-byte PCB program format (Appendix 6) that SysEx dumps and the OS's internal patches use. */
#ifndef ESQ_TYPES_H
#define ESQ_TYPES_H

#include <stdint.h>

/* Modulation sources, in the ESQ-1's order (the PCB nibble values). */
enum {
    ESQ_SRC_LFO1, ESQ_SRC_LFO2, ESQ_SRC_LFO3,
    ESQ_SRC_ENV1, ESQ_SRC_ENV2, ESQ_SRC_ENV3, ESQ_SRC_ENV4,
    ESQ_SRC_VEL, ESQ_SRC_VEL2,
    ESQ_SRC_KYBD, ESQ_SRC_KYBD2,
    ESQ_SRC_WHEEL, ESQ_SRC_PEDAL,
    ESQ_SRC_PRESS, ESQ_SRC_XCTRL,
    ESQ_SRC_OFF,
    ESQ_NSRC
};

enum { ESQ_LFO_TRI, ESQ_LFO_SAW, ESQ_LFO_SQR, ESQ_LFO_NOISE };

typedef struct {
    uint8_t src;                 /* ESQ_SRC_* */
    int8_t amt;                  /* -63..63 */
} esq_mod_t;

typedef struct {
    int8_t semi;                 /* -36..36: OCT x 12 + SEMI */
    uint8_t fine;                /* 0..31, 1/32 semitone (about 3 cents) */
    uint8_t wave;                /* 0..31 the ESQ-1 set; higher: hidden ROM waves, custom banks */
    esq_mod_t fm[2];             /* pitch: amount 63 x source 63 is about 15 semitones */
} esq_osc_t;

typedef struct {
    uint8_t on;                  /* OUTPUT enable */
    uint8_t level;               /* 0..63 */
    esq_mod_t am[2];             /* level */
} esq_dca_t;

typedef struct {
    uint8_t freq;                /* 0..127 */
    uint8_t q;                   /* 0..31 */
    uint8_t keybd;               /* 0..63 keyboard tracking (63: about 1:1) */
    esq_mod_t mod[2];
} esq_filter_t;

typedef struct {
    uint8_t level;               /* 0..63: ENV4's depth on the final VCA */
    uint8_t pan;                 /* 0..15, 8 = centre */
    esq_mod_t panmod;
} esq_dca4_t;

typedef struct {
    int8_t l1, l2, l3;           /* -63..63 */
    uint8_t t1, t2, t3, t4;      /* 0..63 (0.01 .. 20.48 s, the manual's table) */
    uint8_t lv;                  /* 0..63: softer notes lower L1..L3 */
    uint8_t t1v;                 /* 0..63: harder notes shorten T1 */
    uint8_t tk;                  /* 0..63: higher keys shorten T2, T3 */
} esq_env_t;

typedef struct {
    uint8_t freq;                /* 0..63 */
    uint8_t wave;                /* ESQ_LFO_* (SQR is unipolar) */
    uint8_t l1, l2;              /* 0..63: depth at note-on, depth after the delay */
    uint8_t delay;               /* 0..63: the L1 -> L2 ramp rate (0: stays at L1) */
    uint8_t reset;               /* phase restarts at note-on */
    uint8_t human;               /* rate wanders */
    uint8_t mod_src;             /* ESQ_SRC_*: added to the depth */
} esq_lfo_t;

typedef struct {
    uint8_t sync;                /* OSC1 hard-syncs OSC2 */
    uint8_t am;                  /* OSC1 amplitude-modulates OSC2 (DCA2 ignored, OSC1 still heard through DCA1) */
    uint8_t mono;                /* one voice, fingered glide, no retrigger while a key is held */
    uint8_t glide;               /* 0..63 */
    uint8_t vc;                  /* VC: a restruck key takes its own voice back */
    uint8_t env_reset;           /* ENV: a restruck key restarts the envelopes from zero (off: from their level) */
    uint8_t osc_reset;           /* OSC: oscillators restart in phase at note-on */
    uint8_t cycle;               /* CYC: envelopes run T1..T4 and ignore the key */
} esq_modes_t;

typedef struct {                 /* kept for SysEx round trips; the engine plays a single program */
    uint8_t split_dir, split_point;
    uint8_t layer, layer_prog;
    uint8_t split, split_prog;
    uint8_t split_layer, split_layer_prog;
} esq_split_t;

/* (SQ1D R5) the extension block: what the 102-byte program has no room for. All zero is the original exactly.
 * Sources here: 0 OFF, 1..15 the ESQ-1's (LFO1 .. XCTRL, its index + 1), then RAND (per note, -127..127), ALT (0 / 127
 * on alternate notes), RR4 (0, 42, 85, 127 round the notes), ZONE (127 at and above ZONE KEY, else 0) */
enum { ESQ_XS_OFF, ESQ_XS_RAND = 16, ESQ_XS_ALT, ESQ_XS_RR4, ESQ_XS_ZONE, ESQ_NXS };
enum { ESQ_XD_OFF, ESQ_XD_PITCH, ESQ_XD_CUTOFF, ESQ_XD_Q, ESQ_XD_LEVEL, ESQ_XD_PAN, ESQ_XD_LFO1, ESQ_XD_SCAN, ESQ_NXD };
enum { ESQ_LSH_WAV, ESQ_LSH_SINE, ESQ_LSH_RAMPDN, ESQ_LSH_SH, ESQ_LSH_SMOOTH, ESQ_LSH_DRUNK, ESQ_NLSH };
#define ESQ_NSYNC 10             /* LFO SYNC: 0 FREQ, 1..9 4/1 2/1 1/1 1/2 1/4 1/8 1/16 1/4T 1/8T */
typedef struct {
    uint8_t lfo_shape[3];        /* ESQ_LSH_*: 0 the WAV field's */
    uint8_t lfo_sync[3];         /* 0: FREQ; else a note value at the tempo (esq_synth_t.tempo) */
    uint8_t lfo_oneshot;         /* bit l: one cycle, then it holds */
    uint8_t env_loop;            /* bit e: while the key is held, segments 1..3 again and again */
    uint8_t q_src;               /* Q modulation: source (ESQ_XS_*), amount -63..63 */
    int8_t q_amt;
    uint8_t scan_src[3];         /* wave scanning: each oscillator's wave moves by source x amount (63: 15 waves) */
    int8_t scan_amt[3];
    uint8_t uni;                 /* unison: voices a note (0, 1: one) */
    uint8_t uni_det;             /* .. detune of the outer voices, cents */
    uint8_t drift;               /* 0..63: each voice card's own tuning and cutoff, and a slow wander */
    uint8_t xm_src[2], xm_dst[2];   /* two more modulations: any source to ESQ_XD_* */
    int8_t xm_amt[2];
    uint8_t zone_key;            /* ZONE's split key */
    uint8_t pad[6];
} esq_ext_t;                     /* 32 bytes */

typedef struct {
    char name[7];                /* 6 characters + NUL */
    esq_osc_t osc[3];
    esq_dca_t dca[3];
    esq_filter_t flt;
    esq_dca4_t dca4;
    esq_env_t env[4];
    esq_lfo_t lfo[3];
    esq_modes_t modes;
    esq_split_t split;
    esq_ext_t x;                 /* (SQ1D R5) not in the PCB: esq_patch_from_pcb leaves it zero */
} esq_patch_t;

#endif
