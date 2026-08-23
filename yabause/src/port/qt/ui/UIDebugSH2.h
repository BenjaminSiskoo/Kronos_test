/*	Copyright 2012-2013 Theo Berkau <cwx@cyberwarriorx.com>

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
#ifndef UIDEBUGSH2_H
#define UIDEBUGSH2_H

#include "UIDebugCPU.h"
#include "../QtYabause.h"

/* ------------------------------------------------------------------ */
/* Extended SH2 reporting (implemented in UIDebugSH2.cpp)             */
/*                                                                    */
/* Free functions rather than members, so the automatic snapshot can   */
/* run from the emulation thread with no debug window open.            */
/* ------------------------------------------------------------------ */

/* Full human-readable report: registers + decoded SR, rule-based
   diagnostics, SH2 exception/TRAPA frame detection over the whole live
   stack, stack walk with call-chain reconstruction, stack hex dump,
   disassembly around PC/PR, VBR vector table, cache state and cache/RAM
   coherency check, SH2 on-chip registers, SCU registers and DMA
   channels. */
QString SH2DebugBuildReport(SH2_struct *sh2);

/* Shorter version for the in-window panel: diagnostics + exception
   frames + stack walk + stack hex dump. */
QString SH2DebugBuildStackPanel(SH2_struct *sh2);

/* Writes the full report to
   <dir>/sh2_debug_<MSH2|SSH2>_YYYYMMDD_HHMMSS.txt
   plus a raw <...>_stack.bin snapshot of the stack page.
   An empty dir means the current working directory. */
bool SH2DebugExportToFile(SH2_struct *sh2, const QString &dir, QString *outPath);

/* Directory the reports are written to. Resolution order:
     1. environment variable KRONOS_SH2DUMP_DIR
     2. value pushed in by SH2DebugSetDumpDir() (Settings "Debug/SH2DumpDir")
     3. compiled-in default (E:/Kronos64Bits on Windows, "." elsewhere)
   Created on demand; falls back to the working directory if unwritable. */
QString SH2DebugDumpDir();
void    SH2DebugSetDumpDir(const QString &dir);

class UIDebugSH2 : public UIDebugCPU
{
	Q_OBJECT
private:
   SH2_struct *debugSH2;
   QString addr2line;
   void restoreAddr2line();

   /* When true, the "Other Debug" tab shows the live stack walk and is
      refreshed on every step instead of the addr2line source view. */
   bool stackPanelPinned = false;

public:
   UIDebugSH2( UIDebugCPU::PROCTYPE proc, YabauseThread *mYabauseThread, QWidget* parent = 0 );
   void updateRegList();
   void updateCodeList(u32 addr);
   void updateCodePage(u32 evaluateAddress) override;
   void updateBackTrace();
   void updateTrackInfLoop();
   void updateAll();
   u32 getRegister(int index, int *size);
   void setRegister(int index, u32 value);
   bool addCodeBreakpoint(u32 addr);
   bool delCodeBreakpoint(u32 addr);
   bool addMemoryBreakpoint(u32 addr, u32 flags);
   bool delMemoryBreakpoint(u32 addr);
   u32 getMemoryBreakpointFlags(u32 addr);
   void stepInto();
   void stepOver();
   void stepOut();
   void reserved1();
   void reserved2();
	void reserved3();
   void reserved4();   /* Export Debug */
   void reserved5();   /* Stack Walk   */
protected:

protected slots:
	void loadCodeAddress();
};

#endif // UIDEBUGSH2_H
