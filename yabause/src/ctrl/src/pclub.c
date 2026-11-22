/*  Copyright 2026 Kronos contributors

    This file is part of Kronos.

    Kronos is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kronos is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kronos; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/*! \file pclub.c
    \brief ST-V Print Club video board 837-12936 and video printer.

    The camera reaches the game twice: live, through the VDP2 external
    screen input (EXBG, drawn by the video core as the NBG1 layer, see
    PClubGetExbgFrameRGBA() and yglcs.c), and as a still field read on the
    capture port when the photo is taken.

    See pclub.h for the register map and where it comes from. What is not
    emulated: the Brooktree chips themselves (the game only writes to
    them) and the two UARTs' peers (nothing is plugged in: the game
    programs them at 19200 baud but sends nothing during a game).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pclub.h"
#include "yabause.h"
#include "threads.h"
#include "yui.h"

/* ---- picture layout ----------------------------------------------------- */

#define PCLUB_VRAM_BANK   0x100000          /* bytes per bank */
#define PCLUB_VRAM_PITCH  0x800             /* bytes per line (1024 words) */
#define PCLUB_VRAM_LINES  512
#define PCLUB_CAP_BYTES   (PCLUB_CAPTURE_WIDTH * PCLUB_CAPTURE_HEIGHT * 3)
#define PCLUB_PRINT_BYTES (PCLUB_PRINT_WIDTH * PCLUB_PRINT_HEIGHT * 3)

/* Printed window of the VRAM. The frame artwork the game lays over the
   photo is 640 pixels wide, columns 94-733, and ends on line 503 (VRAM
   dumped at PRINT time, pclub2, "the United States" frame), hence a 640 x
   480 window at (94, 24). The camera field itself is copied from line 20,
   columns 718 down to 79 (mirror), each line twice (RAM 0x060052CC). */
#define PCLUB_PRINT_X0 94
#define PCLUB_PRINT_Y0 24

/* Order of the three bytes of a pixel. p0 / p1: low / high byte of the bank
   6 word, p2: low byte of the bank 7 word. The same order is used by the
   capture port (word +0 low, word +0 high, word +2 low), the game copying
   the bytes unchanged. */
#define PCLUB_P_R 0
#define PCLUB_P_G 1
#define PCLUB_P_B 2

/* ---- state -------------------------------------------------------------- */

typedef struct {
   u8 rbr, ier, fcr, lcr, mcr, scr, dll, dlm, dr;
} PClubUart;

static int pclub_on = 0;
static u16 pclub_ctrl;        /* 0x00500000 */
static u16 pclub_i2c;         /* 0x00500002 */
static u16 pclub_remote;      /* 0x00500004 */
static u16 pclub_preview;     /* 0x00500006 */
static u16 pclub_reg8;        /* 0x00500008 */
static u16 pclub_gain[3];     /* 0x0050000A, C, E */
static PClubUart pclub_uart[2];

static u8 *pclub_vram = NULL;     /* bank 6 then bank 7, big endian */
static u8 *pclub_live = NULL;     /* last camera field, 640 x 240 x 3 */
static u8 *pclub_frozen = NULL;   /* field taken by control bit 1 */
static u8 *pclub_staging = NULL;  /* host frame, written by another thread */
static int pclub_staging_new = 0;
static int pclub_have_host = 0;   /* the host has sent at least one frame */
static u32 pclub_host_serial = 0; /* incremented by each host frame */
static u32 pclub_freeze_serial = 0; /* incremented by each freeze */
static u8 *pclub_exbg = NULL;     /* RGBA picture for the VDP2 EXBG input */
static u32 pclub_exbg_src = 0xFFFFFFFF;
static u32 pclub_exbg_serial = 0;
static YabMutex *pclub_mutex = NULL;
/* The capture port is two streams with their own counter: +0 gives the
   red/green word of the next pixel, +2 the blue byte of the next pixel.
   pclub2 reads them both ways: +0 then +2 for each pixel (0x060050BC), or
   +0 for the whole field then +2 for the whole field (0x06005484 and
   0x06005548, 80 pixels per call). Reading 0x00540004 rewinds both. */
static u32 pclub_cap_rg;          /* next pixel of the +0 stream */
static u32 pclub_cap_b;           /* next pixel of the +2 stream */

/* printer */
static u8 *pclub_printer_mem = NULL; /* picture taken by MEMORY IN */
static int pclub_printer_mem_valid = 0;
static u32 pclub_busy_until = 0;
static int pclub_busy = 0;
static u32 pclub_print_frames = 600;
static u32 pclub_print_count = 0;
static char pclub_outdir[1024] = ".";
static PClubPrintFunc pclub_print_cb = NULL;

/* remote line decoder: runs of the same level, one write = one time slot */
static int pclub_level = -1;
static u32 pclub_run = 0;
static int pclub_sirc_active = 0, pclub_sirc_n = 0;
static u32 pclub_sirc_bits = 0;
static int pclub_t1_mark = 0, pclub_t1_n = 0;
static u32 pclub_t1_bits = 0;
static int pclub_tx_cmd = -1;     /* command already run in this transmission */

/* ---- helpers ------------------------------------------------------------ */

static void PClubPutRGB(u8 *p, u8 r, u8 g, u8 b) {
   p[PCLUB_P_R] = r; p[PCLUB_P_G] = g; p[PCLUB_P_B] = b;
}

/* Test pattern shown while the host gives no camera picture: 75% colour
   bars on the top two thirds, a grey ramp below. */
static void PClubTestPattern(u8 *dst) {
   static const u8 bars[7][3] = {
      {191,191,191},{191,191,0},{0,191,191},{0,191,0},{191,0,191},{191,0,0},{0,0,191}
   };
   int x, y;
   for (y = 0; y < PCLUB_CAPTURE_HEIGHT; y++)
      for (x = 0; x < PCLUB_CAPTURE_WIDTH; x++) {
         u8 *p = dst + (y * PCLUB_CAPTURE_WIDTH + x) * 3;
         if (y < PCLUB_CAPTURE_HEIGHT * 2 / 3) {
            const u8 *c = bars[x * 7 / PCLUB_CAPTURE_WIDTH];
            PClubPutRGB(p, c[0], c[1], c[2]);
         } else {
            u8 v = (u8)(x * 255 / (PCLUB_CAPTURE_WIDTH - 1));
            PClubPutRGB(p, v, v, v);
         }
      }
}

static int PClubAlloc(void) {
   if (pclub_vram == NULL) pclub_vram = (u8 *)calloc(2, PCLUB_VRAM_BANK);
   if (pclub_live == NULL) pclub_live = (u8 *)malloc(PCLUB_CAP_BYTES);
   if (pclub_frozen == NULL) pclub_frozen = (u8 *)malloc(PCLUB_CAP_BYTES);
   if (pclub_staging == NULL) pclub_staging = (u8 *)malloc(PCLUB_CAP_BYTES);
   if (pclub_printer_mem == NULL) pclub_printer_mem = (u8 *)malloc(PCLUB_PRINT_BYTES);
   if (pclub_exbg == NULL) pclub_exbg = (u8 *)malloc(PCLUB_CAPTURE_WIDTH * PCLUB_CAPTURE_HEIGHT * 4);
   if (pclub_mutex == NULL) pclub_mutex = YabThreadCreateMutex();
   return (pclub_vram && pclub_live && pclub_frozen && pclub_staging && pclub_printer_mem && pclub_exbg) ? 0 : -1;
}

/* Takes the last host frame, if any. Emulation thread only. */
static void PClubTakeStaging(void) {
   if (pclub_mutex == NULL) return;
   YabThreadLock(pclub_mutex);
   if (pclub_staging_new) {
      memcpy(pclub_live, pclub_staging, PCLUB_CAP_BYTES);
      pclub_staging_new = 0;
   }
   YabThreadUnLock(pclub_mutex);
}

/* Print window of the VRAM as RGB. */
static void PClubVramToRGB(u8 *dst) {
   int x, y;
   for (y = 0; y < PCLUB_PRINT_HEIGHT; y++)
      for (x = 0; x < PCLUB_PRINT_WIDTH; x++) {
         u32 off = (u32)(PCLUB_PRINT_Y0 + y) * PCLUB_VRAM_PITCH + (u32)(PCLUB_PRINT_X0 + x) * 2;
         u8 px[3];
         u8 *d = dst + (y * PCLUB_PRINT_WIDTH + x) * 3;
         px[0] = pclub_vram[off + 1];                   /* bank 6, low byte */
         px[1] = pclub_vram[off];                       /* bank 6, high byte */
         px[2] = pclub_vram[PCLUB_VRAM_BANK + off + 1]; /* bank 7, low byte */
         d[0] = px[PCLUB_P_R]; d[1] = px[PCLUB_P_G]; d[2] = px[PCLUB_P_B];
      }
}

static int PClubWriteBMP(const char *path, const u8 *rgb, int w, int h) {
   FILE *f = fopen(path, "wb");
   u32 rowsize = ((u32)w * 3 + 3) & ~3u, datasize = rowsize * (u32)h;
   u8 hdr[54];
   static const u8 pad[3] = {0, 0, 0};
   int x, y;
   if (f == NULL) return -1;
   memset(hdr, 0, sizeof(hdr));
   hdr[0] = 'B'; hdr[1] = 'M';
#define PUT32(o, v) do { hdr[o] = (u8)(v); hdr[o+1] = (u8)((v) >> 8); hdr[o+2] = (u8)((v) >> 16); hdr[o+3] = (u8)((v) >> 24); } while (0)
   PUT32(2, 54 + datasize);
   PUT32(10, 54);
   PUT32(14, 40);
   PUT32(18, (u32)w);
   PUT32(22, (u32)h);
   hdr[26] = 1; hdr[28] = 24;
   PUT32(34, datasize);
   PUT32(38, 2835); PUT32(42, 2835);
#undef PUT32
   fwrite(hdr, 1, sizeof(hdr), f);
   for (y = h - 1; y >= 0; y--) {
      for (x = 0; x < w; x++) {
         const u8 *s = rgb + (y * w + x) * 3;
         u8 bgr[3];
         bgr[0] = s[2]; bgr[1] = s[1]; bgr[2] = s[0];
         fwrite(bgr, 1, 3, f);
      }
      fwrite(pad, 1, rowsize - (u32)w * 3, f);
   }
   fclose(f);
   return 0;
}

/* ---- printer ------------------------------------------------------------ */

static void PClubPrinterMemoryIn(void) {
   PClubVramToRGB(pclub_printer_mem);
   pclub_printer_mem_valid = 1;
}

static void PClubPrinterPrint(void) {
   char path[1100];
   if (!pclub_printer_mem_valid)
      PClubPrinterMemoryIn(); /* PRINT without MEMORY IN: what is on the line */
   /* the counter starts again at 0 with each session (and comes back with
      a savestate): skip the numbers already used, never overwrite a print */
   for (;;) {
      FILE *f;
      pclub_print_count++;
      snprintf(path, sizeof(path), "%s/print_%04u.bmp", pclub_outdir, (unsigned)pclub_print_count);
      f = fopen(path, "rb");
      if (f == NULL) break;
      fclose(f);
   }
   if (PClubWriteBMP(path, pclub_printer_mem, PCLUB_PRINT_WIDTH, PCLUB_PRINT_HEIGHT) == 0)
      YuiMsg("Print Club: print written to %s\n", path);
   else
      YuiMsg("Print Club: cannot write %s\n", path);
   if (pclub_print_cb != NULL)
      pclub_print_cb(pclub_printer_mem, PCLUB_PRINT_WIDTH, PCLUB_PRINT_HEIGHT, path);
   pclub_busy = 1;
   pclub_busy_until = yabsys.frame_count + pclub_print_frames;
}

/* SIRC (kind 0): 16 = MEMORY IN, 17 = PRINT.
   Pulse-distance (kind 1): 23 = MEMORY IN, 19 = PRINT.
   (main program RAM 0x06006E92-0x06006EE2, diagnostic 'm', 'p', 'M', 'P') */
static void PClubPrinterCommand(int kind, int cmd) {
   int key = (kind << 8) | cmd;
   if (key == pclub_tx_cmd) return; /* the frame is repeated (2 or 5 times) */
   pclub_tx_cmd = key;
   if ((kind == 0 && cmd == 16) || (kind == 1 && cmd == 23))
      PClubPrinterMemoryIn();
   else if ((kind == 0 && cmd == 17) || (kind == 1 && cmd == 19))
      PClubPrinterPrint();
   else
      YuiMsg("Print Club: unknown printer command %d (kind %d)\n", cmd, kind);
}

/* One run of the remote line: `len` time slots at `level`.
   SIRC: start = 4 marks, then for each bit 1 space + 1 mark (0) or 2 marks
   (1), 7 command bits then 8 address bits, LSB first.
   Kind 1: each bit is 1 mark + 3 spaces (0) or 7 spaces (1), 6 bits LSB
   first, a stop mark, then a gap. */
static void PClubRemoteRun(int level, u32 len) {
   if (level) {
      if (len >= 4) {
         pclub_sirc_active = 1; pclub_sirc_n = 0; pclub_sirc_bits = 0;
      } else if (pclub_sirc_active) {
         pclub_sirc_bits |= (u32)(len >= 2) << pclub_sirc_n;
         if (++pclub_sirc_n == 15) {
            PClubPrinterCommand(0, (int)(pclub_sirc_bits & 0x7F));
            pclub_sirc_active = 0;
         }
      }
      if (len == 1) pclub_t1_mark = 1;
      else { pclub_t1_mark = 0; pclub_t1_n = 0; pclub_t1_bits = 0; }
   } else {
      if (len > 2) pclub_sirc_active = 0;
      if (pclub_t1_mark && (len == 3 || len == 7) && pclub_t1_n < 6) {
         pclub_t1_bits |= (u32)(len == 7) << pclub_t1_n;
         pclub_t1_n++;
      } else {
         if (pclub_t1_mark && len > 7 && pclub_t1_n == 6)
            PClubPrinterCommand(1, (int)pclub_t1_bits);
         pclub_t1_n = 0; pclub_t1_bits = 0;
      }
      pclub_t1_mark = 0;
   }
}

static void PClubRemoteFlush(void) {
   if (pclub_level >= 0 && pclub_run)
      PClubRemoteRun(pclub_level, pclub_level ? pclub_run : 0xFFFF);
   pclub_level = -1;
   pclub_run = 0;
}

static void PClubRemoteWrite(u16 val) {
   int level = val & 1;
   pclub_remote = val;
   if (level == pclub_level) { pclub_run++; return; }
   if (pclub_level >= 0) PClubRemoteRun(pclub_level, pclub_run);
   pclub_level = level;
   pclub_run = 1;
}

u8 PClubPrinterPortG(void) {
   if (pclub_busy && (s32)(yabsys.frame_count - pclub_busy_until) >= 0)
      pclub_busy = 0;
   /* bit 0 = 1 (no error), bit 2 = 0 (printer on), bit 3 = busy */
   return (u8)(0x01 | (pclub_busy ? 0x08 : 0x00));
}

/* ---- control register --------------------------------------------------- */

static void PClubControlWrite(u16 val) {
   u16 old = pclub_ctrl;
   pclub_ctrl = val;
   if ((val & 0x40) && !(old & 0x40)) {   /* printer command starts */
      PClubRemoteFlush();
      pclub_sirc_active = 0; pclub_t1_mark = 0; pclub_t1_n = 0;
      pclub_tx_cmd = -1;
   } else if (!(val & 0x40) && (old & 0x40)) {
      PClubRemoteFlush();                 /* last frame ends on a gap */
   } else if ((val ^ old) & 0x08) {
      PClubRemoteFlush();
   }
   if ((val & 0x02) && !(old & 0x02)) {   /* freeze: take the field */
      PClubTakeStaging();
      YabThreadLock(pclub_mutex);
      memcpy(pclub_frozen, pclub_live, PCLUB_CAP_BYTES);
      pclub_freeze_serial++;
      YabThreadUnLock(pclub_mutex);
   }
}

/* ---- UART 16550 (nothing on the line) ----------------------------------- */

static u8 PClubUartRead(PClubUart *u, u32 reg) {
   switch (reg) {
   case 0:
      if (u->lcr & 0x80) return u->dll;
      u->dr = 0;
      return u->rbr;
   case 1: return (u->lcr & 0x80) ? u->dlm : u->ier;
   case 2: return (u8)(((u->fcr & 1) ? 0xC0 : 0x00) | 0x01); /* no interrupt */
   case 3: return u->lcr;
   case 4: return u->mcr;
   case 5: return (u8)(0x60 | u->dr);     /* THRE | TEMT, DR in loopback */
   case 6:                                 /* loopback: RTS->CTS, DTR->DSR, OUT1->RI, OUT2->DCD */
      if (!(u->mcr & 0x10)) return 0x00;
      return (u8)(((u->mcr & 0x02) << 3) | ((u->mcr & 0x01) << 5) | ((u->mcr & 0x0C) << 4));
   default: return u->scr;
   }
}

static void PClubUartWrite(PClubUart *u, u32 reg, u8 val) {
   switch (reg) {
   case 0:
      if (u->lcr & 0x80) u->dll = val;
      else if (u->mcr & 0x10) { u->rbr = val; u->dr = 1; } /* loopback: back to RBR */
      break;                               /* else sent to nothing */
   case 1: if (u->lcr & 0x80) u->dlm = val; else u->ier = val & 0x0F; break;
   case 2: u->fcr = val & 0xC9; break;
   case 3: u->lcr = val; break;
   case 4: u->mcr = val & 0x1F; break;
   case 7: u->scr = val; break;
   default: break;
   }
}

/* ---- bus ---------------------------------------------------------------- */

#define PCLUB_CAP_PIXELS (PCLUB_CAPTURE_WIDTH * PCLUB_CAPTURE_HEIGHT)

static u16 PClubCaptureRead(u32 off) {
   const u8 *p;
   if ((off & 2) == 0) {   /* +0: pixel bytes 0 (low) and 1 (high) */
      if (pclub_cap_rg >= PCLUB_CAP_PIXELS) pclub_cap_rg = 0;
      p = pclub_frozen + pclub_cap_rg++ * 3;
      return (u16)(p[0] | (p[1] << 8));
   }
   /* +2: pixel byte 2 (low) */
   if (pclub_cap_b >= PCLUB_CAP_PIXELS) pclub_cap_b = 0;
   p = pclub_frozen + pclub_cap_b++ * 3;
   return p[2];
}

static u16 PClubRegRead(u32 a) {
   if (a >= 0x00600000) {
      u32 off = a - 0x00600000;   /* bank 7 follows bank 6 */
      return (u16)((pclub_vram[off] << 8) | pclub_vram[off + 1]);
   }
   if (a >= 0x00580000) {
      if (a < 0x00580010) return PClubUartRead(&pclub_uart[1], (a >> 1) & 7);
      return 0;
   }
   if (a >= 0x00540000) {
      switch (a & 0xFFFF) {
      case 0: case 2: return PClubCaptureRead(a);
      case 4:            /* status; rewinds the read pointer */
         pclub_cap_rg = pclub_cap_b = 0;
         if (!(pclub_ctrl & 0x02)) { /* not frozen: the current field */
            PClubTakeStaging();
            YabThreadLock(pclub_mutex);
            memcpy(pclub_frozen, pclub_live, PCLUB_CAP_BYTES);
            YabThreadUnLock(pclub_mutex);
         }
         return 0;
      default: return 0;
      }
   }
   if (a >= 0x00500000 && a < 0x00500010) {
      switch (a & 0xF) {
      case 0x0: return pclub_ctrl;
      case 0x2: return pclub_i2c;
      case 0x4: return pclub_remote;
      case 0x6: return pclub_preview;
      case 0x8: return pclub_reg8;
      case 0xA: return pclub_gain[0];
      case 0xC: return pclub_gain[1];
      default:  return pclub_gain[2];
      }
   }
   if (a >= 0x00500010 && a < 0x00500020)
      return PClubUartRead(&pclub_uart[0], (a >> 1) & 7);
   return 0;
}

static void PClubRegWrite(u32 a, u16 val) {
   if (a >= 0x00600000) {
      u32 off = a - 0x00600000;
      pclub_vram[off] = (u8)(val >> 8);
      pclub_vram[off + 1] = (u8)val;
      return;
   }
   if (a >= 0x00580000) {
      if (a < 0x00580010) PClubUartWrite(&pclub_uart[1], (a >> 1) & 7, (u8)val);
      return;
   }
   if (a >= 0x00540000) return;
   if (a >= 0x00500000 && a < 0x00500010) {
      switch (a & 0xF) {
      case 0x0: PClubControlWrite(val); break;
      case 0x2: pclub_i2c = val; break;  /* Bt chips: write only */
      case 0x4: PClubRemoteWrite(val); break;
      case 0x6: pclub_preview = val; break;
      case 0x8: pclub_reg8 = val; break;
      case 0xA: pclub_gain[0] = val; break;
      case 0xC: pclub_gain[1] = val; break;
      default:  pclub_gain[2] = val; break;
      }
      return;
   }
   if (a >= 0x00500010 && a < 0x00500020)
      PClubUartWrite(&pclub_uart[0], (a >> 1) & 7, (u8)val);
}

#define PCLUB_ADDR(addr) ((addr) & 0x00FFFFFF)

u16 FASTCALL PClubReadWord(SH2_struct *context, u8 *memory, u32 addr) {
   u32 a = PCLUB_ADDR(addr) & ~1u;
   (void)context; (void)memory;
   if (!pclub_on) return 0;
   return PClubRegRead(a);
}

u8 FASTCALL PClubReadByte(SH2_struct *context, u8 *memory, u32 addr) {
   u32 a = PCLUB_ADDR(addr);
   (void)context; (void)memory;
   if (!pclub_on) return 0;
   if (a >= 0x00600000) return pclub_vram[a - 0x00600000];
   {
      u16 w = PClubRegRead(a & ~1u);
      return (a & 1) ? (u8)w : (u8)(w >> 8);
   }
}

u32 FASTCALL PClubReadLong(SH2_struct *context, u8 *memory, u32 addr) {
   u32 a = PCLUB_ADDR(addr) & ~3u;
   (void)context; (void)memory;
   if (!pclub_on) return 0;
   return ((u32)PClubRegRead(a) << 16) | PClubRegRead(a + 2);
}

void FASTCALL PClubWriteWord(SH2_struct *context, u8 *memory, u32 addr, u16 val) {
   (void)context; (void)memory;
   if (!pclub_on) return;
   PClubRegWrite(PCLUB_ADDR(addr) & ~1u, val);
}

void FASTCALL PClubWriteByte(SH2_struct *context, u8 *memory, u32 addr, u8 val) {
   u32 a = PCLUB_ADDR(addr);
   (void)context; (void)memory;
   if (!pclub_on) return;
   if (a >= 0x00600000) { pclub_vram[a - 0x00600000] = val; return; }
   /* 8-bit registers sit on the low byte of the 16-bit bus */
   if (a & 1) PClubRegWrite(a & ~1u, val);
}

void FASTCALL PClubWriteLong(SH2_struct *context, u8 *memory, u32 addr, u32 val) {
   u32 a = PCLUB_ADDR(addr) & ~3u;
   (void)context; (void)memory;
   if (!pclub_on) return;
   PClubRegWrite(a, (u16)(val >> 16));
   PClubRegWrite(a + 2, (u16)val);
}

/* ---- life cycle --------------------------------------------------------- */

void PClubReset(void) {
   if (!pclub_on) return;
   pclub_ctrl = pclub_i2c = pclub_remote = pclub_preview = pclub_reg8 = 0;
   memset(pclub_gain, 0, sizeof(pclub_gain));
   memset(pclub_uart, 0, sizeof(pclub_uart));
   memset(pclub_vram, 0, 2 * PCLUB_VRAM_BANK);
   pclub_cap_rg = pclub_cap_b = 0;
   pclub_printer_mem_valid = 0;
   pclub_busy = 0;
   pclub_level = -1; pclub_run = 0;
   pclub_sirc_active = 0; pclub_t1_mark = 0; pclub_t1_n = 0;
   pclub_tx_cmd = -1;
   PClubTakeStaging();
   YabThreadLock(pclub_mutex);
   memcpy(pclub_frozen, pclub_live, PCLUB_CAP_BYTES);
   pclub_freeze_serial++;
   YabThreadUnLock(pclub_mutex);
}

void PClubEnable(void) {
   if (PClubAlloc() != 0) {
      YuiMsg("Print Club: out of memory, board disabled\n");
      return;
   }
   if (!pclub_on) PClubTestPattern(pclub_live);
   pclub_on = 1;
   PClubReset();
}

void PClubShutdown(void) {
   pclub_on = 0;
}

int PClubIsEnabled(void) {
   return pclub_on;
}

void PClubSetOutputDir(const char *dir) {
   snprintf(pclub_outdir, sizeof(pclub_outdir), "%s", (dir != NULL && dir[0]) ? dir : ".");
}

void PClubSetPrintCallback(PClubPrintFunc cb) {
   pclub_print_cb = cb;
}

void PClubSetPrintFrames(u32 frames) {
   pclub_print_frames = frames ? frames : 1;
}

void PClubSetCameraFrame(const u8 *rgb, int width, int height, int stride) {
   int x, y;
   if (PClubAlloc() != 0) return;
   if (rgb == NULL) {
      /* the host camera went away: back to the test pattern */
      YabThreadLock(pclub_mutex);
      PClubTestPattern(pclub_staging);
      pclub_staging_new = 1;
      pclub_have_host = 1;
      pclub_host_serial++;
      YabThreadUnLock(pclub_mutex);
      return;
   }
   if (width <= 0 || height <= 0) return;
   if (stride <= 0) stride = width * 3;
   YabThreadLock(pclub_mutex);
   /* one field: every other line of the picture scaled to 640 x 480 */
   for (y = 0; y < PCLUB_CAPTURE_HEIGHT; y++) {
      const u8 *src = rgb + (size_t)((y * 2 * height) / (PCLUB_CAPTURE_HEIGHT * 2)) * stride;
      u8 *dst = pclub_staging + y * PCLUB_CAPTURE_WIDTH * 3;
      for (x = 0; x < PCLUB_CAPTURE_WIDTH; x++) {
         const u8 *s = src + ((x * width) / PCLUB_CAPTURE_WIDTH) * 3;
         PClubPutRGB(dst + x * 3, s[0], s[1], s[2]);
      }
   }
   pclub_staging_new = 1;
   pclub_have_host = 1;
   pclub_host_serial++;
   YabThreadUnLock(pclub_mutex);
}

/* ---- VDP2 external screen ----------------------------------------------- */

const u8 *PClubGetExbgFrameRGBA(int *width, int *height) {
   const u8 *src;
   u32 want;
   int i;
   if (!pclub_on || pclub_exbg == NULL || pclub_mutex == NULL) return NULL;
   YabThreadLock(pclub_mutex);
   /* frozen field after control bit 1, else the newest camera frame (or
      the test pattern while the host sends none) */
   if (pclub_ctrl & 0x02) { src = pclub_frozen; want = 0x80000000u | (pclub_freeze_serial & 0x7FFFFFFFu); }
   else if (pclub_have_host) { src = pclub_staging; want = pclub_host_serial & 0x7FFFFFFFu; }
   else { src = pclub_live; want = 0x7FFFFFFFu; }
   if (want != pclub_exbg_src) {
      for (i = 0; i < PCLUB_CAPTURE_WIDTH * PCLUB_CAPTURE_HEIGHT; i++) {
         pclub_exbg[i * 4 + 0] = src[i * 3 + PCLUB_P_R];
         pclub_exbg[i * 4 + 1] = src[i * 3 + PCLUB_P_G];
         pclub_exbg[i * 4 + 2] = src[i * 3 + PCLUB_P_B];
         pclub_exbg[i * 4 + 3] = 0xFF;
      }
      pclub_exbg_src = want;
      pclub_exbg_serial++;
   }
   YabThreadUnLock(pclub_mutex);
   if (width) *width = PCLUB_CAPTURE_WIDTH;
   if (height) *height = PCLUB_CAPTURE_HEIGHT;
   return pclub_exbg;
}

u32 PClubGetExbgSerial(void) {
   return pclub_exbg_serial;
}

/* ---- save state --------------------------------------------------------- */

/* 1: one byte cursor for the capture port; 2: one pixel cursor per stream */
#define PCLUB_STATE_VERSION 2

void PClubSaveState(void **stream) {
   u32 version = PCLUB_STATE_VERSION;
   u32 on = (u32)pclub_on;
   u32 busy_left = 0;
   MemStateWrite(&version, 4, 1, stream);
   MemStateWrite(&on, 4, 1, stream);
   if (!pclub_on) return;
   if (pclub_busy && (s32)(pclub_busy_until - yabsys.frame_count) > 0)
      busy_left = pclub_busy_until - yabsys.frame_count;
   MemStateWrite(&pclub_ctrl, 2, 1, stream);
   MemStateWrite(&pclub_i2c, 2, 1, stream);
   MemStateWrite(&pclub_remote, 2, 1, stream);
   MemStateWrite(&pclub_preview, 2, 1, stream);
   MemStateWrite(&pclub_reg8, 2, 1, stream);
   MemStateWrite(pclub_gain, 2, 3, stream);
   MemStateWrite(pclub_uart, sizeof(PClubUart), 2, stream);
   MemStateWrite(&pclub_cap_rg, 4, 1, stream);
   MemStateWrite(&pclub_cap_b, 4, 1, stream);
   MemStateWrite(&busy_left, 4, 1, stream);
   MemStateWrite(&pclub_print_count, 4, 1, stream);
   MemStateWrite(&pclub_printer_mem_valid, sizeof(int), 1, stream);
   MemStateWrite(pclub_vram, 1, 2 * PCLUB_VRAM_BANK, stream);
   MemStateWrite(pclub_frozen, 1, PCLUB_CAP_BYTES, stream);
   MemStateWrite(pclub_printer_mem, 1, PCLUB_PRINT_BYTES, stream);
}

int PClubLoadState(const void *stream, int size) {
   u32 version, on, busy_left;
   int need = 8 + 2 * 8 + (int)sizeof(PClubUart) * 2 + 4 * 3 + (int)sizeof(int)
            + 2 * PCLUB_VRAM_BANK + PCLUB_CAP_BYTES + PCLUB_PRINT_BYTES;
   if (size < 8) return -1;
   MemStateRead(&version, 4, 1, stream);
   if (version != 1 && version != PCLUB_STATE_VERSION) return -1;
   if (version >= 2) need += 4;
   MemStateRead(&on, 4, 1, stream);
   if (!on || !pclub_on) return on == (u32)pclub_on ? 0 : -1;
   if (size < need) return -1;
   MemStateRead(&pclub_ctrl, 2, 1, stream);
   MemStateRead(&pclub_i2c, 2, 1, stream);
   MemStateRead(&pclub_remote, 2, 1, stream);
   MemStateRead(&pclub_preview, 2, 1, stream);
   MemStateRead(&pclub_reg8, 2, 1, stream);
   MemStateRead(pclub_gain, 2, 3, stream);
   MemStateRead(pclub_uart, sizeof(PClubUart), 2, stream);
   if (version == 1) {
      u32 pos;   /* old shared byte cursor */
      MemStateRead(&pos, 4, 1, stream);
      pclub_cap_rg = pclub_cap_b = pos / 3;
   } else {
      MemStateRead(&pclub_cap_rg, 4, 1, stream);
      MemStateRead(&pclub_cap_b, 4, 1, stream);
   }
   MemStateRead(&busy_left, 4, 1, stream);
   MemStateRead(&pclub_print_count, 4, 1, stream);
   MemStateRead(&pclub_printer_mem_valid, sizeof(int), 1, stream);
   MemStateRead(pclub_vram, 1, 2 * PCLUB_VRAM_BANK, stream);
   MemStateRead(pclub_frozen, 1, PCLUB_CAP_BYTES, stream);
   MemStateRead(pclub_printer_mem, 1, PCLUB_PRINT_BYTES, stream);
   pclub_freeze_serial++;   /* the EXBG picture may have changed */
   if (pclub_cap_rg > PCLUB_CAP_PIXELS) pclub_cap_rg = 0;
   if (pclub_cap_b > PCLUB_CAP_PIXELS) pclub_cap_b = 0;
   pclub_busy = busy_left != 0;
   pclub_busy_until = yabsys.frame_count + busy_left;
   pclub_level = -1; pclub_run = 0;
   pclub_sirc_active = 0; pclub_t1_mark = 0; pclub_t1_n = 0;
   return 0;
}
