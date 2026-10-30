/* SPDX-License-Identifier: GPL-3.0-only */
/* The ESQ-1 engine as SQ1D builds it: included by sq_esq.h before the engine's sources, so the engine and the app see
 * the same structures. Integers only (the FM-1 has no FPU), the voices are OberVave's eight cards (the per-voice API),
 * the zones and the knot curves read in place from flash. */
#ifndef ESQ_SQ1D_H
#define ESQ_SQ1D_H
#define ESQ_FIXED 1                       /* integers only, 44.1 kHz */
#define ESQ_HOST 0                        /* no C library, no float in the API */
#define ESQ_NVOICE 1                      /* the voices are the app's (esq_voice_on / _off / _render_q) */
#define ESQ_MAX_WAVES 76                  /* the ESQ-1's 75 */
#define ESQ_ZONE_POOL 344                 /* .. and their 340 zones (not in RAM: ESQ_ZONE_INPLACE) */
#define ESQ_ZONE_INPLACE 1                /* the zones read in place from the bank image */
#define ESQ_CONST_TABLES 1                /* the knot curves in flash (esq_ktab.h) */
#endif
