/*  Copyright 2004-2005 Theo Berkau
    Copyright 2006 Ex-Cyber
    Copyright 2005 Guillaume Duhamel

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

/*! \file cs0.c
    \brief A-bus CS0 emulation functions. Most of the cartridge related code is here.
*/

#include <stdlib.h>
#include <string.h>
#include "cs0.h"
#include "error.h"
#include "japmodem.h"
#include "netlink.h"
#include "decrypt.h"
#include "yui.h"
#include "rax.h"
#include "peripheral.h"
#include "eeprom.h"

cartridge_struct *CartridgeArea;

static u8 decryptOn = 0;
/* Bus-echo shadow for the 315-5881 protection registers, see
   ROMSTVCs1ReadWord/ROMSTVCs1WriteWord (mirrors MAME's m_a_bus[]). */
static u16 protRegEnable = 0;
static u16 protRegLowAddr = 0;
static u16 protRegHighAddr = 0;
static u16 protRegSubkey = 0;

/* Set by init_5838*() (stv.c) for the carts using the 315-5838 family
 * compression chip (Decathlete 315-5838, Name Club 317-0229, Print Club
 * 317-0230/0231). Cleared at every game load (loadGame) and STVDeInit,
 * so for every other ROMSTV game the normal CS0 ROM read/write path below
 * is completely unaffected. */
u8 decathleteProtEnabled = 0;

#define LOGSTV
#define LOGBUP

#define DEV_LOG_ADDR 0x1000
#define DEV_LOG_SIZE 1024

static uint8_t log_buffer[DEV_LOG_SIZE];
static uint8_t *log_pos = log_buffer;

//////////////////////////////////////////////////////////////////////////////
// Dummy/No Cart Functions
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL DummyCs0ReadByte(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL DummyCs0ReadWord(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   return 0xFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DummyCs0ReadLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs0WriteByte(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u8 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs0WriteWord(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u16 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs0WriteLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u32 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL DummyCs1ReadByte(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   if (addr == 0xFFFFFF)
      return CartridgeArea->cartid;
   return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL DummyCs1ReadWord(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   if (addr == 0xFFFFFE)
      return (0xFF00 | CartridgeArea->cartid);
   return 0xFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DummyCs1ReadLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   if (addr == 0xFFFFFC)
      return (0xFF00FF00 | (CartridgeArea->cartid << 16) | CartridgeArea->cartid);
   return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs1WriteByte(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u8 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs1WriteWord(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u16 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs1WriteLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u32 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL DummyCs2ReadByte(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL DummyCs2ReadWord(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   return 0xFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DummyCs2ReadLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr)
{
   return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs2WriteByte(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u8 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs2WriteWord(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u16 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DummyCs2WriteLong(SH2_struct *context, UNUSED u8* memory, UNUSED u32 addr, UNUSED u32 val)
{
}

//////////////////////////////////////////////////////////////////////////////
// Action Replay 4M Plus funcions
//////////////////////////////////////////////////////////////////////////////

typedef enum
  {
    FL_READ,
    FL_SDP,
    FL_CMD,
    FL_ID,
    FL_IDSDP,
    FL_IDCMD,
    FL_WRITEBUF,
    FL_WRITEARRAY
  } flashstate;

u8 flreg0 = 0;
u8 flreg1 = 0;

// Default value is for chip AT29C010
u8 vendorid=0x1F;
u8 deviceid=0xD5;

flashstate flstate0;
flashstate flstate1;

u8 flbuf0[128];
u8 flbuf1[128];

//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL FlashCs0ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  flashstate* state;
  u8* reg;

  if (addr & 1)
    {
      state = &flstate1;
      reg = &flreg1;
    }
  else
    {
      state = &flstate0;
      reg = &flreg0;
    }

  switch (*state)
    {
    case FL_ID:
    case FL_IDSDP:
    case FL_IDCMD:
       if (addr & 2) return deviceid;
       else return vendorid;
    case FL_WRITEARRAY: *reg ^= 0x02;
    case FL_WRITEBUF: return *reg;
    case FL_SDP:
    case FL_CMD: *state = FL_READ;
    case FL_READ:
    default: return T2ReadByte(memory, addr);
    }
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL FlashCs0ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  return ((u16)(FlashCs0ReadByte(NULL, memory, addr) << 8) | (u16)(FlashCs0ReadByte(NULL, memory, addr+1)));
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL FlashCs0ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  return ((u32)FlashCs0ReadWord(NULL, memory, addr) << 16) |(u32) FlashCs0ReadWord(NULL, memory, addr + 2);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL FlashCs0WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   flashstate* state;
   u8* reg;
   u8* buf;

   if (addr & 1)
   {
      state = &flstate1;
      reg = &flreg1;
      buf = flbuf1;
   }
   else
   {
      state = &flstate0;
      reg = &flreg0;
      buf = flbuf0;
   }

   switch (*state)
   {
      case FL_READ:
         if (((addr & 0xfffe) == 0xaaaa) && (val == 0xaa))
            *state = FL_SDP;
         return;
      case FL_WRITEBUF:
         buf[(addr >> 1) & 0x7f] = val;
         if (((addr >> 1) & 0x7f) == 0x7f)
         {
            int i;
            int j = addr & 0x1;
            addr &= 0xffffff00;
            for (i = 0; i <= 127; i++)
            {
               T2WriteByte(memory, (addr + i*2 + j), buf[i]);
            }
            *state = FL_READ;
         }
         return;
      case FL_SDP:
         if (((addr & 0xfffe) == 0x5554) && (val == 0x55))
            *state = FL_CMD;
         else *state = FL_READ;
         return;
      case FL_ID:
         if (((addr & 0xfffe) == 0xaaaa) && (val == 0xaa))
            *state = FL_IDSDP;
         else *state = FL_ID;
         return;
      case FL_IDSDP:
         if (((addr & 0xfffe) == 0x5554) && (val == 0x55))
            *state = FL_READ;
         else *state=FL_ID;
         return;
      case FL_IDCMD:
         if (((addr & 0xfffe) == 0xaaaa) && (val == 0xf0))
            *state = FL_READ;
         else *state = FL_ID;
         return;
      case FL_CMD:
         if ((addr & 0xfffe) != 0xaaaa)
         {
            *state = FL_READ;
            return;
         }

         switch (val)
         {
            case 0xa0:
               *state = FL_WRITEBUF;
               return;
            case 0x90:
               *state = FL_ID;
               return;
            default:
               *state = FL_READ;
               return;
         }
      default: break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL FlashCs0WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
  FlashCs0WriteByte(context, memory, addr, (u8)(val >> 8));
  FlashCs0WriteByte(context, memory, addr + 1, (u8)(val & 0xff));
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL FlashCs0WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
  FlashCs0WriteWord(context, memory, addr, (u16)(val >> 16));
  FlashCs0WriteWord(context, memory, addr + 2, (u16)(val & 0xffff));
}

//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL AR4MCs0ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x00:
      {
         if ((addr & 0x80000) == 0) // EEPROM
	   return FlashCs0ReadByte(NULL, memory,addr);
//            return biosarea->getByte(addr);
//         else // Outport
//            fprintf(stderr, "Commlink Outport Byte read\n");
         break;
      }
      case 0x01:
      {
//         if ((addr & 0x80000) == 0) // Commlink Status flag
//            fprintf(stderr, "Commlink Status Flag read\n");
//         else // Inport for Commlink
//            fprintf(stderr, "Commlink Inport Byte read\n");
         break;
      }
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Dram area
         return T1ReadByte(CartridgeArea->dram, addr & 0x3FFFFF);
      default:   // The rest doesn't matter
         break;
   }

   return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL AR4MCs0ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x00:
      {
         if ((addr & 0x80000) == 0) // EEPROM
	   return FlashCs0ReadWord(NULL, memory, addr);
//         else // Outport
//            fprintf(stderr, "Commlink Outport Word read\n");
         break;
      }
      case 0x01:
      {
//         if ((addr & 0x80000) == 0) // Commlink Status flag
//            fprintf(stderr, "Commlink Status Flag read\n");
//         else // Inport for Commlink
//            fprintf(stderr, "Commlink Inport Word read\n");
         break;
      }
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         return T1ReadWord(CartridgeArea->dram, addr & 0x3FFFFF);
      case 0x12:
      case 0x1E:
         if (0x80000)
            return 0xFFFD;
         break;
      case 0x13:
      case 0x16:
      case 0x17:
      case 0x1A:
      case 0x1B:
      case 0x1F:
         return 0xFFFD;
      default:   // The rest doesn't matter
         break;
   }

   return 0xFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL AR4MCs0ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x00:
      {
         if ((addr & 0x80000) == 0) // EEPROM
	   return FlashCs0ReadLong(NULL, memory, addr);
//         else // Outport
//            fprintf(stderr, "Commlink Outport Long read\n");
         break;
      }
      case 0x01:
      {
//         if ((addr & 0x80000) == 0) // Commlink Status flag
//            fprintf(stderr, "Commlink Status Flag read\n");
//         else // Inport for Commlink
//            fprintf(stderr, "Commlink Inport Long read\n");
         break;
      }
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         return T1ReadLong(CartridgeArea->dram, addr & 0x3FFFFF);
      case 0x12:
      case 0x1E:
         if (0x80000)
            return 0xFFFDFFFD;
         break;
      case 0x13:
      case 0x16:
      case 0x17:
      case 0x1A:
      case 0x1B:
      case 0x1F:
         return 0xFFFDFFFD;
      default:   // The rest doesn't matter
         break;
   }

   return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL AR4MCs0WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x00:
      {
         if ((addr & 0x80000) == 0) // EEPROM
	   FlashCs0WriteByte(context, memory, addr, val);
//         else // Outport
//            fprintf(stderr, "Commlink Outport byte write\n");
         break;
      }
      case 0x01:
      {
//         if ((addr & 0x80000) == 0) // Commlink Status flag
//            fprintf(stderr, "Commlink Status Flag write\n");
//         else // Inport for Commlink
//            fprintf(stderr, "Commlink Inport Byte write\n");
         break;
      }
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         T1WriteByte(CartridgeArea->dram, addr & 0x3FFFFF, val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL AR4MCs0WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x00:
      {
         if ((addr & 0x80000) == 0) // EEPROM
	   FlashCs0WriteWord(context, memory, addr, val);
//         else // Outport
//            fprintf(stderr, "Commlink Outport Word write\n");
         break;
      }
      case 0x01:
      {
//         if ((addr & 0x80000) == 0) // Commlink Status flag
//            fprintf(stderr, "Commlink Status Flag write\n");
//         else // Inport for Commlink
//            fprintf(stderr, "Commlink Inport Word write\n");
         break;
      }
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         T1WriteWord(CartridgeArea->dram, addr & 0x3FFFFF, val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL AR4MCs0WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x00:
      {
         if ((addr & 0x80000) == 0) // EEPROM
	   FlashCs0WriteLong(context, memory, addr, val);
//         else // Outport
//            fprintf(stderr, "Commlink Outport Long write\n");
         break;
      }
      case 0x01:
      {
//         if ((addr & 0x80000) == 0) // Commlink Status flag
//            fprintf(stderr, "Commlink Status Flag write\n");
//         else // Inport for Commlink
//            fprintf(stderr, "Commlink Inport Long write\n");
         break;
      }
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         T1WriteLong(CartridgeArea->dram, addr & 0x3FFFFF, val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////
// 8 Mbit Dram
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL DRAM8MBITCs0ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04: // Dram area
         return T1ReadByte(CartridgeArea->dram, addr & 0x7FFFF);
      case 0x06: // Dram area
         return T1ReadByte(CartridgeArea->dram, 0x80000 | (addr & 0x7FFFF));
      default:   // The rest doesn't matter
         break;
   }

   return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL DRAM8MBITCs0ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04: // Dram area
         return T1ReadWord(CartridgeArea->dram, addr & 0x7FFFF);
      case 0x06: // Dram area
         return T1ReadWord(CartridgeArea->dram, 0x80000 | (addr & 0x7FFFF));
      default:   // The rest doesn't matter
         break;
   }

   return 0xFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DRAM8MBITCs0ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04: // Dram area
         return T1ReadLong(CartridgeArea->dram, addr & 0x7FFFF);
      case 0x06: // Dram area
         return T1ReadLong(CartridgeArea->dram, 0x80000 | (addr & 0x7FFFF));
      default:   // The rest doesn't matter
         break;
   }

   return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DRAM8MBITCs0WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04: // Dram area
         T1WriteByte(CartridgeArea->dram, addr & 0x7FFFF, val);
         break;
      case 0x06: // Dram area
         T1WriteByte(CartridgeArea->dram, 0x80000 | (addr & 0x7FFFF), val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DRAM8MBITCs0WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04: // Dram area
         T1WriteWord(CartridgeArea->dram, addr & 0x7FFFF, val);
         break;
      case 0x06: // Dram area
         T1WriteWord(CartridgeArea->dram, 0x80000 | (addr & 0x7FFFF), val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DRAM8MBITCs0WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04: // Dram area
         T1WriteLong(CartridgeArea->dram, addr & 0x7FFFF, val);
         break;
      case 0x06: // Dram area
         T1WriteLong(CartridgeArea->dram, 0x80000 | (addr & 0x7FFFF), val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////
// 32 Mbit Dram
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL DRAM32MBITCs0ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Dram area
         return T1ReadByte(CartridgeArea->dram, addr & 0x3FFFFF);
      default:   // The rest doesn't matter
         break;
   }

   return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL DRAM32MBITCs0ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         return T1ReadWord(CartridgeArea->dram, addr & 0x3FFFFF);
      default:   // The rest doesn't matter
         break;
   }

   return 0xFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DRAM32MBITCs0ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         return T1ReadLong(CartridgeArea->dram, addr & 0x3FFFFF);
      default:   // The rest doesn't matter
         break;
   }

   return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DRAM32MBITCs0WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         T1WriteByte(CartridgeArea->dram, addr & 0x3FFFFF, val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DRAM32MBITCs0WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         T1WriteWord(CartridgeArea->dram, addr & 0x3FFFFF, val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DRAM32MBITCs0WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   addr &= 0x1FFFFFF;

   switch (addr >> 20)
   {
      case 0x04:
      case 0x05:
      case 0x06:
      case 0x07: // Ram cart area
         T1WriteLong(CartridgeArea->dram, addr & 0x3FFFFF, val);
         break;
      default:   // The rest doesn't matter
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////
// Development Cartridge
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL DevCs1ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  if (addr == 0xFFFFFF)
     return CartridgeArea->cartid;
   if (addr == 0xFFFFF0)
      return CartridgeArea->cartid;
  return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL DevCs1ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  if (addr == 0xFFFFFE)
     return (0xFF00 | CartridgeArea->cartid);
  if (addr == 0xFFFFF0)
     return (0xFF00 | CartridgeArea->cartid);
  return 0xFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DevCs1ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  if (addr == 0xFFFFFC)
     return (0xFF00FF00 | (CartridgeArea->cartid << 16) | CartridgeArea->cartid);
  if (addr == 0xFFFFF0)
     return (0xFF00FF00 | (CartridgeArea->cartid << 16) | CartridgeArea->cartid);
  return 0xFFFFFFFF;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DevCs1WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
  addr &= 0x1FFFFFF;
  if (addr == DEV_LOG_ADDR)
  {
     if ((val == '\n')||(log_pos - log_buffer)>=(DEV_LOG_SIZE-1))
     {
        *log_pos++ = 0; //add \0 character to end the %s
        YuiMsg("%s\n", log_buffer);
        log_pos = log_buffer;
     } else {
        *log_pos++ = val;
     }
     return;
  }

}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DevCs1WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL DevCs1WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
}

//////////////////////////////////////////////////////////////////////////////
// 4 Mbit Backup Ram
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL BUP4MBITCs1ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr = addr & ((CART_BUP4MBIT_SIZE<<1) - 1);
   if (addr & 0x1) {
      return T1ReadByte(CartridgeArea->bupram, addr>>1);
   }
   else
      return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL BUP4MBITCs1ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   LOGBUP("bup\t: BUP4MBIT read word - %08X\n", addr);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL BUP4MBITCs1ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   LOGBUP("bup\t: BUP4MBIT read long - %08X\n", addr);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP4MBITCs1WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   addr = addr & ((CART_BUP4MBIT_SIZE<<1) - 1);
   if (addr & 0x1) {
      T1WriteByte(CartridgeArea->bupram, addr>>1, val);
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP4MBITCs1WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   LOGBUP("bup\t: BUP4MBIT write word - %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP4MBITCs1WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   LOGBUP("bup\t: BUP4MBIT write long - %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////
// 8 Mbit Backup Ram
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL BUP8MBITCs1ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr = addr & ((CART_BUP8MBIT_SIZE<<1) - 1);
   if (addr & 0x1) {
      return T1ReadByte(CartridgeArea->bupram, addr>>1);
   }
   else
      return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL BUP8MBITCs1ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   LOGBUP("bup\t: BUP8MBIT read word - %08X\n", addr);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL BUP8MBITCs1ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   LOGBUP("bup\t: BUP8MBIT read long - %08X\n", addr);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP8MBITCs1WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   addr = addr & ((CART_BUP8MBIT_SIZE<<1) - 1);
   if (addr & 0x1) {
      T1WriteByte(CartridgeArea->bupram, addr>>1, val);
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP8MBITCs1WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   LOGBUP("bup\t: BUP8MBIT write word - %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP8MBITCs1WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   LOGBUP("bup\t: BUP8MBIT write long - %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////
// 16 Mbit Backup Ram
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL BUP16MBITCs1ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr = addr & ((CART_BUP16MBIT_SIZE<<1) - 1);
   if (addr & 0x1) {
      return T1ReadByte(CartridgeArea->bupram, addr>>1);
   }
   else
      return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL BUP16MBITCs1ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   LOGBUP("bup\t: BUP16MBIT read word - %08X\n", addr);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL BUP16MBITCs1ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   LOGBUP("bup\t: BUP16MBIT read long - %08X\n", addr);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP16MBITCs1WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   addr = addr & ((CART_BUP16MBIT_SIZE<<1) - 1);
   if (addr & 0x1) {
      T1WriteByte(CartridgeArea->bupram, addr>>1, val);
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP16MBITCs1WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   LOGBUP("bup\t: BUP16MBIT write word - %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP16MBITCs1WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   LOGBUP("bup\t: BUP16MBIT write long - %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////
// 32 Mbit Backup Ram
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL BUP32MBITCs1ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   addr = addr & ((CART_BUP32MBIT_SIZE<<1) - 1);
   if (addr & 0x1) {
      return T1ReadByte(CartridgeArea->bupram, addr>>1);
   }
   else
      return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL BUP32MBITCs1ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   LOGBUP("bup\t: BUP32MBIT read word - %08X\n", addr);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL BUP32MBITCs1ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   LOGBUP("bup\t: BUP32MBIT read long - %08X\n", addr);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP32MBITCs1WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   addr = addr & ((CART_BUP32MBIT_SIZE<<1) - 1);
   if (addr & 0x1) {
      T1WriteByte(CartridgeArea->bupram, addr>>1, val);
   }
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP32MBITCs1WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   LOGBUP("bup\t: BUP32MBIT write word - %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP32MBITCs1WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   LOGBUP("bup\t: BUP32MBIT write long - %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////
// 128 Mbit Backup Ram
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL BUP128MBITCs1ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  u32 ret = T1ReadByte(CartridgeArea->dram, addr & 0xFFFFFF);
  return ret;
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL BUP128MBITCs1ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  u32 ret = T1ReadWord(CartridgeArea->dram, addr & 0xFFFFFF);
  return ret;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL BUP128MBITCs1ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  u32 ret = T1ReadLong(CartridgeArea->dram, addr & 0xFFFFFF);
  return ret;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP128MBITCs1WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
  addr &= 0xFFFFFF;
  T1WriteByte(CartridgeArea->dram, addr, val);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP128MBITCs1WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
  addr &= 0xFFFFFF;
  T1WriteWord(CartridgeArea->dram, addr, val);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BUP128MBITCs1WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
  addr &= 0xFFFFFF;
  T1WriteLong(CartridgeArea->dram, addr, val);
}


//////////////////////////////////////////////////////////////////////////////
// 16 Mbit Rom
//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL ROM16MBITCs0ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   return T1ReadByte(CartridgeArea->rom, addr & 0x1FFFFF);
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL ROM16MBITCs0ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   return T1ReadWord(CartridgeArea->rom, addr & 0x1FFFFF);
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL ROM16MBITCs0ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   return T1ReadLong(CartridgeArea->rom, addr & 0x1FFFFF);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL ROM16MBITCs0WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   T1WriteByte(CartridgeArea->rom, addr & 0x1FFFFF, val);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL ROM16MBITCs0WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   T1WriteWord(CartridgeArea->rom, addr & 0x1FFFFF, val);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL ROM16MBITCs0WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   T1WriteLong(CartridgeArea->rom, addr & 0x1FFFFF, val);
}

//////////////////////////////////////////////////////////////////////////////
// 256 Mbit ST-V Rom
//////////////////////////////////////////////////////////////////////////////

/* 315-5838/317-0229 (Decathlete) register window: within any of the 3
 * bank-switched 8MB regions of CS0 (bank = (addr>>23) & 0x3, matching
 * MAME's m_protbankval), the last 16 bytes are special:
 *   ...FFFF0  srcaddr_w  (compressed data source address)
 *   ...FFFF4  data_w     (upper half = mode select, lower half = table/
 *                          dictionary payload)
 *   ...FFFF8  data_r     (streamed decompressed output)
 * Everything else in CS0 is unaffected and falls through to normal ROM
 * access, exactly like before this device existed. */
#define DECATHLT_REG_OFFSET(addr) ((addr) & 0x7FFFFF)

static u8 FASTCALL ROMSTVCs0ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   return T1ReadByte(CartridgeArea->rom, addr & 0x1FFFFFF);
}

//////////////////////////////////////////////////////////////////////////////

static u16 FASTCALL ROMSTVCs0ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   /* MAME maps the 32-bit data register at ...FFF8, so BOTH halves
      (FFF8 and FFFA) hit the decoder, not only the first one. */
   if (decathleteProtEnabled && decathlt5838DecoderEnabled() &&
       (DECATHLT_REG_OFFSET(addr) == 0x7FFFF8 || DECATHLT_REG_OFFSET(addr) == 0x7FFFFA))
   {
      return decathlt5838DataRead();
   }
   u16 ret = T1ReadWord(CartridgeArea->rom, addr & 0x1FFFFFF);
   return ret;
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL ROMSTVCs0ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   if (decathleteProtEnabled && decathlt5838DecoderEnabled() && DECATHLT_REG_OFFSET(addr) == 0x7FFFF8)
   {
      u16 hi = decathlt5838DataRead();
      u16 lo = decathlt5838DataRead();
      return ((u32)hi << 16) | lo;
   }
   u32 ret = T1ReadLong(CartridgeArea->rom, addr & 0x1FFFFFF);
   return ret;
}

//////////////////////////////////////////////////////////////////////////////

/* The ST-V cartridge is ROM: MAME maps 0x02000000-0x04FFFFFF with
   .rom().region("abus") and no write handler, so a write that is not
   claimed by a protection/sound chip below is lost. Writing into
   CartridgeArea->rom made the ROM writable: a later read returned the
   written value instead of the ROM data. */
static void FASTCALL ROMSTVCs0WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
   (void)context; (void)addr; (void)val;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL ROMSTVCs0WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
   if (decathleteProtEnabled)
   {
      u32 offs = DECATHLT_REG_OFFSET(addr);
      decathlt5838SetBank(addr);
      if (offs == 0x7FFFF0) { decathlt5838SetSrcAddr((u32)val << 16, 0xFFFF0000); return; }
      if (offs == 0x7FFFF2) { decathlt5838SetSrcAddr((u32)val,       0x0000FFFF); return; }
      if (offs == 0x7FFFF4) { decathlt5838SetTableUploadMode(val); return; }
      if (offs == 0x7FFFF6) { decathlt5838UploadTableData(val); return; }
   }
   /* ROM: write lost (see ROMSTVCs0WriteByte) */
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL ROMSTVCs0WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
   if (decathleteProtEnabled)
   {
      u32 offs = DECATHLT_REG_OFFSET(addr);
      decathlt5838SetBank(addr);
      if (offs == 0x7FFFF0) { decathlt5838SetSrcAddr(val, 0xFFFFFFFF); return; }
      if (offs == 0x7FFFF4)
      {
         decathlt5838SetTableUploadMode((u16)(val >> 16));
         decathlt5838UploadTableData((u16)(val & 0xFFFF));
         return;
      }
   }
   /* ROM: write lost (see ROMSTVCs0WriteByte) */
}

//////////////////////////////////////////////////////////////////////////////

static u8 FASTCALL ROMSTVCs1ReadByte(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
   return T1ReadByte(&CartridgeArea->rom[0x2000000], addr & 0xFFFFFF);
}

//////////////////////////////////////////////////////////////////////////////

/* Common ST-V 315-5881 protection block. Sur le vrai hardware/MAME, ce
   bloc de registres n'est mappé que sur les 16 derniers octets de la
   fenêtre CS1 (0xFFFFF0-0xFFFFFF) - voir MAME stv.cpp,
   install_common_protection() installé à 0x4fffff0-0x4ffffff.
   Toute autre adresse est de la ROM cartouche normale. Sans cette
   restriction, l'interception tombait sur 1 lecture/écriture sur 16
   dans TOUT le cart, pour TOUS les jeux ST-V passant par ce handler -
   corrompant les données ROM des jeux qui n'ont pas cette puce
   (ex: Critter Crusher bloque sur SYSTEM CHECKING). */
#define STV_PROT_WINDOW_BASE 0xFFFFF0

static INLINE int STVAddrInProtWindow(u32 addr)
{
  return (addr & 0xFFFFFF) >= STV_PROT_WINDOW_BASE;
}

static u16 FASTCALL ROMSTVCs1ReadWord(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  if (STVAddrInProtWindow(addr))
  {
    u8 decryptCmd = addr & 0xF;
    switch (decryptCmd)
    {
      case 0x1: return protRegEnable;
      case 0x8: return protRegLowAddr;
      case 0xa: return protRegHighAddr;
      case 0xc: return protRegSubkey;
      default: break;
    }
  }
  return T1ReadWord(&CartridgeArea->rom[0x2000000], addr & 0xFFFFFF);
}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL ROMSTVCs1ReadLong(SH2_struct *context, UNUSED u8* memory, u32 addr)
{
  LOGSTV("%s %x\n", __FUNCTION__, addr);

  if (STVAddrInProtWindow(addr))
  {
    u8 decryptCmd = addr & 0xF;
    /* Gating sur decryptOn restauré : le vrai bug (le mot d'activation
       jamais posé sur une écriture 16 bits) est maintenant corrigé dans
       ROMSTVCs1WriteWord (decryptCmd == 0x0). Le hack inconditionnel
       n'est donc plus nécessaire et est retiré : il cassait toute
       lecture 32 bits légitime finissant par 0xC dans les jeux qui
       n'activent jamais decryptOn (ex: Critter Crusher). */
    if (decryptCmd == 0xc && (decryptOn & 0x1))
    {
      u16 res = cryptoDecrypt();
      u16 res2 = cryptoDecrypt();
      res = ((res & 0xff00) >> 8) | ((res & 0x00ff) << 8);
      res2 = ((res2 & 0xff00) >> 8) | ((res2 & 0x00ff) << 8);
      return res2 | (res << 16);
    }
    else if (decryptOn & 0x1)
    {
      if (decryptCmd == 0x0) return protRegEnable;
      else if (decryptCmd == 0x8) return (protRegLowAddr << 16) | protRegHighAddr;
    }
  }
  return T1ReadLong(&CartridgeArea->rom[0x2000000], addr & 0xFFFFFF);
}

//////////////////////////////////////////////////////////////////////////////

/* Batman Forever: sound latch of the Acclaim RAX board at 0x04800000-3
   (MAME stv.cpp init_batmanfr / batmanfr_sound_comms_w). Only the upper
   16 bits of the 32-bit register reach the board (data_w(data >> 16));
   the lower half is only logged by MAME. Reads are left to the cart ROM,
   MAME installs a write handler only. */
#define RAX_LATCH_ADDR 0x800000

static INLINE int STVRaxLatchAddr(u32 addr)
{
  return RaxIsEnabled() && (addr & 0xFFFFFC) == RAX_LATCH_ADDR;
}

static void FASTCALL ROMSTVCs1WriteByte(SH2_struct *context, UNUSED u8* memory, u32 addr, u8 val)
{
  LOGSTV("%s %x=%x\n", __FUNCTION__, addr,val);
  if (STVRaxLatchAddr(addr))
  {
    /* bits 31-24 / 23-16 of the big-endian long; MAME passes the whole
       upper word with the other byte lane at 0 */
    if ((addr & 3) == 0) RaxHostWrite((u16)val << 8);
    else if ((addr & 3) == 1) RaxHostWrite(val);
    return;
  }
  if (STVAddrInProtWindow(addr))
  {
    u8 decryptCmd = addr & 0xF;
    /* Byte lanes of the 32-bit enable register (MAME common_prot_w,
       COMBINE_DATA on m_abus_protenable): 0xFFFFF0 = bits 31-24,
       0xFFFFF1 = bits 23-16, which holds the enable bit 16. The test used
       to apply to every CS1 address ending in 1, so any byte write there
       was swallowed and toggled decryptOn. */
    if (decryptCmd == 0x0)
    {
      protRegEnable = (u16)((protRegEnable & 0x00FF) | ((u16)val << 8));
      return;
    }
    if (decryptCmd == 0x1)
    {
      protRegEnable = (u16)((protRegEnable & 0xFF00) | val);
      decryptOn = val & 0x1;
      return;
    }
    /* bus echo of the other window registers, as before */
    T1WriteByte(&CartridgeArea->rom[0x2000000], addr & 0xFFFFFF, val);
  }
  /* outside the window: ROM, write lost (see ROMSTVCs0WriteByte) */
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL ROMSTVCs1WriteWord(SH2_struct *context, UNUSED u8* memory, u32 addr, u16 val)
{
  LOGSTV("%s %x=%x\n", __FUNCTION__, addr, val);

  if (STVRaxLatchAddr(addr))
  {
    if ((addr & 2) == 0) RaxHostWrite(val);   /* upper word */
    return;
  }

  /* 315-5838 family control register at 0x04200000 (MAME
     sega5838_control_w): Name Club selects plain ROM reads with 1 and the
     decoder output with 2. */
  if (decathleteProtEnabled && (addr & 0xFFFFFF) == 0x200000)
  {
    decathlt5838SetControl(val);
    return;
  }

  if (STVAddrInProtWindow(addr))
  {
    u8 decryptCmd = addr & 0xF;
    if (decryptCmd == 0x0)
    {
      /* decryptCmd == 0x0 et non 0x1 : addr & 0xF ne peut jamais valoir
         0x1 pour un accès 16 bits aligné (toujours pair) - c'était du
         code mort, decryptOn n'était jamais activé par une écriture
         mot. Sur ce SH2 big-endian, le bit d'activation (bit
         0x10000 du registre 32 bits complet côté MAME) tombe dans le
         premier mot 16 bits du registre, donc pas de décalage
         supplémentaire ici, contrairement à WriteLong. */
      decryptOn = val & 0x1;
      protRegEnable = val;
      return;
    }
    else if (decryptCmd == 0x8)
    {
      cyptoSetLowAddr(val);
      protRegLowAddr = val;
      return;
    }
    else if (decryptCmd == 0xa)
    {
      cyptoSetHighAddr(val);
      protRegHighAddr = val;
      return;
    }
    else if (decryptCmd == 0xc)
    {
      cyptoSetSubkey(val);
      protRegSubkey = val;
      return;
    }
    /* Other window offsets: kept as a bus echo, MAME common_prot_r returns
       m_a_bus[] (the last value written) there. This 16-byte window is
       the only part of the cart that still takes writes. */
    T1WriteWord(&CartridgeArea->rom[0x2000000], addr & 0xFFFFFF, val);
  }
  /* outside the window: ROM, write lost (see ROMSTVCs0WriteByte) */
}
//////////////////////////////////////////////////////////////////////////////
static void FASTCALL ROMSTVCs1WriteLong(SH2_struct *context, UNUSED u8* memory, u32 addr, u32 val)
{
  LOGSTV("%s %x=%x\n", __FUNCTION__, addr, val);

  if (STVRaxLatchAddr(addr))
  {
    RaxHostWrite((u16)(val >> 16));
    return;
  }

  if (decathleteProtEnabled && (addr & 0xFFFFFF) == 0x200000)
  {
    decathlt5838SetControl((u16)(val >> 16)); /* big-endian: first halfword */
    return;
  }

  if (STVAddrInProtWindow(addr))
  {
    u8 decryptCmd = addr & 0xF;
    /* Mirroring MAME's common_prot_w : une écriture 32 bits sur le
       registre d'activation ou d'adresse doit mettre à jour les deux
       moitiés du registre en un seul accès (ACCESSING_BITS_16_31 /
       ACCESSING_BITS_0_15), pas être silencieusement ignorée. */
    if (decryptCmd == 0x0)
    {
      decryptOn = (val >> 16) & 0x1;
      protRegEnable = (u16)(val >> 16);
      return;
    }
    else if (decryptCmd == 0x8)
    {
      cyptoSetLowAddr(val >> 16);
      cyptoSetHighAddr(val & 0xFFFF);
      protRegLowAddr = (u16)(val >> 16);
      protRegHighAddr = (u16)(val & 0xFFFF);
      return;
    }
    else if (decryptCmd == 0xc)
    {
      cyptoSetSubkey(val >> 16);
      protRegSubkey = (u16)(val >> 16);
      return;
    }
    /* bus echo, see ROMSTVCs1WriteWord */
    T1WriteLong(&CartridgeArea->rom[0x2000000], addr & 0xFFFFFF, val);
  }
  /* outside the window: ROM, write lost (see ROMSTVCs0WriteByte) */
}

//////////////////////////////////////////////////////////////////////////////
// General Cart functions
//////////////////////////////////////////////////////////////////////////////

/* Chargement de la cartouche de sauvegarde : meme protection que la RAM
 * interne (memory.c, BackupLoadFile()). Un fichier present mais illisible
 * provoquait un formatage puis son remplacement par la memoire vide a la
 * premiere ecriture (CartFlush()/CartDeInit()). */
static int cartBupSaveDisabled = 0;

static void CartBupLoad(u8 *bupram, u32 size, const char *filename)
{
   int status = BackupLoadFile(bupram, size, filename, 0);
   cartBupSaveDisabled = 0;
   if (status < 0) {
      FormatBackupRam(bupram, size);
      if (status == -2)
         cartBupSaveDisabled = 1;
   }
}

static void CartBupSave(u8 *bupram, u32 size, const char *filename)
{
   if (cartBupSaveDisabled) {
      YuiMsg("Backup cartridge: %s was not readable at start-up, not overwritten\n", filename ? filename : "");
      return;
   }
   /* Ecriture atomique (fichier temporaire puis renommage, version
    * precedente gardee en .bak) : voir BackupSaveFile(), memory.c. */
   if (BackupSaveFile(bupram, size, filename) != 0)
      YabSetError(YAB_ERR_FILEWRITE, (void *)filename);
}

int CartInit(const char * filename, int type)
{
   if ((CartridgeArea = (cartridge_struct *)calloc(1, sizeof(cartridge_struct))) == NULL)
      return -1;

   CartridgeArea->carttype = type;
   CartridgeArea->filename = filename;

   // Setup default mappings
   CartridgeArea->Cs0ReadByte = &DummyCs0ReadByte;
   CartridgeArea->Cs0ReadWord = &DummyCs0ReadWord;
   CartridgeArea->Cs0ReadLong = &DummyCs0ReadLong;
   CartridgeArea->Cs0WriteByte = &DummyCs0WriteByte;
   CartridgeArea->Cs0WriteWord = &DummyCs0WriteWord;
   CartridgeArea->Cs0WriteLong = &DummyCs0WriteLong;

   CartridgeArea->Cs1ReadByte = &DummyCs1ReadByte;
   CartridgeArea->Cs1ReadWord = &DummyCs1ReadWord;
   CartridgeArea->Cs1ReadLong = &DummyCs1ReadLong;
   CartridgeArea->Cs1WriteByte = &DummyCs1WriteByte;
   CartridgeArea->Cs1WriteWord = &DummyCs1WriteWord;
   CartridgeArea->Cs1WriteLong = &DummyCs1WriteLong;

   CartridgeArea->Cs2ReadByte = &DummyCs2ReadByte;
   CartridgeArea->Cs2ReadWord = &DummyCs2ReadWord;
   CartridgeArea->Cs2ReadLong = &DummyCs2ReadLong;
   CartridgeArea->Cs2WriteByte = &DummyCs2WriteByte;
   CartridgeArea->Cs2WriteWord = &DummyCs2WriteWord;
   CartridgeArea->Cs2WriteLong = &DummyCs2WriteLong;

   decryptOn = 0;

   switch(type)
   {
      case CART_PAR: // Action Replay 4M Plus(or equivalent)
      {
         if ((CartridgeArea->rom = T2MemoryInit(0x40000)) == NULL)
            return -1;

         if ((CartridgeArea->dram = T1MemoryInit(0x400000)) == NULL)
            return -1;

         // Use 32 Mbit Dram id
         CartridgeArea->cartid = 0x5C;

         // Load AR firmware to memory
         if (T123Load(CartridgeArea->rom, 0x40000, 2, filename) != 0)
            return -1;

         vendorid = 0x1F;
         deviceid = 0xD5;
         flstate0 = FL_READ;
         flstate1 = FL_READ;

         // Setup Functions
         CartridgeArea->Cs0ReadByte = &AR4MCs0ReadByte;
         CartridgeArea->Cs0ReadWord = &AR4MCs0ReadWord;
         CartridgeArea->Cs0ReadLong = &AR4MCs0ReadLong;
         CartridgeArea->Cs0WriteByte = &AR4MCs0WriteByte;
         CartridgeArea->Cs0WriteWord = &AR4MCs0WriteWord;
         CartridgeArea->Cs0WriteLong = &AR4MCs0WriteLong;
         break;
      }
      case CART_BACKUPRAM4MBIT: // 4 Mbit Backup Ram
      {
         if ((CartridgeArea->bupram = T1MemoryInit(CART_BUP4MBIT_SIZE)) == NULL)
            return -1;

         CartridgeArea->cartid = 0x21;

         // Load Backup Ram data from file
         CartBupLoad(CartridgeArea->bupram, CART_BUP4MBIT_SIZE, filename);

         // Setup Functions
         CartridgeArea->Cs1ReadByte = &BUP4MBITCs1ReadByte;
         CartridgeArea->Cs1ReadWord = &BUP4MBITCs1ReadWord;
         CartridgeArea->Cs1ReadLong = &BUP4MBITCs1ReadLong;
         CartridgeArea->Cs1WriteByte = &BUP4MBITCs1WriteByte;
         CartridgeArea->Cs1WriteWord = &BUP4MBITCs1WriteWord;
         CartridgeArea->Cs1WriteLong = &BUP4MBITCs1WriteLong;
         break;
      }
      case CART_BACKUPRAM8MBIT: // 8 Mbit Backup Ram
      {
         if ((CartridgeArea->bupram = T1MemoryInit(CART_BUP8MBIT_SIZE)) == NULL)
            return -1;

         CartridgeArea->cartid = 0x22;

         // Load Backup Ram data from file
         CartBupLoad(CartridgeArea->bupram, CART_BUP8MBIT_SIZE, filename);

         // Setup Functions
         CartridgeArea->Cs1ReadByte = &BUP8MBITCs1ReadByte;
         CartridgeArea->Cs1ReadWord = &BUP8MBITCs1ReadWord;
         CartridgeArea->Cs1ReadLong = &BUP8MBITCs1ReadLong;
         CartridgeArea->Cs1WriteByte = &BUP8MBITCs1WriteByte;
         CartridgeArea->Cs1WriteWord = &BUP8MBITCs1WriteWord;
         CartridgeArea->Cs1WriteLong = &BUP8MBITCs1WriteLong;
         break;
      }
      case CART_BACKUPRAM16MBIT: // 16 Mbit Backup Ram
      {
         if ((CartridgeArea->bupram = T1MemoryInit(CART_BUP16MBIT_SIZE)) == NULL)
            return -1;

         CartridgeArea->cartid = 0x23;

         // Load Backup Ram data from file
         CartBupLoad(CartridgeArea->bupram, CART_BUP16MBIT_SIZE, filename);

         // Setup Functions
         CartridgeArea->Cs1ReadByte = &BUP16MBITCs1ReadByte;
         CartridgeArea->Cs1ReadWord = &BUP16MBITCs1ReadWord;
         CartridgeArea->Cs1ReadLong = &BUP16MBITCs1ReadLong;
         CartridgeArea->Cs1WriteByte = &BUP16MBITCs1WriteByte;
         CartridgeArea->Cs1WriteWord = &BUP16MBITCs1WriteWord;
         CartridgeArea->Cs1WriteLong = &BUP16MBITCs1WriteLong;
         break;
      }
      case CART_BACKUPRAM32MBIT: // 32 Mbit Backup Ram
      {
         if ((CartridgeArea->bupram = T1MemoryInit(CART_BUP32MBIT_SIZE)) == NULL)
            return -1;

         CartridgeArea->cartid = 0x24;
         // Load Backup Ram data from file
         CartBupLoad(CartridgeArea->bupram, CART_BUP32MBIT_SIZE, filename);
         // Setup Functions
         CartridgeArea->Cs1ReadByte = &BUP32MBITCs1ReadByte;
         CartridgeArea->Cs1ReadWord = &BUP32MBITCs1ReadWord;
         CartridgeArea->Cs1ReadLong = &BUP32MBITCs1ReadLong;
         CartridgeArea->Cs1WriteByte = &BUP32MBITCs1WriteByte;
         CartridgeArea->Cs1WriteWord = &BUP32MBITCs1WriteWord;
         CartridgeArea->Cs1WriteLong = &BUP32MBITCs1WriteLong;
         break;
      }
      case CART_DRAM128MBIT: // 128 Mbit Ram for development
      //Required by Heart of darkness
      {
        if ((CartridgeArea->dram = T1MemoryInit(CART_DRAM128MBIT_SIZE)) == NULL)
           return -1;

        CartridgeArea->cartid = 0xFF; // I have no idea what the real id is
        // Setup Functions
        CartridgeArea->Cs1ReadByte = &BUP128MBITCs1ReadByte;
        CartridgeArea->Cs1ReadWord = &BUP128MBITCs1ReadWord;
        CartridgeArea->Cs1ReadLong = &BUP128MBITCs1ReadLong;
        CartridgeArea->Cs1WriteByte = &BUP128MBITCs1WriteByte;
        CartridgeArea->Cs1WriteWord = &BUP128MBITCs1WriteWord;
        CartridgeArea->Cs1WriteLong = &BUP128MBITCs1WriteLong;
         break;
      }
      case CART_DRAM8MBIT: // 8 Mbit Dram Cart
      {
         if ((CartridgeArea->dram = T1MemoryInit(0x100000)) == NULL)
            return -1;

         CartridgeArea->cartid = 0x5A;

         // Setup Functions
         CartridgeArea->Cs0ReadByte = &DRAM8MBITCs0ReadByte;
         CartridgeArea->Cs0ReadWord = &DRAM8MBITCs0ReadWord;
         CartridgeArea->Cs0ReadLong = &DRAM8MBITCs0ReadLong;
         CartridgeArea->Cs0WriteByte = &DRAM8MBITCs0WriteByte;
         CartridgeArea->Cs0WriteWord = &DRAM8MBITCs0WriteWord;
         CartridgeArea->Cs0WriteLong = &DRAM8MBITCs0WriteLong;
         break;
      }
      case CART_DRAM32MBIT: // 32 Mbit Dram Cart
      {
         if ((CartridgeArea->dram = T1MemoryInit(0x400000)) == NULL)
            return -1;

         CartridgeArea->cartid = 0x5C;

         // Setup Functions
         CartridgeArea->Cs0ReadByte = &DRAM32MBITCs0ReadByte;
         CartridgeArea->Cs0ReadWord = &DRAM32MBITCs0ReadWord;
         CartridgeArea->Cs0ReadLong = &DRAM32MBITCs0ReadLong;
         CartridgeArea->Cs0WriteByte = &DRAM32MBITCs0WriteByte;
         CartridgeArea->Cs0WriteWord = &DRAM32MBITCs0WriteWord;
         CartridgeArea->Cs0WriteLong = &DRAM32MBITCs0WriteLong;
         break;
      }
      case CART_NETLINK:
      {
         CartridgeArea->cartid = 0xFF;
         CartridgeArea->Cs2ReadByte = &NetlinkReadByte;
         CartridgeArea->Cs2WriteByte = &NetlinkWriteByte;
         break;
      }
      case CART_MPEGCARD: // Video CD Card / MPEG Card
      {
         // Unlike the RAM/ROM carts above, the Video CD Card does not
         // expose anything on the CS0 memory-mapped cartridge bus: per the
         // Yabause wiki (MPEGCard page), "All communication with the
         // device goes through the CD Block using commands" (CR1-CR4,
         // commands 0x90-0xAF/0xE0-0xE2 handled in cs2.c). So there is
         // nothing to wire up here beyond marking a cart as present --
         // Cs2.c/bios.c gate the actual MPEG/VCD behaviour on
         // CartridgeArea->carttype == CART_MPEGCARD directly.
         CartridgeArea->cartid = 0xFF; // real id unknown/undocumented
         break;
      }
      case CART_ROM16MBIT: // 16 Mbit Rom Cart
      {
         if ((CartridgeArea->rom = T1MemoryInit(0x200000)) == NULL)
            return -1;

         CartridgeArea->cartid = 0xFF; // I have no idea what the real id is

         // Load Rom to memory
         if (T123Load(CartridgeArea->rom, 0x200000, 1, filename) != 0)
            return -1;

         // Setup Functions
         CartridgeArea->Cs0ReadByte = &ROM16MBITCs0ReadByte;
         CartridgeArea->Cs0ReadWord = &ROM16MBITCs0ReadWord;
         CartridgeArea->Cs0ReadLong = &ROM16MBITCs0ReadLong;
         CartridgeArea->Cs0WriteByte = &ROM16MBITCs0WriteByte;
         CartridgeArea->Cs0WriteWord = &ROM16MBITCs0WriteWord;
         CartridgeArea->Cs0WriteLong = &ROM16MBITCs0WriteLong;
         break;
      }
      case CART_ROMSTV: // 256 Mbit STV Rom Cart
      {
         if ((CartridgeArea->rom = T1MemoryInit(0x3000000)) == NULL)
            return -1;
         CartridgeArea->cartid = 0xFF; // I have no idea what the real id is
         // Setup Functions
         CartridgeArea->Cs0ReadByte = &ROMSTVCs0ReadByte;
         CartridgeArea->Cs0ReadWord = &ROMSTVCs0ReadWord;
         CartridgeArea->Cs0ReadLong = &ROMSTVCs0ReadLong;
         CartridgeArea->Cs0WriteByte = &ROMSTVCs0WriteByte;
         CartridgeArea->Cs0WriteWord = &ROMSTVCs0WriteWord;
         CartridgeArea->Cs0WriteLong = &ROMSTVCs0WriteLong;

         CartridgeArea->Cs1ReadByte = &ROMSTVCs1ReadByte;
         CartridgeArea->Cs1ReadWord = &ROMSTVCs1ReadWord;
         CartridgeArea->Cs1ReadLong = &ROMSTVCs1ReadLong;
         CartridgeArea->Cs1WriteByte = &ROMSTVCs1WriteByte;
         CartridgeArea->Cs1WriteWord = &ROMSTVCs1WriteWord;
         CartridgeArea->Cs1WriteLong = &ROMSTVCs1WriteLong;
         break;
      }
      case CART_JAPMODEM: // Sega Saturn Modem(Japanese)
      {
         CartridgeArea->cartid = 0xFF;

         CartridgeArea->Cs0ReadByte = &JapModemCs0ReadByte;
         CartridgeArea->Cs0ReadWord = &JapModemCs0ReadWord;
         CartridgeArea->Cs0ReadLong = &JapModemCs0ReadLong;

         CartridgeArea->Cs2ReadByte = &JapModemCs2ReadByte;
         CartridgeArea->Cs2WriteByte = &JapModemCs2WriteByte;
         break;
      }
      case CART_USBDEV: // USB Dev Cartridge
      {
         if ((CartridgeArea->rom = T2MemoryInit(0x40000)) == NULL)
            return -1;

         if ((CartridgeArea->dram = T1MemoryInit(0x400000)) == NULL)
            return -1;

         // No extra dram, etc. built-in
         CartridgeArea->cartid = 0;

         // Load AR firmware to memory
         if (T123Load(CartridgeArea->rom, 0x40000, 2, filename) != 0)
            return -1;

         // ID for SST39SF010A
         vendorid = 0xBF;
         deviceid = 0xB5;

         flstate0 = FL_READ;
         flstate1 = FL_READ;

         // Setup Functions
         CartridgeArea->Cs0ReadByte = &AR4MCs0ReadByte;
         CartridgeArea->Cs0ReadWord = &AR4MCs0ReadWord;
         CartridgeArea->Cs0ReadLong = &AR4MCs0ReadLong;
         CartridgeArea->Cs0WriteByte = &AR4MCs0WriteByte;
         CartridgeArea->Cs0WriteWord = &AR4MCs0WriteWord;
         CartridgeArea->Cs0WriteLong = &AR4MCs0WriteLong;
         break;
      }
      case CART_DEV:
      {
        // Setup Functions
        CartridgeArea->Cs1ReadByte = &DevCs1ReadByte;
        CartridgeArea->Cs1ReadWord = &DevCs1ReadWord;
        CartridgeArea->Cs1ReadLong = &DevCs1ReadLong;
        CartridgeArea->Cs1WriteByte = &DevCs1WriteByte;
        CartridgeArea->Cs1WriteWord = &DevCs1WriteWord;
        /* FIX: this assignment used to sit after the case's break/return
           path (as dead code following an earlier `break;`), so it never
           ran and Cs1WriteLong silently stayed on DummyCs1WriteLong for
           CART_DEV. Moved next to the other Cs1Write* assignments so it
           actually takes effect. */
        CartridgeArea->Cs1WriteLong = &DevCs1WriteLong;

         if ((CartridgeArea->dram = T1MemoryInit(0x400000)) == NULL)
              return -1;

         CartridgeArea->cartid = 0x5C;

         // Setup Functions
         CartridgeArea->Cs0ReadByte = &DRAM32MBITCs0ReadByte;
         CartridgeArea->Cs0ReadWord = &DRAM32MBITCs0ReadWord;
         CartridgeArea->Cs0ReadLong = &DRAM32MBITCs0ReadLong;
         CartridgeArea->Cs0WriteByte = &DRAM32MBITCs0WriteByte;
         CartridgeArea->Cs0WriteWord = &DRAM32MBITCs0WriteWord;
         CartridgeArea->Cs0WriteLong = &DRAM32MBITCs0WriteLong;
         break;
      }

      default: // No Cart
      {
         CartridgeArea->cartid = 0xFF;
         break;
      }
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void CartFlush(void){
   if (CartridgeArea)
   {
      if (CartridgeArea->carttype == CART_PAR)
      {
         if (CartridgeArea->rom)
         {
            if (T123Save(CartridgeArea->rom, 0x40000, 2, CartridgeArea->filename) != 0)
               YabSetError(YAB_ERR_FILEWRITE, (void *)CartridgeArea->filename);
         }
      }

      if (CartridgeArea->bupram)
      {
         u32 size = 0;

         switch (CartridgeArea->carttype)
         {
            case CART_BACKUPRAM4MBIT: // 4 Mbit Backup Ram
            {
               size = CART_BUP4MBIT_SIZE;
               break;
            }
            case CART_BACKUPRAM8MBIT: // 8 Mbit Backup Ram
            {
               size = CART_BUP8MBIT_SIZE;
               break;
            }
            case CART_BACKUPRAM16MBIT: // 16 Mbit Backup Ram
            {
               size = CART_BUP16MBIT_SIZE;
               break;
            }
            case CART_BACKUPRAM32MBIT: // 32 Mbit Backup Ram
            {
               size = CART_BUP32MBIT_SIZE;
               break;
            }
         }

         if (size != 0)
         {
            CartBupSave(CartridgeArea->bupram, size, CartridgeArea->filename);
         }
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

void CartDeInit(void)
{
   if (CartridgeArea)
   {
      if (CartridgeArea->carttype == CART_PAR)
      {
         if (CartridgeArea->rom)
         {
            if (T123Save(CartridgeArea->rom, 0x40000, 2, CartridgeArea->filename) != 0)
               YabSetError(YAB_ERR_FILEWRITE, (void *)CartridgeArea->filename);
            T2MemoryDeInit(CartridgeArea->rom);
         }
      }
      else
      {
         if (CartridgeArea->rom)
            T1MemoryDeInit(CartridgeArea->rom);
      }

      if (CartridgeArea->bupram)
      {
         u32 size=0;

         switch (CartridgeArea->carttype)
         {
            case CART_BACKUPRAM4MBIT: // 4 Mbit Backup Ram
            {
               size = CART_BUP4MBIT_SIZE;
               break;
            }
            case CART_BACKUPRAM8MBIT: // 8 Mbit Backup Ram
            {
               size = CART_BUP8MBIT_SIZE;
               break;
            }
            case CART_BACKUPRAM16MBIT: // 16 Mbit Backup Ram
            {
               size = CART_BUP16MBIT_SIZE;
               break;
            }
            case CART_BACKUPRAM32MBIT: // 32 Mbit Backup Ram
            {
               size = CART_BUP32MBIT_SIZE;
               break;
            }
         }

         if (size != 0)
         {
            CartBupSave(CartridgeArea->bupram, size, CartridgeArea->filename);

            T1MemoryDeInit(CartridgeArea->bupram);
         }
      }

      if (CartridgeArea->dram)
         T1MemoryDeInit(CartridgeArea->dram);

      free(CartridgeArea);
   }
   CartridgeArea = NULL;
}

//////////////////////////////////////////////////////////////////////////////

/* Taille de la DRAM de cartouche a serialiser (0 : rien a sauver).
 * CART_DRAM128MBIT (requise par Heart of Darkness) et CART_DEV ont aussi
 * une DRAM, qui n'etait pas sauvee : apres chargement, le jeu retrouvait le
 * contenu de la session courante au lieu de celui de l'etat. */
static u32 CartStateDramSize(int carttype)
{
   switch (carttype) {
      case CART_DRAM8MBIT:   return 0x100000;
      case CART_DRAM32MBIT:  return 0x400000;
      case CART_DEV:         return 0x400000;
      case CART_DRAM128MBIT: return CART_DRAM128MBIT_SIZE;
      default:               return 0;
   }
}

/* CART chunk version 2:
 *   u32 cart type
 *   u32 DRAM length (0 if none), then the DRAM
 *   ST-V cartridges only: tagged blocks until the end of the chunk, each
 *   4-char tag + u32 length + data, so a block that is unknown, missing or
 *   of another version is skipped without shifting the rest:
 *     "PROT" 315-5881 register window (decryptOn and the bus-echo shadows)
 *     "5881" 315-5881 transfer in progress (decrypt.c)
 *     "5838" 315-5838 family tables and decoder position (decrypt.c)
 *     "IOGA" I/O gate array registers, PORT-G counters, hopper (peripheral.c)
 *     "EEPR" ST-V serial EEPROM: interface state and contents (eeprom.c)
 *     "RAX " Acclaim RAX sound board of Batman Forever (rax.c, adsp2181.c)
 * Version 1 (type + DRAM when the type has one) is still read. Before
 * version 2 nothing of the ST-V protection chips or of the IOGA was saved:
 * a state taken in the middle of a 315-5881/5838 transfer resumed with the
 * session's current position and tables, i.e. wrong data. */
#define CART_STATE_VERSION 2
#define CART_STV_PROT_STATE_VERSION 1
#define CART_STV_PROT_STATE_SIZE (4 + 1 + 4*2)

static void CartStvProtSaveState(void ** stream)
{
   u32 version = CART_STV_PROT_STATE_VERSION;
   MemStateWrite(&version, 4, 1, stream);
   MemStateWrite(&decryptOn, 1, 1, stream);
   MemStateWrite(&protRegEnable, 2, 1, stream);
   MemStateWrite(&protRegLowAddr, 2, 1, stream);
   MemStateWrite(&protRegHighAddr, 2, 1, stream);
   MemStateWrite(&protRegSubkey, 2, 1, stream);
}

static int CartStvProtLoadState(const void * stream, int size)
{
   u32 version;
   if (size < CART_STV_PROT_STATE_SIZE)
      return -1;
   MemStateRead(&version, 4, 1, stream);
   if (version != CART_STV_PROT_STATE_VERSION)
      return -1;
   MemStateRead(&decryptOn, 1, 1, stream);
   MemStateRead(&protRegEnable, 2, 1, stream);
   MemStateRead(&protRegLowAddr, 2, 1, stream);
   MemStateRead(&protRegHighAddr, 2, 1, stream);
   MemStateRead(&protRegSubkey, 2, 1, stream);
   decryptOn &= 1;
   return 0;
}

static void CartStateWriteBlock(void ** stream, const char * tag, void (*writer)(void **))
{
   int lenoffset;
   u32 len = 0;

   MemStateWrite((void *)tag, 1, 4, stream);
   lenoffset = MemStateGetOffset();
   MemStateWrite(&len, 4, 1, stream);   /* place holder */
   writer(stream);
   len = (u32)(MemStateGetOffset() - lenoffset - 4);
   MemStateWriteOffset(&len, 4, 1, stream, lenoffset);
}

static void CartStvLoadBlocks(const void * stream, int chunkend)
{
   while ((chunkend - MemStateGetOffset()) >= 8)
   {
      char tag[4];
      u32 len;
      int blockend, status = -1;

      MemStateRead(tag, 1, 4, stream);
      MemStateRead(&len, 4, 1, stream);
      if (len > (u32)(chunkend - MemStateGetOffset())) {
         YuiMsg("Save state: CART block %.4s truncated, ignored\n", tag);
         break;
      }
      blockend = MemStateGetOffset() + (int)len;

      if (memcmp(tag, "PROT", 4) == 0)
         status = CartStvProtLoadState(stream, (int)len);
      else if (memcmp(tag, "5881", 4) == 0)
         status = cryptoLoadState(stream, (int)len);
      else if (memcmp(tag, "5838", 4) == 0)
         status = decathlt5838LoadState(stream, (int)len);
      else if (memcmp(tag, "IOGA", 4) == 0)
         status = PerIogaLoadState(stream, (int)len);
      else if (memcmp(tag, "EEPR", 4) == 0)
         status = eeprom_load_state(stream, (int)len);
      else if (memcmp(tag, "RAX ", 4) == 0)
         status = RaxLoadState(stream, (int)len);

      if (status != 0)
         YuiMsg("Save state: CART block %.4s not loaded (unknown or other version)\n", tag);
      MemStateSetOffset(blockend);
   }
}

int CartSaveState(void ** stream)
{
   int offset;
   u32 dramsize;

   offset = MemStateWriteHeader(stream, "CART", CART_STATE_VERSION);

   // Write cart type
   MemStateWrite((void *)&CartridgeArea->carttype, 4, 1, stream);

   // Write the areas associated with the cart type here
   dramsize = CartStateDramSize(CartridgeArea->carttype);
   if (CartridgeArea->dram == NULL)
      dramsize = 0;
   MemStateWrite((void *)&dramsize, 4, 1, stream);
   if (dramsize != 0)
      MemStateWrite((void *)CartridgeArea->dram, 1, dramsize, stream);

   if (CartridgeArea->carttype == CART_ROMSTV)
   {
      CartStateWriteBlock(stream, "PROT", CartStvProtSaveState);
      CartStateWriteBlock(stream, "5881", cryptoSaveState);
      CartStateWriteBlock(stream, "5838", decathlt5838SaveState);
      CartStateWriteBlock(stream, "IOGA", PerIogaSaveState);
      CartStateWriteBlock(stream, "EEPR", eeprom_save_state);
      CartStateWriteBlock(stream, "RAX ", RaxSaveState);
   }

   return MemStateFinishHeader(stream, offset);
}

//////////////////////////////////////////////////////////////////////////////

/* Replaces the current cartridge by one of the saved type (it has DRAM). */
static int CartStateChangeType(int newtype)
{
   YuiMsg("Save state cartridge type (%d) differs from the current one (%d): cartridge replaced\n",
          newtype, CartridgeArea->carttype);
   CartDeInit();
   if (CartInit(NULL, newtype) != 0) {
      YuiMsg("Save state: can not create cartridge type %d\n", newtype);
      return -1;
   }
   /* FIX : la zone CS0 de la carte memoire pointait encore sur les
    * accesseurs et sur la structure liberee de l'ancienne cartouche. */
   MappedMemoryRemapCs0();
   return 0;
}

int CartLoadState(const void * stream, int version, int size)
{
   int newtype;
   u32 dramsize;
   int chunkend = MemStateGetOffset() + size;

   // Read cart type
   if (size < 4)
      return size;
   MemStateRead((void *)&newtype, 4, 1, stream);

   if (version < 2)
   {
      dramsize = CartStateDramSize(newtype);

      /* Le bloc doit contenir la DRAM annoncee : les etats ecrits avant l'ajout
       * de CART_DEV / CART_DRAM128MBIT n'ont que le type. */
      if ((dramsize == 0) || (size < (int)(4 + dramsize)))
         return size;

      // Check to see if old cart type and new cart type match, if they don't,
      // reallocate memory areas
      if ((newtype != CartridgeArea->carttype) && (CartStateChangeType(newtype) != 0))
         return size;

      if (CartridgeArea->dram != NULL)
         MemStateRead((void *)CartridgeArea->dram, 1, dramsize, stream);

      return size;
   }

   if ((chunkend - MemStateGetOffset()) < 4)
      return size;
   MemStateRead((void *)&dramsize, 4, 1, stream);
   if (dramsize > (u32)(chunkend - MemStateGetOffset())) {
      YuiMsg("Save state: CART DRAM size %u larger than the chunk, ignored\n", dramsize);
      return size;
   }

   if (dramsize != 0)
   {
      int dramend = MemStateGetOffset() + (int)dramsize;

      if (dramsize == CartStateDramSize(newtype))
      {
         if ((newtype != CartridgeArea->carttype) && (CartStateChangeType(newtype) != 0))
            return size;
         if (CartridgeArea->dram != NULL)
            MemStateRead((void *)CartridgeArea->dram, 1, dramsize, stream);
      }
      MemStateSetOffset(dramend);
   }

   /* ST-V blocks: only meaningful when the running cartridge is the ST-V
      one (the state belongs to the game currently loaded). */
   if ((newtype == CART_ROMSTV) && (CartridgeArea->carttype == CART_ROMSTV))
      CartStvLoadBlocks(stream, chunkend);

   return size;
}

//////////////////////////////////////////////////////////////////////////////
