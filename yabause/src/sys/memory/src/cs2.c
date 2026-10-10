/*  Copyright 2003 Guillaume Duhamel
    Copyright 2004-2006, 2013 Theo Berkau

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

/*! \file cs2.c
    \brief A-bus CS2 emulation functions. Mainly CD-Block code.
*/

#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include "cs2.h"
#include "sh2core.h"
#include "debug.h"
#include "error.h"
#include "mpegcard.h"
#include "japmodem.h"
#include "netlink.h"
#include "scsp.h"
#include "scu.h"
#include "smpc.h"
#include "yui.h"
#include "db.h"

#define CDB_HIRQ_CMOK      0x0001
#define CDB_HIRQ_DRDY      0x0002
#define CDB_HIRQ_CSCT      0x0004
#define CDB_HIRQ_BFUL      0x0008
#define CDB_HIRQ_PEND      0x0010
#define CDB_HIRQ_DCHG      0x0020
#define CDB_HIRQ_ESEL      0x0040
#define CDB_HIRQ_EHST      0x0080
#define CDB_HIRQ_ECPY      0x0100
#define CDB_HIRQ_EFLS      0x0200
#define CDB_HIRQ_SCDQ      0x0400
#define CDB_HIRQ_MPED      0x0800
#define CDB_HIRQ_MPCM      0x1000
#define CDB_HIRQ_MPST      0x2000

#define CDB_STAT_BUSY      0x00
#define CDB_STAT_PAUSE     0x01
#define CDB_STAT_STANDBY   0x02
#define CDB_STAT_PLAY      0x03
#define CDB_STAT_SEEK      0x04
#define CDB_STAT_SCAN      0x05
#define CDB_STAT_OPEN      0x06
#define CDB_STAT_NODISC    0x07
#define CDB_STAT_RETRY     0x08
#define CDB_STAT_ERROR     0x09
#define CDB_STAT_FATAL     0x0A
#define CDB_STAT_PERI      0x20
#define CDB_STAT_TRNS      0x40
#define CDB_STAT_WAIT      0x80
#define CDB_STAT_REJECT    0xFF

#define CDB_PLAYTYPE_SECTOR     0x01
#define CDB_PLAYTYPE_FILE       0x02

// #define CDLOG YuiMsg

// Video CD Card traces (handshake, MPEG commands 0x90-0xAF, CD-side commands
// that route sectors to the decoder), through CDLOG like the rest of the
// CD block: compiled in only for debug builds.
#define CS2_MPEG_TRACE(...) CDLOG(__VA_ARGS__)
#define CS2_MPEG_CMD_TRACE(name) CDLOG("MPEG card: %s CR1=%04X CR2=%04X CR3=%04X CR4=%04X\n", name, \
   Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4)
#define CS2_CD_TRACE(name) CDLOG("MPEG card: [CD] %s CR1=%04X CR2=%04X CR3=%04X CR4=%04X\n", name, \
   Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4)

// Set once the host has issued MPEG Set Connection (0x9A): from then on the
// buffer partitions it named are fed to the decoder (Cs2MpegDecoderPump()).
// Kept out of Cs2Area on purpose so the savestate layout doesn't change.
static int Cs2MpegConnSet = 0;


// Set by MPEG Play (0x95), cleared by MPEG Init (0x93). Drives the MPEG
// status report below.
static int Cs2MpegPlaying = 0;



// Put Sector Data (64h): sectors being written by the host, routed through
// filter Cs2PutFilter at End Data Transfer.
static block_struct *Cs2PutBlk[MAX_BLOCKS];
static u8 Cs2PutBlkNum[MAX_BLOCKS];
static u32 Cs2PutCount = 0;
static u32 Cs2PutBase = 24;
static u8 Cs2PutFilter = 0;

// Video CD Card shown to Video CDs and to the games of db.c only (see
// Cs2IsMpegCardPresent()).
static void Cs2MpegDiscInvalidate(void);

// MPEG Set Decoding Method (0x96, CDC_MpSetDec, CD Communication Interface
// MPEG part 20.8). pautim: 0000H pause (also "re-pause": one picture forward
// when already paused), 0001H release, 0002H-FFFEH slow playback (picture
// interval until the next pause), FFFFH no change. frztim: same scheme for
// the displayed picture (freeze / strobe). mute: bit 0 right, bit 1 left.
// Initial values (manual): mute 00H, pautim 0000H (paused), frztim 0000H
// (frozen); MPEG Init restores them.
static u16 Cs2MpegPauTim = 0;
static u16 Cs2MpegFrzTim = 0;
static int Cs2MpegStep = 0;        // pictures to advance while paused
static u32 Cs2MpegSlowCount = 0;
static void Cs2MpegApplyDecodingMethod(u8 mute, u16 pautim, u16 frztim, int init);

// CD Scan (0x12): -1 = not scanning, 0 = forward, 1 = reverse.
static int Cs2ScanMode = -1;
static int Cs2ScanCounter = 0;
void Cs2MpegUpdateStatus(void);


// Pending MPEG interrupt factors, reported by MPEG Get Interrupt (0x91).
static u32 Cs2MpegIntPending = 0;



extern void resetSyncVideo(void);

//////////////////////////////////////////////////////////////////////////////
// Modele de temps de recherche (seek) du bloc optique
//
// Formule de Mednafen (ss/cdb.c, DRIVEPHASE_SEEK_START3), dans son unite de
// 44100*256 par seconde :
//   12 secteurs a double vitesse (80 ms, reaccrochage)
//   + 26 unites par secteur de distance vers l'avant, 28 vers l'arriere
//   + 1 secteur si l'on recule ou si l'on saute 150 secteurs ou plus.
// Soit environ 87 ms pour une recherche courte.
//
// Le modele precedent (b2facad, geometrie de la spirale, 20 ms a 100 ms)
// cherchait environ quatre fois plus vite que Mednafen. Tant que les donnees
// etaient lues en 1x (voir Cs2SetTiming()), la lenteur de la lecture le
// masquait ; en 2x, la chronologie de 3D Mission Shooting se compressait
// davantage que sur la console : le lecteur video demarrait avant que le
// reste du jeu soit pret, prenait un chemin de reprise (060D7E90) et
// appelait une routine absente (060ECE74), d'ou un plantage dans la boucle
// d'arret du BIOS au lieu de la video.
//
// Un FAD invalide (0xFFFFFFFF apres un Stop) est traite comme le bord
// exterieur, comme le faisait le modele precedent.
//////////////////////////////////////////////////////////////////////////////
#define CS2_SEEK_FAD_MAX        333000u   /* 74 min * 4500 FAD/min           */

/* _periodictiming est exprime en microsecondes * 3 (cf. Cs2Exec_unit) */
#define CS2_US_TO_PERIODIC(us)  ((u32)(us) * 3u)

// Duree du seek entre deux FAD, en unites de _periodictiming (us * 3).
static u32 Cs2ComputeSeekTiming(u32 from_fad, u32 to_fad)
{
   const double unit_us = 1000000.0 / (44100.0 * 256.0);
   const double sector  = (44100.0 * 256.0) / 150.0;   /* 1 secteur a 2x */
   double units;
   long delta;

   if (from_fad > CS2_SEEK_FAD_MAX) from_fad = CS2_SEEK_FAD_MAX;
   if (to_fad > CS2_SEEK_FAD_MAX)   to_fad   = CS2_SEEK_FAD_MAX;

   delta = (long)from_fad - (long)to_fad;   /* Mednafen : CurPosInfo.fad - CurSector */
   units  = 12.0 * sector;
   units += (double)labs(delta) * ((delta < 0) ? 28.0 : 26.0);
   units += (delta < 0 || delta >= 150) ? sector : 0.0;

   return CS2_US_TO_PERIODIC((u32)(units * unit_us));
}


enum CDB_DATATRANSTYPE
{
   CDB_DATATRANSTYPE_INVALID=-1,
   CDB_DATATRANSTYPE_GETSECTOR=0,
   CDB_DATATRANSTYPE_GETDELSECTOR=2,
   CDB_DATATRANSTYPE_PUTSECTOR=3
};

#define ToBCD(val) ((val % 10 ) + ((val / 10 ) << 4))

Cs2 * Cs2Area = NULL;
ip_struct *cdip = NULL;

extern CDInterface *CDCoreList[];

//////////////////////////////////////////////////////////////////////////////

static INLINE void setBusyStatus(u8 status) {
  Cs2Area->status = CDB_STAT_BUSY;
  Cs2Area->nextStatus = status;
}
static INLINE void setStatus(u8 status) {
  Cs2Area->status = status;
}

static INLINE void doCDReport(u8 status)
{
   Cs2Area->reg.CR1 = (status << 8) | ((Cs2Area->options & 0xF) << 4) | (Cs2Area->repcnt & 0xF);
   Cs2Area->reg.CR2 = (Cs2Area->ctrladdr << 8) | Cs2Area->track;
   Cs2Area->reg.CR3 = (u16)((Cs2Area->index << 8) | ((Cs2Area->FAD >> 16) & 0xFF));
   Cs2Area->reg.CR4 = (u16) Cs2Area->FAD;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void doMPEGReport(u8 status)
{
   Cs2Area->reg.CR1 = (status << 8) | Cs2Area->actionstatus;
   Cs2Area->reg.CR2 = Cs2Area->vcounter;
   Cs2Area->reg.CR3 = (Cs2Area->pictureinfo << 8) | Cs2Area->mpegaudiostatus;
   Cs2Area->reg.CR4 = Cs2Area->mpegvideostatus;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void Cs2SetIRQ(u32 irq){
  Cs2Area->reg.HIRQ |= irq;
  if (Cs2Area->reg.HIRQ & Cs2Area->reg.HIRQMASK){
    ScuSendExternalInterrupt00();
  }
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// A-bus CS2 access time for the SH-2.
//
// Kronos charged nothing for an SH-2 access to the CD block, so a register
// poll cost only the instruction cycles. Mednafen (ss/scu.inc,
// ABusRW_DB_u16_W0_SH0/SH1, "A-Bus CS2") adds 8 cycles per 16-bit bus access
// in 0x05800000-0x058FFFFF; an SH-2 read always goes through ABus_Read
// (both 16-bit halves, 16 cycles, whatever the size), a byte or word write
// is one half (8 cycles), a long write both halves (16 cycles). DMA accesses
// (context == NULL) are not charged here.
//
// Bounded poll loops in CD-block libraries depend on it: the file library
// of the Video CD Card player waits for SCDQ with 24372 reads of HIRQ
// (06068BDC in the card program). Without the bus time that loop gave up
// after ~13 ms, shorter than the 16.7 ms periodic SCDQ interval of a seek,
// so the read of the ISO volume descriptor failed now and then; the player
// then took another start-up path that never sent the Play Disc of the
// movie. With the bus time the loop lasts ~27 ms.
#define CS2_ABUS_READ_CYCLES     16
#define CS2_ABUS_WRITE16_CYCLES   8
#define CS2_ABUS_WRITE32_CYCLES  16

static INLINE void Cs2ABusCost(SH2_struct *context, u32 cycles)
{
   if (context != NULL)
      context->cycles += cycles;
}

u8 FASTCALL Cs2ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   Cs2ABusCost(context, CS2_ABUS_READ_CYCLES);
   return CartridgeArea->Cs2ReadByte(context, memory, addr);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Cs2WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   Cs2ABusCost(context, CS2_ABUS_WRITE16_CYCLES);
   CartridgeArea->Cs2WriteByte(context, memory, addr, val);
}

//////////////////////////////////////////////////////////////////////////////

u16 FASTCALL Cs2ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr) {
  u16 val = 0;
   Cs2ABusCost(context, CS2_ABUS_READ_CYCLES);
  addr &= 0x3F; // fix me(I should really have proper mapping)

  switch(addr) {
    case 0x08:
    case 0x0A:
                  val = Cs2Area->reg.HIRQ;

                  //if (Cs2Area->isbufferfull)
                  //  val |= CDB_HIRQ_BFUL;
                  //else
                  //  val &= ~CDB_HIRQ_BFUL;

                  //if (Cs2Area->isdiskchanged)
                  //  val |= CDB_HIRQ_DCHG;
                  //else
                  //  val &= ~CDB_HIRQ_DCHG;

                  //if (Cs2Area->isonesectorstored)
                  //  val |= CDB_HIRQ_CSCT;
                  //else
                  //  val &= ~CDB_HIRQ_CSCT;

                  Cs2Area->reg.HIRQ = val;

//                  CDLOG("cs2\t: Hirq read, Hirq mask = %x - ret: %x\n", Memory::getWord(0x9000C), val);
                  return val;
    case 0x0C:
    case 0x0E: return Cs2Area->reg.HIRQMASK;
    case 0x18:
    case 0x1A: return Cs2Area->reg.CR1;
    case 0x1C:
    case 0x1E: return Cs2Area->reg.CR2;
    case 0x20:
    case 0x22: return Cs2Area->reg.CR3;
    case 0x24:
    case 0x26: Cs2Area->_command = 0;
                  return Cs2Area->reg.CR4;
    case 0x28:
    case 0x2A: return Cs2Area->reg.MPEGRGB;
    case 0x00:
                  // transfer info
                  switch (Cs2Area->infotranstype) {
                     case 0:
                             // Get Toc Data
                             if (Cs2Area->transfercount % 4 == 0)
                                val = (u16)((Cs2Area->TOC[Cs2Area->transfercount >> 2] & 0xFFFF0000) >> 16);
                             else
                                val = (u16)Cs2Area->TOC[Cs2Area->transfercount >> 2];

                             Cs2Area->transfercount += 2;
                             Cs2Area->cdwnum += 2;

                             if (Cs2Area->transfercount >= (0xCC * 2)) // >= : TOC = 0xCC mots, evite OOB TOC[102]
                             {
                                Cs2Area->transfercount = 0;
                                Cs2Area->infotranstype = -1;
                             }
                             break;
                     case 1:
                             // Get File Info(1 file info)
                             val = (Cs2Area->transfileinfo[Cs2Area->transfercount] << 8) |
                                    Cs2Area->transfileinfo[Cs2Area->transfercount + 1];
                             Cs2Area->transfercount += 2;
                             Cs2Area->cdwnum += 2;

                             if (Cs2Area->transfercount >= (0x6 * 2)) // >= : 6 mots, evite OOB transfileinfo[12]
                             {
                                Cs2Area->transfercount = 0;
                                Cs2Area->infotranstype = -1;
                             }

                             break;
                     case 2:
                             // Get File Info(254 file info)

                             // Do we need to retrieve the next file info?
                             if (Cs2Area->transfercount % (0x6 * 2) == 0) {
                               // yes we do
                               Cs2SetupFileInfoTransfer(2 + (Cs2Area->transfercount / (0x6 * 2)));
                             }

                             val = (Cs2Area->transfileinfo[Cs2Area->transfercount % (0x6 * 2)] << 8) |
                                    Cs2Area->transfileinfo[Cs2Area->transfercount % (0x6 * 2) + 1];

                             Cs2Area->transfercount += 2;
                             Cs2Area->cdwnum += 2;

                             if (Cs2Area->transfercount >= (254 * (0x6 * 2))) // >= : 254 fichiers, evite SetupFileInfoTransfer(256)/fileinfo[256] OOB
                             {
                                Cs2Area->transfercount = 0;
                                Cs2Area->infotranstype = -1;
                             }

                             break;
                     case 3:
                             // Get Subcode Q
                             val = (Cs2Area->transscodeq[Cs2Area->transfercount] << 8) |
                                    Cs2Area->transscodeq[Cs2Area->transfercount + 1];

                             Cs2Area->transfercount += 2;
                             Cs2Area->cdwnum += 2;

                             if (Cs2Area->transfercount >= (5 * 2)) // >= : 5 mots, evite OOB transscodeq[10]
                             {
                                Cs2Area->transfercount = 0;
                                Cs2Area->infotranstype = -1;
                             }
                             break;
                     case 4:
                             // Get Subcode RW
                             val = (Cs2Area->transscoderw[Cs2Area->transfercount] << 8) |
                                    Cs2Area->transscoderw[Cs2Area->transfercount + 1];

                             Cs2Area->transfercount += 2;
                             Cs2Area->cdwnum += 2;

                             if (Cs2Area->transfercount >= (12 * 2)) // >= : 12 mots, evite OOB transscoderw[24]
                             {
                                Cs2Area->transfercount = 0;
                                Cs2Area->infotranstype = -1;
                             }
                             break;
                     case 5:
                              // Read sector data
                              CDLOG("Read data\n");
                              if (Cs2Area->datatranstype != CDB_DATATRANSTYPE_INVALID)
                              {
                                 // get sector
                                 // Make sure we still have sectors to transfer
                                 if (Cs2Area->datanumsecttrans < Cs2Area->datasectstotrans)
                                 {
									block_struct *blk = Cs2Area->datatranspartition->block[
										Cs2Area->datatranssectpos + Cs2Area->datanumsecttrans];
									if (blk == NULL)
									{
										CDLOG("cs2\t: block was NULL at datanumsecttrans=%d\n",
											  Cs2Area->datanumsecttrans);
										return 0;
									}
									u8 *ptr = &blk->data[Cs2Area->datatransoffset];
									val = T1ReadWord(ptr, 0);
                                    //LOG("[CS2] get addr = %d,val = %08X", Cs2Area->datatransoffset, val);

                                    // increment datatransoffset/cdwnum
                                    Cs2Area->cdwnum += 2;
                                    Cs2Area->datatransoffset += 2;

                                    // Make sure we're not beyond the sector size boundary
                            if (Cs2Area->datatransoffset >= Cs2Area->datatranspartition->block[Cs2Area->datatranssectpos + Cs2Area->datanumsecttrans]->size)
                                    {
                                       Cs2Area->datatransoffset = 0;
                                       Cs2Area->datanumsecttrans++;
                                    }
                                 }
                                 else
                                 {
                                    if (Cs2Area->datatranstype == CDB_DATATRANSTYPE_GETDELSECTOR)
                                    {
                                       // Ok, so we don't have any more sectors to
                                       // transfer, might as well delete them all.

                                       Cs2Area->datatranstype = CDB_DATATRANSTYPE_INVALID;

                                       // free blocks
                                       for (int i = Cs2Area->datatranssectpos; i < (Cs2Area->datatranssectpos+Cs2Area->datasectstotrans); i++)
                                       {
                                          Cs2FreeBlock(Cs2Area->datatranspartition->block[i]);
                                          Cs2Area->datatranspartition->block[i] = NULL;
                                          Cs2Area->datatranspartition->blocknum[i] = 0xFF;
                                       }

                                       // sort remaining blocks
                                       Cs2SortBlocks(Cs2Area->datatranspartition);

                                       Cs2Area->datatranspartition->size -= Cs2Area->cdwnum;
									   // ST-040-R4-051795, §5.4.1 « Buffer Partition Structure » :
										// numblocks indique le nombre de blocs valides ; ne doit jamais dépasser ni être inférieur à 0.
										if (Cs2Area->datasectstotrans <= Cs2Area->datatranspartition->numblocks)
											Cs2Area->datatranspartition->numblocks -= Cs2Area->datasectstotrans;
										else
											Cs2Area->datatranspartition->numblocks = 0;

                                       CDLOG("cs2\t: datatranspartition->size = %x\n", Cs2Area->datatranspartition->size);
                                    }
                                 }
                              }
                              break;
                     default: break;
                  }
                  break;
    default:
             LOG("cs2\t: Undocumented register read %08X\n", addr);
//             val = T3ReadWord(Cs2Area->mem, addr);
             break;
  }

  return val;
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Cs2WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val) {
   Cs2ABusCost(context, CS2_ABUS_WRITE16_CYCLES);

  addr &= 0x3F; // fix me(I should really have proper mapping)

  switch(addr) {
    case 0x08:
    case 0x0A:
      Cs2Area->reg.HIRQ = Cs2Area->reg.HIRQ & val;
				  //if (val != 0xFFFE){
					//  CDLOG("write HIRQ %04X, %04X\n", Cs2Area->reg.HIRQ, val);
				  //}
      if (Cs2Area->reg.HIRQ & Cs2Area->reg.HIRQMASK){
        ScuSendExternalInterrupt00();
      }
                  return;
    case 0x0C:
    case 0x0E: Cs2Area->reg.HIRQMASK = val;
    if (Cs2Area->reg.HIRQ & Cs2Area->reg.HIRQMASK){
      ScuSendExternalInterrupt00();
    }
                  return;
    case 0x18:
    case 0x1A: Cs2Area->status &= ~CDB_STAT_PERI;
                  Cs2Area->_command = 1;
                  Cs2Area->reg.CR1 = val;
                  //CDLOG("Start command %04X\n", Cs2Area->reg.CR1);
                  return;
    case 0x1C:
    case 0x1E: Cs2Area->reg.CR2 = val;
                  return;
    case 0x20:
    case 0x22: Cs2Area->reg.CR3 = val;
                  return;
    case 0x24:
    case 0x26: Cs2Area->reg.CR4 = val;
                  Cs2SetCommandTiming(Cs2Area->reg.CR1 >> 8);
                  return;
    case 0x28:
    case 0x2A: Cs2Area->reg.MPEGRGB = val;
                  return;
    default:
             LOG("cs2\t:Undocumented register write %08X\n", addr);
//                  T3WriteWord(Cs2Area->mem, addr, val);
             break;
  }
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL Cs2ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr) {
  s32 i;
  u32 val = 0;
   Cs2ABusCost(context, CS2_ABUS_READ_CYCLES);
  addr &= 0x3F; // fix me(I should really have proper mapping)

  switch(addr) {
    case 0x08:
                  val = Cs2Area->reg.HIRQ;

                  //if (Cs2Area->isbufferfull)
                  //  val |= CDB_HIRQ_BFUL;
                  //else
                  //  val &= ~CDB_HIRQ_BFUL;

                  //if (Cs2Area->isdiskchanged)
                  //  val |= CDB_HIRQ_DCHG;
                  //else
                  //  val &= ~CDB_HIRQ_DCHG;

                  //if (Cs2Area->isonesectorstored)
                  //  val |= CDB_HIRQ_CSCT;
                  //else
                   // val &= ~CDB_HIRQ_CSCT;

                  Cs2Area->reg.HIRQ = (u16)val;

                  val |= (val << 16);
                  return val;
    case 0x0C: return ((Cs2Area->reg.HIRQMASK << 16) | Cs2Area->reg.HIRQMASK);
    case 0x18: return ((Cs2Area->reg.CR1 << 16) | Cs2Area->reg.CR1);
    case 0x1C: return ((Cs2Area->reg.CR2 << 16) | Cs2Area->reg.CR2);
    case 0x20: return ((Cs2Area->reg.CR3 << 16) | Cs2Area->reg.CR3);
    case 0x24: Cs2Area->_command = 0;
                  return ((Cs2Area->reg.CR4 << 16) | Cs2Area->reg.CR4);
    case 0x28: return ((Cs2Area->reg.MPEGRGB << 16) | Cs2Area->reg.MPEGRGB);
    case 0x00:
                  // transfer data
                  if (Cs2Area->datatranstype != CDB_DATATRANSTYPE_INVALID)
                  {
                    // CDLOG("Get long data\n");
                     // get sector

                     // Make sure we still have sectors to transfer
                     if (Cs2Area->datanumsecttrans < Cs2Area->datasectstotrans)
                     {
						block_struct *blk = Cs2Area->datatranspartition->block[
							Cs2Area->datatranssectpos + Cs2Area->datanumsecttrans];
						if (blk == NULL)
						{
							CDLOG("cs2\t: block was NULL at datanumsecttrans=%d\n",
								  Cs2Area->datanumsecttrans);
							return 0;
						}
						u8 *ptr = &blk->data[Cs2Area->datatransoffset];
						val = T1ReadLong(ptr, 0);
                        // CDLOG("[CS2] get addr = %d,val = %08X\n", Cs2Area->datatransoffset, val);

                        // increment datatransoffset/cdwnum
                        Cs2Area->cdwnum += 4;
                        Cs2Area->datatransoffset += 4;

                        // Make sure we're not beyond the sector size boundary
						if (Cs2Area->datatransoffset >= Cs2Area->datatranspartition->block[Cs2Area->datatranssectpos + Cs2Area->datanumsecttrans]->size)
                        {
                           Cs2Area->datatransoffset = 0;
                           Cs2Area->datanumsecttrans++;
                        }
                     }
                     else
                     {
                        if (Cs2Area->datatranstype == CDB_DATATRANSTYPE_GETDELSECTOR)
                        {
                           // Ok, so we don't have any more sectors to
                           // transfer, might as well delete them all.

                           Cs2Area->datatranstype = CDB_DATATRANSTYPE_INVALID;

                           // free blocks
                           for (i = Cs2Area->datatranssectpos; i < (Cs2Area->datatranssectpos+Cs2Area->datasectstotrans); i++)
                           {
                              Cs2FreeBlock(Cs2Area->datatranspartition->block[i]);
                              Cs2Area->datatranspartition->block[i] = NULL;
                              Cs2Area->datatranspartition->blocknum[i] = 0xFF;
                           }

                           // sort remaining blocks
                           Cs2SortBlocks(Cs2Area->datatranspartition);

                           Cs2Area->datatranspartition->size -= Cs2Area->cdwnum;
                           // garde anti-underflow (idem chemin mot, lignes ~324) :
                           // numblocks ne doit jamais passer sous 0
                           if (Cs2Area->datasectstotrans <= Cs2Area->datatranspartition->numblocks)
                              Cs2Area->datatranspartition->numblocks -= Cs2Area->datasectstotrans;
                           else
                              Cs2Area->datatranspartition->numblocks = 0;

                           CDLOG("cs2\t: datatranspartition->size = %x\n", Cs2Area->datatranspartition->size);
                        }
                     }
                  }
                  break;
    default:
             LOG("cs2\t: Undocumented register read %08X\n", addr);
//             val = T3ReadLong(Cs2Area->mem, addr);
             break;
  }

  return val;
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL Cs2WriteLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u32 val) {
   Cs2ABusCost(context, CS2_ABUS_WRITE32_CYCLES);
   addr &= 0x3F; // fix me(I should really have proper mapping)

   switch (addr)
   {
	case 0x00:
	   if (Cs2Area->datatranstype == CDB_DATATRANSTYPE_PUTSECTOR)
	   {
		  // Put Sector Data: the words fill the staged sectors one after
		  // the other, from Cs2PutBase on, putsectsize bytes each.
		  if (Cs2Area->datanumsecttrans < Cs2Area->datasectstotrans &&
		      Cs2PutBlk[Cs2Area->datanumsecttrans] != NULL)
		  {
			 u8 *ptr = &Cs2PutBlk[Cs2Area->datanumsecttrans]->data[Cs2PutBase + Cs2Area->datatransoffset];
			 T1WriteLong(ptr, 0, val);

			 Cs2Area->cdwnum          += 4;
			 Cs2Area->datatransoffset += 4;

			 if (Cs2Area->datatransoffset >= (u32)Cs2Area->putsectsize)
			 {
				Cs2Area->datatransoffset = 0;
				Cs2Area->datanumsecttrans++;
			 }
		  }
	   }
	   break;
      default:
		   LOG("cs2\t: Undocumented register write %08X\n", addr);
//         T3WriteLong(Cs2Area->mem, addr, val);
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////
int Cs2Init(int coreid, const char *cdpath, const char *mpegpath) {
   int ret;

   if ((Cs2Area = (Cs2 *) malloc(sizeof(Cs2))) == NULL)
      return -1;
   memset(Cs2Area, 0, sizeof(*Cs2Area));

   Cs2Area->nextStatus = 0xFF;
   Cs2Area->mpegpath = mpegpath;

   // Video CD Card boot ROM: raw dump or .zip (see MpegCardLoadRom()).
   MpegCardLoadRom(mpegpath);
   Cs2MpegConnSet = 0;
   Cs2MpegPlaying = 0;
   Cs2MpegIntPending = 0;
   Cs2Area->cdi=NULL;

   if ((ret = Cs2ChangeCDCore(coreid, cdpath)) != 0)
      return ret;

   Cs2Reset();

   /* plus rien a precalculer pour le temps de seek (cf. modele en haut du
      fichier) */

#if 0
   // This stuff need to go elsewhere
   // If Modem is connected, set the registers
   if(Cs2Area->carttype == CART_NETLINK)
   {
      if ((ret = NetlinkInit(modemip, modemport)) != 0)
         return ret;
   }
   else if (Cs2Area->carttype == CART_JAPMODEM)
   {
      if ((ret = JapModemInit(modemip, modemport)) != 0)
         return ret;
   }
#endif

   if ((cdip = (ip_struct *) calloc(sizeof(ip_struct), 1)) == NULL)
      return -1;

   MpegCardInit();

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int Cs2ChangeCDCore(int coreid, const char *cdpath)
{
   int i;

   // Make sure the old core is freed
   if ((Cs2Area != NULL) && (Cs2Area->cdi != NULL))
      Cs2Area->cdi->DeInit();
   //else return -1;

   // So which core do we want?
   if (coreid == CDCORE_DEFAULT)
      coreid = 0; // Assume we want the first one

   // Go through core list and find the id
   for (i = 0; CDCoreList[i] != NULL; i++)
   {
      if (CDCoreList[i]->id == coreid)
      {
         // Set to current core
         Cs2Area->cdi = CDCoreList[i];
         break;
      }
   }

   if (Cs2Area->cdi == NULL)
   {
      Cs2Area->cdi = &DummyCD;
      return -1;
   }

   if (Cs2Area->cdi->Init(cdpath) != 0)
   {
      // This might be helpful.
      YabSetError(YAB_ERR_CANNOTINIT, (void *)Cs2Area->cdi->Name);

      // Since it failed, instead of it being fatal, we'll just use the dummy
      // core instead
      Cs2Area->cdi = &DummyCD;
   }

   Cs2Area->isdiskchanged = 1;
   Cs2MpegDiscInvalidate();
   setStatus(CDB_STAT_PAUSE);
   SmpcRecheckRegion();

   if (Cs2GetRegionID() >= 0xA) YabauseSetVideoFormat(VIDEOFORMATTYPE_PAL);
   else YabauseSetVideoFormat(VIDEOFORMATTYPE_NTSC);

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2DeInit(void) {
   MpegCardDeInit();
   MpegCardFreeRom();

   if(Cs2Area != NULL) {
      if (Cs2Area->cdi != NULL) {
         Cs2Area->cdi->DeInit();
      }

#if 0
      // This stuff need to go elsewhere
      if(Cs2Area->carttype == CART_NETLINK)
         NetlinkDeInit();
      else if (Cs2Area->carttype == CART_JAPMODEM)
         JapModemDeInit();
#endif

      free(Cs2Area);
   }
   Cs2Area = NULL;

   if (cdip)
      free(cdip);
   cdip = NULL;
}

//////////////////////////////////////////////////////////////////////////////

/* End of a play range (data).
 *
 * Kronos switched to PAUSE and raised PEND (and EFLS/EHST for Read File) at
 * the very periodic step that stored the last sector. On the console the
 * drive first reports BUSY and only reaches PAUSE, raising the play-end
 * interrupts, two periodic reports later. Mednafen (ss/cdb.c, drive loop):
 * when the end is met, "CurPosInfo.status = STATUS_BUSY; DrivePhase =
 * DRIVEPHASE_PAUSE; PauseCounter = PlayEndIRQType ? 0 : 1;", then PauseCounter
 * goes 0 -> 1 on the next periodic report and, at the following one,
 * "CurPosInfo.status = STATUS_PAUSE" and TriggerIRQ(PlayEndIRQType).
 *
 * Hop Step Idol: the GFS server step that copies the last sector of a file
 * declares the read finished only if the drive has already ended its play.
 * With PAUSE reported in the same step, the one-sector MET file read
 * finished in the step that copied it, the decompressor (0601ADF4, PR
 * 0602E368) never saw a partial read size, skipped the header parse at
 * 0601AE86, decompressed with garbage sizes and overwrote the vector table.
 * In Mednafen the step returns "busy" with the 1779 bytes already read,
 * the header is parsed, and the next step finishes.
 *
 * The delay is kept in two statics (not in the save state): a state saved
 * inside the two-report window resumes as BUSY -> PAUSE without PEND. */
static u8 Cs2PlayEndReports = 0;
static u8 Cs2PlayEndPending = 0;
/* Seek finished, sectors are being read, but the status still says SEEK
   until a report that follows the first processed sector (Mednafen:
   CurPosInfo.status = PLAY only when PlaySectorProcessed). 1 = reading,
   no sector processed yet; 2 = first sector processed at this report. */
static u8 Cs2SeekReading = 0;
/* Seek time still to elapse after a Play Disc, in _periodictiming units
   (us * 3). The drive keeps sending its periodic report and raising SCDQ
   while it seeks; see the periodic handler in Cs2Exec_unit. */
static u32 Cs2SeekRemaining = 0;
#define CS2_PERIODIC_IDLE 50000u   /* 16.7 ms * 3: "when not playing" (ST-162 4.2, STTECH08) */
static u16 Cs2PlayEndIrqs = 0;

/* Called at the periodic report that stored the LAST sector of the range.
 * That report still says PLAY: in Mednafen the end is only detected at the
 * next periodic report (CheckEndMet() on the following sector), which goes
 * BUSY; PAUSE and the play-end interrupts come two reports after that
 * (PauseCounter 0 -> 1 -> PAUSE). So: PLAY (last sector), BUSY, BUSY, PAUSE.
 *
 * Timing of that next report: in Mednafen the drive tick that hands the
 * last sector to the buffer (CSCT) also prefetches the following one --
 * CurPosInfo.fad is then already the FAD after the range, so Get Status
 * shows PLAY with the end FAD -- and schedules the periodic report 17712
 * clocks of 44100*256 Hz later (~1.57 ms), which detects the end (BUSY).
 * So there is a short "PLAY, end FAD, every sector buffered" window:
 *  - Zero Divide's GFS CD read (GFS_NwCdRead of 162 sectors) completes
 *    only inside it (0603D784: FAD >= end, HIRQ CSCT or PAUSE, drive not
 *    BUSY); the game polls Get Sector Number right after the sector and
 *    stops serving that access once all sectors are counted.
 *  - Hop Step Idol's GFS step copies the last sector (51 52 53 61, CPU
 *    copy, 06 62 51) and only then looks at the drive: it must see BUSY
 *    there, or it finishes its read in the copying step and the
 *    decompressor overwrites the vector table.
 * Hop Step Idol's file is ONE sector: in Mednafen the status only becomes
 * PLAY at a periodic report that follows a processed sector
 * (PlaySectorProcessed); after a seek, the first sector is buffered while
 * the drive still reports SEEK. With a one-sector range the end is met at
 * the very next report, so PLAY never appears: SEEK (sector buffered),
 * BUSY, BUSY, PAUSE -- and its GFS step sees SEEK ("moving") there. Kronos
 * switched to PLAY as soon as the seek ended (see Cs2SeekReading).
 * * Going BUSY in the same report as the last sector broke the GFS "CD read"
 * access (GFS_NwCdRead, a read into the CD block buffer with no transfer):
 * GFS completes it when it sees every requested sector AND a drive state
 * that is not BUSY/SEEK (0603DA1C/0603D784 in Zero Divide's GFS: BUSY maps
 * to "still moving", PLAY or PAUSE to "stopped or reading"). Zero Divide
 * pre-reads 162 sectors with it and stops serving that access as soon as
 * all sectors are counted; with BUSY at that moment GFS never completed it,
 * kept the drive "owned" by that handle ([work+A8]) and every later
 * GFS_Fread waited for the owner and timed out (-22): the data of the
 * fight were never loaded and the game froze in its AI code. */
#define CS2_PLAYEND_REPORT_DELAY 4707   /* 17712 / (44100*256) s = 1.569 ms, in us * 3 */
static void Cs2BeginPlayEnd(u16 irqs)
{
   Cs2PlayEndPending = 1;      /* this report stays PLAY */
   Cs2PlayEndReports = 0;
   Cs2PlayEndIrqs = irqs;
   Cs2Area->_periodictiming = CS2_PLAYEND_REPORT_DELAY;   /* the BUSY report comes ~1.57 ms later */
}

/* next periodic report after the last sector: the end is met */
static void Cs2PlayEndMet(void)
{
   Cs2PlayEndPending = 0;
   setStatus(CDB_STAT_BUSY);
   Cs2Area->nextStatus = CDB_STAT_PAUSE;
   Cs2Area->options = 0x8;
   Cs2PlayEndReports = 2;
}

static INLINE void Cs2CancelPlayEnd(void)
{
   Cs2SeekReading = 0;
   Cs2SeekRemaining = 0;
   Cs2ScanMode = -1;
   Cs2PlayEndPending = 0;
   Cs2PlayEndReports = 0;
   Cs2PlayEndIrqs = 0;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2Reset(void) {
  Cs2CancelPlayEnd();   /* a new drive command replaces a pending play end */
  u32 i, i2;


  resetSyncVideo();
  Cs2MpegDiscInvalidate();
  switch (Cs2Area->cdi->GetStatus())
  {
     case 0:
     case 1:
             setStatus(CDB_STAT_PAUSE);
             Cs2Area->FAD = 150;
             Cs2Area->options = 0;
             Cs2Area->repcnt = 0;
             Cs2Area->ctrladdr = 0x41;
             Cs2Area->track = 1;
             Cs2Area->index = 1;
             break;
     case 2:
             setStatus(CDB_STAT_NODISC);

             Cs2Area->FAD = 0xFFFFFFFF;
             Cs2Area->options = 0xFF;
             Cs2Area->repcnt = 0xFF;
             Cs2Area->ctrladdr = 0xFF;
             Cs2Area->track = 0xFF;
             Cs2Area->index = 0xFF;
             break;
     case 3:
             setStatus(CDB_STAT_OPEN);

             Cs2Area->FAD = 0xFFFFFFFF;
             Cs2Area->options = 0xFF;
             Cs2Area->repcnt = 0xFF;
             Cs2Area->ctrladdr = 0xFF;
             Cs2Area->track = 0xFF;
             Cs2Area->index = 0xFF;
             break;
     default: break;
  }

  Cs2Area->infotranstype = -1;
  Cs2Area->datatranstype = CDB_DATATRANSTYPE_INVALID;
  Cs2Area->transfercount = 0;
  Cs2Area->cdwnum = 0;
  Cs2Area->getsectsize = Cs2Area->putsectsize = 2048;
  Cs2Area->isdiskchanged = 1;
  Cs2Area->isbufferfull = 0;
  Cs2Area->isonesectorstored = 0;
  Cs2Area->isaudio = 0;

  // Video CD Card / MPEG state defaults. Picture size starts at the White
  // Book NTSC frame and is replaced by the decoder's real frame size as soon
  // as a picture comes out (see Cs2MpegUpdateStatus): a Nova trace of a PAL
  // disc shows the card answering 352x288 here, so a fixed 352x240 would be
  // wrong for every PAL Video CD.
  Cs2Area->mpegpicturewidth = 352;
  Cs2Area->mpegpictureheight = 240;
  Cs2Area->isvideocd = 0;

  Cs2Area->reg.CR1 = ( 0 <<8) | 'C';
  Cs2Area->reg.CR2 = ('D'<<8) | 'B';
  Cs2Area->reg.CR3 = ('L'<<8) | 'O';
  Cs2Area->reg.CR4 = ('C'<<8) | 'K';
  Cs2Area->reg.HIRQ = 0xFFFF;
  Cs2Area->reg.HIRQMASK = 0x0000;

  Cs2Area->playFAD = 0xFFFFFFFF;
  Cs2Area->playendFAD = 0xFFFFFFFF;
  Cs2Area->playtype = 0;
  Cs2Area->maxrepeat = 0;

  // set authentication variables to 0(not authenticated)
  Cs2Area->satauth = 0;
  Cs2Area->mpgauth = 0;

  // clear filter conditions
  for (i = 0; i < MAX_SELECTORS; i++)
  {
     Cs2Area->filter[i].FAD = 0;
     Cs2Area->filter[i].range = 0xFFFFFFFF;
     Cs2Area->filter[i].mode = 0;
     Cs2Area->filter[i].chan = 0;
     Cs2Area->filter[i].smmask = 0;
     Cs2Area->filter[i].cimask = 0;
     Cs2Area->filter[i].fid = 0;
     Cs2Area->filter[i].smval = 0;
     Cs2Area->filter[i].cival = 0;
     Cs2Area->filter[i].condtrue = i;
     Cs2Area->filter[i].condfalse = 0xFF;
  }

  // clear partitions
  for (i = 0; i < MAX_SELECTORS; i++)
  {
     Cs2Area->partition[i].size = -1;
     Cs2Area->partition[i].numblocks = 0;

     for (i2 = 0; i2 < MAX_BLOCKS; i2++)
     {
        Cs2Area->partition[i].block[i2] = NULL;
        Cs2Area->partition[i].blocknum[i2] = 0xFF;
     }
  }

  // clear blocks
  for (i = 0; i < MAX_BLOCKS; i++)
  {
     Cs2Area->block[i].size = -1;
     memset(Cs2Area->block[i].data, 0, 2352);
  }
	//ST-040-R4-051795, §5.3 « Reset Selector (command 0x48) », tableau des flags :
	//bit 2 = 1 → reset buffer data, all blocks freed, freespace = MAX
  Cs2Area->blockfreespace = MAX_BLOCKS;

  // initialize TOC
 // memset(Cs2Area->TOC, 0xFF, sizeof(Cs2Area->TOC));

  // clear filesystem stuff
  Cs2Area->curdirsect = 0;
  Cs2Area->curdirsize = 0;
  Cs2Area->curdirfidoffset = 0;
  memset(&Cs2Area->fileinfo, 0, sizeof(Cs2Area->fileinfo));
  Cs2Area->numfiles = 0;

  Cs2Area->lastbuffer = 0xFF;

  Cs2Area->_command = 0;
  Cs2Area->_statuscycles = 0;
  Cs2Area->_statustiming = 1000000;
  Cs2Area->_periodiccycles = 0;
  Cs2Area->_periodictiming = 0;
  Cs2Area->_seekToStop = 0;
  Cs2Area->_commandtiming = 0;
  Cs2Area->nextStatus = 0xFF;
  Cs2SetTiming(0);

  // MPEG specific stuff
  Cs2Area->mpegcon[0].audcon = Cs2Area->mpegcon[0].vidcon = 0x00;
  Cs2Area->mpegcon[0].audlay = Cs2Area->mpegcon[0].vidlay = 0x00;
  Cs2Area->mpegcon[0].audbufnum = Cs2Area->mpegcon[0].vidbufnum = 0xFF;
  Cs2Area->mpegcon[1].audcon = Cs2Area->mpegcon[1].vidcon = 0x00;
  Cs2Area->mpegcon[1].audlay = Cs2Area->mpegcon[1].vidlay = 0x00;
  Cs2Area->mpegcon[1].audbufnum = Cs2Area->mpegcon[1].vidbufnum = 0xFF;

  // should verify the following
  Cs2Area->mpegstm[0].audstm = Cs2Area->mpegstm[0].vidstm = 0x00;
  Cs2Area->mpegstm[0].audstmid = Cs2Area->mpegstm[0].vidstmid = 0x00;
  Cs2Area->mpegstm[0].audchannum = Cs2Area->mpegstm[0].vidchannum = 0x00;
  Cs2Area->mpegstm[1].audstm = Cs2Area->mpegstm[1].vidstm = 0x00;
  Cs2Area->mpegstm[1].audstmid = Cs2Area->mpegstm[1].vidstmid = 0x00;
  Cs2Area->mpegstm[1].audchannum = Cs2Area->mpegstm[1].vidchannum = 0x00;

}


void Cs2ForceOpenTray(){
	if (Cs2Area && Cs2Area->cdi){
		Cs2Area->cdi->SetStatus(CDCORE_OPEN);
		Cs2Reset();
	}
};

int Cs2ForceCloseTray( int coreid, const char * cdpath ){
  int ret = 0;
   if (Cs2Area == NULL) {
     return -1;
   }
   if ((ret = Cs2ChangeCDCore(coreid, cdpath)) != 0) {
     return ret;
   }
  Cs2Reset();

  if (yabsys.emulatebios)
  {
	  if (YabauseQuickLoadGame() != 0)
	  {
		  YabSetError(YAB_ERR_CANNOTINIT, _("Game"));
		  return -2;
	  }
  }
  Cs2Area->cdi->SetStatus(CDCORE_NORMAL);
  Cs2Area->cdi->ReadTOC(Cs2Area->TOC);
  Cs2DetectVideoCD();
  return 0;
};


//////////////////////////////////////////////////////////////////////////////
static int Cs2Exec_CMD_unit(u32 timing) {
  if (Cs2Area->_commandtiming > 0)
  {
    if (Cs2Area->_commandtiming <= timing)
    {
      Cs2Execute();
      Cs2Area->_commandtiming = 0;
      return 1;
    }
    else
      Cs2Area->_commandtiming -= timing;
  }
  return 0;
}

/* 1 if FAD lies in an audio track (TOC control nibble without the data bit,
 * 0x40 of the control/ADR byte). CD-DA sectors go to the audio output, not to
 * the CD buffer (Cs2ReadFilteredSector), so a full buffer must not hold them. */
static int Cs2FADIsAudio(u32 fad) {
  const u8 t = Cs2FADToTrack(fad);
  if (t == 0 || t == 0xFF || t > 99) return 0;
  return ((Cs2Area->TOC[t - 1] >> 24) & 0x40) == 0;
}

//////////////////////////////////////////////////////////////////////////////
// CD Scan (command 12h, fast forward / fast reverse).
//
// ST-162 / ST-038 only name the command (CDC_CdScan, "fast forward play",
// direction 0 forward, 1 reverse). The sector pattern is Mednafen's
// (ss/cdb.c, StartScan and DRIVEPHASE_PLAY): data keeps being read and stored
// as in PLAY, and after every 6 sector units (1 per CD-ROM sector, 2 per
// CD-DA sector) the pickup jumps forward by 102 + FAD * 1773936 / 2^32
// sectors, or back by 104 + FAD * 2180000 / 2^32. The status reads SCAN.
// Kronos only set the SCAN status and read nothing, without even a CD
// report (the command registers were left as the reply).
static void Cs2ScanStep(void)
{
   Cs2ScanCounter += Cs2Area->isaudio ? 2 : 1;
   if (Cs2ScanCounter < 6)
      return;
   Cs2ScanCounter = 0;

   if (Cs2ScanMode == 0)
      Cs2Area->FAD += 102 + (u32)(((u64)1773936 * Cs2Area->FAD + ((u64)1 << 31)) >> 32);
   else
   {
      u32 back = 104 + (u32)(((u64)2180000 * Cs2Area->FAD + ((u64)1 << 31)) >> 32);
      /* Mednafen stops at FAD 0; Kronos cannot read below the play range
         start (lead-in / pregap), so reverse scanning stops there. */
      Cs2Area->FAD = (Cs2Area->FAD > Cs2Area->playFAD + back) ? Cs2Area->FAD - back : Cs2Area->playFAD;
   }
   Cs2Area->cdi->ReadAheadFAD(Cs2Area->FAD);
}

static void Cs2Exec_unit(u32 timing) {
   Cs2Area->_statuscycles += timing * 3;
   Cs2Area->_periodiccycles += timing * 3;

   if (Cs2Area->_statuscycles >= Cs2Area->_statustiming)
   {
      Cs2Area->_statuscycles -= Cs2Area->_statustiming;
      switch(Cs2Area->cdi->GetStatus())
      {
         case 0:
         case 1:
            if ((Cs2Area->status & 0xF) == CDB_STAT_NODISC ||
                (Cs2Area->status & 0xF) == CDB_STAT_OPEN)
            {
               setStatus(CDB_STAT_PAUSE);
               Cs2Area->isdiskchanged = 1;
               Cs2MpegDiscInvalidate();
            }
            break;
         case 2:
            // may need to change this
            if ((Cs2Area->status & 0xF) != CDB_STAT_NODISC)
               setStatus(CDB_STAT_NODISC);
            break;
         case 3:
            // may need to change this
            if ((Cs2Area->status & 0xF) != CDB_STAT_OPEN)
               setStatus(CDB_STAT_OPEN);
            break;
         default: break;
      }
   }

   if (Cs2Area->_periodiccycles >= Cs2Area->_periodictiming)
   {
      u32 elapsed = Cs2Area->_periodictiming;
      int seeking;

      Cs2Area->_periodiccycles -= Cs2Area->_periodictiming;

      Cs2Area->_periodictiming = 0;
      Cs2Area->status |= CDB_STAT_PERI;

      /* Periodic report and SCDQ during a seek.
       *
       * ST-162 (4.2, periodic response) and STTECH08 give the update cycle of
       * the periodic response, "the same as the SCDQ flag update timing":
       * 13.3 ms at standard speed, 6.7 ms at double speed, "16.7 ms or less"
       * otherwise. A seek is not an exception: Mednafen (ss/cdb.c, Drive_Run)
       * runs PeriodicIdleCounter (reload 187065 / (44100*256) s, ~16.6 ms)
       * independently of DrivePhase, so a report and SCDQ come every ~16.6 ms
       * during DRIVEPHASE_SEEK too.
       *
       * Kronos used the whole seek time as one periodic interval, so nothing
       * came for the length of the seek. With the Mednafen seek model
       * (0236a88dd) a short seek lasts ~87 ms. The file library of the Video
       * CD Card player waits for SCDQ with a bounded poll loop (06068BDC in
       * the card program, 24372 reads of HIRQ) inside its load loop: it timed
       * out during the seek to the ISO volume descriptor, abandoned the read
       * without releasing its CD-drive lock (060B94D8), and the movie
       * playback, which waits for that lock, never sent its Play Disc
       * (pressing Play did nothing, then the player's watchdog reset).
       *
       * The seek time is now consumed in periodic steps of at most 16.7 ms;
       * the seek completion itself still happens after the full seek time. */
      seeking = ((Cs2Area->status & 0xF) == CDB_STAT_SEEK && !Cs2SeekReading && Cs2SeekRemaining > 0);
      if (seeking)
      {
         Cs2SeekRemaining = (elapsed >= Cs2SeekRemaining) ? 0 : Cs2SeekRemaining - elapsed;
         seeking = (Cs2SeekRemaining > 0);
      }

      if (seeking)
         Cs2Area->_periodictiming = (Cs2SeekRemaining > CS2_PERIODIC_IDLE) ? CS2_PERIODIC_IDLE : Cs2SeekRemaining;
      else
      // Get Drive's current status and compare with old status
      /* seek done, reading under a SEEK status: run the PLAY logic */
      switch ((((Cs2Area->status & 0xF) == CDB_STAT_SEEK && Cs2SeekReading) ||
               ((Cs2Area->status & 0xF) == CDB_STAT_SCAN && Cs2ScanMode >= 0)) ? CDB_STAT_PLAY : (Cs2Area->status & 0xF)) {
         case CDB_STAT_PAUSE:
         {
             break;
         }
         case CDB_STAT_PLAY:
         {
            partition_struct * playpartition;
            if (Cs2PlayEndPending) {
              /* the previous report stored the last sector (see Cs2BeginPlayEnd) */
              Cs2SeekReading = 0;
              Cs2PlayEndMet();
              Cs2SetTiming(1);
              break;
            }
            if (Cs2SeekReading == 2) {
              /* the previous report processed the first sector: PLAY now */
              Cs2SeekReading = 0;
              setStatus((Cs2ScanMode >= 0) ? CDB_STAT_SCAN : CDB_STAT_PLAY);
            }
            CDLOG("Effective Read %x \n", Cs2Area->FAD);
            int ret = Cs2ReadFilteredSector(Cs2Area->FAD, &playpartition);
            switch (ret)
            {
               case 0:
                  // Sector Read OK
                  Cs2Area->FAD++;
                  if (Cs2ScanMode >= 0)
                     Cs2ScanStep();
                  Cs2Area->track = Cs2FADToTrack(Cs2Area->FAD);
                  Cs2Area->cdi->ReadAheadFAD(Cs2Area->FAD);
                  Cs2SetTiming(1); //As we read one disc sector, we need to wait a while to simulate disc speed
                  if (Cs2SeekReading == 1)
                     Cs2SeekReading = 2;   /* first sector processed, this report still SEEK */

                  if (playpartition != NULL)
                  {
                     // We can use this sector
                     CDLOG("partition number = %d blocks = %d blockfreespace = %d fad = %x playpartition->size = %x isbufferfull = %x IRQMAsk %x\n",
                       (playpartition - Cs2Area->partition),
                       playpartition->numblocks,
                       Cs2Area->blockfreespace, Cs2Area->FAD, playpartition->size, Cs2Area->isbufferfull, Cs2Area->reg.HIRQMASK);

                     Cs2SetIRQ(CDB_HIRQ_CSCT);
                     Cs2Area->isonesectorstored = 1;

                     // Feed the MPEG decoder here, and only with the sectors
                     // the filters actually routed to the partitions named by
                     // MPEG Set Connection. That is what the decoder device
                     // reads on real hardware, and it keeps the streams the
                     // application did not select out of the decoder.
                     if (Cs2MpegConnSet && Cs2IsMpegCardPresent() && (Cs2Area->workblock.sm & 0x20))
                     {
                        int pnum = (int)(playpartition - Cs2Area->partition);

                        if ((pnum == Cs2Area->mpegcon[0].vidbufnum && Cs2Area->mpegcon[0].vidcon != 0) ||
                            (pnum == Cs2Area->mpegcon[0].audbufnum && Cs2Area->mpegcon[0].audcon != 0))
                           MpegCardPushData(Cs2Area->workblock.data + 24, 2324);
                     }


					 if (Cs2Area->isbufferfull) {
						 CDLOG("BUFFER IS FULL\n");
						 Cs2SeekReading = 0;   /* a real SEEK (buffer full), not reading */
						 Cs2SeekRemaining = 0;
						 setStatus(CDB_STAT_SEEK);
						 Cs2Area->nextStatus = 0xFF;
						 Cs2Area->options = 0x00;
					 }

                     if (Cs2Area->FAD >= Cs2Area->playendFAD) {
                        // Make sure we don't have to do a repeat
                        if (Cs2Area->repcnt >= Cs2Area->maxrepeat) {
                           // we're done: BUSY, then PAUSE + PEND two
                           // periodic reports later (see Cs2BeginPlayEnd)
                           Cs2BeginPlayEnd((Cs2Area->playtype == CDB_PLAYTYPE_FILE) ?
                                (CDB_HIRQ_PEND | CDB_HIRQ_EFLS | CDB_HIRQ_EHST) : // EHST: Assault Leynos 2
                                CDB_HIRQ_PEND);

                           CDLOG("PLAY HAS ENDED\n");
                        }
                        else {

                           Cs2Area->FAD = Cs2Area->playFAD;
                           if (Cs2Area->repcnt < 0xE)
                              Cs2Area->repcnt++;
                           Cs2Area->track = Cs2FADToTrack(Cs2Area->FAD);

                           CDLOG("PLAY HAS REPEATED\n");
                        }
                     }

                  }
                  else
                  {
                     CDLOG("Sector filtered out\n");
                     if (Cs2Area->FAD >= Cs2Area->playendFAD) {
                        // Make sure we don't have to do a repeat
                        if (Cs2Area->repcnt >= Cs2Area->maxrepeat) {
                           // we're done (see Cs2BeginPlayEnd)
                           Cs2BeginPlayEnd((Cs2Area->playtype == CDB_PLAYTYPE_FILE) ?
                                (CDB_HIRQ_PEND | CDB_HIRQ_EFLS) : CDB_HIRQ_PEND);

                           CDLOG("PLAY HAS ENDED\n");
                        }
                        else {
                           Cs2Area->FAD = Cs2Area->playFAD;
                           if (Cs2Area->repcnt < 0xE)
                              Cs2Area->repcnt++;
                           Cs2Area->track = Cs2FADToTrack(Cs2Area->FAD);

                           CDLOG("PLAY HAS REPEATED\n");
                        }
                     }
                  }
                  break;
               case -1:
                  // Things weren't setup correctly
                  break;
               case -2:
                  // Do a read retry
                  break;
            }

            break;
         }
		 case CDB_STAT_SEEK:{
			/* A full buffer only stops sectors that would be stored in it.
			   CD-DA is played to the audio output: ST-162 / STTECH08 describe the
			   buffer-full PAUSE for CD reads. Steam-Heart's switches its music
			   from a data stream to CD-DA track 4 while the buffer is still full
			   of stream sectors it no longer reads: the drive stayed in SEEK and
			   the CD-DA never started (no in-game music). */
			if (!Cs2Area->isbufferfull || Cs2FADIsAudio(Cs2Area->FAD)) {
				/* the status stays SEEK until a sector has been processed
				   (see Cs2SeekReading); the next reports read sectors */
				Cs2SeekReading = 1;
				Cs2Area->_periodiccycles = 0;
				Cs2SetTiming(1);          // ← AJOUT : timing lecture, pas seek
				Cs2Area->options = 0x8;
			}
			break;
    		 }
         case CDB_STAT_SCAN:
            break;
         case CDB_STAT_RETRY:
            break;
         case CDB_STAT_BUSY:
            if (Cs2PlayEndReports > 0) {
              /* end of a play range: see Cs2BeginPlayEnd */
              if (--Cs2PlayEndReports > 0) {
                Cs2SetTiming(1);   /* drive still turning at sector rate */
                break;
              }
              setStatus(CDB_STAT_PAUSE);
              Cs2Area->nextStatus = 0xFF;
              Cs2Area->status &= ~CDB_STAT_PERI;
              Cs2SetIRQ(Cs2PlayEndIrqs);
              Cs2PlayEndIrqs = 0;
              break;
            }
            setStatus(Cs2Area->nextStatus);
            Cs2Area->nextStatus = 0xFF;
            Cs2Area->status &= ~CDB_STAT_PERI;
            // doCDReport(Cs2Area->status);
            break;
         default: break;
      }

      // Si l'etat courant n'a pas redefini la cadence (PAUSE/SCAN/RETRY/BUSY/...),
      // _periodictiming reste a 0 et le periodique se re-declencherait a CHAQUE
      // appel de Cs2Exec (flood de doCDReport + SCDQ, ~par scanline au lieu de
      // ~13.3 ms). Cela arrive notamment apres une lecture CD-DA qui se termine
      // en PAUSE. On retombe sur la cadence "non-playing" standard (cf. Cs2SetTiming(0)).
      if (Cs2Area->_periodictiming == 0)
         Cs2Area->_periodictiming = 50000; // 16666.6.. us * 3

      if (Cs2Area->_command) {
        Cs2Area->status &= ~CDB_STAT_PERI;
        return;
      }

      // adjust registers appropriately here(fix me)
      doCDReport(Cs2Area->status);
      Cs2SetIRQ(CDB_HIRQ_SCDQ);
   }

#if 0
   // This stuff need to go elsewhere
   if(Cs2Area->carttype == CART_NETLINK)
      NetlinkExec(timing);
   else if (Cs2Area->carttype == CART_JAPMODEM)
      JapModemExec(timing);
#endif
}

void Cs2Exec(u32 timing) {
  int cycles = 0;
  for (int i = 0; i<timing; i++) {
    cycles++;
    if (Cs2Exec_CMD_unit(1) == 1) {
      Cs2Exec_unit(cycles);
      cycles = 0;
    }
  }
  Cs2Exec_unit(cycles);
}

//////////////////////////////////////////////////////////////////////////////

/* Returns the number of (emulated) microseconds before the next sector
 * will have been completely read in */
int Cs2GetTimeToNextSector(void) {
   if ((Cs2Area->status & 0xF) != CDB_STAT_PLAY && !Cs2SeekReading) {
      return 0;
   } else {
      // Round up, since the caller wants to know when it'll be safe to check
      int time = (Cs2Area->_periodictiming - Cs2Area->_periodiccycles + 2) / 3;
      return time<0 ? 0 : time;
   }
}

//////////////////////////////////////////////////////////////////////////////

/* Cadence de lecture : CD-DA en vitesse standard (1x), donnees CD-ROM en
 * double vitesse (2x).
 *
 * Le bit 4 du drapeau d'Initialize CD System (vitesse standard, memorise dans
 * speed1x) n'est plus applique, comme dans Mednafen (ss/cdb.c, COMMAND_INIT :
 * "CD read speed (unused?)") et Ymir (cdblock.cpp, CmdInitializeCDSystem :
 * selection 1x laissee en commentaire, bit 7 = keepSettings). ST-162 (1.7)
 * donne au bit 7 le sens "1 : aucun changement" ; 3D Mission Shooting envoie
 * 90 puis attend un flux a 2x. Kronos lisait ses videos en 1x, 80 secteurs
 * par seconde au lieu de 150 : videos lentes et hachees.
 * ST-162 1.7 (d) : le CD-DA est toujours lu en vitesse standard. */
void Cs2SetTiming(int playing) {
  if (playing) {
     if (Cs2Area->isaudio) {
       Cs2Area->_periodictiming = 40000;  // 13333.333... * 3
     }
     else {
       Cs2Area->_periodictiming = 20000;  // 6666.666... * 3
     }
  }
  else {
     Cs2Area->_periodictiming = 50000;  // 16666.666... * 3
  }
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetCommandTiming(u8 cmd) {
   switch(cmd) {
      default:
               Cs2Area->_commandtiming = 1;
               break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void Cs2Execute(void) {
  u16 instruction = Cs2Area->reg.CR1 >> 8;

  //Cs2Area->reg.HIRQ &= ~CDB_HIRQ_CMOK;

  switch (instruction) {
    case 0x00:
      //CDLOG("cs2\t: Command: getStatus\n");
      Cs2GetStatus();
      //CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x01:
      CDLOG("cs2\t: Command: getHardwareInfo\n");
      Cs2GetHardwareInfo();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x02:
      CDLOG("cs2\t: Command: getToc\n");
      Cs2GetToc();
      break;
    case 0x03:
      CDLOG("cs2\t: Command: getSessionInfo\n");
      Cs2GetSessionInfo();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x04:
      CDLOG("cs2\t: Command: initializeCDSystem %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2InitializeCDSystem();
      break;
    case 0x05:
       CDLOG("cs2\t: Command: Open Tray %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
       Cs2OpenTray();
       break;
    case 0x06:
      CDLOG("cs2\t: Command: endDataTransfer %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2EndDataTransfer();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x10:
      CDLOG("cs2\t: Command: playDisc %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      CS2_CD_TRACE("Play Disc (0x10)");
      Cs2PlayDisc();
      break;
    case 0x11:
      CDLOG("cs2\t: Command: seekDisc %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2SeekDisc();
      break;
    case 0x12:
      CDLOG("cs2\t: Command: Scan Disc %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2ScanDisc();
      break;
    case 0x20:
      CDLOG("cs2\t: Command: getSubcodeQRW %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetSubcodeQRW();
      break;
    case 0x30:
      CDLOG("cs2\t: Command: setCDDeviceConnection %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      CS2_CD_TRACE("Set CD Device Connection (0x30)");
      Cs2SetCDDeviceConnection();
      break;
	case 0x31:
    CDLOG("cs2\t: Command: Get CD Device Connection\n");
    Cs2GetCDDeviceConnection();   // était: Cs2SetCDDeviceConnection()
    CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n",
          Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2,
          Cs2Area->reg.CR3, Cs2Area->reg.CR4);
    break;
    case 0x32:
      CDLOG("cs2\t: Command: getLastBufferDestination %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetLastBufferDestination();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x40:
      CDLOG("cs2\t: Command: setFilterRange %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      CS2_CD_TRACE("Set Filter Range (0x40)");
      Cs2SetFilterRange();
      break;
    case 0x41:
      CDLOG("cs2\t: Command: Get Filter Range\n");
      Cs2GetFilterRange();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x42:
      CDLOG("cs2\t: Command: setFilterSubheaderConditions %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      CS2_CD_TRACE("Set Filter Subheader Conditions (0x42)");
      Cs2SetFilterSubheaderConditions();
      break;
    case 0x43:
      CDLOG("cs2\t: Command: getFilterSubheaderConditions\n");
      Cs2GetFilterSubheaderConditions();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x44:
      CDLOG("cs2\t: Command: setFilterMode %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      CS2_CD_TRACE("Set Filter Mode (0x44)");
      Cs2SetFilterMode();
      break;
    case 0x45:
      CDLOG("cs2\t: Command: getFilterMode\n");
      Cs2GetFilterMode();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x46:
      CDLOG("cs2\t: Command: setFilterConnection %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      CS2_CD_TRACE("Set Filter Connection (0x46)");
      Cs2SetFilterConnection();
      break;
    case 0x47:
       CDLOG("cs2\t: Command: getFilterConnection %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
       Cs2GetFilterConnection();
       CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
       break;
    case 0x48:
      CDLOG("cs2\t: Command: resetSelector %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      CS2_CD_TRACE("Reset Selector (0x48)");
      Cs2ResetSelector();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x50:
      CDLOG("cs2\t: Command: getBufferSize\n");
      Cs2GetBufferSize();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x51:
      CDLOG("cs2\t: Command: getSectorNumber %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetSectorNumber();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x52:
      CDLOG("cs2\t: Command: calculateActualSize %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2CalculateActualSize();
      break;
    case 0x53:
      CDLOG("cs2\t: Command: getActualSize\n");
      Cs2GetActualSize();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x54:
      CDLOG("cs2\t: Command: getSectorInfo %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetSectorInfo();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x55:
      CDLOG("cs2\t: Command: Exec FAD Search %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2ExecFadSearch();
      break;
    case 0x56:
      CDLOG("cs2\t: Command: Get FAD Search Results %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetFadSearchResults();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x60:
      CDLOG("cs2\t: Command: setSectorLength %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      CS2_CD_TRACE("Set Sector Length (0x60)");
      Cs2SetSectorLength();
      break;
    case 0x61:
      CDLOG("cs2\t: Command: getSectorData %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetSectorData();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x62:
      CDLOG("cs2\t: Command: deleteSectorData %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2DeleteSectorData();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x63:
      CDLOG("cs2\t: Command: getThenDeleteSectorData %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetThenDeleteSectorData();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x64:
      CDLOG("cs2\t: Command: putSectorData %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2PutSectorData();
      break;
    case 0x65:
      CDLOG("cs2\t: Command: copySectorData %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2CopySectorData();
      break;
    case 0x66:
      CDLOG("cs2\t: Command: moveSectorData %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MoveSectorData();
      break;
    case 0x67:
      CDLOG("cs2\t: Command: getCopyError\n");
      Cs2GetCopyError();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x70:
      CDLOG("cs2\t: Command: changeDirectory %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2ChangeDirectory();
      break;
    case 0x71:
      CDLOG("cs2\t: Command: readDirectory %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2ReadDirectory();
      break;
    case 0x72:
      CDLOG("cs2\t: Command: getFileSystemScope\n");
      Cs2GetFileSystemScope();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x73:
      CDLOG("cs2\t: Command: getFileInfo %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetFileInfo();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x74:
      CDLOG("cs2\t: Command: readFile %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2ReadFile();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x75:
      CDLOG("cs2\t: Command: abortFile\n");
      CS2_CD_TRACE("Abort File (0x75)");
      Cs2AbortFile();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x90:
      CDLOG("cs2\t: Command: mpegGetStatus\n");
      Cs2MpegGetStatus();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x91:
      CDLOG("cs2\t: Command: mpegGetInterrupt\n");
      Cs2MpegGetInterrupt();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x92:
      CDLOG("cs2\t: Command: mpegSetInterruptMask %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2);
      Cs2MpegSetInterruptMask();
      break;
    case 0x93:
      CDLOG("cs2\t: Command: mpegInit %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2);
      Cs2MpegInit();
      break;
    case 0x94:
      CDLOG("cs2\t: Command: mpegSetMode %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3);
      Cs2MpegSetMode();
      break;
    case 0x95:
      CDLOG("cs2\t: Command: mpegPlay %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR4);
      Cs2MpegPlay();
      break;
    case 0x96:
      CDLOG("cs2\t: Command: mpegSetDecodingMethod %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR4);
      Cs2MpegSetDecodingMethod();
      break;
    case 0x97:
      CDLOG("cs2\t: Command: mpegOutDecodingSync %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR4);
      Cs2MpegOutDecodingSync();
      break;
    case 0x98:
      CDLOG("cs2\t: Command: mpegGetTimecode\n");
      Cs2MpegGetTimecode();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x99:
      CDLOG("cs2\t: Command: mpegGetPts\n");
      Cs2MpegGetPts();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x9A:
      CDLOG("cs2\t: Command: mpegSetConnection %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegSetConnection();
      break;
    case 0x9B:
      CDLOG("cs2\t: Command: mpegGetConnection\n");
      Cs2MpegGetConnection();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x9C:
      CDLOG("cs2\t: Command: mpegChangeConnection %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegChangeConnection();
      break;
    case 0x9D:
      CDLOG("cs2\t: Command: mpegSetStream %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegSetStream();
      break;
    case 0x9E:
      CDLOG("cs2\t: Command: mpegGetStream\n");
      Cs2MpegGetStream();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0x9F:
      CDLOG("cs2\t: Command: mpegGetPictureSize\n");
      Cs2MpegGetPictureSize();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0xA0:
      CDLOG("cs2\t: Command: mpegDisplay %04x %04x %04x \n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2);
      Cs2MpegDisplay();
      break;
    case 0xA1:
      CDLOG("cs2\t: Command: mpegSetWindow %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegSetWindow();
      break;
    case 0xA2:
      CDLOG("cs2\t: Command: mpegSetBorderColor %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2);
      Cs2MpegSetBorderColor();
      break;
    case 0xA3:
      CDLOG("cs2\t: Command: mpegSetFade %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2);
      Cs2MpegSetFade();
      break;
    case 0xA4:
      CDLOG("cs2\t: Command: mpegSetVideoEffects %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegSetVideoEffects();
      break;
    case 0xA5:
      CDLOG("cs2\t: Command: mpegGetImage %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegGetImage();
      break;
    case 0xA6:
      CDLOG("cs2\t: Command: mpegSetImage %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegSetImage();
      break;
    case 0xA7:
      CDLOG("cs2\t: Command: mpegReadImage %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegReadImage();
      break;
    case 0xA8:
      CDLOG("cs2\t: Command: mpegWriteImage %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegWriteImage();
      break;
    case 0xA9:
      CDLOG("cs2\t: Command: mpegReadSector %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegReadSector();
      break;
    case 0xAA:
      CDLOG("cs2\t: Command: mpegWriteSector %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegWriteSector();
      break;
    case 0xAE:
      CDLOG("cs2\t: Command: mpegGetLSI %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegGetLSI();
      break;
    case 0xAF:
      CDLOG("cs2\t: Command: mpegSetLSI %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2MpegSetLSI();
      break;
    case 0xE0:
      CDLOG("cs2\t: Command: authenticateDevice %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2AuthenticateDevice();
      break;
    case 0xE1:
      CDLOG("cs2\t: Command: isDeviceAuthenticated %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2IsDeviceAuthenticated();
      CDLOG("cs2\t: ret: %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      break;
    case 0xE2:
      CDLOG("cs2\t: Command: getMPEGRom %04x %04x %04x %04x %04x\n", Cs2Area->reg.HIRQ, Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4);
      Cs2GetMPEGRom();
      break;
    default:
      CDLOG("cs2\t: Command %02x not implemented\n", instruction);
      break;
  }
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetStatus(void) {
  doCDReport(Cs2Area->status);
  Cs2Area->reg.HIRQ |= CDB_HIRQ_CMOK;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetHardwareInfo(void) {
  if ((Cs2Area->status & 0xF) != CDB_STAT_OPEN && (Cs2Area->status & 0xF) != CDB_STAT_NODISC)
     Cs2Area->isdiskchanged = 0;

  Cs2Area->reg.CR1 = Cs2Area->status << 8;
  // hardware flags/CD Version. Bit 9 (0x0200) of CR2 is the "MPEG card
  // present" flag per the Yabause wiki (CDBlock page, "Get Hardware
  // Info"); it was hardcoded to always report absent (0x0001), which
  // means BiosCheckMPEGCard() and any game probing this command first
  // (before ever reaching Authenticate Device / 0xE0-0xE2) would never
  // see the card even with one configured.
  Cs2Area->reg.CR2 = Cs2IsMpegCardPresent() ? 0x0201 : 0x0001;
  // mpeg version, it actually is required(at least by the bios)

  if (Cs2Area->mpgauth)
     Cs2Area->reg.CR3 = 0x1;
  else
     Cs2Area->reg.CR3 = 0;

  // drive info/revision
  Cs2Area->reg.CR4 = 0x0400;
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetToc(void) {
    Cs2Area->cdi->ReadTOC(Cs2Area->TOC);
	// ST-040-R4-051795, §6.4 « Get TOC (command 0x02) » :
	// À l'issue de la commande, le flag Disc Changed doit être effacé si la lecture TOC réussit.
    if (Cs2Area->isdiskchanged)
    {
       Cs2DetectVideoCD(); // new disc: (re)check for a Video CD layout
       MpegCardReset();    // drop whatever the old disc was decoding
    }
    Cs2Area->isdiskchanged = 0;

    Cs2Area->transfercount = 0;
    Cs2Area->infotranstype = 0;

    /* Get TOC only prepares a data transfer: it does not touch the drive.
       Mednafen (ss/cdb.c, COMMAND_GET_TOC) answers with the current status
       plus DTREQ (TRNS, 40h) and leaves the drive phase alone. Kronos forced
       BUSY -> PAUSE here, which stopped any seek or play in progress: Mass
       Destruction reads the TOC every frame while it starts its in-game
       music (Play track 7 index 0, repeat 15, Play command 10h with
       CR1-CR4 = 1000 0700 0F00 0700), so the seek to FAD 021A2A was cut
       to PAUSE each frame and the CD-DA never played. */
    Cs2Area->reg.CR1 = (Cs2Area->status | CDB_STAT_TRNS) << 8;
    Cs2Area->reg.CR2 = 0xCC;
    Cs2Area->reg.CR3 = 0x0;
    Cs2Area->reg.CR4 = 0x0;
    Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_DRDY);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetSessionInfo(void) {

  switch (Cs2Area->reg.CR1 & 0xFF) {
    case 0:
            Cs2Area->reg.CR3 = (u16)(0x0100 | ((Cs2Area->TOC[101] & 0xFF0000) >> 16));
            Cs2Area->reg.CR4 = (u16)Cs2Area->TOC[101];
            break;
    case 1:
            Cs2Area->reg.CR3 = 0x0100; // return Session number(high byte)/and first byte of Session lba
            Cs2Area->reg.CR4 = 0; // lower word of Session lba
            break;
    default:
            Cs2Area->reg.CR3 = 0xFFFF;
            Cs2Area->reg.CR4 = 0xFFFF;
            break;
  }
  /* Get Session Info does not touch the drive either (Mednafen,
     COMMAND_GET_SESSINFO: current status, no phase change); forcing PAUSE
     here stopped a play in progress the same way as Get TOC did. */
  Cs2Area->reg.CR1 = Cs2Area->status << 8;
  Cs2Area->reg.CR2 = 0;

  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2InitializeCDSystem(void) {
  Cs2CancelPlayEnd();   /* a new drive command replaces a pending play end */
  u16 val = 0;
  u8 initflag = Cs2Area->reg.CR1 & 0xFF;

  Cs2Area->nextStatus = 0xFF;

  if ((Cs2Area->status & 0xF) != CDB_STAT_OPEN && (Cs2Area->status & 0xF) != CDB_STAT_NODISC)
  {
     setStatus(CDB_STAT_PAUSE);
     Cs2Area->FAD = 150;
  }

  if (initflag & 0x1)
  {
    int i, i2;
    Cs2Area->playFAD = 0xFFFFFFFF;
    Cs2Area->playendFAD = 0xFFFFFFFF;
    Cs2Area->playtype = 0;
    Cs2Area->maxrepeat = 0;

    // set authentication variables to 0(not authenticated)
    Cs2Area->satauth = 0;
    Cs2Area->mpgauth = 0;

    // clear filter conditions
    for (i = 0; i < MAX_SELECTORS; i++)
    {
      Cs2Area->filter[i].FAD = 0;
      Cs2Area->filter[i].range = 0xFFFFFFFF;
      Cs2Area->filter[i].mode = 0;
      Cs2Area->filter[i].chan = 0;
      Cs2Area->filter[i].smmask = 0;
      Cs2Area->filter[i].cimask = 0;
      Cs2Area->filter[i].fid = 0;
      Cs2Area->filter[i].smval = 0;
      Cs2Area->filter[i].cival = 0;
      Cs2Area->filter[i].condtrue = i;
      Cs2Area->filter[i].condfalse = 0xFF;
    }

    // clear partitions
    for (i = 0; i < MAX_SELECTORS; i++)
    {
      Cs2Area->partition[i].size = -1;
      Cs2Area->partition[i].numblocks = 0;

      for (i2 = 0; i2 < MAX_BLOCKS; i2++)
      {
        Cs2Area->partition[i].block[i2] = NULL;
        Cs2Area->partition[i].blocknum[i2] = 0xFF;
      }
    }

    // clear blocks
    for (i = 0; i < MAX_BLOCKS; i++)
    {
      Cs2Area->block[i].size = -1;
      memset(Cs2Area->block[i].data, 0, 2352);
    }

    Cs2Area->blockfreespace = MAX_BLOCKS;

    // initialize TOC
   // memset(Cs2Area->TOC, 0xFF, sizeof(Cs2Area->TOC));

    // clear filesystem stuff
    Cs2Area->curdirsect = 0;
    Cs2Area->curdirsize = 0;
    Cs2Area->curdirfidoffset = 0;
    memset(&Cs2Area->fileinfo, 0, sizeof(Cs2Area->fileinfo));
    Cs2Area->numfiles = 0;

    Cs2Area->lastbuffer = 0xFF;

  }

  if (initflag & 0x2)
  {
     // Decode RW subcode
  }

  if (initflag & 0x4)
  {
     // Don't confirm Mode 2 subheader
  }

  if (initflag & 0x8)
  {
     // Retry reading Form 2 sectors
  }

  if (initflag & 0x10)
     Cs2Area->speed1x = 1;
  else
     Cs2Area->speed1x = 0;

  val = Cs2Area->reg.HIRQ & 0xFFE5;
  Cs2Area->isbufferfull = 0;

  if (Cs2Area->isdiskchanged)
     val |= CDB_HIRQ_DCHG;
  else
     val &= ~CDB_HIRQ_DCHG;

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(val | CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2OpenTray(void)
{
   u16 val = 0;

   setStatus(CDB_STAT_OPEN);
   doCDReport(Cs2Area->status);
   Cs2SetIRQ(val | CDB_HIRQ_CMOK | CDB_HIRQ_DCHG);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2EndDataTransfer(void) {
  s32 i;
  if (Cs2Area->cdwnum)
  {
     Cs2Area->reg.CR1 = (u16)((Cs2Area->status << 8) | ((Cs2Area->cdwnum >> 17) & 0xFF));
     Cs2Area->reg.CR2 = (u16)(Cs2Area->cdwnum >> 1);
     Cs2Area->reg.CR3 = 0;
     Cs2Area->reg.CR4 = 0;
  }
  else
  {
     Cs2Area->reg.CR1 = (Cs2Area->status << 8) | 0xFF; // FIXME
     Cs2Area->reg.CR2 = 0xFFFF;
     Cs2Area->reg.CR3 = 0;
     Cs2Area->reg.CR4 = 0;
  }

  // stop any transfers that may be going(this is still probably wrong), and
  // set/clear the appropriate flags

  switch (Cs2Area->datatranstype)
  {
     case 0:
        // Get Sector Data
        break;
     case 2:
     {
        // Get Then Delete Sector

        // Make sure we actually have to free something
        if (Cs2Area->datatranspartition->size <= 0) break;

        Cs2Area->datatranstype = CDB_DATATRANSTYPE_INVALID;

        // free blocks
        for (i = Cs2Area->datatranssectpos; i < (Cs2Area->datatranssectpos + Cs2Area->datasectstotrans); i++)
        {
           Cs2FreeBlock(Cs2Area->datatranspartition->block[i]);
           Cs2Area->datatranspartition->block[i] = NULL;
           Cs2Area->datatranspartition->blocknum[i] = 0xFF;
        }

        // sort remaining blocks
        Cs2SortBlocks(Cs2Area->datatranspartition);

        Cs2Area->datatranspartition->size -= Cs2Area->cdwnum;
        // Meme garde anti-underflow que dans Cs2ReadWord/Cs2ReadLong (cf. lignes
        // ~324/~488) : numblocks ne doit jamais passer sous 0.
        if (Cs2Area->datasectstotrans <= Cs2Area->datatranspartition->numblocks)
           Cs2Area->datatranspartition->numblocks -= Cs2Area->datasectstotrans;
        else
           Cs2Area->datatranspartition->numblocks = 0;

        if (Cs2Area->blockfreespace == MAX_BLOCKS) Cs2Area->isonesectorstored = 0;

        break;
     }
     case CDB_DATATRANSTYPE_PUTSECTOR:
     {
        // Put Sector Data finished: the sectors enter the CD buffer through
        // the filter named by the command, into its true-output partition
        // (Mednafen FilterBuf; the filter's conditions are not applied
        // here). Without a connected partition they are dropped.
        u8 dst = Cs2Area->filter[Cs2PutFilter].condtrue;
        u32 n;

        Cs2Area->datatranstype = CDB_DATATRANSTYPE_INVALID;

        for (n = 0; n < Cs2PutCount; n++)
        {
           partition_struct *part;

           if (Cs2PutBlk[n] == NULL)
              continue;

           if (dst >= MAX_SELECTORS || Cs2Area->partition[dst].numblocks >= MAX_BLOCKS)
           {
              Cs2FreeBlock(Cs2PutBlk[n]);
              Cs2PutBlk[n] = NULL;
              continue;
           }

           part = &Cs2Area->partition[dst];
           part->block[part->numblocks] = Cs2PutBlk[n];
           part->blocknum[part->numblocks] = Cs2PutBlkNum[n];
           part->numblocks++;
           part->size += Cs2PutBlk[n]->size;
           Cs2Area->isonesectorstored = 1;

           Cs2PutBlk[n] = NULL;
        }
        Cs2PutCount = 0;
        break;
     }
     default: break;
  }

  Cs2Area->cdwnum = 0;

  Cs2SetIRQ(CDB_HIRQ_EHST | CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2PlayDisc(void) {
  Cs2CancelPlayEnd();   /* a new drive command replaces a pending play end */
  u32 pdspos;
  u32 pdepos;
  u32 pdpmode;

  // Get all the arguments
  pdspos = ((Cs2Area->reg.CR1 & 0xFF) << 16) | Cs2Area->reg.CR2;
  pdepos = ((Cs2Area->reg.CR3 & 0xFF) << 16) | Cs2Area->reg.CR4;
  pdpmode = Cs2Area->reg.CR3 >> 8;
  /* Play mode bit 7: do not move the pickup to the start position
   * (CDC_PM_PIC_NOMOV, ST-162 "CD Play Parameters"). 0xFF means no change. */
  const int pick_nomove = (pdpmode != 0xFF) && (pdpmode & 0x80);
  const int start_given = (pdspos != 0xFFFFFF) && (pdpmode != 0xFF);

  CDLOG("[CDB] Command: Play; Start = 0x%06x, End = 0x%06x, Mode = 0x%02x\n", pdspos, pdepos, pdpmode);
  u32 current_fad = Cs2Area->FAD;
  CDLOG("Current FAD is %x\n", current_fad);

  // Convert Start Position to playFAD
  if (pdspos == 0xFFFFFF || pdpmode == 0xFF) // This still isn't right
  {
     // No Change
	 CDLOG("[CDB] pos = current\n");
  }
  else if (pdspos & 0x800000)
  {
     // FAD Mode
     Cs2Area->playFAD = (pdspos & 0xFFFFF);

	 CDLOG("[CDB] pos = FAD:%02X\n", Cs2Area->playFAD);

     Cs2SetupDefaultPlayStats(Cs2FADToTrack(Cs2Area->playFAD), 0);

     if (!(pdpmode & 0x80))
        // Move pickup to start position
        Cs2Area->FAD = Cs2Area->playFAD;
  }
  else
  {
     // Track Mode

     // If track == 0, set it to the first available track, or something like that
     if (pdspos == 0)
        pdspos = 0x0100;

     if (!(pdpmode & 0x80))
     {
        Cs2SetupDefaultPlayStats((u8)(pdspos >> 8), 1);
        Cs2Area->playFAD = Cs2Area->FAD;
        Cs2Area->track = (u8)(pdspos >> 8);
        Cs2Area->index = (u8)pdspos;

		CDLOG("[CDB] pos = TRACK:%02X FAD:%02X upd\n", (u8)(pdspos >> 8), Cs2Area->FAD );
     }
     else
     {
        // Preserve Pickup Position
        Cs2SetupDefaultPlayStats((u8)(pdspos >> 8), 0);
        /* The play range still starts at the new track: only the pickup
         * stays where it is. playFAD was left at the previous range, so a
         * repeat or a move to the start went back to the old position
         * (Steam-Heart's: PlayDisc track 4, mode 8Fh, kept reading the data
         * stream at 89E1h instead of the CD-DA). */
        {
          const u8 trk = (u8)(pdspos >> 8);
          if (trk != 0 && trk != 0xFF && trk <= 99)
            Cs2Area->playFAD = Cs2Area->TOC[trk - 1] & 0x00FFFFFF;
        }

		CDLOG("[CDB] pos = TRACK:%02X FAD:%02X noupd\n", (u8)(pdspos >> 8), Cs2Area->FAD );
     }
  }

  pdpmode &= 0x7F;

  // Only update max repeat if bits 0-6 aren't all set
  if (pdpmode != 0x7F)
     Cs2Area->maxrepeat = pdpmode;

  // Convert End Position to playendFAD
  if (pdepos == 0xFFFFFF)
  {
     // No Change
  }
  else if (pdepos & 0x800000)
  {
     // FAD Mode -ST-040-R4-051795, §6.2 « Play Disc (command 0x20) », champ EP : EP [23:0] 
	 // End FAD or Track/Index. Bit 23 = 1 → EP[22:0] = relative FAD offset
     Cs2Area->playendFAD = Cs2Area->playFAD+(pdepos & 0x7FFFFF); // était: (pdepos & 0xFFFFF) — masquait à tort les bits 20-22
  }
  else if (pdepos != 0)
  {
	 Cs2Area->playendFAD = Cs2TrackToFAD((u16)(pdepos | 0x0063));
  }
  else
  {
     // Default Mode
     Cs2Area->playendFAD = Cs2TrackToFAD(0xFFFF);
  }

  /* Pickup not moved and current position outside the new play range:
   * STTECH08 table 4.5 "Operation outside the play range", row "CD play
   * (play range modification, pause release)": without repeat, <PAUSE> at
   * the current position; with repeat, repeat operation (seek to the start
   * position, then <PLAY>). ST-162 CDC_PM_PIC_NOMOV: "<PAUSE> status when
   * current position is outside play range". */
  if (pick_nomove && start_given &&
      (Cs2Area->FAD < Cs2Area->playFAD || Cs2Area->FAD > Cs2Area->playendFAD))
  {
     if (Cs2Area->maxrepeat != 0)
     {
        Cs2Area->FAD = Cs2Area->playFAD;
        Cs2Area->track = Cs2FADToTrack(Cs2Area->FAD);
     }
     else
     {
        Cs2Area->_periodiccycles = 0;
        Cs2Area->_periodictiming = 0;
        setStatus(CDB_STAT_PAUSE);
        Cs2Area->nextStatus = 0xFF;
        Cs2Area->options = 0;
        Cs2Area->playtype = CDB_PLAYTYPE_SECTOR;
        doCDReport(Cs2Area->status);
        Cs2SetIRQ(CDB_HIRQ_CMOK);
        return;
     }
  }

  // setup play mode here
#ifdef CDDEBUG
  if (pdpmode != 0)
     CDLOG("cs2\t: playDisc: Unsupported play mode = %02X\n", pdpmode);
#endif

  // Cs2SetTiming(1);

  Cs2Area->_periodiccycles = 0;
  Cs2Area->_periodictiming = 0;
  Cs2SeekRemaining = 0;
  if (Cs2Area->_seekToStop == 1) {
    // The seek command as previously generated a stop.
    // Simulate a wait time - need for batman forever texture loading
    Cs2SetTiming(0); //Need a big delay to restart
    Cs2Area->_seekToStop = 0;
  } else {
    // Calcul du temps de seek.
    //
    // Le deplacement reel du bloc optique va de la position PHYSIQUE de la
    // tete a la nouvelle position de lecture. Apres un secteur lu, FAD
    // designe deja le secteur *suivant* : la tete est donc sur FAD-1, d'ou
    // le decalage (repris de 8328b5e). playendFAD n'intervient pas : la fin
    // de la zone a lire ne dit rien de la distance parcourue par la tete.
    u32 head_fad = current_fad;
    if (head_fad != 0 && head_fad != 0xFFFFFFFF)
      head_fad--;

    /* The seek lasts Cs2SeekRemaining; the periodic report keeps its own
       16.7 ms cycle meanwhile (see the periodic handler in Cs2Exec_unit). */
    Cs2SeekRemaining = Cs2ComputeSeekTiming(head_fad, Cs2Area->FAD);
    Cs2Area->_periodictiming = (Cs2SeekRemaining > CS2_PERIODIC_IDLE) ? CS2_PERIODIC_IDLE : Cs2SeekRemaining;

    CDLOG("cs2\t: seek %x -> %x : %d us\n",
          head_fad, Cs2Area->FAD, Cs2SeekRemaining / 3);
  }
  setStatus(CDB_STAT_SEEK);      // need to be seek
  Cs2Area->nextStatus = 0xFF;
  Cs2Area->options = 0;
  Cs2Area->playtype = CDB_PLAYTYPE_SECTOR;
  Cs2Area->cdi->ReadAheadFAD(Cs2Area->FAD);

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SeekDisc(void) {
  Cs2CancelPlayEnd();   /* a new drive command replaces a pending play end */

	// Stop
	if ((Cs2Area->reg.CR1 & 0xFF) == 0x00 && Cs2Area->reg.CR2 == 0x0000){

		setStatus(CDB_STAT_STANDBY);
		Cs2Area->options = 0xFF;
		Cs2Area->repcnt = 0xFF;
		Cs2Area->ctrladdr = 0xFF;
		Cs2Area->track = 0xFF;
		Cs2Area->index = 0xFF;
		Cs2Area->FAD = 0xFFFFFFFF;

		CDLOG("[CDB] Seek pos = CDB_STAT_STANDBY" );
	}
	// Pause
	else if ((Cs2Area->reg.CR1 & 0xFF) == 0xFF && Cs2Area->reg.CR2 == 0xFFFF){
    Cs2Area->_seekToStop = 1; //The seek command is generating a stop.
		setBusyStatus(CDB_STAT_PAUSE);
	}
  else if (Cs2Area->reg.CR1 & 0x80)
  {
     // Seek by FAD
     u32 sdFAD;
    int i;

     sdFAD = ((Cs2Area->reg.CR1 & 0xFF) << 16) | Cs2Area->reg.CR2;
	//ST-040-R4-051795, §6.3 « Seek Disc (command 0x11) » :
	// SP[23:0]. Bit 23 = 1 → SP[22:0] = FAD
     sdFAD = (sdFAD & 0x7FFFFF);   // était: 0xFFFFF (20 bits)
    setStatus(CDB_STAT_PAUSE);
    for (i = 0; i < 99; i++){
       u32 tfad = Cs2Area->TOC[i] & 0x00FFFFFF;
       if (tfad >= sdFAD){
          Cs2SetupDefaultPlayStats(i, 1);
          Cs2Area->FAD = sdFAD;
          break;
       }
    }
	CDLOG("[CDB] Seek pos = FAD:%02X", Cs2Area->FAD );
  }
  else
  {
     // Were we given a valid track number?
     if (Cs2Area->reg.CR2 >> 8)
     {
        // Seek by index
        setStatus(CDB_STAT_PAUSE);
        Cs2SetupDefaultPlayStats((Cs2Area->reg.CR2 >> 8), 1);
        Cs2Area->index = Cs2Area->reg.CR2 & 0xFF;

		CDLOG("[CDB] Seek pos = TRACK:%02X FAD:%02X", Cs2Area->track, Cs2Area->FAD );
	 }
     else
     {
        // Error
        setStatus(CDB_STAT_STANDBY);
        Cs2Area->options = 0xFF;
        Cs2Area->repcnt = 0xFF;
        Cs2Area->ctrladdr = 0xFF;
        Cs2Area->track = 0xFF;
        Cs2Area->index = 0xFF;
        Cs2Area->FAD = 0xFFFFFFFF;

		CDLOG("[CDB] Seek pos = CDB_STAT_STANDBY" );
     }
  }

  // Cs2SetTiming(0);

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2ScanDisc(void) {
   u8 dir = Cs2Area->reg.CR1 & 0xFF;

   /* Mednafen COMMAND_SCAN: a direction other than 0/1 is rejected. */
   if (dir >= 2)
   {
      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK);
      return;
   }

   Cs2CancelPlayEnd();   /* a new drive command replaces a pending play end */
   Cs2ScanMode = dir;
   Cs2ScanCounter = 0;

   if ((Cs2Area->status & 0xF) != CDB_STAT_PLAY)
   {
      /* not reading yet: start reading where the pickup is, like a seek
         that has already arrived (Mednafen: DRIVEPHASE_SEEK_START2) */
      Cs2SeekReading = 1;
      Cs2Area->_periodiccycles = 0;
      Cs2SetTiming(1);
      Cs2Area->options = 0x8;
   }
   setStatus(CDB_STAT_SCAN);
   Cs2Area->nextStatus = 0xFF;

   doCDReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetSubcodeQRW(void) {
   u32 rel_fad;
   u8 rel_m, rel_s, rel_f, m, s, f;

  // According to Tyranid's doc, the subcode type is stored in the low byte
  // of CR2. However, Sega's CDC library writes the type to the low byte
  // of CR1. Somehow I'd sooner believe Sega is right.
  switch(Cs2Area->reg.CR1 & 0xFF) {
     case 0:
             // Get Q Channel
             Cs2Area->reg.CR1 = (Cs2Area->status << 8) | 0;
             Cs2Area->reg.CR2 = 5;
             Cs2Area->reg.CR3 = 0;
             Cs2Area->reg.CR4 = 0;

             // Cs2Area->track is a u8 and can legitimately hold the reset/
             // "no track" sentinel 0xFF, or 0 when Cs2FADToTrack() can't
             // place the current FAD in any track (e.g. lead-in). Either
             // makes 'track-1' wrap to 254 or -1, indexing well past
             // TOC[102] (tracks only occupy TOC[0..98]). Guard it instead
             // of trusting track to always be a valid 1-99 track number.
             if (Cs2Area->track >= 1 && Cs2Area->track <= 99)
                rel_fad = Cs2Area->FAD-(Cs2Area->TOC[Cs2Area->track-1] & 0xFFFFFF);
             else
                rel_fad = Cs2Area->FAD;
             Cs2FADToMSF(rel_fad, &rel_m, &rel_s, &rel_f);
             Cs2FADToMSF(Cs2Area->FAD, &m, &s, &f);

             Cs2Area->transscodeq[0] = Cs2Area->ctrladdr; // ctl/adr
             Cs2Area->transscodeq[1] = ToBCD(Cs2Area->track); // track number
             Cs2Area->transscodeq[2] = ToBCD(Cs2Area->index); // index
             Cs2Area->transscodeq[3] = ToBCD(rel_m); // relative M
             Cs2Area->transscodeq[4] = ToBCD(rel_s); // relative S
             Cs2Area->transscodeq[5] = ToBCD(rel_f); // relative F
             Cs2Area->transscodeq[6] = 0;
             Cs2Area->transscodeq[7] = ToBCD(m); // M
             Cs2Area->transscodeq[8] = ToBCD(s); // S
             Cs2Area->transscodeq[9] = ToBCD(f); // F

             Cs2Area->transfercount = 0;
             Cs2Area->infotranstype = 3;
             break;
     case 1:
     {
             // Get RW Channel
             static int lastfad=0;
             static u16 group=0;
             int i;

             Cs2Area->reg.CR1 = (Cs2Area->status << 8) | 0;
             Cs2Area->reg.CR2 = 12;
             Cs2Area->reg.CR3 = 0;
             if (Cs2Area->FAD != lastfad)
             {
                lastfad = Cs2Area->FAD;
                group = 0;
             }
             else
                group++;
             if (group > 3) group = 3; // R-W = 4 packs de 24 octets max -> evite lecture OOB de workblock.data
             Cs2Area->reg.CR4 = group; // Subcode flag

             for (i = 0; i < 24; i++)
                Cs2Area->transscoderw[i] = Cs2Area->workblock.data[2352+i+(24*group)] & 0x3F;

             Cs2Area->transfercount = 0;
             Cs2Area->infotranstype = 4;
             break;
     }
     default: break;
  }

  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_DRDY);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetCDDeviceConnection(void) {
  u32 scdcfilternum;

  scdcfilternum = (Cs2Area->reg.CR3 >> 8);

  if (scdcfilternum == 0xFF)
     Cs2Area->outconcddev = NULL;
  else if (scdcfilternum < MAX_SELECTORS)
     Cs2Area->outconcddev = Cs2Area->filter + scdcfilternum;

  Cs2Area->outconcddevnum = (u8)scdcfilternum;

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetCDDeviceConnection(void)
{
   Cs2Area->reg.CR1 = (Cs2Area->status << 8);
   Cs2Area->reg.CR2 = 0;
   Cs2Area->reg.CR3 = Cs2Area->outconcddevnum << 8;
   Cs2Area->reg.CR4 = 0;
   Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetLastBufferDestination(void) {
  Cs2Area->reg.CR1 = (Cs2Area->status << 8);
  Cs2Area->reg.CR2 = 0;
  Cs2Area->reg.CR3 = Cs2Area->lastbuffer << 8;
  Cs2Area->reg.CR4 = 0;
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetFilterRange(void) {
  u8 sfrfilternum;

  sfrfilternum = Cs2Area->reg.CR3 >> 8;

  // Guard against an out-of-range selector number: filter[] only has
  // MAX_SELECTORS (24) valid entries, but sfrfilternum comes straight
  // from an 8-bit command register field (0-255).
  if (sfrfilternum < MAX_SELECTORS)
  {
     Cs2Area->filter[sfrfilternum].FAD = ((Cs2Area->reg.CR1 & 0xFF) << 16) | Cs2Area->reg.CR2;
     Cs2Area->filter[sfrfilternum].range = ((Cs2Area->reg.CR3 & 0xFF) << 16) | Cs2Area->reg.CR4;
  }

  // return default cd stats
  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetFilterRange(void) {
   u8 sfrfilternum;

   sfrfilternum = Cs2Area->reg.CR3 >> 8;

   if (sfrfilternum < MAX_SELECTORS)
   {
      Cs2Area->reg.CR1 = (Cs2Area->status << 8) | ((Cs2Area->filter[sfrfilternum].FAD & 0xFF0000) >> 16);
      Cs2Area->reg.CR2 = Cs2Area->filter[sfrfilternum].FAD & 0xFFFF;
      Cs2Area->reg.CR3 = ((Cs2Area->filter[sfrfilternum].range & 0xFF0000) >> 16);
      Cs2Area->reg.CR4 = Cs2Area->filter[sfrfilternum].range & 0xFFFF;
   }
   else
   {
      Cs2Area->reg.CR1 = (Cs2Area->status << 8);
      Cs2Area->reg.CR2 = 0;
      Cs2Area->reg.CR3 = 0;
      Cs2Area->reg.CR4 = 0;
   }
   Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetFilterSubheaderConditions(void) {
  u8 sfscfilternum;

  sfscfilternum = Cs2Area->reg.CR3 >> 8;

  if (sfscfilternum < MAX_SELECTORS)
  {
     Cs2Area->filter[sfscfilternum].chan = Cs2Area->reg.CR1 & 0xFF;
     Cs2Area->filter[sfscfilternum].smmask = Cs2Area->reg.CR2 >> 8;
     Cs2Area->filter[sfscfilternum].cimask = Cs2Area->reg.CR2 & 0xFF;
     Cs2Area->filter[sfscfilternum].fid = Cs2Area->reg.CR3 & 0xFF;;
     Cs2Area->filter[sfscfilternum].smval = Cs2Area->reg.CR4 >> 8;
     Cs2Area->filter[sfscfilternum].cival = Cs2Area->reg.CR4 & 0xFF;
  }

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetFilterSubheaderConditions(void) {
  u8 gfscfilternum;

  gfscfilternum = Cs2Area->reg.CR3 >> 8;

  if (gfscfilternum < MAX_SELECTORS)
  {
     Cs2Area->reg.CR1 = (Cs2Area->status << 8) | Cs2Area->filter[gfscfilternum].chan;
     Cs2Area->reg.CR2 = (Cs2Area->filter[gfscfilternum].smmask << 8) | Cs2Area->filter[gfscfilternum].cimask;
     Cs2Area->reg.CR3 = Cs2Area->filter[gfscfilternum].fid;
     Cs2Area->reg.CR4 = (Cs2Area->filter[gfscfilternum].smval << 8) | Cs2Area->filter[gfscfilternum].cival;
  }
  else
  {
     Cs2Area->reg.CR1 = (Cs2Area->status << 8);
     Cs2Area->reg.CR2 = 0;
     Cs2Area->reg.CR3 = 0;
     Cs2Area->reg.CR4 = 0;
  }
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetFilterMode(void) {
  u8 sfmfilternum;

  sfmfilternum = Cs2Area->reg.CR3 >> 8;

  if (sfmfilternum < MAX_SELECTORS)
  {
     Cs2Area->filter[sfmfilternum].mode = Cs2Area->reg.CR1 & 0xFF;

     if (Cs2Area->filter[sfmfilternum].mode & 0x80)
     {
        // Initialize filter conditions
        Cs2Area->filter[sfmfilternum].mode = 0;
        Cs2Area->filter[sfmfilternum].FAD = 0;
	//ST-040-R4-051795, §5.5.3 « Set Filter Mode (command 0x44) » :
	//When bit 7 = 1 : initialize filter. Default range = 0xFFFFFFFF (no restriction)
	//§5.5.2 « Reset Selector (command 0x48) » confirme la même valeur par défaut.
        Cs2Area->filter[sfmfilternum].range = 0xFFFFFFFF;  // était: 0
        Cs2Area->filter[sfmfilternum].chan = 0;
        Cs2Area->filter[sfmfilternum].smmask = 0;
        Cs2Area->filter[sfmfilternum].cimask = 0;
        Cs2Area->filter[sfmfilternum].smval = 0;
        Cs2Area->filter[sfmfilternum].cival = 0;
     }
  }

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetFilterMode(void) {
  u8 gfmfilternum;

  gfmfilternum = Cs2Area->reg.CR3 >> 8;

  Cs2Area->reg.CR1 = (Cs2Area->status << 8) | (gfmfilternum < MAX_SELECTORS ? Cs2Area->filter[gfmfilternum].mode : 0);
  Cs2Area->reg.CR2 = 0;
  Cs2Area->reg.CR3 = 0;
  Cs2Area->reg.CR4 = 0;
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetFilterConnection(void) {
  u8 sfcfilternum;

  sfcfilternum = Cs2Area->reg.CR3 >> 8;

  if (sfcfilternum < MAX_SELECTORS)
  {
     if (Cs2Area->reg.CR1 & 0x1)
     {
        // Set connection for true condition
        Cs2Area->filter[sfcfilternum].condtrue = Cs2Area->reg.CR2 >> 8;
     }

     if (Cs2Area->reg.CR1 & 0x2)
     {
        // Set connection for false condition
        Cs2Area->filter[sfcfilternum].condfalse = Cs2Area->reg.CR2 & 0xFF;
     }
  }

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetFilterConnection(void) {
   u8 sfcfilternum;

   sfcfilternum = Cs2Area->reg.CR3 >> 8;

   Cs2Area->reg.CR1 = (Cs2Area->status << 8);
   if (sfcfilternum < MAX_SELECTORS)
      Cs2Area->reg.CR2 = (Cs2Area->filter[sfcfilternum].condtrue << 8) | Cs2Area->filter[sfcfilternum].condfalse;
   else
      Cs2Area->reg.CR2 = 0;
   Cs2Area->reg.CR3 = 0;
   Cs2Area->reg.CR4 = 0;

   Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2ResetSelector(void) {
  // still needs a bit of work
  u32 i, i2;

  if ((Cs2Area->reg.CR1 & 0xFF) == 0)
  {
     // Reset specified partition buffer only
     u32 rsbufno = Cs2Area->reg.CR3 >> 8;

     // sort remaining blocks
     if (rsbufno < MAX_SELECTORS)
     {
        // clear partition
        for (i = 0; i < Cs2Area->partition[rsbufno].numblocks; i++)
        {
           Cs2FreeBlock(Cs2Area->partition[rsbufno].block[i]);
           Cs2Area->partition[rsbufno].block[i] = NULL;
           Cs2Area->partition[rsbufno].blocknum[i] = 0xFF;
        }

        Cs2Area->partition[rsbufno].size = -1;
        Cs2Area->partition[rsbufno].numblocks = 0;
     }

     if (Cs2Area->blockfreespace > 0) Cs2Area->isbufferfull = 0;
     if (Cs2Area->blockfreespace == MAX_BLOCKS)
     {
        Cs2Area->isonesectorstored = 0;
        Cs2Area->datatranstype = CDB_DATATRANSTYPE_INVALID;
     }
     else if (Cs2Area->datatranspartitionnum == rsbufno)
        Cs2Area->datatranstype = CDB_DATATRANSTYPE_INVALID;

     doCDReport(Cs2Area->status);
     Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
     return;
  }

  // parse flags and reset the specified area(fix me)
  if (Cs2Area->reg.CR1 & 0x80)
  {
     // reset false filter output connections
     for (i = 0; i < MAX_SELECTORS; i++)
        Cs2Area->filter[i].condfalse = 0xFF;
  }

  if (Cs2Area->reg.CR1 & 0x40)
  {
     // reset true filter output connections
     for (i = 0; i < MAX_SELECTORS; i++)
        Cs2Area->filter[i].condtrue = (u8)i;
  }

  if (Cs2Area->reg.CR1 & 0x10)
  {
     // reset filter conditions
     for (i = 0; i < MAX_SELECTORS; i++)
     {
        Cs2Area->filter[i].FAD = 0;
        Cs2Area->filter[i].range = 0xFFFFFFFF;
        Cs2Area->filter[i].mode = 0;
        Cs2Area->filter[i].chan = 0;
        Cs2Area->filter[i].smmask = 0;
        Cs2Area->filter[i].cimask = 0;
        Cs2Area->filter[i].fid = 0;
        Cs2Area->filter[i].smval = 0;
        Cs2Area->filter[i].cival = 0;
     }
  }

  if (Cs2Area->reg.CR1 & 0x8)
  {
     // reset partition output connectors
  }

  if (Cs2Area->reg.CR1 & 0x4)
  {
     // reset partitions buffer data
     Cs2Area->isbufferfull = 0;

     // clear partitions
     for (i = 0; i < MAX_SELECTORS; i++)
     {
        Cs2Area->partition[i].size = -1;
        Cs2Area->partition[i].numblocks = 0;

        for (i2 = 0; i2 < MAX_BLOCKS; i2++)
        {
           Cs2Area->partition[i].block[i2] = NULL;
           Cs2Area->partition[i].blocknum[i2] = 0xFF;
        }
     }

     // clear blocks
     for (i = 0; i < MAX_BLOCKS; i++)
     {
        Cs2Area->block[i].size = -1;
        memset(Cs2Area->block[i].data, 0, 2352);
     }

     Cs2Area->blockfreespace = MAX_BLOCKS;  // était: 200
     Cs2Area->isbufferfull = 0;
     Cs2Area->isonesectorstored = 0;
     Cs2Area->datatranstype = CDB_DATATRANSTYPE_INVALID;
  }

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetBufferSize(void) {
  Cs2Area->reg.CR1 = Cs2Area->status << 8;
  Cs2Area->reg.CR2 = (u16)Cs2Area->blockfreespace;
  Cs2Area->reg.CR3 = MAX_SELECTORS << 8;
  Cs2Area->reg.CR4 = MAX_BLOCKS;
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetSectorNumber(void) {
  u32 gsnbufno;

  gsnbufno = Cs2Area->reg.CR3 >> 8;

  // partition[] only has MAX_SELECTORS entries; every sibling command
  // (Cs2GetSectorInfo, Cs2GetBufferSize, Cs2GetSectorData, etc.) bound-
  // checks this same partition-number field before indexing -- this one
  // didn't.
  if (gsnbufno >= MAX_SELECTORS || Cs2Area->partition[gsnbufno].size == -1)
     Cs2Area->reg.CR4 = 0;
  else
     Cs2Area->reg.CR4 = Cs2Area->partition[gsnbufno].numblocks;

  Cs2Area->reg.CR1 = Cs2Area->status << 8;
  Cs2Area->reg.CR2 = 0;
  Cs2Area->reg.CR3 = 0;
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////
#define CDC_ACTSIZ_ERR  0xffffff

static INLINE void CalcSectorOffsetNumber(u32 bufno, u32 *sectoffset, u32 *sectnum);

void Cs2CalculateActualSize(void) {
  u32 i;
  u32 casbufno;
  u32 cassectoffset;
  u32 casnumsect;

#if 0
  if (Cs2Area->status == CDB_STAT_SEEK){
	  Cs2Area->calcsize = CDC_ACTSIZ_ERR;
	  doCDReport(Cs2Area->status);
	  Cs2Area->reg.HIRQ |= CDB_HIRQ_CMOK;
	  return;
  }
#endif

  cassectoffset = Cs2Area->reg.CR2;
  casbufno = Cs2Area->reg.CR3 >> 8;
  casnumsect = Cs2Area->reg.CR4;

  if (casbufno >= MAX_SELECTORS)
  {
     Cs2Area->calcsize = 0;
     doCDReport(CDB_STAT_REJECT);
     Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
     return;
  }

  /* CDC_CalActSiz takes CDC_SPOS_END and CDC_SNUM_END (both FFFFH) for
   * "the partition's last sector" and "from spos to the end of the
   * partition" (ST-162 sec. 6.3). Without resolving them, idx ran from
   * 65535 and no block was ever counted, so calcsize stayed 0 -- and
   * CDC_GetActSiz returns exactly that, its initial value being 0
   * (ST-162 sec. 6.4). A game taking that 0 as a word count then arms a
   * zero-length SH2 DMA, which the SH7604 reads as the maximum count of
   * 16,777,216 (manual sec. 9.2.3). Same failure as the zero-sector
   * request already handled in Cs2GetSectorData, reached another way. */
  CalcSectorOffsetNumber(casbufno, &cassectoffset, &casnumsect);

  if (Cs2Area->partition[casbufno].size != 0)
  {
     Cs2Area->calcsize = 0;

     // FIXED BUG 12: cassectoffset était constant dans la boucle
     // → on additionnait casnumsect fois la taille du même bloc
     // FIXED: utiliser idx = cassectoffset + i pour parcourir les bons blocs
     // Ref: ST-040-R4-051795 §6.11 "Calculate Actual Size (command 0x52)"
     // CR2 = sector offset, CR4 = number of sectors, retour en mots (taille / 2)
     for (i = 0; i < casnumsect; i++)
     {
        u32 idx = cassectoffset + i;
        if (idx >= (u32)Cs2Area->partition[casbufno].numblocks || idx >= MAX_BLOCKS)
           break;
        if (Cs2Area->partition[casbufno].block[idx])
           Cs2Area->calcsize += (Cs2Area->partition[casbufno].block[idx]->size / 2);
     }
  }
  else
     Cs2Area->calcsize = 0;

  CDLOG("Cs2Area->calcsize = %d", Cs2Area->calcsize);
  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetActualSize(void) {
  Cs2Area->reg.CR1 = (u16)((Cs2Area->status << 8) | ((Cs2Area->calcsize >> 16) & 0xFF));
  Cs2Area->reg.CR2 = (u16)Cs2Area->calcsize;
  Cs2Area->reg.CR3 = 0;
  Cs2Area->reg.CR4 = 0;
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetSectorInfo(void) {
  u32 gsisctnum;
  u32 gsibufno;
	// ST-040-R4-051795, §6.12 « Get Sector Info (command 0x54) » :
	// CR2[15:0] = Sector Number in partition
  gsisctnum = Cs2Area->reg.CR2;   // word entier, était: & 0xFF
  gsibufno = Cs2Area->reg.CR3 >> 8;
  if (gsibufno < MAX_SELECTORS) {
     if (gsisctnum < Cs2Area->partition[gsibufno].numblocks) {
        Cs2Area->reg.CR1 = (u16)((Cs2Area->status << 8) | ((Cs2Area->partition[gsibufno].block[gsisctnum]->FAD >> 16) & 0xFF));
        Cs2Area->reg.CR2 = (u16)Cs2Area->partition[gsibufno].block[gsisctnum]->FAD;
        Cs2Area->reg.CR3 = (Cs2Area->partition[gsibufno].block[gsisctnum]->fn << 8) | Cs2Area->partition[gsibufno].block[gsisctnum]->cn;
        Cs2Area->reg.CR4 = (Cs2Area->partition[gsibufno].block[gsisctnum]->sm << 8) | Cs2Area->partition[gsibufno].block[gsisctnum]->ci;
        Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
        return;
     }
     else
     {
        CDLOG("cs2\t: getSectorInfo: Unsupported Partition Number\n");
     }
  }

  Cs2Area->reg.CR1 = (CDB_STAT_REJECT << 8) | (Cs2Area->reg.CR1 & 0xFF);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2ExecFadSearch(void) {
   // finish me
   doCDReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetFadSearchResults(void) {
   // finish me
   Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetSectorLength(void) {
  switch (Cs2Area->reg.CR1 & 0xFF) {
    case 0:
            Cs2Area->getsectsize = 2048;
            break;
    case 1:
            Cs2Area->getsectsize = 2336;
            break;
    case 2:
            Cs2Area->getsectsize = 2340;
            break;
    case 3:
            Cs2Area->getsectsize = 2352;
            break;
    default: break;
  }

  switch (Cs2Area->reg.CR2 >> 8) {
    case 0:
            Cs2Area->putsectsize = 2048;
            break;
    case 1:
            Cs2Area->putsectsize = 2336;
            break;
    case 2:
            Cs2Area->putsectsize = 2340;
            break;
    case 3:
            Cs2Area->putsectsize = 2352;
            break;
    default: break;
  }

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ESEL);
}

//////////////////////////////////////////////////////////////////////////////

static INLINE void CalcSectorOffsetNumber(u32 bufno, u32 *sectoffset, u32 *sectnum)
{
   if (*sectoffset == 0xFFFF)
   {
      // Last sector
      *sectoffset = Cs2Area->partition[bufno].numblocks - 1;
   }
   // BUG corrige : c'etait un "else if". Si sectoffset ET sectnum valent 0xFFFF
   // (ex: "dernier secteur, jusqu'a la fin"), le else laissait sectnum=0xFFFF ->
   // datasectstotrans=65535 -> sur-lecture massive (GetSectorData) ou boucle de
   // liberation hors-borne -> NULL deref / crash (DeleteSectorData). En "if"
   // independant : sectoffset=numblocks-1 puis sectnum=numblocks-(numblocks-1)=1,
   // soit exactement le dernier secteur. Les autres cas sont inchanges.
   if (*sectnum == 0xFFFF)
   {
      // From sectoffset to last sector in partition
      *sectnum = Cs2Area->partition[bufno].numblocks - *sectoffset;
   }
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetSectorData(void)
{
   u32 gsdsectoffset;
   u32 gsdbufno;
   u32 gsdsectnum;

   gsdsectoffset = Cs2Area->reg.CR2;
   gsdbufno = Cs2Area->reg.CR3 >> 8;
   gsdsectnum = Cs2Area->reg.CR4;

   //LOG("[CS2] COMMAND_GET_SECDATA, pnum=%d, offs = %d, numsec = %d", gsdbufno, gsdsectoffset, gsdsectnum);

   if (gsdbufno >= MAX_SELECTORS)
   {
      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
      return;
   }

   /* A request for zero sectors cannot be satisfied and must be rejected
    * like any other unsatisfiable one. Independence Day polls with
    * getSectorNumber and feeds the result straight back as the sector
    * count, so it asks for zero on every iteration while it waits. We only
    * rejected that while the partition was still empty; the moment a sector
    * landed, a zero-sector request was accepted and DRDY was raised, so the
    * game took its zero count to be valid and armed a zero-length SH2 DMA -
    * which the SH7604 reads as the maximum count of 16,777,216 (manual sec.
    * 9.2.3). The channel then walked 64 MB from the CD data register through
    * VDP2 VRAM, CRAM, the VDP2 and SCU registers and all of work RAM high,
    * leaving the display disabled and both CPUs executing zeroes. */
   if (Cs2Area->reg.CR4 == 0 || Cs2Area->partition[gsdbufno].numblocks == 0)
   {
      CDLOG("No sectors available\n");

      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
      return;
   }

   CalcSectorOffsetNumber(gsdbufno, &gsdsectoffset, &gsdsectnum);
   /* Le bloc CD ne sert une plage de secteurs que si elle est entierement
    * presente dans la partition ; sinon la commande est REJETEE et l hote
    * la reemet plus tard (code de retour CDC_ERR_REJECT, manuel de
    * l interface de communication CD ST-38 / ST-162).
    *
    * Kronos acceptait des que la partition n etait pas vide et transferait
    * simplement moins de secteurs que demande. Un jeu qui fait confiance au
    * compte demande - Sol Divide demande 40 secteurs par tour - avance alors
    * son pointeur de 40 secteurs apres n en avoir recu qu un ou deux : le
    * reste de sa zone de chargement garde son ancien contenu, et il finit par
    * sauter dedans (issue #1222 : le CPU execute des donnees a 0x06010000 et
    * leve un opcode invalide). */
   if ((gsdsectoffset + gsdsectnum) > (u32)Cs2Area->partition[gsdbufno].numblocks)
   {
      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
      return;
   }


   // Setup Data Transfer
   Cs2Area->cdwnum = 0;
   Cs2Area->datatranstype = CDB_DATATRANSTYPE_GETSECTOR;
   Cs2Area->datatranspartition = Cs2Area->partition + gsdbufno;
   Cs2Area->datatranspartitionnum = (u8)gsdbufno;
   Cs2Area->datatransoffset = 0;
   Cs2Area->datanumsecttrans = 0;
   Cs2Area->datatranssectpos = (u16)gsdsectoffset;
   Cs2Area->datasectstotrans = (u16)gsdsectnum;

   doCDReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_DRDY | CDB_HIRQ_EHST);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2DeleteSectorData(void)
{
   u32 dsdsectoffset;
   u32 dsdbufno;
   u32 dsdsectnum;
   u32 i;

   dsdsectoffset = Cs2Area->reg.CR2;
   dsdbufno = Cs2Area->reg.CR3 >> 8;
   dsdsectnum = Cs2Area->reg.CR4;

   if (dsdbufno >= MAX_SELECTORS)
   {
      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
      return;
   }

   if (Cs2Area->partition[dsdbufno].numblocks == 0)
   {
      CDLOG("No sectors available\n");

      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
      return;
   }

   CalcSectorOffsetNumber(dsdbufno, &dsdsectoffset, &dsdsectnum);

   for (i = dsdsectoffset; i < (dsdsectoffset+dsdsectnum); i++)
   {
      Cs2Area->partition[dsdbufno].size -= Cs2Area->partition[dsdbufno].block[i]->size;
      Cs2FreeBlock(Cs2Area->partition[dsdbufno].block[i]);
      Cs2Area->partition[dsdbufno].block[i] = NULL;
      Cs2Area->partition[dsdbufno].blocknum[i] = 0xFF;
   }

   // sort remaining blocks
   Cs2SortBlocks(&Cs2Area->partition[dsdbufno]);

   Cs2Area->partition[dsdbufno].numblocks -= (u8)dsdsectnum;

   if (Cs2Area->blockfreespace == MAX_BLOCKS)
      Cs2Area->isonesectorstored = 0;

   doCDReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetThenDeleteSectorData(void)
{
   u32 gtdsdsectoffset;
   u32 gtdsdbufno;
   u32 gtdsdsectnum;

   gtdsdsectoffset = Cs2Area->reg.CR2;
   gtdsdbufno = Cs2Area->reg.CR3 >> 8;
   gtdsdsectnum = Cs2Area->reg.CR4;

   if (gtdsdbufno >= MAX_SELECTORS)
   {
      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
      return;
   }

   /* A request for zero sectors cannot be satisfied and must be rejected
    * like any other unsatisfiable one. Independence Day polls with
    * getSectorNumber and feeds the result straight back as the sector
    * count, so it asks for zero on every iteration while it waits. We only
    * rejected that while the partition was still empty; the moment a sector
    * landed, a zero-sector request was accepted and DRDY was raised, so the
    * game took its zero count to be valid and armed a zero-length SH2 DMA -
    * which the SH7604 reads as the maximum count of 16,777,216 (manual sec.
    * 9.2.3). The channel then walked 64 MB from the CD data register through
    * VDP2 VRAM, CRAM, the VDP2 and SCU registers and all of work RAM high,
    * leaving the display disabled and both CPUs executing zeroes. */
   if (Cs2Area->reg.CR4 == 0 || Cs2Area->partition[gtdsdbufno].numblocks == 0)
   {
      CDLOG("No sectors available\n");

      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
      return;
   }

   CalcSectorOffsetNumber(gtdsdbufno, &gtdsdsectoffset, &gtdsdsectnum);
   /* Le bloc CD ne sert une plage de secteurs que si elle est entierement
    * presente dans la partition ; sinon la commande est REJETEE et l hote
    * la reemet plus tard (code de retour CDC_ERR_REJECT, manuel de
    * l interface de communication CD ST-38 / ST-162).
    *
    * Kronos acceptait des que la partition n etait pas vide et transferait
    * simplement moins de secteurs que demande. Un jeu qui fait confiance au
    * compte demande - Sol Divide demande 40 secteurs par tour - avance alors
    * son pointeur de 40 secteurs apres n en avoir recu qu un ou deux : le
    * reste de sa zone de chargement garde son ancien contenu, et il finit par
    * sauter dedans (issue #1222 : le CPU execute des donnees a 0x06010000 et
    * leve un opcode invalide). */
   if ((gtdsdsectoffset + gtdsdsectnum) > (u32)Cs2Area->partition[gtdsdbufno].numblocks)
   {
      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EHST);
      return;
   }


   // Setup Data Transfer
   Cs2Area->cdwnum = 0;
   Cs2Area->datatranstype = CDB_DATATRANSTYPE_GETDELSECTOR;
   Cs2Area->datatranspartition = Cs2Area->partition + gtdsdbufno;
   Cs2Area->datatranspartitionnum = (u8)gtdsdbufno; // manquait : ResetSelector teste cet index
   Cs2Area->datatransoffset = 0;
   Cs2Area->datanumsecttrans = 0;
   Cs2Area->datatranssectpos = (u16)gtdsdsectoffset;
   Cs2Area->datasectstotrans = (u16)gtdsdsectnum;

   Cs2Area->infotranstype = 5;

   doCDReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_DRDY | CDB_HIRQ_EHST);

   return;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2PutSectorData(void) {
   // Put Sector Data (64h, CDC_PutSctData, ST-162 7.5): "Writes sector data
   // to the designated filter". CR3 high byte is a FILTER number (ST-162;
   // the older ST-038 called it a buffer partition), CR4 the sector count.
   // The host then writes the data through the data register and ends with
   // End Data Transfer (06h); the sectors go through the filter into its
   // true-output partition at that point (Mednafen ss/cdb.c,
   // COMMAND_PUT_SECDATA and COMMAND_END_DATAXFER).
   //
   // Kronos wrote no response at all (the registers came back as sent, so
   // the status byte read 64h), took the number as a partition, and stored
   // the blocks at once. The Video CD Card player uses this command for
   // fast forward / reverse (one sector put into filter 2, then MPEG Set
   // Connection on partition 2): with the garbage status it gave the
   // operation up and kept repositioning on the same entry.
   u32 fnum = Cs2Area->reg.CR3 >> 8;
   u32 numsec = Cs2Area->reg.CR4;
   u32 i;

   if (fnum >= MAX_SELECTORS)
   {
      doCDReport(CDB_STAT_REJECT);
      Cs2SetIRQ(CDB_HIRQ_CMOK);
      return;
   }

   // Mednafen: no sector, not enough free buffers, or a transfer already
   // running -> WAIT.
   if (numsec == 0 || numsec > MAX_BLOCKS || (s32)numsec > Cs2Area->blockfreespace ||
       Cs2Area->datatranstype == CDB_DATATRANSTYPE_PUTSECTOR)
   {
      doCDReport(CDB_STAT_WAIT);
      Cs2SetIRQ(CDB_HIRQ_CMOK);
      return;
   }

   Cs2PutCount = 0;
   for (i = 0; i < numsec; i++)
   {
      block_struct *blk = Cs2AllocateBlock(&Cs2PutBlkNum[i], Cs2Area->putsectsize);
      if (blk == NULL)
         break;
      memset(blk->data, 0, sizeof(blk->data));
      blk->FAD = 0;
      blk->fn = 0;
      blk->cn = 0;
      // Subheader bytes are "not fixed" (ST-162 7.5). Marked Form 2 so the
      // MPEG card side treats a put sector as stream data, like the decoder
      // that reads it on hardware.
      blk->sm = 0x20;
      blk->ci = 0;
      Cs2PutBlk[i] = blk;
      Cs2PutCount++;
   }

   // Where the written words land in the 2352-byte sector, per put sector
   // length (Mednafen DTW_OffsTab, in bytes): 2048 -> user data of a Mode 2
   // Form 1 sector (24), 2336 -> 16, 2340 -> 12, 2352 -> 0.
   switch (Cs2Area->putsectsize)
   {
      case 2336: Cs2PutBase = 16; break;
      case 2340: Cs2PutBase = 12; break;
      case 2352: Cs2PutBase = 0;  break;
      default:   Cs2PutBase = 24; break;
   }
   Cs2PutFilter = (u8)fnum;

   Cs2Area->cdwnum = 0;
   Cs2Area->datatranstype = CDB_DATATRANSTYPE_PUTSECTOR;
   Cs2Area->datatransoffset = 0;
   Cs2Area->datanumsecttrans = 0;
   Cs2Area->datatranssectpos = 0;
   Cs2Area->datasectstotrans = (u16)Cs2PutCount;

   doCDReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_DRDY);

}

//////////////////////////////////////////////////////////////////////////////

// Teste un bloc deja stocke contre les conditions d'un filtre (sous-en-tete
// Mode 2 + FAD range), comme la partie "conditions" de Cs2FilterData() mais sur
// un block_struct (qui porte deja FAD/cn/fn/sm/ci) au lieu du workblock brut, et
// sans allocation ni conversion. Renvoie 1 si le bloc satisfait le filtre.
// Reference : Copy/Move Sector (0x65/0x66) filtrent chaque secteur via le filtre
// du selector destination (writeup Pseudo Saturn / wiki.yabause.org/CDBlock).
static int Cs2BlockPassesFilter(block_struct *blk, filter_struct *f, int isaudio)
{
   int cond = 1;

   if (blk->data[0xF] == 0x02 && !isaudio)
   {
      if (f->mode & 0x01) { if (blk->fn != f->fid)  cond = 0; }                 // File Number
      if (f->mode & 0x02) { if (blk->cn != f->chan) cond = 0; }                 // Channel Number
      if (f->mode & 0x04) { if ((blk->sm & f->smmask) != f->smval) cond = 0; }  // Sub Mode
      if (f->mode & 0x08) { if ((blk->ci & f->cimask) != f->cival) cond = 0; }  // Coding Info
      if (f->mode & 0x10) cond ^= 1;                                            // Reverse
   }

   if (f->mode & 0x40)                                                          // FAD Range
   {
      if (blk->FAD < f->FAD || blk->FAD >= (f->FAD + f->range))
         cond = 0;
   }

   return cond;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2CopySectorData(void) {
 // Layout registres VALIDE sur materiel reel (writeup Pseudo Saturn / auth type-2,
 // cf. wiki.yabause.org/CDBlock) : dest = CR1[7:0], source = CR3[15:8], count = CR4.
 // Chaque secteur est FILTRE par le filtre du selector destination : seuls ceux
 // qui satisfont ses conditions (FAD range / sous-en-tete) sont copies. Avec les
 // filtres par defaut (mode=0), tout passe -> comportement "copier tout" inchange.
 u32 source = Cs2Area->reg.CR3 >> 8;
  u32 offset = Cs2Area->reg.CR2;
  u32 dest = Cs2Area->reg.CR1 & 0xFF;
  // Le nombre de secteurs (CR4) est un mot 16 bits ; 0xFFFF = "tous les
  // secteurs restants". Le masque & 0xFF rendait le cas 0xFFFF impossible a
  // detecter (devenait 0xFF=255) et tronquait tout compte > 255.
  u32 count = Cs2Area->reg.CR4;       // etait: & 0xFF

  if (source >= 0x18 || dest >= 0x18) {
    setStatus(CDB_STAT_ERROR); // ToDo: check
    doCDReport(Cs2Area->status);
    Cs2SetIRQ(CDB_HIRQ_CMOK);
    return;
  }

  partition_struct *putpartition = &Cs2Area->partition[dest];
  partition_struct *srcpartition = &Cs2Area->partition[source];
  filter_struct *dstfilter = &Cs2Area->filter[dest]; // filtre du selector destination
  if (offset == 0xFFFF) {
    offset = srcpartition->numblocks - 1;
  }

  if (count == 0xFFFF) {
    count = srcpartition->numblocks - offset;
  }

  for (int i = 0; i < count; i++) {
    // Borne sur les blocs source reellement presents (evite OOB / NULL)
    if ((offset + i) >= srcpartition->numblocks ||
        srcpartition->block[offset + i] == NULL)
       break;
    block_struct *sblk = srcpartition->block[offset + i];
    // Filtrage materiel : un secteur qui ne passe pas le filtre destination est ignore
    if (!Cs2BlockPassesFilter(sblk, dstfilter, 0))
       continue;
    block_struct *dblk = Cs2AllocateBlock(&putpartition->blocknum[putpartition->numblocks], 2352);
    if (dblk == NULL)        // buffer plein : ne pas dereferencer NULL
       break;
    putpartition->block[putpartition->numblocks] = dblk;
    memcpy(dblk->data, sblk->data, sizeof(u8) * 2352);
    // Conserver les metadonnees (sinon FAD/cn/fn/sm/ci du bloc copie restent indefinis,
    // ce qui casserait un filtrage ulterieur sur la partition destination)
    dblk->size = sblk->size;
    dblk->FAD  = sblk->FAD;
    dblk->cn   = sblk->cn;
    dblk->fn   = sblk->fn;
    dblk->sm   = sblk->sm;
    dblk->ci   = sblk->ci;
    putpartition->numblocks++;
    putpartition->size += sblk->size;
  }


  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ECPY);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MoveSectorData(void) {
  // Layout VALIDE materiel reel (writeup Pseudo Saturn) : dest=CR1[7:0], source=CR3[15:8], count=CR4.
  // Chaque secteur est FILTRE par le filtre du selector destination ; un secteur qui ne
  // passe pas reste dans la source. Filtres par defaut (mode=0) -> tout passe, "deplacer tout".
  u32 source = Cs2Area->reg.CR3 >> 8;
  u32 offset = Cs2Area->reg.CR2;
  u32 dest = Cs2Area->reg.CR1 & 0xFF;
  // Idem CopySectorData : CR4 est un mot 16 bits, 0xFFFF = "tous". Le masque
  // & 0xFF cassait la semantique "deplacer tout" et tout compte > 255.
  u32 count = Cs2Area->reg.CR4;       // etait: & 0xFF

  if (source >= 0x18 || dest >= 0x18) {
    setStatus(CDB_STAT_ERROR); // ToDo: check
    doCDReport(Cs2Area->status);
    Cs2SetIRQ(CDB_HIRQ_CMOK);
    return;
  }

  partition_struct *putpartition = &Cs2Area->partition[dest];
  partition_struct *srcpartition = &Cs2Area->partition[source];
  filter_struct *dstfilter = &Cs2Area->filter[dest]; // filtre du selector destination
  // Borne STABLE : numblocks decroit dans la boucle (on retire des blocs), il ne faut
  // donc pas l'utiliser comme limite sous peine de couper la boucle a mi-chemin.
  u32 src_orig = srcpartition->numblocks;
  if (offset == 0xFFFF) {
    offset = src_orig - 1;
  }

  if (count == 0xFFFF) {
    count = src_orig - offset;
  }

  for (int i = 0; i < count; i++) {
    // Borne sur les blocs source reellement presents (evite OOB / underflow numblocks)
    if ((offset + i) >= src_orig ||
        srcpartition->block[offset + i] == NULL)
       break;
    block_struct *sblk = srcpartition->block[offset + i];
    // Filtrage materiel : un secteur qui ne passe pas le filtre destination reste dans la source
    if (!Cs2BlockPassesFilter(sblk, dstfilter, 0))
       continue;
    putpartition->block[putpartition->numblocks] = sblk;
    putpartition->blocknum[putpartition->numblocks] = srcpartition->blocknum[offset + i];
    putpartition->numblocks++;
    putpartition->size += sblk->size;
    srcpartition->block[offset + i] = NULL;
    srcpartition->blocknum[offset + i] = 0xFF;
    srcpartition->numblocks--;
    srcpartition->size -= sblk->size;
  }

  Cs2SortBlocks(&Cs2Area->partition[source]);
  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_ECPY);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetCopyError(void) {
  Cs2Area->reg.CR1 = Cs2Area->status << 8;
  Cs2Area->reg.CR2 = 0;
  Cs2Area->reg.CR3 = 0;
  Cs2Area->reg.CR4 = 0;
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2ChangeDirectory(void) {
  u32 cdfilternum;

  cdfilternum = (Cs2Area->reg.CR3 >> 8);

  if (cdfilternum == 0xFF)
  {
     doCDReport(CDB_STAT_REJECT);
     Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EFLS);
     return;
  }
  else if (cdfilternum < MAX_SELECTORS)
  {
     if (Cs2ReadFileSystem(Cs2Area->filter + cdfilternum, ((Cs2Area->reg.CR3 & 0xFF) << 16) | Cs2Area->reg.CR4, 0) != 0)
     {
        CDLOG("cs2\t: ReadFileSystem failed\n");
        doCDReport(CDB_STAT_REJECT);
        Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EFLS);
        return;
     }
  }

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EFLS);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2ReadDirectory(void) {
  u32 rdfilternum;

  rdfilternum = (Cs2Area->reg.CR3 >> 8);

  if (rdfilternum == 0xFF)
  {
     doCDReport(CDB_STAT_REJECT);
     Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EFLS);
     return;
  }
  else if (rdfilternum < MAX_SELECTORS)
  {
		// ST-040-R4-051795, §6.9 « Read Directory (command 0x71) » :
		// CR3[7:0] = FAD[23:16], CR4[15:0] = FAD[15:0]
     if (Cs2ReadFileSystem(Cs2Area->filter + rdfilternum, ((Cs2Area->reg.CR3 & 0xFF) << 16) | Cs2Area->reg.CR4, 1) != 0) // était: << 8
     {
        CDLOG("cs2\t: ReadFileSystem failed\n");
        doCDReport(CDB_STAT_REJECT);
        Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EFLS);
        return;
     }
  }

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EFLS);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetFileSystemScope(void) {
  // may need to fix this
  Cs2Area->reg.CR1 = Cs2Area->status << 8;
  Cs2Area->reg.CR2 = (u16)(Cs2Area->numfiles - 2);
  Cs2Area->reg.CR3 = 0x0100;
  Cs2Area->reg.CR4 = 0x0002;

  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EFLS);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetFileInfo(void) {
  u32 gfifid;

  gfifid = ((Cs2Area->reg.CR3 & 0xFF) << 16) | Cs2Area->reg.CR4;

  if (gfifid == 0xFFFFFF)
  {
     Cs2Area->transfercount = 0;
     Cs2Area->infotranstype = 2;

     Cs2Area->reg.CR1 = Cs2Area->status << 8;
     Cs2Area->reg.CR2 = 0x05F4;
     Cs2Area->reg.CR3 = 0;
     Cs2Area->reg.CR4 = 0;
  }
  else
  {
     Cs2SetupFileInfoTransfer(gfifid);

     Cs2Area->transfercount = 0;
     Cs2Area->infotranstype = 1;

     Cs2Area->reg.CR1 = Cs2Area->status << 8;
     Cs2Area->reg.CR2 = 0x06;
     Cs2Area->reg.CR3 = 0;
     Cs2Area->reg.CR4 = 0;
  }

  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_DRDY);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2ReadFile(void) {
  Cs2CancelPlayEnd();   /* a new drive command replaces a pending play end */
  u32 rfoffset, rffilternum, rffid, rfsize;

  // FIXED: rfoffset = CR2 seul (Sector Offset)
  // CR1[7:0] est réservé pour cette commande, pas partie de l'offset
  // Ref: ST-040-R4-051795 §6.14 "Read File (command 0x74)"
  rfoffset    = Cs2Area->reg.CR2;                              // FIXED: était (CR1&0xFF)<<8 | CR2
  rffilternum = Cs2Area->reg.CR3 >> 8;                        // correct, inchangé
  rffid       = ((Cs2Area->reg.CR3 & 0xFF) << 8) | Cs2Area->reg.CR4;  // correct, inchangé

  // rffid and rffilternum are game-supplied register values with no
  // built-in bound; fileinfo[]/filter[] are only MAX_FILES/MAX_SELECTORS
  // entries. Every equivalent index elsewhere in this file (rdfilternum,
  // dsdbufno, casbufno, ...) is checked before use -- this one wasn't.
  if (rffid >= MAX_FILES || rffilternum >= MAX_SELECTORS)
  {
     doCDReport(CDB_STAT_REJECT);
     Cs2SetIRQ(CDB_HIRQ_CMOK);
     return;
  }

  rfsize = ((Cs2Area->fileinfo[rffid].size + Cs2Area->getsectsize - 1) /
           Cs2Area->getsectsize) - rfoffset;

  Cs2SetupDefaultPlayStats(Cs2FADToTrack(Cs2Area->fileinfo[rffid].lba + rfoffset), 0);
  Cs2Area->maxrepeat = 0;
  Cs2Area->playFAD = Cs2Area->FAD = Cs2Area->fileinfo[rffid].lba + rfoffset;
  Cs2Area->playendFAD = Cs2Area->playFAD + rfsize;
  Cs2Area->options = 0x8;
  Cs2SetTiming(1);                   // FIXED: décommenté
  Cs2Area->outconcddev = Cs2Area->filter + rffilternum;
  setStatus(CDB_STAT_PLAY);
  Cs2Area->_periodiccycles = 0;
  Cs2Area->playtype = CDB_PLAYTYPE_FILE;
  Cs2Area->cdi->ReadAheadFAD(Cs2Area->FAD);
  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2AbortFile(void) {
  Cs2CancelPlayEnd();   /* a new drive command replaces a pending play end */
    if ((Cs2Area->status & 0xF) != CDB_STAT_OPEN &&
        (Cs2Area->status & 0xF) != CDB_STAT_NODISC)
        setStatus(CDB_STAT_PAUSE);

    Cs2Area->isonesectorstored = 0;
    Cs2Area->datatranstype     = CDB_DATATRANSTYPE_INVALID;
    Cs2Area->cdwnum            = 0;
	// ST-040-R4-051795, §6.15 « Abort File (command 0x75) » :
	// The drive returns to Pause state ; all pending sector transfers are cancelled.
    Cs2Area->_periodiccycles   = 0;
    Cs2Area->_periodictiming   = 0;

    doCDReport(Cs2Area->status);
    Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_EFLS);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegGetStatus(void) {
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegGetInterrupt(void) {
   u32 mgiworkinterrupt;

   CS2_MPEG_CMD_TRACE("MPEG Get Interrupt (0x91)");

   // Factors accumulated since the last read. The mask gates the interrupt
   // line (Cs2SetIRQ), not what this command reports: ST-162 §3.1 says a
   // masked factor is still visible in the request register so it can be
   // polled. Reporting mgiworkinterrupt & mpegintmask meant a program that
   // never set a mask -- which is what the Video CD player does -- always
   // read back zero.
   mgiworkinterrupt = Cs2MpegIntPending;
   Cs2MpegIntPending = 0; // reading the factors clears them

   Cs2Area->reg.CR1 = (u16)((Cs2Area->status << 8) | ((mgiworkinterrupt >> 16) & 0xFF));
   Cs2Area->reg.CR2 = (u16) mgiworkinterrupt;
   Cs2Area->reg.CR3 = 0;
   Cs2Area->reg.CR4 = 0;
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);

}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetInterruptMask(void) {
   CS2_MPEG_CMD_TRACE("MPEG Set Interrupt Mask (0x92)");
   Cs2Area->mpegintmask = ((Cs2Area->reg.CR1 & 0xFF) << 16) | Cs2Area->reg.CR2;

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);

}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegInit(void) {
  CS2_MPEG_CMD_TRACE("MPEG Init (0x93)");

  // MPEG Init means "reset the decoder", and the Video CD player issues it
  // whenever playback stops (stop, and before restarting after a pause). The
  // decoder has to genuinely start over: after a stop the drive seeks
  // elsewhere, so the bytes arriving next do not continue the stream still
  // sitting in the decoder's input buffer. Only clearing the picture, as this
  // did, left that buffer and the old stream position in place and the
  // demuxer then tried to decode across the splice -- pressing Play again
  // gave a frozen picture. A full reset makes it re-lock on the new data,
  // which takes well under a second (see MpegCardForceStreams).
  //
  // The picture size is cleared too, so that the "size changed" test in
  // Cs2MpegUpdateStatus() fires again and the player gets a fresh
  // "picture size available" interrupt (factor 0x08) for the new stream,
  // exactly as it did for the first one.
  Cs2MpegPlaying = 0;
  Cs2MpegIntPending = 0;
  Cs2Area->mpegpicturewidth = 0;
  Cs2Area->mpegpictureheight = 0;
  MpegCardReset();
  Cs2MpegApplyDecodingMethod(0, 0, 0, 1);   // manual: mute 00H, paused, frozen
  Cs2MpegUpdateStatus();

  if (Cs2Area->mpgauth)
     Cs2Area->reg.CR1 = Cs2Area->status << 8;
  else
     Cs2Area->reg.CR1 = 0xFF00;

  // double-check this
  if (Cs2Area->reg.CR2 == 0x0001) // software timer/reset?
    Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM | CDB_HIRQ_MPED | CDB_HIRQ_MPST );
  else
    Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPED | CDB_HIRQ_MPST);

  Cs2Area->reg.CR2 = 0;
  Cs2Area->reg.CR3 = 0;
  Cs2Area->reg.CR4 = 0;

  // future mpeg-related variables should be initialized here
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetMode(void) {
   CS2_MPEG_CMD_TRACE("MPEG Set Mode (0x94)");
   u8 vidplaymode=Cs2Area->reg.CR1 & 0xFF;
   u8 dectimingmode=Cs2Area->reg.CR2 >> 8;
   u8 outmode=Cs2Area->reg.CR2 & 0xFF;
   u8 slmode=Cs2Area->reg.CR3 >> 8;

   if (vidplaymode != 0xFF)
      Cs2Area->mpegmode.vidplaymode = vidplaymode;

   if (dectimingmode != 0xFF)
      Cs2Area->mpegmode.dectimingmode = dectimingmode;

   if (outmode != 0xFF)
      Cs2Area->mpegmode.outmode = outmode;

   if (slmode != 0xFF)
      Cs2Area->mpegmode.slmode = slmode;

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM );
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegPlay(void) {
   CS2_MPEG_CMD_TRACE("MPEG Play (0x95)");

   Cs2MpegPlaying = 1;
   Cs2MpegUpdateStatus();

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

static void Cs2MpegApplyDecodingMethod(u8 mute, u16 pautim, u16 frztim, int init)
{
   if (init || !(mute & 0x80))
      MpegCardSetMute(mute & 3);

   if (init)
      Cs2MpegStep = 0;

   if (init || pautim != 0xFFFF)
   {
      /* "0000H: pause (re-pause, frame-by-frame)": already paused -> one
         picture forward, then paused again */
      if (!init && pautim == 0 && Cs2MpegPauTim == 0)
         Cs2MpegStep++;
      Cs2MpegPauTim = pautim;
      Cs2MpegSlowCount = 0;
   }

   if (init || frztim != 0xFFFF)
   {
      Cs2MpegFrzTim = frztim;
      MpegCardSetFreeze(frztim == 0, (frztim >= 2 && frztim != 0xFFFF) ? frztim : 0);
   }
}

void Cs2MpegSetDecodingMethod(void) {
   CS2_MPEG_CMD_TRACE("MPEG Set Decoding Method (0x96)");
   // CDC_MpSetDec (MPEG part 20.8): mute in CR1 low byte (bit 7 = no
   // change), pautim in CR2, frztim in CR4 -- the layout the Video CD player
   // sends: 9604 0001 0000 0001 (sound on, release pause and freeze) before
   // MPEG Play, 9607 FFFF 0000 FFFF (mute both channels) after it.
   Cs2MpegApplyDecodingMethod((u8)(Cs2Area->reg.CR1 & 0xFF), Cs2Area->reg.CR2, Cs2Area->reg.CR4, 0);
   Cs2MpegUpdateStatus();

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////
// MPEG Out Decoding Sync (command 0x97). Not covered by any document we
// could locate (unlike the CD-specific commands, ST-040-R4-051795 stops
// short of the MPEG command set); acknowledged as a one-shot trigger,
// following the same pattern already used for Cs2MpegPlay/
// Cs2MpegSetDecodingMethod above rather than inventing a persisted state
// this project has no way to verify.
void Cs2MpegOutDecodingSync(void) {
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////
// MPEG Get Timecode (command 0x98): reports Cs2Area->mpegtimecode, the
// running decode position VideoCdPlayerAdvance() maintains (see
// mpegcard.c) across CR3:CR4, mirroring how doCDReport() spreads the FAD
// across the same two words.
void Cs2MpegGetTimecode(void) {
   Cs2Area->reg.CR1 = (Cs2Area->status << 8) | Cs2Area->actionstatus;
   Cs2Area->reg.CR2 = Cs2Area->vcounter;
   Cs2Area->reg.CR3 = (u16)(Cs2Area->mpegtimecode >> 16);
   Cs2Area->reg.CR4 = (u16)(Cs2Area->mpegtimecode);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////
// MPEG Get Pts (command 0x99): reports Cs2Area->mpegpts, the last decoded
// picture's presentation timestamp in 90kHz units (ISO/IEC 11172-1
// §2.4.4.3), across CR3:CR4 the same way.
void Cs2MpegGetPts(void) {
   Cs2Area->reg.CR1 = (Cs2Area->status << 8) | Cs2Area->actionstatus;
   Cs2Area->reg.CR2 = Cs2Area->vcounter;
   Cs2Area->reg.CR3 = (u16)(Cs2Area->mpegpts >> 16);
   Cs2Area->reg.CR4 = (u16)(Cs2Area->mpegpts);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetConnection(void) {
   int mscnext = (Cs2Area->reg.CR3 >> 8);
   int wasoff = (Cs2Area->mpegcon[0].audcon == 0 && Cs2Area->mpegcon[0].vidcon == 0);


   if (mscnext == 0)
   {
      // Current
      Cs2Area->mpegcon[0].audcon = Cs2Area->reg.CR1 & 0xFF;
      Cs2Area->mpegcon[0].audlay = Cs2Area->reg.CR2 >> 8;
      Cs2Area->mpegcon[0].audbufnum = Cs2Area->reg.CR2 & 0xFF;
      Cs2Area->mpegcon[0].vidcon = Cs2Area->reg.CR3 & 0xFF;
      Cs2Area->mpegcon[0].vidlay = Cs2Area->reg.CR4 >> 8;
      Cs2Area->mpegcon[0].vidbufnum = Cs2Area->reg.CR4 & 0xFF;

      // Coming back from a full stop (both modes 0): the decoder still holds
      // the stream it was decoding before, and the disc has been re-seeked
      // meanwhile, so the bytes that arrive next do not continue it. Reset it
      // here as well as in MPEG Init, because the player restarts by
      // rebuilding the CD side first and only then re-enabling the streams --
      // without this the decoder tries to span the splice and shows nothing.
      if (wasoff && (Cs2Area->mpegcon[0].audcon != 0 || Cs2Area->mpegcon[0].vidcon != 0))
      {
         // The partitions still hold the sectors that were queued when the
         // user pressed Stop -- the log shows all 200 of them still there.
         // Those belong to the old play position, so feeding them to the
         // freshly reset decoder would splice two unrelated stretches of
         // stream together and leave the picture black. Drop them, and let
         // the decoder start on the sectors the drive is about to read.
         int p, dropped = 0;
         for (p = 0; p < 2; p++)
         {
            u8 bufno = p ? Cs2Area->mpegcon[0].audbufnum : Cs2Area->mpegcon[0].vidbufnum;
            partition_struct *part;

            if (bufno >= MAX_SELECTORS)
               continue;
            if (p && bufno == Cs2Area->mpegcon[0].vidbufnum)
               continue;  // same partition, already emptied

            part = &Cs2Area->partition[bufno];
            while (part->numblocks > 0 && part->block[0] != NULL)
            {
               part->size -= part->block[0]->size;
               Cs2FreeBlock(part->block[0]);
               part->block[0] = NULL;
               part->blocknum[0] = 0xFF;
               Cs2SortBlocks(part);
               part->numblocks--;
               dropped++;
            }
         }

         if (dropped && Cs2Area->blockfreespace == MAX_BLOCKS)
            Cs2Area->isonesectorstored = 0;



         MpegCardReset();
         Cs2Area->mpegpicturewidth = 0;
         Cs2Area->mpegpictureheight = 0;
         Cs2MpegIntPending = 0;
      }


      Cs2MpegConnSet = 1;
      CS2_MPEG_CMD_TRACE("MPEG Set Connection decoded as above:");
   }
   else
   {
      // Next
      Cs2Area->mpegcon[1].audcon = Cs2Area->reg.CR1 & 0xFF;
      Cs2Area->mpegcon[1].audlay = Cs2Area->reg.CR2 >> 8;
      Cs2Area->mpegcon[1].audbufnum = Cs2Area->reg.CR2 & 0xFF;
      Cs2Area->mpegcon[1].vidcon = Cs2Area->reg.CR3 & 0xFF;
      Cs2Area->mpegcon[1].vidlay = Cs2Area->reg.CR4 >> 8;
      Cs2Area->mpegcon[1].vidbufnum = Cs2Area->reg.CR4 & 0xFF;

   }

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegGetConnection(void) {
   int mgcnext = (Cs2Area->reg.CR3 >> 8);

   if (mgcnext == 0)
   {
      // Current
      Cs2Area->reg.CR1 = (Cs2Area->status << 8) | Cs2Area->mpegcon[0].audcon;
      Cs2Area->reg.CR2 = (Cs2Area->mpegcon[0].audlay << 8) | Cs2Area->mpegcon[0].audbufnum;
      Cs2Area->reg.CR3 = Cs2Area->mpegcon[0].vidcon;
      Cs2Area->reg.CR4 = (Cs2Area->mpegcon[0].vidlay << 8) | Cs2Area->mpegcon[0].vidbufnum;
   }
   else
   {
      // Next
      Cs2Area->reg.CR1 = (Cs2Area->status << 8) | Cs2Area->mpegcon[1].audcon;
      Cs2Area->reg.CR2 = (Cs2Area->mpegcon[1].audlay << 8) | Cs2Area->mpegcon[1].audbufnum;
      Cs2Area->reg.CR3 = Cs2Area->mpegcon[1].vidcon;
      Cs2Area->reg.CR4 = (Cs2Area->mpegcon[1].vidlay << 8) | Cs2Area->mpegcon[1].vidbufnum;
   }

   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////
// MPEG Change Connection (command 0x9C): activates the "next" connection
// bank Cs2MpegSetConnection() staged into mpegcon[1], the same
// current/next double-buffering convention SCU/VDP2 use for their own
// "reflected at the next field/line" registers. Layout undocumented
// beyond the command's name; this is the natural reading of it given
// mpegcon[] already existing as a [current,next] pair.
void Cs2MpegChangeConnection(void) {
   // A Nova trace of the same player prints this command as
   //   MpChgCon [chg_a: 00, chg_v: FF, clr_a: 00, clr_v: 00]
   //   MpChgCon [chg_a: FF, chg_v: 00, clr_a: 00, clr_v: 00]
   // It is not a bank swap (my first reading): it carries one value per
   // stream, and 0xFF means "leave this one alone", the convention every
   // other command in this API uses for an unchanged field. The player uses
   // it to switch the audio and the video connection one after the other,
   // which a swap could never express -- swapping twice just put everything
   // back where it started.
   //
   // Field placement confirmed by lining up Kronos's raw registers with a
   // Nova trace of the same two commands:
   //   CR2=00FF -> MpChgCon [chg_a: 00, chg_v: FF, ...]
   //   CR2=FF00 -> MpChgCon [chg_a: FF, chg_v: 00, ...]
   // so the two "change this stream" flags are the high and low bytes of
   // CR2, and the clear flags are the two bytes of CR3. My first guess put
   // them in CR1/CR3, which read both flags as 0x00 and therefore replaced
   // *both* connections at once.
   u8 chg_a = (u8)(Cs2Area->reg.CR2 >> 8);
   u8 chg_v = (u8)(Cs2Area->reg.CR2 & 0xFF);
   u8 clr_a = (u8)(Cs2Area->reg.CR3 >> 8);
   u8 clr_v = (u8)(Cs2Area->reg.CR3 & 0xFF);

   CS2_MPEG_CMD_TRACE("MPEG Change Connection (0x9C)");

   // CDC_MpChgCon (MPEG part, 8.2.2): "Forcibly switch the connection
   // destination of the MPEG decoder. Alternatively, forcibly disconnect the
   // connection destination. To forcibly terminate MPEG playback, specify
   // disconnection with this function."
   //   chg_a / chg_v: CDC_MPCOF_ABT (00H) detachment (forced termination),
   //                  CDC_MPCOF_CHG (01H) forced switch to the next stream
   //                  registered by CDC_MpSetCon(CDC_MPSTF_NEXT),
   //                  CDC_PARA_NOCHG (FFH) no change.
   // (ABT/CHG values: beetle-saturn mpeg.h, Mednafen-derived; the manual
   // only gives the symbols. CDC_MpGetCon returns CDC_NUL_SEL as the
   // buffer partition when "not connected".)
   //
   // A detachment disconnects the stream: its buffer partition becomes
   // NUL_SEL (FFH), which MPEG Get Connection (9Bh) then reports. The Video
   // CD player reads it after a Stop, a fast forward / reverse jump or a
   // chapter change, and only sends MPEG Set Decoding Method, MPEG Play and
   // MPEG Set Connection again when the connection is gone -- Nova trace of
   // the same player: MpChgCon (ABT audio), MpChgCon (ABT video), CdPlay,
   // MpSetDec 04/0001/0001, MpPlay, MpSetCon<CUR> 06/26. Kronos stored the
   // flag as the connection mode and left the partitions attached; the
   // player then skipped straight to waiting for factor 08H and nothing
   // reached the decoder again (fast forward hung, watchdog reset).
   if (chg_a == 0x01)
   {
      Cs2Area->mpegcon[0].audcon = Cs2Area->mpegcon[1].audcon;
      Cs2Area->mpegcon[0].audlay = Cs2Area->mpegcon[1].audlay;
      Cs2Area->mpegcon[0].audbufnum = Cs2Area->mpegcon[1].audbufnum;
   }
   else if (chg_a != 0xFF)
      Cs2Area->mpegcon[0].audbufnum = 0xFF;   // CDC_MPCOF_ABT

   if (chg_v == 0x01)
   {
      Cs2Area->mpegcon[0].vidcon = Cs2Area->mpegcon[1].vidcon;
      Cs2Area->mpegcon[0].vidlay = Cs2Area->mpegcon[1].vidlay;
      Cs2Area->mpegcon[0].vidbufnum = Cs2Area->mpegcon[1].vidbufnum;
   }
   else if (chg_v != 0xFF)
      Cs2Area->mpegcon[0].vidbufnum = 0xFF;   // CDC_MPCOF_ABT

   (void)clr_a;

   // Both streams disconnected: playback is terminated. The decoder drops
   // what it holds (the next data comes from elsewhere on the disc) and the
   // status reports "stopped" until the next MPEG Play.
   if (Cs2Area->mpegcon[0].audbufnum == 0xFF && Cs2Area->mpegcon[0].vidbufnum == 0xFF)
   {
      Cs2MpegPlaying = 0;
      MpegCardReset();
      if (clr_v == 0x00)          // CDC_MPCLV_FRM: VBV and WBC cleared now
         MpegCardClearPicture();
      Cs2Area->mpegpicturewidth = 0;
      Cs2Area->mpegpictureheight = 0;
   }

   Cs2MpegConnSet = 1;


   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetStream(void) {
   CS2_MPEG_CMD_TRACE("MPEG Set Stream (0x9D)");
   int mssnext = (Cs2Area->reg.CR3 >> 8);

   if (mssnext == 0)
   {
      // Current
      Cs2Area->mpegstm[0].audstm = Cs2Area->reg.CR1 & 0xFF;
      Cs2Area->mpegstm[0].audstmid = Cs2Area->reg.CR2 >> 8;
      Cs2Area->mpegstm[0].audchannum = Cs2Area->reg.CR2 & 0xFF;
      Cs2Area->mpegstm[0].vidstm = Cs2Area->reg.CR3 & 0xFF;
      Cs2Area->mpegstm[0].vidstmid = Cs2Area->reg.CR4 >> 8;
      Cs2Area->mpegstm[0].vidchannum = Cs2Area->reg.CR4 & 0xFF;
   }
   else
   {
      // Next
      Cs2Area->mpegstm[1].audstm = Cs2Area->reg.CR1 & 0xFF;
      Cs2Area->mpegstm[1].audstmid = Cs2Area->reg.CR2 >> 8;
      Cs2Area->mpegstm[1].audchannum = Cs2Area->reg.CR2 & 0xFF;
      Cs2Area->mpegstm[1].vidstm = Cs2Area->reg.CR3 & 0xFF;
      Cs2Area->mpegstm[1].vidstmid = Cs2Area->reg.CR4 >> 8;
      Cs2Area->mpegstm[1].vidchannum = Cs2Area->reg.CR4 & 0xFF;
   }

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegGetStream(void) {
   int mgsnext = (Cs2Area->reg.CR3 >> 8);
   static int traced = 0;

   // The Video CD player asks which streams the decoder locked onto, but it
   // never issues MPEG Set Stream (0x9D), so mpegstm[] was still all zeros
   // and this answered "no audio stream, no video stream". While playing,
   // report the identifiers a Video CD always uses (White Book: MPEG-1 video
   // on 0xE0, MPEG-1 Layer II audio on 0xC0), on the channels the
   // application selected through MPEG Set Connection.
   if (Cs2MpegPlaying && Cs2Area->mpegstm[0].vidstmid == 0 &&
       Cs2Area->mpegstm[0].audstmid == 0)
   {
      Cs2Area->mpegstm[0].vidstm = 1;
      Cs2Area->mpegstm[0].vidstmid = 0xE0;
      Cs2Area->mpegstm[0].vidchannum = Cs2Area->mpegcon[0].vidbufnum;
      Cs2Area->mpegstm[0].audstm = 1;
      Cs2Area->mpegstm[0].audstmid = 0xC0;
      Cs2Area->mpegstm[0].audchannum = Cs2Area->mpegcon[0].audbufnum;
   }

   if (mgsnext == 0)
   {
      // Current
      Cs2Area->reg.CR1 = (Cs2Area->status << 8) | Cs2Area->mpegstm[0].audstm;
      Cs2Area->reg.CR2 = (Cs2Area->mpegstm[0].audstmid << 8) | Cs2Area->mpegstm[0].audchannum;
      Cs2Area->reg.CR3 = Cs2Area->mpegstm[0].vidstm;
      Cs2Area->reg.CR4 = (Cs2Area->mpegstm[0].vidstmid << 8) | Cs2Area->mpegstm[0].vidchannum;
   }
   else
   {
      // Next
      Cs2Area->reg.CR1 = (Cs2Area->status << 8) | Cs2Area->mpegstm[1].audstm;
      Cs2Area->reg.CR2 = (Cs2Area->mpegstm[1].audstmid << 8) | Cs2Area->mpegstm[1].audchannum;
      Cs2Area->reg.CR3 = Cs2Area->mpegstm[1].vidstm;
      Cs2Area->reg.CR4 = (Cs2Area->mpegstm[1].vidstmid << 8) | Cs2Area->mpegstm[1].vidchannum;
   }

   if (!traced)
   {
      traced = 1;
      CS2_MPEG_CMD_TRACE("MPEG Get Stream (0x9E) answered");
   }

   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////
// MPEG Get Picture Size (command 0x9F): reports the currently decoded
// picture's dimensions across CR3:CR4 (same slots doMPEGReport() uses for
// pictureinfo/mpegvideostatus, since this command returns picture data
// instead of the general MPEG status). Cs2Area->mpegpicturewidth/height
// default to the White Book Video CD's standard NTSC frame size
// (352x240; PAL discs use 352x288) until mpegcard.c starts decoding a
// real stream and updates them per-frame.
void Cs2MpegGetPictureSize(void) {
   CS2_MPEG_CMD_TRACE("MPEG Get Picture Size (0x9F)");
   Cs2Area->reg.CR1 = (Cs2Area->status << 8) | Cs2Area->actionstatus;
   Cs2Area->reg.CR2 = Cs2Area->vcounter;
   Cs2Area->reg.CR3 = Cs2Area->mpegpicturewidth;
   Cs2Area->reg.CR4 = Cs2Area->mpegpictureheight;
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegDisplay(void) {
   CS2_MPEG_CMD_TRACE("MPEG Display (0xA0)");
   // "MPEG Display"(0xA0): toggles the "display enable" bit of the LSI
   // status register (see the mpeglsi_struct comment in cs2.h) on/off.
   // CR1 low byte: 0=hide the decoded picture, 1=show it, 0xFF=leave as-is
   // (0xFF-means-"unchanged" is the same convention Cs2MpegSetMode()
   // already uses for its own CR1-CR3 fields just above).
   u8 dispctrl = Cs2Area->reg.CR1 & 0xFF;

   if (dispctrl != 0xFF)
   {
      if (dispctrl)
         Cs2Area->mpeglsi.status |= 0x02;
      else
         Cs2Area->mpeglsi.status &= ~0x02;
   }

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetWindow(void) {
   CS2_MPEG_CMD_TRACE("MPEG Set Window (0xA1)");
   // "MPEG Set Window"(0xA1): display-window rectangle. See the
   // mpegwindow_struct comment in cs2.h for why this is read as a
   // top-left/bottom-right rectangle across all four command words.
   // X/Y position are also mirrored into the LSI register bank, since
   // that is where 0xAE/0xAF (Get/Set LSI) and real BIOS window-drawing
   // code would read them back from.
   Cs2Area->mpegwindow.x1 = Cs2Area->reg.CR1;
   Cs2Area->mpegwindow.y1 = Cs2Area->reg.CR2;
   Cs2Area->mpegwindow.x2 = Cs2Area->reg.CR3;
   Cs2Area->mpegwindow.y2 = Cs2Area->reg.CR4;

   Cs2Area->mpeglsi.xpos = Cs2Area->mpegwindow.x1;
   Cs2Area->mpeglsi.ypos = Cs2Area->mpegwindow.y1;

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetBorderColor(void) {
   // "MPEG Set Border Color"(0xA2): only CR1/CR2 carry data (see the
   // command's own CDLOG line above), so the color is read the same way
   // Cs2GetMPEGRom() already reads its own 24-bit offset field: low byte
   // of CR1 as the high byte, CR2 as the low 16 bits.
   Cs2Area->mpeglsi.bordercolor = ((u32)(Cs2Area->reg.CR1 & 0xFF) << 16) | Cs2Area->reg.CR2;

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetFade(void) {
   // "MPEG Set Fade"(0xA3): rate in CR1, direction in CR2, see the
   // mpegfade_struct comment in cs2.h (modelled on the VDP2 gradation
   // function, ST-058-R2 §12.2, the only documented Saturn fading
   // hardware).
   Cs2Area->mpegfade.rate = Cs2Area->reg.CR1 & 0xFF;
   Cs2Area->mpegfade.direction = Cs2Area->reg.CR2 & 0xFF;

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetVideoEffects(void) {
   // "MPEG Set Video Effects"(0xA4): bit-level meaning of the effect
   // flags isn't publicly documented anywhere we could find, so CR1 is
   // stored as-is and echoed back verbatim by Cs2MpegGetStatus() rather
   // than being decoded into effects we can't verify.
   Cs2Area->mpegvideoeffects = Cs2Area->reg.CR1;

   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////
// MPEG Get/Set Image, MPEG Read/Write Image, MPEG Read/Write Sector
// (commands 0xA5-0xAA): these six commands have no public documentation
// at all -- not even an opcode-only mention survives outside the Yabause
// wiki's command list, and no other Saturn emulator implements them
// either. Rather than invent a plausible-looking but unverifiable buffer
// format, they are acknowledged as harmless completed commands, the same
// spirit as Cs2MpegPlay/Cs2MpegSetDecodingMethod's existing "fix me"
// stubs: any game or BIOS path that calls them gets a valid CMOK/MPCM
// response instead of falling through to "Command %02x not implemented"
// and stalling.

void Cs2MpegGetImage(void) {                       // 0xA5
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

void Cs2MpegSetImage(void) {                       // 0xA6
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

void Cs2MpegReadImage(void) {                      // 0xA7
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

void Cs2MpegWriteImage(void) {                     // 0xA8
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

void Cs2MpegReadSector(void) {                     // 0xA9
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

void Cs2MpegWriteSector(void) {                    // 0xAA
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegGetLSI(void) {
   // "MPEG Get LSI"(0xAE), counterpart to Cs2MpegSetLSI() below: same
   // address encoding, value returned across CR3:CR4.
   u32 addr = ((u32)(Cs2Area->reg.CR1 & 0xFF) << 16) | Cs2Area->reg.CR2;
   u32 val = 0;

   switch (addr)
   {
      case 0x000000: val = Cs2Area->mpeglsi.status; break;
      case 0x000006: val = Cs2Area->mpeglsi.xpos; break;
      case 0x000008: val = Cs2Area->mpeglsi.ypos; break;
      case 0x000012: val = Cs2Area->mpeglsi.bordercolor; break;
      default: break; // register outside the documented set: reads back 0
   }

   Cs2Area->reg.CR1 = (Cs2Area->status << 8) | ((addr >> 16) & 0xFF);
   Cs2Area->reg.CR2 = (u16)addr;
   Cs2Area->reg.CR3 = (u16)(val >> 16);
   Cs2Area->reg.CR4 = (u16)val;
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2MpegSetLSI(void) {
   // "MPEG Set/Get LSI"(0xAF/0xAE) address the small SH-1-side register
   // bank documented on the Yabause wiki (MPEGCard page, "LSI" table):
   // 0xA100000 status (interpolation/display enable), 0xA100006 X
   // position, 0xA100008 Y position, 0xA100012 border color -- see
   // mpeglsi_struct in cs2.h. CR1/CR2 carry the low 24 bits of the
   // address (the same "(CR1&0xFF)<<16|CR2" convention Cs2GetMPEGRom()
   // already uses for its own offset field); CR3/CR4 carry the value.
   u32 addr = ((u32)(Cs2Area->reg.CR1 & 0xFF) << 16) | Cs2Area->reg.CR2;
   u32 val  = ((u32)Cs2Area->reg.CR3 << 16) | Cs2Area->reg.CR4;

   switch (addr)
   {
      case 0x000000: Cs2Area->mpeglsi.status = (u8)val; break;
      case 0x000006: Cs2Area->mpeglsi.xpos = (u16)val; break;
      case 0x000008: Cs2Area->mpeglsi.ypos = (u16)val; break;
      case 0x000012: Cs2Area->mpeglsi.bordercolor = val; break;
      default: break; // register outside the documented set, ignored
   }

   // No reply used to be written: the command registers came back as they
   // were sent, so the status byte read 0xAF (WAIT, PERI, ...). The Video CD
   // player sends AF03 0008 0000 0000 after each display update and kept
   // retrying it, about 1300 commands a second.
   doMPEGReport(Cs2Area->status);
   Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPCM);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2AuthenticateDevice(void) {
  int mpegauth;

  mpegauth = Cs2Area->reg.CR2 & 0xFF;


  if ((Cs2Area->status & 0xF) != CDB_STAT_NODISC &&
      (Cs2Area->status & 0xF) != CDB_STAT_OPEN)
  {
     // Set registers all to invalid values(aside from status)

     Cs2Area->reg.CR1 = (Cs2Area->status << 8) | 0xFF;
     Cs2Area->reg.CR2 = 0xFFFF;
     Cs2Area->reg.CR3 = 0xFFFF;
     Cs2Area->reg.CR4 = 0xFFFF;

     if (mpegauth == 1)
     {
        Cs2SetIRQ(CDB_HIRQ_MPED);
        // "MPEG Get Pts"(1) authentication only succeeds if a Video CD
        // Card is actually the cartridge plugged into CS0: per the
        // MPEGCard page on the Yabause wiki, this is how games probe for
        // the card before touching any of the 0x90-0xAF commands. Without
        // this check every game used to see an MPEG card whether or not
        // one was configured.
        Cs2Area->mpgauth = Cs2IsMpegCardPresent() ? 2 : 0;
     }
     else
     {
        // if authentication passes(obviously it always does), CDB_HIRQ_CSCT is set
        Cs2Area->isonesectorstored = 1;
        Cs2SetIRQ(CDB_HIRQ_EFLS | CDB_HIRQ_CSCT);
        // Was hardcoded to 4 (always "original Saturn disc"): a mounted
        // audio CD or Video CD would still be reported as a genuine
        // Saturn game. Cs2DetectDiscType() tells them apart using the
        // same "SEGA SEGASATURN" IP.BIN signature check Cs2GetIP() uses,
        // and the isaudio flag already tracked elsewhere in this file.
        Cs2Area->satauth = Cs2DetectDiscType();
     }

     // Set registers all back to normal values
     setStatus(CDB_STAT_PAUSE);
  }
  else
  {
     if (mpegauth == 1)
     {
        Cs2SetIRQ(CDB_HIRQ_MPED);
        Cs2Area->mpgauth = Cs2IsMpegCardPresent() ? 2 : 0;
     }
     else
       Cs2SetIRQ(CDB_HIRQ_EFLS | CDB_HIRQ_CSCT);
  }

  CS2_MPEG_TRACE("MPEG card: Authenticate Device (0xE0) type=%d -> %s=%d\n",
                 mpegauth, mpegauth == 1 ? "mpgauth" : "satauth",
                 mpegauth == 1 ? Cs2Area->mpgauth : Cs2Area->satauth);

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

void Cs2IsDeviceAuthenticated(void) {
  static int lastmpg = -1, lastsat = -1;

  Cs2Area->reg.CR1 = (Cs2Area->status << 8);
  if (Cs2Area->reg.CR2)
  {
     Cs2Area->reg.CR2 = Cs2Area->mpgauth;
     if (lastmpg != Cs2Area->mpgauth) // polled in a loop: only log changes
     {
        lastmpg = Cs2Area->mpgauth;
        CS2_MPEG_TRACE("MPEG card: Is Device Authenticated (0xE1) MPEG -> %d\n", Cs2Area->mpgauth);
     }
  }
  else
  {
     Cs2Area->reg.CR2 = Cs2Area->satauth;
     if (lastsat != Cs2Area->satauth)
     {
        lastsat = Cs2Area->satauth;
        CS2_MPEG_TRACE("MPEG card: Is Device Authenticated (0xE1) disc -> %d (2 = non-Saturn data disc, expected for a VCD)\n", Cs2Area->satauth);
     }
  }
  Cs2Area->reg.CR3 = 0;
  Cs2Area->reg.CR4 = 0;
  Cs2SetIRQ(CDB_HIRQ_CMOK);
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
// Which discs see the Video CD Card.
//
// The card is only shown to Video CDs and to the Saturn games listed in
// db.c (MpegCardDBList). Any other disc sees no card: its CD block reports
// no MPEG device, as on a console without one, so a game that probes for
// the card does not take an MPEG code path by accident.
//
// The disc is identified with plain reads through the CD interface into a
// local buffer, so this can be called at any time (from a command handler,
// the BIOS emulation...) without touching the CD block's partitions,
// filters or device connection. The result is kept until the disc changes
// (Cs2MpegDiscInvalidate()).
static int Cs2NameEqualsCI(const char *name, const char *ref);
static int Cs2MpegDiscChecked = 0;
static int Cs2MpegDiscAllowed = 0;

static void Cs2MpegDiscInvalidate(void)
{
  Cs2MpegDiscChecked = 0;
  Cs2MpegDiscAllowed = 0;
}

// Reads one sector straight from the CD interface into the caller's buffer
// and returns a pointer to its user data (Mode 1: offset 16, Mode 2 Form 1:
// offset 24), or NULL for an audio sector or a read error. No CD block state
// is touched: no partition, filter, block or CD device connection.
//
// buf MUST hold CS2_PEEK_SECTOR_SIZE (2448) bytes: ISOCDReadSectorFAD()
// starts with memset(buffer, 0, 2448), and the CHD reader copies
// track->sector_size bytes, 2448 for a track with subcode
// (cdbase.c; Cs2Area->workblock.data is 2448 bytes for the same reason).
#define CS2_PEEK_SECTOR_SIZE 2448
static const u8 *Cs2PeekSector(u32 fad, u8 *buf)
{
  static const u8 sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                               0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

  if (Cs2Area->cdi == NULL || !Cs2Area->cdi->ReadSectorFAD(fad, buf))
     return NULL;
  if (memcmp(buf, sync, 12) != 0)
     return NULL; // audio sector
  return (buf[15] == 2) ? buf + 24 : buf + 16;
}

// A Video CD is recognised by its root directory.
//
// White Book (Video CD 1.1 / 2.0): the root holds the directories VCD
// (INFO.VCD, ENTRIES.VCD -- INFO.VCD itself starts with the system
// identification "VIDEO_CD") and MPEGAV (the MPEG tracks); a Super Video CD
// has SVCD (INFO.SVD) and MPEG2 instead. The check only looked for a root
// entry named "VIDEO_CD" (the identifier INSIDE INFO.VCD, not a directory
// name) or "SVCD", so a standard Video CD was never recognised: the Video
// CD Card stayed hidden and the BIOS CD player answered "Disc requires
// system application". Accepted now: VCD, MPEGAV, SVCD, and VIDEO_CD kept
// for any non-standard disc that would really name a directory that way.
// Only directory entries count (flags bit 1, ECMA-119 9.1.6), so a file
// that happens to bear one of these names is not taken for a Video CD.
//
// Returns 1 for a Video CD, 0 for any other disc, -1 when a sector could
// not be read (no verdict: the caller must not cache it).
static int Cs2MpegDiscIsVideoCD(void)
{
  u8 buf[CS2_PEEK_SECTOR_SIZE];
  const u8 *d;
  u32 rootlba, rootsize, sectors, s;

  // Primary Volume Descriptor: ISO9660 sector 16 (FAD 166). An audio
  // sector there (audio CD) is a verdict, not a read failure.
  if (Cs2Area->cdi == NULL || !Cs2Area->cdi->ReadSectorFAD(166, buf))
     return -1;
  if ((d = Cs2PeekSector(166, buf)) == NULL)
     return 0;
  if (d[0] != 1 || memcmp(d + 1, "CD001", 5) != 0)
     return 0;

  // Root directory record at offset 156: extent (LBA) at +2, size at +10,
  // both-endian fields, little-endian half first.
  rootlba  = d[156 + 2] | (d[156 + 3] << 8) | (d[156 + 4] << 16) | ((u32)d[156 + 5] << 24);
  rootsize = d[156 + 10] | (d[156 + 11] << 8) | (d[156 + 12] << 16) | ((u32)d[156 + 13] << 24);
  sectors = (rootsize + 2047) / 2048;
  if (sectors == 0 || sectors > 16)
     sectors = (sectors == 0) ? 1 : 16;

  for (s = 0; s < sectors; s++)
  {
     u32 ofs = 0;

     if ((d = Cs2PeekSector(rootlba + 150 + s, buf)) == NULL)
        return -1;

     while (ofs < 2048 && d[ofs] != 0)
     {
        u32 reclen = d[ofs];
        u32 namelen = d[ofs + 32];
        char name[32];

        if (ofs + 33 + namelen > 2048)
           break;
        if (namelen > sizeof(name) - 1)
           namelen = sizeof(name) - 1;
        memcpy(name, d + ofs + 33, namelen);
        name[namelen] = '\0';

        if ((d[ofs + 25] & 0x02) &&
            (Cs2NameEqualsCI(name, "VCD") || Cs2NameEqualsCI(name, "MPEGAV") ||
             Cs2NameEqualsCI(name, "SVCD") || Cs2NameEqualsCI(name, "VIDEO_CD")))
           return 1;

        ofs += reclen;
     }
  }

  return 0;
}

// Product number of a Saturn disc (IP.BIN, FAD 150, offset 0x20), as
// Cs2GetIP() reads it.
static int Cs2MpegDiscGameCode(char *code, size_t size)
{
  u8 buf[CS2_PEEK_SECTOR_SIZE];
  const u8 *d;
  char tmp[11];

  if (size < 11)
     return 0;
  if ((d = Cs2PeekSector(150, buf)) == NULL)
     return 0;
  if (memcmp(d, "SEGA SEGASATURN", 15) != 0)
     return 0;

  memcpy(tmp, d + 0x20, 10);
  tmp[10] = '\0';
  if (sscanf(tmp, "%10s", code) != 1)
     return 0;
  return 1;
}

static int Cs2MpegDiscUsesCard(void)
{
  if (!Cs2MpegDiscChecked)
  {
     char code[16];

     if (Cs2Area == NULL || Cs2Area->cdi == NULL)
        return 0;
     // No disc yet: decide when one is there.
     if ((Cs2Area->status & 0xF) == CDB_STAT_NODISC || (Cs2Area->status & 0xF) == CDB_STAT_OPEN ||
         Cs2Area->cdi->GetStatus() > 1)
        return 0;

     {
        // A failed read is not a verdict: caching it would hide the card
        // until the next disc change. Ask again at the next call.
        int vcd = Cs2MpegDiscIsVideoCD();
        if (vcd < 0)
           return 0;
        Cs2MpegDiscAllowed = (vcd > 0) ||
                             (Cs2MpegDiscGameCode(code, sizeof(code)) && DBLookupMpegCard(code));
     }
     Cs2MpegDiscChecked = 1;
  }

  return Cs2MpegDiscAllowed;
}

int Cs2IsMpegCardPresent(void) {
  if (!MpegCardHasRom() &&
      !(CartridgeArea != NULL && CartridgeArea->carttype == CART_MPEGCARD))
     return 0;

  return Cs2MpegDiscUsesCard();
}

//////////////////////////////////////////////////////////////////////////////
// MPEG decoder input side.
//
// CD Communication Interface manual (ST-162, §5 "Stream Select", Fig. 5.1
// and the connector table p.46): the CD-ROM device outputs into a filter,
// filters store into buffer partitions, and the MPEG decoders (A)/(V) are
// *devices whose input is a partition output connector*. So on a real
// Saturn the decoder pulls the MPEG sectors out of the partitions named by
// MPEG Set Connection and they never stay in the CD buffer.
//
// The WIP never emptied those partitions: after 200 sectors (the whole CD
// buffer, ~2.7s of VCD at 1x) the buffer was full, the drive went to SEEK
// and stopped reading, so almost nothing ever reached the decoder.
//
// The payload itself is still handed to the decoder at read time from the
// raw sector (Cs2ReadFilteredSector(), full 2324-byte Form 2 user data,
// independent of the host's Get Sector Length setting); here the
// corresponding blocks are released, in FAD order, as long as the
// decoder's own input buffer has room. That gives the same flow control as
// the hardware: when the decoder is full, the partitions fill up and the
// drive pauses; when it drains, reading resumes.
//
// Only Form 2 sectors are consumed, so Form 1 data (INFO.VCD, PSD, ISO9660)
// the host may have routed to the same partition is never touched.

// How much not-yet-demuxed stream the decoder may hold before it stops
// pulling sectors (~2s of VCD at 1x). Size of the real card's buffer is
// not documented; this only needs to absorb CD read jitter.
#define CS2_MPEG_DECODER_BUFFER (384 * 1024)

static int Cs2MpegPartitionHead(u8 bufno, u8 conmode, u32 *fad)
{
  partition_struct *p;

  // conmode 0 means the application has switched this stream off (see
  // Cs2MpegChangeConnection); 0xFF means no partition is attached.
  if (bufno >= MAX_SELECTORS || conmode == 0)
     return 0;

  p = &Cs2Area->partition[bufno];
  if (p->numblocks == 0 || p->block[0] == NULL)
     return 0;

  if (!(p->block[0]->sm & 0x20)) // not Form 2: not MPEG, leave it to the host
     return 0;

  *fad = p->block[0]->FAD;
  return 1;
}

//////////////////////////////////////////////////////////////////////////////
// Runs the decoder for one emulated frame, honouring pause and slow playback
// (CDC_MpSetDec pautim). Called by Vdp2VBlankIN instead of MpegCardAdvance.
void Cs2MpegAdvance(double seconds)
{
   double fps;

   if (Cs2MpegPauTim == 1 || Cs2MpegPauTim == 0xFFFF)
   {
      MpegCardAdvance(seconds);
      return;
   }

   fps = MpegCardGetFrameRate();
   if (fps <= 0.0)
      fps = 25.0;

   if (Cs2MpegPauTim == 0)
   {
      /* paused: only the pictures requested by re-pause commands */
      if (Cs2MpegStep > 0)
      {
         Cs2MpegStep--;
         MpegCardAdvance(1.0 / fps);
      }
      return;
   }

   /* slow playback: one picture every pautim operation intervals */
   if ((++Cs2MpegSlowCount % Cs2MpegPauTim) == 0)
      MpegCardAdvance(1.0 / fps);
}

void Cs2MpegDecoderPump(void)
{
  u8 bufs[2];
  u32 drained = 0;

  if (Cs2Area == NULL || !Cs2MpegConnSet || !Cs2IsMpegCardPresent())
     return;

  bufs[0] = Cs2Area->mpegcon[0].vidbufnum;
  bufs[1] = Cs2Area->mpegcon[0].audbufnum;

  while (MpegCardGetBufferedBytes() < CS2_MPEG_DECODER_BUFFER)
  {
     u32 fad0 = 0, fad1 = 0;
     int has0 = Cs2MpegPartitionHead(bufs[0], Cs2Area->mpegcon[0].vidcon, &fad0);
     int has1 = (bufs[1] != bufs[0]) &&
                Cs2MpegPartitionHead(bufs[1], Cs2Area->mpegcon[0].audcon, &fad1);
     partition_struct *p;

     if (!has0 && !has1)
        break;

     // Oldest sector first, so audio and video leave in disc order.
     p = &Cs2Area->partition[(has0 && (!has1 || fad0 <= fad1)) ? bufs[0] : bufs[1]];

     p->size -= p->block[0]->size;
     Cs2FreeBlock(p->block[0]);
     p->block[0] = NULL;
     p->blocknum[0] = 0xFF;
     Cs2SortBlocks(p);
     p->numblocks--;
     drained++;
  }

  if (drained && Cs2Area->blockfreespace == MAX_BLOCKS)
     Cs2Area->isonesectorstored = 0;

  // A Video CD player has nothing to poll while a movie runs: it sleeps and
  // relies on the card's interrupt to wake up each picture, refresh its
  // display and read the pad. Kronos never raised one, so after MPEG Get
  // Stream the program went to sleep for good -- video and audio kept
  // running (this module decodes them independently of the SH2), but the
  // pad appeared dead. Signal one MPEG status interrupt per decoded picture.
  if (Cs2MpegPlaying)
  {
     static u32 lastserial = 0;
     u32 serial = MpegCardGetFrameSerial();

     if (serial != lastserial)
     {
        lastserial = serial;
        Cs2MpegIntPending |= 0x000001; // picture decoded
        Cs2SetIRQ(CDB_HIRQ_MPST);
     }
  }

  Cs2MpegUpdateStatus();
}

//////////////////////////////////////////////////////////////////////////////
// MPEG status data (returned by every 0x9x/0xAx command through
// doMPEGReport(), and polled continuously by the Video CD player):
//
//   CR1 = CD status (high byte), MPEG Play Status (low byte)
//   CR2 = V-counter
//   CR3 = Picture Info (high byte), MPEG Audio Status (low byte)
//   CR4 = MPEG Video Status (word)
//
// Play Status:  0x01 video stopped, 0x04 video transferring/playing,
//               0x10 audio stopped, 0x40 audio transferring/playing
//               (video in the low nibble, audio in the high nibble).
// Audio Status: 0x01 decoding, 0x10 buffer empty, 0x40 left output,
//               0x80 right output.
// Video Status: 0x0001 decoding, 0x0002 display, 0x0040 update picture,
//               0x0100 output preparation completion, 0x0800 first
//               picture display, 0x1000 video buffer empty.
// Source: Yabause wiki, MPEGStatusData (the Sega "MPEG part" of the CD
// Communication Interface manual is not publicly available).
//
// These four fields were never assigned anywhere: the report always said
// "stopped, nothing decoded, no output", so the player had no reason to
// ever move past its start-up polling loop.

void Cs2MpegUpdateStatus(void)
{
   u32 buffered;

   if (Cs2Area == NULL || !Cs2IsMpegCardPresent())
      return;

   if (!Cs2MpegPlaying)
   {
      Cs2Area->actionstatus = 0x01 | 0x10; // video stopped, audio stopped
      Cs2Area->mpegvideostatus = 0;
      Cs2Area->mpegaudiostatus = 0;
      Cs2Area->pictureinfo = 0;
      return;
   }

   buffered = MpegCardGetBufferedBytes();

   {
      int w = 0, h = 0;
      if (MpegCardGetFrameRGBA(&w, &h) != NULL && w > 0 && h > 0)
      {
         if (Cs2Area->mpegpicturewidth != (u32)w || Cs2Area->mpegpictureheight != (u32)h)
         {
            Cs2Area->mpegpicturewidth = (u32)w;
            Cs2Area->mpegpictureheight = (u32)h;
            Cs2MpegIntPending |= 0x000008;   // picture size available
            Cs2SetIRQ(CDB_HIRQ_MPST);
         }
      }
   }

   // Video and audio are both "transferring/playing" while MPEG Play is in
   // effect; this module decodes the two together from one program stream.
   Cs2Area->actionstatus = 0x04 | 0x40;

   Cs2Area->mpegvideostatus = 0x0001 | 0x0002 | 0x0100; // decoding, display, output ready
   if (Cs2MpegPauTim == 0)
      Cs2Area->mpegvideostatus |= 0x0004;               // pause (MPEG part 7.2.2, stat_v bit 2)
   if (Cs2MpegFrzTim == 0)
      Cs2Area->mpegvideostatus |= 0x0008;               // freeze (stat_v bit 3)
   if (MpegCardIsActive())
      Cs2Area->mpegvideostatus |= 0x0040 | 0x0800;      // picture updated, first picture shown
   if (buffered == 0)
      Cs2Area->mpegvideostatus |= 0x1000;               // starved: video buffer empty

   Cs2Area->mpegaudiostatus = 0x01 | 0x40 | 0x80;       // decoding, left and right output
   if (buffered == 0)
      Cs2Area->mpegaudiostatus |= 0x10;                 // audio buffer empty
}

//////////////////////////////////////////////////////////////////////////////

void Cs2GetMPEGRom(void) {
  u32 i;
  u32 readoffset;
  u32 readsize;
  partition_struct * mpgpartition;

  if (!Cs2IsMpegCardPresent())
  {
     // No Video CD Card installed: there is no ROM to read. Mirror the
     // "invalid registers" pattern Cs2AuthenticateDevice uses above for
     // an absent/not-ready device instead of pretending success.
     Cs2Area->reg.CR1 = (Cs2Area->status << 8) | 0xFF;
     Cs2Area->reg.CR2 = 0xFFFF;
     Cs2Area->reg.CR3 = 0xFFFF;
     Cs2Area->reg.CR4 = 0xFFFF;
     Cs2SetIRQ(CDB_HIRQ_CMOK);
     return;
  }

  // The ROM is normally loaded once by Cs2Init(); retry here in case the
  // path only became valid afterwards (file copied/extracted meanwhile).
  if (!MpegCardHasRom() && Cs2Area->mpegpath != NULL && Cs2Area->mpegpath[0] != '\0')
     MpegCardLoadRom(Cs2Area->mpegpath);

  // fix me
  Cs2Area->mpgauth |= 0x300;

  Cs2Area->outconmpegrom = Cs2Area->filter + 0;
  Cs2Area->outconmpegromnum = 0;

  // Sector offset: 24-bit value split across CR1 low byte / CR2, the same
  // layout every other 24-bit CD Block parameter (FAD etc.) uses. It used
  // to be shifted by 8 instead of 16, which only mattered for offsets
  // >= 64K sectors, i.e. never for a 512KB ROM.
  readoffset = ((u32)(Cs2Area->reg.CR1 & 0xFF) << 16) | Cs2Area->reg.CR2;
  readsize = Cs2Area->reg.CR4;

  CS2_MPEG_TRACE("MPEG card: Get MPEG ROM (0xE2) CR1=%04X CR2=%04X CR3=%04X CR4=%04X -> offset=%u sectors, count=%u, sectsize=%u, rom=%u bytes\n",
        Cs2Area->reg.CR1, Cs2Area->reg.CR2, Cs2Area->reg.CR3, Cs2Area->reg.CR4,
        readoffset, readsize, Cs2Area->getsectsize, MpegCardGetRomSize());

  if (MpegCardHasRom())
  {
     if ((mpgpartition = Cs2GetPartition(Cs2Area->outconmpegrom)) != NULL && !Cs2Area->isbufferfull)
     {
        for (i = 0; i < readsize && mpgpartition->numblocks < MAX_BLOCKS; i++)
        {
           mpgpartition->block[mpgpartition->numblocks] = Cs2AllocateBlock(&mpgpartition->blocknum[mpgpartition->numblocks], Cs2Area->getsectsize);

           if (mpgpartition->block[mpgpartition->numblocks] == NULL)
              break; // global block pool exhausted, matches Cs2CopySectorData's convention

           // Served from the in-memory image (loaded from a raw dump or
           // extracted from a .zip by MpegCardLoadRom(), already remapped to
           // the Saturn-side layout); reads past the end mirror the chip.
           MpegCardReadRom((readoffset + i) * Cs2Area->getsectsize,
                           mpgpartition->block[mpgpartition->numblocks]->data,
                           Cs2Area->getsectsize);

           mpgpartition->numblocks++;
           mpgpartition->size += Cs2Area->getsectsize;
        }

        Cs2Area->isonesectorstored = 1;
        Cs2SetIRQ(CDB_HIRQ_CSCT);
     }
  }

  doCDReport(Cs2Area->status);
  Cs2SetIRQ(CDB_HIRQ_CMOK | CDB_HIRQ_MPED);
}

//////////////////////////////////////////////////////////////////////////////
// Small, portable (no strcasecmp/_stricmp dependency) case-insensitive
// ISO9660 identifier comparison, used by Cs2DetectVideoCD() below.
static int Cs2NameEqualsCI(const char *name, const char *ref)
{
   while (*name && *ref)
   {
      char a = *name, b = *ref;
      if (a >= 'a' && a <= 'z') a -= 32;
      if (b >= 'a' && b <= 'z') b -= 32;
      if (a != b) return 0;
      name++; ref++;
   }
   return *name == '\0' && *ref == '\0';
}

//////////////////////////////////////////////////////////////////////////////
// Tells apart what "Authenticate Device"(0xE0)/satauth is supposed to
// report: 1=audio CD, 2=any other non-Saturn data disc (Video CD, Photo
// CD, plain CD-ROM...), 4=genuine Saturn disc. See the Yabause wiki,
// MPEGCard page: "If a disc is detected to be an audio disc or a
// non-Saturn game disc(such as a video cd), the CD Block instantly
// approves" -- i.e. only a real Saturn header should ever fail here.
//
// 3="pirated Saturn disc" is deliberately never returned: that value
// reflects the physical "ring" copy-protection groove Sega pressed into
// genuine discs, which has no equivalent in a disc image, so no emulator
// can detect it from image data alone.
//
// The authentication is the CD block's own business: it does not go
// through the host's CD device connection, filters or buffer partitions.
// This used to read IP.BIN with Cs2ReadUnFilteredSector(), i.e. through
// Cs2Area->outconcddev. After a game disconnects the CD device (Set CD
// Device Connection, filter FFh: outconcddev = NULL) and the BIOS then
// authenticates the disc again (Skeleton Warriors, after its intro movie),
// Cs2GetPartition(NULL) dereferenced a NULL pointer and Kronos crashed.
// It could also fail (satauth 0) whenever the buffer was full, and it took
// a block from whatever partition the game had connected.
//
// The disc type also came from Cs2Area->isaudio, which only describes the
// LAST SECTOR READ: after a game played a CD-DA track, a Saturn disc was
// reported as an audio CD. The first track's control field (TOC) is used
// instead: an audio CD starts with an audio track.
int Cs2DetectDiscType(void)
{
   u8 buf[CS2_PEEK_SECTOR_SIZE];
   u32 toc[102];
   const u8 *d;

   if (Cs2Area->cdi == NULL ||
       (Cs2Area->status & 0xF) == CDB_STAT_NODISC || (Cs2Area->status & 0xF) == CDB_STAT_OPEN)
      return 0;

   // TOC entry of track 1: control/ADR in the top byte, bit 6 (0x40) set
   // for a data track (same test as Cs2FADIsAudio()). Read into a local
   // copy: Cs2Area->TOC is only filled by Get TOC / tray close, and Get TOC
   // is not required before Authenticate Device.
   memset(toc, 0xFF, sizeof(toc));
   Cs2Area->cdi->ReadTOC(toc);
   if (toc[0] != 0xFFFFFFFF && ((toc[0] >> 24) & 0x40) == 0)
      return 1;

   if ((d = Cs2PeekSector(150, buf)) == NULL) // LBA 0 / IP.BIN
      return 0;

   // Same check Cs2GetIP() uses to accept/reject the IP.BIN it just read.
   if (memcmp(d, "SEGA SEGASATURN", 15) == 0)
      return 4;

   return 2;
}

//////////////////////////////////////////////////////////////////////////////
// Looks for the mandatory VCD / MPEGAV (or SVCD, for the Super Video CD
// variant) root directories the Philips "Video CD Specification" ("White
// Book") requires on every compliant Video CD.
//
// Same scan as Cs2MpegDiscIsVideoCD(): plain reads through the CD
// interface into a local buffer. The previous version read through
// Cs2ReadUnFilteredSector(), i.e. through the host's CD device connection
// and buffer partitions, with the same NULL dereference as
// Cs2DetectDiscType() when the CD device was disconnected (it is called
// from Get TOC after a disc change), and it never touches
// curdirsect/fileinfo/numfiles either.
int Cs2DetectVideoCD(void)
{
   Cs2Area->isvideocd = 0;

   if (Cs2Area->cdi == NULL ||
       (Cs2Area->status & 0xF) == CDB_STAT_NODISC || (Cs2Area->status & 0xF) == CDB_STAT_OPEN)
      return 0;

   Cs2Area->isvideocd = (Cs2MpegDiscIsVideoCD() > 0);
   return Cs2Area->isvideocd;
}

//////////////////////////////////////////////////////////////////////////////

u8 Cs2FADToTrack(u32 val) {
  int i;
  for (i = 0; i < 99; i++)
  {
     if (Cs2Area->TOC[i] == 0xFFFFFFFF) return 0xFF;

     if (val >= (Cs2Area->TOC[i] & 0xFFFFFF) && val < (Cs2Area->TOC[i + 1] & 0xFFFFFF))
        return (i + 1);
  }

  return 0;
}

//////////////////////////////////////////////////////////////////////////////

u32 Cs2TrackToFAD(u16 trackandindex) {
  if (trackandindex == 0xFFFF)
     // leadout position
     return (Cs2Area->TOC[101] & 0x00FFFFFF);
  if (trackandindex != 0x0000)
  {
     // regular track
     // (really, we should be fetching subcode q's here)
     if ((trackandindex & 0xFF) == 0x01)
        // Return Start of Track
        // Cs2TrackToFAD isn't static, so callers outside this file
        // aren't guaranteed to pre-mask the low byte the way
        // Cs2PlayDisc does; guard track==0 the same way
        // Cs2SetupDefaultPlayStats does, to avoid TOC[-1].
        return (((trackandindex >> 8) == 0) ? 0 : (Cs2Area->TOC[(trackandindex >> 8) - 1] & 0x00FFFFFF));
     else if ((trackandindex & 0xFF) == 0x63)
        // Return End of Track
        return ((Cs2Area->TOC[(trackandindex >> 8)] & 0x00FFFFFF) - 1);
  }

  // assume it's leadin
  return 0;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2FADToMSF(u32 val, u8 *m, u8 *s, u8 *f)
{
   u32 temp;
   m[0] = val / 4500;
   temp = val % 4500;
   s[0] = temp / 75;
   f[0] = temp % 75;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetupDefaultPlayStats(u8 track_number, int writeFAD) {
  // 0xFF is the documented "no track" sentinel, but track_number also
  // reaches here as 0 (e.g. Cs2FADToTrack() returns 0 when a FAD isn't
  // within any track, such as the lead-in area) and straight from a
  // game-supplied register field (CR2>>8) with no prior validation.
  // track_number - 1 must stay a valid TOC[] index (tracks occupy
  // TOC[0..98]), so 0 needs rejecting the same as 0xFF -- as written,
  // track_number==0 wrapped to TOC[-1], an out-of-bounds read just
  // before the array.
  if (track_number != 0xFF && track_number != 0)
  {
     Cs2Area->options = 8;
     Cs2Area->repcnt = 0;
     Cs2Area->ctrladdr = (u8)(Cs2Area->TOC[track_number - 1] >> 24);
     Cs2Area->index = 1;
     Cs2Area->track = track_number;
     if (writeFAD)
        Cs2Area->FAD = Cs2Area->TOC[track_number - 1] & 0x00FFFFFF;
  }
}

//////////////////////////////////////////////////////////////////////////////

block_struct * Cs2AllocateBlock(u8 * blocknum, s32 sectsize) {
  u32 i;
  // find a free block
  for(i = 0; i < MAX_BLOCKS; i++)
  {
     if (Cs2Area->block[i].size == -1)
     {
        Cs2Area->blockfreespace--;

		if (Cs2Area->blockfreespace <= 0) {
			Cs2Area->isbufferfull = 1;
      Cs2SetIRQ(CDB_HIRQ_BFUL);
		}

        Cs2Area->block[i].size = sectsize;

        *blocknum = (u8)i;
        return (Cs2Area->block + i);
     }
  }

  Cs2Area->isbufferfull = 1;
  Cs2SetIRQ(CDB_HIRQ_BFUL);
  return NULL;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2FreeBlock(block_struct * blk) {
  if (blk == NULL) return;
  blk->size = -1;
  CDLOG("Free Block\n");
  Cs2Area->blockfreespace++;
  Cs2Area->isbufferfull = 0;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SortBlocks(partition_struct * part) {
  unsigned int from, to;

  for (from = to = 0; from < MAX_BLOCKS; from++)
  {
     if (part->block[from] != NULL)
     {
        if (to != from)
        {
           part->block[to] = part->block[from];
        }
        to++;
     }
  }

  for (; to < MAX_BLOCKS; to++) {
      part->block[to] = NULL;
  }
}

//////////////////////////////////////////////////////////////////////////////

partition_struct * Cs2GetPartition(filter_struct * curfilter)
{
  // go through various filter conditions here(fix me)

  // No filter: the CD device is not connected (Set CD Device Connection
  // with filter FFh, ST-162 7.4). Nothing can be stored, and there is no
  // filter to read condtrue from.
  if (curfilter == NULL)
     return NULL;

  // condtrue is an 8-bit value written verbatim from a command register
  // (Cs2SetFilterConnection); guard against it pointing past the 24
  // physical partitions before using it as an array index.
  if (curfilter->condtrue >= MAX_SELECTORS)
     return NULL;

  return &Cs2Area->partition[curfilter->condtrue];
}

//////////////////////////////////////////////////////////////////////////////

partition_struct * Cs2FilterData(filter_struct * curfilter, int isaudio)
{
  int condresults;
  partition_struct * fltpartition = NULL;

  for (;;)
  {
     // reset result
     condresults = 1;
     // detect which type of sector we're dealing with
     // If it's not mode 2, ignore the subheader conditions
     if (Cs2Area->workblock.data[0xF] == 0x02 && !isaudio)
     {
        // Mode 2
        // go through various subheader filter conditions

        if (curfilter->mode & 0x01)
        {
           // File Number Check
           if (Cs2Area->workblock.fn != curfilter->fid)
              condresults = 0;
        }

        if (curfilter->mode & 0x02)
        {
           // Channel Number Check
           if (Cs2Area->workblock.cn != curfilter->chan)
              condresults = 0;
        }

        if (curfilter->mode & 0x04)
        {
           // Sub Mode Check
           if ((Cs2Area->workblock.sm & curfilter->smmask) != curfilter->smval)
              condresults = 0;
        }

        if (curfilter->mode & 0x08)
        {
           // Coding Information Check
           CDLOG("cs2\t: FilterData: Coding Information Check. Coding Information = %02X. Filter's Coding Information Mask = %02X, Coding Information Value = %02X\n", Cs2Area->workblock.ci, curfilter->cimask, curfilter->cival);
           if ((Cs2Area->workblock.ci & curfilter->cimask) != curfilter->cival)
              condresults = 0;
        }

        if (curfilter->mode & 0x10)
        {
           // Reverse Subheader Conditions
           CDLOG("cs2\t: FilterData: Reverse Subheader Conditions\n");
           condresults ^= 1;
        }
     }

     if (curfilter->mode & 0x40)
     {
        // FAD Range Check
        if (Cs2Area->workblock.FAD < curfilter->FAD ||
            Cs2Area->workblock.FAD >= (curfilter->FAD+curfilter->range))
            condresults = 0;
     }

     if (condresults == 1)
     {
        Cs2Area->lastbuffer = curfilter->condtrue;
        // condtrue is written verbatim from a command register
        // (Cs2SetFilterConnection); guard against an out-of-range value
        // before indexing partition[] (24 entries).
        if (curfilter->condtrue >= MAX_SELECTORS)
           return NULL;
        fltpartition = &Cs2Area->partition[curfilter->condtrue];
        break;
     }
     else
     {
        // Why was it rejected? Report each condition and the raw sector
        // header, so a wrong subheader offset in the disc image can be told
        // apart from a filter condition the player set that we mishandle.

        Cs2Area->lastbuffer = curfilter->condfalse;

        // 0xFF means "not connected" (sector discarded); any other
        // value >= MAX_SELECTORS is out of range for filter[] (24
        // entries) and must be rejected the same way rather than
        // indexed.
        if (curfilter->condfalse >= MAX_SELECTORS)
           return NULL;
        // loop and try filter that was connected to the false connector
        curfilter = &Cs2Area->filter[curfilter->condfalse];
     }
  }

  // Allocate block
  fltpartition->block[fltpartition->numblocks] = Cs2AllocateBlock(&fltpartition->blocknum[fltpartition->numblocks], Cs2Area->getsectsize);

  if (fltpartition->block[fltpartition->numblocks] == NULL)
    return NULL;

  // Copy workblock settings to allocated block
  fltpartition->block[fltpartition->numblocks]->size = Cs2Area->workblock.size;
  fltpartition->block[fltpartition->numblocks]->FAD = Cs2Area->workblock.FAD;
  fltpartition->block[fltpartition->numblocks]->cn = Cs2Area->workblock.cn;
  fltpartition->block[fltpartition->numblocks]->fn = Cs2Area->workblock.fn;
  fltpartition->block[fltpartition->numblocks]->sm = Cs2Area->workblock.sm;
  fltpartition->block[fltpartition->numblocks]->ci = Cs2Area->workblock.ci;

  // convert raw sector to type specified in getsectsize
  switch(Cs2Area->workblock.size)
  {
	case 2048:
		if (Cs2Area->workblock.data[0xF] == 0x02)
		{
			if (!(Cs2Area->workblock.data[0x12] & 0x20))
			{
				// Mode 2 Form 1 — 2048 bytes user data at offset +24
				memcpy(fltpartition->block[fltpartition->numblocks]->data,
					   Cs2Area->workblock.data + 24, 2048);
			}
			else
			{
				// Mode 2 Form 2 — truncate to 2048 bytes (pas 2324)
				memcpy(fltpartition->block[fltpartition->numblocks]->data,
					   Cs2Area->workblock.data + 24, 2048);
			}
		}
		else
		{
			// Mode 1
			memcpy(fltpartition->block[fltpartition->numblocks]->data,
				   Cs2Area->workblock.data + 16, 2048);
		}
		break;
     case 2324: // m2f2 user data only
                memcpy(fltpartition->block[fltpartition->numblocks]->data,
                       Cs2Area->workblock.data + 24, Cs2Area->workblock.size);
                break;
     case 2336: // m2f2 skip sync+header data
                memcpy(fltpartition->block[fltpartition->numblocks]->data,
                Cs2Area->workblock.data + 16, Cs2Area->workblock.size);
                break;
     case 2340: // m2f2 skip sync data
                memcpy(fltpartition->block[fltpartition->numblocks]->data,
                Cs2Area->workblock.data + 12, Cs2Area->workblock.size);
                break;
     case 2352: // Copy data as is
                memcpy(fltpartition->block[fltpartition->numblocks]->data,
                       Cs2Area->workblock.data, Cs2Area->workblock.size);
                break;
     default: break;
  }

  // Modify Partition values
  if (fltpartition->size == -1) fltpartition->size = 0;
  fltpartition->size += fltpartition->block[fltpartition->numblocks]->size;
  fltpartition->numblocks++;

  return fltpartition;
}

//////////////////////////////////////////////////////////////////////////////

int Cs2CopyDirRecord(u8 * buffer, dirrec_struct * dirrec)
{
  u8 * temp_pointer;

  temp_pointer = buffer;

  memcpy(&dirrec->recordsize, buffer, sizeof(dirrec->recordsize));
  buffer += sizeof(dirrec->recordsize);

  memcpy(&dirrec->xarecordsize, buffer, sizeof(dirrec->xarecordsize));
  buffer += sizeof(dirrec->xarecordsize);

#ifdef WORDS_BIGENDIAN
  buffer += sizeof(dirrec->lba);
  memcpy(&dirrec->lba, buffer, sizeof(dirrec->lba));
  buffer += sizeof(dirrec->lba);
#else
  memcpy(&dirrec->lba, buffer, sizeof(dirrec->lba));
  buffer += (sizeof(dirrec->lba) * 2);
#endif

#ifdef WORDS_BIGENDIAN
  buffer += sizeof(dirrec->size);
  memcpy(&dirrec->size, buffer, sizeof(dirrec->size));
  buffer += sizeof(dirrec->size);
#else
  memcpy(&dirrec->size, buffer, sizeof(dirrec->size));
  buffer += (sizeof(dirrec->size) * 2);
#endif

  dirrec->dateyear = buffer[0];
  dirrec->datemonth = buffer[1];
  dirrec->dateday = buffer[2];
  dirrec->datehour = buffer[3];
  dirrec->dateminute = buffer[4];
  dirrec->datesecond = buffer[5];
  dirrec->gmtoffset = buffer[6];
  buffer += 7;

  dirrec->flags = buffer[0];
  buffer += sizeof(dirrec->flags);

  dirrec->fileunitsize = buffer[0];
  buffer += sizeof(dirrec->fileunitsize);

  dirrec->interleavegapsize = buffer[0];
  buffer += sizeof(dirrec->interleavegapsize);

#ifdef WORDS_BIGENDIAN
  buffer += sizeof(dirrec->volumesequencenumber);
  memcpy(&dirrec->volumesequencenumber, buffer, sizeof(dirrec->volumesequencenumber));
  buffer += sizeof(dirrec->volumesequencenumber);
#else
  memcpy(&dirrec->volumesequencenumber, buffer, sizeof(dirrec->volumesequencenumber));
  buffer += (sizeof(dirrec->volumesequencenumber) * 2);
#endif

  dirrec->namelength = buffer[0];
  buffer += sizeof(dirrec->namelength);

  memset(dirrec->name, 0, sizeof(dirrec->name));
  memcpy(dirrec->name, buffer, dirrec->namelength);
  buffer += dirrec->namelength;

  // handle padding
  // ECMA-119 / ISO 9660 sec 9.1.12 "Padding Field": present ONLY when the
  // Length of File Identifier (LEN_FI) is EVEN, and then exactly 1 byte.
  // The used part of the record is 33 + LEN_FI + padding, and must stay of
  // even length -- hence (1 - LEN_FI % 2), not (LEN_FI % 2).
  //
  // Getting this wrong shifts the cursor by one byte and breaks the XA
  // record detection just below: recordsize - (buffer - temp_pointer)
  // then yields 13 or 15 instead of 14, so xarecord (groupid, userid,
  // attributes, "XA" signature, filenumber) is never parsed and stays
  // zeroed. Games lose the CD-XA file number used to filter Mode 2 Form 2
  // streams -- i.e. no FMV (cf. Deep Fear with the real BIOS).
  buffer += (1 - dirrec->namelength % 2);

  memset(&dirrec->xarecord, 0, sizeof(dirrec->xarecord));

  // sadily, this is the best way I can think of for detecting XA records

  if ((dirrec->recordsize - (buffer - temp_pointer)) == 14)
  {
     memcpy(&dirrec->xarecord.groupid, buffer, sizeof(dirrec->xarecord.groupid));
     buffer += sizeof(dirrec->xarecord.groupid);

     memcpy(&dirrec->xarecord.userid, buffer, sizeof(dirrec->xarecord.userid));
     buffer += sizeof(dirrec->xarecord.userid);

     memcpy(&dirrec->xarecord.attributes, buffer, sizeof(dirrec->xarecord.attributes));
     buffer += sizeof(dirrec->xarecord.attributes);

#ifndef WORDS_BIGENDIAN
     // byte swap it
     dirrec->xarecord.attributes = ((dirrec->xarecord.attributes & 0xFF00) >> 8) +
                                   ((dirrec->xarecord.attributes & 0x00FF) << 8);
#endif

     memcpy(&dirrec->xarecord.signature, buffer, sizeof(dirrec->xarecord.signature));
     buffer += sizeof(dirrec->xarecord.signature);

     memcpy(&dirrec->xarecord.filenumber, buffer, sizeof(dirrec->xarecord.filenumber));
     buffer += sizeof(dirrec->xarecord.filenumber);

     memcpy(dirrec->xarecord.reserved, buffer, sizeof(dirrec->xarecord.reserved));
     buffer += sizeof(dirrec->xarecord.reserved);
  }

  return 0;
}

//////////////////////////////////////////////////////////////////////////////

int Cs2ReadFileSystem(filter_struct * curfilter, u32 fid, int isoffset)
{
   u8 * workbuffer;
   u32 i;
   dirrec_struct dirrec;
   u8 numsectorsleft = 0;
   u32 curdirlba = 0;
   partition_struct * rfspartition;
   u32 blocksectsize = Cs2Area->getsectsize;

   Cs2Area->outconcddev = curfilter;

   if (isoffset)
   {
      // readDirectory operation

      // make sure we have a valid current directory
      if (Cs2Area->curdirsect == 0)
         return -1;

      /* Le nombre d'enregistrements que la boucle de saut plus bas ignore
       * reellement vaut max(0, fid - 2) : elle s'ecrit "for (i = 2; i < fid;
       * i++)" et ne tourne donc pas quand fid vaut 0 ou 1. Memoriser fid - 2
       * dans un u32 dans ce cas produit un debordement (0xFFFFFFFE pour
       * fid = 0), et le Change Directory suivant indexe alors
       * fileinfo[fid + 2] au lieu de fileinfo[fid].
       *
       * L'offset memorise doit valoir exactement le nombre d'enregistrements
       * sautes, sans quoi la correspondance entre identificateur de fichier
       * et entree de fileinfo[] est decalee.
       * Ref : ST-040-R4-051795 §6.10 "Read Directory (command 0x71)". */
      Cs2Area->curdirfidoffset = (fid > 2) ? (fid - 2) : 0;
      curdirlba = Cs2Area->curdirsect;
      numsectorsleft = (u8)Cs2Area->curdirsize;
   }
   else
   {
      // changeDirectory operation

      if (fid == 0xFFFFFF)
      {
         // Figure out root directory's location

         // Read sector 16
         if ((rfspartition = Cs2ReadUnFilteredSector(166)) == NULL)
            return -2;

         blocksectsize = rfspartition->block[rfspartition->numblocks - 1]->size;

         // Retrieve directory record's lba
         Cs2CopyDirRecord(rfspartition->block[rfspartition->numblocks - 1]->data + 0x9C, &dirrec);

         // Free Block
         rfspartition->size -= rfspartition->block[rfspartition->numblocks - 1]->size;
         Cs2FreeBlock(rfspartition->block[rfspartition->numblocks - 1]);
         rfspartition->block[rfspartition->numblocks - 1] = NULL;
         rfspartition->blocknum[rfspartition->numblocks - 1] = 0xFF;

         // Sort remaining blocks
         Cs2SortBlocks(rfspartition);
         rfspartition->numblocks -= 1;

         curdirlba = Cs2Area->curdirsect = dirrec.lba;
         Cs2Area->curdirsize = (dirrec.size / blocksectsize) - 1;
         numsectorsleft = (u8)Cs2Area->curdirsize;
         Cs2Area->curdirfidoffset = 0;
      }
      else
      {
         // Read in new directory record of specified directory

         // make sure we have a valid current directory
         if (Cs2Area->curdirsect == 0)
            return -1;

         /* Index borne comme partout ailleurs dans ce fichier : fid vient
          * directement d'un registre du bloc CD et fileinfo[] ne compte que
          * MAX_FILES entrees. */
         {
            u32 cdfid = fid - Cs2Area->curdirfidoffset;

            if (cdfid >= MAX_FILES)
               return -1;

            curdirlba = Cs2Area->curdirsect = Cs2Area->fileinfo[cdfid].lba - 150;
            Cs2Area->curdirsize = (Cs2Area->fileinfo[cdfid].size / blocksectsize) - 1;
         }
         numsectorsleft = (u8)Cs2Area->curdirsize;
         Cs2Area->curdirfidoffset = 0;
      }
   }

   // Make sure any old records are cleared
   memset(Cs2Area->fileinfo, 0, sizeof(dirrec_struct) * MAX_FILES);

   // now read in first sector of directory record
   if ((rfspartition = Cs2ReadUnFilteredSector(curdirlba+150)) == NULL)
      return -2;

   curdirlba++;
   workbuffer = rfspartition->block[rfspartition->numblocks - 1]->data;

   // Fill in first two entries of fileinfo
   for (i = 0; i < 2; i++)
   {
      Cs2CopyDirRecord(workbuffer, Cs2Area->fileinfo + i);
      Cs2Area->fileinfo[i].lba += 150;
      workbuffer += Cs2Area->fileinfo[i].recordsize;

      if (workbuffer[0] == 0)
      {
         Cs2Area->numfiles = i;
         break;
      }
   }

   // If doing a ReadDirectory operation, parse sector entries until we've
   // found the fid that matches fid
   if (isoffset)
   {
      for (i = 2; i < fid; i++)
      {
         Cs2CopyDirRecord(workbuffer, Cs2Area->fileinfo + 2);
         workbuffer += Cs2Area->fileinfo[2].recordsize;

         if (workbuffer[0] == 0)
         {
            if (numsectorsleft > 0)
            {
               // Free previous read sector
               rfspartition->size -= rfspartition->block[rfspartition->numblocks - 1]->size;
               Cs2FreeBlock(rfspartition->block[rfspartition->numblocks - 1]);
               rfspartition->block[rfspartition->numblocks - 1] = NULL;
               rfspartition->blocknum[rfspartition->numblocks - 1] = 0xFF;

               // Sort remaining blocks
               Cs2SortBlocks(rfspartition);
               rfspartition->numblocks -= 1;

               // Read in next sector of directory record
               if ((rfspartition = Cs2ReadUnFilteredSector(curdirlba+150)) == NULL)
                  return -2;

               curdirlba++;

               numsectorsleft--;
               workbuffer = rfspartition->block[rfspartition->numblocks - 1]->data;
            }
            else
            {
               break;
            }
         }
      }
   }

   // Now generate the last 254 entries(the first two should've already been
   // generated earlier)
   for (i = 2; i < MAX_FILES; i++)
   {
      Cs2CopyDirRecord(workbuffer, Cs2Area->fileinfo + i);
      Cs2Area->fileinfo[i].lba += 150;
      workbuffer += Cs2Area->fileinfo[i].recordsize;

      if (workbuffer[0] == 0)
      {
         if (numsectorsleft > 0)
         {
            // Free previous read sector
            rfspartition->size -= rfspartition->block[rfspartition->numblocks - 1]->size;
            Cs2FreeBlock(rfspartition->block[rfspartition->numblocks - 1]);
            rfspartition->block[rfspartition->numblocks - 1] = NULL;
            rfspartition->blocknum[rfspartition->numblocks - 1] = 0xFF;

            // Sort remaining blocks
            Cs2SortBlocks(rfspartition);
            rfspartition->numblocks -= 1;

            // Read in next sector of directory record
            if ((rfspartition = Cs2ReadUnFilteredSector(curdirlba+150)) == NULL)
               return -2;

            curdirlba++;
            numsectorsleft--;
            workbuffer = rfspartition->block[rfspartition->numblocks - 1]->data;
         }
         else
         {
            Cs2Area->numfiles = i;
            break;
         }
      }
   }

   // Free the remaining sector
   rfspartition->size -= rfspartition->block[rfspartition->numblocks - 1]->size;
   Cs2FreeBlock(rfspartition->block[rfspartition->numblocks - 1]);
   rfspartition->block[rfspartition->numblocks - 1] = NULL;
   rfspartition->blocknum[rfspartition->numblocks - 1] = 0xFF;

   // Sort remaining blocks
   Cs2SortBlocks(rfspartition);
   rfspartition->numblocks -= 1;

//#if CDDEBUG
//  for (i = 0; i < MAX_FILES; i++)
//  {
//     CDLOG("fileinfo[%d].name = %s\n", i, Cs2Area->fileinfo[i].name);
//  }
//#endif

  return 0;
}

//////////////////////////////////////////////////////////////////////////////

void Cs2SetupFileInfoTransfer(u32 fid) {
  // fid is caller-supplied straight from a CD Block register (see
  // Cs2GetFileInfo / Cs2ReadFile) with no upstream bound check, unlike
  // every other selector/buffer index elsewhere in this file
  // (rdfilternum, dsdbufno, casbufno, ... all checked against
  // MAX_SELECTORS). fileinfo[] only has MAX_FILES entries, so an
  // out-of-range fid was an out-of-bounds read.
  if (fid >= MAX_FILES)
     return;

  Cs2Area->transfileinfo[0] = (u8)(Cs2Area->fileinfo[fid].lba >> 24);
  Cs2Area->transfileinfo[1] = (u8)(Cs2Area->fileinfo[fid].lba >> 16);
  Cs2Area->transfileinfo[2] = (u8)(Cs2Area->fileinfo[fid].lba >> 8);
  Cs2Area->transfileinfo[3] = (u8)Cs2Area->fileinfo[fid].lba;

  Cs2Area->transfileinfo[4] = (u8)(Cs2Area->fileinfo[fid].size >> 24);
  Cs2Area->transfileinfo[5] = (u8)(Cs2Area->fileinfo[fid].size >> 16);
  Cs2Area->transfileinfo[6] = (u8)(Cs2Area->fileinfo[fid].size >> 8);
  Cs2Area->transfileinfo[7] = (u8)Cs2Area->fileinfo[fid].size;

  Cs2Area->transfileinfo[8] = Cs2Area->fileinfo[fid].interleavegapsize;
  Cs2Area->transfileinfo[9] = Cs2Area->fileinfo[fid].fileunitsize;
  Cs2Area->transfileinfo[10] = (u8) fid;
  Cs2Area->transfileinfo[11] = Cs2Area->fileinfo[fid].flags;
}

//////////////////////////////////////////////////////////////////////////////

partition_struct * Cs2ReadUnFilteredSector(u32 rufsFAD) {
  partition_struct * rufspartition;
  unsigned char syncheader[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                          0xFF, 0xFF, 0xFF, 0x00};

  if ((rufspartition = Cs2GetPartition(Cs2Area->outconcddev)) != NULL && !Cs2Area->isbufferfull)
  {
     // Allocate Block
     rufspartition->block[rufspartition->numblocks] = Cs2AllocateBlock(&rufspartition->blocknum[rufspartition->numblocks], Cs2Area->getsectsize);

     if (rufspartition->block[rufspartition->numblocks] == NULL)
        return NULL;

     // read a sector using cd interface function
     if (!Cs2Area->cdi->ReadSectorFAD(rufsFAD, Cs2Area->workblock.data))
     {
        // Give the block back: it was taken from the pool (blockfreespace)
        // but never attached to the partition (numblocks not incremented),
        // so it would otherwise be lost for good.
        Cs2FreeBlock(rufspartition->block[rufspartition->numblocks]);
        rufspartition->block[rufspartition->numblocks] = NULL;
        rufspartition->blocknum[rufspartition->numblocks] = 0xFF;
        return NULL;
     }

     // convert raw sector to type specified in getsectsize
     switch(Cs2Area->getsectsize)
     {
        case 2048: // user data only
                   if (Cs2Area->workblock.data[0xF] == 0x02)
                   {
                      // is it form1/form2 data?
                      if (!(Cs2Area->workblock.data[0x12] & 0x20))
                      {
                         // form 1
                         memcpy(rufspartition->block[rufspartition->numblocks]->data,
                                Cs2Area->workblock.data + 24, 2048);
                         Cs2Area->workblock.size = Cs2Area->getsectsize;
                      }
                      else
                      {
                         // form 2
                         memcpy(rufspartition->block[rufspartition->numblocks]->data,
                                Cs2Area->workblock.data + 24, 2324);
                         Cs2Area->workblock.size = 2324;
                      }
                   }
                   else
                   {
                      memcpy(rufspartition->block[rufspartition->numblocks]->data,
                             Cs2Area->workblock.data + 16, 2048);
                      Cs2Area->workblock.size = Cs2Area->getsectsize;
                   }
                   break;
        case 2336: // skip sync+header data
                   memcpy(rufspartition->block[rufspartition->numblocks]->data,
                   Cs2Area->workblock.data + 16, 2336);
                   Cs2Area->workblock.size = Cs2Area->getsectsize;
                   break;
        case 2340: // skip sync data
                   memcpy(rufspartition->block[rufspartition->numblocks]->data,
                   Cs2Area->workblock.data + 12, 2340);
                   Cs2Area->workblock.size = Cs2Area->getsectsize;
                   break;
        case 2352: // no conversion needed
                   Cs2Area->workblock.size = Cs2Area->getsectsize;
                   break;
        default: break;
     }

     // if mode 2 track, setup the subheader values
     if (memcmp(syncheader, Cs2Area->workblock.data, 12) == 0 &&
         Cs2Area->workblock.data[0xF] == 0x02)
     {
        rufspartition->block[rufspartition->numblocks]->fn = Cs2Area->workblock.data[0x10];
        rufspartition->block[rufspartition->numblocks]->cn = Cs2Area->workblock.data[0x11];
        rufspartition->block[rufspartition->numblocks]->sm = Cs2Area->workblock.data[0x12];
        rufspartition->block[rufspartition->numblocks]->ci = Cs2Area->workblock.data[0x13];
     }

     Cs2Area->workblock.FAD = rufsFAD;

     // Modify Partition values
     if (rufspartition->size == -1) rufspartition->size = 0;
     rufspartition->size += rufspartition->block[rufspartition->numblocks]->size;
     rufspartition->numblocks++;

     return rufspartition;
  }

  return NULL;
}

//////////////////////////////////////////////////////////////////////////////

int Cs2ReadFilteredSector(u32 rfsFAD, partition_struct **partition) {
  unsigned char syncheader[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                          0xFF, 0xFF, 0xFF, 0x00};
  int isaudio = 0;


  if (Cs2Area->outconcddev != NULL && !Cs2Area->isbufferfull)
  {
     // read a sector using cd interface function to workblock.data
     if (!Cs2Area->cdi->ReadSectorFAD(rfsFAD, Cs2Area->workblock.data))
     {
        *partition = NULL;
        return -2;
     }

     Cs2Area->workblock.size = Cs2Area->getsectsize;
     Cs2Area->workblock.FAD = rfsFAD;

     if (memcmp(syncheader, Cs2Area->workblock.data, 12) != 0) isaudio = 1;


     // force 1x speed if reading from an audio track
     Cs2Area->isaudio = isaudio;
     // Cs2SetTiming(1);

     // if mode 2 track, setup the subheader values
     if (isaudio)
     {
        ScspReceiveCDDA(Cs2Area->workblock.data);
        *partition = NULL;
        return 0;
     }
     else if (Cs2Area->workblock.data[0xF] == 0x02)
     {
        // if it's form 2 data the sector size should be 2324
        if (Cs2Area->workblock.data[0x12] & 0x20) Cs2Area->workblock.size = 2324;

        Cs2Area->workblock.fn = Cs2Area->workblock.data[0x10];
        Cs2Area->workblock.cn = Cs2Area->workblock.data[0x11];
        Cs2Area->workblock.sm = Cs2Area->workblock.data[0x12];
        Cs2Area->workblock.ci = Cs2Area->workblock.data[0x13];

        // Video CD Card (EXPERIMENTAL, see mpegcard.h): every Mode 2
        // Form 2 sector read while a White Book Video CD is mounted and
        // the Video CD Card is the configured cartridge is handed to the
        // MPEG-PS decoder as-is. PL_MPEG's own demuxer sorts video from
        // audio packets internally, so this doesn't need to decode the
        // XA submode bits itself -- simpler, and avoids guessing at
        // Video-CD-specific submode conventions no document confirms.
        // Only Form 2 sectors (submode bit 5) carry the MPEG streams; their
        // user data is exactly 2324 bytes after the 24-byte sync+header+
        // subheader. Form 1 sectors (ISO9660, INFO.VCD...) are not MPEG and
        // may report a 2352-byte size, which would read past the buffer.

        // NB: the decoder is fed further down, once the filters have decided
        // where the sector goes (see the "stored in partition" path in the
        // play loop). Feeding it from here would bypass the routing the
        // application set up with Set Filter Subheader Conditions.
     }


     // pass workblock to filter function(after it identifies partition,
     // it should allocate the partition block, setup/change the partition
     // values, and copy workblock to the allocated block)
     *partition = Cs2FilterData(Cs2Area->outconcddev, isaudio);
     return 0;
  }
  else{
    // read a sector using cd interface function to workblock.data
    if (!Cs2Area->cdi->ReadSectorFAD(rfsFAD, Cs2Area->workblock.data))
    {
      *partition = NULL;
      return -2;
    }

    Cs2Area->workblock.size = Cs2Area->getsectsize;
    Cs2Area->workblock.FAD = rfsFAD;

    if (memcmp(syncheader, Cs2Area->workblock.data, 12) != 0) isaudio = 1;

    // force 1x speed if reading from an audio track
    Cs2Area->isaudio = isaudio;
    // Cs2SetTiming(1);

    // if mode 2 track, setup the subheader values
    if (isaudio)
    {
      ScspReceiveCDDA(Cs2Area->workblock.data);
      *partition = NULL;
      return 0;
    }
  }

  *partition = NULL;
  return -1;
}

char * Cs2GetCurrentGmaecode(){
	if(cdip==NULL) return NULL;
	return cdip->itemnum;
}

//////////////////////////////////////////////////////////////////////////////
u8 Cs2GetIP(int autoregion) {
   partition_struct * gripartition;
   u8 ret = 0;

   Cs2Area->outconcddev = Cs2Area->filter + 0;
   Cs2Area->outconcddevnum = 0;

   // read in lba 0/FAD 150
   if ((gripartition = Cs2ReadUnFilteredSector(150)) != NULL)
   {
	   int i;
      unsigned char *buf=(unsigned char*)gripartition->block[gripartition->numblocks - 1]->data;

      // Make sure we're dealing with a saturn game
      if (memcmp(buf, "SEGA SEGASATURN", 15) == 0)
      {
         memcpy(cdip->system, buf, 16);
         cdip->system[16]='\0';
         memcpy(cdip->company, buf+0x10, 16);
         cdip->company[16]='\0';
         char tmp[11];
         memcpy(tmp, buf+0x20, 0x0A);
         tmp[10]='\0';
         sscanf(tmp, "%s", cdip->itemnum);

		 // make gameid as u64
		 cdip->gameid = 0;
		 for (i = 0; i < 8; i++){
			 cdip->gameid |= ((u64)cdip->itemnum[i]) << (i * 8);
		 }
         memcpy(cdip->version, buf+0x2A, 6);
         cdip->version[6]='\0';
         sprintf(cdip->date, "%c%c/%c%c/%c%c%c%c", buf[0x34], buf[0x35], buf[0x36], buf[0x37], buf[0x30], buf[0x31], buf[0x32], buf[0x33]);
         sscanf((const char*)(buf+0x38), "%s", cdip->cdinfo);
         sscanf((const char*)(buf+0x40), "%s", cdip->region);
         sscanf((const char*)(buf+0x50), "%s", cdip->peripheral);
         memcpy(cdip->gamename, buf+0x60, 112);
         cdip->gamename[112]='\0';
#ifdef WORDS_BIGENDIAN
         memcpy(&cdip->ipsize, buf+0xE0, sizeof(u32));
         memcpy(&cdip->msh2stack, buf+0xE8, sizeof(u32));
         memcpy(&cdip->ssh2stack, buf+0xEC, sizeof(u32));
         memcpy(&cdip->firstprogaddr, buf+0xF0, sizeof(u32));
         memcpy(&cdip->firstprogsize, buf+0xF4, sizeof(u32));
#else
         cdip->ipsize = (buf[0xE0] << 24) | (buf[0xE1] << 16) |
                        (buf[0xE2] << 8) | buf[0xE3];
         cdip->msh2stack = (buf[0xE8] << 24) | (buf[0xE9] << 16) |
                           (buf[0xEA] << 8) | buf[0xEB];
         cdip->ssh2stack = (buf[0xEC] << 24) | (buf[0xED] << 16) |
                           (buf[0xEE] << 8) | buf[0xEF];
         cdip->firstprogaddr = (buf[0xF0] << 24) | (buf[0xF1] << 16) |
                               (buf[0xF2] << 8) | buf[0xF3];
         cdip->firstprogsize = (buf[0xF4] << 24) | (buf[0xF5] << 16) |
                               (buf[0xF6] << 8) | buf[0xF7];
//Real bios is copying data at the firstprogaddr which correspond to the entry point (MSH2->PC) of the game.
// ST-040-R4-051795.pdf is describing a bit the mechanism, look at 1st READ ADDRESS
         if (cdip->msh2stack == 0 )
         {
            /* STACK-M defaut (ST-040-R4-051795 / TECH#11) : "Default (0
               specified) 6001000H ~ 6001FFFH becomes the stack area."

               C'est la ZONE de pile, pas la valeur initiale de R15. Sur
               SH-2 l'empilement se fait en pre-decrement (MOV.L Rn,@-R15),
               donc R15 doit demarrer une case APRES le haut de la zone :
               6001FFFH + 1 = 6002000H. Le premier push ecrit alors en
              6001FFCH, a l'interieur de la zone.
               Mettre R15 = 6001000H (le BAS de la zone) fait descendre le
               premier push en 6000FFCH, c'est-a-dire hors de la zone du
               maitre, dans celle de l'esclave, puis dans 6000900H-60009FFH
               (table des handlers SCU du BIOS emule), 6000348H (masque
               d'interruption memorise) et enfin la table de vecteurs
               construite par BiosInit(). En BIOS emule le jeu detruit donc
               le BIOS lui-meme des ses premiers appels de fonction.

               Le diagramme de TECH#35 se lit de bas en haut :
               6000000H-6000E00H vecteurs et routines residentes,
               6000E00H-6001000H pile de l'esclave,
               6001000H-6002000H pile du maitre.
               Les etiquettes marquent les bornes BASSES des zones.

               Coherent avec YabauseFullInit(), qui ecrit deja 0x06002000
               dans le vecteur 1 (SP initial) de la ROM BIOS emulee. */
            cdip->msh2stack = 0x6002000;
         }

         // for Panzer Dragoon Zwei. This operation is not written in the document.
         if (cdip->msh2stack & 0x80000000)
         {
            cdip->msh2stack = 0x06000000 + (cdip->msh2stack & 0x0000FFFF );
         }

         if (cdip->ssh2stack == 0 )
         {
            /* STACK-S defaut : zone 6000D00H ~ 6000FFFH (ST-040-R4 /
               TECH#11), donc R15 initial = 6001000H, meme raisonnement que
               pour le maitre ci-dessus.

               Et cette fois le document donne directement la valeur du
               registre : SEGA Saturn Dual CPU User's Guide (ST-202-R1),
               4.4 "Initialization (Vector, Stack) by the Boot ROM" :
                 1. Vector VBR is set to address 6000400H.
                 2. Stack SP is set to address 6001000H.
               YabauseStartSlave() pose deja VBR = 0x06000400 ; SP doit
               aller avec. 6000E00H ne laissait que 256 octets de pile et
               ne correspond a aucune valeur de registre documentee. */
            cdip->ssh2stack = 0x6001000;
         }

         if (cdip->ssh2stack & 0x80000000)
         {
            cdip->ssh2stack = 0x06000000 + (cdip->ssh2stack & 0x0000FFFF);
         }
#endif

         if (autoregion)
         {
            // Read first available region, that'll be what we'll use
            switch (cdip->region[0])
            {
               case 'J':
                         ret = 1;
                         break;
               case 'T':
                         ret = 2;
                         break;
               case 'U':
                         ret = 4;
                         break;
               case 'B':
                         ret = 5;
                         break;
               case 'K':
                         ret = 6;
                         break;
               case 'A':
                         ret = 0xA;
                         break;
               case 'E':
                         ret = 0xC;
                         break;
               case 'L':
                         ret = 0xD;
                         break;
               default: break;
            }
         }
      }

      // Free Block
      gripartition->size -= gripartition->block[gripartition->numblocks - 1]->size;
      Cs2FreeBlock(gripartition->block[gripartition->numblocks - 1]);
      gripartition->block[gripartition->numblocks - 1] = NULL;
      gripartition->blocknum[gripartition->numblocks - 1] = 0xFF;

      // Sort remaining blocks
      Cs2SortBlocks(gripartition);
      gripartition->numblocks -= 1;
   }

   return ret;
}

//////////////////////////////////////////////////////////////////////////////

u8 Cs2GetRegionID(void)
{
   return Cs2GetIP(1);
}

//////////////////////////////////////////////////////////////////////////////

int Cs2SaveState(void ** stream) {
   int offset, i, i2;

   // This is mostly kludge, but it will have to do until I have time to rewrite it all

   offset = MemStateWriteHeader(stream, "CS2 ", 3);

   // Write cart type
   MemStateWrite((void *)&Cs2Area->carttype, 4, 1, stream);

   // Write cd block registers
   MemStateWrite((void *)&Cs2Area->reg, sizeof(blockregs_struct), 1, stream);

   // Write current Status variables(needs a rewrite)
   MemStateWrite((void *)&Cs2Area->FAD, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->status, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->options, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->repcnt, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->ctrladdr, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->track, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->index, 1, 1, stream);

   // Write other cd block internal variables
   MemStateWrite((void *)&Cs2Area->satauth, 2, 1, stream);
   MemStateWrite((void *)&Cs2Area->mpgauth, 2, 1, stream);

   MemStateWrite((void *)&Cs2Area->transfercount, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->cdwnum, 4, 1, stream);
   MemStateWrite((void *)Cs2Area->TOC, 4, 102, stream);
   MemStateWrite((void *)&Cs2Area->playFAD, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->playendFAD, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->maxrepeat, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->getsectsize, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->putsectsize, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->calcsize, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->infotranstype, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->datatranstype, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->isonesectorstored, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->isdiskchanged, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->isbufferfull, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->speed1x, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->isaudio, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->transfileinfo, 1, 12, stream);
   MemStateWrite((void *)&Cs2Area->lastbuffer, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->transscodeq, 5*2, 1, stream);
   MemStateWrite((void *)&Cs2Area->transscoderw, 12*2, 1, stream);
   MemStateWrite((void *)&Cs2Area->_command, 1, 1, stream);
   {
      u32 temp = (Cs2Area->_periodictiming + 3) / 3;
      MemStateWrite((void *)&temp, 4, 1, stream);
   }
   MemStateWrite((void *)&Cs2Area->_commandtiming, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->outconcddevnum, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->outconmpegfbnum, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->outconmpegbufnum, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->outconmpegromnum, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->outconhostnum, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->datatranspartitionnum, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->datatransoffset, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->datanumsecttrans, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->datatranssectpos, 2, 1, stream);
   MemStateWrite((void *)&Cs2Area->datasectstotrans, 2, 1, stream);
   MemStateWrite((void *)&Cs2Area->blockfreespace, 4, 1, stream);
   MemStateWrite((void *)&Cs2Area->curdirsect, 4, 1, stream);

   // Write CD buffer
   MemStateWrite((void *)Cs2Area->block, sizeof(block_struct), MAX_BLOCKS, stream);

   // Write partition data
   for (i = 0; i < MAX_SELECTORS; i++)
   {
      MemStateWrite((void *)&Cs2Area->partition[i].size, 4, 1, stream);
      MemStateWrite((void *)Cs2Area->partition[i].blocknum, 1, MAX_BLOCKS, stream);
      MemStateWrite((void *)&Cs2Area->partition[i].numblocks, 1, 1, stream);

      u32 index = 0;
      for (i2 = 0; i2 < MAX_BLOCKS; i2++)
      {
        if (Cs2Area->partition[i].block[i2] == NULL)
          index = 0xFFFFFFFF;
        else
          index = Cs2Area->partition[i].block[i2] - Cs2Area->block;
        MemStateWrite(&index, 4, 1, stream);
      }
   }

   // Write filter data
   MemStateWrite((void *)Cs2Area->filter, sizeof(filter_struct), MAX_SELECTORS, stream);

   // Write File Info Table
   MemStateWrite((void *)Cs2Area->fileinfo, sizeof(dirrec_struct), MAX_FILES, stream);

   // Write MPEG card registers here

   // Write current MPEG card status variables
   MemStateWrite((void *)&Cs2Area->actionstatus, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->pictureinfo, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->mpegaudiostatus, 1, 1, stream);
   MemStateWrite((void *)&Cs2Area->mpegvideostatus, 2, 1, stream);
   MemStateWrite((void *)&Cs2Area->vcounter, 2, 1, stream);

   // Write other MPEG card internal variables
   MemStateWrite((void *)&Cs2Area->mpegintmask, 4, 1, stream);
   MemStateWrite((void *)Cs2Area->mpegcon, sizeof(mpegcon_struct), 2, stream);
   MemStateWrite((void *)Cs2Area->mpegstm, sizeof(mpegstm_struct), 2, stream);

   MemStateWrite((void *)&Cs2Area->playtype, 4, 1, stream);

   MemStateWrite((void *)&Cs2Area->_seekToStop, 1, 1, stream);
   return MemStateFinishHeader(stream, offset);
}

//////////////////////////////////////////////////////////////////////////////

int Cs2LoadState(const void * stream, int version, int size) {
   int i, i2;

   Cs2Reset();

   // This is mostly kludge, but it will have to do until I have time to rewrite it all
   CDLOG("************* Cs2LoadState *********************");

   // Read cart type
   MemStateRead((void *)&Cs2Area->carttype, 4, 1, stream);

   // Read cd block registers
   MemStateRead((void *)&Cs2Area->reg, sizeof(blockregs_struct), 1, stream);

   // Read current Status variables(needs a reRead)
   MemStateRead((void *)&Cs2Area->FAD, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->status, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->options, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->repcnt, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->ctrladdr, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->track, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->index, 1, 1, stream);

   // Read other cd block internal variables
   MemStateRead((void *)&Cs2Area->satauth, 2, 1, stream);
   MemStateRead((void *)&Cs2Area->mpgauth, 2, 1, stream);

   MemStateRead((void *)&Cs2Area->transfercount, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->cdwnum, 4, 1, stream);
   MemStateRead((void *)Cs2Area->TOC, 4, 102, stream);
   MemStateRead((void *)&Cs2Area->playFAD, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->playendFAD, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->maxrepeat, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->getsectsize, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->putsectsize, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->calcsize, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->infotranstype, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->datatranstype, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->isonesectorstored, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->isdiskchanged, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->isbufferfull, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->speed1x, 1, 1, stream);
   if (version > 1)
      MemStateRead((void *)&Cs2Area->isaudio, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->transfileinfo, 1, 12, stream);
   MemStateRead((void *)&Cs2Area->lastbuffer, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->transscodeq, 5 * 2, 1, stream);
   MemStateRead((void *)&Cs2Area->transscoderw, 12 * 2, 1, stream);
   MemStateRead((void *)&Cs2Area->_command, 1, 1, stream);
   {
      u32 temp;
      MemStateRead((void *)&temp, 4, 1, stream);
      // Derive the actual, accurate value (always a multiple of 10)
      Cs2Area->_periodictiming = ((temp * 3) / 10) * 10;
   }
   MemStateRead((void *)&Cs2Area->_commandtiming, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->outconcddevnum, 1, 1, stream);
   if (Cs2Area->outconcddevnum == 0xFF)
      Cs2Area->outconcddev = NULL;
   else
      Cs2Area->outconcddev = Cs2Area->filter + Cs2Area->outconcddevnum;

   MemStateRead((void *)&Cs2Area->outconmpegfbnum, 1, 1, stream);
   if (Cs2Area->outconmpegfbnum == 0xFF)
      Cs2Area->outconmpegfb = NULL;
   else
      Cs2Area->outconmpegfb = Cs2Area->filter + Cs2Area->outconmpegfbnum;

   MemStateRead((void *)&Cs2Area->outconmpegbufnum, 1, 1, stream);
   if (Cs2Area->outconmpegbufnum == 0xFF)
      Cs2Area->outconmpegbuf = NULL;
   else
      Cs2Area->outconmpegbuf = Cs2Area->filter + Cs2Area->outconmpegbufnum;

   MemStateRead((void *)&Cs2Area->outconmpegromnum, 1, 1, stream);
   if (Cs2Area->outconmpegromnum == 0xFF)
      Cs2Area->outconmpegrom = NULL;
   else
      Cs2Area->outconmpegrom = Cs2Area->filter + Cs2Area->outconmpegromnum;

   MemStateRead((void *)&Cs2Area->outconhostnum, 1, 1, stream);
   if (Cs2Area->outconhostnum == 0xFF)
      Cs2Area->outconhost = NULL;
   else
      Cs2Area->outconhost = Cs2Area->filter + Cs2Area->outconhostnum;

   MemStateRead((void *)&Cs2Area->datatranspartitionnum, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->datatransoffset, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->datanumsecttrans, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->datatranssectpos, 2, 1, stream);
   MemStateRead((void *)&Cs2Area->datasectstotrans, 2, 1, stream);
   MemStateRead((void *)&Cs2Area->blockfreespace, 4, 1, stream);
   MemStateRead((void *)&Cs2Area->curdirsect, 4, 1, stream);

   // Read CD buffer
   MemStateRead((void *)Cs2Area->block, sizeof(block_struct), MAX_BLOCKS, stream);

   // Read partition data
   for (i = 0; i < MAX_SELECTORS; i++)
   {
      MemStateRead((void *)&Cs2Area->partition[i].size, 4, 1, stream);
      MemStateRead((void *)Cs2Area->partition[i].blocknum, 1, MAX_BLOCKS, stream);
      MemStateRead((void *)&Cs2Area->partition[i].numblocks, 1, 1, stream);

      u32 index=0;
      for (i2 = 0; i2 < MAX_BLOCKS; i2++)
      {
        MemStateRead((void *)&index, 4, 1, stream);
        if (index == 0xFFFFFFFF){
          Cs2Area->partition[i].block[i2] = NULL;
        }
        else{
          Cs2Area->partition[i].block[i2] = Cs2Area->block + index;
        }
      }
   }

   // Read filter data
   MemStateRead((void *)Cs2Area->filter, sizeof(filter_struct), MAX_SELECTORS, stream);

   // Read File Info Table
   MemStateRead((void *)Cs2Area->fileinfo, sizeof(dirrec_struct), MAX_FILES, stream);

   // Read MPEG card registers here

   // Read current MPEG card status variables
   MemStateRead((void *)&Cs2Area->actionstatus, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->pictureinfo, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->mpegaudiostatus, 1, 1, stream);
   MemStateRead((void *)&Cs2Area->mpegvideostatus, 2, 1, stream);
   MemStateRead((void *)&Cs2Area->vcounter, 2, 1, stream);

   // Read other MPEG card internal variables
   MemStateRead((void *)&Cs2Area->mpegintmask, 4, 1, stream);
   MemStateRead((void *)Cs2Area->mpegcon, sizeof(mpegcon_struct), 2, stream);
   MemStateRead((void *)Cs2Area->mpegstm, sizeof(mpegstm_struct), 2, stream);

   MemStateRead((void *)&Cs2Area->playtype, 4, 1, stream);

   if (version > 2) {
     MemStateRead((void *)&Cs2Area->_seekToStop, 1, 1, stream);
   }
   return size;
}

/* Valeurs de repli : memes SP que ci-dessus (ST-202-R1 4.4 pour
   l'esclave), pas les bornes basses des zones de pile. */
/* Un cdip alloue mais pas encore rempli (cf. le commentaire en tete de
   YabauseQuickLoadGame()) vaut zero sans etre NULL. Tester le champ lui-meme,
   et pas seulement le pointeur, evite de renvoyer PC = 0 et SP = 0 au maitre.
   Replis : entree de l'AIP (6002000H + 100H SYSTEM ID + D00H code de securite
   = 6002E00H) et sommets des zones de pile documentees. */
u32 Cs2GetMasterStackAdress(){ if (cdip && cdip->msh2stack) return cdip->msh2stack; else return 0x6002000; }
u32 Cs2GetSlaveStackAdress(){ if (cdip && cdip->ssh2stack) return cdip->ssh2stack; else return 0x6001000; }
u32 Cs2GetMasterExecutionAdress(){ if (cdip && cdip->firstprogaddr) return cdip->firstprogaddr; else return 0x06002E00; }
u64 Cs2GetGameId(){ if (cdip) return cdip->gameid; else return 0x00; }

//////////////////////////////////////////////////////////////////////////////
