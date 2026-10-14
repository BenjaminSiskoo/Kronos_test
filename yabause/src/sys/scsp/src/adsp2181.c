/*  Copyright 2026 Kronos_test contributors

    ADSP-2181 interpreter. C port of the ADSP-21xx core of MAME
    (src/devices/cpu/adsp2100/adsp2100.cpp + 2100ops.hxx,
    license BSD-3-Clause, copyright-holders: Aaron Giles), reduced to
    the ADSP-2181 variant used by the Acclaim RAX sound board
    (Batman Forever ST-V). Instruction semantics, flag computation,
    stacks and interrupt priorities follow MAME line for line. No
    ADSP-2181 documentation is in the workspace: references are the
    "ADSP-2100 Family User's Manual" (3rd ed., Analog Devices 1995) and
    the "ADSP-2181 Data Sheet" (Rev. D).

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

#include <string.h>
#include "adsp2181.h"

/* ASTAT flags */
#define SSFLAG 0x80
#define MVFLAG 0x40
#define QFLAG  0x20
#define SFLAG  0x10
#define CFLAG  0x08
#define VFLAG  0x04
#define NFLAG  0x02
#define ZFLAG  0x01

/* MSTAT bits */
#define MSTAT_BANK      0x01
#define MSTAT_REVERSE   0x02
#define MSTAT_STICKYV   0x04
#define MSTAT_SATURATE  0x08
#define MSTAT_INTEGER   0x10
#define MSTAT_TIMER     0x20
#define MSTAT_GOMODE    0x40

/* SSTAT bits */
#define PC_EMPTY     0x01
#define PC_OVER      0x02
#define COUNT_EMPTY  0x04
#define COUNT_OVER   0x08
#define STATUS_EMPTY 0x10
#define STATUS_OVER  0x20
#define LOOP_EMPTY   0x40
#define LOOP_OVER    0x80

#define PC_STACK_DEPTH   16
#define CNTR_STACK_DEPTH 4
#define STAT_STACK_DEPTH 4
#define LOOP_STACK_DEPTH 4

#define MSTAT_MASK 0x7f
#define IMASK_MASK 0x3ff

#define BITS(x, n, w) (((u32)(x) >> (n)) & ((1u << (w)) - 1))
#define BIT1(x, n)    (((u32)(x) >> (n)) & 1u)

static s32 sext(u32 val, int bits)
{
   u32 m = 1u << (bits - 1);
   val &= (1u << bits) - 1;
   return (s32)((val ^ m) - m);
}

/* ADSP-218x ALU constants (2100ops.hxx) */
static const s32 adsp_constants[32] =
{
   0x0001, 0xfffe, 0x0002, 0xfffd, 0x0004, 0xfffb, 0x0008, 0xfff7,
   0x0010, 0xffef, 0x0020, 0xffdf, 0x0040, 0xffbf, 0x0080, 0xff7f,
   0x0100, 0xfeff, 0x0200, 0xfdff, 0x0400, 0xfbff, 0x0800, 0xf7ff,
   0x1000, 0xefff, 0x2000, 0xdfff, 0x4000, 0xbfff, 0x8000, 0x7fff
};

static u8  condition_table[0x1000];
static u16 mask_table[0x4000];
static u16 reverse_table[0x4000];
static int tables_ready = 0;

static void create_tables(void)
{
   int i;
   for (i = 0; i < 0x4000; i++)
   {
      u16 data = 0;
      data |= (i >> 13) & 0x0001;
      data |= (i >> 11) & 0x0002;
      data |= (i >> 9)  & 0x0004;
      data |= (i >> 7)  & 0x0008;
      data |= (i >> 5)  & 0x0010;
      data |= (i >> 3)  & 0x0020;
      data |= (i >> 1)  & 0x0040;
      data |= (i << 1)  & 0x0080;
      data |= (i << 3)  & 0x0100;
      data |= (i << 5)  & 0x0200;
      data |= (i << 7)  & 0x0400;
      data |= (i << 9)  & 0x0800;
      data |= (i << 11) & 0x1000;
      data |= (i << 13) & 0x2000;
      reverse_table[i] = data;
   }

   for (i = 0; i < 0x4000; i++)
   {
      if (i > 0x2000)      mask_table[i] = 0x0000;
      else if (i > 0x1000) mask_table[i] = 0x2000;
      else if (i > 0x0800) mask_table[i] = 0x3000;
      else if (i > 0x0400) mask_table[i] = 0x3800;
      else if (i > 0x0200) mask_table[i] = 0x3c00;
      else if (i > 0x0100) mask_table[i] = 0x3e00;
      else if (i > 0x0080) mask_table[i] = 0x3f00;
      else if (i > 0x0040) mask_table[i] = 0x3f80;
      else if (i > 0x0020) mask_table[i] = 0x3fc0;
      else if (i > 0x0010) mask_table[i] = 0x3fe0;
      else if (i > 0x0008) mask_table[i] = 0x3ff0;
      else if (i > 0x0004) mask_table[i] = 0x3ff8;
      else if (i > 0x0002) mask_table[i] = 0x3ffc;
      else if (i > 0x0001) mask_table[i] = 0x3ffe;
      else                 mask_table[i] = 0x3fff;
   }

   memset(condition_table, 0, sizeof(condition_table));
   for (i = 0; i < 0x100; i++)
   {
      int az = ((i & ZFLAG) != 0);
      int an = ((i & NFLAG) != 0);
      int av = ((i & VFLAG) != 0);
      int ac = ((i & CFLAG) != 0);
      int mv = ((i & MVFLAG) != 0);
      int as = ((i & SFLAG) != 0);

      condition_table[i | 0x000] = az;
      condition_table[i | 0x100] = !az;
      condition_table[i | 0x200] = !((an ^ av) | az);
      condition_table[i | 0x300] = (an ^ av) | az;
      condition_table[i | 0x400] = an ^ av;
      condition_table[i | 0x500] = !(an ^ av);
      condition_table[i | 0x600] = av;
      condition_table[i | 0x700] = !av;
      condition_table[i | 0x800] = ac;
      condition_table[i | 0x900] = !ac;
      condition_table[i | 0xa00] = as;
      condition_table[i | 0xb00] = !as;
      condition_table[i | 0xc00] = mv;
      condition_table[i | 0xd00] = !mv;
      /* 0xe00: CE, handled by slow_condition() */
      condition_table[i | 0xf00] = 1;
   }
   tables_ready = 1;
}

static int clz32(u32 x)
{
   int n = 0;
   if (x == 0) return 32;
   while (!(x & 0x80000000u)) { x <<= 1; n++; }
   return n;
}

/*---------------------------------------------------------------------------
   Memory accessors
---------------------------------------------------------------------------*/

static INLINE u16 data_read(Adsp2181 *a, u32 addr)
{
   u16 *p;
   addr &= 0x3fff;
   p = a->dpage[addr >> 10];
   if (p != NULL)
      return p[addr & 0x3ff];
   return a->cb.data_read(a->cb.user, addr);
}

static INLINE void data_write(Adsp2181 *a, u32 addr, u16 data)
{
   u16 *p;
   addr &= 0x3fff;
   p = a->dpage[addr >> 10];
   if (p != NULL)
      p[addr & 0x3ff] = data;
   else
      a->cb.data_write(a->cb.user, addr, data);
}

static INLINE u16 io_read(Adsp2181 *a, u32 addr)
{
   return a->cb.io_read(a->cb.user, addr & 0x7ff);
}

static INLINE void io_write(Adsp2181 *a, u32 addr, u16 data)
{
   a->cb.io_write(a->cb.user, addr & 0x7ff, data);
}

static INLINE u32 program_read(Adsp2181 *a, u32 addr)
{
   return a->pram[addr & 0x3fff];
}

static INLINE void program_write(Adsp2181 *a, u32 addr, u32 data)
{
   a->pram[addr & 0x3fff] = data & 0xffffff;
}

/*---------------------------------------------------------------------------
   Core register helpers (MR / SR sub-fields)
---------------------------------------------------------------------------*/

#define MR0_U(c) ((c)->mr.w[ADSP_MRW(0)])
#define MR1_U(c) ((c)->mr.w[ADSP_MRW(1)])
#define MR2_U(c) ((c)->mr.w[ADSP_MRW(2)])
#define SR0_U(c) ((c)->sr.w[ADSP_SRW(0)])
#define SR1_U(c) ((c)->sr.w[ADSP_SRW(1)])

static INLINE void set_mr0(AdspCore *c, u16 v) { c->mr.w[ADSP_MRW(0)] = v; }
static INLINE void set_mr1(AdspCore *c, u16 v) { c->mr.w[ADSP_MRW(1)] = v; }
static INLINE void set_mr2(AdspCore *c, u16 v) { c->mr.w[ADSP_MRW(2)] = v; }
static INLINE void set_sr0(AdspCore *c, u16 v) { c->sr.w[ADSP_SRW(0)] = v; }
static INLINE void set_sr1(AdspCore *c, u16 v) { c->sr.w[ADSP_SRW(1)] = v; }

/* Operand selection through pointer tables, as MAME does:
   ALU X:   AX0 AX1 AR MR0 MR1 MR2 SR0 SR1    ALU Y:   AY0 AY1 AF 0
   MAC X:   MX0 MX1 AR MR0 MR1 MR2 SR0 SR1    MAC Y:   MY0 MY1 MF 0
   SHIFT X: SI  SI  AR MR0 MR1 MR2 SR0 SR1 */
static void build_operand_maps(Adsp2181 *a)
{
   AdspCore *c = &a->core;
   u16 *common[6];
   int k;

   common[0] = &c->ar.u;
   common[1] = &MR0_U(c);
   common[2] = &MR1_U(c);
   common[3] = &MR2_U(c);
   common[4] = &SR0_U(c);
   common[5] = &SR1_U(c);
   for (k = 0; k < 6; k++)
      a->alu_xp[2 + k] = a->mac_xp[2 + k] = a->shift_xp[2 + k] = common[k];

   a->alu_xp[0] = &c->ax0.u;   a->alu_xp[1] = &c->ax1.u;
   a->mac_xp[0] = &c->mx0.u;   a->mac_xp[1] = &c->mx1.u;
   a->shift_xp[0] = &c->si.u;  a->shift_xp[1] = &c->si.u;
   a->alu_yp[0] = &c->ay0.u;   a->alu_yp[1] = &c->ay1.u;
   a->alu_yp[2] = &c->af.u;    a->alu_yp[3] = &c->zero.u;
   a->mac_yp[0] = &c->my0.u;   a->mac_yp[1] = &c->my1.u;
   a->mac_yp[2] = &c->mf.u;    a->mac_yp[3] = &c->zero.u;
}

#define alu_x(a, x)   (*(a)->alu_xp[(x) & 7])
#define alu_y(a, y)   (*(a)->alu_yp[(y) & 3])
#define mac_x(a, x)   (*(a)->mac_xp[(x) & 7])
#define mac_y(a, y)   (*(a)->mac_yp[(y) & 3])
#define shift_x(a, x) (*(a)->shift_xp[(x) & 7])

/*---------------------------------------------------------------------------
   Flags
---------------------------------------------------------------------------*/

#define GET_SS (a->astat & SSFLAG)
#define GET_MV (a->astat & MVFLAG)
#define GET_Q  (a->astat & QFLAG)
#define GET_C  (a->astat & CFLAG)
#define GET_V  (a->astat & VFLAG)

#define CLR_FLAGS        (a->astat &= a->astat_clear)
#define CALC_Z(r)        (a->astat |= (((r) & 0xffff) == 0))
#define CALC_N(r)        (a->astat |= ((r) >> 14) & 0x02)
#define CALC_V(s,d,r)    (a->astat |= (((s) ^ (d) ^ (r) ^ ((r) >> 1)) >> 13) & 0x04)
#define CALC_C(r)        (a->astat |= ((r) >> 13) & 0x08)
#define CALC_C_SUB(r)    (a->astat |= (~(r) >> 13) & 0x08)
#define CALC_NZ(r)       do { CLR_FLAGS; CALC_N(r); CALC_Z(r); } while (0)
#define CALC_NZVC(s,d,r) do { CLR_FLAGS; CALC_N(r); CALC_Z(r); CALC_V(s,d,r); CALC_C(r); } while (0)
#define CALC_NZVC_SUB(s,d,r) do { CLR_FLAGS; CALC_N(r); CALC_Z(r); CALC_V(s,d,r); CALC_C_SUB(r); } while (0)

static void check_irqs(Adsp2181 *a);

static void update_mstat(Adsp2181 *a)
{
   if ((a->mstat ^ a->mstat_prev) & MSTAT_BANK)
   {
      AdspCore temp = a->core;
      a->core = a->alt;
      a->alt = temp;
   }
   /* MSTAT_TIMER: MAME calls m_timer_fired_cb, which the RAX board does
      not bind - the on-chip timer is not used/emulated there either. */
   if (a->mstat & MSTAT_STICKYV)
      a->astat_clear = ~(u32)(CFLAG | NFLAG | ZFLAG);
   else
      a->astat_clear = ~(u32)(CFLAG | VFLAG | NFLAG | ZFLAG);
   a->mstat_prev = a->mstat;
}

/*---------------------------------------------------------------------------
   Stacks
---------------------------------------------------------------------------*/

static INLINE u32 pc_stack_top(Adsp2181 *a)
{
   return (a->pc_sp > 0) ? a->pc_stack[a->pc_sp - 1] : a->pc_stack[0];
}

static INLINE void pc_stack_push(Adsp2181 *a)
{
   if (a->pc_sp < PC_STACK_DEPTH)
   {
      a->pc_stack[a->pc_sp] = a->pc;
      a->pc_sp++;
      a->sstat &= ~PC_EMPTY;
   }
   else
      a->sstat |= PC_OVER;
}

static INLINE void pc_stack_push_val(Adsp2181 *a, u32 val)
{
   if (a->pc_sp < PC_STACK_DEPTH)
   {
      a->pc_stack[a->pc_sp] = val;
      a->pc_sp++;
      a->sstat &= ~PC_EMPTY;
   }
   else
      a->sstat |= PC_OVER;
}

static INLINE void pc_stack_pop(Adsp2181 *a)
{
   if (a->pc_sp > 0)
   {
      a->pc_sp--;
      if (a->pc_sp == 0)
         a->sstat |= PC_EMPTY;
   }
   a->pc = a->pc_stack[a->pc_sp];
}

static INLINE u32 pc_stack_pop_val(Adsp2181 *a)
{
   if (a->pc_sp > 0)
   {
      a->pc_sp--;
      if (a->pc_sp == 0)
         a->sstat |= PC_EMPTY;
   }
   return a->pc_stack[a->pc_sp];
}

static INLINE void cntr_stack_push(Adsp2181 *a)
{
   if (a->cntr_sp < CNTR_STACK_DEPTH)
   {
      a->cntr_stack[a->cntr_sp] = a->cntr;
      a->cntr_sp++;
      a->sstat &= ~COUNT_EMPTY;
   }
   else
      a->sstat |= COUNT_OVER;
}

static INLINE void cntr_stack_pop(Adsp2181 *a)
{
   if (a->cntr_sp > 0)
   {
      a->cntr_sp--;
      if (a->cntr_sp == 0)
         a->sstat |= COUNT_EMPTY;
   }
   a->cntr = a->cntr_stack[a->cntr_sp];
}

static INLINE void loop_stack_push(Adsp2181 *a, u32 value)
{
   if (a->loop_sp < LOOP_STACK_DEPTH)
   {
      a->loop_stack[a->loop_sp] = value;
      a->loop_sp++;
      a->loop = value >> 4;
      a->loop_condition = value & 15;
      a->sstat &= ~LOOP_EMPTY;
   }
   else
      a->sstat |= LOOP_OVER;
}

static INLINE void loop_stack_pop(Adsp2181 *a)
{
   if (a->loop_sp > 0)
   {
      a->loop_sp--;
      if (a->loop_sp == 0)
      {
         a->loop = 0xffff;
         a->loop_condition = 0;
         a->sstat |= LOOP_EMPTY;
      }
      else
      {
         a->loop = a->loop_stack[a->loop_sp - 1] >> 4;
         a->loop_condition = a->loop_stack[a->loop_sp - 1] & 15;
      }
   }
}

static INLINE void stat_stack_push(Adsp2181 *a)
{
   if (a->stat_sp < STAT_STACK_DEPTH)
   {
      a->stat_stack[a->stat_sp][0] = (u16)a->mstat;
      a->stat_stack[a->stat_sp][1] = a->imask;
      a->stat_stack[a->stat_sp][2] = (u16)a->astat;
      a->stat_sp++;
      a->sstat &= ~STATUS_EMPTY;
   }
   else
      a->sstat |= STATUS_OVER;
}

static void stat_stack_pop(Adsp2181 *a)
{
   if (a->stat_sp > 0)
   {
      a->stat_sp--;
      if (a->stat_sp == 0)
         a->sstat |= STATUS_EMPTY;
   }
   a->mstat = a->stat_stack[a->stat_sp][0];
   update_mstat(a);
   a->imask = a->stat_stack[a->stat_sp][1];
   a->astat = a->stat_stack[a->stat_sp][2];
   check_irqs(a);
}

/*---------------------------------------------------------------------------
   Conditions
---------------------------------------------------------------------------*/

static int slow_condition(Adsp2181 *a)
{
   if ((s32)--a->cntr > 0)
      return 1;
   cntr_stack_pop(a);
   return 0;
}

#define CONDITION(c) (((c) != 14) ? condition_table[((c) << 8) | a->astat] : slow_condition(a))

/*---------------------------------------------------------------------------
   Register writes / reads
---------------------------------------------------------------------------*/

static INLINE void update_i(Adsp2181 *a, int which)
{
   a->base[which] = a->i[which] & a->lmask[which];
}

static INLINE void update_l(Adsp2181 *a, int which)
{
   a->lmask[which] = mask_table[a->l[which] & 0x3fff];
   a->base[which] = a->i[which] & a->lmask[which];
}

static INLINE void update_dmovlay(Adsp2181 *a)
{
   if (a->cb.dmovlay)
      a->cb.dmovlay(a->cb.user, a->dmovlay);
}

static void write_reg0(Adsp2181 *a, int regnum, s32 val)
{
   AdspCore *c = &a->core;
   switch (regnum)
   {
      case 0x00: c->ax0.s = (s16)val; break;
      case 0x01: c->ax1.s = (s16)val; break;
      case 0x02: c->mx0.s = (s16)val; break;
      case 0x03: c->mx1.s = (s16)val; break;
      case 0x04: c->ay0.s = (s16)val; break;
      case 0x05: c->ay1.s = (s16)val; break;
      case 0x06: c->my0.s = (s16)val; break;
      case 0x07: c->my1.s = (s16)val; break;
      case 0x08: c->si.s = (s16)val; break;
      case 0x09: c->se.s = (s8)val; break;
      case 0x0a: c->ar.s = (s16)val; break;
      case 0x0b: set_mr0(c, (u16)val); break;
      case 0x0c:
         set_mr1(c, (u16)val);
         set_mr2(c, (u16)(((s16)val) >> 15));
         break;
      case 0x0d: set_mr2(c, (u16)(s16)(s8)val); break;
      case 0x0e: set_sr0(c, (u16)val); break;
      case 0x0f: set_sr1(c, (u16)val); break;
   }
}

static void write_reg1(Adsp2181 *a, int regnum, s32 val)
{
   int index = regnum & 3;
   switch (regnum >> 2)
   {
      case 0:
         a->i[index] = val & 0x3fff;
         update_i(a, index);
         break;
      case 1:
         a->m[index] = sext((u32)val, 14);
         break;
      case 2:
         a->l[index] = val & 0x3fff;
         update_l(a, index);
         break;
      case 3:
         /* ADSP-2181 overlay registers */
         if (index == 2)
            a->pmovlay = val & 0x3fff;
         else if (index == 3)
         {
            a->dmovlay = val & 0x3fff;
            update_dmovlay(a);
         }
         break;
   }
}

static void write_reg2(Adsp2181 *a, int regnum, s32 val)
{
   int index = 4 + (regnum & 3);
   switch (regnum >> 2)
   {
      case 0:
         a->i[index] = val & 0x3fff;
         update_i(a, index);
         break;
      case 1:
         a->m[index] = sext((u32)val, 14);
         break;
      case 2:
         a->l[index] = val & 0x3fff;
         update_l(a, index);
         break;
      case 3:
         /* invalid register */
         break;
   }
}

static void write_reg3(Adsp2181 *a, int regnum, s32 val)
{
   switch (regnum)
   {
      case 0x00: a->astat = val & 0x00ff; break;
      case 0x01: a->mstat = val & MSTAT_MASK; update_mstat(a); break;
      case 0x03: a->imask = val & IMASK_MASK; check_irqs(a); break;
      case 0x04: a->icntl = val & 0x001f; check_irqs(a); break;
      case 0x05: cntr_stack_push(a); a->cntr = val & 0x3fff; break;
      case 0x06: a->core.sb.s = (s16)sext((u32)val, 5); break;
      case 0x07: a->px = (u8)val; break;
      case 0x09: if (a->cb.sport_tx) a->cb.sport_tx(a->cb.user, 0, (u32)val); break;
      case 0x0b: if (a->cb.sport_tx) a->cb.sport_tx(a->cb.user, 1, (u32)val); break;
      case 0x0c:
         a->ifc = (u16)val;
         /* clear */
         if (BIT1(val, 1)) a->irq_latch[ADSP2181_IRQ0] = 0;
         if (BIT1(val, 2)) a->irq_latch[ADSP2181_IRQ1] = 0;
         if (BIT1(val, 3)) a->irq_latch[ADSP2181_BDMA] = 0;
         if (BIT1(val, 4)) a->irq_latch[ADSP2181_IRQE] = 0;
         if (BIT1(val, 5)) a->irq_latch[ADSP2181_SPORT0_RX] = 0;
         if (BIT1(val, 6)) a->irq_latch[ADSP2181_SPORT0_TX] = 0;
         if (BIT1(val, 7)) a->irq_latch[ADSP2181_IRQ2] = 0;
         /* force */
         if (BIT1(val, 9))  a->irq_latch[ADSP2181_IRQ0] = 1;
         if (BIT1(val, 10)) a->irq_latch[ADSP2181_IRQ1] = 1;
         if (BIT1(val, 11)) a->irq_latch[ADSP2181_BDMA] = 1;
         if (BIT1(val, 12)) a->irq_latch[ADSP2181_IRQE] = 1;
         if (BIT1(val, 13)) a->irq_latch[ADSP2181_SPORT0_RX] = 1;
         if (BIT1(val, 14)) a->irq_latch[ADSP2181_SPORT0_TX] = 1;
         if (BIT1(val, 15)) a->irq_latch[ADSP2181_IRQ2] = 1;
         check_irqs(a);
         break;
      case 0x0d: a->cntr = val & 0x3fff; break;
      case 0x0f: pc_stack_push_val(a, val & 0x3fff); break;
      default: break;
   }
}

static INLINE s32 read_reg0(Adsp2181 *a, int regnum)
{
   const AdspCore *c = &a->core;
   switch (regnum)
   {
      case 0x00: return c->ax0.s;
      case 0x01: return c->ax1.s;
      case 0x02: return c->mx0.s;
      case 0x03: return c->mx1.s;
      case 0x04: return c->ay0.s;
      case 0x05: return c->ay1.s;
      case 0x06: return c->my0.s;
      case 0x07: return c->my1.s;
      case 0x08: return c->si.s;
      case 0x09: return c->se.s;
      case 0x0a: return c->ar.s;
      case 0x0b: return (s16)MR0_U(c);
      case 0x0c: return (s16)MR1_U(c);
      case 0x0d: return (s16)MR2_U(c);
      case 0x0e: return (s16)SR0_U(c);
      default:   return (s16)SR1_U(c);
   }
}

static s32 read_reg1(Adsp2181 *a, int regnum)
{
   int index = regnum & 3;
   switch (regnum >> 2)
   {
      case 0: return (s32)a->i[index];
      case 1: return a->m[index];
      case 2: return (s32)a->l[index];
      default:
         /* 0xc/0xd alias L0/L1 in MAME; 0xe/0xf are PMOVLAY/DMOVLAY */
         if (regnum == 0xe) return (s32)a->pmovlay;
         if (regnum == 0xf) return (s32)a->dmovlay;
         return (s32)a->l[index];
   }
}

static s32 read_reg2(Adsp2181 *a, int regnum)
{
   int index = 4 + (regnum & 3);
   switch (regnum >> 2)
   {
      case 0: return (s32)a->i[index];
      case 1: return a->m[index];
      default: return (s32)a->l[index];
   }
}

static s32 read_reg3(Adsp2181 *a, int regnum)
{
   switch (regnum)
   {
      case 0x00: return (s32)a->astat;
      case 0x01: return (s32)a->mstat;
      case 0x02: return (s32)a->sstat;
      case 0x03: return a->imask;
      case 0x04: return a->icntl;
      case 0x05: return (s32)a->cntr;
      case 0x06: return a->core.sb.s;
      case 0x07: return a->px;
      case 0x08: return 0;    /* SPORT0 RX: not connected on RAX (MAME default 0) */
      case 0x0a: return 0;    /* SPORT1 RX */
      case 0x0f: return (s32)pc_stack_pop_val(a);
      default:   return 0;
   }
}

/*---------------------------------------------------------------------------
   Data address generators
---------------------------------------------------------------------------*/

static INLINE void modify_address(Adsp2181 *a, u32 ireg, u32 mreg)
{
   u32 base = a->base[ireg];
   u32 i = a->i[ireg];
   u32 l = a->l[ireg];

   i = (i + (u32)a->m[mreg]) & 0x3fff;
   if (i < base) i += l;
   else if (i >= base + l) i -= l;
   a->i[ireg] = i;
}

static INLINE void data_write_dag1(Adsp2181 *a, u32 op, s32 val)
{
   u32 ireg = BITS(op, 2, 2);
   u32 i = a->i[ireg];

   if (a->mstat & MSTAT_REVERSE)
      data_write(a, reverse_table[i & 0x3fff], (u16)val);
   else
      data_write(a, i, (u16)val);

   modify_address(a, ireg, BITS(op, 0, 2));
}

static INLINE u32 data_read_dag1(Adsp2181 *a, u32 op)
{
   u32 ireg = BITS(op, 2, 2);
   u32 i = a->i[ireg];
   u32 res;

   if (a->mstat & MSTAT_REVERSE)
      res = data_read(a, reverse_table[i & 0x3fff]);
   else
      res = data_read(a, i);

   modify_address(a, ireg, BITS(op, 0, 2));
   return res;
}

static INLINE void data_write_dag2(Adsp2181 *a, u32 op, s32 val)
{
   u32 ireg = 4 + BITS(op, 2, 2);
   data_write(a, a->i[ireg], (u16)val);
   modify_address(a, ireg, 4 + BITS(op, 0, 2));
}

static INLINE u32 data_read_dag2(Adsp2181 *a, u32 op)
{
   u32 ireg = 4 + BITS(op, 2, 2);
   u32 res = data_read(a, a->i[ireg]);
   modify_address(a, ireg, 4 + BITS(op, 0, 2));
   return res;
}

static INLINE void pgm_write_dag2(Adsp2181 *a, u32 op, s32 val)
{
   u32 ireg = 4 + BITS(op, 2, 2);
   program_write(a, a->i[ireg], ((u32)val << 8) | a->px);
   modify_address(a, ireg, 4 + BITS(op, 0, 2));
}

static INLINE u32 pgm_read_dag2(Adsp2181 *a, u32 op)
{
   u32 ireg = 4 + BITS(op, 2, 2);
   u32 res = program_read(a, a->i[ireg]);
   a->px = (u8)res;
   res >>= 8;
   modify_address(a, ireg, 4 + BITS(op, 0, 2));
   return res;
}

/*---------------------------------------------------------------------------
   ALU. alu_op_ar/af/none and their *_const forms in MAME only differ by
   where the Y operand comes from and where the result goes; the Y
   operand is read up front here (reading a register has no side effect).
---------------------------------------------------------------------------*/

static s32 alu_compute(Adsp2181 *a, u32 op, s32 yop)
{
   s32 xop = (s32)BITS(op, 8, 3);
   s32 res;

   switch (BITS(op, 13, 4))
   {
      case 0x00: /* Y */
         res = yop;
         CALC_NZ(res);
         break;
      case 0x01: /* Y + 1 */
         res = yop + 1;
         CALC_NZ(res);
         if (yop == 0x7fff) a->astat |= VFLAG;
         else if (yop == 0xffff) a->astat |= CFLAG;
         break;
      case 0x02: /* X + Y + C */
         xop = alu_x(a, xop);
         yop += GET_C >> 3;
         res = xop + yop;
         CALC_NZVC(xop, yop, res);
         break;
      case 0x03: /* X + Y */
         xop = alu_x(a, xop);
         res = xop + yop;
         CALC_NZVC(xop, yop, res);
         break;
      case 0x04: /* NOT Y */
         res = yop ^ 0xffff;
         CALC_NZ(res);
         break;
      case 0x05: /* -Y */
         res = -yop;
         CALC_NZ(res);
         if (yop == 0x8000) a->astat |= VFLAG;
         if (yop == 0x0000) a->astat |= CFLAG;
         break;
      case 0x06: /* X - Y + C - 1 */
         xop = alu_x(a, xop);
         res = xop - yop + (GET_C >> 3) - 1;
         CALC_NZVC_SUB(xop, yop, res);
         break;
      case 0x07: /* X - Y */
         xop = alu_x(a, xop);
         res = xop - yop;
         CALC_NZVC_SUB(xop, yop, res);
         break;
      case 0x08: /* Y - 1 */
         res = yop - 1;
         CALC_NZ(res);
         if (yop == 0x8000) a->astat |= VFLAG;
         else if (yop == 0x0000) a->astat |= CFLAG;
         break;
      case 0x09: /* Y - X */
         xop = alu_x(a, xop);
         res = yop - xop;
         CALC_NZVC_SUB(yop, xop, res);
         break;
      case 0x0a: /* Y - X + C - 1 */
         xop = alu_x(a, xop);
         res = yop - xop + (GET_C >> 3) - 1;
         CALC_NZVC_SUB(yop, xop, res);
         break;
      case 0x0b: /* NOT X */
         res = alu_x(a, xop) ^ 0xffff;
         CALC_NZ(res);
         break;
      case 0x0c: /* X AND Y */
         xop = alu_x(a, xop);
         res = xop & yop;
         CALC_NZ(res);
         break;
      case 0x0d: /* X OR Y */
         xop = alu_x(a, xop);
         res = xop | yop;
         CALC_NZ(res);
         break;
      case 0x0e: /* X XOR Y */
         xop = alu_x(a, xop);
         res = xop ^ yop;
         CALC_NZ(res);
         break;
      default:   /* 0x0f: ABS X */
         xop = alu_x(a, xop);
         res = (xop & 0x8000) ? -xop : xop;
         CLR_FLAGS;
         a->astat &= ~SFLAG;
         if (xop == 0) a->astat |= ZFLAG;
         if (xop == 0x8000) a->astat |= NFLAG | VFLAG;
         if (xop & 0x8000) a->astat |= SFLAG;
         break;
   }
   return res;
}

static INLINE s32 alu_yreg(Adsp2181 *a, u32 op)
{
   return alu_y(a, (int)BITS(op, 11, 2));
}

static INLINE s32 alu_yconst(u32 op)
{
   return adsp_constants[BITS(op, 5, 3) | (BITS(op, 11, 2) << 3)];
}

static INLINE void alu_store_ar(Adsp2181 *a, s32 res)
{
   if ((a->mstat & MSTAT_SATURATE) && GET_V)
      res = GET_C ? -32768 : 32767;
   a->core.ar.u = (u16)res;
}

static void alu_op_ar(Adsp2181 *a, u32 op)       { alu_store_ar(a, alu_compute(a, op, alu_yreg(a, op))); }
static void alu_op_ar_const(Adsp2181 *a, u32 op) { alu_store_ar(a, alu_compute(a, op, alu_yconst(op))); }
static void alu_op_af(Adsp2181 *a, u32 op)       { a->core.af.u = (u16)alu_compute(a, op, alu_yreg(a, op)); }
static void alu_op_af_const(Adsp2181 *a, u32 op) { a->core.af.u = (u16)alu_compute(a, op, alu_yconst(op)); }
static void alu_op_none(Adsp2181 *a, u32 op)     { (void)alu_compute(a, op, alu_yreg(a, op)); }

/*---------------------------------------------------------------------------
   MAC. mac_op_mr/mf and their _xop forms (Y operand = X operand) share
   one computation; `square` selects the _xop form.
---------------------------------------------------------------------------*/

/* (xop * yop) << shift with MAME's 32-bit wrap-around: only the low 32
   bits of the product are kept, then sign-extended */
static INLINE s32 mac_mul(s32 xop, s32 yop, int shift)
{
   return (s32)(((u32)xop * (u32)yop) << shift);
}

/* MAME mac_op_mr / mac_op_mf and their _xop forms (square != 0: the X
   operand is multiplied by itself with the X signedness on both sides,
   so SU -> SS and US -> UU). Returns 0 for the no-op form. */
static INLINE int mac_compute(Adsp2181 *a, u32 op, int square, s64 *out)
{
   const u32 f = BITS(op, 13, 4);
   const int shift = (a->mstat & MSTAT_INTEGER) ? 0 : 1;
   u16 xu, yu;
   s32 x, y, temp;
   s64 res;

   if (f == 0)
      return 0;

   xu = mac_x(a, BITS(op, 8, 3));
   yu = square ? xu : mac_y(a, BITS(op, 11, 2));

   if (f <= 3)
   {
      /* RND forms: signed x signed */
      x = (s16)xu;
      y = (s16)yu;
      temp = mac_mul(x, y, shift);
      if (f == 1) res = (s64)temp;
      else if (f == 2) res = (s64)a->core.mr.mr + (s64)temp;
      else res = (s64)a->core.mr.mr - (s64)temp;
      temp &= 0xffff;
      res += 0x8000;
      if (temp == 0x8000)
         res &= ~(s64)0x10000;
   }
   else
   {
      switch (f & 3)
      {
         case 0:  x = (s16)xu; y = (s16)yu; break;                    /* SS */
         case 1:  x = (s16)xu; y = square ? x : (s32)yu; break;       /* SU */
         case 2:  x = (s32)xu; y = square ? x : (s32)(s16)yu; break;  /* US */
         default: x = (s32)xu; y = (s32)yu; break;                    /* UU */
      }
      temp = mac_mul(x, y, shift);
      if (f < 8) res = (s64)temp;
      else if (f < 12) res = (s64)a->core.mr.mr + (s64)temp;
      else res = (s64)a->core.mr.mr - (s64)temp;
   }
   *out = res;
   return 1;
}

static void mac_store_mr(Adsp2181 *a, s64 res)
{
   u32 temp = (u32)(((u64)res >> 31) & 0x1ff);
   a->astat &= ~MVFLAG;
   if (temp != 0x000 && temp != 0x1ff) a->astat |= MVFLAG;
   a->core.mr.mr = (u64)res;
}

static void mac_op_mr(Adsp2181 *a, u32 op)     { s64 r; if (mac_compute(a, op, 0, &r)) mac_store_mr(a, r); }
static void mac_op_mr_xop(Adsp2181 *a, u32 op) { s64 r; if (mac_compute(a, op, 1, &r)) mac_store_mr(a, r); }
static void mac_op_mf(Adsp2181 *a, u32 op)     { s64 r; if (mac_compute(a, op, 0, &r)) a->core.mf.u = (u16)((u32)r >> 16); }
static void mac_op_mf_xop(Adsp2181 *a, u32 op) { s64 r; if (mac_compute(a, op, 1, &r)) a->core.mf.u = (u16)((u32)r >> 16); }

/*---------------------------------------------------------------------------
   Shifter
---------------------------------------------------------------------------*/

static void shift_common(Adsp2181 *a, u32 op, s8 sc, int allow_exp)
{
   AdspCore *c = &a->core;
   int xi = (int)BITS(op, 8, 3);
   u16 xu = shift_x(a, xi);
   s32 xs = (s16)xu;
   u32 res = 0;
   int do_or = (int)BITS(op, 11, 1);
   int sel = (int)BITS(op, 11, 4);

   switch (sel)
   {
      case 0x00: case 0x01: /* LSHIFT (HI) */
      {
         u32 x = (u32)xu << 16;
         if (sc > 0) res = (sc < 32) ? (x << sc) : 0;
         else res = (sc > -32) ? (x >> -sc) : 0;
         break;
      }
      case 0x02: case 0x03: /* LSHIFT (LO) */
      {
         u32 x = xu;
         if (sc > 0) res = (sc < 32) ? (x << sc) : 0;
         else res = (sc > -32) ? (x >> -sc) : 0;
         break;
      }
      case 0x04: case 0x05: /* ASHIFT (HI) */
      {
         s32 x = (s32)((u32)xs << 16);
         if (sc > 0) res = (sc < 32) ? ((u32)x << sc) : 0;
         else res = (u32)((sc > -32) ? (x >> -sc) : (x >> 31));
         break;
      }
      case 0x06: case 0x07: /* ASHIFT (LO) */
      {
         s32 x = xs;
         if (sc > 0) res = (sc < 32) ? ((u32)x << sc) : 0;
         else res = (u32)((sc > -32) ? (x >> -sc) : (x >> 31));
         break;
      }
      case 0x08: case 0x09: /* NORM (HI) */
      {
         s32 x = (s32)((u32)xs << 16);
         if (sc > 0)
         {
            s32 t = (s32)(((u32)x >> 1) | ((a->astat & CFLAG) << 28));
            /* MAME shifts by (sc - 1) unguarded; x86 and ARM shifters use
               the count modulo 32, reproduced here for SE > 32 */
            res = (u32)(t >> ((sc - 1) & 31));
         }
         else res = (sc > -32) ? ((u32)x << -sc) : 0;
         break;
      }
      case 0x0a: case 0x0b: /* NORM (LO) */
      {
         u32 x = xu;
         if (sc > 0) res = (sc < 32) ? (x >> sc) : 0;
         else res = (sc > -32) ? (x << -sc) : 0;
         break;
      }
      default:
         if (!allow_exp)
            return;   /* immediate shifts have no EXP/EXPADJ forms */
         switch (sel)
         {
            case 0x0c: /* EXP (HI) */
            {
               s32 r;
               if (xs < 0) { a->astat |= SSFLAG; r = clz32(~(u32)xs) - 16 - 1; }
               else        { a->astat &= ~SSFLAG; r = clz32((u32)xs) - 16 - 1; }
               c->se.s = (s16)-r;
               break;
            }
            case 0x0d: /* EXP (HIX) */
               if (GET_V)
               {
                  c->se.s = 1;
                  if (xs < 0) a->astat &= ~SSFLAG;
                  else a->astat |= SSFLAG;
               }
               else
               {
                  s32 r;
                  if (xs < 0) { a->astat |= SSFLAG; r = clz32(~(u32)xs) - 16 - 1; }
                  else        { a->astat &= ~SSFLAG; r = clz32((u32)xs) - 16 - 1; }
                  c->se.s = (s16)-r;
               }
               break;
            case 0x0e: /* EXP (LO) */
               if (c->se.s == -15)
               {
                  s32 r = clz32((u32)((GET_SS ? ~xs : xs) & 0xffff)) - 1;
                  c->se.s = (s16)-r;
               }
               break;
            default:   /* 0x0f: EXPADJ */
            {
               /* MAME keeps `res` in a uint32_t, so this compare is
                  unsigned; it only differs from a signed one when SB > 0 */
               u32 r = (u32)(clz32((u32)((xs < 0) ? ~xs : xs)) - 16 - 1);
               if (r < (u32)(-(s32)c->sb.s))
                  c->sb.s = (s16)(0u - r);
               break;
            }
         }
         return;
   }

   if (do_or) c->sr.sr |= res;
   else c->sr.sr = res;
}

static void shift_op(Adsp2181 *a, u32 op)
{
   shift_common(a, op, (s8)a->core.se.s, 1);
}

static void shift_op_imm(Adsp2181 *a, u32 op)
{
   shift_common(a, op, (s8)op, 0);
}

/*---------------------------------------------------------------------------
   Interrupts (ADSP-2181 priorities, MAME adsp2181_device::check_irqs)
---------------------------------------------------------------------------*/

static int generate_irq(Adsp2181 *a, int which, int indx)
{
   if (!(a->imask & (0x200 >> indx)))
      return 0;

   a->irq_latch[which] = 0;

   pc_stack_push(a);
   stat_stack_push(a);

   a->pc = 0x04 + indx * 4;
   a->idle = 0;

   if (a->icntl & 0x10) a->imask &= ~(0x3ff >> indx);
   else a->imask &= ~0x3ff;

   return 1;
}

static void check_irqs(Adsp2181 *a)
{
   u8 check;

   /* DIS INTS: nothing is serviced, requests stay latched until ENA INTS */
   if (!a->ints_enabled)
      return;

   check = (a->icntl & 4) ? a->irq_latch[ADSP2181_IRQ2] : a->irq_state[ADSP2181_IRQ2];
   if (check && generate_irq(a, ADSP2181_IRQ2, 0)) return;

   check = a->irq_state[ADSP2181_IRQL1];
   if (check && generate_irq(a, ADSP2181_IRQL1, 1)) return;

   check = a->irq_state[ADSP2181_IRQL0];
   if (check && generate_irq(a, ADSP2181_IRQL0, 2)) return;

   check = a->irq_latch[ADSP2181_SPORT0_TX];
   if (check && generate_irq(a, ADSP2181_SPORT0_TX, 3)) return;

   check = a->irq_latch[ADSP2181_SPORT0_RX];
   if (check && generate_irq(a, ADSP2181_SPORT0_RX, 4)) return;

   check = a->irq_latch[ADSP2181_IRQE];
   if (check && generate_irq(a, ADSP2181_IRQE, 5)) return;

   check = a->irq_latch[ADSP2181_BDMA];
   if (check && generate_irq(a, ADSP2181_BDMA, 6)) return;

   check = (a->icntl & 2) ? a->irq_latch[ADSP2181_IRQ1] : a->irq_state[ADSP2181_IRQ1];
   if (check && generate_irq(a, ADSP2181_IRQ1, 7)) return;

   check = (a->icntl & 1) ? a->irq_latch[ADSP2181_IRQ0] : a->irq_state[ADSP2181_IRQ0];
   if (check && generate_irq(a, ADSP2181_IRQ0, 8)) return;

   check = a->irq_latch[ADSP2181_TIMER];
   if (check && generate_irq(a, ADSP2181_TIMER, 9)) return;
}

/*---------------------------------------------------------------------------
   Public interface
---------------------------------------------------------------------------*/

void Adsp2181Init(Adsp2181 *a, u32 *pram, const Adsp2181Callbacks *cb)
{
   if (!tables_ready)
      create_tables();
   memset(a, 0, sizeof(*a));
   a->pram = pram;
   a->cb = *cb;
   build_operand_maps(a);
}

void Adsp2181SetDataPage(Adsp2181 *a, int page, u16 *ptr)
{
   if (page >= 0 && page < 16)
      a->dpage[page] = ptr;
}

void Adsp2181Reset(Adsp2181 *a)
{
   int k;

   /* ensure that zero is zero */
   a->core.zero.u = a->alt.zero.u = 0;

   /* Computation registers, I/M/L and stack contents keep their values
      across a reset, like MAME's adsp21xx_device::device_reset().
      Recompute the DAG bases from the current I/L values. */
   for (k = 0; k < 8; k++)
   {
      update_l(a, k);
      update_i(a, k);
   }

   a->pmovlay = a->dmovlay = 0;
   update_dmovlay(a);

   a->pc = 0;
   a->ppc = a->pc;
   a->loop = 0xffff;
   a->loop_condition = 0;

   a->astat_clear = ~(u32)(CFLAG | VFLAG | NFLAG | ZFLAG);
   a->mstat = 0;
   a->sstat = 0x55;
   a->idle = 0;
   update_mstat(a);

   a->pc_sp = 0;
   a->cntr_sp = 0;
   a->stat_sp = 0;
   a->loop_sp = 0;

   a->flagout = 0;
   a->flagin = 0;
   a->fl0 = 0;
   a->fl1 = 0;
   a->fl2 = 0;

   a->imask = 0;
   a->ints_enabled = 1;   /* interrupts are globally enabled after reset */
   for (k = 0; k < ADSP2181_NUM_IRQ; k++)
      a->irq_state[k] = a->irq_latch[k] = ADSP_LINE_CLEAR;
}

void Adsp2181SetIrqLine(Adsp2181 *a, int line, int state)
{
   if (line < 0 || line >= ADSP2181_NUM_IRQ)
      return;
   if (state != ADSP_LINE_CLEAR && a->irq_state[line] == ADSP_LINE_CLEAR)
      a->irq_latch[line] = 1;
   a->irq_state[line] = (u8)state;
}

u32 Adsp2181GetI(const Adsp2181 *a, int idx) { return a->i[idx & 7]; }
s32 Adsp2181GetM(const Adsp2181 *a, int idx) { return a->m[idx & 7]; }
u32 Adsp2181GetL(const Adsp2181 *a, int idx) { return a->l[idx & 7]; }

void Adsp2181SetI(Adsp2181 *a, int idx, u32 val)
{
   idx &= 7;
   a->i[idx] = val & 0x3fff;
   update_i(a, idx);
}

/* Group 0x50-0x7f: ALU/MAC with one memory transfer.
   bits 17-18: 0 = MAC->MR, 1 = ALU->AR, 2 = MAC->MF, 3 = ALU->AF */
static INLINE void do_compute(Adsp2181 *a, u32 op, u32 kind)
{
   switch (kind)
   {
      case 0: mac_op_mr(a, op); break;
      case 1: alu_op_ar(a, op); break;
      case 2: mac_op_mf(a, op); break;
      default: alu_op_af(a, op); break;
   }
}

/* Interrupts are sampled at the start of each slice, plus whenever IMASK,
   ICNTL, IFC are written or the status stack is popped (MAME behaviour).
   IDLE only ends the current slice, as in MAME: on the chip IDLE waits for
   an interrupt, but the RAX board leaves the SPORT0 and timer interrupts
   unemulated (MAME too), so parking the core would hang a program that
   idles on one of them. */
void Adsp2181Execute(Adsp2181 *a, int cycles)
{
   a->icount = cycles;

   check_irqs(a);

   while (a->icount > 0)
   {
      u32 op, temp;

      a->ppc = a->pc;
      op = program_read(a, a->pc);

      if (a->pc != a->loop)
         a->pc = (a->pc + 1) & 0x3fff;
      else
      {
         if (CONDITION(a->loop_condition))
            a->pc = pc_stack_top(a);
         else
         {
            loop_stack_pop(a);
            pc_stack_pop_val(a);
            a->pc = (a->pc + 1) & 0x3fff;
         }
      }

      switch (BITS(op, 16, 8))
      {
         case 0x00: /* NOP */
            break;

         case 0x01: /* IO read / write (ADSP-218x) */
            if (!BIT1(op, 15))
               write_reg0(a, (int)BITS(op, 0, 4), io_read(a, BITS(op, 4, 11)));
            else
               io_write(a, BITS(op, 4, 11), (u16)read_reg0(a, (int)BITS(op, 0, 4)));
            break;

         case 0x02: /* modify flag out / idle */
            if (BIT1(op, 15))
            {
               a->idle = 1;
               a->icount = 0;
            }
            else if (CONDITION(BITS(op, 0, 4)))
            {
               if (BIT1(op, 5)) a->flagout = 0;
               if (BIT1(op, 4)) a->flagout ^= 1;
               if (BIT1(op, 7)) a->fl0 = 0;
               if (BIT1(op, 6)) a->fl0 ^= 1;
               if (BIT1(op, 9)) a->fl1 = 0;
               if (BIT1(op, 8)) a->fl1 ^= 1;
               if (BIT1(op, 11)) a->fl2 = 0;
               if (BIT1(op, 10)) a->fl2 ^= 1;
            }
            break;

         case 0x03: /* call or jump on flag in */
            if (BIT1(op, 1) ? a->flagin : !a->flagin)
            {
               if (BIT1(op, 0))
                  pc_stack_push(a);
               a->pc = BITS(op, 4, 12) | (BITS(op, 2, 2) << 12);
            }
            break;

         case 0x04: /* stack control, ENA/DIS INTS */
            if (BIT1(op, 4)) pc_stack_pop_val(a);
            if (BIT1(op, 3)) loop_stack_pop(a);
            if (BIT1(op, 2)) cntr_stack_pop(a);
            if (BIT1(op, 1))
            {
               if (BIT1(op, 0)) stat_stack_pop(a);
               else stat_stack_push(a);
            }
            /* ADSP-218x global interrupt control, not decoded by MAME:
               0x040060 = ENA INTS, 0x040040 = DIS INTS (bit 6 = interrupt
               control, bit 5 = enable). The Batman Forever RAX program runs
               with interrupts globally disabled and only opens them in
               "ENA INTS; NOP; DIS INTS" windows. Without this, a BDMA
               interrupt could land between "AR = IMASK" and "IMASK = AR";
               the handler clobbers AR, IMASK was then written as 0 and
               the DSP stopped taking host commands for good. A request
               latched while disabled is serviced right after ENA INTS. */
            if (BIT1(op, 6))
            {
               a->ints_enabled = (u8)BIT1(op, 5);
               if (a->ints_enabled)
                  check_irqs(a);
            }
            break;

         case 0x05: /* saturate MR */
            if (GET_MV)
            {
               /* MAME writes only the MR0/MR1/MR2 fields */
               if (MR2_U(&a->core) & 0x80)
               {
                  set_mr2(&a->core, 0xffff);
                  set_mr1(&a->core, 0x8000);
                  set_mr0(&a->core, 0x0000);
               }
               else
               {
                  set_mr2(&a->core, 0x0000);
                  set_mr1(&a->core, 0x7fff);
                  set_mr0(&a->core, 0xffff);
               }
            }
            break;

         case 0x06: /* DIVS */
         {
            u32 xop = alu_x(a, (int)BITS(op, 8, 3));
            u32 yop = alu_y(a, (int)BITS(op, 11, 2));
            temp = xop ^ yop;
            a->astat = (a->astat & ~QFLAG) | ((temp >> 10) & QFLAG);
            a->core.af.u = (u16)((yop << 1) | (a->core.ay0.u >> 15));
            a->core.ay0.u = (u16)((a->core.ay0.u << 1) | (temp >> 15));
            break;
         }

         case 0x07: /* DIVQ */
         {
            s32 xop = alu_x(a, (int)BITS(op, 8, 3));
            s32 res;
            if (GET_Q)
               res = a->core.af.u + xop;
            else
               res = a->core.af.u - xop;
            temp = (u32)(res ^ xop);
            a->astat = (a->astat & ~QFLAG) | ((temp >> 10) & QFLAG);
            a->core.af.u = (u16)(((u32)res << 1) | (a->core.ay0.u >> 15));
            a->core.ay0.u = (u16)((a->core.ay0.u << 1) | ((~temp >> 15) & 0x0001));
            break;
         }

         case 0x08: /* reserved */
            break;

         case 0x09: /* modify address register */
            temp = BITS(op, 2, 3);
            modify_address(a, temp, (temp & 4) | (op & 3));
            break;

         case 0x0a: /* conditional return / RTI */
            if (CONDITION(BITS(op, 0, 4)))
            {
               pc_stack_pop(a);
               if (BIT1(op, 4))
                  stat_stack_pop(a);
            }
            break;

         case 0x0b: /* conditional jump (indirect) */
            if (CONDITION(BITS(op, 0, 4)))
            {
               if (BIT1(op, 4))
                  pc_stack_push(a);
               a->pc = a->i[4 + BITS(op, 6, 2)] & 0x3fff;
            }
            break;

         case 0x0c: /* mode control */
            if (BIT1(op, 3))  a->mstat = BIT1(op, 2)  ? (a->mstat | MSTAT_GOMODE)   : (a->mstat & ~MSTAT_GOMODE);
            if (BIT1(op, 13)) a->mstat = BIT1(op, 12) ? (a->mstat | MSTAT_INTEGER)  : (a->mstat & ~MSTAT_INTEGER);
            if (BIT1(op, 15)) a->mstat = BIT1(op, 14) ? (a->mstat | MSTAT_TIMER)    : (a->mstat & ~MSTAT_TIMER);
            if (BIT1(op, 5))  a->mstat = BIT1(op, 4)  ? (a->mstat | MSTAT_BANK)     : (a->mstat & ~MSTAT_BANK);
            if (BIT1(op, 7))  a->mstat = BIT1(op, 6)  ? (a->mstat | MSTAT_REVERSE)  : (a->mstat & ~MSTAT_REVERSE);
            if (BIT1(op, 9))  a->mstat = BIT1(op, 8)  ? (a->mstat | MSTAT_STICKYV)  : (a->mstat & ~MSTAT_STICKYV);
            if (BIT1(op, 11)) a->mstat = BIT1(op, 10) ? (a->mstat | MSTAT_SATURATE) : (a->mstat & ~MSTAT_SATURATE);
            update_mstat(a);
            break;

         case 0x0d: /* internal data move */
         {
            int dst = (int)BITS(op, 4, 4);
            int src = (int)BITS(op, 0, 4);
            s32 v;
            switch (BITS(op, 8, 2))
            {
               case 0: v = read_reg0(a, src); break;
               case 1: v = read_reg1(a, src); break;
               case 2: v = read_reg2(a, src); break;
               default: v = read_reg3(a, src); break;
            }
            switch (BITS(op, 10, 2))
            {
               case 0: write_reg0(a, dst, v); break;
               case 1: write_reg1(a, dst, v); break;
               case 2: write_reg2(a, dst, v); break;
               default: write_reg3(a, dst, v); break;
            }
            break;
         }

         case 0x0e: /* conditional shift */
            if (CONDITION(BITS(op, 0, 4))) shift_op(a, op);
            break;

         case 0x0f: /* shift immediate */
            shift_op_imm(a, op);
            break;

         case 0x10: /* shift with internal data register move */
            shift_op(a, op);
            temp = (u32)read_reg0(a, (int)BITS(op, 0, 4));
            write_reg0(a, (int)BITS(op, 4, 4), (s32)temp);
            break;

         case 0x11: /* shift with pgm memory read/write */
            if (BIT1(op, 15))
            {
               pgm_write_dag2(a, op, read_reg0(a, (int)BITS(op, 4, 4)));
               shift_op(a, op);
            }
            else
            {
               shift_op(a, op);
               write_reg0(a, (int)BITS(op, 4, 4), (s32)pgm_read_dag2(a, op));
            }
            break;

         case 0x12: /* shift with data memory read/write DAG1 */
            if (BIT1(op, 15))
            {
               data_write_dag1(a, op, read_reg0(a, (int)BITS(op, 4, 4)));
               shift_op(a, op);
            }
            else
            {
               shift_op(a, op);
               write_reg0(a, (int)BITS(op, 4, 4), (s32)data_read_dag1(a, op));
            }
            break;

         case 0x13: /* shift with data memory read/write DAG2 */
            if (BIT1(op, 15))
            {
               data_write_dag2(a, op, read_reg0(a, (int)BITS(op, 4, 4)));
               shift_op(a, op);
            }
            else
            {
               shift_op(a, op);
               write_reg0(a, (int)BITS(op, 4, 4), (s32)data_read_dag2(a, op));
            }
            break;

         case 0x14: case 0x15: case 0x16: case 0x17: /* do until */
            loop_stack_push(a, op & 0x3ffff);
            pc_stack_push(a);
            break;

         case 0x18: case 0x19: case 0x1a: case 0x1b: /* conditional jump (immediate) */
            if (CONDITION(BITS(op, 0, 4)))
            {
               a->pc = BITS(op, 4, 14);
               if (a->pc == a->ppc)
                  a->icount = 0;   /* busy loop */
            }
            break;

         case 0x1c: case 0x1d: case 0x1e: case 0x1f: /* conditional call (immediate) */
            if (CONDITION(BITS(op, 0, 4)))
            {
               pc_stack_push(a);
               a->pc = BITS(op, 4, 14);
            }
            break;

         case 0x20: case 0x21: /* conditional MAC to MR */
            if (CONDITION(BITS(op, 0, 4)))
            {
               if ((op & 0x0018f0) == 0x000010) mac_op_mr_xop(a, op);
               else mac_op_mr(a, op);
            }
            break;

         case 0x22: case 0x23: /* conditional ALU to AR */
            if (CONDITION(BITS(op, 0, 4)))
            {
               if (BIT1(op, 4)) alu_op_ar_const(a, op);
               else alu_op_ar(a, op);
            }
            break;

         case 0x24: case 0x25: /* conditional MAC to MF */
            if (CONDITION(BITS(op, 0, 4)))
            {
               if ((op & 0x0018f0) == 0x000010) mac_op_mf_xop(a, op);
               else mac_op_mf(a, op);
            }
            break;

         case 0x26: case 0x27: /* conditional ALU to AF */
            if (CONDITION(BITS(op, 0, 4)))
            {
               if (BIT1(op, 4)) alu_op_af_const(a, op);
               else alu_op_af(a, op);
            }
            break;

         case 0x28: case 0x29: /* MAC to MR with internal data register move */
            temp = (u32)read_reg0(a, (int)BITS(op, 0, 4));
            mac_op_mr(a, op);
            write_reg0(a, (int)BITS(op, 4, 4), (s32)temp);
            break;

         case 0x2a: case 0x2b: /* ALU to AR with internal data register move */
            if (BITS(op, 0, 8) == 0xaa)
               alu_op_none(a, op);
            else
            {
               temp = (u32)read_reg0(a, (int)BITS(op, 0, 4));
               alu_op_ar(a, op);
               write_reg0(a, (int)BITS(op, 4, 4), (s32)temp);
            }
            break;

         case 0x2c: case 0x2d: /* MAC to MF with internal data register move */
            temp = (u32)read_reg0(a, (int)BITS(op, 0, 4));
            mac_op_mf(a, op);
            write_reg0(a, (int)BITS(op, 4, 4), (s32)temp);
            break;

         case 0x2e: case 0x2f: /* ALU to AF with internal data register move */
            temp = (u32)read_reg0(a, (int)BITS(op, 0, 4));
            alu_op_af(a, op);
            write_reg0(a, (int)BITS(op, 4, 4), (s32)temp);
            break;

         case 0x30: case 0x31: case 0x32: case 0x33:
            write_reg0(a, (int)BITS(op, 0, 4), sext(op >> 4, 14));
            break;
         case 0x34: case 0x35: case 0x36: case 0x37:
            write_reg1(a, (int)BITS(op, 0, 4), sext(op >> 4, 14));
            break;
         case 0x38: case 0x39: case 0x3a: case 0x3b:
            write_reg2(a, (int)BITS(op, 0, 4), sext(op >> 4, 14));
            break;
         case 0x3c: case 0x3d: case 0x3e: case 0x3f:
            write_reg3(a, (int)BITS(op, 0, 4), sext(op >> 4, 14));
            break;

         case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
         case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
            /* load data register immediate */
            write_reg0(a, (int)BITS(op, 0, 4), (s32)BITS(op, 4, 16));
            break;

         case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
         case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
         case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
         {
            /* ALU/MAC with memory read: compute first, then load */
            u32 v;
            do_compute(a, op, BITS(op, 17, 2));
            switch (BITS(op, 20, 2))
            {
               case 1: v = pgm_read_dag2(a, op); break;
               case 2: v = data_read_dag1(a, op); break;
               default: v = data_read_dag2(a, op); break;
            }
            write_reg0(a, (int)BITS(op, 4, 4), (s32)v);
            break;
         }

         case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
         case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: case 0x6f:
         case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
         {
            /* ALU/MAC with memory write: store first, then compute */
            s32 v = read_reg0(a, (int)BITS(op, 4, 4));
            switch (BITS(op, 20, 2))
            {
               case 1: pgm_write_dag2(a, op, v); break;
               case 2: data_write_dag1(a, op, v); break;
               default: data_write_dag2(a, op, v); break;
            }
            do_compute(a, op, BITS(op, 17, 2));
            break;
         }

         case 0x80: case 0x81: case 0x82: case 0x83:
            write_reg0(a, (int)BITS(op, 0, 4), data_read(a, BITS(op, 4, 14)));
            break;
         case 0x84: case 0x85: case 0x86: case 0x87:
            write_reg1(a, (int)BITS(op, 0, 4), data_read(a, BITS(op, 4, 14)));
            break;
         case 0x88: case 0x89: case 0x8a: case 0x8b:
            write_reg2(a, (int)BITS(op, 0, 4), data_read(a, BITS(op, 4, 14)));
            break;
         case 0x8c: case 0x8d: case 0x8e: case 0x8f:
            write_reg3(a, (int)BITS(op, 0, 4), data_read(a, BITS(op, 4, 14)));
            break;
         case 0x90: case 0x91: case 0x92: case 0x93:
            data_write(a, BITS(op, 4, 14), (u16)read_reg0(a, (int)BITS(op, 0, 4)));
            break;
         case 0x94: case 0x95: case 0x96: case 0x97:
            data_write(a, BITS(op, 4, 14), (u16)read_reg1(a, (int)BITS(op, 0, 4)));
            break;
         case 0x98: case 0x99: case 0x9a: case 0x9b:
            data_write(a, BITS(op, 4, 14), (u16)read_reg2(a, (int)BITS(op, 0, 4)));
            break;
         case 0x9c: case 0x9d: case 0x9e: case 0x9f:
            data_write(a, BITS(op, 4, 14), (u16)read_reg3(a, (int)BITS(op, 0, 4)));
            break;

         case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7:
         case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
            data_write_dag1(a, op, (s32)BITS(op, 4, 16));
            break;
         case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
         case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
            data_write_dag2(a, op, (s32)BITS(op, 4, 16));
            break;

         default:
         {
            /* 0xc0-0xff: ALU/MAC with dual memory read
               bit 17 = ALU->AR (1) / MAC->MR (0)
               bits 18-19 = DM destination: AX0 AX1 MX0 MX1
               bits 20-21 = PM destination: AY0 AY1 MY0 MY1 */
            u32 dv, pv;
            if (BIT1(op, 17)) alu_op_ar(a, op);
            else mac_op_mr(a, op);
            dv = data_read_dag1(a, op);
            switch (BITS(op, 18, 2))
            {
               case 0: a->core.ax0.u = (u16)dv; break;
               case 1: a->core.ax1.u = (u16)dv; break;
               case 2: a->core.mx0.u = (u16)dv; break;
               default: a->core.mx1.u = (u16)dv; break;
            }
            pv = pgm_read_dag2(a, op >> 4);
            switch (BITS(op, 20, 2))
            {
               case 0: a->core.ay0.u = (u16)pv; break;
               case 1: a->core.ay1.u = (u16)pv; break;
               case 2: a->core.my0.u = (u16)pv; break;
               default: a->core.my1.u = (u16)pv; break;
            }
            break;
         }
      }

      a->icount--;
   }
}

/*---------------------------------------------------------------------------
   Save states (RAX block of the CART chunk, see rax.c)

   Every register is written with its own size, so the layout does not
   depend on struct padding. Not written: the board callbacks, pram (saved
   by the board), the operand maps (they point into `core`, rebuilt by
   Init) and the data pages (set again by the board). lmask/base are
   recomputed from I and L.
---------------------------------------------------------------------------*/

#define ADSP_STATE_VERSION 1

static void adsp_w16(u16 v, void **stream) { MemStateWrite(&v, 2, 1, stream); }
static void adsp_w32(u32 v, void **stream) { MemStateWrite(&v, 4, 1, stream); }
static u16 adsp_r16(const void *stream) { u16 v; MemStateRead(&v, 2, 1, stream); return v; }
static u32 adsp_r32(const void *stream) { u32 v; MemStateRead(&v, 4, 1, stream); return v; }

static void adsp_core_save(const AdspCore *c, void **stream)
{
   adsp_w16(c->ax0.u, stream); adsp_w16(c->ax1.u, stream);
   adsp_w16(c->ay0.u, stream); adsp_w16(c->ay1.u, stream);
   adsp_w16(c->ar.u, stream);  adsp_w16(c->af.u, stream);
   adsp_w16(c->mx0.u, stream); adsp_w16(c->mx1.u, stream);
   adsp_w16(c->my0.u, stream); adsp_w16(c->my1.u, stream);
   adsp_w16(c->mf.u, stream);
   MemStateWrite((void *)&c->mr.mr, 8, 1, stream);
   adsp_w16(c->si.u, stream);  adsp_w16(c->se.u, stream);
   adsp_w16(c->sb.u, stream);
   adsp_w32(c->sr.sr, stream);
}

static void adsp_core_load(AdspCore *c, const void *stream)
{
   c->ax0.u = adsp_r16(stream); c->ax1.u = adsp_r16(stream);
   c->ay0.u = adsp_r16(stream); c->ay1.u = adsp_r16(stream);
   c->ar.u = adsp_r16(stream);  c->af.u = adsp_r16(stream);
   c->mx0.u = adsp_r16(stream); c->mx1.u = adsp_r16(stream);
   c->my0.u = adsp_r16(stream); c->my1.u = adsp_r16(stream);
   c->mf.u = adsp_r16(stream);
   MemStateRead(&c->mr.mr, 8, 1, stream);
   c->si.u = adsp_r16(stream);  c->se.u = adsp_r16(stream);
   c->sb.u = adsp_r16(stream);
   c->sr.sr = adsp_r32(stream);
   c->zero.u = 0;
}

#define ADSP_CORE_STATE_SIZE (11*2 + 8 + 3*2 + 4)
#define ADSP_STATE_SIZE (4 + 11*4 + 2*ADSP_CORE_STATE_SIZE + 8*4*3 + 1 + 2*4 + \
                         (4+4+16)*4 + 4*3*2 + 4*4 + 5 + 2 + 1 + 1 + 2 + \
                         2*ADSP2181_NUM_IRQ + 4)

int Adsp2181StateSize(void)
{
   return ADSP_STATE_SIZE;
}

void Adsp2181SaveState(const Adsp2181 *a, void **stream)
{
   int k;
   u8 b[5];

   adsp_w32(ADSP_STATE_VERSION, stream);
   adsp_w32(a->pc, stream); adsp_w32(a->ppc, stream); adsp_w32(a->loop, stream);
   adsp_w32(a->loop_condition, stream); adsp_w32(a->cntr, stream);
   adsp_w32(a->astat, stream); adsp_w32(a->sstat, stream); adsp_w32(a->mstat, stream);
   adsp_w32(a->mstat_prev, stream); adsp_w32(a->astat_clear, stream); adsp_w32(a->idle, stream);
   adsp_core_save(&a->core, stream);
   adsp_core_save(&a->alt, stream);
   for (k = 0; k < 8; k++) adsp_w32(a->i[k], stream);
   for (k = 0; k < 8; k++) adsp_w32((u32)a->m[k], stream);
   for (k = 0; k < 8; k++) adsp_w32(a->l[k], stream);
   MemStateWrite((void *)&a->px, 1, 1, stream);
   adsp_w32(a->pmovlay, stream); adsp_w32(a->dmovlay, stream);
   for (k = 0; k < 4; k++) adsp_w32(a->loop_stack[k], stream);
   for (k = 0; k < 4; k++) adsp_w32(a->cntr_stack[k], stream);
   for (k = 0; k < 16; k++) adsp_w32(a->pc_stack[k], stream);
   for (k = 0; k < 4; k++)
   {
      adsp_w16(a->stat_stack[k][0], stream);
      adsp_w16(a->stat_stack[k][1], stream);
      adsp_w16(a->stat_stack[k][2], stream);
   }
   adsp_w32((u32)a->pc_sp, stream); adsp_w32((u32)a->cntr_sp, stream);
   adsp_w32((u32)a->stat_sp, stream); adsp_w32((u32)a->loop_sp, stream);
   b[0] = a->flagout; b[1] = a->flagin; b[2] = a->fl0; b[3] = a->fl1; b[4] = a->fl2;
   MemStateWrite(b, 1, 5, stream);
   adsp_w16(a->imask, stream);
   MemStateWrite((void *)&a->icntl, 1, 1, stream);
   MemStateWrite((void *)&a->ints_enabled, 1, 1, stream);
   adsp_w16(a->ifc, stream);
   MemStateWrite((void *)a->irq_state, 1, ADSP2181_NUM_IRQ, stream);
   MemStateWrite((void *)a->irq_latch, 1, ADSP2181_NUM_IRQ, stream);
   adsp_w32((u32)a->icount, stream);
}

static s32 adsp_clamp_sp(s32 v, s32 depth)
{
   return (v < 0) ? 0 : (v > depth) ? depth : v;
}

/* `a` must have been through Adsp2181Init (callbacks and pram set). */
int Adsp2181LoadState(Adsp2181 *a, const void *stream, int size)
{
   int k;
   u8 b[5];

   if (size < ADSP_STATE_SIZE)
      return -1;
   if (adsp_r32(stream) != ADSP_STATE_VERSION)
      return -1;

   a->pc = adsp_r32(stream) & 0x3fff; a->ppc = adsp_r32(stream) & 0x3fff;
   a->loop = adsp_r32(stream); a->loop_condition = adsp_r32(stream) & 15;
   a->cntr = adsp_r32(stream);
   a->astat = adsp_r32(stream) & 0xff;   /* index of condition_table */
   a->sstat = adsp_r32(stream); a->mstat = adsp_r32(stream);
   a->mstat_prev = adsp_r32(stream); a->astat_clear = adsp_r32(stream);
   a->idle = adsp_r32(stream);
   /* core/alt are saved already swapped for the current MSTAT bank */
   adsp_core_load(&a->core, stream);
   adsp_core_load(&a->alt, stream);
   for (k = 0; k < 8; k++) a->i[k] = adsp_r32(stream) & 0x3fff;
   for (k = 0; k < 8; k++) a->m[k] = (s32)adsp_r32(stream);
   for (k = 0; k < 8; k++) a->l[k] = adsp_r32(stream) & 0x3fff;
   MemStateRead(&a->px, 1, 1, stream);
   a->pmovlay = adsp_r32(stream); a->dmovlay = adsp_r32(stream);
   for (k = 0; k < 4; k++) a->loop_stack[k] = adsp_r32(stream);
   for (k = 0; k < 4; k++) a->cntr_stack[k] = adsp_r32(stream);
   for (k = 0; k < 16; k++) a->pc_stack[k] = adsp_r32(stream) & 0x3fff;
   for (k = 0; k < 4; k++)
   {
      a->stat_stack[k][0] = adsp_r16(stream);
      a->stat_stack[k][1] = adsp_r16(stream);
      a->stat_stack[k][2] = adsp_r16(stream) & 0xff;
   }
   a->pc_sp = adsp_clamp_sp((s32)adsp_r32(stream), PC_STACK_DEPTH);
   a->cntr_sp = adsp_clamp_sp((s32)adsp_r32(stream), CNTR_STACK_DEPTH);
   a->stat_sp = adsp_clamp_sp((s32)adsp_r32(stream), STAT_STACK_DEPTH);
   a->loop_sp = adsp_clamp_sp((s32)adsp_r32(stream), LOOP_STACK_DEPTH);
   MemStateRead(b, 1, 5, stream);
   a->flagout = b[0]; a->flagin = b[1]; a->fl0 = b[2]; a->fl1 = b[3]; a->fl2 = b[4];
   a->imask = adsp_r16(stream);
   MemStateRead(&a->icntl, 1, 1, stream);
   MemStateRead(&a->ints_enabled, 1, 1, stream);
   a->ifc = adsp_r16(stream);
   MemStateRead(a->irq_state, 1, ADSP2181_NUM_IRQ, stream);
   MemStateRead(a->irq_latch, 1, ADSP2181_NUM_IRQ, stream);
   a->icount = (int)adsp_r32(stream);

   for (k = 0; k < 8; k++)
      update_l(a, k);   /* lmask and base */
   return 0;
}
