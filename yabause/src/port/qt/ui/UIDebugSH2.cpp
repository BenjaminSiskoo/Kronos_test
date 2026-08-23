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

#include "Settings.h"
#include "UIDebugSH2.h"
#include "../CommonDialogs.h"
#include "UIYabause.h"
#include "VolatileSettings.h"
#include <QProcess>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QFileInfo>
#include <QDir>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <string.h>

int SH2Dis(SH2_struct *context, u32 addr, char *string)
{
   SH2Disasm(addr, SH2MappedMemoryReadWord(context, addr), 0, NULL, string);
   return 2;
}

/* ------------------------------------------------------------------ */
/* Extended SH2 reporting: stack walk, exception frame, cache, export */
/*                                                                    */
/* Everything between this banner and the matching END banner is self  */
/* contained: it only relies on SH2GetRegisters / SH2MappedMemoryRead* */
/* / SH2Disasm plus the HighWram-LowWram globals.                      */
/* ------------------------------------------------------------------ */

/* Where the reports land. Resolution order:
     1. the environment variable KRONOS_SH2DUMP_DIR
     2. whatever the GUI pushed in via SH2DebugSetDumpDir() (Settings key
        "Debug/SH2DumpDir")
     3. the compiled-in default below

   The cached QString exists so the automatic snapshot, which runs on the
   emulation thread, never has to touch QSettings -- QSettings is not
   thread-safe for a shared instance. */
#ifndef SH2_DUMP_DEFAULT_DIR
# ifdef Q_OS_WIN
#  define SH2_DUMP_DEFAULT_DIR "E:/Kronos64Bits"
# else
#  define SH2_DUMP_DEFAULT_DIR "."
# endif
#endif

static QString sh2DumpDirCached;

QString SH2DebugDumpDir()
{
    QByteArray env = qgetenv("KRONOS_SH2DUMP_DIR");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    if (!sh2DumpDirCached.isEmpty())
        return sh2DumpDirCached;
    return QString::fromLatin1(SH2_DUMP_DEFAULT_DIR);
}

void SH2DebugSetDumpDir(const QString &dir)
{
    sh2DumpDirCached = dir;
}

namespace {

struct SH2ReportOptions
{
    u32  stackBytesBelowSP;   /* how far below SP to dump   */
    u32  stackBytesAboveSP;   /* how far above SP to dump   */
    u32  disasmBytesAroundPC; /* window around PC           */
    int  maxFrames;           /* max reconstructed frames   */
    bool dumpCacheCheck;
    bool dumpOnchip;
    bool dumpScu;
    bool dumpRawBin;

    SH2ReportOptions()
        : stackBytesBelowSP(0x80)
        , stackBytesAboveSP(0x200)
        , disasmBytesAroundPC(0x40)
        , maxFrames(32)
        , dumpCacheCheck(true)
        , dumpOnchip(true)
        , dumpScu(true)
        , dumpRawBin(false)
    {}
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static QString hex32(u32 v)  { return QString("%1").arg(v, 8, 16, QChar('0')).toUpper(); }
static QString hex16(u32 v)  { return QString("%1").arg(v & 0xFFFF, 4, 16, QChar('0')).toUpper(); }
static QString hex8 (u32 v)  { return QString("%1").arg(v & 0xFF,   2, 16, QChar('0')).toUpper(); }

/* Strip the SH2 address-space partition bits so 0x06xxxxxx, 0x26xxxxxx
   (cache-through), 0x46xxxxxx (associative purge) and 0x66xxxxxx (address
   array) all normalise to the same physical location. */
static u32 physOf(u32 a) { return a & 0x0FFFFFFF; }

/* Left-aligned padding helper. Avoids relying on implicit const char*
   -> QString conversion, which breaks under QT_NO_CAST_FROM_ASCII. */
static QString pad(const char *s, int width)
{ return QString("%1").arg(QString::fromLatin1(s), width); }

/* Human name of the memory region an address belongs to. */
static const char *regionName(u32 a)
{
    if (a == 0xFFFFFFFF)                      return "-1 / invalid marker";
    if (a <  0x00001000)                      return "immediate / not a pointer";
    u32 p = physOf(a);
    if (p < 0x00100000)                       return "Boot ROM";
    if (p >= 0x00100000 && p < 0x00100080)    return "SMPC";
    if (p >= 0x00180000 && p < 0x00190000)    return "Backup RAM";
    if (p >= 0x00200000 && p < 0x00300000)    return "Work RAM-L";
    if (p >= 0x01000000 && p < 0x01000004)    return "MINIT";
    if (p >= 0x01800000 && p < 0x01800004)    return "SINIT";
    if (p >= 0x02000000 && p < 0x05000000)    return "A-Bus (cart/CS0-2)";
    if (p >= 0x05800000 && p < 0x05900000)    return "CD block";
    if (p >= 0x05A00000 && p < 0x05B00EE4)    return "SCSP / sound RAM";
    if (p >= 0x05C00000 && p < 0x05C80000)    return "VDP1 VRAM";
    if (p >= 0x05C80000 && p < 0x05D00000)    return "VDP1 framebuffer";
    if (p >= 0x05D00000 && p < 0x05D00018)    return "VDP1 registers";
    if (p >= 0x05E00000 && p < 0x05E80000)    return "VDP2 VRAM";
    if (p >= 0x05F00000 && p < 0x05F01000)    return "VDP2 CRAM";
    if (p >= 0x05F80000 && p < 0x05F80120)    return "VDP2 registers";
    if (p >= 0x05FE0000 && p < 0x05FE00D0)    return "SCU registers";
    if (p >= 0x06000000 && p < 0x06000400)    return "Work RAM-H : master vector table";
    if (p >= 0x06000400 && p < 0x06000800)    return "Work RAM-H : slave vector table";
    if (p >= 0x06000800 && p < 0x06002000)    return "Work RAM-H : SYSTEM (TECH#35)";
    if (p >= 0x060FF000 && p < 0x06100000)    return "Work RAM-H : SYSTEM area (TECH#35)";
    if (p >= 0x06000000 && p < 0x06100000)    return "Work RAM-H";
    if (a  >= 0xFFFFFE00)                     return "SH2 on-chip";
    return "unmapped / unknown";
}

/* Which SH2 address partition was used (tells you if the program is
   going through the cache or not -- central to this class of bug). */
static const char *partitionName(u32 a)
{
    switch (a >> 28) {
        case 0x0: return "cached";
        case 0x2: return "cache-through";
        case 0x4: return "associative purge";
        case 0x6: return "cache address array";
        case 0xC: return "cache data array / 2KB RAM";
        case 0xF: return "on-chip";
        default:  return "";
    }
}

/* ---- raw (bypass whatever cache emulation exists) reads ------------ */
/* If your fork renamed the work RAM globals, only these two functions
   need adapting. */
static bool rawReadLong(u32 addr, u32 *out)
{
    u32 p = physOf(addr);
    if (p >= 0x06000000 && p < 0x06100000) {
        if (!HighWram) return false;
        *out = T2ReadLong(HighWram, p & 0xFFFFF);
        return true;
    }
    if (p >= 0x00200000 && p < 0x00300000) {
        if (!LowWram) return false;
        *out = T2ReadLong(LowWram, p & 0xFFFFF);
        return true;
    }
    return false;
}

/* Read going through the emulator's normal SH2 path (i.e. through the
   cache emulation, if the fork implements one). */
static u32 cpuReadLong(SH2_struct *sh2, u32 addr)
{
    return SH2MappedMemoryReadLong(sh2, addr);
}
static u16 cpuReadWord(SH2_struct *sh2, u32 addr)
{
    return SH2MappedMemoryReadWord(sh2, addr);
}
static u8 cpuReadByte(SH2_struct *sh2, u32 addr)
{
    return SH2MappedMemoryReadByte(sh2, addr);
}

/* ---- classification helpers --------------------------------------- */

/* Could this value be a code pointer? */
static bool looksLikeCodePtr(u32 v)
{
    if (v & 1) return false;                 /* SH2 instructions are 2-byte aligned */
    u32 p = physOf(v);
    if (p >= 0x06000000 && p < 0x06100000) return true;   /* Work RAM-H */
    if (p >= 0x00200000 && p < 0x00300000) return true;   /* Work RAM-L */
    if (p >= 0x02000000 && p < 0x03000000) return true;   /* cart / CS0  */
    if (p >= 0x00001000 && p < 0x00080000) return true;   /* boot ROM    */
    return false;
}

/* Could this value be an SR? Only T,S,I0-3,Q,M are implemented. */
static bool looksLikeSR(u32 v) { return (v & ~0x000003F3u) == 0; }

/* Is the instruction at addr a call (BSR / BSRF / JSR)? A genuine return
   address always has its call instruction 4 bytes earlier, because SH2
   calls are delayed branches (call at -4, delay slot at -2).

   FIX: TRAPA used to be listed here. It must not be: TRAPA has no delay
   slot and pushes PC + 2 (sh2_opcodes.c SH2trapa, sh2_simple/sh2int.c),
   so looking for it at value-4 lands two bytes too low and invents a
   "TRAPA at <addr-4>" that is not the trapa instruction. TRAPA is handled
   by trapaImmAt() / scanExcFrames() below, as a frame and not a call. */
static const char *callKindAt(SH2_struct *sh2, u32 addr)
{
    if (addr & 1) return NULL;
    u16 op = cpuReadWord(sh2, addr);
    if ((op & 0xF000) == 0xB000) return "BSR";
    if ((op & 0xF0FF) == 0x0003) return "BSRF";
    if ((op & 0xF0FF) == 0x400B) return "JSR";
    return NULL;
}

static QString disasmAt(SH2_struct *sh2, u32 addr)
{
    char buf[256];
    buf[0] = 0;
    SH2Disasm(addr, cpuReadWord(sh2, addr), 0, NULL, buf);
    return QString(buf).trimmed();
}

/* ------------------------------------------------------------------ */
/* SH2 exception vectors                                              */
/* ------------------------------------------------------------------ */

struct VecInfo { int num; const char *name; };

static const VecInfo kVectors[] = {
    {  0, "Power-on reset PC"            },
    {  1, "Power-on reset SP"            },
    {  2, "Manual reset PC"              },
    {  3, "Manual reset SP"              },
    {  4, "General illegal instruction"  },
    {  6, "Slot illegal instruction"     },
    {  9, "CPU address error"            },
    { 10, "DMA address error"            },
    { 11, "NMI"                          },
    { 12, "User break"                   },
    { 0x40, "SCU: V-Blank IN"            },
    { 0x41, "SCU: V-Blank OUT"           },
    { 0x42, "SCU: H-Blank IN"            },
    { 0x43, "SCU: Timer 0"               },
    { 0x44, "SCU: Timer 1"               },
    { 0x45, "SCU: DSP End"               },
    { 0x46, "SCU: Sound Request"         },
    { 0x47, "SCU: System Manager"        },
    { 0x48, "SCU: Pad Interrupt"         },
    { 0x49, "SCU: Level 2 DMA End"       },
    { 0x4A, "SCU: Level 1 DMA End"       },
    { 0x4B, "SCU: Level 0 DMA End"       },
    { 0x4C, "SCU: DMA-illegal"           },
    { 0x4D, "SCU: Sprite draw end"       },
    { 0x64, "FRT input capture (M<->S)"  },
    { 0x65, "FRT compare match"          },
    { 0x66, "FRT overflow"               },
    { 0x6C, "SH2 DMAC ch1"               },
    { 0x6D, "SH2 DMAC ch0"               },
    { 0x6E, "DIVU"                       },
};

/* ------------------------------------------------------------------ */
/* Exception / TRAPA frame detection                                  */
/*                                                                    */
/* Both cores push the frame the same way (sh2int.c SH2HandleInterrupts,*/
/* sh2_opcodes.c SH2trapa):                                            */
/*                                                                    */
/*     R15 -= 4 ; [R15] = SR   -> ends up at the HIGHER address        */
/*     R15 -= 4 ; [R15] = PC   -> ends up at the LOWER address         */
/*                                                                    */
/* so a frame at address A is: [A] = saved PC, [A+4] = saved SR.       */
/* ------------------------------------------------------------------ */

/* TRAPA immediate at addr, or -1. */
static int trapaImmAt(SH2_struct *sh2, u32 addr)
{
    if (addr & 1) return -1;
    if (!looksLikeCodePtr(addr)) return -1;
    u16 op = cpuReadWord(sh2, addr);
    return ((op & 0xFF00) == 0xC300) ? (int)(op & 0x00FF) : -1;
}

static const char *vectorName(int num)
{
    for (unsigned i = 0; i < sizeof(kVectors) / sizeof(kVectors[0]); i++)
        if (kVectors[i].num == num) return kVectors[i].name;
    if (num >= 0x50 && num <= 0x5F) return "SCU: external interrupt";
    if (num >= 0x20 && num <= 0x3F) return "user TRAPA vector";
    return "unknown vector";
}

/* A saved PC has to point at real code: not into the stack window we are
   walking, and not at a word that cannot be an opcode. Without this every
   zeroed slot looks like a frame whose saved PC is 00000000 -- a valid
   boot ROM address -- and the section drowns in false positives. */
static bool plausibleSavedPC(SH2_struct *sh2, u32 pc, u32 lo, u32 hi)
{
    if (!looksLikeCodePtr(pc)) return false;
    if (pc >= lo && pc <= hi)  return false;    /* that is stack, not code */
    u16 op = cpuReadWord(sh2, pc);
    if (op == 0x0000 || op == 0xFFFF) return false;
    return true;
}

struct ExcFrame
{
    u32  addr;        /* stack address holding the saved PC */
    u32  savedPC;
    u32  savedSR;
    int  trapaImm;    /* TRAPA immediate, or -1              */
    u32  trapaAddr;   /* address of the trapa opcode         */
    bool confirmed;   /* the long word at +4 is a valid SR   */
};

/* Scans [sp, ceiling) and returns the frames found, innermost first. */
static int scanExcFrames(SH2_struct *sh2, const sh2regs_struct &r,
                         u32 sp, u32 ceiling,
                         ExcFrame *out, int maxOut)
{
    int n = 0;
    if (ceiling <= sp) return 0;

    u32 lo = sp - 0x40;          /* tolerance: the walker's own window   */
    u32 hi = ceiling + 0x40;

    for (u32 a = sp; a + 8 <= ceiling && n < maxOut; a += 4) {
        u32 pc = cpuReadLong(sh2, a);
        u32 sr = cpuReadLong(sh2, a + 4);

        if (!plausibleSavedPC(sh2, pc, lo, hi)) continue;

        int imm = trapaImmAt(sh2, pc - 2);

        if (imm < 0) {
            /* An ordinary JSR/BSR return address is not an exception frame. */
            if (callKindAt(sh2, pc - 4) != NULL) continue;
            /* An interrupt handler runs with SR.I set to the level that was
               accepted, written by the core right after the frame is pushed.
               I == 0 here means these are two unrelated long words. */
            if (!looksLikeSR(sr) || ((sr >> 4) & 0xF) == 0) continue;
            /* A value still held in a register is a spill the compiler just
               pushed, not a saved PC. Without this, "pointer followed by a
               small constant" -- an extremely common prologue -- is reported
               as an interrupt frame. */
            {
                bool isSpill = false;
                for (int k = 0; k < 16; k++)
                    if (r.R[k] == pc) { isSpill = true; break; }
                if (isSpill) continue;
            }
        }

        ExcFrame &f = out[n++];
        f.addr      = a;
        f.savedPC   = pc;
        f.savedSR   = sr;
        f.trapaImm  = imm;
        f.trapaAddr = (imm >= 0) ? (pc - 2) : 0;
        /* The opcode proves a trapa sits at savedPC-2, but a real frame must
           also carry a valid SR at +4. When it does not, the long word is
           data that merely happens to point just after a trapa -- say so
           instead of announcing a handler that is not there. */
        f.confirmed = looksLikeSR(sr);
    }
    return n;
}

/* Shared by writeExceptionFrame() and writeStackWalk() so both agree on
   where the stack ends. */
static u32 stackCeiling(const sh2regs_struct &r, const SH2ReportOptions &opt,
                        const char **src)
{
    u32 sp = r.R[15] & ~3u;
    if (r.GBR > sp && (r.GBR - sp) <= 0x8000) {
        if (src) *src = "GBR -- globals start here, so the stack tops out below it";
        return r.GBR & ~3u;
    }
    if (src) *src = "SP + window (GBR is not just above SP, ceiling unknown)";
    return sp + opt.stackBytesAboveSP;
}

/* ------------------------------------------------------------------ */
/* Sections                                                           */
/* ------------------------------------------------------------------ */

static void sectionHeader(QTextStream &ts, const char *title)
{
    ts << "\n########## " << title << " ##########\n\n";
}

static void writeRegisters(QTextStream &ts, SH2_struct *sh2, const sh2regs_struct &r)
{
    sectionHeader(ts, "CPU REGISTERS");

    for (int i = 0; i < 8; i++) {
        ts << QString("R%1 = %2   %3")
                .arg(i, 2, 10, QChar('0')).arg(hex32(r.R[i]))
                .arg(QString::fromLatin1(regionName(r.R[i])), -34);
        ts << QString("R%1 = %2   %3\n")
                .arg(i + 8, 2, 10, QChar('0')).arg(hex32(r.R[i + 8]))
                .arg(QString::fromLatin1(regionName(r.R[i + 8])));
    }

    ts << "\n";
    ts << "SR   = " << hex32(r.SR.all)
       << "   I=" << ((r.SR.all >> 4) & 0xF)
       << "  T=" << (r.SR.all & 1)
       << "  S=" << ((r.SR.all >> 1) & 1)
       << "  Q=" << ((r.SR.all >> 8) & 1)
       << "  M=" << ((r.SR.all >> 9) & 1) << "\n";

    int imask = (r.SR.all >> 4) & 0xF;
    ts << "       interrupt mask level " << imask;
    if (imask == 15)
        ts << "  <- ALL interrupts masked. Boot ROM uses SR=15 for V-Blank IN,\n"
              "          and FRT input capture (master<->slave) is also level 15.\n";
    else if (imask == 0)
        ts << "  (normal user code)\n";
    else
        ts << "\n";

    ts << "GBR  = " << hex32(r.GBR)  << "   " << regionName(r.GBR)  << "\n";
    ts << "VBR  = " << hex32(r.VBR)  << "   " << regionName(r.VBR)  << "\n";
    ts << "MACH = " << hex32(r.MACH) << "\n";
    ts << "MACL = " << hex32(r.MACL) << "\n";
    ts << "PR   = " << hex32(r.PR)   << "   " << regionName(r.PR)   << "\n";
    ts << "PC   = " << hex32(r.PC)   << "   " << regionName(r.PC)
       << "  [" << partitionName(r.PC) << "]\n";
    ts << "\nInstruction at PC : " << hex16(cpuReadWord(sh2, r.PC))
       << "   " << disasmAt(sh2, r.PC) << "\n";
}

static void writeVectorTable(QTextStream &ts, SH2_struct *sh2, const sh2regs_struct &r)
{
    sectionHeader(ts, "EXCEPTION VECTORS (from VBR)");

    ts << "VBR = " << hex32(r.VBR) << "\n\n";
    ts << "Vec  Addr      Handler   Where                              First insn\n";
    ts << "---- --------- --------- ---------------------------------- ----------\n";

    for (unsigned i = 0; i < sizeof(kVectors) / sizeof(kVectors[0]); i++) {
        u32 va = r.VBR + kVectors[i].num * 4;
        u32 h  = cpuReadLong(sh2, va);
        QString insn;
        if (kVectors[i].num > 3 && looksLikeCodePtr(h))
            insn = disasmAt(sh2, h);
        ts << QString("%1 %2 %3 %4 %5\n")
                .arg(hex8(kVectors[i].num), -4)
                .arg(hex32(va))
                .arg(hex32(h))
                .arg(QString::fromLatin1(regionName(h)), -34)
                .arg(insn);
    }

    ts << "\nNote: the boot ROM installs dummy handlers that are infinite\n"
          "loops (Saturn System Library User's Guide, 1.1). If a handler\n"
          "disassembles to a self-branch (e.g. 'bf <same address>'), the\n"
          "application never registered its own handler for that vector.\n";
}

/* Scans the whole live stack instead of only probing @SP / @(SP+4): as
   soon as a handler had pushed anything of its own, the old test reported
   "not inside a handler". */
static void writeExceptionFrame(QTextStream &ts, SH2_struct *sh2,
                                const sh2regs_struct &r,
                                const SH2ReportOptions &opt)
{
    sectionHeader(ts, "EXCEPTION / TRAPA FRAMES");

    u32 sp      = r.R[15] & ~3u;
    u32 ceiling = stackCeiling(r, opt, NULL);

    ExcFrame frames[32];
    int n = scanExcFrames(sh2, r, sp, ceiling, frames, 32);

    ts << "VBR = " << hex32(r.VBR)
       << "   SP = " << hex32(sp)
       << "   stack top = " << hex32(ceiling) << "\n\n";

    if (n == 0) {
        ts << "No exception frame between SP and the top of the stack.\n"
              "The CPU is most likely not inside a handler.\n";
        return;
    }

    ts << "Slot      savedPC   savedSR   Confidence  Detail\n";
    ts << "--------- --------- --------- ----------- -----------------------------\n";

    int confirmed = 0;
    for (int i = 0; i < n; i++) {
        const ExcFrame &f = frames[i];
        if (f.confirmed) confirmed++;

        ts << QString("%1 %2 %3 %4 ")
                .arg(hex32(f.addr)).arg(hex32(f.savedPC)).arg(hex32(f.savedSR))
                .arg(QString::fromLatin1(!f.confirmed  ? "REJECTED   "
                                       : f.trapaImm >= 0 ? "confirmed  "
                                                         : "plausible  "));

        if (f.trapaImm >= 0)
            ts << QString("trapa #0x%1 at %2 -> vector %3H, %4")
                    .arg(hex8(f.trapaImm))
                    .arg(hex32(f.trapaAddr))
                    .arg(hex8(f.trapaImm))
                    .arg(QString::fromLatin1(vectorName(f.trapaImm)));
        else
            ts << QString("interrupt/exception frame, SR.I=%1")
                    .arg((f.savedSR >> 4) & 0xF);
        ts << "\n";

        if (!f.confirmed)
            ts << "                                          +4 = " << hex32(f.savedSR)
               << " is not a valid SR -> data, not a frame\n";

        if (f.confirmed) {
            ts << "                                          resumes at "
               << hex32(f.savedPC) << " : " << disasmAt(sh2, f.savedPC) << "\n";
            if (f.trapaImm >= 0)
                ts << "                                          handler = "
                   << hex32(cpuReadLong(sh2, r.VBR + f.trapaImm * 4)) << "\n";
        }
    }

    if (confirmed == 0) {
        ts << "\n=> No confirmed frame: the CPU is probably NOT inside a handler.\n"
              "   The rows above are candidates the SR check rejected; treat them\n"
              "   as leads only.\n";
        return;
    }

    ts << "\n=> The CPU is inside " << confirmed << " handler(s).\n"
          "   Only TRAPA frames are certain (the opcode proves it). An\n"
          "   interrupt frame is inferred from the PC/SR pair alone, so it is\n"
          "   reported as plausible, never as proof.\n";

    for (int i = 0; i < n; i++) {
        if (!frames[i].confirmed || frames[i].trapaImm < 0) continue;
        ts << "   NOTE: a TRAPA frame means the application entered the vector by\n"
              "         software. Vector " << hex8(frames[i].trapaImm) << "H ("
           << vectorName(frames[i].trapaImm) << ") is then reachable both by\n"
              "         TRAPA and by the real interrupt -- check the handler for\n"
              "         re-entrancy.\n";
        break;
    }

    for (int i = 0; i < n; i++) {
        if (!frames[i].confirmed) continue;
        u32 p = physOf(frames[i].savedPC);
        if (p >= 0x06000000 && p < 0x06100000 &&
            frames[i].savedPC >= sp - 0x400 && frames[i].savedPC <= ceiling) {
            ts << "   !! saved PC " << hex32(frames[i].savedPC)
               << " is inside the stack: the CPU was executing stack DATA\n"
                  "      (corrupted return address, not corrupted code).\n";
        }
    }
}

/* Reconstruct the call chain by scanning the stack for values that are
   preceded by a real BSR/JSR. Works without any symbol file.

   Three zones matter and must not be mixed:
     - below SP          : dead, already popped. Stale values there are
                           often useful, but they are NOT live frames and
                           must never enter the call chain.
     - SP .. stack top   : the live frames.
     - above stack top   : globals, not stack at all. Scanning into it
                           yields pure noise (VDP2 shadow tables, tile
                           maps, ...) that all look like "code pointers".

   The ceiling is taken from GBR when GBR sits just above SP, which is the
   usual Saturn layout: GBR-relative globals immediately above the stack
   top, stack growing down away from them. */
void writeStackWalk(QTextStream &ts, SH2_struct *sh2,
                    const sh2regs_struct &r,
                    const SH2ReportOptions &opt)
{
    sectionHeader(ts, "STACK WALK / RECONSTRUCTED CALL CHAIN");

    u32 sp = r.R[15] & ~3u;
    const char *ceilingSrc = NULL;
    u32 ceiling = stackCeiling(r, opt, &ceilingSrc);
    u32 from    = sp - opt.stackBytesBelowSP;

    ExcFrame frames[32];
    int nframes = scanExcFrames(sh2, r, sp, ceiling, frames, 32);

    ts << "SP         = " << hex32(sp) << "\n";
    ts << "stack top  = " << hex32(ceiling) << "   [" << ceilingSrc << "]\n";
    ts << "depth used = " << (ceiling > sp ? ceiling - sp : 0) << " bytes\n";
    ts << "scanning     " << hex32(from) << " .. " << hex32(ceiling) << "\n\n";

    ts << "Zone   Slot      Value     Kind          Call site / note\n";
    ts << "------ --------- --------- ------------- -------------------------------\n";

    int frameCount = 0;
    QStringList chain;

    for (u32 a = from; a <= ceiling && frameCount < opt.maxFrames; a += 4) {
        u32 v = cpuReadLong(sh2, a);
        if (v == 0) continue;

        bool live = (a >= sp);
        QString kind, note;

        /* (a) Slots belonging to an exception frame are named as such, so
               they are never mistaken for return addresses or loose SRs. */
        int fi = -1, fpart = 0;
        for (int i = 0; i < nframes; i++) {
            if (a == frames[i].addr)     { fi = i; fpart = 0; break; }
            /* Only a confirmed frame owns its +4 slot; for a rejected one
               that long word is ordinary data. */
            if (frames[i].confirmed && a == frames[i].addr + 4)
                                         { fi = i; fpart = 1; break; }
        }

        /* A value pointing back into the stack is a saved frame pointer
           (R14/R15), never a return address. Test this before the code
           pointer test or work RAM-H stack addresses win. */
        bool inStack = (v >= from - 0x40 && v <= ceiling + 0x40);

        if (fi >= 0 && fpart == 0) {
            kind = !frames[fi].confirmed  ? "exc PC?"
                 : frames[fi].trapaImm >= 0 ? "EXC saved PC" : "exc PC (irq)";
            if (frames[fi].trapaImm >= 0)
                note = QString("trapa #0x%1 at %2 (%3)")
                          .arg(hex8(frames[fi].trapaImm))
                          .arg(hex32(frames[fi].trapaAddr))
                          .arg(QString::fromLatin1(vectorName(frames[fi].trapaImm)));
            else
                note = QString("interrupt frame, resumes at %1").arg(hex32(v));
            if (!frames[fi].confirmed) note += "   <- SR at +4 invalid, unconfirmed";
        }
        else if (fi >= 0 && fpart == 1) {
            kind = "EXC saved SR";
            note = QString("I=%1 T=%2").arg((v >> 4) & 0xF).arg(v & 1);
        }
        else if (inStack && physOf(v) >= 0x06000000 && physOf(v) < 0x06100000) {
            kind = "STACK ptr";
            note = QString("saved frame pointer -> %1").arg(hex32(v));
            if (v == a)          note += "   <- SELF-REFERENCE";
            else if (v == a + 4) note += "   <- points to the next slot";
        }
        else if (looksLikeCodePtr(v)) {
            const char *ck = callKindAt(sh2, v - 4);
            if (ck) {
                kind = "RETURN ADDR";
                note = QString("%1 at %2 : %3")
                          .arg(QString::fromLatin1(ck)).arg(hex32(v - 4))
                          .arg(disasmAt(sh2, v - 4));
                if (live) { chain.prepend(hex32(v)); frameCount++; }
            } else {
                int imm = trapaImmAt(sh2, v - 2);
                if (imm >= 0) {
                    /* Points just after a trapa but was not accepted as a
                       frame: report it without claiming a handler. */
                    kind = "after TRAPA";
                    note = QString("trapa #0x%1 at %2, but no valid SR at +4")
                              .arg(hex8(imm))
                              .arg(hex32(v - 2));
                } else {
                    kind = "code ptr?";
                    note = QString("no call at %1 (%2)")
                              .arg(hex32(v - 4)).arg(disasmAt(sh2, v - 4));
                }
            }
        }
        /* The bare "SR?" row is gone: a lone value matching the SR bit mask
           proves nothing -- 00000001 and 00000100 are far more often a
           counter or a flag. An SR is only reported as part of a frame,
           i.e. in branch (a) above. */
        else if (physOf(v) >= 0x06000000 && physOf(v) < 0x06100000) {
            kind = "RAM ptr";
            note = QString::fromLatin1(regionName(v));
        }
        else {
            continue;   /* plain data */
        }

        ts << QString("%1 %2 %3 %4 %5\n")
                .arg(QString::fromLatin1(live ? "live  " : "dead  "))
                .arg(hex32(a)).arg(hex32(v))
                .arg(kind, -13).arg(note);
    }

    ts << "\n'dead' rows sit below SP: already popped, shown only as history.\n";
    ts << "Only 'live' rows feed the chain below.\n";

    ts << "\nProbable call chain (outermost first):\n";
    if (chain.isEmpty())
        ts << "  <none recovered>\n";
    else {
        for (int i = 0; i < chain.size(); i++)
            ts << "  " << QString(i * 2, QChar(' ')) << "-> " << chain[i] << "\n";
        ts << "  " << QString(chain.size() * 2, QChar(' '))
           << "-> " << hex32(r.PC) << "   (current PC)\n";
    }

    if (nframes > 0) {
        ts << "\nThe chain above crosses " << nframes
           << " exception frame candidate(s); see the EXCEPTION / TRAPA FRAMES\n"
              "section. Frames marked REJECTED are not real handler entries.\n";
    }
}

void writeStackHexDump(QTextStream &ts, SH2_struct *sh2,
                              const sh2regs_struct &r,
                              const SH2ReportOptions &opt)
{
    sectionHeader(ts, "STACK HEX DUMP");

    u32 sp   = r.R[15] & ~3u;
    u32 from = (sp - opt.stackBytesBelowSP) & ~0xFu;
    u32 to   = (sp + opt.stackBytesAboveSP) | 0xFu;

    for (u32 a = from; a < to; a += 16) {
        ts << hex32(a) << "  ";
        for (int i = 0; i < 16; i += 4)
            ts << hex32(cpuReadLong(sh2, a + i)) << " ";

        /* markers */
        if (sp >= a && sp < a + 16)      ts << " <- SP";
        if (r.R[14] >= a && r.R[14] < a + 16) ts << " <- R14";
        if (r.GBR  >= a && r.GBR  < a + 16)   ts << " <- GBR";
        ts << "\n";
    }
}

static void writeDisasmWindow(QTextStream &ts, SH2_struct *sh2,
                              u32 center, const char *label,
                              const SH2ReportOptions &opt)
{
    ts << "\n--- disassembly around " << label << " (" << hex32(center) << ") ---\n";
    u32 from = (center - opt.disasmBytesAroundPC / 2) & ~1u;
    u32 to   = center + opt.disasmBytesAroundPC / 2;
    for (u32 a = from; a <= to; a += 2) {
        ts << (a == center ? ">> " : "   ")
           << hex32(a) << "  " << hex16(cpuReadWord(sh2, a))
           << "  " << disasmAt(sh2, a) << "\n";
    }
}

/* Compare what the CPU path returns against the untouched RAM contents.
   A mismatch means the SH2 read a stale cache line (or that the fork's
   cache emulation is broken). */
static void writeCacheCheck(QTextStream &ts, SH2_struct *sh2,
                            const sh2regs_struct &r,
                            const SH2ReportOptions &opt)
{
    sectionHeader(ts, "CACHE STATE & COHERENCY CHECK");

    u8 ccr = cpuReadByte(sh2, 0xFFFFFE92);
    ts << "CCR (FFFFFE92) = " << hex8(ccr) << "\n";
    ts << "  CE (cache enable)      = " << ((ccr >> 0) & 1) << "\n";
    ts << "  ID (instr. replace dis)= " << ((ccr >> 1) & 1) << "\n";
    ts << "  OD (data replace dis)  = " << ((ccr >> 2) & 1) << "\n";
    ts << "  TW (two-way mode)      = " << ((ccr >> 3) & 1)
       << ((ccr & 0x08) ? "   <- 2KB cache + 2KB RAM at C0000000-C0000FFF\n"
                        : "   (4KB / 4-way)\n");
    ts << "  CP (cache purge)       = " << ((ccr >> 4) & 1) << "\n";
    ts << "\n  Expected values per TECH#28 6.2: 01H = 4KB cache,\n"
          "  09H = 2KB cache + 2KB RAM, 10H = full purge.\n";

    if (!opt.dumpCacheCheck) return;

    u32 sp   = r.R[15] & ~3u;
    u32 from = (sp - opt.stackBytesBelowSP) & ~3u;
    u32 to   =  sp + opt.stackBytesAboveSP;

    ts << "\nComparing CPU-path reads against raw work RAM over "
       << hex32(from) << ".." << hex32(to) << "\n\n";

    int mismatches = 0, checked = 0;
    for (u32 a = from; a <= to; a += 4) {
        u32 raw;
        if (!rawReadLong(a, &raw)) continue;
        u32 viaCpu = cpuReadLong(sh2, a);
        u32 viaThr = cpuReadLong(sh2, (a & 0x0FFFFFFF) | 0x20000000);
        checked++;
        if (viaCpu != raw || viaThr != raw) {
            mismatches++;
            ts << "  MISMATCH " << hex32(a)
               << "  cpu=" << hex32(viaCpu)
               << "  cache-through=" << hex32(viaThr)
               << "  raw=" << hex32(raw) << "\n";
        }
    }
    /* The stack is not the only thing worth checking. On this build the
       instruction fetch for Work RAM-H goes through SH2MappedMemoryReadWord,
       exactly like the disassembler, so fetch and debugger can never
       disagree with each other -- but BOTH can disagree with raw RAM if a
       cache line is stale. Cover the code around PC and PR as well. */
    {
        u32 wins[2][2];
        int nwin = 0;
        wins[nwin][0] = (r.PC - 0x40) & ~3u; wins[nwin][1] = r.PC + 0x40; nwin++;
        if (looksLikeCodePtr(r.PR) && (r.PR < r.PC - 0x80 || r.PR > r.PC + 0x80)) {
            wins[nwin][0] = (r.PR - 0x40) & ~3u; wins[nwin][1] = r.PR + 0x40; nwin++;
        }
        for (int w = 0; w < nwin; w++) {
            for (u32 a = wins[w][0]; a <= wins[w][1]; a += 4) {
                u32 raw;
                if (!rawReadLong(a, &raw)) continue;
                u32 viaCpu = cpuReadLong(sh2, a);
                checked++;
                if (viaCpu != raw) {
                    mismatches++;
                    ts << "  MISMATCH (code) " << hex32(a)
                       << "  cpu=" << hex32(viaCpu)
                       << "  raw=" << hex32(raw) << "\n";
                }
            }
        }
    }

    ts << "\n  " << checked << " long-words checked (stack + code), "
       << mismatches << " mismatch(es).\n";
    if (mismatches == 0)
        ts << "  => No stale cache line on the stack. If the return address was\n"
              "     still wrong, the bug is a stack imbalance or an overwrite,\n"
              "     NOT a cache coherency problem.\n";
    else
        ts << "  => Stale data seen through the CPU path. Check that the game's\n"
              "     purges (16-bit write of 0 to addr|40000000H, TECH#28 6.2) are\n"
              "     actually honoured by the emulator.\n";
}

static void writeOnchip(QTextStream &ts, SH2_struct *sh2)
{
    sectionHeader(ts, "SH2 ON-CHIP REGISTERS");

    struct { u32 addr; const char *name; int size; } regs[] = {
        { 0xFFFFFE10, "TIER   (FRT interrupt enable)", 1 },
        { 0xFFFFFE11, "FTCSR  (FRT status)",           1 },
        { 0xFFFFFE12, "FRC-H",                         1 },
        { 0xFFFFFE13, "FRC-L",                         1 },
        { 0xFFFFFE16, "TCR",                           1 },
        { 0xFFFFFE17, "TOCR",                          1 },
        { 0xFFFFFE18, "ICR-H",                         1 },
        { 0xFFFFFE60, "IPRB",                          2 },
        { 0xFFFFFE62, "VCRA",                          2 },
        { 0xFFFFFE64, "VCRB",                          2 },
        { 0xFFFFFE66, "VCRC",                          2 },
        { 0xFFFFFE68, "VCRD",                          2 },
        { 0xFFFFFE71, "DRCR0",                         1 },
        { 0xFFFFFE72, "DRCR1",                         1 },
        { 0xFFFFFE80, "WTCSR/WTCNT",                   2 },
        { 0xFFFFFE92, "CCR    (cache control)",        1 },
        { 0xFFFFFEE0, "IPRA",                          2 },
        { 0xFFFFFEE2, "VCRWDT",                        2 },
        { 0xFFFFFF40, "DVSR",                          4 },
        { 0xFFFFFF80, "SAR0 (SH2 DMAC)",               4 },
        { 0xFFFFFF84, "DAR0",                          4 },
        { 0xFFFFFF88, "TCR0",                          4 },
        { 0xFFFFFF8C, "CHCR0",                         4 },
        { 0xFFFFFF90, "SAR1",                          4 },
        { 0xFFFFFF94, "DAR1",                          4 },
        { 0xFFFFFF98, "TCR1",                          4 },
        { 0xFFFFFF9C, "CHCR1",                         4 },
        { 0xFFFFFFB0, "DMAOR",                         4 },
        { 0xFFFFFFE0, "BCR1",                          4 },
        { 0xFFFFFFE4, "BCR2",                          4 },
    };

    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        QString v;
        switch (regs[i].size) {
            case 1: v = hex8 (cpuReadByte(sh2, regs[i].addr)); break;
            case 2: v = hex16(cpuReadWord(sh2, regs[i].addr)); break;
            default:v = hex32(cpuReadLong(sh2, regs[i].addr)); break;
        }
        ts << hex32(regs[i].addr) << "  "
           << pad(regs[i].name, -32) << " = " << v << "\n";
    }
}

static void writeScu(QTextStream &ts, SH2_struct *sh2, const sh2regs_struct &r)
{
    sectionHeader(ts, "SCU REGISTERS / DMA");

    struct { u32 off; const char *name; } regs[] = {
        { 0x00, "D0R  (lvl0 read addr)"  }, { 0x04, "D0W  (lvl0 write addr)" },
        { 0x08, "D0C  (lvl0 count)"      }, { 0x0C, "D0AD (lvl0 add value)"  },
        { 0x10, "D0EN (lvl0 enable)"     }, { 0x14, "D0MD (lvl0 mode)"       },
        { 0x20, "D1R"                    }, { 0x24, "D1W"                    },
        { 0x28, "D1C"                    }, { 0x2C, "D1AD"                   },
        { 0x30, "D1EN"                   }, { 0x34, "D1MD"                   },
        { 0x40, "D2R"                    }, { 0x44, "D2W"                    },
        { 0x48, "D2C"                    }, { 0x4C, "D2AD"                   },
        { 0x50, "D2EN"                   }, { 0x54, "D2MD"                   },
        { 0x7C, "DSTA (DMA status)"      },
        { 0x80, "PPAF (DSP prog ctrl)"   }, { 0x8C, "T0C"                    },
        { 0x90, "T1S"                    }, { 0x94, "T1MD"                   },
        { 0xA0, "IMS  (interrupt mask)"  }, { 0xA4, "IST  (interrupt status)"},
        { 0xA8, "AIACK"                  }, { 0xB0, "ASR0"                   },
        { 0xB4, "ASR1"                   }, { 0xB8, "AREF"                   },
        { 0xC4, "RSEL"                   }, { 0xC8, "VER"                    },
    };

    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        u32 a = 0x25FE0000 + regs[i].off;
        u32 v = cpuReadLong(sh2, a);
        ts << hex32(a) << "  " << pad(regs[i].name, -26) << " = " << hex32(v);
        if ((regs[i].off & 0x1F) == 0x04) {          /* a D?W write address */
            u32 p = physOf(v);
            ts << "   -> " << regionName(v);
            u32 sp = r.R[15];
            if (p >= 0x06000000 && p < 0x06100000 &&
                v >= (sp - 0x1000) && v <= (sp + 0x1000))
                ts << "   !! DMA DESTINATION IS INSIDE THE STACK !!";
        }
        ts << "\n";
    }

    /* IST/IMS bit by bit. Bit N of IST/IMS is SCU interrupt vector 40H + N,
       so the vector table names apply directly. This is what tells you
       whether a "frame ready" signal (Level 0 DMA End = bit 11, Sprite draw
       end = bit 13) is actually being raised and taken. */
    {
        u32 ist = cpuReadLong(sh2, 0x25FE00A4);
        u32 ims = cpuReadLong(sh2, 0x25FE00A0);

        ts << "\nSCU interrupt bits (bit N = vector " << "40H + N):\n";
        ts << "Bit  Vec  Name                        IST  IMS\n";
        ts << "---- ---- --------------------------- ---- ----\n";
        for (int b = 0; b <= 15; b++) {
            u32 pending = (ist >> b) & 1;
            u32 masked  = (ims >> b) & 1;
            if (!pending && !masked && b > 13) continue;
            ts << QString("%1 %2H  %3 %4    %5\n")
                    .arg(b, 4)
                    .arg(hex8(0x40 + b))
                    .arg(QString::fromLatin1(vectorName(0x40 + b)), -27)
                    .arg(pending ? "SET " : "  . ")
                    .arg(masked  ? "MASK" : "  . ");
        }
        ts << "\n  A pending IST bit that never clears means the handler is not\n"
              "  being entered: either IMS masks it, or the core never raises it.\n";
        if (((ist >> 11) & 1) && !((ims >> 11) & 1))
            ts << "  -> Level 0 DMA End is pending and unmasked.\n";
        if (((ist >> 13) & 1) && !((ims >> 13) & 1))
            ts << "  -> Sprite draw end is pending and unmasked.\n";
    }

    ts << "\nReminders (ST-210 / TECH#10):\n"
          "  - SCU-DMA cannot write to the A-Bus.\n"
          "  - SCU-DMA cannot read from the VDP2 area, nor burst-read VRAM.\n"
          "  - SCU-DMA can only use Work RAM-H, never Work RAM-L.\n"
          "  - SCU registers must ALWAYS be accessed through cache-through\n"
          "    addresses (25FExxxx), never 05FExxxx.\n"
          "  - Do not start DMA level 2 while level 1 is running.\n";
}

/* Rule-based checks derived from the Sega technical bulletins. */
static void writeDiagnostics(QTextStream &ts, SH2_struct *sh2, const sh2regs_struct &r)
{
    sectionHeader(ts, "DIAGNOSTICS");

    int n = 0;
    u32 sp = r.R[15];
    u32 pc = r.PC;

    if (pc & 1) {
        ts << ++n << ". PC is odd (" << hex32(pc)
           << "). The SH2 cannot fetch from an odd address -> address error.\n";
    }

    if (physOf(pc) >= 0x06000000 && physOf(pc) < 0x06100000 &&
        pc >= sp - 0x400 && pc <= sp + 0x4000) {
        ts << ++n << ". PC is inside the stack region. The CPU is executing stack\n"
              "   DATA. Look for a corrupted return address rather than corrupted code.\n";
    }

    if (pc == r.PR) {
        ts << ++n << ". PC == PR. Consistent with an RTS taken on a corrupted PR:\n"
              "   RTS does not modify PR, so after the jump they are equal.\n";
    }

    if (pc == r.R[15] || pc == r.R[14]) {
        ts << ++n << ". PC equals R14/R15. A stack ADDRESS was loaded into PR instead\n"
              "   of the VALUE stored there -- classic off-by-4 stack imbalance.\n"
              "   Check the value at PC itself: if it is a plausible code pointer,\n"
              "   the epilogue popped from one slot too low.\n";
        u32 v = cpuReadLong(sh2, pc);
        if (looksLikeCodePtr(v))
            ts << "   -> value at " << hex32(pc) << " is " << hex32(v)
               << ", which IS a plausible return address. Confirms the off-by-4.\n";
    }

    /* A legitimate PR was written by a BSR/JSR, so a real call instruction
       always sits 4 bytes before it (SH2 calls are delayed branches: call
       at -4, delay slot at -2). If that is not the case, PR did not come
       from a call and has been clobbered. */
    if (looksLikeCodePtr(r.PR) && callKindAt(sh2, r.PR - 4) == NULL) {
        ts << ++n << ". PR = " << hex32(r.PR) << " but there is no BSR/JSR at "
           << hex32(r.PR - 4) << ".\n"
              "   PR was not produced by a call instruction, so it is corrupted.\n";

        u32 at = cpuReadLong(sh2, r.PR);
        if (looksLikeCodePtr(at) && callKindAt(sh2, at - 4) != NULL) {
            ts << "   The VALUE stored at " << hex32(r.PR) << " is "
               << hex32(at) << ", and that one IS a valid return address.\n"
                  "   => PR holds the ADDRESS of the stack slot instead of its\n"
                  "      CONTENT. That is the semantics of 'lds Rm,PR' where\n"
                  "      'lds.l @Rm+,PR' was intended -- one level of indirection\n"
                  "      lost. Check R15 too: a correct pop also increments it.\n";
        }
    }

    u32 spp = physOf(sp);
    if (spp >= 0x060FF000 && spp < 0x06100000) {
        ts << ++n << ". SP is in 060FF000H-060FFFFFH. Per TECH#35 this range belongs\n"
              "   to the system only while the 1st Read File is being loaded, and is\n"
              "   released to the application afterwards -- an application stack here\n"
              "   is legal and common, so this is informational, not an error.\n"
              "   It only turns into a problem if a boot ROM service routine is\n"
              "   re-entered later (e.g. the horizontal resolution switch of TECH#37,\n"
              "   or the CD-DA control screen) and reclaims the area.\n";
    }
    if (spp >= 0x06000000 && spp < 0x06002000) {
        ts << ++n << ". SP is inside 6000000H-6001FFFH, reserved for the system\n"
              "   (TECH#35). Only 6000E00H-6001FFFH may legally hold a stack.\n";
    }

    if (((r.SR.all >> 4) & 0xF) == 15) {
        ts << ++n << ". SR interrupt mask is 15. The boot ROM priority table uses SR=15\n"
              "   for V-Blank IN, and the FRT input capture interrupt used for\n"
              "   master/slave communication is also level 15 (System Library 1.1).\n"
              "   You are very likely inside one of those handlers or a critical section.\n";
    }

    /* SCU accessed through cached addresses? */
    for (int i = 0; i < 16; i++) {
        u32 v = r.R[i];
        if (v >= 0x05FE0000 && v < 0x05FE0100) {
            ts << ++n << ". R" << i << " = " << hex32(v)
               << " points at the SCU through a CACHED address.\n"
                  "   ST-210 No.05 / TECH#10 No.05 forbid this: use 25FExxxx.\n";
        }
    }

    /* Cache-through discipline for VDP areas is fine; flag cached VDP writes. */
    for (int i = 0; i < 16; i++) {
        u32 v = r.R[i];
        if ((v >= 0x05C00000 && v < 0x05D00020) ||
            (v >= 0x05E00000 && v < 0x05F80120)) {
            ts << ++n << ". R" << i << " = " << hex32(v)
               << " addresses VDP memory through the CACHED partition.\n"
                  "   Prefer the cache-through mirror (25xxxxxx) for VDP accesses.\n";
        }
    }

    u8 ccr = cpuReadByte(sh2, 0xFFFFFE92);
    if (ccr & 0x08) {
        ts << ++n << ". CCR TW bit set: the cache is in 2KB cache + 2KB RAM mode.\n"
              "   The 2KB RAM lives at C0000000H-C0000FFFH and, per TECH#28 4.1,\n"
              "   code placed there is NOT shared between the two CPUs. Make sure\n"
              "   your build emulates that region.\n";
    }
    if ((ccr & 0x01) == 0) {
        ts << ++n << ". Cache is currently DISABLED (CCR.CE = 0).\n";
    }

    if (n == 0)
        ts << "No rule triggered.\n";
}

/* ---- report assembly ---------------------------------------------- */

QString buildReport(SH2_struct *sh2, const SH2ReportOptions &opt)
{
    QString out;
    QTextStream ts(&out);

    if (sh2 == NULL) {
        ts << "SH2 context is NULL -- emulation not initialised.\n";
        return out;
    }

    sh2regs_struct r;
    memset(&r, 0, sizeof(r));
    SH2GetRegisters(sh2, &r);

    const char *who = (sh2 == MSH2) ? "Master SH2" : "Slave SH2";

    ts << "Yabause/Kronos SH2 Debug Export\n";
    ts << "Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << "CPU: " << who << "\n";
    ts << "==================================================\n";

    writeRegisters(ts, sh2, r);
    writeDiagnostics(ts, sh2, r);
    writeExceptionFrame(ts, sh2, r, opt);
    writeStackWalk(ts, sh2, r, opt);
    writeStackHexDump(ts, sh2, r, opt);

    sectionHeader(ts, "DISASSEMBLY");
    writeDisasmWindow(ts, sh2, r.PC, "PC", opt);
    if (r.PR != r.PC && looksLikeCodePtr(r.PR))
        writeDisasmWindow(ts, sh2, r.PR, "PR", opt);

    writeVectorTable(ts, sh2, r);
    writeCacheCheck(ts, sh2, r, opt);
    if (opt.dumpOnchip) writeOnchip(ts, sh2);
    if (opt.dumpScu)    writeScu(ts, sh2, r);

    sectionHeader(ts, "CORE BRANCH BACKTRACE");
    {
        int size = 0;
        u32 *list = SH2GetBacktraceList(sh2, &size);
        if (list && size > 0) {
            for (int i = 0; i < size; i++)
                ts << "  " << hex32(list[i]) << "   " << disasmAt(sh2, list[i]) << "\n";
        } else {
            ts << "  <empty>\n";
        }
        ts << "  " << hex32(r.PC) << "   (current PC)\n";
    }

    return out;
}

QString buildStackPanel(SH2_struct *sh2, const SH2ReportOptions &opt)
{
    QString out;
    QTextStream ts(&out);

    if (sh2 == NULL) { ts << "no SH2 context\n"; return out; }

    sh2regs_struct r;
    memset(&r, 0, sizeof(r));
    SH2GetRegisters(sh2, &r);

    writeDiagnostics(ts, sh2, r);
    writeExceptionFrame(ts, sh2, r, opt);
    writeStackWalk(ts, sh2, r, opt);
    writeStackHexDump(ts, sh2, r, opt);
    return out;
}

bool writeReportToFile(SH2_struct *sh2, const QString &dir,
                       QString *outPath, const SH2ReportOptions &opt)
{
    const char *who = (sh2 == SSH2) ? "SSH2" : "MSH2";
    QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString base  = QString("sh2_debug_%1_%2").arg(who).arg(stamp);

    QString target = dir.isEmpty() ? SH2DebugDumpDir() : dir;
    QDir d(target);
    if (!d.exists() && !d.mkpath(".")) {
        /* Unwritable configured directory: fall back rather than lose the
           report, which is usually being written from a crash path. */
        d = QDir(QDir::currentPath());
    }
    QString path = d.filePath(base + ".txt");

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        d = QDir(QDir::currentPath());
        path = d.filePath(base + ".txt");
        f.setFileName(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
    }

    QTextStream ts(&f);
    ts << buildReport(sh2, opt);
    f.close();

    if (outPath) *outPath = path;

    if (opt.dumpRawBin && sh2) {
        sh2regs_struct r;
        memset(&r, 0, sizeof(r));
        SH2GetRegisters(sh2, &r);

        u32 from = (r.R[15] - opt.stackBytesBelowSP) & ~0xFu;
        u32 to   = (r.R[15] + opt.stackBytesAboveSP) | 0xFu;

        QFile b(d.filePath(base + "_stack.bin"));
        if (b.open(QIODevice::WriteOnly)) {
            for (u32 a = from; a < to; a += 4) {
                u32 v = cpuReadLong(sh2, a);
                char be[4] = { (char)(v >> 24), (char)(v >> 16),
                               (char)(v >> 8),  (char)v };
                b.write(be, 4);
            }
            b.close();
        }
    }
    return true;
}

} /* anonymous namespace */

/* ------------------------------------------------------------------ */
/* Public entry points (declared in UIDebugSH2.h)                     */
/* ------------------------------------------------------------------ */

QString SH2DebugBuildReport(SH2_struct *sh2)
{
    return buildReport(sh2, SH2ReportOptions());
}

QString SH2DebugBuildStackPanel(SH2_struct *sh2)
{
    return buildStackPanel(sh2, SH2ReportOptions());
}

bool SH2DebugExportToFile(SH2_struct *sh2, const QString &dir, QString *outPath)
{
    SH2ReportOptions opt;
    opt.dumpRawBin = true;
    return writeReportToFile(sh2, dir, outPath, opt);
}

/* ---- END of the extended SH2 reporting block ---------------------- */


void SH2BreakpointHandler (SH2_struct *context, u32 addr, void *userdata)
{
   UIYabause* ui = QtYabause::mainWindow( false );

   if (context == MSH2)
      emit ui->breakpointHandlerMSH2((breakpoint_userdata*)userdata);
   else
      emit ui->breakpointHandlerSSH2((breakpoint_userdata*)userdata);
}

UIDebugSH2::UIDebugSH2(UIDebugCPU::PROCTYPE proc, YabauseThread *mYabauseThread, QWidget* p )
	: UIDebugCPU( proc, mYabauseThread, p )
{
	switch (proc)
	{
		case UIDebugCPU::PROC_MSH2:
			this->setWindowTitle(QtYabause::translate("Debug Master SH2"));
			gbRegisters->setTitle(QtYabause::translate("SH2 Registers"));
			debugSH2 = MSH2;
			break;
		case UIDebugCPU::PROC_SSH2:
			this->setWindowTitle(QtYabause::translate("Debug Slave SH2"));
			gbRegisters->setTitle(QtYabause::translate("SH2 Registers"));
			debugSH2 = SSH2;
			break;
		default: break;
	}

	lwDisassembledCode->setContext(debugSH2);

   if (debugSH2)
   {
      const codebreakpoint_struct *cbp;
      const memorybreakpoint_struct *mbp;
      int i;

      cbp = SH2GetBreakpointList(debugSH2);
      mbp = SH2GetMemoryBreakpointList(debugSH2);

      for (i = 0; i < MAX_BREAKPOINTS; i++)
      {
         QString text;
         if (cbp[i].addr != 0xFFFFFFFF)
         {
            text.sprintf("%08X", (int)cbp[i].addr);
            lwCodeBreakpoints->addItem(text);
         }

         if (mbp[i].addr != 0xFFFFFFFF)
         {
            text.sprintf("%08X", (int)mbp[i].addr);
            lwMemoryBreakpoints->addItem(text);
         }
      }

      lwDisassembledCode->setDisassembleFunction((int (*)(void *, u32, char *))SH2Dis);
			lwDisassembledCode->setEndAddress(0x06100000);
      lwDisassembledCode->setMinimumInstructionSize(2);
      gbBackTrace->setVisible( true );

      SH2SetBreakpointCallBack(debugSH2, (void (*)(void *, u32, void *))SH2BreakpointHandler, NULL);
   }

   updateAll();

   if (debugSH2 && debugSH2->trackInfLoop.enabled)
      pbReserved1->setText(QtYabause::translate("Loop Track Stop"));
   else
      pbReserved1->setText(QtYabause::translate("Loop Track Start"));
  pbReserved2->setText(QtYabause::translate("Loop Track Clear"));
	pbReserved3->setText(QtYabause::translate("Inline Assembly"));

  pbStepOver->setVisible( true );
  pbStepOut->setVisible( true );
  pbReserved1->setVisible( true );
  pbReserved2->setVisible( true );
	pbReserved3->setVisible( true );

  // Two extra actions on the previously unused spare buttons:
  //   Reserved4 -> write a full SH2 report (registers, diagnostics,
  //                exception frame, stack walk, cache state, SCU/on-chip)
  //                to a timestamped file next to the executable.
  //   Reserved5 -> show the stack walk in the "Other Debug" panel.
  pbReserved4->setText(QtYabause::translate("Export Debug"));
  pbReserved5->setText(QtYabause::translate("Stack Walk"));
  pbReserved4->setVisible( true );
  pbReserved5->setVisible( true );

  pbLoadCode->setVisible( true );
	connect( pbLoadCode, SIGNAL( clicked() ), this, SLOT( loadCodeAddress() ) );

  restoreAddr2line();

  // Push the configured dump directory down to the report engine, from
  // the GUI thread, so the automatic snapshot never touches QSettings.
  {
    Settings* settings = QtYabause::settings();
    if (settings)
    {
      QString dir = settings->value("Debug/SH2DumpDir").toString();
      if (!dir.isEmpty())
        SH2DebugSetDumpDir(dir);
    }
  }
}

void UIDebugSH2::restoreAddr2line()
{
  Settings* settings = QtYabause::settings();
  addr2line = settings->value( "Debug/Addr2Line" ).toString();
}

void UIDebugSH2::updateRegList()
{
   int i;
   sh2regs_struct sh2regs;
   QString str;

   if (debugSH2 == NULL)
      return;

   SH2GetRegisters(debugSH2, &sh2regs);
   lwRegisters->clear();

   for (i = 0; i < 16; i++)
   {
      str.sprintf("R%02d =  %08X", i, (int)sh2regs.R[i]);
      lwRegisters->addItem(str);
   }

   // SR
   str.sprintf("SR =   %08X", (int)sh2regs.SR.all);
   lwRegisters->addItem(str);

   // GBR
   str.sprintf("GBR =  %08X", (int)sh2regs.GBR);
   lwRegisters->addItem(str);

   // VBR
   str.sprintf("VBR =  %08X", (int)sh2regs.VBR);
   lwRegisters->addItem(str);

   // MACH
   str.sprintf("MACH = %08X", (int)sh2regs.MACH);
   lwRegisters->addItem(str);

   // MACL
   str.sprintf("MACL = %08X", (int)sh2regs.MACL);
   lwRegisters->addItem(str);

   // PR
   str.sprintf("PR =   %08X", (int)sh2regs.PR);
   lwRegisters->addItem(str);

   // PC
   str.sprintf("PC =   %08X", (int)sh2regs.PC);
   lwRegisters->addItem(str);
}

void UIDebugSH2::updateCodeList(u32 addr)
{
   addr &= 0x0FFFFFFF;
   lwDisassembledCode->goToAddress(addr);
   lwDisassembledCode->setPC(addr);
}

void UIDebugSH2::updateBackTrace()
{
   int size;
   u32 *addr=SH2GetBacktraceList(debugSH2, &size);

   lwBackTrace->clear();
   for (int i = 0; i < size; i++)
      lwBackTrace->addItem(QString("%1").arg(addr[i], 8, 16, QChar('0')).toUpper());
   lwBackTrace->addItem(QString("%1").arg(debugSH2->regs.PC, 8, 16, QChar('0')).toUpper());
}

void UIDebugSH2::updateTrackInfLoop()
{
   if (debugSH2)
   {
      tilInfo_struct *match=debugSH2->trackInfLoop.match;

      twTrackInfLoop->clearContents();
      twTrackInfLoop->setRowCount(0);
      twTrackInfLoop->setSortingEnabled(false);
      for (int i = 0; i < debugSH2->trackInfLoop.num; i++)
      {
         twTrackInfLoop->insertRow(i);
         QTableWidgetItem *newItem = new QTableWidgetItem(QString("%1").arg(match[i].addr, 8, 16, QChar('0')).toUpper());
         twTrackInfLoop->setItem(i, 0, newItem);

         newItem = new QTableWidgetItem();
         newItem->setData(Qt::DisplayRole, (qulonglong) match[i].count);
         twTrackInfLoop->setItem(i, 1, newItem);
      }
      twTrackInfLoop->setSortingEnabled(true);
   }
}

void UIDebugSH2::loadCodeAddress()
{
  sh2regs_struct sh2regs;
  SH2GetRegisters(debugSH2, &sh2regs);

  std::stringstream currentAddress;
  currentAddress << std::hex << sh2regs.PC;

	bool ok = false;
  const std::string newAddress = QInputDialog::getText(this, tr("Input code address"),
		tr("Address (hex):"), QLineEdit::Normal,
    QString::fromStdString(currentAddress.str()), &ok).toStdString();

	if (ok) {
    const u32 addr = static_cast<uint32_t>(std::stoull(newAddress, nullptr, 16));
    /* Fix: this used to only call updateCodePage(), which looks up a
     * matching .elf file for source-level display (addr2line) -- useful
     * for homebrew builds, but for a commercial game ISO (no matching
     * .elf) it silently returns without changing anything visible, so
     * "Load Code" appeared to do nothing. Also jump the actual
     * disassembly view to the requested address. */
    updateCodeList(addr);
    updateCodePage(addr);
  }
}

void UIDebugSH2::updateCodePage(u32 evaluateAddress)
{
  // When the user asked for the stack walk, keep it on screen across
  // steps instead of letting the addr2line source lookup overwrite it.
  if (stackPanelPinned)
  {
    codeBrowser->setPlainText(SH2DebugBuildStackPanel(debugSH2));
    return;
  }

  YuiMsg("Address to inspect %x\n", evaluateAddress);
  if (addr2line.isEmpty())
    restoreAddr2line();

  QString elfPath;
  const QString program{ addr2line };

  VolatileSettings *vs = QtYabause::volatileSettings();
	if ( vs->value( "General/CdRom" ) != CDCORE_ISO )
  {
    YuiMsg("Not using ISO, ignoring code\n");
    return;
  }
  else
  {
    const QString isoPathString{ vs->value( "Recents/ISOs" ).toString() };
    const QFileInfo fileInfo(isoPathString);
    QDir searchPath = fileInfo.dir();
    const QString filename = fileInfo.completeBaseName() + ".elf";

    YuiMsg("looking for %s\n", filename.toStdString().c_str());

    if (searchPath.cd("build")) {
      YuiMsg("looking for %s in %s\n", filename.toStdString().c_str(), searchPath.path().toStdString().c_str());
      if (searchPath.exists(filename)) {
        //Found in build folder
        elfPath = QFileInfo(searchPath, filename).absoluteFilePath();
        printf("Found %s !!\n", elfPath.toStdString().c_str());
      }
      else {
        searchPath.cdUp();
      }
    }
    if (elfPath.isEmpty()) {
      YuiMsg("looking for %s in %s\n", filename.toStdString().c_str(), searchPath.path().toStdString().c_str());
      if (searchPath.exists(filename)) {
        //Found in local folder
        YuiMsg("Found %s in %s\n", filename.toStdString().c_str(), searchPath.path().toStdString().c_str());
        elfPath = QFileInfo(searchPath, filename).absoluteFilePath();
      }
      else {
        // Not found at all
        YuiMsg("Could not find elf file, ignoring code\n");
        return;
      }
    }
  }
  std::stringstream hexAddress;
  hexAddress << std::setfill('0') << std::setw(8) << std::hex
             << evaluateAddress;

  QStringList arguments;
  arguments.push_back("-a");
  arguments.push_back(QString::fromStdString(hexAddress.str()));
  arguments.push_back("-p");
  arguments.push_back("-i");
  arguments.push_back("-f");
  arguments.push_back("-C");
  arguments.push_back("-e");
  arguments.push_back(elfPath);

  QProcess addr2lineProgram;
  addr2lineProgram.start(program, arguments);
  addr2lineProgram.waitForFinished();

  std::string commandString;
  commandString += program.toStdString() + " " + arguments.join(" ").toStdString();

  const QByteArray pstdout = addr2lineProgram.readAllStandardOutput();
  if (addr2lineProgram.exitCode() != 0) {
    const QByteArray pstderr = addr2lineProgram.readAllStandardError();
    const std::string outputString =
        pstdout.toStdString() + " / " + pstderr.toStdString();

    YuiMsg("Cmd: %s\n",
                commandString.c_str());
    YuiMsg("Output (%d): %s\n",
                addr2lineProgram.exitCode(), outputString.c_str());

    codeBrowser->setText(QString::fromStdString(outputString));
  } else {

    // QRegularExpression re("(0x[0-9a-fA-F]+):\\s*(.+?(?= at )) at (.+?(?=:[0-9]+)):([0-9]+)");
    QRegularExpression re("(0x[0-9a-fA-F]+):\\s*(.+?(?= at )) at (.+?(?=:[0-9]+)):([0-9]+)"
      "(\\s*\\(inlined by\\)\\s*(.+?(?= at )) at (.+?(?=:[0-9]+)):([0-9]+))?");

    QRegularExpressionMatch match = re.match(pstdout);
    bool hasMatch = match.hasMatch();

    if (match.hasMatch())
    {
      bool ok;
      const int filenameGroup = match.lastCapturedIndex() == 8 ? 7 : 3;
      const int lineGroup = match.lastCapturedIndex() == 8 ? 8 : 4;

      const QString filename = match.captured(filenameGroup);
      const int line = match.captured(lineGroup).toInt(&ok, 10);

      std::ifstream inputFile(filename.toStdString() , std::ios_base::in);
      inputFile.seekg(0, std::ios_base::end);
      const size_t fileSize = inputFile.tellg();
      inputFile.seekg(0, std::ios_base::beg);

      std::string tmpString(fileSize, '\0');
      inputFile.read(&tmpString[0], fileSize);

      const QString tooltip = filename+ ":" + line;

      codeTab->setTabText(1, QFileInfo(filename).fileName());
      codeTab->setTabToolTip(1, tooltip);
      codeBrowser->setText(QString::fromStdString(tmpString));

      QTextBlock lineBlock = codeBrowser->document()->findBlockByLineNumber(line - 1);
      codeBrowser->moveCursor(QTextCursor::End);
      codeBrowser->setTextCursor(QTextCursor(lineBlock));

      QTextBlockFormat highlightFormat;
      highlightFormat.setBackground(Qt::yellow);
      highlightFormat.setNonBreakableLines(true);
      highlightFormat.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysBefore);

      QTextCursor newCursor(codeBrowser->textCursor());
      newCursor.setPosition(lineBlock.position());
      newCursor.select(QTextCursor::LineUnderCursor);
      newCursor.setBlockFormat(highlightFormat);
    }
    else
    {
      codeTab->setTabText(1, "Source");
      if (addr2line.isEmpty())
        codeBrowser->setText("addr2line utility is not configured properly, source not available.");
      else
        codeBrowser->setText("Source not found or available.\nIf sources are present, on linux be sure the shell output is in english.\nthis can be achieved by adding LC_ALL=C before the Kronos launch command\n");
    }
  }
}

void UIDebugSH2::updateAll()
{
   updateRegList();
   if (debugSH2)
   {
      sh2regs_struct sh2regs;

      SH2GetRegisters(debugSH2, &sh2regs);
      updateCodeList(sh2regs.PC);
      updateBackTrace();
      updateTrackInfLoop();
      updateCodePage(sh2regs.PC);
   }
}

u32 UIDebugSH2::getRegister(int index, int *size)
{
   sh2regs_struct sh2regs;
   u32 value;

   SH2GetRegisters(debugSH2, &sh2regs);

   if (index < 16)
      value = sh2regs.R[index];
   else
   {
      switch(index)
      {
         case 16:
            value = sh2regs.SR.all;
            break;
         case 17:
            value = sh2regs.GBR;
            break;
         case 18:
            value = sh2regs.VBR;
            break;
         case 19:
            value = sh2regs.MACH;
            break;
         case 20:
            value = sh2regs.MACL;
            break;
         case 21:
            value = sh2regs.PR;
            break;
         case 22:
            value = sh2regs.PC;
            break;
			default:
				value = 0;
				break;
      }
   }

   *size = 4;
   return value;
}

void UIDebugSH2::setRegister(int index, u32 value)
{
   sh2regs_struct sh2regs;

   SH2GetRegisters(debugSH2, &sh2regs);

   if (index < 16)
      sh2regs.R[index] = value;
   else
   {
      switch(index)
      {
         case 16:
            sh2regs.SR.all = value;
            break;
         case 17:
            sh2regs.GBR = value;
            break;
         case 18:
            sh2regs.VBR = value;
            break;
         case 19:
            sh2regs.MACH = value;
            break;
         case 20:
            sh2regs.MACL = value;
            break;
         case 21:
            sh2regs.PR = value;
            break;
         case 22:
            sh2regs.PC = value;
            updateCodeList(sh2regs.PC);
            break;
      }
   }

   SH2SetRegisters(debugSH2, &sh2regs);
}

bool UIDebugSH2::addCodeBreakpoint(u32 addr)
{
	if (!debugSH2)
		return false;
   return SH2AddCodeBreakpoint(debugSH2, addr) == 0;
}

bool UIDebugSH2::delCodeBreakpoint(u32 addr)
{
	if (!debugSH2)
		return false;
    return SH2DelCodeBreakpoint(debugSH2, addr) == 0;
}

bool UIDebugSH2::addMemoryBreakpoint(u32 addr, u32 flags)
{
	if (!debugSH2)
		return false;
   return SH2AddMemoryBreakpoint(debugSH2, addr, flags) == 0;
}

bool UIDebugSH2::delMemoryBreakpoint(u32 addr)
{
	if (!debugSH2)
		return false;
    return SH2DelMemoryBreakpoint(debugSH2, addr) == 0;
}

u32 UIDebugSH2::getMemoryBreakpointFlags(u32 addr)
{
	if (!debugSH2)
		return 0;

	const memorybreakpoint_struct *mbp = SH2GetMemoryBreakpointList(debugSH2);
	for (int i = 0; i < MAX_BREAKPOINTS; i++)
	{
		if (mbp[i].addr == addr)
			return mbp[i].flags;
	}
	return 0;
}

void UIDebugSH2::stepInto()
{
   if (debugSH2)
   {
      SH2Step(debugSH2);
      updateAll();
   }
}

void UIDebugSH2::stepOver()
{
   if (debugSH2)
   {
      if (SH2StepOver(debugSH2, (void (*)(void *, u32, void *))SH2BreakpointHandler) == 0)
         updateAll();
      else
         // Close dialog and wait
         this->accept();
   }

}

void UIDebugSH2::stepOut()
{
   if (debugSH2)
   {
      SH2StepOut(debugSH2, (void (*)(void *, u32, void *))SH2BreakpointHandler);

      // Close dialog and wait
      this->accept();
   }

}

void UIDebugSH2::reserved1()
{
   if (debugSH2)
   {
      if (!debugSH2->trackInfLoop.enabled)
      {
         SH2TrackInfLoopStart(debugSH2);
         pbReserved1->setText(QtYabause::translate("Loop Track Stop"));
      }
      else
      {
         SH2TrackInfLoopStop(debugSH2);
         pbReserved1->setText(QtYabause::translate("Loop Track Start"));

         // Auto-save results to a CSV file next to the executable, since
         // the results table doesn't support copy/paste. Sorted by count
         // descending so the hottest loop addresses are at the top.
         {
            tilInfo_struct *match = debugSH2->trackInfLoop.match;
            int num = debugSH2->trackInfLoop.num;
            std::vector<tilInfo_struct> sorted(match, match + num);
            std::sort(sorted.begin(), sorted.end(),
               [](const tilInfo_struct &a, const tilInfo_struct &b) {
                  return a.count > b.count;
               });

            const char * procName = (debugSH2 == MSH2) ? "MSH2" : "SSH2";
            std::stringstream filename;
            filename << "trackinfloop_" << procName << ".csv";

            std::ofstream out(filename.str(), std::ios_base::out | std::ios_base::trunc);
            if (out.is_open())
            {
               out << "Address,Count\n";
               for (int i = 0; i < num; i++)
               {
                  out << std::hex << std::uppercase << std::setfill('0') << std::setw(8)
                      << sorted[i].addr << std::dec << "," << sorted[i].count << "\n";
               }
               out.close();
               YuiMsg("Track Inf Loop results saved to %s\n", filename.str().c_str());
            }
            else
            {
               YuiMsg("Failed to open %s for writing Track Inf Loop results\n", filename.str().c_str());
            }
         }
      }
   }
}

void UIDebugSH2::reserved2()
{
   if (debugSH2)
      SH2TrackInfLoopClear(debugSH2);
   updateAll();
}

void UIDebugSH2::reserved3()
{
	if (debugSH2)
	{
		bool ok;

		for(;;)
		{
			QString text = QInputDialog::getText(this, QtYabause::translate("Assembly code"),
				QtYabause::translate("Enter new assembly code") + ":", QLineEdit::Normal,
				QString(), &ok);

			if (ok && !text.isEmpty())
			{
				char errorMsg[512];
				int op = sh2iasm(text.toLatin1().data(), errorMsg);
				if (op != 0)
				{
					SH2MappedMemoryWriteWord(debugSH2, debugSH2->regs.PC, op);
					break;
				}
				else
					QMessageBox::critical(QApplication::activeWindow(), QtYabause::translate("Error"), QString(errorMsg));
			}
			else if (!ok)
				break;
		}
	}
	updateAll();
}

void UIDebugSH2::reserved4()
{
   if (!debugSH2)
      return;

   QString path;
   if (SH2DebugExportToFile(debugSH2, QString(), &path))
   {
      YuiMsg("SH2 debug exported to %s\n", path.toStdString().c_str());
      QMessageBox::information(this,
         QtYabause::translate("Export Debug"),
         QtYabause::translate("Report written to:") + QString("\n") + path +
         QString("\n\n") +
         QtYabause::translate("Change the folder with the Debug/SH2DumpDir "
                              "setting or the KRONOS_SH2DUMP_DIR variable."));
   }
   else
   {
      QMessageBox::critical(this,
         QtYabause::translate("Export Debug"),
         QtYabause::translate("Could not write the report file."));
   }
}

void UIDebugSH2::reserved5()
{
   if (!debugSH2)
      return;

   stackPanelPinned = !stackPanelPinned;

   if (stackPanelPinned)
   {
      pbReserved5->setText(QtYabause::translate("Stack Walk Off"));
      codeTab->setTabText(1, QtYabause::translate("Stack"));
      codeBrowser->setPlainText(SH2DebugBuildStackPanel(debugSH2));
      codeTab->setCurrentIndex(1);
   }
   else
   {
      pbReserved5->setText(QtYabause::translate("Stack Walk"));
      sh2regs_struct sh2regs;
      SH2GetRegisters(debugSH2, &sh2regs);
      updateCodePage(sh2regs.PC);
   }
}
