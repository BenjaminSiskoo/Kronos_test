/*  Copyright 2026 Kronos team

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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file sci.c
    \brief SH-2 serial communication interface (SCI), SH7604 Hardware Manual
    (ADE-602-085C rev. 4.0) section 13, and the Saturn communication
    connector (sci.h).

    What the old code in sh2core.c did, against the manual:
      - a write to SSR was ignored: TDRE, RDRF, ORER, FER and PER could never
        be cleared by software (13.2.7: "cleared to 0 only if they have
        first been read (after being set to 1)", by writing 0);
      - TDRE never went to 0 and TEND was never handled; a write to SSR with
        TE = 1 called SCITransmitByte() at once, whatever was written;
      - no transfer took time and none set RDRF / TDRE / TEND, so the RXI,
        TXI, TEI and ERI sources of SH2EvaluateInterrupt() never fired;
      - SCITransmitByte() / SCIReceiveByte() were empty.

    Model (per SH-2):
      Transmitter (13.3.2 / 13.3.4, figures 13.5 and 13.15): when TE = 1,
      TSR is empty and software has cleared TDRE after writing TDR, TDR is
      loaded into TSR, TDRE is set to 1 (TXI) and TEND cleared; the frame
      then takes frame_bits x bit time. At its end the character goes to the
      communication port device, the next TDR is loaded if TDRE = 0, else
      TEND is set to 1 (TEI). TE = 0 sets TDRE and TEND to 1 and stops TSR.
      Receiver: a character becomes available after one frame; if RDRF is
      still 1 it is lost and ORER is set (ERI), else it goes to RDR and RDRF
      is set (RXI). While ORER, FER or PER is 1, reception stops, and in
      clocked synchronous mode transmission too (13.2.7).
      Clocked synchronous mode, SCI clock (CKE1 = 0): the SCI only clocks
      while it transmits, or continuously when only RE = 1 (figure 13.19).
      With TE = RE = 1 each transmitted character clocks in one received
      character (full duplex, figure 13.20). External clock (CKE1 = 1): the
      device gives the bit time; without a device or clock nothing moves.
      Bit time (13.2.8, BRR formulas): asynchronous 128 x 4^n x (N + 1)
      cycles of phi, clocked synchronous 16 x 4^n x (N + 1), n = CKS1-0,
      N = BRR. Kronos counts SH-2 cycles at phi.
      Asynchronous frame: start + 7/8 data + parity or multiprocessor bit
      + 1/2 stop bits (SMR, table 13.11). The device gives clean characters,
      so FER and PER are never set; the multiprocessor reception filter
      (MPIE) is not modelled.

    Not modelled: SCI-requested DMAC transfers (DRCR RS = RXI / TXI: the
    DMAC of Kronos treats every channel as auto-request). The trace logs
    the DRCR writes so a driver using them shows up. The transfer state
    (TSR / RSR in flight) is not in save states, the registers are.

    Instrumentation: set the environment variable KRONOS_SCI_TRACE to a file
    name (e.g. sci_trace.txt) before starting Kronos to get every SCI
    register write, every character sent / received and every DRCR write,
    with the frame, the line and the PC.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sci.h"
#include "sh2core.h"
#include "yabause.h"

extern yabsys_struct yabsys;

/* SSR */
#define SSR_TDRE 0x80
#define SSR_RDRF 0x40
#define SSR_ORER 0x20
#define SSR_FER  0x10
#define SSR_PER  0x08
#define SSR_TEND 0x04
#define SSR_MPB  0x02
#define SSR_MPBT 0x01
#define SSR_ERRORS (SSR_ORER | SSR_FER | SSR_PER)
#define SSR_CLEARABLE (SSR_TDRE | SSR_RDRF | SSR_ORER | SSR_FER | SSR_PER)

/* SCR */
#define SCR_TIE  0x80
#define SCR_RIE  0x40
#define SCR_TE   0x20
#define SCR_RE   0x10
#define SCR_MPIE 0x08
#define SCR_TEIE 0x04
#define SCR_CKE1 0x02
#define SCR_CKE0 0x01

/* SMR */
#define SMR_CA   0x80
#define SMR_CHR  0x40
#define SMR_PE   0x20
#define SMR_OE   0x10
#define SMR_STOP 0x08
#define SMR_MP   0x04

typedef struct
{
   int tx_busy;        /* TSR holds a character being shifted out */
   s32 tx_left;        /* cycles left in the current transmit frame */
   u8  tsr;
   int rx_busy;        /* a character is being shifted in */
   s32 rx_left;
   u8  rsr;
   u8  ssr_read;       /* flags read as 1 since they were last cleared */
   int rx_wait_re;     /* device ended a frame: wait for RE 0 -> 1 */
   u32 last_cycles;    /* context->cycles at the last SCIExec() */
} SciState;

static SciState sci[2];
static CommPortDevice * commport = NULL;

static FILE * sci_trace = NULL;
static int sci_trace_init = 0;

static int SCIIndex(SH2_struct * context)
{
   return (context == SSH2) ? SCI_CPU_SLAVE : SCI_CPU_MASTER;
}

//////////////////////////////////////////////////////////////////////////////
// Instrumentation
//////////////////////////////////////////////////////////////////////////////

static FILE * SCITraceFile(void)
{
   if (!sci_trace_init)
   {
      const char * path = getenv("KRONOS_SCI_TRACE");
      sci_trace_init = 1;
      if (path != NULL && path[0] != 0)
      {
         sci_trace = fopen(path, "w");
         if (sci_trace != NULL)
            fprintf(sci_trace, "# Kronos SCI trace (SH7604 SCI, Saturn communication connector)\n"
                               "# frame line cpu PC event\n");
      }
   }
   return sci_trace;
}

static void SCITrace(SH2_struct * context, const char * fmt, ...)
{
   FILE * f = SCITraceFile();
   va_list ap;
   if (f == NULL)
      return;
   fprintf(f, "%8u %3d %s %08X ", (unsigned)yabsys.frame_count, yabsys.LineCount,
           (context == SSH2) ? "SSH2" : "MSH2",
           (SH2Core != NULL) ? (unsigned)SH2Core->GetPC(context) : 0u);
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fflush(f);
}

void SCITraceDRCR(SH2_struct * context, int channel, u8 val)
{
   static const char * const rs[4] = { "DREQ", "RXI", "TXI", "reserved" };
   SCITrace(context, "DRCR%d <- %02X (resource %s)", channel, val, rs[val & 3]);
}

//////////////////////////////////////////////////////////////////////////////
// Timing
//////////////////////////////////////////////////////////////////////////////

static int SCISync(SH2_struct * context)
{
   return (context->onchip.SMR & SMR_CA) != 0;
}

static int SCIExternalClock(SH2_struct * context)
{
   return (context->onchip.SCR & SCR_CKE1) != 0;
}

/* SH-2 cycles per bit, 0 = no clock. */
static u32 SCIBitCycles(SH2_struct * context)
{
   if (SCIExternalClock(context))
   {
      if (commport == NULL || commport->ExternalBitCycles == NULL)
         return 0;
      return commport->ExternalBitCycles(SCIIndex(context));
   }
   {
      const u32 n = context->onchip.SMR & 3;
      const u32 N = context->onchip.BRR;
      const u32 scale = 1u << (2 * n); /* 4^n */
      return (SCISync(context) ? 16u : 128u) * scale * (N + 1);
   }
}

static u32 SCIFrameBits(SH2_struct * context)
{
   const u8 smr = context->onchip.SMR;
   u32 bits;
   if (smr & SMR_CA)
      return 8;
   bits = 1 + ((smr & SMR_CHR) ? 7 : 8);
   if (smr & SMR_MP)
      bits++;
   else if (smr & SMR_PE)
      bits++;
   bits += (smr & SMR_STOP) ? 2 : 1;
   return bits;
}

//////////////////////////////////////////////////////////////////////////////
// Transfer engine
//////////////////////////////////////////////////////////////////////////////

/* A received character arrives in RSR -> RDR. */
static void SCIReceiveDone(SH2_struct * context, u8 data, int from_device)
{
   if (context->onchip.SSR & SSR_RDRF)
   {
      context->onchip.SSR |= SSR_ORER; /* 13.2.7: the character is lost */
      SCITrace(context, "RX %02X lost: overrun (RDRF still 1), ORER = 1", data);
   }
   else
   {
      context->onchip.RDR = data;
      context->onchip.SSR |= SSR_RDRF;
      SCITrace(context, "RX %02X%s", data, from_device ? "" : " (idle line)");
   }
   SH2EvaluateInterrupt(context);
}

/* Asks the device for a character; idle line = FFh. */
static int SCIDeviceReceive(SH2_struct * context, u8 * data)
{
   *data = 0xFF;
   SciState * st = &sci[SCIIndex(context)];
   int r;
   if (commport == NULL || commport->Receive == NULL || st->rx_wait_re)
      return 0;
   r = commport->Receive(SCIIndex(context), data, SCISync(context));
   if (r == SCI_RX_END_OF_FRAME)
   {
      st->rx_wait_re = 1;
      r = 1;
   }
   return r;
}

static void SCIDeviceTransmit(SH2_struct * context, u8 data)
{
   const u8 out = (!SCISync(context) && (context->onchip.SMR & SMR_CHR)) ? (data & 0x7F) : data;
   SCITrace(context, "TX %02X%s", out, commport ? "" : " (nothing plugged in)");
   if (commport != NULL && commport->Transmit != NULL)
      commport->Transmit(SCIIndex(context), out, SCISync(context));
}

/* TDR -> TSR, if allowed. */
static int SCITxLoad(SH2_struct * context, SciState * st, u32 frame)
{
   if (!(context->onchip.SCR & SCR_TE) || (context->onchip.SSR & SSR_TDRE))
      return 0;
   if (SCISync(context) && (context->onchip.SSR & SSR_ERRORS))
      return 0; /* clocked synchronous: transmission disabled by an error flag */
   st->tsr = context->onchip.TDR;
   SCITrace(context, "TDR -> TSR %02X (frame %u cycles)", st->tsr, (unsigned)frame);
   st->tx_busy = 1;
   st->tx_left += (s32)frame;
   context->onchip.SSR |= SSR_TDRE;
   context->onchip.SSR &= ~SSR_TEND;
   SH2EvaluateInterrupt(context); /* TXI */
   return 1;
}

static void SCIProcess(SH2_struct * context, s32 elapsed)
{
   SciState * st = &sci[SCIIndex(context)];
   const u8 scr = context->onchip.SCR;
   const int sync = SCISync(context);
   const u32 bit = SCIBitCycles(context);
   const u32 frame = bit * SCIFrameBits(context);
   const int full_duplex = sync && !SCIExternalClock(context) && (scr & SCR_TE) && (scr & SCR_RE);

   /* ---- transmitter ---- */
   if (scr & SCR_TE)
   {
      if (!st->tx_busy)
      {
         st->tx_left = 0;
         if (frame != 0)
            SCITxLoad(context, st, frame);
      }
      else if (frame != 0)
      {
         st->tx_left -= elapsed;
      }

      /* Several frames may end in one call (long elapsed, fast bit rate). */
      while (st->tx_busy && frame != 0 && st->tx_left <= 0)
      {
         SCIDeviceTransmit(context, st->tsr);

         /* Same 8 clocks shift a character in (figure 13.20). */
         if (full_duplex && !(context->onchip.SSR & SSR_ERRORS))
         {
            u8 data;
            const int got = SCIDeviceReceive(context, &data);
            SCIReceiveDone(context, data, got);
         }

         st->tx_busy = 0;
         if (!SCITxLoad(context, st, frame))
         {
            st->tx_left = 0;
            context->onchip.SSR |= SSR_TEND;
            SH2EvaluateInterrupt(context); /* TEI */
         }
      }
   }
   else
   {
      st->tx_busy = 0;
      st->tx_left = 0;
   }

   /* ---- receiver ---- */
   if (!(scr & SCR_RE) || (context->onchip.SSR & SSR_ERRORS) || full_duplex || frame == 0)
   {
      if (!(scr & SCR_RE) || frame == 0)
         st->rx_busy = 0;
      return;
   }

   if (sync && !SCIExternalClock(context))
   {
      /* Receive only, SCI clock: clocks one character after the other. */
      if (!st->rx_busy)
      {
         st->rx_busy = 1;
         st->rx_left = (s32)frame;
      }
      else
         st->rx_left -= elapsed;

      while (st->rx_busy && st->rx_left <= 0 && !(context->onchip.SSR & SSR_ERRORS))
      {
         u8 data;
         const int got = SCIDeviceReceive(context, &data);
         SCIReceiveDone(context, data, got);
         st->rx_left += (s32)frame;
      }
      return;
   }

   /* Asynchronous, or clocked synchronous with the device clock: the
      device starts a character when it has one. */
   if (st->rx_busy)
   {
      st->rx_left -= elapsed;
      if (st->rx_left <= 0)
      {
         st->rx_busy = 0;
         SCIReceiveDone(context, st->rsr, 1);
      }
   }
   if (!st->rx_busy && !(context->onchip.SSR & SSR_ERRORS))
   {
      u8 data;
      if (SCIDeviceReceive(context, &data))
      {
         st->rsr = data;
         st->rx_busy = 1;
         st->rx_left = (s32)frame;
      }
   }
}

//////////////////////////////////////////////////////////////////////////////
// Interface with sh2core.c
//////////////////////////////////////////////////////////////////////////////

/* Instrumentation: "probe" device, plugged in when the environment variable
   KRONOS_SCI_PROBE is set to a bit time in SH-2 cycles (e.g. 64). It drives
   SCK at that rate when the SCI expects an external clock and never sends
   anything (idle line), so that a driver waiting for an external clock
   still sends its first characters and they show up in the trace. Not a
   real device: what the driver does next without the expected replies is
   only a hint. */
static u32 sci_probe_bit = 0;

static void SCIProbeReset(void) { }
static void SCIProbeTransmit(int cpu, u8 data, int sync) { (void)cpu; (void)data; (void)sync; }
static int SCIProbeReceive(int cpu, u8 * data, int sync) { (void)cpu; (void)sync; *data = 0xFF; return 0; }
static u32 SCIProbeExternalBitCycles(int cpu) { (void)cpu; return sci_probe_bit; }

static CommPortDevice SCIProbeDevice = {
   "SCI probe (instrumentation)",
   SCIProbeReset,
   SCIProbeTransmit,
   SCIProbeReceive,
   SCIProbeExternalBitCycles
};

static void SCIProbeInit(SH2_struct * context)
{
   static int done = 0;
   const char * v;
   if (done)
      return;
   done = 1;
   v = getenv("KRONOS_SCI_PROBE");
   if (v != NULL && atoi(v) > 0 && commport == NULL)
   {
      sci_probe_bit = (u32)atoi(v);
      commport = &SCIProbeDevice;
      SCITrace(context, "probe device plugged in, external clock %u cycles/bit", (unsigned)sci_probe_bit);
   }
}

void SCISetCommPortDevice(CommPortDevice * dev)
{
   commport = dev;
   if (commport != NULL && commport->Reset != NULL)
      commport->Reset();
}

CommPortDevice * SCIGetCommPortDevice(void)
{
   return commport;
}

void SCIReset(SH2_struct * context)
{
   SciState * st = &sci[SCIIndex(context)];
   SCIProbeInit(context);
   memset(st, 0, sizeof(*st));
   st->last_cycles = context->cycles;
   if (context == MSH2 && commport != NULL && commport->Reset != NULL)
      commport->Reset();
}

void SCIExec(SH2_struct * context)
{
   SciState * st = &sci[SCIIndex(context)];
   const u32 now = context->cycles;
   u32 elapsed;

   /* context->cycles goes back to 0 at each frame (YabauseEmulate) and on
      a reset. */
   if (now < st->last_cycles)
      st->last_cycles = 0;
   elapsed = now - st->last_cycles;
   st->last_cycles = now;

   if (!(context->onchip.SCR & (SCR_TE | SCR_RE)))
      return;
   if (elapsed > 0x7FFFFFFF)
      elapsed = 0x7FFFFFFF;
   SCIProcess(context, (s32)elapsed);
}

u8 SCIReadSSR(SH2_struct * context)
{
   SciState * st = &sci[SCIIndex(context)];
   SCIExec(context);
   st->ssr_read |= context->onchip.SSR & SSR_CLEARABLE;
   return context->onchip.SSR;
}

u8 SCIReadRDR(SH2_struct * context)
{
   SCIExec(context);
   return context->onchip.RDR;
}

void SCIWriteSMR(SH2_struct * context, u8 val)
{
   SCIExec(context);
   context->onchip.SMR = val;
   SCITrace(context, "SMR <- %02X (%s, %s, CKS=phi/%d)", val,
            (val & SMR_CA) ? "clocked synchronous" : "asynchronous",
            (val & SMR_CA) ? "8 bits" :
            ((val & SMR_MP) ? "multiprocessor" : ((val & SMR_PE) ? ((val & SMR_OE) ? "odd parity" : "even parity") : "no parity")),
            4 << (2 * (val & 3)));
}

void SCIWriteBRR(SH2_struct * context, u8 val)
{
   SCIExec(context);
   context->onchip.BRR = val;
   SCITrace(context, "BRR <- %02X", val);
}

void SCIWriteSCR(SH2_struct * context, u8 val)
{
   SciState * st = &sci[SCIIndex(context)];
   const u8 old = context->onchip.SCR;

   SCIExec(context);
   context->onchip.SCR = val;

   /* TE = 0: TDRE locked at 1, TEND = 1, TSR initialised (13.2.6/13.2.7). */
   if (!(val & SCR_TE))
   {
      context->onchip.SSR |= SSR_TDRE | SSR_TEND;
      st->tx_busy = 0;
      st->tx_left = 0;
   }
   /* RE = 0: reception stops, the flags keep their values. */
   if (!(val & SCR_RE))
      st->rx_busy = 0;
   /* RE 0 -> 1: a device that waited for it may send its next frame. */
   if ((val & SCR_RE) && !(old & SCR_RE))
      st->rx_wait_re = 0;
   if ((val ^ old) & (SCR_TE | SCR_RE))
      st->last_cycles = context->cycles;

   SCITrace(context, "SCR <- %02X (TIE=%d RIE=%d TE=%d RE=%d TEIE=%d CKE=%d%d, bit=%u cycles)",
            val, !!(val & SCR_TIE), !!(val & SCR_RIE), !!(val & SCR_TE), !!(val & SCR_RE),
            !!(val & SCR_TEIE), !!(val & SCR_CKE1), !!(val & SCR_CKE0),
            (unsigned)SCIBitCycles(context));

   SCIProcess(context, 0);
   SH2EvaluateInterrupt(context);
}

void SCIWriteTDR(SH2_struct * context, u8 val)
{
   SCIExec(context);
   context->onchip.TDR = val;
}

void SCIWriteSSR(SH2_struct * context, u8 val)
{
   SciState * st = &sci[SCIIndex(context)];
   u8 ssr;
   u8 clear;

   SCIExec(context);
   ssr = context->onchip.SSR;

   /* Only a 0 can be written, and only to a flag read as 1 before. */
   clear = (u8)(~val & st->ssr_read & SSR_CLEARABLE & ssr);
   /* TE = 0: TDRE is locked at 1. */
   if (!(context->onchip.SCR & SCR_TE))
      clear &= ~SSR_TDRE;
   ssr &= ~clear;
   st->ssr_read &= ~clear;

   /* TEND is cleared with TDRE (13.2.7, bit 2). */
   if (clear & SSR_TDRE)
      ssr &= ~SSR_TEND;

   /* MPBT is a plain read/write bit; TEND and MPB are read-only. */
   ssr = (u8)((ssr & ~SSR_MPBT) | (val & SSR_MPBT));
   context->onchip.SSR = ssr;

   if (clear & ~SSR_RDRF)
      SCITrace(context, "SSR <- %02X (clears%s%s%s%s), TDR = %02X", val,
               (clear & SSR_TDRE) ? " TDRE" : "", (clear & SSR_ORER) ? " ORER" : "",
               (clear & SSR_FER) ? " FER" : "", (clear & SSR_PER) ? " PER" : "",
               context->onchip.TDR);

   if ((clear & SSR_TDRE) && SCIBitCycles(context) == 0)
      SCITrace(context, "TDR = %02X waiting: no clock on SCK (external clock, nothing drives it)", context->onchip.TDR);

   /* TDRE cleared with TE = 1: TDR goes to TSR as soon as TSR is empty. */
   SCIProcess(context, 0);
   SH2EvaluateInterrupt(context);
}
