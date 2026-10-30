/* SPDX-License-Identifier: GPL-3.0-only */
/* The ESQ-1 engine as SQ1D builds it: included by sq_esq.h before the engine's sources, so the engine and the app see
 * the same structures. Integers only (the FM-1 has no FPU), eight voices the engine allocates itself, as the OS does (the whole-synth API),
 * the zones and the knot curves read in place from flash. */
#ifndef ESQ_SQ1D_H
#define ESQ_SQ1D_H
#define ESQ_FIXED 1                       /* integers only, 44.1 kHz */
#define ESQ_HOST 0                        /* no C library, no float in the API */
#define ESQ_NVOICE 8                      /* the engine allocates its eight voices itself, as OS 3.5 does (esq_note_on) */
#define ESQ_MAX_WAVES 76                  /* the ESQ-1's 75 */
#define ESQ_ZONE_POOL 344                 /* .. and their 340 zones (not in RAM: ESQ_ZONE_INPLACE) */
#define ESQ_ZONE_INPLACE 1                /* the zones read in place from the bank image */
#define ESQ_CONST_TABLES 1                /* the knot curves in flash (esq_ktab.h) */
/* (R4) the device build (tools/build.py: -DSQ1D_ESQ_SPLIT=1) compiles esq_engine.c as its own object at -O2, the rest
 * for size, as Felucca does, and puts a second copy of the per-sample loop in .ram_text (crt0 copies it to RAM; no
 * calls in it: build.py checks); esq_ram_on picks it. The host includes the engine in the unity build */
#if SQ1D_ESQ_SPLIT
#define ESQ_SPLIT 1
#ifndef SQ1D_ESQ_NORAM
#define ESQ_RAMFN __attribute__((section(".ram_text.esq"), noinline, used))
#endif
#elif defined(SQ1D_ESQ_RAM_TEST)          /* (host: the RAM copy built and picked, without the section) */
#define ESQ_RAMFN __attribute__((noinline, used))
#endif
#endif
