/*  Copyright 2003-2005 Guillaume Duhamel
    Copyright 2004-2005, 2013 Theo Berkau

    This file is part of Yabause.

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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file sh2core.c
    \brief SH2 shared emulation functions.
*/

#include <stdlib.h>
#include "sh2core.h"
#include "debug.h"
#include "memory.h"
#include "yabause.h"
#include <scu.h>
#include <error.h>
#include "yui.h"
#include "sh2int_kronos.h"

SH2_struct *SSH2=NULL;
SH2_struct *MSH2=NULL;
SH2Interface_struct *SH2Core=NULL;
extern SH2Interface_struct *SH2CoreList[];

void OnchipReset(SH2_struct *context);
static void FRTExec(SH2_struct *context);
static void WDTExec(SH2_struct *context);
u8 SCIReceiveByte(void);
void SCITransmitByte(u8);


void enableCache(SH2_struct *ctx);
void disableCache(SH2_struct *ctx);
void InvalidateCache(SH2_struct *ctx);

static void (*SH2BlockableExec)(SH2_struct *context, u32 cycles);
static void (*SH2StandardExec)(SH2_struct *context, u32 cycles);

#define CACHE_LOG

void DMATransferCycles(SH2_struct *context, Dmac * dmac, int cycles);
int DMAProc(SH2_struct *context, int cycles );

//////////////////////////////////////////////////////////////////////////////

void SH2IntcSetIrl(SH2_struct *sh, u8 irl, u8 d)
{
  if (sh->intc.irl != irl) {
    sh->intc.d = d;
    sh->intc.irl = irl;
    SH2EvaluateInterrupt(sh);
  }
}
void SH2IntcSetNmi(SH2_struct *sh)
{
  sh->intc.nmi = 0x1;
  SH2EvaluateInterrupt(sh);
}

void SH2EvaluateInterrupt(SH2_struct *sh) {
  if (sh->intPriority != 0) return;
  sh->intVector = 0xFF;

  if (sh->intc.nmi != 0) {
    sh->intVector = 0xB;
    sh->intPriority = 0xF;
    sh->intc.nmi = 0;
  }
  else if ((sh->intc.irl != 0)&&(sh->intc.irl > sh->regs.SR.part.I)) //Test IRL
  {
    //interrupt on IRL, determine the priority
    sh->intPriority = sh->intc.irl;
    if (sh->onchip.ICR & 0x1) {
      sh->intVector = sh->intc.d;
      ScuAcceptInterrupt(sh);
    }
    else {
      sh->intVector = 0x40+(sh->intc.irl>>1);
    }
    sh->intc.irl = 0;
  }
  else if (((sh->onchip.DVCR & 0x3)==0x3) && (((sh->onchip.IPRA >> 12) & 0xF) > sh->regs.SR.part.I)) //DIVU
  {
    sh->intVector = sh->onchip.VCRDIV & 0x7F;
    sh->intPriority = ((sh->onchip.IPRA >> 12) & 0xF);
  }
  else if (((sh->onchip.CHCR0 & 0x6)==0x6) && (((sh->onchip.IPRA & 0xF00) >> 8) > sh->regs.SR.part.I)) //DMAC0
  {
      sh->intVector = sh->onchip.VCRDMA0;
      sh->intPriority = ((sh->onchip.IPRA & 0xF00) >> 8);
  }
  else if (((sh->onchip.CHCR1 & 0x6)==0x6) && (((sh->onchip.IPRA & 0xF00) >> 8) > sh->regs.SR.part.I)) //DMAC1
  {
      sh->intVector = sh->onchip.VCRDMA1;
      sh->intPriority = ((sh->onchip.IPRA & 0xF00) >> 8);
  }
  else if ((sh->wdt.isinterval != 0) && ((sh->onchip.WTCSR & 0x80) != 0) && (((sh->onchip.IPRA >> 4) & 0xF) > sh->regs.SR.part.I)) //WDT
  {
    sh->intVector = (sh->onchip.VCRWDT >> 8) & 0x7F;
    sh->intPriority = ((sh->onchip.IPRA >> 4) & 0xF);
  }
  //BSC not implemented
  /* SCI (SH7095 manual, sec. 5.2.4 and the SCI chapter). Four sources share
   * the SCI priority field, IPRB bits 15-12 (SCIIP3-SCIIP0), and they rank
   * ERI > RXI > TXI > TEI among themselves. Vectors come from VCRA/VCRB:
   *   VCRA 14-8  SERV  receive-error       (ERI)
   *   VCRA  6-0  SRXV  receive-data-full   (RXI)
   *   VCRB 14-8  STXV  transmit-data-empty (TXI)
   *   VCRB  6-0  STEV  transmit-end        (TEI)
   * Enables live in SCR (bit 7 TIE, bit 6 RIE, bit 2 TEIE) and the status
   * flags in SSR (bit 7 TDRE, bit 6 RDRF, bit 5 ORER, bit 4 FER, bit 3 PER,
   * bit 2 TEND).
   *
   * These were previously stubbed out entirely. Note this cannot fire on its
   * own: it needs software to have assigned a non-zero SCI priority in IPRB
   * *and* enabled the source in SCR, so code that never touches the SCI - i.e.
   * essentially every retail Saturn title - is unaffected. */
  else if (((sh->onchip.SCR & 0x40)!=0) && ((sh->onchip.SSR & 0x38)!=0) && (((sh->onchip.IPRB >> 12) & 0xF) > sh->regs.SR.part.I)) //SCI ERI
  {
    sh->intVector = (sh->onchip.VCRA >> 8) & 0x7F;
    sh->intPriority = ((sh->onchip.IPRB >> 12) & 0xF);
  }
  else if (((sh->onchip.SCR & 0x40)!=0) && ((sh->onchip.SSR & 0x40)!=0) && (((sh->onchip.IPRB >> 12) & 0xF) > sh->regs.SR.part.I)) //SCI RXI
  {
    sh->intVector = sh->onchip.VCRA & 0x7F;
    sh->intPriority = ((sh->onchip.IPRB >> 12) & 0xF);
  }
  else if (((sh->onchip.SCR & 0x80)!=0) && ((sh->onchip.SSR & 0x80)!=0) && (((sh->onchip.IPRB >> 12) & 0xF) > sh->regs.SR.part.I)) //SCI TXI
  {
    sh->intVector = (sh->onchip.VCRB >> 8) & 0x7F;
    sh->intPriority = ((sh->onchip.IPRB >> 12) & 0xF);
  }
  else if (((sh->onchip.SCR & 0x04)!=0) && ((sh->onchip.SSR & 0x04)!=0) && (((sh->onchip.IPRB >> 12) & 0xF) > sh->regs.SR.part.I)) //SCI TEI
  {
    sh->intVector = sh->onchip.VCRB & 0x7F;
    sh->intPriority = ((sh->onchip.IPRB >> 12) & 0xF);
  }
  else if (((sh->onchip.TIER & 0x80)!=0) && ((sh->onchip.FTCSR & 0x80)!=0) && (((sh->onchip.IPRB >> 8) & 0xF) > sh->regs.SR.part.I)) //FRT ICI
  {
    sh->intVector = (sh->onchip.VCRC >> 8) & 0x7F;
    sh->intPriority = ((sh->onchip.IPRB >> 8) & 0xF);
  }
  else if (((sh->onchip.TIER & 0x8)!=0) && ((sh->onchip.FTCSR & 0x8)!=0) && (((sh->onchip.IPRB >> 8) & 0xF) > sh->regs.SR.part.I)) //FRT OCIA
  {
     sh->intVector = sh->onchip.VCRC & 0x7F;
     sh->intPriority = ((sh->onchip.IPRB >> 8) & 0xF);
  }
  else if (((sh->onchip.TIER & 0x4)!=0) && ((sh->onchip.FTCSR & 0x4)!=0) && (((sh->onchip.IPRB >> 8) & 0xF) > sh->regs.SR.part.I)) //FRT OCIB
  {
     /* Output Compare B shares the FRT priority (IPRB 11-8) and the output
      * compare vector FOCV (VCRC bits 6-0) with OCIA. FRTExec already sets
      * OCFB (FTCSR bit 2) on a compare-match B, but the interrupt source was
      * missing here, so OCIB-driven interrupts were never raised. */
     sh->intVector = sh->onchip.VCRC & 0x7F;
     sh->intPriority = ((sh->onchip.IPRB >> 8) & 0xF);
  }
  else if (((sh->onchip.TIER & 0x2)!=0) && ((sh->onchip.FTCSR & 0x2)!=0) && (((sh->onchip.IPRB >> 8) & 0xF) > sh->regs.SR.part.I)) //FRT OVI
  {
     sh->intVector = (sh->onchip.VCRD >> 8) & 0x7F;
     sh->intPriority = ((sh->onchip.IPRB >> 8) & 0xF);
  }
  if (sh->intPriority != 0x0) {
    // force the next PC to be decodeWithInterrupt so that interrupt is evaluated asap
    if (SH2Core->notifyInterrupt != NULL) SH2Core->notifyInterrupt(sh);
  }
}


static void SH2StandardExecFast(SH2_struct *context, u32 cycles) {
  SH2Core->Exec(context, cycles);
}

static void SH2StandardExecDebug(SH2_struct *context, u32 cycles) {
  int oldbp = context->bp.inbreakpoint;
  SH2Core->Exec(context, cycles);
  if (context->bp.inbreakpoint && !oldbp) {
    context->bp.BreakpointCallBack(context, 0, &context->bp.BreakpointUserData);
    context->bp.inbreakpoint = 0;
  }
}

static sh2regs_struct oldRegs;
static void SH2BlockableExecDebug(SH2_struct *context, u32 cycles) {
  if (context->isBlocked == 0) {
    int oldbp = context->bp.inbreakpoint;
    SH2Core->ExecSave(context, cycles, &oldRegs);
    if (context->bp.inbreakpoint && !oldbp) {
      context->bp.BreakpointCallBack(context, 0, &context->bp.BreakpointUserData);
      context->bp.inbreakpoint = 0;
    }
  } else {
    context->cycles += cycles;
  }
}
static void SH2BlockableExecFast(SH2_struct *context, u32 cycles) {
  if (context->isBlocked == 0) {
    SH2Core->ExecSave(context, cycles, &oldRegs);
  } else {
    context->cycles += cycles;
  }
}

void SH2SetExecSet(int debug) {
  if (debug == 0) {
    SH2BlockableExec = SH2BlockableExecFast;
    SH2StandardExec = SH2StandardExecFast;
  } else {
    SH2BlockableExec = SH2BlockableExecDebug;
    SH2StandardExec = SH2StandardExecDebug;
  }
}

void SH2UpdateABusAccess(SH2_struct *context, int on) {
  if (context->isAccessingCPUBUS != on) {
    context->isAccessingCPUBUS = on;
    SH2UpdateBlockedState(context);
  }
}

void SH2SetVRamAccess(SH2_struct *context, int mask) {
  if (!(context->isAccessingVram & mask)) {
    context->isAccessingVram |= mask;
    SH2UpdateBlockedState(context);
  }
}
void SH2ClearVRamAccess(SH2_struct *context, int mask) {
  if (context->isAccessingVram & mask) {
    context->isAccessingVram &= ~mask;
    SH2UpdateBlockedState(context);
  }
}

static int isDMABlocked(SH2_struct *context) {
  return (context->isAccessingCPUBUS != 0)&&((context->blockingMask & A_BUS_ACCESS)!=0);
}

void SH2UpdateBlockedState(SH2_struct *context){
  context->isBlocked =  (context->isAccessingCPUBUS != 0)||((context->blockingMask & A_BUS_ACCESS)!=0);
  context->isBlocked |= ((context->isAccessingVram & context->blockingMask)!=0);
}

void SH2SetCPUConcurrency(SH2_struct *context, u8 mask) {
  if ((context->SH2InterruptibleExec != SH2BlockableExec) || !(context->blockingMask & mask)) {
    context->blockingMask |= mask;
    if (context->blockingMask != 0) context->SH2InterruptibleExec = SH2BlockableExec;
    if (mask == A_BUS_ACCESS) SH2UpdateABusAccess(context, 0);
    else SH2ClearVRamAccess(context, mask);
  }
}

void SH2ClearCPUConcurrency(SH2_struct *context, u8 mask) {
  if ((context->SH2InterruptibleExec != SH2StandardExec) && (context->blockingMask & mask)) {
    context->blockingMask &= ~mask;
    if (context->blockingMask == 0) context->SH2InterruptibleExec = SH2StandardExec;
    if (mask == A_BUS_ACCESS) SH2UpdateABusAccess(context, 0);
    else SH2ClearVRamAccess(context, mask);
  }
}

int SH2Init(int coreid)
{
   int i;
   // MSH2
   if ((MSH2 = (SH2_struct *)calloc(1, sizeof(SH2_struct))) == NULL)
      return -1;
  MSH2->SH2InterruptibleExec = SH2StandardExec;

   if (SH2TrackInfLoopInit(MSH2) != 0)
      return -1;

#ifdef SH2_HANG_WATCH
   if (SH2HangWatchInit(MSH2) != 0)
      return -1;
#endif

   MSH2->onchip.BCR1 = 0x0000;
   MSH2->isslave = 0;
   MSH2->isAccessingCPUBUS = 0;
   MSH2->interruptReturnAddress = 0;
   MSH2->isAccessingVram = 0;
   MSH2->isBlocked = 0;

    MSH2->dma_ch0.CHCR = &MSH2->onchip.CHCR0;
    MSH2->dma_ch0.CHCRM = &MSH2->onchip.CHCR0M;
    MSH2->dma_ch0.SAR = &MSH2->onchip.SAR0;
    MSH2->dma_ch0.DAR = &MSH2->onchip.DAR0;
    MSH2->dma_ch0.TCR = &MSH2->onchip.TCR0;
    MSH2->dma_ch0.VCRDMA = &MSH2->onchip.VCRDMA0;
    MSH2->dma_ch1.CHCR = &MSH2->onchip.CHCR1;
    MSH2->dma_ch1.CHCRM = &MSH2->onchip.CHCR1M;
    MSH2->dma_ch1.SAR = &MSH2->onchip.SAR1;
    MSH2->dma_ch1.DAR = &MSH2->onchip.DAR1;
    MSH2->dma_ch1.TCR = &MSH2->onchip.TCR1;
    MSH2->dma_ch1.VCRDMA = &MSH2->onchip.VCRDMA1;

   // SSH2
   if ((SSH2 = (SH2_struct *)calloc(1, sizeof(SH2_struct))) == NULL)
      return -1;

  SSH2->SH2InterruptibleExec = SH2StandardExec;

   if (SH2TrackInfLoopInit(SSH2) != 0)
      return -1;

#ifdef SH2_HANG_WATCH
   if (SH2HangWatchInit(SSH2) != 0)
      return -1;
#endif

    SSH2->interruptReturnAddress = 0;
    SSH2->onchip.BCR1 = 0x8000;
    SSH2->isslave = 1;
    SSH2->isAccessingCPUBUS = 0;
    SSH2->isAccessingVram = 0;
    SSH2->isBlocked = 0;

    SSH2->dma_ch0.CHCR = &SSH2->onchip.CHCR0;
    SSH2->dma_ch0.CHCRM = &SSH2->onchip.CHCR0M;
    SSH2->dma_ch0.SAR = &SSH2->onchip.SAR0;
    SSH2->dma_ch0.DAR = &SSH2->onchip.DAR0;
    SSH2->dma_ch0.TCR = &SSH2->onchip.TCR0;
    SSH2->dma_ch0.VCRDMA = &SSH2->onchip.VCRDMA0;
    SSH2->dma_ch1.CHCR = &SSH2->onchip.CHCR1;
    SSH2->dma_ch1.CHCRM = &SSH2->onchip.CHCR1M;
    SSH2->dma_ch1.SAR = &SSH2->onchip.SAR1;
    SSH2->dma_ch1.DAR = &SSH2->onchip.DAR1;
    SSH2->dma_ch1.TCR = &SSH2->onchip.TCR1;
    SSH2->dma_ch1.VCRDMA = &SSH2->onchip.VCRDMA1;

   MSH2->cacheOn = 0;
   SSH2->cacheOn = 0;

#ifdef USE_CACHE
   memset(MSH2->tagWay, 0x4, 64*0x80000);
   memset(MSH2->cacheTagArray, 0x0, 64*4*sizeof(u32));
   memset(SSH2->tagWay, 0x4, 64*0x80000);
   memset(SSH2->cacheTagArray, 0x0, 64*4*sizeof(u32));
#endif
   // So which core do we want?
   if (coreid == SH2CORE_DEFAULT)
      coreid = 0; // Assume we want the first one

   // Go through core list and find the id
   for (i = 0; SH2CoreList[i] != NULL; i++)
   {
      if (SH2CoreList[i]->id == coreid)
      {
         // Set to current core
         SH2Core = SH2CoreList[i];
         break;
      }
   }

   if ((SH2Core == NULL) || (SH2Core->Init() != 0)) {
      free(MSH2);
      free(SSH2);
      MSH2 = SSH2 = NULL;
      return -1;
   }

#ifdef SH2_HANG_WATCH
   /* The watchdog reads the backward-branch counters, which only the debug
      interpreter maintains, so arm it there and leave the fast core alone.
      Set KRONOS_NO_HANGWATCH in the environment to opt out. */
   if (SH2Core->id == SH2CORE_KRONOS_DEBUG_INTERPRETER && getenv("KRONOS_NO_HANGWATCH") == NULL)
   {
      SH2HangWatchStart(MSH2);
      SH2HangWatchStart(SSH2);
   }
#endif

   SH2Reset(MSH2);
   SH2Reset(SSH2);

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void SH2DeInit()
{
   if (SH2Core)
      SH2Core->DeInit();
   SH2Core = NULL;

   if (MSH2)
   {
      SH2TrackInfLoopDeInit(MSH2);
#ifdef SH2_HANG_WATCH
      SH2HangWatchDeInit(MSH2);
#endif
      free(MSH2);
   }
   MSH2 = NULL;

   if (SSH2)
   {
      SH2TrackInfLoopDeInit(SSH2);
#ifdef SH2_HANG_WATCH
      SH2HangWatchDeInit(SSH2);
#endif
      free(SSH2);
   }
   SSH2 = NULL;
}

//////////////////////////////////////////////////////////////////////////////

void SH2Reset(SH2_struct *context)
{
   int i;
CACHE_LOG("%s reset\n", (context==SSH2)?"SSH2":"MSH2" );
   context->SH2InterruptibleExec = SH2StandardExec;
   SH2Core->Reset(context);

   // Reset general registers
   for (i = 0; i < 15; i++)
      SH2Core->SetGPR(context, i, 0x00000000);

   SH2Core->SetSR(context, 0x000000F0);
   SH2Core->SetGBR(context, 0x00000000);
   SH2Core->SetVBR(context, 0x00000000);
   SH2Core->SetMACH(context, 0x00000000);
   SH2Core->SetMACL(context, 0x00000000);
   SH2Core->SetPR(context, 0x00000000);

   // Internal variables
   context->target_cycles = 0x00000000;
   context->cycles = 0;
   context->divcycles = 0;
   context->frtcycles = 0;
   context->wdtcycles = 0;

   context->frc.leftover = 0;
   context->frc.shift = 3;

   context->wdt.isenable = 0;
   context->wdt.isinterval = 1;
   context->wdt.shift = 1;
   context->wdt.leftover = 0;

   context->cycleFrac = 0;

   // Reset Onchip modules
   OnchipReset(context);
   InvalidateCache(context);

   // Reset backtrace
   context->bt.numbacktrace = 0;

   SH2EvaluateInterrupt(context);

#ifdef DMPHISTORY
   memset(context->pchistory, 0, sizeof(context->pchistory));
   context->pchistory_index = 0;
#endif

#ifdef SH2_HANG_WATCH
   SH2HangWatchClear(context);
#endif
}

//////////////////////////////////////////////////////////////////////////////

void SH2PowerOn(SH2_struct *context) {
   /* A power-on fetches the reset vector from addresses 0 and 4, with VBR
    * forced to zero - that is what the hardware does. Reading it at the
    * current VBR only happens to work while VBR is still zero, which is not
    * the case when a CPU that has already been running is restarted. */
   SH2Core->SetVBR(context, 0x00000000);
   SH2Core->SetPC(context, SH2MappedMemoryReadLong(context, 0x00000000));
   SH2Core->SetGPR(context, 15, SH2MappedMemoryReadLong(context, 0x00000004));
   CACHE_LOG("%s start\n", (context==SSH2)?"SSH2":"MSH2" );
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL SH2TestExec(SH2_struct *context, u32 cycles)
{
   SH2Core->TestExec(context, cycles);
}

void FASTCALL SH2Exec(SH2_struct *context, u32 cycles)
{
   int sh2start = context->cycles;
   context->SH2InterruptibleExec(context, cycles);
   FRTExec(context);
   WDTExec(context);
   /* WDTExec() can now reset the CPU (watchdog-mode overflow with RSTE set),
    * and SH2Reset() zeroes context->cycles. Guard the elapsed-cycle
    * computation so DMAProc is never handed a negative count. */
   if ((int)context->cycles >= sh2start)
      DMAProc(context, context->cycles-sh2start);
}

void FASTCALL SH2OnFrame(SH2_struct *context) {
  SH2Core->OnFrame(context);
}
//////////////////////////////////////////////////////////////////////////////


void SH2Step(SH2_struct *context)
{
   if (SH2Core)
   {
      u32 tmp = SH2Core->GetPC(context);

      // Execute 1 instruction
      SH2Exec(context, 1);

      // Sometimes it doesn't always execute one instruction,
      // let's make sure it did
      if (tmp == SH2Core->GetPC(context))
         SH2Exec(context, 1);
   }
}

//////////////////////////////////////////////////////////////////////////////

int SH2StepOver(SH2_struct *context, void (*func)(void *, u32, void *))
{
   if (SH2Core)
   {
      u32 tmp = SH2Core->GetPC(context);
      u16 inst= SH2MappedMemoryReadWord(context, context->regs.PC);

      // If instruction is jsr, bsr, or bsrf, step over it
      if ((inst & 0xF000) == 0xB000 || // BSR
         (inst & 0xF0FF) == 0x0003 || // BSRF
         (inst & 0xF0FF) == 0x400B)   // JSR
      {
         // Set breakpoint after at PC + 4
         context->stepOverOut.callBack = func;
         context->stepOverOut.type = SH2ST_STEPOVER;
         context->stepOverOut.enabled = 1;
         context->stepOverOut.address = context->regs.PC+4;
         return 1;
      }
      else
      {
         // Execute 1 instruction instead
         SH2Exec(context, 1);

         // Sometimes it doesn't always execute one instruction,
         // let's make sure it did
         if (tmp == SH2Core->GetPC(context))
            SH2Exec(context, 1);
      }
   }
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void SH2StepOut(SH2_struct *context, void (*func)(void *, u32, void *))
{
   if (SH2Core)
   {
      context->stepOverOut.callBack = func;
      context->stepOverOut.type = SH2ST_STEPOUT;
      context->stepOverOut.enabled = 1;
      context->stepOverOut.address = 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

int SH2TrackInfLoopInit(SH2_struct *context)
{
   context->trackInfLoop.maxNum = 100;
   if ((context->trackInfLoop.match = calloc(context->trackInfLoop.maxNum, sizeof(tilInfo_struct))) == NULL)
      return -1;

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void SH2TrackInfLoopDeInit(SH2_struct *context)
{
   if (context->trackInfLoop.match)
      free(context->trackInfLoop.match);
}

//////////////////////////////////////////////////////////////////////////////

void SH2TrackInfLoopStart(SH2_struct *context)
{
   context->trackInfLoop.enabled = 1;
}

//////////////////////////////////////////////////////////////////////////////

void SH2TrackInfLoopStop(SH2_struct *context)
{
   context->trackInfLoop.enabled = 0;
}

//////////////////////////////////////////////////////////////////////////////

void SH2TrackInfLoopClear(SH2_struct *context)
{
   memset(context->trackInfLoop.match, 0, sizeof(tilInfo_struct) * context->trackInfLoop.maxNum);
   context->trackInfLoop.num = 0;
}

//////////////////////////////////////////////////////////////////////////////

void SH2FormatRegs(SH2_struct *context, char *buf, int size)
{
   sh2regs_struct r;

   SH2GetRegisters(context, &r);

   snprintf(buf, size,
            "R0  = %08lX\tR12  = %08lX\n"
            "R1  = %08lX\tR13  = %08lX\n"
            "R2  = %08lX\tR14  = %08lX\n"
            "R3  = %08lX\tR15  = %08lX\n"
            "R4  = %08lX\tSR   = %08lX\n"
            "R5  = %08lX\tGBR  = %08lX\n"
            "R6  = %08lX\tVBR  = %08lX\n"
            "R7  = %08lX\tMACH = %08lX\n"
            "R8  = %08lX\tMACL = %08lX\n"
            "R9  = %08lX\tPR   = %08lX\n"
            "R10 = %08lX\tPC   = %08lX\n"
            "R11 = %08lX\n",
            (long)r.R[0],  (long)r.R[12],
            (long)r.R[1],  (long)r.R[13],
            (long)r.R[2],  (long)r.R[14],
            (long)r.R[3],  (long)r.R[15],
            (long)r.R[4],  (long)r.SR.all,
            (long)r.R[5],  (long)r.GBR,
            (long)r.R[6],  (long)r.VBR,
            (long)r.R[7],  (long)r.MACH,
            (long)r.R[8],  (long)r.MACL,
            (long)r.R[9],  (long)r.PR,
            (long)r.R[10], (long)r.PC,
            (long)r.R[11]);
}

//////////////////////////////////////////////////////////////////////////////

#ifdef SH2_TRAP_ADDRESS_ERROR

#define ADDRESS_ERROR_MAX_REPORTS (16)

static int addressErrorFatal = 0;
static int addressErrorBusy = 0;   /* the stack pushes below re-enter the
                                      memory accessors; do not recurse */
static int addressErrorCount = 0;

void SH2SetAddressErrorFatal(int fatal)
{
   addressErrorFatal = fatal;
}

int SH2GetAddressErrorFatal(void)
{
   return addressErrorFatal;
}

void SH2AddressError(SH2_struct *context, u32 addr, int width, int isWrite)
{
   char regs[512];

   if (context == NULL || addressErrorBusy)
      return;

   addressErrorBusy = 1;

   /* A game looping on a bad pointer would otherwise flood the log with the
      same fault a few hundred thousand times a second. YuiMsg rather than
      YuiErrorMsg for the same reason: this goes to the log, it does not raise
      a dialog. */
   if (addressErrorCount < ADDRESS_ERROR_MAX_REPORTS)
   {
      addressErrorCount++;
      SH2FormatRegs(context, regs, sizeof(regs));
      YuiMsg("%s SH2 address error: %s%d @ %08lX (PC=%08lX PR=%08lX)\n\n%s\n",
             context->isslave ? "Slave" : "Master",
             isWrite ? "W" : "R", width,
             (unsigned long)addr,
             (unsigned long)context->regs.PC,
             (unsigned long)context->regs.PR,
             regs);
#ifdef DMPHISTORY
      SH2DumpHistory(context);
#endif
      if (addressErrorCount == ADDRESS_ERROR_MAX_REPORTS)
         YuiMsg("further SH2 address errors suppressed\n");
   }

   if (addressErrorFatal)
   {
      /* SH7095 sec. 4.5 and table 4.11: an address error pushes SR then PC,
         PC being the start address of the instruction that caused it - the
         same convention as the general illegal instruction, not the +2 used
         for interrupts. */
      context->regs.R[15] -= 4;
      SH2MappedMemoryWriteLong(context, context->regs.R[15], context->regs.SR.all);
      context->regs.R[15] -= 4;
      SH2MappedMemoryWriteLong(context, context->regs.R[15], context->regs.PC);
      context->regs.PC = SH2MappedMemoryReadLong(context, context->regs.VBR + (9 << 2));
   }

   addressErrorBusy = 0;
}

#endif /* SH2_TRAP_ADDRESS_ERROR */

//////////////////////////////////////////////////////////////////////////////

#ifdef SH2_HANG_WATCH

/* A frame counts as suspicious when a single backward branch accounts for
   almost every backward branch taken during that frame. HANG_RATIO is that
   share in percent, HANG_MIN_BRANCHES keeps it quiet on a frame where the CPU
   barely ran, and HANG_FRAMES is how long it has to last before reporting -
   two seconds is well past any legitimate load or fade. */
#define HANG_RATIO         (95)
#define HANG_MIN_BRANCHES  (2000)
#define HANG_FRAMES        (120)
#define HANG_MAX_POLL      (6)

int SH2HangWatchInit(SH2_struct *context)
{
   context->hangWatch.prevNum = context->trackInfLoop.maxNum;
   if ((context->hangWatch.prev = calloc(context->hangWatch.prevNum, sizeof(u64))) == NULL)
      return -1;
   return 0;
}

void SH2HangWatchDeInit(SH2_struct *context)
{
   if (context->hangWatch.prev)
      free(context->hangWatch.prev);
   context->hangWatch.prev = NULL;
   context->hangWatch.prevNum = 0;
}

void SH2HangWatchClear(SH2_struct *context)
{
   if (context->hangWatch.prev)
      memset(context->hangWatch.prev, 0, sizeof(u64) * context->hangWatch.prevNum);
   context->hangWatch.armed = 0;
   context->hangWatch.reported = 0;
   context->hangWatch.frames = 0;
   context->hangWatch.hotAddr = 0;
   context->hangWatch.hotDelta = 0;
   context->hangWatch.lastTotal = 0;
   context->hangWatch.pollIdx = 0;
   memset(context->hangWatch.pollAddr, 0, sizeof(context->hangWatch.pollAddr));
   memset(context->hangWatch.pollPC, 0, sizeof(context->hangWatch.pollPC));
}

void SH2HangWatchStart(SH2_struct *context)
{
   SH2HangWatchClear(context);
   context->hangWatch.enabled = 1;
   /* the detector reads the backward-branch counters, so it needs them */
   SH2TrackInfLoopStart(context);
}

void SH2HangWatchStop(SH2_struct *context)
{
   context->hangWatch.enabled = 0;
   context->hangWatch.armed = 0;
}

void SH2HangWatchLogRead(SH2_struct *context, u32 addr)
{
   u32 i;
   u32 slot;

   /* One entry per distinct address: a loop polling the same register a
      million times must not flush everything else out of the buffer. */
   for (i = 0; i < SH2_POLL_LOG; i++)
      if (context->hangWatch.pollAddr[i] == addr)
         return;

   slot = context->hangWatch.pollIdx;
   context->hangWatch.pollAddr[slot] = addr;
   context->hangWatch.pollPC[slot] = context->regs.PC;
   context->hangWatch.pollIdx = (slot + 1) % SH2_POLL_LOG;
}

/* How many distinct addresses the loop read. A genuine wait loop touches one
   or two; a loop doing real work touches a dozen and is not hung. */
static int SH2HangWatchPollCount(SH2_struct *context)
{
   int i, n = 0;
   for (i = 0; i < SH2_POLL_LOG; i++)
      if (context->hangWatch.pollAddr[i] != 0)
         n++;
   return n;
}

/* Names the thing behind a polled address. A wait loop is only readable once
   you know whether it is spinning on the CD block, the SMPC, a VDP status bit,
   its own free-running timer or a plain RAM flag. */
static const char *SH2HangWatchWhat(u32 addr)
{
   u32 a = addr & 0x0FFFFFFF;

   if (addr >= 0xFFFFFE00)
   {
      switch (addr)
      {
      case 0xFFFFFE10: return "SH2 TIER  (FRT interrupt enable)";
      case 0xFFFFFE11: return "SH2 FTCSR (FRT status, bit7 = input capture)";
      case 0xFFFFFE12:
      case 0xFFFFFE13: return "SH2 FRC   (free-running counter)";
      case 0xFFFFFE92: return "SH2 CCR   (cache control)";
      }
      return "SH2 on-chip register";
   }

   switch (a >> 20)
   {
   case 0x000: return "boot ROM";
   case 0x001: if (a == 0x0010007F) return "SMPC SF (command busy flag)";
               if (a >= 0x00100021 && a <= 0x0010005F) return "SMPC OREG (INTBACK result)";
               return "SMPC";
   case 0x002: return "Work RAM-L";
   case 0x058:
   case 0x059: if (a == 0x00589008) return "CD block HIRQ";
               return "CD block";
   case 0x05A:
   case 0x05B: return "SCSP / sound RAM";
   case 0x05C:
   case 0x05D: if (a == 0x005D0010) return "VDP1 EDSR (draw end status)";
               return "VDP1";
   case 0x05E:
   case 0x05F: if (a == 0x005F8004) return "VDP2 TVSTAT (bit3 VBLANK, bit2 HBLANK)";
               if (a == 0x005FE0A4) return "SCU IST (interrupt status)";
               if (a == 0x005FE0A0) return "SCU IMS (interrupt mask)";
               if (a >= 0x005FE000) return "SCU register";
               return "VDP2";
   default: break;
   }

   if (a >= 0x06000000 && a < 0x06100000) return "Work RAM-H";
   if (a >= 0x00200000 && a < 0x00300000) return "Work RAM-L";
   return "unknown area";
}

/* Reads Work RAM straight out of the backing buffer. Going through the normal
   read path would be wrong here: it can clear status bits and would perturb
   the very state we are trying to describe. */
static int SH2HangWatchPeek(u32 addr, u32 *out)
{
   u32 a = addr & 0x0FFFFFFF;

   if (a >= 0x06000000 && a < 0x06100000 && HighWram != NULL)
   {
      *out = T2ReadLong(HighWram, a & 0xFFFFF);
      return 1;
   }
   if (a >= 0x00200000 && a < 0x00300000 && LowWram != NULL)
   {
      *out = T2ReadLong(LowWram, a & 0xFFFFF);
      return 1;
   }
   return 0;
}

/* The dual-CPU idle loop of TECH#28 5.1: the slave masks every interrupt
   through SR and polls the FRT input capture flag until the master signals it.
   A slave with nothing to do sits there permanently. It is the expected state,
   not a hang, and reporting it hides whatever the master is really doing. */
static int SH2HangWatchIsIdleSlave(SH2_struct *context)
{
   int i, n = 0;

   if (!context->isslave)
      return 0;
   if (context->regs.VBR != 0x06000400)
      return 0;
   if (((context->regs.SR.all >> 4) & 0xF) != 0xF)
      return 0;

   for (i = 0; i < SH2_POLL_LOG; i++)
   {
      u32 a = context->hangWatch.pollAddr[i];
      if (a == 0)
         continue;
      if (a != 0xFFFFFE11)
         return 0;
      n++;
   }
   return (n > 0);
}

void SH2HangWatchFormat(SH2_struct *context, char *buf, int size)
{
   char regs[512];
   int i;
   int used;

   SH2FormatRegs(context, regs, sizeof(regs));

   used = snprintf(buf, size,
                   "%s SH2 appears hung\n\n"
                   "Loop at %08lX took %lu of the last %lu backward branches,\n"
                   "for %lu consecutive frames.\n\n%s\nAddresses polled from the loop:\n",
                   context->isslave ? "Slave" : "Master",
                   (unsigned long)context->hangWatch.hotAddr,
                   (unsigned long)context->hangWatch.hotDelta,
                   (unsigned long)context->hangWatch.lastTotal,
                   (unsigned long)context->hangWatch.frames,
                   regs);

   for (i = 0; i < SH2_POLL_LOG && used < size - 1; i++)
   {
      int slot = (context->hangWatch.pollIdx + i) % SH2_POLL_LOG;
      if (context->hangWatch.pollAddr[slot] == 0)
         continue;
      u32 addr = context->hangWatch.pollAddr[slot];
      u32 val;

      if (SH2HangWatchPeek(addr, &val))
         used += snprintf(buf + used, size - used,
                          "  %08lX = %08lX  %s  (read from PC %08lX)\n",
                          (unsigned long)addr, (unsigned long)val,
                          SH2HangWatchWhat(addr),
                          (unsigned long)context->hangWatch.pollPC[slot]);
      else
         used += snprintf(buf + used, size - used,
                          "  %08lX             %s  (read from PC %08lX)\n",
                          (unsigned long)addr, SH2HangWatchWhat(addr),
                          (unsigned long)context->hangWatch.pollPC[slot]);
   }

   if (used < size - 1)
      used += snprintf(buf + used, size - used,
         "\nThe loop exits when one of those values changes. A Work RAM-H\n"
         "address is written by an interrupt handler or by the other SH2;\n"
         "anything else is a hardware status register.\n");
}


void SH2HangWatchFrame(SH2_struct *context)
{
   u64 total = 0;
   u64 bestDelta = 0;
   u32 bestAddr = 0;
   u64 delta;
   int i;
   int n;

   if (!context->hangWatch.enabled || !context->trackInfLoop.enabled)
      return;

   n = context->trackInfLoop.num;

   if (n > context->hangWatch.prevNum)
   {
      u64 *grown = realloc(context->hangWatch.prev, sizeof(u64) * context->trackInfLoop.maxNum);
      if (grown == NULL)
         return;
      memset(grown + context->hangWatch.prevNum, 0,
             sizeof(u64) * (context->trackInfLoop.maxNum - context->hangWatch.prevNum));
      context->hangWatch.prev = grown;
      context->hangWatch.prevNum = context->trackInfLoop.maxNum;
   }

   /* Per-entry delta rather than raw count: a loop that ran hot during boot
      must not keep masking a hang that starts twenty seconds later. */
   for (i = 0; i < n; i++)
   {
      delta = context->trackInfLoop.match[i].count - context->hangWatch.prev[i];
      context->hangWatch.prev[i] = context->trackInfLoop.match[i].count;
      total += delta;
      if (delta > bestDelta)
      {
         bestDelta = delta;
         bestAddr = context->trackInfLoop.match[i].addr;
      }
   }

   context->hangWatch.lastTotal = total;
   context->hangWatch.hotDelta = bestDelta;

   if (total < HANG_MIN_BRANCHES || bestDelta * 100 < total * HANG_RATIO)
   {
      /* healthy frame */
      context->hangWatch.frames = 0;
      context->hangWatch.armed = 0;
      context->hangWatch.reported = 0;
      return;
   }

   if (bestAddr != context->hangWatch.hotAddr)
   {
      /* a different loop than last frame: start counting again */
      context->hangWatch.hotAddr = bestAddr;
      context->hangWatch.frames = 0;
      context->hangWatch.reported = 0;
      context->hangWatch.pollIdx = 0;
      memset(context->hangWatch.pollAddr, 0, sizeof(context->hangWatch.pollAddr));
      memset(context->hangWatch.pollPC, 0, sizeof(context->hangWatch.pollPC));
   }

   context->hangWatch.frames++;

   /* Arm the read logger well before reporting, so that by the time we print,
      the buffer holds the addresses the loop is actually polling. */
   if (context->hangWatch.frames > 2)
      context->hangWatch.armed = 1;

   /* A dominant backward branch is not enough on its own. The outer dispatch
      loop of a busy slave also dominates its frame, and reporting it buries
      the real thing. A loop that reads more than a handful of distinct
      addresses is working, not waiting. */
   if (context->hangWatch.frames >= HANG_FRAMES &&
       SH2HangWatchPollCount(context) > HANG_MAX_POLL)
   {
      context->hangWatch.frames = 0;
      context->hangWatch.armed = 0;
      return;
   }

   /* An idle slave is not a hang: keep watching, but stay quiet. */
   if (context->hangWatch.frames >= HANG_FRAMES && SH2HangWatchIsIdleSlave(context))
   {
      context->hangWatch.reported = 1;
      return;
   }

   if (context->hangWatch.frames >= HANG_FRAMES && !context->hangWatch.reported)
   {
      context->hangWatch.reported = 1;
#ifdef DMPHISTORY
      SH2DumpHistory(context);
#endif
      YabSetError(YAB_ERR_SH2HANG, context);
   }
}

#endif /* SH2_HANG_WATCH */

//////////////////////////////////////////////////////////////////////////////

void SH2HandleStepOverOut(SH2_struct *context)
{
   if (context->stepOverOut.enabled)
   {
      switch ((int)context->stepOverOut.type)
      {
      case SH2ST_STEPOVER: // Step Over
         if (context->regs.PC == context->stepOverOut.address)
         {
            context->stepOverOut.enabled = 0;
            context->stepOverOut.callBack(context, context->regs.PC, (void *)context->stepOverOut.type);
         }
         break;
      case SH2ST_STEPOUT: // Step Out
         {
            u16 inst;
            if ((context->stepOverOut.levels < 0) && (context->regs.PC == context->regs.PR))
            {
               context->stepOverOut.enabled = 0;
               context->stepOverOut.callBack(context, context->regs.PC, (void *)context->stepOverOut.type);
               return;
            }

            inst = context->instruction;;

            if ((inst & 0xF000) == 0xB000 || // BSR
               (inst & 0xF0FF) == 0x0003 || // BSRF
               (inst & 0xF0FF) == 0x400B)   // JSR
               context->stepOverOut.levels++;
            else if (inst == 0x000B || // RTS
                     inst == 0x002B)   // RTE
               context->stepOverOut.levels--;

            break;
         }
      default: break;
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

void SH2HandleTrackInfLoop(SH2_struct *context)
{
   if (context->trackInfLoop.enabled)
   {
      /* Look for branches that go backwards, i.e. loop candidates.
       *
       * The tests used to be written as (op & 0x8B80) == 0x8B80 and friends,
       * which does not isolate the opcode field at all: 0xCF80, that is
       * or.b #imm,@(R0,GBR), satisfies the first one, and
       * (op & 0xA800) == 0xA800 matches every 0xE8xx, that is mov #imm,Rn.
       * The match list filled up with addresses that are not branches.
       *
       * The 8-bit displacement forms are backwards when bit 7 of the
       * displacement is set; bra/bsr carry a 12-bit displacement, so the sign
       * bit is bit 11. jmp/jsr/braf/bsrf are register-relative, so the target
       * has to be computed to know which way they go - worth doing, since a
       * wait loop built around jmp @Rn was previously invisible here, and
       * that shape is common in SGL code. */
      u16 op = context->instruction;
      int backward = 0;

      if ((op & 0xFF80) == 0x8B80 ||    // bf    disp<0
          (op & 0xFF80) == 0x8F80 ||    // bf/s  disp<0
          (op & 0xFF80) == 0x8980 ||    // bt    disp<0
          (op & 0xFF80) == 0x8D80 ||    // bt/s  disp<0
          (op & 0xF800) == 0xA800 ||    // bra   disp<0
          (op & 0xF800) == 0xB800)      // bsr   disp<0
         backward = 1;
      else if ((op & 0xF0FF) == 0x402B ||  // jmp  @Rn
               (op & 0xF0FF) == 0x400B)    // jsr  @Rn
         backward = (context->regs.R[(op >> 8) & 0xF] <= context->regs.PC);
      else if ((op & 0xF0FF) == 0x0023 ||  // braf Rn
               (op & 0xF0FF) == 0x0003)    // bsrf Rn
         backward = ((s32)context->regs.R[(op >> 8) & 0xF] + 4 <= 0);

      if (backward)
      {
         int i;

         // See if it's already on match list
         for (i = 0; i < context->trackInfLoop.num; i++)
         {
            if (context->regs.PC == context->trackInfLoop.match[i].addr)
            {
               context->trackInfLoop.match[i].count++;
               return;
            }
         }

         if (context->trackInfLoop.num >= context->trackInfLoop.maxNum)
         {
            context->trackInfLoop.match = realloc(context->trackInfLoop.match, sizeof(tilInfo_struct) * (context->trackInfLoop.maxNum * 2));
            context->trackInfLoop.maxNum *= 2;
         }

         // Add new
         i=context->trackInfLoop.num;
         context->trackInfLoop.match[i].addr = context->regs.PC;
         context->trackInfLoop.match[i].count = 1;
         context->trackInfLoop.num++;
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

void SH2NMI(SH2_struct *context)
{
   context->onchip.ICR |= 0x8000;

   /* SH7095 manual, sec. 9.2.7 (DMAOR bit 1, NMIF): "This flag indicates that
    * an NMI interrupt has occurred. When the NMIF bit is set to 1, the DMA
    * transfer cannot be enabled even if the DE bit in the CHCR and the DME bit
    * are set to 1. [...] When the NMI interrupt is input while the DMAC is not
    * operating, the NMIF bit is set to 1."
    *
    * DMAProc() already refuses to transfer while NMIF is set, but nothing ever
    * set it, so an NMI did not actually halt an in-flight DMA. Software is
    * expected to clear NMIF (read 1, write 0) before restarting a channel;
    * that path already exists in the DMAOR write handler. */
   context->onchip.DMAOR |= 0x2;

   SH2IntcSetNmi(context);
   SH2EvaluateInterrupt(context);
}

//////////////////////////////////////////////////////////////////////////////

void SH2GetRegisters(SH2_struct *context, sh2regs_struct * r)
{
   if (r != NULL) {
      SH2Core->GetRegisters(context, r);
   }
}

//////////////////////////////////////////////////////////////////////////////

void SH2SetRegisters(SH2_struct *context, sh2regs_struct * r)
{
   if (r != NULL) {
      SH2Core->SetRegisters(context, r);
   }
}

//////////////////////////////////////////////////////////////////////////////

void SH2WriteNotify(SH2_struct *context, u32 start, u32 length) {
   if (SH2Core->WriteNotify) {
     SH2Core->WriteNotify(MSH2, start, length);
     SH2Core->WriteNotify(SSH2, start, length);
   }
}

//////////////////////////////////////////////////////////////////////////////
// Onchip specific
//////////////////////////////////////////////////////////////////////////////

void OnchipReset(SH2_struct *context) {
   context->onchip.SMR = 0x00;
   context->onchip.BRR = 0xFF;
   context->onchip.SCR = 0x00;
   context->onchip.TDR = 0xFF;
   context->onchip.SSR = 0x84;
   context->onchip.RDR = 0x00;
   context->onchip.TIER = 0x01;
   context->onchip.FTCSR = 0x00;
   context->onchip.FTCSRM = 0x00;
   context->onchip.FRC.all = 0x0000;
   context->onchip.OCRA = 0xFFFF;
   context->onchip.OCRB = 0xFFFF;
   context->onchip.TCR = 0x00;
   context->onchip.TOCR = 0xE0;
   context->onchip.FICR = 0x0000;
   context->onchip.IPRB = 0x0000;
   context->onchip.VCRA = 0x0000;
   context->onchip.VCRB = 0x0000;
   context->onchip.VCRC = 0x0000;
   context->onchip.VCRD = 0x0000;
   context->onchip.DRCR0 = 0x00;
   context->onchip.DRCR1 = 0x00;
   context->onchip.WTCSR = 0x18;
   context->onchip.WTCSRM = 0x0;
   context->onchip.WTCNT = 0x00;
   context->onchip.RSTCSR = 0x1F;
   context->onchip.SBYCR = 0x60;
   context->onchip.CCR = 0x00;
   context->onchip.ICR = 0x0000;
   context->onchip.IPRA = 0x0000;
   context->onchip.VCRWDT = 0x0000;
   context->onchip.DVCR = 0x00000000;
   context->onchip.VCRDIV = 0x00000000;
   context->onchip.BARA.all = 0x00000000;
   context->onchip.BAMRA.all = 0x00000000;
   context->onchip.BBRA = 0x0000;
   context->onchip.BARB.all = 0x00000000;
   context->onchip.BAMRB.all = 0x00000000;
   context->onchip.BDRB.all = 0x00000000;
   context->onchip.BDMRB.all = 0x00000000;
   context->onchip.BBRB = 0x0000;
   context->onchip.BRCR = 0x0000;
   context->onchip.CHCR0 = 0x00000000;
   context->onchip.CHCR1 = 0x00000000;
   context->onchip.DMAOR = 0x00000000;
   context->onchip.BCR1 &= 0x8000; // preserve MASTER bit
   context->onchip.BCR1 |= 0x03F0;
   context->onchip.BCR2 = 0x00FC;
   context->onchip.WCR = 0xAAFF;
   context->onchip.MCR = 0x0000;
   context->onchip.RTCSR = 0x0000;
   context->onchip.RTCNT = 0x0000;
   context->onchip.RTCOR = 0x0000;
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL OnchipReadByte(SH2_struct *context, u32 addr) {
  switch(addr) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
      FRTExec(context);
      break;
    default:
      break;
  }
   switch(addr)
   {
      case 0x000:
         return context->onchip.SMR;
      case 0x001:
//         LOG("Bit Rate Register read: %02X\n", context->onchip.BRR);
         return context->onchip.BRR;
      case 0x002:
//         LOG("Serial Control Register read: %02X\n", context->onchip.SCR);
         return context->onchip.SCR;
      case 0x003:
//         LOG("Transmit Data Register read: %02X\n", context->onchip.TDR);
         return context->onchip.TDR;
      case 0x004:
//         LOG("Serial Status Register read: %02X\n", context->onchip.SSR);

/*
         // if Receiver is enabled, clear SSR's TDRE bit, set SSR's RDRF and update RDR.

         if (context->onchip.SCR & 0x10)
         {
            context->onchip.RDR = SCIReceiveByte();
            context->onchip.SSR = (context->onchip.SSR & 0x7F) | 0x40;
         }
         // if Transmitter is enabled, clear SSR's RDRF bit, and set SSR's TDRE bit.
         else if (context->onchip.SCR & 0x20)
         {
            context->onchip.SSR = (context->onchip.SSR & 0xBF) | 0x80;
         }
*/
         return context->onchip.SSR;
      case 0x005:
//         LOG("Receive Data Register read: %02X PC = %08X\n", context->onchip.RDR, SH2Core->GetPC(context));
         return context->onchip.RDR;
      case 0x010:
         return context->onchip.TIER;
      case 0x011:{
        context->onchip.FTCSRM = 0x00;
        return context->onchip.FTCSR;
      }
      case 0x012:
         return context->onchip.FRC.part.H;
      case 0x013:
         return context->onchip.FRC.part.L;
      case 0x014:
         if (!(context->onchip.TOCR & 0x10))
            return context->onchip.OCRA >> 8;
         else
            return context->onchip.OCRB >> 8;
      case 0x015:
         if (!(context->onchip.TOCR & 0x10))
            return context->onchip.OCRA & 0xFF;
         else
            return context->onchip.OCRB & 0xFF;
      case 0x016:
         return context->onchip.TCR;
      case 0x017:
         return context->onchip.TOCR;
      case 0x018:
         return context->onchip.FICR >> 8;
      case 0x019:
         return context->onchip.FICR & 0xFF;
      case 0x060:
         return context->onchip.IPRB >> 8;
      case 0x062:
         return context->onchip.VCRA >> 8;
      case 0x063:
         return context->onchip.VCRA & 0xFF;
      case 0x064:
         return context->onchip.VCRB >> 8;
      case 0x065:
         return context->onchip.VCRB & 0xFF;
      case 0x066:
         return context->onchip.VCRC >> 8;
      case 0x067:
         return context->onchip.VCRC & 0xFF;
      case 0x068:
         return context->onchip.VCRD >> 8;
      case 0x080:
         return context->onchip.WTCSR; // & 0x18;
      case 0x081:
         return context->onchip.WTCNT;
      case 0x092:
         return context->onchip.CCR;
      case 0x0E0:
         return context->onchip.ICR >> 8;
      case 0x0E1:
         return context->onchip.ICR & 0xFF;
      case 0x0E2:
         return context->onchip.IPRA >> 8;
      case 0x0E3:
         return context->onchip.IPRA & 0xFF;
      case 0x0E4:
         return context->onchip.VCRWDT >> 8;
      case 0x0E5:
         return context->onchip.VCRWDT & 0xFF;
      default:
         LOG("Unhandled Onchip byte read %08X\n", (int)addr);
         break;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL OnchipReadWord(SH2_struct *context, u32 addr) {
  switch(addr) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
      FRTExec(context);
      break;
    default:
      break;
  }
   switch(addr)
   {
      case 0x012:
         return context->onchip.FRC.all;
      case 0x014:
         if (!(context->onchip.TOCR & 0x10))
            return context->onchip.OCRA;
         else
            return context->onchip.OCRB;
      case 0x060:
         return context->onchip.IPRB;
      case 0x062:
         return context->onchip.VCRA;
      case 0x064:
         return context->onchip.VCRB;
      case 0x066:
         return context->onchip.VCRC;
      case 0x068:
         return context->onchip.VCRD;
      case 0x0E0:
         return context->onchip.ICR;
      case 0x0E2:
         return context->onchip.IPRA;
      case 0x0E4:
         return context->onchip.VCRWDT;
      case 0x1E2: // real BCR1 register is located at 0x1E2-0x1E3; Sega Rally OK
         return context->onchip.BCR1;
      case 0x1E6:
         return context->onchip.BCR2;
      case 0x1EA:
         return context->onchip.WCR;
      case 0x1EE:
         return context->onchip.MCR;
      case 0x1F2:
         return context->onchip.RTCSR;
      case 0x1F6:
         return context->onchip.RTCNT;
      case 0x1FA:
         return context->onchip.RTCOR;
      default:
         LOG("Unhandled Onchip word read %08X\n", (int)addr);
         return 0;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL OnchipReadLong(SH2_struct *context, u32 addr) {
  switch(addr) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
      FRTExec(context);
      break;
    default:
      break;
  }
   switch(addr)
   {
      case 0x100:
      case 0x120:
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         return context->onchip.DVSR;
      case 0x104: // DVDNT
      case 0x124:
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         return context->onchip.DVDNTL;
      case 0x108:
      case 0x128:
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         return context->onchip.DVCR;
      case 0x10C:
      case 0x12C:
         return context->onchip.VCRDIV;
      case 0x110:
      case 0x130:
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         return context->onchip.DVDNTH;
      case 0x114:
      case 0x134:
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         return context->onchip.DVDNTL;
      case 0x118: // Acts as a separate register, but is set to the same value
      case 0x138: // as DVDNTH after division
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         return context->onchip.DVDNTUH;
      case 0x11C: // Acts as a separate register, but is set to the same value
      case 0x13C: // as DVDNTL after division
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         return context->onchip.DVDNTUL;
      case 0x180:
         return context->onchip.SAR0;
      case 0x184:
         return context->onchip.DAR0;
      case 0x188:
         return context->onchip.TCR0;
      case 0x18C:
         context->onchip.CHCR0M = 0;
         return context->onchip.CHCR0;
      case 0x190:
         return context->onchip.SAR1;
      case 0x194:
         return context->onchip.DAR1;
      case 0x198:
         return context->onchip.TCR1;
      case 0x19C:
          context->onchip.CHCR1M = 0;
         return context->onchip.CHCR1;
      case 0x1A0:
         return context->onchip.VCRDMA0;
      case 0x1A8:
         return context->onchip.VCRDMA1;
      case 0x1B0:
         return context->onchip.DMAOR;
      case 0x1E0:
         return context->onchip.BCR1;
      case 0x1E4:
         return context->onchip.BCR2;
      case 0x1E8:
         return context->onchip.WCR;
      case 0x1EC:
         return context->onchip.MCR;
      case 0x1F0:
         return context->onchip.RTCSR;
      case 0x1F4:
         return context->onchip.RTCNT;
      case 0x1F8:
         return context->onchip.RTCOR;
      default:
         LOG("Unhandled Onchip long read %08X\n", (int)addr);
         return 0;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL OnchipWriteByte(SH2_struct *context, u32 addr, u8 val) {
  switch(addr) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
      FRTExec(context);
      break;
    default:
      break;
  }
   switch(addr) {
      case 0x000:
//         LOG("Serial Mode Register write: %02X\n", val);
         context->onchip.SMR = val;
         return;
      case 0x001:
//         LOG("Bit Rate Register write: %02X\n", val);
         context->onchip.BRR = val;
         return;
      case 0x002:
//         LOG("Serial Control Register write: %02X\n", val);

         // If Transmitter is getting disabled, set TDRE
         if (!(val & 0x20))
            context->onchip.SSR |= 0x80;

         context->onchip.SCR = val;
         return;
      case 0x003:
//         LOG("Transmit Data Register write: %02X. PC = %08X\n", val, SH2Core->GetPC(context));
         context->onchip.TDR = val;
         return;
      case 0x004:
//         LOG("Serial Status Register write: %02X\n", val);

         if (context->onchip.SCR & 0x20)
         {
            // Transmitter Mode

            // If the TDRE bit cleared, let's do a transfer
            if (!(val & 0x80))
               SCITransmitByte(context->onchip.TDR);

            // Generate an interrupt if need be here
         }
         return;
      case 0x010:

         context->onchip.TIER = (val & 0x8E) | 0x1;
         SH2EvaluateInterrupt(context);
         return;
      case 0x011:
         context->onchip.FTCSR = (context->onchip.FTCSR & ((val|context->onchip.FTCSRM) & 0x8E)) | (val & 0x1);
         SH2EvaluateInterrupt(context);
         return;
      case 0x012:
         context->onchip.FRC.part.H = val;
         context->frtcycles = context->cycles;
         return;
      case 0x013:
         context->onchip.FRC.part.L = val;
         context->frtcycles = context->cycles;
         return;
      case 0x014:
         if (!(context->onchip.TOCR & 0x10))
            context->onchip.OCRA = (val << 8) | (context->onchip.OCRA & 0xFF);
         else
            context->onchip.OCRB = (val << 8) | (context->onchip.OCRB & 0xFF);
         return;
      case 0x015:
         if (!(context->onchip.TOCR & 0x10))
            context->onchip.OCRA = (context->onchip.OCRA & 0xFF00) | val;
         else
            context->onchip.OCRB = (context->onchip.OCRB & 0xFF00) | val;
         return;
      case 0x016:
         context->onchip.TCR = val & 0x83;
         switch (val & 3)
         {
            case 0:
               context->frc.shift = 3;
               break;
            case 1:
               context->frc.shift = 5;
               break;
            case 2:
               context->frc.shift = 7;
               break;
            case 3:
               LOG("FRT external input clock not implemented.\n");
               break;
         }
         return;
      case 0x017:
         context->onchip.TOCR = 0xE0 | (val & 0x13);
         return;
      case 0x060:
         context->onchip.IPRB = (val << 8);
         SH2EvaluateInterrupt(context);
         return;
      case 0x061:
         return;
      case 0x062:
         context->onchip.VCRA = ((val & 0x7F) << 8) | (context->onchip.VCRA & 0x00FF);
         return;
      case 0x063:
         context->onchip.VCRA = (context->onchip.VCRA & 0xFF00) | (val & 0x7F);
         return;
      case 0x064:
         context->onchip.VCRB = ((val & 0x7F) << 8) | (context->onchip.VCRB & 0x00FF);
         return;
      case 0x065:
         context->onchip.VCRB = (context->onchip.VCRB & 0xFF00) | (val & 0x7F);
         return;
      case 0x066:
         context->onchip.VCRC = ((val & 0x7F) << 8) | (context->onchip.VCRC & 0x00FF);
         SH2EvaluateInterrupt(context);
         return;
      case 0x067:
         context->onchip.VCRC = (context->onchip.VCRC & 0xFF00) | (val & 0x7F);
         SH2EvaluateInterrupt(context);
         return;
      case 0x068:
         context->onchip.VCRD = (val & 0x7F) << 8;
         SH2EvaluateInterrupt(context);
         return;
      case 0x069:
         return;
      case 0x071:
         context->onchip.DRCR0 = val & 0x3;
         return;
      case 0x072:
         context->onchip.DRCR1 = val & 0x3;
         return;
      case 0x091:
         context->onchip.SBYCR = val & 0xDF;
         return;
      case 0x092:
         context->onchip.CCR = val & 0xCF;
		 if (val & 0x10){
			 InvalidateCache(context);
		 }
		 if ( (context->onchip.CCR & 0x01)  ){
                         enableCache(context);
		 }
		 else{
                         disableCache(context);
		 }
         return;
      case 0x0E0:
         context->onchip.ICR = ((val & 0x1) << 8) | (context->onchip.ICR & 0xFEFF);
         SH2EvaluateInterrupt(context);
         return;
      case 0x0E1:
         context->onchip.ICR = (context->onchip.ICR & 0xFFFE) | (val & 0x1);
         SH2EvaluateInterrupt(context);
         return;
      case 0x0E2:
         context->onchip.IPRA = (val << 8) | (context->onchip.IPRA & 0x00FF);
         SH2EvaluateInterrupt(context);
         return;
      case 0x0E3:
         context->onchip.IPRA = (context->onchip.IPRA & 0xFF00) | (val & 0xF0);
         SH2EvaluateInterrupt(context);
         return;
      case 0x0E4:
         context->onchip.VCRWDT = ((val & 0x7F) << 8) | (context->onchip.VCRWDT & 0x00FF);
         SH2EvaluateInterrupt(context);
         return;
      case 0x0E5:
         context->onchip.VCRWDT = (context->onchip.VCRWDT & 0xFF00) | (val & 0x7F);
         SH2EvaluateInterrupt(context);
         return;
      default:
         LOG("Unhandled Onchip byte write %08X\n", (int)addr);
   }
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL OnchipWriteWord(SH2_struct *context, u32 addr, u16 val) {
  switch(addr) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
      FRTExec(context);
      break;
    default:
      break;
  }
   switch(addr)
   {
      case 0x060:
         context->onchip.IPRB = val & 0xFF00;
         SH2EvaluateInterrupt(context);
         return;
      case 0x062:
         context->onchip.VCRA = val & 0x7F7F;
         return;
      case 0x064:
         context->onchip.VCRB = val & 0x7F7F;
         return;
      case 0x066:
         context->onchip.VCRC = val & 0x7F7F;
         SH2EvaluateInterrupt(context);
         return;
      case 0x068:
         context->onchip.VCRD = val & 0x7F7F;
         SH2EvaluateInterrupt(context);
         return;
      case 0x080:
         // This and RSTCSR have got to be the most wackiest register
         // mappings I've ever seen

         if (val >> 8 == 0xA5)
         {
            // WTCSR
            switch (val & 7)
            {
               case 0:
                  context->wdt.shift = 1;
                  break;
               case 1:
                  context->wdt.shift = 6;
                  break;
               case 2:
                  context->wdt.shift = 7;
                  break;
               case 3:
                  context->wdt.shift = 8;
                  break;
               case 4:
                  context->wdt.shift = 9;
                  break;
               case 5:
                  context->wdt.shift = 10;
                  break;
               case 6:
                  context->wdt.shift = 12;
                  break;
               case 7:
                  context->wdt.shift = 13;
                  break;
            }

            context->wdt.isenable = (val & 0x20);
            context->wdt.isinterval = (~val & 0x40);

            context->onchip.WTCSR = (context->onchip.WTCSR & (context->onchip.WTCSRM | val) & 0x80) | (val & 0x67);
            // FIXME(?): the line below unconditionally clears bit 7 (OVF)
            // right after the formula above computed it using the same
            // read-then-write-0/WTCSRM race-avoidance logic FTCSR uses
            // (Hitachi SH7095 manual, Sec. 12.2.2: OVF "cleared by reading
            // OVF, then writing 0 in OVF"). That makes this unconditional
            // clear override the formula's result, and also makes the
            // '&= ~0x80' a few lines below (in the TME==0 branch) dead code
            // (bit 7 is already 0 by the time it's reached). Not changed
            // here since the WDT is rarely used as a real watchdog by
            // Saturn games and the practical impact is unclear -- flagging
            // for a future pass rather than guessing.
            context->onchip.WTCSR &= ~0x80;
            if(context->onchip.WTCSR & 0x20){
               context->onchip.SBYCR &= 0x7F;
            }else {
               context->onchip.WTCSR &= ~0x80;
               context->onchip.WTCNT = 0;
            }
            SH2EvaluateInterrupt(context);
         }
         else if (val >> 8 == 0x5A)
         {
            // WTCNT
            if(context->onchip.WTCSR & 0x20)
               context->onchip.WTCNT = (u8)val;
         }
         return;
      case 0x082:
         if (val == 0xA500)
            // clear WOVF bit
            context->onchip.RSTCSR &= 0x7F;
         else if (val >> 8 == 0x5A)
            // RSTE and RSTS bits
            context->onchip.RSTCSR = (context->onchip.RSTCSR & 0x80) | (val & 0x60) | 0x1F;
         return;
      case 0x092:
         context->onchip.CCR = val & 0xCF;
		 if (val & 0x10){
			 InvalidateCache(context);
		 }
		 if ( (context->onchip.CCR & 0x01)  ){
                         enableCache(context);
		 }
		 else{
                         disableCache(context);
		 }
         return;
      case 0x0E0:
         context->onchip.ICR = val & 0x0101;
         SH2EvaluateInterrupt(context);
         return;
      case 0x0E2:
         context->onchip.IPRA = val & 0xFFF0;
         SH2EvaluateInterrupt(context);
         return;
      case 0x0E4:
      case 0x0E5:
         context->onchip.VCRWDT = val & 0x7F7F;
         SH2EvaluateInterrupt(context);
         return;
      case 0x108:
      case 0x128:
         context->onchip.DVCR = val & 0x3;
         SH2EvaluateInterrupt(context);
         return;
      case 0x148:
         context->onchip.BBRA = val & 0xFF;
         return;
      case 0x178:
         context->onchip.BRCR = val & 0xF4DC;
         return;
      default:
         LOG("Unhandled Onchip word write %08X(%04X)\n", (int)addr, val);
         return;
   }
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// Division unit (DIVU) - SH7095 manual, section 10
//////////////////////////////////////////////////////////////////////////////

/* Set to 0 to keep the old, simpler behaviour of always leaving the saturated
 * value in DVDNTL on overflow, whatever OVFIE says. See DivuExecute(). */
#define DIVU_STRICT_OVFIE_INTERMEDIATE 1

/* Model of the state the divider is left in by the early exit taken on
 * overflow. Sec. 10.3.3: the operation "will then end with the result after 6
 * cycles of operation stored in the DVDNTH and DVDNTL registers [...] The
 * first three of the 6 cycles executed when an overflow occurs are used for
 * flag setting within the division unit and the next three for division."
 *
 * Each division step shifts the 64-bit dividend one place to the left, so
 * three steps leave (dividend << 3); DVDNTH receives its upper word. This is
 * the same model the divide-by-zero path already used, except that the
 * negative branch there shifted by two instead of three
 * (0xFFFFFFFC | 2 bits of val) while the positive branch shifted by three -
 * the two disagreed on the sign extension by one bit. */
static u32 DivuIntermediateH(s64 dividend)
{
   return (u32)(((u64)dividend) >> 29);
}

static u32 DivuIntermediateL(s64 dividend)
{
   return (u32)(((u64)dividend) << 3);
}

static void DivuExecute(SH2_struct *context, s64 dividend)
{
   s32 divisor = (s32)context->onchip.DVSR;
   s64 quotient = 0;
   int overflow;
   int negative;

   /* Sec. 10.3.3: an overflow is raised "when the results of operations exceed
    * the ranges expressed as signed 32 bits [...] or when the divisor is 0". */
   if (divisor == 0)
   {
      /* The quotient runs off to +/-infinity; the sign of the dividend picks
       * which end DVDNTL saturates to. */
      overflow = 1;
      negative = (dividend < 0);
   }
   else if ((divisor == -1) && (dividend == (s64)0x8000000000000000LL))
   {
      /* -2^63 / -1 is not representable. In C this is undefined behaviour and
       * on x86 the idiv traps (#DE), so it has to be caught *before* the
       * division is issued rather than by inspecting the result afterwards.
       * The same trap used to be reachable from the 32-bit path through
       * (s32)0x80000000 / -1, which is how a game could take the emulator
       * down with a perfectly legal - if overflowing - divide. */
      overflow = 1;
      negative = 0;
   }
   else
   {
      quotient = dividend / divisor;
      overflow = (quotient > 0x7FFFFFFFLL) || (quotient < -0x80000000LL);
      negative = (quotient < 0);
   }

   if (overflow)
   {
      /* Sec. 10.3.1 / 10.3.2: a normal operation takes 39 cycles, but "when an
       * overflow occurs, however, the operation ends in 6 cycles". Both paths
       * used to be charged the full 39. */
      context->divcycles = context->cycles + 6;

      context->onchip.DVCR |= 1;   // OVF

      /* Table 10.2, "Overflow Processing": DVDNTH always holds the
       * intermediate result. DVDNTL (and its DVDNT alias) holds the
       * intermediate result as well when the overflow interrupt is enabled,
       * and is forced to the maximum value for a positive overflow or the
       * minimum for a negative one when it is disabled. The old code always
       * wrote the saturated value.
       *
       * Only the OVFIE = 0 column is precisely specified; the manual says no
       * more about the OVFIE = 1 case than "the operation intermediate
       * result", so what lands in DVDNTL there is the shift model above.
       * Software that enables OVFIE does so to trap the error and normally
       * ignores the value, but set DIVU_STRICT_OVFIE_INTERMEDIATE to 0 to fall
       * back to writing the saturated value unconditionally. */
      context->onchip.DVDNTH = DivuIntermediateH(dividend);
#if DIVU_STRICT_OVFIE_INTERMEDIATE
      if (context->onchip.DVCR & 0x2)   // OVFIE
         context->onchip.DVDNTL = DivuIntermediateL(dividend);
      else
#endif
         context->onchip.DVDNTL = negative ? 0x80000000 : 0x7FFFFFFF;
   }
   else
   {
      context->divcycles = context->cycles + 39;
      context->onchip.DVDNTL = (u32)quotient;
      context->onchip.DVDNTH = (u32)(dividend % divisor);
   }

   /* DVDNT is the same physical register as DVDNTL (sec. 10.2.6). The old
    * divide-by-zero paths forgot to refresh it, so a program that started a
    * division that overflowed and then read DVDNT got the quotient of the
    * *previous* division. */
   context->onchip.DVDNT = context->onchip.DVDNTL;
   context->onchip.DVDNTUL = context->onchip.DVDNTL;
   context->onchip.DVDNTUH = context->onchip.DVDNTH;

   SH2EvaluateInterrupt(context);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL OnchipWriteLong(SH2_struct *context, u32 addr, u32 val)  {
  switch(addr) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
      FRTExec(context);
      break;
    default:
      break;
  }
   switch (addr)
   {
   case 0x010:
     context->onchip.TIER = (val & 0x8E) | 0x1;
     SH2EvaluateInterrupt(context);
     break;
   case 0x060:
     context->onchip.IPRB = val & 0xFF00;
     SH2EvaluateInterrupt(context);
     break;
      case 0x100:
      case 0x120:
        context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         context->onchip.DVSR = val;
         return;
      case 0x104: // 32-bit / 32-bit divide operation
      case 0x124:
         /* Sec. 10.2.2: writing DVDNT starts the 32/32 operation, "the same
          * value is written in the DVDNTL register [and] the MSB written is
          * sign extended to the DVDNTH register" - so the operand really is a
          * 64-bit sign-extended dividend and the 64/32 machinery applies.
          *
          * The two overflow tests that used to live here were dead code: the
          * quotient was an s32, so "quotient > 0x7FFFFFFF" and
          * "(s32)((s64)quotient >> 32) < -1" could never be true. The one case
          * that does overflow a 32/32 division, H'80000000 / -1, therefore
          * fell through to the plain C division and trapped. */
         DivuExecute(context, (s64)(s32)val);
         return;
      case 0x108:
      case 0x128:
         context->onchip.DVCR = val & 0x3;
         SH2EvaluateInterrupt(context);
         return;
      case 0x10C:
      case 0x12C:
         context->onchip.VCRDIV = val & 0xFFFF;
         SH2EvaluateInterrupt(context);
         return;
      case 0x110:
      case 0x130:
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         context->onchip.DVDNTH = val;
         return;
      case 0x114:
      case 0x134: { // 64-bit / 32-bit divide operation
         /* Sec. 10.3.1: DVDNTH holds the upper half, and writing DVDNTL both
          * supplies the lower half and starts the operation.
          *
          * The negative-overflow test used to be "(s32)(quotient >> 32) < -1",
          * which never fires for the quotients that actually overflow: for
          * anything in [-2^63, -2^31) down to about -2^32 the arithmetic shift
          * still yields -1. Negative overflows were therefore written out as
          * truncated quotients with no OVF flag at all. */
         s64 dividend = (s64)((((u64)context->onchip.DVDNTH) << 32) | val);
         DivuExecute(context, dividend);
         return;
      }
      case 0x118:
      case 0x138:
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         context->onchip.DVDNTUH = val;
         return;
      case 0x11C:
      case 0x13C:
         context->cycles += MAX((int)context->divcycles - (int)context->cycles,0);
         context->onchip.DVDNTUL = val;
         return;
      case 0x140:
         context->onchip.BARA.all = val;
         return;
      case 0x144:
         context->onchip.BAMRA.all = val;
         return;
      case 0x180:
         context->onchip.SAR0 = val;
         return;
      case 0x184:
         context->onchip.DAR0 = val;
         return;
      case 0x188:
         context->onchip.TCR0 = val & 0xFFFFFF;
         return;
      case 0x18C:
        if (context->onchip.TCR0 != 0) {
          DMAProc(context, 0x7FFFFFFF);
        }
//         context->onchip.CHCR0 = val & 0xFFFF;

         context->onchip.CHCR0 = (val & ~2) | (context->onchip.CHCR0 & (val| context->onchip.CHCR0M) & 2);
         SH2EvaluateInterrupt(context);

         // If the DMAOR DME bit is set and AE and NMIF bits are cleared,
         // and CHCR's DE bit is set and TE bit is cleared,
         // do a dma transfer
         if ((context->onchip.DMAOR & 7) == 1 && (val & 0x3) == 1) {
            context->dma_ch0.copy_clock = 0;
            DMAExec(context);
         }
         return;
      case 0x190:
         context->onchip.SAR1 = val;
         return;
      case 0x194:
         context->onchip.DAR1 = val;
         return;
      case 0x198:
         context->onchip.TCR1 = val & 0xFFFFFF;
         return;
      case 0x19C:
        if (context->onchip.TCR1 != 0) {
          DMAProc(context, 0x7FFFFFFF);
        }
//         context->onchip.CHCR1 = val & 0xFFFF;

         context->onchip.CHCR1 = (val & ~2) | (context->onchip.CHCR1 & (val| context->onchip.CHCR1M) & 2);
         SH2EvaluateInterrupt(context);

         // If the DMAOR DME bit is set and AE and NMIF bits are cleared,
         // and CHCR's DE bit is set and TE bit is cleared,
         // do a dma transfer
         if ((context->onchip.DMAOR & 7) == 1 && (context->onchip.CHCR1 & 0x3) == 1) {
            context->dma_ch1.copy_clock = 0;
            DMAExec(context);
         }
         return;
      case 0x1A0:
         context->onchip.VCRDMA0 = val & 0xFFFF;
         SH2EvaluateInterrupt(context);
         return;
      case 0x1A8:
         context->onchip.VCRDMA1 = val & 0xFFFF;
         SH2EvaluateInterrupt(context);
         return;
      case 0x1B0:
         // AE (bit 2) and NMIF (bit 1) are documented "R/(W)*: only
         // writing permitted is 0 to clear the flag" (Hitachi SH7095
         // manual, Sec. 9.2.7) -- hardware-set-only status bits that
         // software can clear but never set. PR (bit 3) and DME (bit 0)
         // are plain R/W. A direct 'val & 0xF' let a stray write set
         // AE/NMIF to 1 and spuriously block all DMA (the manual says DMA
         // stays disabled while either is 1), which real hardware
         // wouldn't allow.
         context->onchip.DMAOR = (context->onchip.DMAOR & val & 0x6) | (val & 0x9);

         // If the DMAOR DME bit is set and AE and NMIF bits are cleared,
         // and CHCR's DE bit is set and TE bit is cleared,
         // do a dma transfer
         if ((val & 7) == 1)
            DMAExec(context);
         return;
      case 0x1E0:
         context->onchip.BCR1 &= 0x8000;
         context->onchip.BCR1 |= val & 0x1FF7;
         return;
      case 0x1E4:
         context->onchip.BCR2 = val & 0xFC;
         return;
      case 0x1E8:
         context->onchip.WCR = val;
         return;
      case 0x1EC:
         context->onchip.MCR = val & 0xFEFC;
         return;
      case 0x1F0:
         context->onchip.RTCSR = val & 0xF8;
         return;
      case 0x1F8:
         context->onchip.RTCOR = val & 0xFF;
         return;
      default:
         LOG("Unhandled Onchip long write %08X,%08X\n", (int)addr, val);
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////
#ifdef USE_CACHE
static void UpdateLRU(SH2_struct *context, u8 line, u8 way) {
//Table 8.3 SH7604_Hardware_Manual.pdf
  switch (way) {
    case 0:
      context->cacheLRU[line] &= 0x7;
    break;
    case 1:
      context->cacheLRU[line] &= 0x19;
      context->cacheLRU[line] |= 0x20;
    break;
    case 2:
      context->cacheLRU[line] &= 0x2A;
      context->cacheLRU[line] |= 0x14;
    break;
    case 3:
      context->cacheLRU[line] |= 0x0B;
    break;
    default:
    break;
  }
  // CACHE_LOG("%s : Update Line %d => way %d\n", (context==SSH2)?"SSH2":"MSH2", line, way);
}

static u8 getLRU(SH2_struct *context, u32 tag, u8 line) {
//Table 8.3 SH7604_Hardware_Manual.pdf
  u8 way = -1;
  if (context->onchip.CCR & (1 << 3))//2-way mode
  {
    if ((context->cacheLRU[line] & 1) == 1)
      return 2;
    else
      return 3;
  }
  else
  {
    if ((context->cacheLRU[line] & 0x38) == 0x38) way=0;
    else if ((context->cacheLRU[line] & 0x26) == 0x6) way=1;
    else if ((context->cacheLRU[line] & 0x15) == 0x1) way=2;
    else if ((context->cacheLRU[line] & 0x0B) == 0x0) way=3;
    //Init phase
    else if (context->cacheLRU[line] == 0xB) way=2;
    //Shall never be reached
    else if (context->cacheLRU[line] == 0x1E) way=1;
    else if (context->cacheLRU[line] == 0x38) way=0;
    /* Table 8.3 leaves 32 of the 64 LRU encodings undefined. They are not
       reachable from the reset state, but "way" starts life as (u8)-1 and is
       used unchecked as an index into cacheTagArray[64][4] and
       cacheData[64][4][16], so any future change that reaches one of those
       states becomes a silent out-of-bounds write. Pin it to a valid way. */
    else way=0;

    // CACHE_LOG("%s : Line %d => way %d\n", (context==SSH2)?"SSH2":"MSH2", line, way);
  }
  return way;
}

static inline void CacheWriteThrough(SH2_struct *context, u8* mem, u32 addr, u32 val, u8 size) {
  SH2UpdateABusAccess(context, 1); //When cpu access CPU-BUs at the same time as SCU, there might be a penalty
  switch(size) {
  case 1:
    WriteByteList[(addr >> 16) & 0xFFF](context, mem, addr, val);
    break;
  case 2:
    WriteWordList[(addr >> 16) & 0xFFF](context, mem, addr, val);
    break;
  case 4:
    WriteLongList[(addr >> 16) & 0xFFF](context, mem, addr, val);
    break;
  }
}

static inline void CacheWriteVal(SH2_struct *context, u32 addr, u32 val, u8 size ) {
  u8 line = (addr>>4)&0x3F;
  u32 tag = (addr>>10)&0x7FFFF;
  u8 byte = addr&0xF;
  u8 way=context->tagWay[line][tag];
  switch(size) {
  case 1:
    WriteByteList[(addr >> 16) & 0xFFF](context,context->cacheData[line][way], byte, val);
    break;
  case 2:
    WriteWordList[(addr >> 16) & 0xFFF](context,context->cacheData[line][way], byte, val);
    break;
  case 4:
    WriteLongList[(addr >> 16) & 0xFFF](context,context->cacheData[line][way], byte, val);
    break;
  }
}

void CacheWrite(SH2_struct *context, u8* mem, u32 addr, u32 val, u8 size) {
  u8 line = (addr>>4)&0x3F;
  u32 tag = (addr>>10)&0x7FFFF;
  u8 byte = addr&0xF;
  u8 way=context->tagWay[line][tag];
  u8 ret = 0;
  if (byte + size > 16) CACHE_LOG("!!!!!!!!!!!!!!!! Warn out of line....\n");
  // CACHE_LOG("Write (%d %x) tag %x **** %x (%x %x)\n", line, way, context->cacheTagArray[line][way], tag, addr, val);
  if ((way <= 0x3) && (context->cacheTagArray[line][way] == tag)) {
    // Cache hit => update cache
    // CACHE_LOG("Hit Write (%d %x) tag %x **** %x (%x %x)\n", line, way, context->cacheTagArray[line][way], tag, addr, val);
    UpdateLRU(context, line, way);
    CacheWriteVal(context, addr, val, size);
    // for (int i =0; i<=0xF; i++) {
    //   printf("%x ", context->cacheData[line][way][i]);
    // }
    // printf("\n");
  }
  // else   CACHE_LOG("Write Miss (%d %x) tag %x **** %x (%x %x)\n", line, way, context->cacheTagArray[line][way], tag, addr, val);
  CacheWriteThrough(context, mem, addr, val, size);
}

void CacheWriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val){
  CacheWrite(context, mem, addr, val, 1);
}
void CacheWriteWord(SH2_struct *context,u8* mem, u32 addr, u16 val){
  CacheWrite(context, mem, addr, val, 2);
}
void CacheWriteLong(SH2_struct *context,u8* mem, u32 addr, u32 val){
  CacheWrite(context, mem, addr, val, 4);
}
#endif

void InvalidateCache(SH2_struct *ctx) {
#ifdef USE_CACHE
  int line, way;

  if (yabsys.usecache == 0) return;

  /* Drop the decoded-instruction cache for every line that was resident,
     not for 4 KB at address 0.

     A line decoded while the SH2 cache held stale data stays decoded from
     that stale data forever, because the decode cache never re-reads. The
     old SH2WriteNotify(ctx, 0, 0x1000) only ever covered the boot ROM, so
     Work RAM-L and Work RAM-H -- the two regions enableCache() actually
     caches -- were never invalidated on a purge.

     Walking cacheTagArray reconstructs each resident line's address as
     (tag << 10) | (line << 4): 256 lines of 16 bytes at most, far cheaper
     than notifying the 2 MB of cacheable space, and precise. */
  for (line = 0; line < 64; line++)
    for (way = 0; way < 4; way++)
      SH2WriteNotify(ctx,
                     (ctx->cacheTagArray[line][way] << 10) | (line << 4), 16);

  memset(ctx->cacheLRU, 0, 64);
  memset(ctx->tagWay, 0x4, 64*0x80000);
  memset(ctx->cacheTagArray, 0x0, 64*4*sizeof(u32));
#endif
  ctx->cycles += 1;
}

void enableCache(SH2_struct *context) {
#ifdef USE_CACHE
  int i;
  if (yabsys.usecache == 0) return;
  if (context->cacheOn == 0) {
    context->cacheOn = 1;
    context->nbCacheWay = 4;
    for (i=0x20; i < 0x30; i++)
    {
      //LowWRam is cached
       CacheReadByteList[i] = CacheReadByte;
       CacheReadWordList[i] = CacheReadWord;
       CacheReadLongList[i] = CacheReadLong;
       CacheWriteByteList[i] = CacheWriteByte;
       CacheWriteWordList[i] = CacheWriteWord;
       CacheWriteLongList[i] = CacheWriteLong;
    }
    for (i=0x600; i < 0x800; i++)
    {
      //HiWRam is cached
       CacheReadByteList[i] = CacheReadByte;
       CacheReadWordList[i] = CacheReadWord;
       CacheReadLongList[i] = CacheReadLong;
       CacheWriteByteList[i] = CacheWriteByte;
       CacheWriteWordList[i] = CacheWriteWord;
       CacheWriteLongList[i] = CacheWriteLong;
    }
  }
#else
  return;
#endif
}

void disableCache(SH2_struct *context) {
#ifdef USE_CACHE
  /* Only clear this CPU's flag.

     CacheRead*List / CacheWrite*List are GLOBAL tables shared by both SH2s,
     while cacheOn is per-context. Rewriting them here rerouted the *other*
     CPU's accesses as well: with the master running cached code, a
     slave-side disable silently sent every master write straight to memory,
     bypassing its resident cache lines. The line then kept its old value and
     the next master read returned it -- a stale read with no bad write
     anywhere to blame, which is how a saved PR came back as a stack address
     and the following RTS branched into the stack.

     Routing is now decided per access from context->cacheOn in
     SH2MappedMemoryRead/Write, so the tables can simply stay installed. */
  if (context->cacheOn == 1)
    context->cacheOn = 0;
#else
  return;
#endif
}

#ifdef USE_CACHE
void CacheFetch(SH2_struct *context, u8* memory, u32 addr, u8 way) {
  u8 line = (addr>>4)&0x3F;
  u32 tag = (addr>>10)&0x7FFFF;
  SH2UpdateABusAccess(context, 1); //When cpu access CPU-BUs at the same time as SCU, there might be a penalty
  UpdateLRU(context, line, way);
  context->tagWay[line][tag] = way;
  context->cacheTagArray[line][way] = tag;
  for (int i=0; i<4; i++) {
    u32 ret = ReadLongList[(addr >> 16) & 0xFFF](context, memory,(addr&(~0xF))|(i*4));
    CacheWriteVal(context, (addr&(~0xF))|(i*4), ret, 4);
    // printf("Fetch (%x) (%d)=%x\n", (addr&(~0xF))|(i*4), i, ret);
  }
  /* A cache line is 16 bytes and all four longwords were just refilled by
     the loop above, but only the first four bytes were invalidated: the
     remaining six instructions of the line kept whatever decode they had
     from before the fetch. */
  SH2WriteNotify(context, (addr&(~0xF)), 16);
  // for (int i =0; i<=0xF; i++) {
  //   printf("%x ", context->cacheData[line][way][i]);
  // }
  // printf("\n");
}

u8 CacheReadByte(SH2_struct *context,u8* memory, u32 addr) {
  u8 line = (addr>>4)&0x3F;
  u32 tag = (addr>>10)&0x7FFFF;
  u8 byte = addr&0xF;
  u8 way = context->tagWay[line][tag];
  if (byte + 1 > 16) CACHE_LOG("!!!!!!!!!!!!!!!! Warn out of line....\n");
  if ((way <= 0x3) && (context->cacheTagArray[line][way] == tag)) {
    UpdateLRU(context, line, way);
    u8 ret = ReadByteList[(addr >> 16) & 0xFFF](context,context->cacheData[line][way],byte);
#ifdef CACHE_DEBUG
    if (ret != ReadByteList[(addr >> 16) & 0xFFF](context, memory, addr)) {
      YuiMsg("Read Byte addr %x from cache = %x (%x)\n", addr, ret, ReadByteList[(addr >> 16) & 0xFFF](context, memory, addr));
      fflush(stdout);
      abort();
    }
#endif
    return ret;
  }
  way = getLRU(context, tag, line);
  CacheFetch(context, memory, addr, way);
  u8 ret = ReadByteList[(addr >> 16) & 0xFFF](context,context->cacheData[line][way],byte);
#ifdef CACHE_DEBUG
  if (ret != ReadByteList[(addr >> 16) & 0xFFF](context, memory, addr)) {
    YuiMsg("Read Byte addr %x out of cache = %x (%x)\n", addr, ret, ReadByteList[(addr >> 16) & 0xFFF](context, memory, addr));
    fflush(stdout);
    abort();
  }
#endif
  return ret;
}

u16 CacheReadWord(SH2_struct *context,u8* memory, u32 addr) {
  u8 line = (addr>>4)&0x3F;
  u32 tag = (addr>>10)&0x7FFFF;
  u8 byte = (addr&0xF);
  u8 way = context->tagWay[line][tag];
  if (byte + 2 > 16) CACHE_LOG("!!!!!!!!!!!!!!!! Warn out of line....\n");
  if ((way <= 0x3) && (context->cacheTagArray[line][way] == tag)) {
    UpdateLRU(context, line, way);
    u16 ret = ReadWordList[(addr >> 16) & 0xFFF](context, context->cacheData[line][way],byte);
#ifdef CACHE_DEBUG
    if (ret != ReadWordList[(addr >> 16) & 0xFFF](context, memory, addr)) {
      YuiMsg("Read Word addr %x (%x) from of cache = %x (%x)\n", addr, (addr >> 16) & 0xFFF, ret, ReadWordList[(addr >> 16) & 0xFFF](context, memory, addr));
      fflush(stdout);
      abort();
    }
#endif
    return ret;
  }
  way = getLRU(context, tag, line);
  CacheFetch(context, memory, addr, way);
  u16 ret = ReadWordList[(addr >> 16) & 0xFFF](context, context->cacheData[line][way],byte);
#ifdef CACHE_DEBUG
  if (ret != ReadWordList[(addr >> 16) & 0xFFF](context, memory, addr)) {
    YuiMsg("Read Word addr %x (%x) out of cache = %x (%x)\n", addr, (addr >> 16) & 0xFFF, ret, ReadWordList[(addr >> 16) & 0xFFF](context, memory, addr));
    fflush(stdout);
    abort();
  }
#endif
  return ret;
}

u32 CacheReadLong(SH2_struct *context,u8* memory, u32 addr) {
  u8 line = (addr>>4)&0x3F;
  u32 tag = (addr>>10)&0x7FFFF;
  u8 byte = (addr&0xF);
  u8 way = context->tagWay[line][tag];
  if (byte + 4 > 16) CACHE_LOG("!!!!!!!!!!!!!!!! Warn out of line....\n");
  if ((way <= 0x3) && (context->cacheTagArray[line][way] == tag)) {
    UpdateLRU(context, line, way);
    u32 ret = ReadLongList[(addr >> 16) & 0xFFF](context,context->cacheData[line][way],byte);
#ifdef CACHE_DEBUG
    if (ret != ReadLongList[(addr >> 16) & 0xFFF](context, memory, addr)) {
      YuiMsg("Read Long addr %x from cache = %x (%x)\n", addr, ret, ReadLongList[(addr >> 16) & 0xFFF](context, memory, addr));
      fflush(stdout);
      abort();
    }
#endif
    return ret;
  }
  way = getLRU(context, tag, line);
  CacheFetch(context, memory, addr, way);
  u32 ret = ReadLongList[(addr >> 16) & 0xFFF](context,context->cacheData[line][way],byte);
#ifdef CACHE_DEBUG
  if (ret != ReadLongList[(addr >> 16) & 0xFFF](context, memory, addr)) {
    YuiMsg("Read Long addr %x out of cache = %x (%x)\n", addr, ret, ReadLongList[(addr >> 16) & 0xFFF](context, memory, addr));
    fflush(stdout);
    abort();
  }
#endif
  return ret;
}
#endif

void CacheInvalidate(SH2_struct *context,u32 addr){
#ifdef USE_CACHE
  if (yabsys.usecache == 0) return;
  u8 line = (addr>>4)&0x3F;
  u32 tag = (addr>>10)&0x7FFFF;
  u8 way = context->tagWay[line][tag];
  context->tagWay[line][tag] = 0x4;
  if (way <= 0x3) context->cacheTagArray[line][way] = 0x0;
  context->cacheLRU[line] = 0;
#endif
  context->cycles += 2;
}

u32 FASTCALL AddressArrayReadLong(SH2_struct *context,u32 addr) {
#ifdef USE_CACHE
  if (yabsys.usecache == 0) return 0;
  u8 line = (addr>>4)&0x3F;
  u8 way = (context->onchip.CCR>>6)&0x3;
  return ((context->cacheLRU[line]&0x3F)<<4) | ((context->cacheTagArray[line][way]&0x7FFFF)<<10) | ((context->cacheTagArray[line][way]!= 0x0)<<1);
#else
  return 0;
#endif
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL AddressArrayWriteLong(SH2_struct *context,u32 addr, u32 val)  {
#ifdef USE_CACHE
  if (yabsys.usecache == 0) return;
  u8 line = (addr>>4)&0x3F;
  u32 tag = (addr>>10)&0x7FFFF;
  u8 valid = (addr>>2)&0x1;
  u8 way = (context->onchip.CCR>>6)&0x3;
  context->cacheLRU[line] = (val>>4)&0x3F;
  if (valid) {
    context->tagWay[line][tag] = way;
    context->cacheTagArray[line][way] = tag;
  } else {
    context->tagWay[line][tag] = 0x4;
    context->cacheTagArray[line][way] = 0x0;
  }
#endif
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL DataArrayReadByte(SH2_struct *context,u32 addr) {
  return T2ReadByte(context->DataArray, addr & 0xFFF);
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL DataArrayReadWord(SH2_struct *context,u32 addr) {
  return T2ReadWord(context->DataArray, addr & 0xFFF);
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL DataArrayReadLong(SH2_struct *context,u32 addr) {
  return T2ReadLong(context->DataArray, addr & 0xFFF);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL DataArrayWriteByte(SH2_struct *context,u32 addr, u8 val)  {
  T2WriteByte(context->DataArray, addr & 0xFFF, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL DataArrayWriteWord(SH2_struct *context,u32 addr, u16 val)  {
  T2WriteWord(context->DataArray, addr & 0xFFF, val);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL DataArrayWriteLong(SH2_struct *context,u32 addr, u32 val)  {
  T2WriteLong(context->DataArray, addr & 0xFFF, val);
}

//////////////////////////////////////////////////////////////////////////////

/* Was the OCR value crossed during this step?
 *
 * frctemp is the *un-wrapped* counter, so it can exceed 0xFFFF; a match
 * happened if the half-open interval (frcold, frctemp] contains the OCR value,
 * either on this pass round the counter or on the next one.
 *
 * The second term is what the old inline test was missing: a compare match
 * scheduled just above the current FRC that only came due after the counter
 * wrapped through H'FFFF -> H'0000 was silently dropped, because the test read
 * "frcold < OCRA" and frcold was by then larger than OCRA. It also means an
 * OCR of 0 - previously unmatchable, since nothing is "< 0" - now matches on
 * the wrap, which is what the comparator does in hardware. */
static INLINE int FRTCompareMatch(u32 frcold, u32 frctemp, u32 ocr)
{
   if ((ocr > frcold) && (ocr <= frctemp))
      return 1;
   ocr += 0x10000;
   return ((ocr > frcold) && (ocr <= frctemp));
}

void FRTExec(SH2_struct *context)
{
   u32 frcold;
   u32 frctemp;
   u32 mask;
   int matchA, matchB;

   u32 cycles = context->cycles - context->frtcycles;

   context->frtcycles = context->cycles;

   frcold = frctemp = (u32)context->onchip.FRC.all;
   mask = (1 << context->frc.shift) - 1;

   // Increment FRC
   frctemp += ((cycles + context->frc.leftover) >> context->frc.shift);
   context->frc.leftover = (cycles + context->frc.leftover) & mask;

   /* Both comparators watch the same counter, so they are evaluated against
    * the same window before anything modifies it. The old code tested OCRB
    * against a frctemp that CCLRA may already have zeroed on the OCRA match,
    * which lost the OCRB match whenever OCRB sat below OCRA. */
   matchA = FRTCompareMatch(frcold, frctemp, context->onchip.OCRA);
   matchB = FRTCompareMatch(frcold, frctemp, context->onchip.OCRB);

   // Check to see if there is or was a Output Compare A match
   if (matchA)
   {
      // Do we need to clear the FRC?
      if (context->onchip.FTCSR & 0x1)
      {
         /* CCLRA clears FRC to 0 on the OCRA match. Carry the cycles that
          * overshot the match into leftover BEFORE zeroing FRC — frctemp still
          * holds the matched value here. The previous order set frctemp=0
          * first, so the carry evaluated (0 - OCRA): an unsigned underflow that
          * injected a huge bogus value into frc.leftover and corrupted the next
          * FRC increment (FRC-based timing/RNG in games drifted). */
         context->frc.leftover += ((frctemp - context->onchip.OCRA) << context->frc.shift);
         frctemp = 0;
      }

      // Set OCFA flag
      context->onchip.FTCSR |= 0x8;
      context->onchip.FTCSRM |= 0x8;
      SH2EvaluateInterrupt(context);
   }

   // Check to see if there is or was a Output Compare B match
   if (matchB)
   {
      // Set OCFB flag
      context->onchip.FTCSR |= 0x4;
      context->onchip.FTCSRM |= 0x4;
      SH2EvaluateInterrupt(context);
   }

   // If FRC overflows, set overflow flag
   if (frctemp > 0xFFFF) {
     if ((context->onchip.FTCSR & 0x2)== 0x0)
     {
       context->onchip.FTCSR |= 0x2;
       context->onchip.FTCSRM |= 0x2;
       SH2EvaluateInterrupt(context);
     }
     /* SH7095 manual, sec. 11.2.1: the FRC is a free-running *up-counter* and
      * "when the FRC overflows (H'FFFF -> H'0000), the overflow flag (OVF) of
      * the FTCSR is set to 1". It wraps modulo 65536 - it does not restart
      * from zero discarding whatever counts had already accumulated past the
      * wrap point.
      *
      * The old code zeroed frctemp outright, so e.g. FRC=H'FFF0 plus H'20
      * ticks landed on H'0000 instead of H'0010: up to 65535 counts could be
      * silently dropped on every wrap, making the FRC run slow. The line that
      * was meant to carry the excess was also dead - it read (frctemp >> 16)
      * *after* frctemp had been set to 0, so it always added exactly nothing.
      * Masking is both simpler and correct, and it needs no leftover fixup. */
     frctemp &= 0xFFFF;
   }

   // Write new FRC value
   context->onchip.FRC.all = frctemp;
}

//////////////////////////////////////////////////////////////////////////////

void WDTExec(SH2_struct *context) {
   u32 wdttemp;
   u32 mask;

   u32 cycles = context->cycles - context->wdtcycles;

   context->wdtcycles = context->cycles;

   if ((!context->wdt.isenable) || (context->onchip.WTCSR & 0x80) || (context->onchip.RSTCSR & 0x80))
      return;

   wdttemp = (u32)context->onchip.WTCNT;
   mask = (1 << context->wdt.shift) - 1;
   wdttemp += ((cycles + context->wdt.leftover) >> context->wdt.shift);
   context->wdt.leftover = (cycles + context->wdt.leftover) & mask;

   // Are we overflowing?
   if (wdttemp > 0xFF)
   {
      // Obviously depending on whether or not we're in Watchdog or Interval
      // Modes, they'll handle an overflow differently.
      if ((context->onchip.WTCSR & 0x80)==0) {
        if (context->wdt.isinterval)
        {
          // Interval Timer Mode

          // Set OVF flag
          context->onchip.WTCSR |= 0x80;
          SH2EvaluateInterrupt(context);
        }
        else
        {
          /* Watchdog Timer Mode.
           *
           * SH7095 manual, sec. 12.2.3 and 12.3.1/12.3.5: on a WTCNT overflow
           * in watchdog mode the WOVF flag (RSTCSR bit 7) is set - it is
           * explicitly "not set in the interval timer mode" - and a WDTOVF
           * signal is emitted. "If the RSTE bit in the RSTCSR is set to 1, a
           * signal to reset the chip will be generated internally [...] Either
           * a power-on reset or a manual reset can be selected by the RSTS
           * bit."
           *
           * When RSTE is 0 the chip is not reset, but sec. 12.2.3 notes that
           * "WTCNT and WTCSR reset within WDT" all the same.
           *
           * This used to be a bare YabErrorMsg(), so a game that armed the
           * watchdog and then genuinely hung simply hung inside the emulator
           * too instead of resetting the way real hardware would. */
          context->onchip.RSTCSR |= 0x80;   // WOVF

          if (context->onchip.RSTCSR & 0x40)   // RSTE
          {
            /* RSTS (bit 5) picks power-on vs manual reset. The distinction
             * only matters for on-chip modules the Saturn does not rely on
             * here, so both are serviced by the same reset path; SH2Reset()
             * re-runs OnchipReset() and would clear WOVF, so the flag is
             * restored afterwards for the benefit of software that reads it
             * to tell a watchdog reset from a RES reset (sec. 12.3.1). */
            SH2Reset(context);
            context->onchip.RSTCSR |= 0x80;
            return;
          }
          else
          {
            context->onchip.WTCNT = 0;
            context->onchip.WTCSR = 0x18;
            context->wdt.isenable = 0;
            context->wdt.leftover = 0;
            return;
          }
        }
      }
   }

   // Write new WTCNT value
   context->onchip.WTCNT = (u8)wdttemp;
}

//////////////////////////////////////////////////////////////////////////////

void DMAExec(SH2_struct *context) {
  DMAProc(context, 200);
}

int DMAProc(SH2_struct *context, int cycles ){

   /* DMAOR (SH7095 manual, sec. 9.2.6): bit 0 DME, bit 1 NMIF, bit 2 AE,
    * bit 3 PR. NMIF and AE halt every channel, which was already handled, but
    * DME - the master enable - was never consulted: a channel whose DE bit was
    * left set would keep transferring even with the DMAC globally disabled.
    * Per sec. 9.3.1 a transfer runs only when "DE = 1, DME = 1, TE = 0"
    * (and NMIF = AE = 0). */
   if (context->onchip.DMAOR & 0x6)
      return 0;

   if (!(context->onchip.DMAOR & 0x1))
      return 0;


   if ( ((context->onchip.CHCR0 & 0x3)==0x01)  && ((context->onchip.CHCR1 & 0x3)==0x01) ) { // both channel wants DMA
      if (context->onchip.DMAOR & 0x8) { // round robin priority

        if ((context->onchip.CHCR0 & 0x08) == 0) { cycles <<= 1; } //Dual Chanel

        DMATransferCycles(context, &context->dma_ch0, cycles);
        DMATransferCycles(context, &context->dma_ch1, cycles);

       }
      else { // channel 0 > channel 1 priority

         if( (context->onchip.CHCR0 & 0x03) == 0x01 ){
           if ((context->onchip.CHCR0 & 0x08) == 0) { cycles <<= 1; } //Dual Chanel
           DMATransferCycles(context, &context->dma_ch0, cycles);
         }else if( (context->onchip.CHCR1 &0x03) == 0x01 ) {
           if ((context->onchip.CHCR1 & 0x08) == 0) { cycles <<= 1; } //Dual Chanel
           DMATransferCycles(context, &context->dma_ch1, cycles);
         }
      }
   }
   else { // only one channel wants DMA
	   if (((context->onchip.CHCR0 & 0x3) == 0x01)) { // DMA for channel 0

       if ((context->onchip.CHCR0 & 0x08) == 0) { cycles <<= 1;  } //Dual Chanel
       DMATransferCycles(context, &context->dma_ch0, cycles);
       return 0;
      }else if (((context->onchip.CHCR1 & 0x3) == 0x01)) { // DMA for channel 1
        if ((context->onchip.CHCR1 & 0x08) == 0) { cycles <<= 1; } //Dual Chanel
         DMATransferCycles(context, &context->dma_ch1, cycles);
         return 0;
      }
   }
   return 0;
}

int getEatClock(u32 src, u32 dst) {
  switch (src & 0x0FF00000) {
  case 0x05800000:
    return 1;
    break;
  case 0x05E00000: // VDP2 RAM
    switch (dst & 0x0FF00000) {
    case 0x06000000: // High
      return 44;
      break;
    case 0x00200000: // Low
      return 50;
      break;
    case 0x05A00000: // SOUND RAM
    case 0x05B00000: // SOUND REG
      return 427;
      break;
    case 0x05C00000: // VDP1 RAM
      return 427;
    case 0x05D00000: // VDP1 REG
      return 427;
      break;
    case 0x05E00000: // VDP2 RAM
      return 1;
      break;
    case 0x05F00000: // VDP2 REG
      return 50;
      break;
    default:
      return 44;
      break;
    }
    break;
  case 0x05C00000: // VDP1 RAM
    switch (dst & 0x0FF00000) {
    case 0x06000000: // High
      return 50;
      break;
    case 0x00200000: // Low
      return 50;
      break;
    case 0x05A00000: // SOUND RAM
    case 0x05B00000: // SOUND REG
      return 50;
      break;
    case 0x05C00000: // VDP1 RAM
      return 570;
    case 0x05D00000: // VDP1 REG
      return 570;
      break;
    case 0x05E00000: // VDP2 RAM
      return 225;
      break;
    case 0x05F00000: // VDP2 REG
      return 50;
      break;
    default:
      return 44;
      break;
    }
    break;
  case 0x06000000: // High
  case 0x00200000: // Low
  default:
    switch (dst & 0x0FF00000) {
    case 0x06000000: // High
    case 0x00200000: // Low
      return 14;
      break;
    case 0x05A00000: // SOUND RAM
    case 0x05B00000: // SOUND REG
      return 20;
      break;
    case 0x05C00000: // VDP1 RAM
      return 14;
    case 0x05D00000: // VDP1 REG
      return 30;
      break;
    case 0x05E00000: // VDP2 RAM
      return 82;
      break;
    case 0x05F00000: // VDP2 REG
      return 14;
      break;
    default:
      return 14;
      break;
    }
    break;
  }

  return 14;

}

void DMATransferCycles(SH2_struct *context, Dmac * dmac, int cycles ){
   int size;
   u32 i = 0;
   int count;

   //LOG("sh2 dma src=%08X,dst=%08X,%d type:%d cycle:%d\n", *dmac->SAR, *dmac->DAR, *dmac->TCR, ((*dmac->CHCR & 0x0C00) >> 10), cycles);
   if (isDMABlocked(context)) {
     return;
   }

   /* TCR is a 24-bit counter (sec. 9.2.3: "the bottom 24 bits of the 32 are
    * effective"), and a stored value of 0 means the maximum count of
    * 16,777,216 rather than "no transfer". The decrements below therefore wrap
    * within 24 bits; previously TCR = 0 underflowed to H'FFFFFFFF and the
    * "TCR <= 0" end test - which on an unsigned register only ever meant
    * "== 0" - then took roughly 2^32 iterations to be reached. */
   if (!(*dmac->CHCR & 0x2)) { // TE is not set
      int srcInc;
      int destInc;

      int type = ((*dmac->CHCR & 0x0C00) >> 10);
      int eat = getEatClock(*dmac->SAR, *dmac->DAR);

      dmac->copy_clock += cycles;

      /* SH7604 sec. 9.2.4, bits AR (CHCR.9) et TB (CHCR.4) : en requete
       * automatique et mode rafale, le DMAC acquiert le bus et le conserve
       * jusqu a TCR = 0. Le SH-2 n execute aucune instruction pendant toute
       * la duree du transfert.
       *
       * Decouper un tel transfert en tranches et laisser le CPU tourner entre
       * deux tranches permet a un jeu de reecrire son propre tampon de
       * transit alors que le DMAC est encore en train de le lire. La copie
       * reste exacte octet pour octet, mais elle melange deux blocs
       * consecutifs. Golden Axe: The Duel fait passer chaque image
       * d animation par un tampon unique et montrait exactement ce defaut :
       * sprites des combattants zebres, decor intact (issue #1280).
       *
       * Le transfert est donc mene a son terme en une seule fois. La borne
       * evite qu un TCR aberrant ne fasse deborder le budget. */
      if ((*dmac->CHCR & 0x210) == 0x210) {
         u32 remaining = *dmac->TCR & 0xFFFFFF;
         s64 budget;

         if (remaining == 0) remaining = 0x1000000;   /* TCR = 0 => maximum */
         budget = (s64)remaining * (s64)eat;
         if (budget > 0x20000000) budget = 0x20000000;
         if (budget > (s64)dmac->copy_clock) dmac->copy_clock = (int)budget;
      }

      if (dmac->copy_clock < eat) return;

      switch(*dmac->CHCR & 0x3000) {
         case 0x0000: srcInc = 0; break;
         case 0x1000: srcInc = 1; break;
         case 0x2000: srcInc = -1; break;
         default: srcInc = 0; break;
      }

      switch(*dmac->CHCR & 0xC000) {
         case 0x0000: destInc = 0; break;
         case 0x4000: destInc = 1; break;
         case 0x8000: destInc = -1; break;
         default: destInc = 0; break;
      }

      switch (type) {
         case 0:
            while( dmac->copy_clock >= 0 )  {
               dmac->copy_clock -= eat;
				       DMAMappedMemoryWriteByte(*dmac->DAR, DMAMappedMemoryReadByte(*dmac->SAR));
               *dmac->SAR += srcInc;
               *dmac->DAR += destInc;
               *dmac->TCR = (*dmac->TCR - 1) & 0xFFFFFF;
               i++;
               if( *dmac->TCR == 0 ){
                 LOG("DMA finished");
                  // Set Transfer End bit
                  *dmac->CHCR |= 0x2;
                  *dmac->CHCRM |= 0x2;
                  SH2EvaluateInterrupt(context);
                  SH2WriteNotify(context, destInc<0 ? *dmac->DAR : *dmac->DAR - i*destInc, i*abs(destInc));
                  return;
               }
            }
            break;
         case 1:
            destInc *= 2;
            srcInc *= 2;
            while (dmac->copy_clock >= 0) {
              dmac->copy_clock -= eat;
				      DMAMappedMemoryWriteWord(*dmac->DAR, DMAMappedMemoryReadWord(*dmac->SAR));
               *dmac->SAR += srcInc;
               *dmac->DAR += destInc;
               *dmac->TCR = (*dmac->TCR - 1) & 0xFFFFFF;
               i++;
               if( *dmac->TCR == 0 ){
                  LOG("DMA finished");
                  // Set Transfer End bit
                  *dmac->CHCR |= 0x2;
                  *dmac->CHCRM |= 0x2;
                  SH2EvaluateInterrupt(context);
                  SH2WriteNotify(context, destInc<0 ? *dmac->DAR : *dmac->DAR - i*destInc, i*abs(destInc));
                  return;
               }
            }
            break;
         case 2:
            destInc *= 4;
            srcInc *= 4;
            while (dmac->copy_clock >= 0) {
              dmac->copy_clock -= eat;
               u32 val = DMAMappedMemoryReadLong(*dmac->SAR);
               //printf("CPU DMA src:%08X dst:%08X val:%08X\n", *SAR, *DAR, val);
				       DMAMappedMemoryWriteLong(*dmac->DAR,val);
               *dmac->DAR += destInc;
               *dmac->SAR += srcInc;
               *dmac->TCR = (*dmac->TCR - 1) & 0xFFFFFF;
               i++;
               if( *dmac->TCR == 0 ){
                 LOG("DMA finished");
                  *dmac->CHCR |= 0x2;
                  *dmac->CHCRM |= 0x2;
                  SH2EvaluateInterrupt(context);
                  SH2WriteNotify(context, destInc<0 ? *dmac->DAR : *dmac->DAR - i*destInc, i*abs(destInc));
                  return;
               }
            }
            break;
         case 3:
           /* Transfert par blocs de 16 octets.
            *
            * SH7604 sec. 9.1.1 : un transfert 16 octets effectue quatre
            * lectures long word puis quatre ecritures long word, et le pas
            * d'adresse est +16 / -16. Jusque-la le code precedent avait
            * raison d'avancer SAR/DAR de 16.
            *
            * Mais sec. 9.2.3 (registre TCR) precise que TCR s'exprime en
            * LONG WORDS meme dans ce mode : il faut y programmer un multiple
            * de 4, et il est decremente de 4 a chaque bloc de 16 octets.
            * Le code decrementait TCR de 1 par bloc, donc chaque transfert
            * en mode 16 octets deplacait QUATRE FOIS trop de donnees et
            * ecrasait tout ce qui suivait la zone visee.
            *
            * Cas reel (Digital Ange, ecran-titre) : SAR=0x00200020,
            * DAR=0x25C10000, TCR=35840 en mode 16 octets. Avec la regle du
            * manuel cela fait 35840*4 = 0x23000 octets, donc une fin exacte
            * a 0x25C33000 -- precisement l'adresse ou le jeu a place la
            * texture du logo. Avec l'ancien decompte le transfert ecrivait
            * 35840*16 = 0x8C000 octets, debordait au-dela meme de la fin de
            * la VRAM VDP1 (0x80000) et remplissait la texture d'octets de
            * LWRAM, d'ou le logo absent.
            *
            * On garde donc le pas d'adresse de 16 et on retablit le
            * decrement de 4 sur TCR. */
           destInc *= 16;
           srcInc *= 16;
           while (dmac->copy_clock >= 0) {
             int k;
             u32 src = *dmac->SAR;
             u32 dst = *dmac->DAR;
             dmac->copy_clock -= eat;
             /* The hardware issues the four reads before the four writes; the
              * distinction only matters for overlapping source and
              * destination, which the manual prohibits anyway, so a simple
              * read/write pairing per longword is kept here. */
             for (k = 0; k < 4; k++)
               DMAMappedMemoryWriteLong(dst + (k * 4), DMAMappedMemoryReadLong(src + (k * 4)));
             *dmac->DAR += destInc;
             *dmac->SAR += srcInc;
             /* sec. 9.2.3 : -4 par bloc de 16 octets, TCR etant compte en
              * long words. Le test de fin devient "<= 0" apres masquage sur
              * 24 bits : si un jeu programme un TCR non multiple de 4 (que le
              * manuel interdit), le decrement de 4 sauterait par-dessus zero
              * et la boucle repartirait pour 2^24 blocs. */
             *dmac->TCR = (*dmac->TCR - 4) & 0xFFFFFF;
             i++;
             if (*dmac->TCR == 0 || *dmac->TCR > 0xFFFFFC) {
               *dmac->TCR = 0;
               LOG("DMA finished");
               *dmac->CHCR |= 0x2;
               *dmac->CHCRM |= 0x2;
               SH2EvaluateInterrupt(context);
               SH2WriteNotify(context, destInc<0 ? *dmac->DAR : *dmac->DAR - i*destInc, i*abs(destInc));
               return;
             }
           }
           break;
      }
      SH2WriteNotify(context, destInc<0?*dmac->DAR:*dmac->DAR-i*destInc,i*abs(destInc));
   }

}

//////////////////////////////////////////////////////////////////////////////
// Input Capture Specific
//////////////////////////////////////////////////////////////////////////////

/* MINIT (0100 0000H) et SINIT (0180 0000H) ne sont pas des registres : toute
 * ecriture dans la fenetre, quelle que soit sa taille, declenche l'input
 * capture du FRT de l'autre CPU (communication maitre/esclave par FRT ICI,
 * niveau 15). Les six points d'entree partagent donc le meme corps. */

static void InputCaptureFire(SH2_struct *target, SH2_struct *context)
{
   FRTExec(target);

   // Set Input Capture Flag
   target->onchip.FTCSR |= 0x80;
   target->onchip.FTCSRM |= 0x80;

   // Copy FRC register to FICR
   target->onchip.FICR = target->onchip.FRC.all;

   //Ensure there is some delay between input capture flag and effective interrupt handling
   //Docs says it takes around 4 instructions to accept an interrupt
   //And some games like Scorcher are using this delay to write some usefull values for the slave
   if ((context->target_cycles - context->cycles) < 10) context->target_cycles += 10;

   SH2EvaluateInterrupt(target);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL MSH2InputCaptureWriteWord(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u16 data)
{
   InputCaptureFire(MSH2, context);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL MSH2InputCaptureWriteByte(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u8 data)
{
   InputCaptureFire(MSH2, context);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL MSH2InputCaptureWriteLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u32 data)
{
   InputCaptureFire(MSH2, context);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL SSH2InputCaptureWriteWord(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u16 data)
{
   InputCaptureFire(SSH2, context);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL SSH2InputCaptureWriteByte(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u8 data)
{
   InputCaptureFire(SSH2, context);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL SSH2InputCaptureWriteLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u32 data)
{
   InputCaptureFire(SSH2, context);
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// SCI Specific
//////////////////////////////////////////////////////////////////////////////

u8 SCIReceiveByte(void) {
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void SCITransmitByte(UNUSED u8 val) {
}

//////////////////////////////////////////////////////////////////////////////

int SH2SaveState(SH2_struct *context, void ** stream)
{
   int offset;
   sh2regs_struct regs;

   // Write header
   if (context->isslave == 0)
      offset = MemStateWriteHeader(stream, "MSH2", 3);
   else
   {
      offset = MemStateWriteHeader(stream, "SSH2", 3);
      MemStateWrite((void *)&yabsys.IsSSH2Running, 1, 1, stream);
   }

   // Write registers
   SH2GetRegisters(context, &regs);
   MemStateWrite((void *)&regs, sizeof(sh2regs_struct), 1, stream);

   // Write onchip registers
   MemStateWrite((void *)&context->onchip, sizeof(Onchip_struct), 1, stream);

   // Write internal variables
   // FIXME: write the clock divisor rather than the shift amount for
   // backward compatibility (fix this next time the save state version
   // is updated)
   context->frc.shift = 1 << context->frc.shift;
   MemStateWrite((void *)&context->frc, sizeof(context->frc), 1, stream);
   {
      u32 div = context->frc.shift;
      context->frc.shift = 0;
      while ((div >>= 1) != 0)
         context->frc.shift++;
   }
   MemStateWrite((void *)context->AddressArray, sizeof(u32), 0x100, stream);
   MemStateWrite((void *)context->DataArray, sizeof(u8), 0x1000, stream);
   MemStateWrite((void *)&context->target_cycles, sizeof(u32), 1, stream);
   MemStateWrite((void *)&context->cycles, sizeof(u32), 1, stream);
   MemStateWrite((void *)&context->isslave, sizeof(u8), 1, stream);
   MemStateWrite((void *)&context->instruction, sizeof(u16), 1, stream);

   MemStateWrite((void *)&context->dma_ch0.copy_clock, sizeof(u32), 1, stream);
   MemStateWrite((void *)&context->dma_ch1.copy_clock, sizeof(u32), 1, stream);

   return MemStateFinishHeader(stream, offset);
}

//////////////////////////////////////////////////////////////////////////////

int SH2LoadState(SH2_struct *context, const void * stream, UNUSED int version, int size)
{
   sh2regs_struct regs;

   SH2Reset(context);

   if (context->isslave == 1)
      MemStateRead((void *)&yabsys.IsSSH2Running, 1, 1, stream);

   // Read registers
   MemStateRead((void *)&regs, sizeof(sh2regs_struct), 1, stream);
   SH2SetRegisters(context, &regs);

   // Read onchip registers
   if (version < 2) {
      MemStateRead((void *)&context->onchip, sizeof(Onchip_struct)-sizeof(u32)/*WTCSRM*/, 1, stream);
   }else {
     MemStateRead((void *)&context->onchip, sizeof(Onchip_struct), 1, stream);
   }

   // Read internal variables
   MemStateRead((void *)&context->frc, sizeof(context->frc), 1, stream);
   {  // FIXME: backward compatibility hack (see SH2SaveState() comment)
      u32 div = context->frc.shift;
      context->frc.shift = 0;
      while ((div >>= 1) != 0)
         context->frc.shift++;
   }
   MemStateRead((void *)context->AddressArray, sizeof(u32), 0x100, stream);
   MemStateRead((void *)context->DataArray, sizeof(u8), 0x1000, stream);
   MemStateRead((void *)&context->target_cycles, sizeof(u32), 1, stream);
   MemStateRead((void *)&context->cycles, sizeof(u32), 1, stream);
   MemStateRead((void *)&context->isslave, sizeof(u8), 1, stream);
   MemStateRead((void *)&context->instruction, sizeof(u16), 1, stream);

   if (version >= 3) {
     MemStateRead((void *)&context->dma_ch0.copy_clock, sizeof(u32), 1, stream);
     MemStateRead((void *)&context->dma_ch1.copy_clock, sizeof(u32), 1, stream);
   }

   return size;
}



void SH2DumpHistory(SH2_struct *context){

#ifdef DMPHISTORY
	FILE * history = NULL;
	history = fopen("history.txt", "w");
	if (history){
		int i;
		int index = context->pchistory_index;
		fprintf(history, "%s SH2, most recent instruction first\n\n",
		        context->isslave ? "Slave" : "Master");
		for (i = 0; i < (MAX_DMPHISTORY - 1); i++){
		  char lineBuf[128];
		  /* SH2MappedMemoryReadWord takes the context as its first argument,
		     so the old one-argument call meant this file did not compile at
		     all with DMPHISTORY defined. The register dump was commented out
		     for a related reason: regshistory was indexed with 0xFF instead
		     of MAX_DMPHISTORY - 1. */
		  u32 addr = context->pchistory[index & (MAX_DMPHISTORY - 1)];
		  SH2Disasm(addr, SH2MappedMemoryReadWord(context, addr), 0,
		            &context->regshistory[index & (MAX_DMPHISTORY - 1)], lineBuf);
		  fprintf(history, "%s\n", lineBuf);
		  index--;
	    }
		fclose(history);
	}
#endif
}

//////////////////////////////////////////////////////////////////////////////

// DEBUG stuff

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

void SH2SetBreakpointCallBack(SH2_struct *context, void (*func)(void *, u32, void *), void *userdata) {
   context->bp.BreakpointCallBack = func;
}

//////////////////////////////////////////////////////////////////////////////

int SH2AddCodeBreakpoint(SH2_struct *context, u32 addr) {
   int i;

   if (context->bp.numcodebreakpoints < MAX_BREAKPOINTS) {
      // Make sure it isn't already on the list
      for (i = 0; i < context->bp.numcodebreakpoints; i++)
      {
         if (addr == context->bp.codebreakpoint[i].addr)
            return -1;
      }

      context->bp.codebreakpoint[context->bp.numcodebreakpoints].addr = addr;
      context->bp.numcodebreakpoints++;

      return 0;
   }

   return -1;
}

//////////////////////////////////////////////////////////////////////////////

static void SH2SortCodeBreakpoints(SH2_struct *context) {
   int i, i2;
   u32 tmp;

   for (i = 0; i < (MAX_BREAKPOINTS-1); i++)
   {
      for (i2 = i+1; i2 < MAX_BREAKPOINTS; i2++)
      {
         if (context->bp.codebreakpoint[i].addr == 0xFFFFFFFF &&
             context->bp.codebreakpoint[i2].addr != 0xFFFFFFFF)
         {
            tmp = context->bp.codebreakpoint[i].addr;
            context->bp.codebreakpoint[i].addr = context->bp.codebreakpoint[i2].addr;
            context->bp.codebreakpoint[i2].addr = tmp;
         }
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

int SH2DelCodeBreakpoint(SH2_struct *context, u32 addr) {
   int i, i2;

   LOG("Deleting breakpoint %08X...\n", addr);

   if (context->bp.numcodebreakpoints > 0) {
      for (i = 0; i < context->bp.numcodebreakpoints; i++) {
         if (context->bp.codebreakpoint[i].addr == addr)
         {
            context->bp.codebreakpoint[i].addr = 0xFFFFFFFF;
            SH2SortCodeBreakpoints(context);
            context->bp.numcodebreakpoints--;

            LOG("Remaining breakpoints: \n");

            for (i2 = 0; i2 < context->bp.numcodebreakpoints; i2++)
            {
               LOG("%08X", context->bp.codebreakpoint[i2].addr);
            }

            return 0;
         }
      }
   }

   LOG("Failed deleting breakpoint\n");

   return -1;
}

//////////////////////////////////////////////////////////////////////////////

codebreakpoint_struct *SH2GetBreakpointList(SH2_struct *context) {
   return context->bp.codebreakpoint;
}

//////////////////////////////////////////////////////////////////////////////

void SH2ClearCodeBreakpoints(SH2_struct *context) {
   int i;
   for (i = 0; i < MAX_BREAKPOINTS; i++) {
      context->bp.codebreakpoint[i].addr = 0xFFFFFFFF;
   }

   context->bp.numcodebreakpoints = 0;
}

//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL SH2MemoryBreakpointReadByte(SH2_struct *sh, u8* mem, u32 addr) {
   int i;
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (sh->bp.memorybreakpoint[i].addr == (addr & 0x0FFFFFFF))
      {
         if (sh->bp.BreakpointCallBack && sh->bp.inbreakpoint == 0)
         {
            sh->bp.inbreakpoint = 1;
            sh->bp.BreakpointUserData.PCAddress = (sh->isDelayed != 0)?sh->isDelayed:sh->regs.PC;
            sh->bp.BreakpointUserData.BPAddress = addr;
         }

         return sh->bp.memorybreakpoint[i].oldreadbyte(sh, mem, addr);
      }
   }

   // Use the closest match if address doesn't match
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (((sh->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
         return sh->bp.memorybreakpoint[i].oldreadbyte(sh, mem, addr);
   }
   SH2_struct *otherSH = (sh == MSH2)?SSH2:MSH2;
   // the breakpoint might have been set for the other core.
   for (i = 0; i < otherSH->bp.nummemorybreakpoints; i++)
   {
      if (((otherSH->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
         return otherSH->bp.memorybreakpoint[i].oldreadbyte(sh, mem, addr);
   }
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL SH2MemoryBreakpointReadWord(SH2_struct *sh, u8* mem, u32 addr) {
   int i;
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (sh->bp.memorybreakpoint[i].addr == (addr & 0x0FFFFFFF))
      {
         if (sh->bp.BreakpointCallBack && sh->bp.inbreakpoint == 0)
         {
            sh->bp.inbreakpoint = 1;
            sh->bp.BreakpointUserData.PCAddress = (sh->isDelayed != 0)?sh->isDelayed:0Xcafedead;
            sh->bp.BreakpointUserData.BPAddress = addr;
         }
         return sh->bp.memorybreakpoint[i].oldreadword(sh, mem, addr);
      }
   }

   // Use the closest match if address doesn't match
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (((sh->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
         return sh->bp.memorybreakpoint[i].oldreadword(sh, mem, addr);
   }
   SH2_struct *otherSH = (sh == MSH2)?SSH2:MSH2;
   // the breakpoint might have been set for the other core.
   for (i = 0; i < otherSH->bp.nummemorybreakpoints; i++)
   {
      if (((otherSH->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
         return otherSH->bp.memorybreakpoint[i].oldreadword(sh, mem, addr);
   }
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL SH2MemoryBreakpointReadLong(SH2_struct *sh, u8* mem, u32 addr) {
   int i;
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (sh->bp.memorybreakpoint[i].addr == (addr & 0x0FFFFFFF))
      {
         if (sh->bp.BreakpointCallBack && sh->bp.inbreakpoint == 0)
         {
            sh->bp.inbreakpoint = 1;
            sh->bp.BreakpointUserData.PCAddress = (sh->isDelayed != 0)?sh->isDelayed:sh->regs.PC;
            sh->bp.BreakpointUserData.BPAddress = addr;
         }
         return sh->bp.memorybreakpoint[i].oldreadlong(sh, mem, addr);
      }
   }

   // Use the closest match if address doesn't match
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (((sh->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
         return sh->bp.memorybreakpoint[i].oldreadlong(sh, mem, addr);
   }
   SH2_struct *otherSH = (sh == MSH2)?SSH2:MSH2;
   // the breakpoint might have been set for the other core.
   for (i = 0; i < otherSH->bp.nummemorybreakpoints; i++)
   {
      if (((otherSH->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
         return otherSH->bp.memorybreakpoint[i].oldreadlong(sh, mem, addr);
   }
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL SH2MemoryBreakpointWriteByte(SH2_struct *sh, u8* mem, u32 addr, u8 val) {
   int i;
   SH2WriteNotify(MSH2, addr, 1);
   SH2WriteNotify(SSH2, addr, 1);
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (sh->bp.memorybreakpoint[i].addr == (addr & 0x0FFFFFFF))
      {
         if (sh->bp.BreakpointCallBack && sh->bp.inbreakpoint == 0)
         {
            sh->bp.inbreakpoint = 1;
            sh->bp.BreakpointUserData.PCAddress = (sh->isDelayed != 0)?sh->isDelayed:sh->regs.PC;
            sh->bp.BreakpointUserData.BPAddress = addr;
         }

         sh->bp.memorybreakpoint[i].oldwritebyte(sh, mem, addr, val);
         return;
      }
   }

   // Use the closest match if address doesn't match
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (((sh->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
      {
         sh->bp.memorybreakpoint[i].oldwritebyte(sh, mem, addr, val);
         return;
      }
   }
   SH2_struct *otherSH = (sh == MSH2)?SSH2:MSH2;
   // the breakpoint might have been set for the other core.
   for (i = 0; i < otherSH->bp.nummemorybreakpoints; i++)
   {
     if (((otherSH->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
     {
        otherSH->bp.memorybreakpoint[i].oldwritebyte(sh, mem, addr, val);
        return;
     }
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL SH2MemoryBreakpointWriteWord(SH2_struct *sh, u8* mem, u32 addr, u16 val) {
   int i;
    SH2WriteNotify(MSH2, addr, 2);
    SH2WriteNotify(SSH2, addr, 2);
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (sh->bp.memorybreakpoint[i].addr == (addr & 0x0FFFFFFF))
      {
         if (sh->bp.BreakpointCallBack && sh->bp.inbreakpoint == 0)
         {
            sh->bp.inbreakpoint = 1;
            sh->bp.BreakpointUserData.PCAddress = (sh->isDelayed != 0)?sh->isDelayed:sh->regs.PC;
            sh->bp.BreakpointUserData.BPAddress = addr;
         }

         sh->bp.memorybreakpoint[i].oldwriteword(sh, mem, addr, val);
         return;
      }
   }

   // Use the closest match if address doesn't match
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (((sh->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
      {
         sh->bp.memorybreakpoint[i].oldwriteword(sh, mem, addr, val);
         return;
      }
   }
   SH2_struct *otherSH = (sh == MSH2)?SSH2:MSH2;
   // the breakpoint might have been set for the other core.
   for (i = 0; i < otherSH->bp.nummemorybreakpoints; i++)
   {
     if (((otherSH->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
     {
        otherSH->bp.memorybreakpoint[i].oldwriteword(sh, mem, addr, val);
        return;
     }
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL SH2MemoryBreakpointWriteLong(SH2_struct *sh, u8* mem, u32 addr, u32 val) {
   int i;
   SH2WriteNotify(MSH2, addr, 4);
   SH2WriteNotify(SSH2, addr, 4);
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (sh->bp.memorybreakpoint[i].addr == (addr & 0x0FFFFFFF))
      {
         if (sh->bp.BreakpointCallBack && sh->bp.inbreakpoint == 0)
         {
            sh->bp.inbreakpoint = 1;
            sh->bp.BreakpointUserData.PCAddress = (sh->isDelayed != 0)?sh->isDelayed:sh->regs.PC;
            sh->bp.BreakpointUserData.BPAddress = addr;
         }

         sh->bp.memorybreakpoint[i].oldwritelong(sh, mem, addr, val);
         return;
      }
   }

   // Use the closest match if address doesn't match
   for (i = 0; i < sh->bp.nummemorybreakpoints; i++)
   {
      if (((sh->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
      {
         sh->bp.memorybreakpoint[i].oldwritelong(sh, mem, addr, val);
         return;
      }
   }
   SH2_struct *otherSH = (sh == MSH2)?SSH2:MSH2;
   // the breakpoint might have been set for the other core.
   for (i = 0; i < otherSH->bp.nummemorybreakpoints; i++)
   {
     if (((otherSH->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) == ((addr >> 16) & 0xFFF))
     {
        otherSH->bp.memorybreakpoint[i].oldwritelong(sh, mem, addr, val);
        return;
     }
   }
}

//////////////////////////////////////////////////////////////////////////////

/* The memory access tables swapped in when a memory breakpoint is installed
   (ReadByteList, WriteLongList and friends) are GLOBAL: one set shared by
   both SH2s. This duplicate check, however, only ever looked at the list
   belonging to `context`.

   Installing a breakpoint on the same 64 KB page for the second CPU, or
   re-installing one after a delete restored the tables, therefore found no
   duplicate in its own empty list, reinstalled the handler, and saved as
   `oldwritelong` whatever was already in the global table: the breakpoint
   handler itself. The first write to that page then called the handler,
   which called `oldwritelong`, which was the handler, for as long as the
   stack lasted. The emulator hung with no message, typically on the first
   screen that touched the page -- which for a breakpoint anywhere near GBR
   means it never got past the BIOS.

   Scanning both cores closes it. `owner` reports which core holds the
   duplicate so the caller copies the saved pointer from the right list. */
static int CheckForMemoryBreakpointDupesIn(SH2_struct *context, u32 addr, u32 flag, int *which)
{
   int i;

   if (context == NULL) return 0;

   for (i = 0; i < context->bp.nummemorybreakpoints; i++)
   {
      if (((context->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) ==
          ((addr >> 16) & 0xFFF))
      {
         // See it actually was using the same operation flag
         if (context->bp.memorybreakpoint[i].flags & flag)
         {
            *which = i;
            return 1;
         }
      }
   }

   return 0;
}

static int CheckForMemoryBreakpointDupesBoth(SH2_struct *context, u32 addr, u32 flag,
                                             int *which, SH2_struct **owner)
{
   SH2_struct *other = (context == MSH2) ? SSH2 : MSH2;

   if (CheckForMemoryBreakpointDupesIn(context, addr, flag, which))
   {
      *owner = context;
      return 1;
   }
   if (CheckForMemoryBreakpointDupesIn(other, addr, flag, which))
   {
      *owner = other;
      return 1;
   }
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int SH2AddMemoryBreakpoint(SH2_struct *context, u32 addr, u32 flags) {
   int which;
   int i;
   SH2_struct *owner = context;

   if (flags == 0)
      return -1;

   if (context->bp.nummemorybreakpoints < MAX_BREAKPOINTS) {
      // Only regular addresses are supported at this point(Sorry, no onchip!)
      switch (addr >> 29) {
         case 0x0:
         case 0x1:
         case 0x5:
            break;
         default:
            return -1;
      }

      addr &= 0x0FFFFFFF;

      // Make sure it isn't already on the list
      for (i = 0; i < context->bp.nummemorybreakpoints; i++)
      {
         if (addr == context->bp.memorybreakpoint[i].addr)
            return -1;
      }

      context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].addr = addr;
      context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].flags = flags;

      context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldreadbyte = ReadByteList[(addr >> 16) & 0xFFF];
      context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldreadword = ReadWordList[(addr >> 16) & 0xFFF];
      context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldreadlong = ReadLongList[(addr >> 16) & 0xFFF];
      context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldwritebyte = WriteByteList[(addr >> 16) & 0xFFF];
      context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldwriteword = WriteWordList[(addr >> 16) & 0xFFF];
      context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldwritelong = WriteLongList[(addr >> 16) & 0xFFF];

      if (flags & BREAK_BYTEREAD)
      {
         // Make sure function isn't already being breakpointed by another breakpoint
         if (!CheckForMemoryBreakpointDupesBoth(context, addr, BREAK_BYTEREAD, &which, &owner))
            ReadByteList[(addr >> 16) & 0xFFF] = CacheReadByteList[(addr >> 16) & 0xFFF] = &SH2MemoryBreakpointReadByte;
         else
            // fix old memory access function, taken from whichever core owns it
            context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldreadbyte = owner->bp.memorybreakpoint[which].oldreadbyte;
      }

      if (flags & BREAK_WORDREAD)
      {
         // Make sure function isn't already being breakpointed by another breakpoint
         if (!CheckForMemoryBreakpointDupesBoth(context, addr, BREAK_WORDREAD, &which, &owner))
            ReadWordList[(addr >> 16) & 0xFFF] = CacheReadWordList[(addr >> 16) & 0xFFF] = &SH2MemoryBreakpointReadWord;
         else
            // fix old memory access function, taken from whichever core owns it
            context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldreadword = owner->bp.memorybreakpoint[which].oldreadword;
      }

      if (flags & BREAK_LONGREAD)
      {
         // Make sure function isn't already being breakpointed by another breakpoint
         if (!CheckForMemoryBreakpointDupesBoth(context, addr, BREAK_LONGREAD, &which, &owner))
            ReadLongList[(addr >> 16) & 0xFFF] = CacheReadLongList[(addr >> 16) & 0xFFF] = &SH2MemoryBreakpointReadLong;
         else
            // fix old memory access function, taken from whichever core owns it
            context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldreadlong = owner->bp.memorybreakpoint[which].oldreadlong;
      }

      if (flags & BREAK_BYTEWRITE)
      {
         // Make sure function isn't already being breakpointed by another breakpoint
         if (!CheckForMemoryBreakpointDupesBoth(context, addr, BREAK_BYTEWRITE, &which, &owner))
            WriteByteList[(addr >> 16) & 0xFFF] = CacheWriteByteList[(addr >> 16) & 0xFFF] = &SH2MemoryBreakpointWriteByte;
         else
            // fix old memory access function, taken from whichever core owns it
            context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldwritebyte = owner->bp.memorybreakpoint[which].oldwritebyte;
      }

      if (flags & BREAK_WORDWRITE)
      {
         // Make sure function isn't already being breakpointed by another breakpoint
         if (!CheckForMemoryBreakpointDupesBoth(context, addr, BREAK_WORDWRITE, &which, &owner))
            WriteWordList[(addr >> 16) & 0xFFF] = CacheWriteWordList[(addr >> 16) & 0xFFF] = &SH2MemoryBreakpointWriteWord;
         else
            // fix old memory access function, taken from whichever core owns it
            context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldwriteword = owner->bp.memorybreakpoint[which].oldwriteword;
      }

      if (flags & BREAK_LONGWRITE)
      {
         // Make sure function isn't already being breakpointed by another breakpoint
         if (!CheckForMemoryBreakpointDupesBoth(context, addr, BREAK_LONGWRITE, &which, &owner))
           WriteLongList[(addr >> 16) & 0xFFF] = CacheWriteLongList[(addr >> 16) & 0xFFF] = &SH2MemoryBreakpointWriteLong;
        else
           // fix old memory access function, taken from whichever core owns it
           context->bp.memorybreakpoint[context->bp.nummemorybreakpoints].oldwritelong = owner->bp.memorybreakpoint[which].oldwritelong;
      }

      context->bp.nummemorybreakpoints++;

      return 0;
   }

   return -1;
}

//////////////////////////////////////////////////////////////////////////////

static void SH2SortMemoryBreakpoints(SH2_struct *context) {
   int i, i2;
   memorybreakpoint_struct tmp;

   for (i = 0; i < (MAX_BREAKPOINTS-1); i++)
   {
      for (i2 = i+1; i2 < MAX_BREAKPOINTS; i2++)
      {
         if (context->bp.memorybreakpoint[i].addr == 0xFFFFFFFF &&
             context->bp.memorybreakpoint[i2].addr != 0xFFFFFFFF)
         {
            memcpy(&tmp, context->bp.memorybreakpoint+i, sizeof(memorybreakpoint_struct));
            memcpy(context->bp.memorybreakpoint+i, context->bp.memorybreakpoint+i2, sizeof(memorybreakpoint_struct));
            memcpy(context->bp.memorybreakpoint+i2, &tmp, sizeof(memorybreakpoint_struct));
         }
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

int SH2DelMemoryBreakpoint(SH2_struct *context, u32 addr) {
   int i, i2;

   if (context->bp.nummemorybreakpoints > 0) {
      for (i = 0; i < context->bp.nummemorybreakpoints; i++) {
         if (context->bp.memorybreakpoint[i].addr == addr)
         {
            // Remove memory access piggyback function to memory access function table

            // Make sure no other breakpoints need the breakpoint functions first
            for (i2 = 0; i2 < context->bp.nummemorybreakpoints; i2++)
            {
               if (((context->bp.memorybreakpoint[i].addr >> 16) & 0xFFF) ==
                   ((context->bp.memorybreakpoint[i2].addr >> 16) & 0xFFF) &&
                   i != i2)
               {
                  // Clear the flags
                  context->bp.memorybreakpoint[i].flags &= ~context->bp.memorybreakpoint[i2].flags;
               }
            }

            if (context->bp.memorybreakpoint[i].flags & BREAK_BYTEREAD)
               ReadByteList[(addr >> 16) & 0xFFF] = CacheReadByteList[(addr >> 16) & 0xFFF] = context->bp.memorybreakpoint[i].oldreadbyte;

            if (context->bp.memorybreakpoint[i].flags & BREAK_WORDREAD)
               ReadWordList[(addr >> 16) & 0xFFF] = CacheReadWordList[(addr >> 16) & 0xFFF] = context->bp.memorybreakpoint[i].oldreadword;

            if (context->bp.memorybreakpoint[i].flags & BREAK_LONGREAD)
               ReadLongList[(addr >> 16) & 0xFFF] = CacheReadLongList[(addr >> 16) & 0xFFF] = context->bp.memorybreakpoint[i].oldreadlong;

            if (context->bp.memorybreakpoint[i].flags & BREAK_BYTEWRITE)
               WriteByteList[(addr >> 16) & 0xFFF] = CacheWriteByteList[(addr >> 16) & 0xFFF] = context->bp.memorybreakpoint[i].oldwritebyte;

            if (context->bp.memorybreakpoint[i].flags & BREAK_WORDWRITE)
               WriteWordList[(addr >> 16) & 0xFFF] = CacheWriteWordList[(addr >> 16) & 0xFFF] = context->bp.memorybreakpoint[i].oldwriteword;

            if (context->bp.memorybreakpoint[i].flags & BREAK_LONGWRITE) {
              WriteLongList[(addr >> 16) & 0xFFF] = CacheWriteLongList[(addr >> 16) & 0xFFF] = context->bp.memorybreakpoint[i].oldwritelong;

            }

            context->bp.memorybreakpoint[i].addr = 0xFFFFFFFF;
            SH2SortMemoryBreakpoints(context);
            context->bp.nummemorybreakpoints--;
            return 0;
         }
      }
   }

   return -1;
}

//////////////////////////////////////////////////////////////////////////////

memorybreakpoint_struct *SH2GetMemoryBreakpointList(SH2_struct *context) {
   return context->bp.memorybreakpoint;
}

//////////////////////////////////////////////////////////////////////////////

void SH2ClearMemoryBreakpoints(SH2_struct *context) {
   int i;
   for (i = 0; i < MAX_BREAKPOINTS; i++)
   {
      context->bp.memorybreakpoint[i].addr = 0xFFFFFFFF;
      context->bp.memorybreakpoint[i].flags = 0;
      context->bp.memorybreakpoint[i].oldreadbyte = NULL;
      context->bp.memorybreakpoint[i].oldreadword = NULL;
      context->bp.memorybreakpoint[i].oldreadlong = NULL;
      context->bp.memorybreakpoint[i].oldwritebyte = NULL;
      context->bp.memorybreakpoint[i].oldwriteword = NULL;
      context->bp.memorybreakpoint[i].oldwritelong = NULL;
   }
   context->bp.nummemorybreakpoints = 0;
}

//////////////////////////////////////////////////////////////////////////////

void SH2HandleBackTrace(SH2_struct *context)
{
   u16 inst = context->instruction;
   if ((inst & 0xF000) == 0xB000 || // BSR
      (inst & 0xF0FF) == 0x0003 || // BSRF
      (inst & 0xF0FF) == 0x400B)   // JSR
   {
      if (context->bt.numbacktrace < sizeof(context->bt.addr)/sizeof(u32))
      {
         context->bt.addr[context->bt.numbacktrace] = context->regs.PC;
         context->bt.numbacktrace++;
      }
   }
   else if ((inst == 0x000B) || // RTS
            (inst == 0x002B)) //RTE
   {
      if (context->bt.numbacktrace > 0)
         context->bt.numbacktrace--;
   }
}

//////////////////////////////////////////////////////////////////////////////

u32 *SH2GetBacktraceList(SH2_struct *context, int *size)
{
   *size = context->bt.numbacktrace;
   return context->bt.addr;
}
