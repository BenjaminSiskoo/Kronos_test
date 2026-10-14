/*  Copyright 2026 Kronos_test contributors

    Acclaim RAX sound board (ADSP-2181 + banked sample ROM), as fitted to
    the Batman Forever ST-V cartridge. C port of MAME
    src/mame/shared/rax.cpp (license BSD-3-Clause, copyright-holders:
    Phil Bennett) and of the batmanfr glue in src/mame/sega/stv.cpp.

    Board summary (from MAME rax.cpp):
    - ADSP-2181 clocked at 16.67 MHz, 16K x 24 program RAM.
    - Data space 0000-1FFF: internal RAM when DMOVLAY = 0, else one of four
      external 8K banks selected by an I/O write (IO 0000).
      2000-3FDF internal RAM, 3FE0-3FFF on-chip control registers.
    - I/O 0001: byte-ROM bank (4 MB pages of the 16 MB "rax" region),
      used by the BDMA engine. I/O 0003: host data latch (read clears the
      IRQL0 request raised by a host write; write = reply latch).
    - Audio leaves through SPORT0 in autobuffer mode, interleaved L/R
      words, to two DACs. MAME mixes them straight to the speakers, next
      to the SCSP ("TODO: RAX output connected to SCSP?").

    Kronos-specific differences, all forced by the emulator structure:
    - The ADSP runs in the SCSP sound thread, one 44.1 kHz sample at a
      time (RaxRunSample), and its DAC output is resampled linearly to
      44.1 kHz. MAME instead streams to dmadac at the SPORT rate.
    - MAME makes the ADSP answer a host write within 5 us
      (perfect_quantum in data_w). The SH-2 runs on another thread here,
      up to a few lines ahead, so host writes are queued and handed to the
      ADSP latch one at a time, each time the previous one has been read.
      A value the ADSP leaves unread for 1 ms of DSP time is overwritten
      by the next one, as the single hardware latch would be.
    - BDMA from internal memory to byte memory (BDMA_CONTROL bit 2) is a
      fatalerror in MAME ("TODO: last stage in Batman Forever!?"). The
      target is ROM on this board, so nothing is written, but the counters
      advance and the BDMA interrupt is raised as for any other transfer.
    - u-law / A-law SPORT1 companding is a fatalerror in MAME; only
      logged here (SPORT1 carries no audio on this board).

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

#include <stdlib.h>
#include <string.h>
#include "rax.h"
#include "adsp2181.h"
#include "threads.h"
#include "yui.h"

#define RAXLOG(...) do { } while (0)

#define RAX_CLOCK        16670000   /* XTAL(16'670'000) */
#define RAX_OUT_RATE     44100      /* SCSP output rate */
#define RAX_SLICE_MAX    128        /* max ADSP cycles run without a sync point */
#define RAX_HOST_FIFO    256
#define RAX_LATCH_TIMEOUT (RAX_CLOCK / 1000)  /* 1 ms of DSP time */

/* On-chip control registers, 3FE0-3FFF (MAME rax.cpp) */
enum
{
   IDMA_CONTROL_REG = 0,   /* 3fe0 */
   BDMA_INT_ADDR_REG,      /* 3fe1 */
   BDMA_EXT_ADDR_REG,      /* 3fe2 */
   BDMA_CONTROL_REG,       /* 3fe3 */
   BDMA_WORD_COUNT_REG,    /* 3fe4 */
   PROG_FLAG_DATA_REG,     /* 3fe5 */
   PROG_FLAG_CONTROL_REG,  /* 3fe6 */

   S1_AUTOBUF_REG = 15,    /* 3fef */
   S1_RFSDIV_REG,          /* 3ff0 */
   S1_SCLKDIV_REG,         /* 3ff1 */
   S1_CONTROL_REG,         /* 3ff2 */
   S0_AUTOBUF_REG,         /* 3ff3 */
   S0_RFSDIV_REG,          /* 3ff4 */
   S0_SCLKDIV_REG,         /* 3ff5 */
   S0_CONTROL_REG,         /* 3ff6 */
   S0_MCTXLO_REG,          /* 3ff7 */
   S0_MCTXHI_REG,          /* 3ff8 */
   S0_MCRXLO_REG,          /* 3ff9 */
   S0_MCRXHI_REG,          /* 3ffa */
   TIMER_SCALE_REG,        /* 3ffb */
   TIMER_COUNT_REG,        /* 3ffc */
   TIMER_PERIOD_REG,       /* 3ffd */
   WAITSTATES_REG,         /* 3ffe */
   SYSCONTROL_REG          /* 3fff */
};

typedef struct
{
   Adsp2181 cpu;
   u32 pram[0x4000];
   u16 banked_ram[5][0x2000];   /* entry 0 = internal, 1-4 = external banks */
   u16 int_ram[0x1fe0];         /* 2000-3FDF */
   u16 control_regs[32];

   u32 data_bank;
   u32 rom_bank;
   u32 dmovlay_val;

   /* host interface */
   u16 data_in;
   u16 data_out;
   int latch_full;
   int latch_age;
   u32 snd_pf0;
   u16 fifo[RAX_HOST_FIFO];
   int fifo_head, fifo_tail;

   /* BDMA completion timer */
   int bdma_pending;
   int bdma_countdown;
   u32 bdma_param;

   /* SPORT0 autobuffer -> DACs */
   int dac_enabled[2];
   int ab_active;
   int ab_ireg;
   u16 ab_incs;
   u16 ab_size;
   u16 ab_base;
   int frame_period;            /* ADSP cycles per stereo frame */
   int frame_count;             /* cycles left until the next frame */
   s16 prev_l, prev_r, cur_l, cur_r;

   u32 cyc_frac;
} RaxState;

static RaxState *rax = NULL;
static u8 *rax_rom = NULL;
static volatile int rax_enabled = 0;
static volatile int rax_reset_pending = 0;
static YabMutex *rax_mtx = NULL;
static int rax_warned_bdma_write = 0;

static void rax_ensure_mutex(void)
{
   if (rax_mtx == NULL)
      rax_mtx = YabThreadCreateMutex();
}

static INLINE u8 rom_byte(u32 addr)
{
   return (rax_rom != NULL && addr < RAX_ROM_SIZE) ? rax_rom[addr] : 0;
}

/*---------------------------------------------------------------------------
   Data space
---------------------------------------------------------------------------*/

static u16 *data_bank_ptr(void)
{
   /* acclaim_rax_device::update_data_ram_bank() */
   if (rax->dmovlay_val == 0)
      return rax->banked_ram[0];
   return rax->banked_ram[1 + rax->data_bank];
}

/* Direct-mapped data pages for the ADSP core: 0000-1FFF follows the
   DMOVLAY/bank selection, 2000-3BFF is internal RAM, 3C00-3FFF (the end of
   internal RAM plus the control registers) goes through rax_data_read /
   rax_data_write. */
static void update_data_pages(void)
{
   u16 *bank = data_bank_ptr();
   int k;
   for (k = 0; k < 8; k++)
      Adsp2181SetDataPage(&rax->cpu, k, bank + k * 0x400);
   for (k = 8; k < 15; k++)
      Adsp2181SetDataPage(&rax->cpu, k, rax->int_ram + (k - 8) * 0x400);
   Adsp2181SetDataPage(&rax->cpu, 15, NULL);
}

static u16 adsp_control_r(u32 offset)
{
   if (offset == PROG_FLAG_DATA_REG)
      return (u16)rax->snd_pf0;
   return rax->control_regs[offset];
}

static void rax_data_write(void *user, u32 addr, u16 data);

static void recompute_sample_rate(int which)
{
   /* MAME: sample_period = clock^-1 * (SCLKDIV + 1) * 16, one stereo frame
      per sample period, both DACs enabled. */
   u16 sclkdiv = rax->control_regs[which ? S1_SCLKDIV_REG : S0_SCLKDIV_REG];
   rax->frame_period = 16 * ((int)sclkdiv + 1);
   rax->dac_enabled[0] = rax->dac_enabled[1] = 1;

   if (rax->ab_incs)
   {
      rax->ab_active = 1;
      rax->frame_count = rax->frame_period;
   }
}

static void bdma_start(u16 data)
{
   u32 page, dir, type, src_addr, count;

   rax->control_regs[BDMA_WORD_COUNT_REG] = data & 0x3fff;

   page = (rax->control_regs[BDMA_CONTROL_REG] >> 8) & 0xff;
   dir = (rax->control_regs[BDMA_CONTROL_REG] >> 2) & 1;
   type = rax->control_regs[BDMA_CONTROL_REG] & 3;
   src_addr = (page << 14) | rax->control_regs[BDMA_EXT_ADDR_REG];
   count = rax->control_regs[BDMA_WORD_COUNT_REG];

   {
      const u32 bank_base = rax->rom_bank * 0x400000;

      if (!dir)
      {
         if (type == 0)
         {
            while (count)
            {
               u32 a = bank_base + src_addr;
               u32 w = ((u32)rom_byte(a) << 16) | ((u32)rom_byte(a + 1) << 8) | rom_byte(a + 2);
               rax->pram[rax->control_regs[BDMA_INT_ADDR_REG] & 0x3fff] = w & 0xffffff;
               src_addr += 3;
               ++rax->control_regs[BDMA_INT_ADDR_REG];
               --count;
            }
         }
         else if (type == 1)
         {
            while (count)
            {
               u32 a = bank_base + src_addr;
               u16 w = (u16)(((u32)rom_byte(a) << 8) | rom_byte(a + 1));
               rax_data_write(NULL, rax->control_regs[BDMA_INT_ADDR_REG] & 0x3fff, w);
               src_addr += 2;
               ++rax->control_regs[BDMA_INT_ADDR_REG];
               --count;
            }
         }
         else
         {
            int shift = (type == 2) ? 8 : 0;
            while (count)
            {
               u16 w = (u16)((u32)rom_byte(bank_base + src_addr) << shift);
               rax_data_write(NULL, rax->control_regs[BDMA_INT_ADDR_REG] & 0x3fff, w);
               ++src_addr;
               ++rax->control_regs[BDMA_INT_ADDR_REG];
               --count;
            }
         }
      }
      else
      {
         /* internal -> byte memory: byte memory is ROM here, see header */
         u32 bytes = (type == 0) ? 3 : (type == 1) ? 2 : 1;
         if (!rax_warned_bdma_write)
         {
            rax_warned_bdma_write = 1;
            YuiMsg("RAX: BDMA to byte memory (page %x, type %d, ext %x, count %x) ignored\n",
                   page, type, rax->control_regs[BDMA_EXT_ADDR_REG], count);
         }
         src_addr += bytes * count;
         rax->control_regs[BDMA_INT_ADDR_REG] = (u16)(rax->control_regs[BDMA_INT_ADDR_REG] + count);
      }
   }

   /* completion after one ADSP clock per word (MAME m_dma_timer) */
   rax->bdma_pending = 1;
   rax->bdma_countdown = (int)(data & 0x3fff);
   rax->bdma_param = src_addr;
}

static void bdma_done(void)
{
   /* acclaim_rax_device::dma_timer_callback */
   u32 param = rax->bdma_param;
   rax->bdma_pending = 0;

   rax->control_regs[BDMA_WORD_COUNT_REG] = 0;
   rax->control_regs[BDMA_EXT_ADDR_REG] = param & 0x3fff;
   rax->control_regs[BDMA_CONTROL_REG] &= ~0xff00;
   rax->control_regs[BDMA_CONTROL_REG] |= ((param >> 14) & 0xff) << 8;

   if (rax->control_regs[BDMA_CONTROL_REG] & 0x0008)
      Adsp2181Reset(&rax->cpu);           /* BCR: reset/run after boot load */
   else
   {
      Adsp2181SetIrqLine(&rax->cpu, ADSP2181_BDMA, ADSP_LINE_ASSERT);
      Adsp2181SetIrqLine(&rax->cpu, ADSP2181_BDMA, ADSP_LINE_CLEAR);
   }
}

static void adsp_control_w(u32 offset, u16 data)
{
   rax->control_regs[offset] = data;

   switch (offset)
   {
      case BDMA_INT_ADDR_REG:
         rax->control_regs[BDMA_INT_ADDR_REG] = data & 0x3fff;
         break;
      case BDMA_EXT_ADDR_REG:
         rax->control_regs[BDMA_EXT_ADDR_REG] = data & 0x3fff;
         break;
      case BDMA_CONTROL_REG:
         rax->control_regs[BDMA_CONTROL_REG] = data & 0xff0f;
         break;
      case BDMA_WORD_COUNT_REG:
         bdma_start(data);
         break;

      case S1_AUTOBUF_REG:
         /* autobuffer off: disable the DAC */
         if (!(data & 0x0002))
            rax->dac_enabled[1] = 0;
         break;

      case S0_AUTOBUF_REG:
         /* autobuffer off: stop the transfer and disable the DAC */
         if (!(data & 0x0002))
         {
            rax->dac_enabled[0] = 0;
            rax->ab_active = 0;
         }
         break;

      case S1_CONTROL_REG:
         if (((data >> 4) & 3) >= 2)
            RAXLOG("RAX: SPORT1 companding (%s) not emulated\n", ((data >> 4) & 3) == 2 ? "u-law" : "A-law");
         break;

      default:
         RAXLOG("RAX: control reg %x = %x\n", 0x3fe0 + offset, data);
         break;
   }
}

static u16 rax_data_read(void *user, u32 addr)
{
   (void)user;
   if (addr < 0x2000)
      return data_bank_ptr()[addr];
   if (addr < 0x3fe0)
      return rax->int_ram[addr - 0x2000];
   return adsp_control_r(addr - 0x3fe0);
}

static void rax_data_write(void *user, u32 addr, u16 data)
{
   (void)user;
   addr &= 0x3fff;
   if (addr < 0x2000)
      data_bank_ptr()[addr] = data;
   else if (addr < 0x3fe0)
      rax->int_ram[addr - 0x2000] = data;
   else
      adsp_control_w(addr - 0x3fe0, data);
}

/*---------------------------------------------------------------------------
   I/O space
---------------------------------------------------------------------------*/

static u16 rax_io_read(void *user, u32 addr)
{
   (void)user;
   if (addr == 3)
   {
      /* host_r */
      Adsp2181SetIrqLine(&rax->cpu, ADSP2181_IRQL0, ADSP_LINE_CLEAR);
      rax->latch_full = 0;
      return rax->data_in;
   }
   return 0xffff;   /* unmap_value_high */
}

static void rax_io_write(void *user, u32 addr, u16 data)
{
   (void)user;
   switch (addr)
   {
      case 0:   /* ram_bank_w: the PCB has two unstuffed RAM locations */
         rax->data_bank = data & 3;
         update_data_pages();
         break;
      case 1:   /* rom_bank_w */
         rax->rom_bank = data;
         break;
      case 3:   /* host_w */
         rax->data_out = data;
         rax->snd_pf0 = 0;
         break;
      default:
         break;
   }
}

/*---------------------------------------------------------------------------
   CPU callbacks
---------------------------------------------------------------------------*/

static void rax_sport_tx(void *user, int which, u32 data)
{
   int autobuf_reg;
   (void)user;
   (void)data;

   if (which != 0)
      return;

   autobuf_reg = S0_AUTOBUF_REG;

   /* SPORT0 enabled (SYSCONTROL bit 12) and autobuffer on? */
   if (rax->control_regs[SYSCONTROL_REG] & 0x1000)
   {
      if (rax->control_regs[autobuf_reg] & 0x0002)
      {
         int ireg = (rax->control_regs[autobuf_reg] >> 9) & 7;
         int mreg = (rax->control_regs[autobuf_reg] >> 7) & 3;
         u16 source;

         mreg |= ireg & 0x04;   /* msb comes from ireg */

         source = (u16)Adsp2181GetI(&rax->cpu, ireg);
         rax->ab_ireg = ireg;
         rax->ab_incs = (u16)Adsp2181GetM(&rax->cpu, mreg);
         rax->ab_size = (u16)Adsp2181GetL(&rax->cpu, ireg);

         /* MAME: go back one word so the first sample is not lost, and
            keep that as the wrap base */
         source = (u16)(source - rax->ab_incs);
         Adsp2181SetI(&rax->cpu, ireg, source);
         rax->ab_base = source;

         recompute_sample_rate(which);
         return;
      }
      RAXLOG("RAX: SPORT0 transmit with autobuffer disabled\n");
   }

   /* something went wrong: stop playing */
   rax->dac_enabled[0] = rax->dac_enabled[1] = 0;
   rax->ab_active = 0;
}

static void rax_dmovlay(void *user, u32 data)
{
   (void)user;
   if (data > 1)
   {
      RAXLOG("RAX: DMOVLAY = %x\n", data);   /* fatalerror in MAME */
      return;
   }
   rax->dmovlay_val = data;
   update_data_pages();
}

/*---------------------------------------------------------------------------
   Audio
---------------------------------------------------------------------------*/

static void fetch_frame(void)
{
   /* acclaim_rax_device::adsp_irq(0), one stereo frame at a time:
      even words go to the left DAC, odd words to the right DAC. */
   u32 reg = Adsp2181GetI(&rax->cpu, rax->ab_ireg);
   u32 end = (u32)rax->ab_base + rax->ab_size;
   s16 w[2];
   int k;

   for (k = 0; k < 2; k++)
   {
      w[k] = (s16)rax_data_read(NULL, reg & 0x3fff);
      reg = (reg + rax->ab_incs) & 0xffff;
      if (reg >= end)
         reg = rax->ab_base;
   }
   Adsp2181SetI(&rax->cpu, rax->ab_ireg, reg);

   rax->prev_l = rax->cur_l;
   rax->prev_r = rax->cur_r;
   rax->cur_l = w[0];
   rax->cur_r = w[1];
}

/*---------------------------------------------------------------------------
   Reset
---------------------------------------------------------------------------*/

/* First use of the CPU (reset or save state load): callbacks and pram. */
static void rax_init_cpu(void)
{
   Adsp2181Callbacks cb;

   if (rax->cpu.pram != NULL)
      return;

   cb.user = NULL;
   cb.data_read = rax_data_read;
   cb.data_write = rax_data_write;
   cb.io_read = rax_io_read;
   cb.io_write = rax_io_write;
   cb.sport_tx = rax_sport_tx;
   cb.dmovlay = rax_dmovlay;
   Adsp2181Init(&rax->cpu, rax->pram, &cb);
}

static void rax_do_reset(void)
{
   int i;

   /* keep the CPU registers/RAM across resets once initialised */
   rax_init_cpu();

   /* acclaim_rax_device::device_reset: boot 32 program words (96 bytes)
      from the start of the ROM, as the 2181 BDMA boot does */
   for (i = 0; i < 32; i++)
   {
      u32 w = ((u32)rom_byte(i * 3 + 0) << 16) | ((u32)rom_byte(i * 3 + 1) << 8) | rom_byte(i * 3 + 2);
      rax->pram[i] = w;
   }

   rax->snd_pf0 = 1;
   rax->rom_bank = 0;
   rax->ab_size = 0;
   rax->ab_incs = 0;
   rax->ab_ireg = 0;
   rax->ab_active = 0;
   rax->dac_enabled[0] = rax->dac_enabled[1] = 0;
   rax->frame_period = 16;
   rax->frame_count = 0;
   rax->prev_l = rax->prev_r = rax->cur_l = rax->cur_r = 0;
   rax->bdma_pending = 0;
   rax->latch_full = 0;
   rax->latch_age = 0;

   memset(rax->control_regs, 0, sizeof(rax->control_regs));

   rax->dmovlay_val = 0;
   rax->data_bank = 0;
   update_data_pages();

   /* the CPU child device resets too */
   Adsp2181Reset(&rax->cpu);
}

/*---------------------------------------------------------------------------
   Public interface
---------------------------------------------------------------------------*/

void RaxLoadRom(u32 offset, const u8 *data, u32 length)
{
   rax_ensure_mutex();
   YabThreadLock(rax_mtx);
   if (rax_rom == NULL)
      rax_rom = (u8 *)calloc(1, RAX_ROM_SIZE);
   if (rax_rom != NULL && offset < RAX_ROM_SIZE)
   {
      if (length > RAX_ROM_SIZE - offset)
         length = RAX_ROM_SIZE - offset;
      memcpy(&rax_rom[offset], data, length);
   }
   YabThreadUnLock(rax_mtx);
}

void RaxEnable(void)
{
   rax_ensure_mutex();
   YabThreadLock(rax_mtx);
   if (rax == NULL)
      rax = (RaxState *)calloc(1, sizeof(RaxState));
   else
      memset(rax, 0, sizeof(RaxState));    /* power-on: RAM cleared */
   rax_warned_bdma_write = 0;
   if (rax != NULL && rax_rom != NULL)
   {
      rax_reset_pending = 1;
      rax_enabled = 1;
   }
   else
      YuiMsg("RAX: sound ROMs (350snda1.u52, snd0-3.u48-u51) not found in the romset, Batman Forever will be silent\n");
   YabThreadUnLock(rax_mtx);
}

void RaxShutdown(void)
{
   rax_ensure_mutex();
   YabThreadLock(rax_mtx);
   rax_enabled = 0;
   rax_reset_pending = 0;
   free(rax);
   rax = NULL;
   free(rax_rom);
   rax_rom = NULL;
   YabThreadUnLock(rax_mtx);
}

int RaxIsEnabled(void)
{
   return rax_enabled;
}

void RaxReset(void)
{
   if (!rax_enabled)
      return;
   YabThreadLock(rax_mtx);
   if (rax != NULL)
   {
      rax->fifo_head = rax->fifo_tail = 0;
      rax_reset_pending = 1;
   }
   YabThreadUnLock(rax_mtx);
}

void RaxHostWrite(u16 data)
{
   int next;

   if (!rax_enabled)
      return;
   YabThreadLock(rax_mtx);
   if (rax != NULL)
   {
      next = (rax->fifo_head + 1) % RAX_HOST_FIFO;
      if (next != rax->fifo_tail)
      {
         rax->fifo[rax->fifo_head] = data;
         rax->fifo_head = next;
      }
      else
         RAXLOG("RAX: host FIFO full, %04x dropped\n", data);
   }
   YabThreadUnLock(rax_mtx);
}

static void feed_host_latch(void)
{
   if (rax->fifo_tail == rax->fifo_head)
      return;

   /* acclaim_rax_device::data_w: latch the value, assert IRQL0 */
   if (!rax->latch_full || rax->latch_age >= RAX_LATCH_TIMEOUT)
   {
      rax->data_in = rax->fifo[rax->fifo_tail];
      rax->fifo_tail = (rax->fifo_tail + 1) % RAX_HOST_FIFO;
      rax->latch_full = 1;
      rax->latch_age = 0;
      Adsp2181SetIrqLine(&rax->cpu, ADSP2181_IRQL0, ADSP_LINE_ASSERT);
   }
}

void RaxRunSample(s32 *l, s32 *r)
{
   int budget;

   if (!rax_enabled)
      return;

   YabThreadLock(rax_mtx);
   if (rax == NULL || !rax_enabled)
   {
      YabThreadUnLock(rax_mtx);
      return;
   }

   if (rax_reset_pending)
   {
      rax_reset_pending = 0;
      rax_do_reset();
   }

   rax->cyc_frac += RAX_CLOCK;
   budget = (int)(rax->cyc_frac / RAX_OUT_RATE);
   rax->cyc_frac %= RAX_OUT_RATE;

   while (budget > 0)
   {
      int slice = budget;

      feed_host_latch();

      if (slice > RAX_SLICE_MAX)
         slice = RAX_SLICE_MAX;
      if (rax->ab_active && rax->frame_count > 0 && rax->frame_count < slice)
         slice = rax->frame_count;
      if (rax->bdma_pending && rax->bdma_countdown > 0 && rax->bdma_countdown < slice)
         slice = rax->bdma_countdown;

      if (!(rax->bdma_pending && rax->bdma_countdown <= 0))
      {
         Adsp2181Execute(&rax->cpu, slice);
         budget -= slice;
         if (rax->latch_full)
            rax->latch_age += slice;
         if (rax->bdma_pending)
            rax->bdma_countdown -= slice;
         if (rax->ab_active)
            rax->frame_count -= slice;
      }

      if (rax->bdma_pending && rax->bdma_countdown <= 0)
         bdma_done();

      while (rax->ab_active && rax->frame_count <= 0)
      {
         fetch_frame();
         rax->frame_count += rax->frame_period;
      }
   }

   /* linear interpolation between the last two DAC frames */
   {
      s32 out_l = rax->cur_l, out_r = rax->cur_r;
      if (rax->ab_active && rax->frame_period > 0)
      {
         s32 pos = rax->frame_period - rax->frame_count;   /* 0..period */
         out_l = rax->prev_l + ((rax->cur_l - rax->prev_l) * pos) / rax->frame_period;
         out_r = rax->prev_r + ((rax->cur_r - rax->prev_r) * pos) / rax->frame_period;
      }
      if (rax->dac_enabled[0]) *l += out_l;
      if (rax->dac_enabled[1]) *r += out_r;
   }

   YabThreadUnLock(rax_mtx);
}

/*---------------------------------------------------------------------------
   Save states ("RAX " block of the CART chunk, see cs0.c)

   The board is part of the Batman Forever cartridge. Saved: program RAM,
   the five data RAM banks, internal RAM, control registers, banking, the
   host latch and the queue of SH-2 writes not yet handed to the ADSP, the
   BDMA timer, the SPORT0 autobuffer/DAC state, the sample clock fraction
   and the ADSP-2181 registers. The ROM is not saved (loaded with the
   game). Called on the main thread; the sound thread runs RaxRunSample
   under the same mutex, so the board is never caught mid-sample.
---------------------------------------------------------------------------*/

#define RAX_STATE_VERSION 1

static void rax_w16(u16 v, void **stream) { MemStateWrite(&v, 2, 1, stream); }
static void rax_w32(u32 v, void **stream) { MemStateWrite(&v, 4, 1, stream); }
static u16 rax_r16(const void *stream) { u16 v; MemStateRead(&v, 2, 1, stream); return v; }
static u32 rax_r32(const void *stream) { u32 v; MemStateRead(&v, 4, 1, stream); return v; }

#define RAX_STATE_SIZE (4 + 4 + 0x4000*4 + 5*0x2000*2 + 0x1fe0*2 + 32*2 + 3*4 + \
                        2*2 + 2*4 + 4 + RAX_HOST_FIFO*2 + 2*4 + 3*4 + 2*4 + 2*4 + \
                        3*2 + 2*4 + 4*2 + 4)

void RaxSaveState(void **stream)
{
   int k;
   int present;

   rax_ensure_mutex();
   YabThreadLock(rax_mtx);
   present = (rax_enabled && rax != NULL) ? 1 : 0;

   rax_w32(RAX_STATE_VERSION, stream);
   rax_w32((u32)present, stream);
   if (present)
   {
      /* A reset still waiting for the sound thread is done now, so the
         saved board is the one that will run. */
      if (rax_reset_pending)
      {
         rax_reset_pending = 0;
         rax_do_reset();
      }

      MemStateWrite(rax->pram, 4, 0x4000, stream);
      MemStateWrite(rax->banked_ram, 2, 5 * 0x2000, stream);
      MemStateWrite(rax->int_ram, 2, 0x1fe0, stream);
      MemStateWrite(rax->control_regs, 2, 32, stream);
      rax_w32(rax->data_bank, stream);
      rax_w32(rax->rom_bank, stream);
      rax_w32(rax->dmovlay_val, stream);

      rax_w16(rax->data_in, stream);
      rax_w16(rax->data_out, stream);
      rax_w32((u32)rax->latch_full, stream);
      rax_w32((u32)rax->latch_age, stream);
      rax_w32(rax->snd_pf0, stream);
      MemStateWrite(rax->fifo, 2, RAX_HOST_FIFO, stream);
      rax_w32((u32)rax->fifo_head, stream);
      rax_w32((u32)rax->fifo_tail, stream);

      rax_w32((u32)rax->bdma_pending, stream);
      rax_w32((u32)rax->bdma_countdown, stream);
      rax_w32(rax->bdma_param, stream);

      for (k = 0; k < 2; k++)
         rax_w32((u32)rax->dac_enabled[k], stream);
      rax_w32((u32)rax->ab_active, stream);
      rax_w32((u32)rax->ab_ireg, stream);
      rax_w16(rax->ab_incs, stream);
      rax_w16(rax->ab_size, stream);
      rax_w16(rax->ab_base, stream);
      rax_w32((u32)rax->frame_period, stream);
      rax_w32((u32)rax->frame_count, stream);
      rax_w16((u16)rax->prev_l, stream);
      rax_w16((u16)rax->prev_r, stream);
      rax_w16((u16)rax->cur_l, stream);
      rax_w16((u16)rax->cur_r, stream);
      rax_w32(rax->cyc_frac, stream);

      Adsp2181SaveState(&rax->cpu, stream);
   }
   YabThreadUnLock(rax_mtx);
}

int RaxLoadState(const void *stream, int size)
{
   int k, status = 0;
   s32 v;

   if (size < 8)
      return -1;
   if (rax_r32(stream) != RAX_STATE_VERSION)
      return -1;
   if (rax_r32(stream) == 0)
      return 0;                      /* saved without the board */
   if (size < RAX_STATE_SIZE + Adsp2181StateSize())
      return -1;

   rax_ensure_mutex();
   YabThreadLock(rax_mtx);
   if (!rax_enabled || rax == NULL)
   {
      /* board absent here (sound ROMs missing): nothing to restore */
      YabThreadUnLock(rax_mtx);
      return -1;
   }

   rax_reset_pending = 0;            /* the state replaces a pending reset */
   rax_init_cpu();

   MemStateRead(rax->pram, 4, 0x4000, stream);
   for (k = 0; k < 0x4000; k++)
      rax->pram[k] &= 0xffffff;
   MemStateRead(rax->banked_ram, 2, 5 * 0x2000, stream);
   MemStateRead(rax->int_ram, 2, 0x1fe0, stream);
   MemStateRead(rax->control_regs, 2, 32, stream);
   rax->data_bank = rax_r32(stream) & 3;
   rax->rom_bank = rax_r32(stream);
   rax->dmovlay_val = rax_r32(stream) ? 1 : 0;

   rax->data_in = rax_r16(stream);
   rax->data_out = rax_r16(stream);
   rax->latch_full = rax_r32(stream) ? 1 : 0;
   rax->latch_age = (int)rax_r32(stream);
   rax->snd_pf0 = rax_r32(stream);
   MemStateRead(rax->fifo, 2, RAX_HOST_FIFO, stream);
   v = (s32)rax_r32(stream);
   rax->fifo_head = (v >= 0 && v < RAX_HOST_FIFO) ? v : 0;
   v = (s32)rax_r32(stream);
   rax->fifo_tail = (v >= 0 && v < RAX_HOST_FIFO) ? v : rax->fifo_head;

   rax->bdma_pending = rax_r32(stream) ? 1 : 0;
   rax->bdma_countdown = (int)rax_r32(stream);
   rax->bdma_param = rax_r32(stream);

   for (k = 0; k < 2; k++)
      rax->dac_enabled[k] = rax_r32(stream) ? 1 : 0;
   rax->ab_active = rax_r32(stream) ? 1 : 0;
   rax->ab_ireg = (int)(rax_r32(stream) & 7);
   rax->ab_incs = rax_r16(stream);
   rax->ab_size = rax_r16(stream);
   rax->ab_base = rax_r16(stream);
   v = (s32)rax_r32(stream);
   rax->frame_period = (v > 0) ? v : 16;   /* divisor of the interpolation */
   rax->frame_count = (int)rax_r32(stream);
   if (rax->frame_count > rax->frame_period)
      rax->frame_count = rax->frame_period;
   rax->prev_l = (s16)rax_r16(stream);
   rax->prev_r = (s16)rax_r16(stream);
   rax->cur_l = (s16)rax_r16(stream);
   rax->cur_r = (s16)rax_r16(stream);
   rax->cyc_frac = rax_r32(stream) % RAX_OUT_RATE;

   if (Adsp2181LoadState(&rax->cpu, stream, size - RAX_STATE_SIZE) != 0)
   {
      /* Board RAM restored but not the CPU: restart it like a reset
         rather than run code from another context. */
      YuiMsg("RAX: ADSP-2181 state not loaded, board reset\n");
      rax_reset_pending = 1;
      status = -1;
   }
   update_data_pages();
   YabThreadUnLock(rax_mtx);
   return status;
}
