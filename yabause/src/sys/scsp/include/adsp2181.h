/*  Copyright 2026 Kronos_test contributors

    ADSP-2181 interpreter. C port of the ADSP-21xx core of MAME
    (src/devices/cpu/adsp2100/adsp2100.cpp + 2100ops.hxx,
    license BSD-3-Clause, copyright-holders: Aaron Giles), reduced to
    the ADSP-2181 variant used by the Acclaim RAX sound board.

    This file is part of Yabause/Kronos.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef ADSP2181_H
#define ADSP2181_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ADSP-2181 interrupt inputs (same numbering as MAME adsp2100.h) */
#define ADSP2181_IRQ0       0   /* IRQ0 / SPORT1 receive */
#define ADSP2181_IRQ1       1   /* IRQ1 / SPORT1 transmit */
#define ADSP2181_IRQ2       2
#define ADSP2181_SPORT0_RX  3
#define ADSP2181_SPORT0_TX  4
#define ADSP2181_TIMER      5
#define ADSP2181_IRQE       6
#define ADSP2181_BDMA       7
#define ADSP2181_IRQL1      8
#define ADSP2181_IRQL0      9
#define ADSP2181_NUM_IRQ    10

#define ADSP_LINE_CLEAR     0
#define ADSP_LINE_ASSERT    1

/* 16-bit register that can be read signed or unsigned */
typedef union
{
   u16 u;
   s16 s;
} AdspReg16;

/* 40-bit MAC result kept in a 64-bit word exactly like MAME's adsp_mac
   union: bits 0-15 = MR0, 16-31 = MR1, 32-47 = MR2 (a full 16-bit field,
   read back sign-extended), 48-63 = unused upper bits. */
typedef union
{
   u64 mr;
   u16 w[4];
} AdspMac;

/* 32-bit shifter result: bits 0-15 = SR0, 16-31 = SR1 */
typedef union
{
   u32 sr;
   u16 w[2];
} AdspShift;

/* 16-bit halves of AdspMac / AdspShift, in host byte order */
#ifdef WORDS_BIGENDIAN
#define ADSP_MRW(k) (3 - (k))
#define ADSP_SRW(k) (1 - (k))
#else
#define ADSP_MRW(k) (k)
#define ADSP_SRW(k) (k)
#endif

/* Replicated (primary/secondary bank) computation registers */
typedef struct
{
   AdspReg16 ax0, ax1, ay0, ay1, ar, af;
   AdspReg16 mx0, mx1, my0, my1, mf;
   AdspMac   mr;
   AdspReg16 si, se, sb;
   AdspShift sr;
   AdspReg16 zero;            /* constant 0 operand */
} AdspCore;

typedef struct Adsp2181_s Adsp2181;

/* Board-side hooks. Program memory is a plain 16K x 24-bit array owned by
   the board (pram); data and I/O spaces go through callbacks. */
typedef struct
{
   void *user;
   u16  (*data_read)(void *user, u32 addr);
   void (*data_write)(void *user, u32 addr, u16 data);
   u16  (*io_read)(void *user, u32 addr);
   void (*io_write)(void *user, u32 addr, u16 data);
   void (*sport_tx)(void *user, int which, u32 data);
   void (*dmovlay)(void *user, u32 data);
} Adsp2181Callbacks;

struct Adsp2181_s
{
   Adsp2181Callbacks cb;
   u32 *pram;                 /* 0x4000 words, low 24 bits used */

   u32 pc, ppc, loop, loop_condition, cntr;
   u32 astat, sstat, mstat, mstat_prev, astat_clear, idle;

   AdspCore core;
   AdspCore alt;

   u32 i[8];
   s32 m[8];
   u32 l[8];
   u32 lmask[8];
   u32 base[8];
   u8  px;
   u32 pmovlay, dmovlay;

   u32 loop_stack[4];
   u32 cntr_stack[4];
   u32 pc_stack[16];
   u16 stat_stack[4][3];
   s32 pc_sp, cntr_sp, stat_sp, loop_sp;

   u8  flagout, flagin, fl0, fl1, fl2;

   u16 imask;
   u8  icntl;
   u8  ints_enabled;          /* global interrupt enable (ENA/DIS INTS) */
   u16 ifc;
   u8  irq_state[ADSP2181_NUM_IRQ];
   u8  irq_latch[ADSP2181_NUM_IRQ];

   int icount;

   /* operand register maps (MAME m_alu_xregs & co), point into `core` */
   u16 *alu_xp[8];
   u16 *alu_yp[4];
   u16 *mac_xp[8];
   u16 *mac_yp[4];
   u16 *shift_xp[8];

   /* Direct data-memory pages of 0x400 words (NULL = use the data_read /
      data_write callbacks). Set by the board with Adsp2181SetDataPage. */
   u16 *dpage[16];
};

void Adsp2181Init(Adsp2181 *a, u32 *pram, const Adsp2181Callbacks *cb);
void Adsp2181Reset(Adsp2181 *a);
/* Runs about `cycles` instructions (one cycle each, as in MAME). The slice
   ends early on IDLE and on a jump-to-self busy loop. */
void Adsp2181Execute(Adsp2181 *a, int cycles);
void Adsp2181SetIrqLine(Adsp2181 *a, int line, int state);
/* Map data-memory words [page * 0x400, page * 0x400 + 0x3ff] to `ptr`
   (NULL: back to the callbacks). */
void Adsp2181SetDataPage(Adsp2181 *a, int page, u16 *ptr);

/* DAG register access for the board (MAME set_state_int/state_int) */
u32  Adsp2181GetI(const Adsp2181 *a, int idx);
void Adsp2181SetI(Adsp2181 *a, int idx, u32 val);
s32  Adsp2181GetM(const Adsp2181 *a, int idx);
u32  Adsp2181GetL(const Adsp2181 *a, int idx);

/* Save states: CPU registers, stacks and interrupt state (not pram, not
   the board callbacks/data pages). Load needs an Adsp2181Init'ed core;
   returns 0 when applied. */
int  Adsp2181StateSize(void);
void Adsp2181SaveState(const Adsp2181 *a, void **stream);
int  Adsp2181LoadState(Adsp2181 *a, const void *stream, int size);

#ifdef __cplusplus
}
#endif

#endif
