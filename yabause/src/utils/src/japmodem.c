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
*/

#include <ctype.h>
#include <stdlib.h>
#include "cs2.h"
#include "error.h"
#include "japmodem.h"
#include "netlink.h"
#include "debug.h"
#include "sh2core.h"
#include "scu.h"

JapModem *JapModemArea = NULL;

u8 FASTCALL JapModemCs0ReadByte(SH2_struct *context, u8* mem, u32 addr)
{
   NETLINK_LOG("%08X Cs0 byte read. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
	if (addr & 0x1)
		return 0xA5;
	else
      return 0xFF;
}

u16 FASTCALL JapModemCs0ReadWord(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   NETLINK_LOG("%08X Cs0 word read. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return 0xFFA5;
}

u32 FASTCALL JapModemCs0ReadLong(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   NETLINK_LOG("%08X Cs0 long read. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return 0xFFA5FFA5;
}

u8 FASTCALL JapModemCs1ReadByte(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   NETLINK_LOG("%08X Cs1 byte read. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return 0xFF;
}

u16 FASTCALL JapModemCs1ReadWord(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   NETLINK_LOG("%08X Cs1 word read. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return 0xFFFF;
}

u32 FASTCALL JapModemCs1ReadLong(SH2_struct *context, u8* mem, UNUSED u32 addr)
{
   NETLINK_LOG("%08X Cs1 long read. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return 0xFFFFFFFF;
}

void FASTCALL JapModemCs1WriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val)
{
   NETLINK_LOG("%08X Cs1 byte write. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return;
}

void FASTCALL JapModemCs1WriteWord(SH2_struct *context, u8* mem, u32 addr, u16 val)
{
   NETLINK_LOG("%08X Cs1 word write. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return;
}

void FASTCALL JapModemCs1WriteLong(SH2_struct *context, u8* mem, u32 addr, u32 val)
{
   NETLINK_LOG("%08X Cs1 long write. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return;
}

u8 FASTCALL JapModemCs2ReadByte(SH2_struct *context, u8* mem, u32 addr)
{
	NETLINK_LOG("%08X Cs2 byte read. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
   return 0xFF;
}

void FASTCALL JapModemCs2WriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val)
{
	NETLINK_LOG("%08X Cs2 byte write. PC = %08X", addr, (context ? context->regs.PC : MSH2->regs.PC));
}

// The register interface of the Japanese Saturn modem is not known: the
// CS0/CS1/CS2 handlers above are only logging stubs and nothing reaches the
// NetLink UART emulation, so no network thread is started for it (it used
// to open the NetLink listening port for nothing, and the struct leaked).
int JapModemInit(const char *ip, const char *port)
{
   (void)ip;
   (void)port;

   if (JapModemArea != NULL)
      JapModemDeInit();

   if ((JapModemArea = (JapModem*)calloc(1, sizeof(JapModem))) == NULL)
   {
      YabSetError(YAB_ERR_CANNOTINIT, (void *)"Japanese Modem");
      return -1;
   }

   return 0;
}

void JapModemDeInit(void)
{
   if (JapModemArea != NULL)
      free(JapModemArea);
   JapModemArea = NULL;
}

void JapModemExec(u32 timing)
{
   (void)timing;
}

//////////////////////////////////////////////////////////////////////////////
