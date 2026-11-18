/*  Copyright 2013 Theo Berkau

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

/*! \file japmodem.c
    \brief Japanese modem emulation functions.

    The register interface of the Japanese Saturn modem is NOT documented.
    What is emulated was deduced from the code of Dragon's Dream (Japan),
    the only game known to use it:
      - CS0 reads answer FF/A5, CS1 answers the cartridge ID only.
      - CS2 25885029h (offset 85029h), byte read: modem detection, the game
        needs 11h ("modem connected").
      - CS2 25885031h (offset 85031h), byte: control byte (the game sets and
        clears bit 7 by read-modify-write), read back as written.
      - CS2 25895001h-2589501Dh: a 16550 UART at the same addresses as the
        NetLink one, with the A-Bus interrupt vector 5Ch (EXT12). Handed to
        the NetLink emulation (16550 + Hayes modem + TCP line).
      - CS2 2582503Dh: written with 00h after each access to the UART
        (register ignored by the NetLink code).
    Anything else reads FFh and writes are ignored.

    Addresses received by the cartridge handlers are not normalised the same
    way for each chip select: CS1 gets a 24-bit offset (cart ID at FFFFFFh),
    CS2 gets the address without the cache bits (05885029 = A-Bus 25885029).
*/

#include <ctype.h>
#include <stdlib.h>
#include "cs0.h"
#include "cs2.h"
#include "error.h"
#include "japmodem.h"
#include "netlink.h"
#include "debug.h"
#include "sh2core.h"
#include "scu.h"

JapModem *JapModemArea = NULL;

#define JM_REG_STATUS 0x85029
#define JM_STATUS_CONNECTED 0x11
#define JM_REG_CTRL   0x85031

// 16550 registers (25895001h .. 2589501Dh) and the 2582503Dh register
static int JapModemIsUartReg(u32 addr)
{
   u32 a = addr & 0xFFFFF;
   return (a >= 0x95001 && a <= 0x9501D) || a == 0x2503D;
}

static u8 jm_ctrl = 0;

//////////////////////////////////////////////////////////////////////////////
// CS0
//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL JapModemCs0ReadByte(SH2_struct *context, u8* mem, u32 addr)
{
   return (addr & 0x1) ? 0xA5 : 0xFF;
}

u16 FASTCALL JapModemCs0ReadWord(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   return 0xFFA5;
}

u32 FASTCALL JapModemCs0ReadLong(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   return 0xFFA5FFA5;
}

void FASTCALL JapModemCs0WriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val)
{
}

void FASTCALL JapModemCs0WriteWord(SH2_struct *context, u8* mem, u32 addr, u16 val)
{
}

void FASTCALL JapModemCs0WriteLong(SH2_struct *context, u8* mem, u32 addr, u32 val)
{
}

//////////////////////////////////////////////////////////////////////////////
// CS1: same answers as the default (no cartridge) handlers in cs0.c, i.e.
// only the cartridge ID is readable (offset FFFFFFh / FFFFFEh / FFFFFCh).
//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL JapModemCs1ReadByte(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   return (addr == 0xFFFFFF) ? CartridgeArea->cartid : 0xFF;
}

u16 FASTCALL JapModemCs1ReadWord(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   return (addr == 0xFFFFFE) ? (u16)(0xFF00 | CartridgeArea->cartid) : 0xFFFF;
}

u32 FASTCALL JapModemCs1ReadLong(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   return (addr == 0xFFFFFC) ?
      (0xFF00FF00 | ((u32)CartridgeArea->cartid << 16) | CartridgeArea->cartid) : 0xFFFFFFFF;
}

void FASTCALL JapModemCs1WriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val)
{
}

void FASTCALL JapModemCs1WriteWord(SH2_struct *context, u8* mem, u32 addr, u16 val)
{
}

void FASTCALL JapModemCs1WriteLong(SH2_struct *context, u8* mem, u32 addr, u32 val)
{
}

//////////////////////////////////////////////////////////////////////////////
// CS2
//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL JapModemCs2ReadByte(SH2_struct *context, u8* mem, u32 addr)
{
   switch (addr & 0xFFFFF)
   {
      case JM_REG_STATUS:
         return JM_STATUS_CONNECTED;
      case JM_REG_CTRL:
         return jm_ctrl;
      default:
         if (JapModemIsUartReg(addr))
            return NetlinkReadByte(context, mem, addr);
         break;
   }
   return 0xFF;
}

void FASTCALL JapModemCs2WriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val)
{
   if ((addr & 0xFFFFF) == JM_REG_CTRL)
      jm_ctrl = val;
   else if (JapModemIsUartReg(addr))
      NetlinkWriteByte(context, mem, addr, val);
}

//////////////////////////////////////////////////////////////////////////////

int JapModemInit(const char *ip, const char *port)
{
   (void)ip;
   (void)port;

   if (JapModemArea != NULL)
      JapModemDeInit();

   jm_ctrl = 0;

   if ((JapModemArea = (JapModem*)calloc(1, sizeof(JapModem))) == NULL)
   {
      YabSetError(YAB_ERR_CANNOTINIT, (void *)"Japanese Modem");
      return -1;
   }

   // UART + Hayes modem + network line: the NetLink emulation (address and
   // port come from NetlinkSetConfig()). A network failure is not fatal: the
   // modem still answers AT commands. NetlinkExec() is called every
   // deciline by yabause.c and returns at once when NetlinkArea is NULL.
   NetlinkInit(NULL, NULL);

   return 0;
}

void JapModemDeInit(void)
{
   if (JapModemArea != NULL)
   {
      NetlinkDeInit();
      free(JapModemArea);
   }
   JapModemArea = NULL;
}

void JapModemExec(u32 timing)
{
   // Timings are handled by NetlinkExec()
   (void)timing;
}

//////////////////////////////////////////////////////////////////////////////
