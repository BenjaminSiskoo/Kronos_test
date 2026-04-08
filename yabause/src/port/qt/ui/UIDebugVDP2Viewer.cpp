/*  Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>

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

#include "UIDebugVDP2Viewer.h"
#include "CommonDialogs.h"
#include "ygl.h"

#include <QImageWriter>
#include <QGraphicsPixmapItem>
#include <sstream>
#include <iomanip>

extern "C" {
#include "vdp2.h"
}

// ===========================================================================
//  Helper macros for formatted output
// ===========================================================================
#define HEX4(v) std::hex << std::uppercase << std::setw(4) << std::setfill('0') << (unsigned)(v)
#define HEX2(v) std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (unsigned)(v)
#define DEC(v)  std::dec << (v)

// ===========================================================================
//  updateVdp2Registers
//  Fills the two panes:
//    - pteRawRegs   : one line per register  "ADDR  NAME = 0xVALUE"
//    - pteDecodedRegs : human-readable decoded fields grouped by topic
// ===========================================================================
void UIDebugVDP2Viewer::updateVdp2Registers()
{
    if (!Vdp2Regs) {
        pteRawRegs->setPlainText("VDP2 not initialised");
        pteDecodedRegs->setPlainText("VDP2 not initialised");
        return;
    }

    const Vdp2 &r = *Vdp2Regs;

    // -----------------------------------------------------------------------
    // Left pane: raw values
    // -----------------------------------------------------------------------
    std::ostringstream raw;
    raw << "Addr     Name     Value\n";
    raw << "-------- -------- ------\n";

#define RAW(addr, name, val) \
    raw << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << (addr) \
        << "  " << std::left << std::setw(8) << std::setfill(' ') << (name) \
        << " = 0x" << HEX4(val) << "\n"

    RAW(0x25F80000, "TVMD",   r.TVMD);
    RAW(0x25F80002, "EXTEN",  r.EXTEN);
    RAW(0x25F80004, "TVSTAT", r.TVSTAT);
    RAW(0x25F80006, "VRSIZE", r.VRSIZE);
    RAW(0x25F80008, "HCNT",   r.HCNT);
    RAW(0x25F8000A, "VCNT",   r.VCNT);
    RAW(0x25F8000E, "RAMCTL", r.RAMCTL);
    RAW(0x25F80010, "CYCA0L", r.CYCA0L);
    RAW(0x25F80012, "CYCA0U", r.CYCA0U);
    RAW(0x25F80014, "CYCA1L", r.CYCA1L);
    RAW(0x25F80016, "CYCA1U", r.CYCA1U);
    RAW(0x25F80018, "CYCB0L", r.CYCB0L);
    RAW(0x25F8001A, "CYCB0U", r.CYCB0U);
    RAW(0x25F8001C, "CYCB1L", r.CYCB1L);
    RAW(0x25F8001E, "CYCB1U", r.CYCB1U);
    RAW(0x25F80020, "BGON",   r.BGON);
    RAW(0x25F80022, "MZCTL",  r.MZCTL);
    RAW(0x25F80024, "SFSEL",  r.SFSEL);
    RAW(0x25F80026, "SFCODE", r.SFCODE);
    RAW(0x25F80028, "CHCTLA", r.CHCTLA);
    RAW(0x25F8002A, "CHCTLB", r.CHCTLB);
    RAW(0x25F8002C, "BMPNA",  r.BMPNA);
    RAW(0x25F8002E, "BMPNB",  r.BMPNB);
    RAW(0x25F80030, "PNCN0",  r.PNCN0);
    RAW(0x25F80032, "PNCN1",  r.PNCN1);
    RAW(0x25F80034, "PNCN2",  r.PNCN2);
    RAW(0x25F80036, "PNCN3",  r.PNCN3);
    RAW(0x25F80038, "PNCR",   r.PNCR);
    RAW(0x25F8003A, "PLSZ",   r.PLSZ);
    RAW(0x25F8003C, "MPOFN",  r.MPOFN);
    RAW(0x25F8003E, "MPOFR",  r.MPOFR);
    RAW(0x25F80040, "MPABN0", r.MPABN0);
    RAW(0x25F80042, "MPCDN0", r.MPCDN0);
    RAW(0x25F80044, "MPABN1", r.MPABN1);
    RAW(0x25F80046, "MPCDN1", r.MPCDN1);
    RAW(0x25F80048, "MPABN2", r.MPABN2);
    RAW(0x25F8004A, "MPCDN2", r.MPCDN2);
    RAW(0x25F8004C, "MPABN3", r.MPABN3);
    RAW(0x25F8004E, "MPCDN3", r.MPCDN3);
    RAW(0x25F80070, "SCXIN0", r.SCXIN0);
    RAW(0x25F80072, "SCXDN0", r.SCXDN0);
    RAW(0x25F80074, "SCYIN0", r.SCYIN0);
    RAW(0x25F80076, "SCYDN0", r.SCYDN0);
    RAW(0x25F80080, "SCXIN1", r.SCXIN1);
    RAW(0x25F80082, "SCXDN1", r.SCXDN1);
    RAW(0x25F80084, "SCYIN1", r.SCYIN1);
    RAW(0x25F80086, "SCYDN1", r.SCYDN1);
    RAW(0x25F80090, "SCXN2",  r.SCXN2);
    RAW(0x25F80092, "SCYN2",  r.SCYN2);
    RAW(0x25F80094, "SCXN3",  r.SCXN3);
    RAW(0x25F80096, "SCYN3",  r.SCYN3);
    RAW(0x25F80098, "ZMCTL",  r.ZMCTL);
    RAW(0x25F8009A, "SCRCTL", r.SCRCTL);
    RAW(0x25F8009C, "VCSTAU", r.VCSTA.part.U);
    RAW(0x25F8009E, "VCstal", r.VCSTA.part.L);
    RAW(0x25F800A0, "LSTA0U", r.LSTA0.part.U);
    RAW(0x25F800A2, "LSTA0L", r.LSTA0.part.L);
    RAW(0x25F800A4, "LSTA1U", r.LSTA1.part.U);
    RAW(0x25F800A6, "LSTA1L", r.LSTA1.part.L);
    RAW(0x25F800A8, "LCTAU",  r.LCTA.part.U);
    RAW(0x25F800AA, "LCTAL",  r.LCTA.part.L);
    RAW(0x25F800AC, "BKTAU",  r.BKTAU);
    RAW(0x25F800AE, "BKTAL",  r.BKTAL);
    RAW(0x25F800B0, "RPMD",   r.RPMD);
    RAW(0x25F800B2, "RPRCTL", r.RPRCTL);
    RAW(0x25F800B4, "KTCTL",  r.KTCTL);
    RAW(0x25F800B6, "KTAOF",  r.KTAOF);
    RAW(0x25F800B8, "OVPNRA", r.OVPNRA);
    RAW(0x25F800BA, "OVPNRB", r.OVPNRB);
    RAW(0x25F800BC, "RPTAU",  r.RPTA.part.U);
    RAW(0x25F800BE, "RPTAL",  r.RPTA.part.L);
    RAW(0x25F800C0, "WPSX0",  r.WPSX0);
    RAW(0x25F800C2, "WPSY0",  r.WPSY0);
    RAW(0x25F800C4, "WPEX0",  r.WPEX0);
    RAW(0x25F800C6, "WPEY0",  r.WPEY0);
    RAW(0x25F800C8, "WPSX1",  r.WPSX1);
    RAW(0x25F800CA, "WPSY1",  r.WPSY1);
    RAW(0x25F800CC, "WPEX1",  r.WPEX1);
    RAW(0x25F800CE, "WPEY1",  r.WPEY1);
    RAW(0x25F800D0, "WCTLA",  r.WCTLA);
    RAW(0x25F800D2, "WCTLB",  r.WCTLB);
    RAW(0x25F800D4, "WCTLC",  r.WCTLC);
    RAW(0x25F800D6, "WCTLD",  r.WCTLD);
    RAW(0x25F800D8, "LWTA0U", r.LWTA0.part.U);
    RAW(0x25F800DA, "LWTA0L", r.LWTA0.part.L);
    RAW(0x25F800DC, "LWTA1U", r.LWTA1.part.U);
    RAW(0x25F800DE, "LWTA1L", r.LWTA1.part.L);
    RAW(0x25F800E0, "SPCTL",  r.SPCTL);
    RAW(0x25F800E2, "SDCTL",  r.SDCTL);
    RAW(0x25F800E4, "CRAOFA", r.CRAOFA);
    RAW(0x25F800E6, "CRAOFB", r.CRAOFB);
    RAW(0x25F800E8, "LNCLEN", r.LNCLEN);
    RAW(0x25F800EA, "SFPRMD", r.SFPRMD);
    RAW(0x25F800EC, "CCCTL",  r.CCCTL);
    RAW(0x25F800EE, "SFCCMD", r.SFCCMD);
    RAW(0x25F800F0, "PRISA",  r.PRISA);
    RAW(0x25F800F2, "PRISB",  r.PRISB);
    RAW(0x25F800F4, "PRISC",  r.PRISC);
    RAW(0x25F800F6, "PRISD",  r.PRISD);
    RAW(0x25F800F8, "PRINA",  r.PRINA);
    RAW(0x25F800FA, "PRINB",  r.PRINB);
    RAW(0x25F800FC, "PRIR",   r.PRIR);
    RAW(0x25F80100, "CCRSA",  r.CCRSA);
    RAW(0x25F80102, "CCRSB",  r.CCRSB);
    RAW(0x25F80104, "CCRSC",  r.CCRSC);
    RAW(0x25F80106, "CCRSD",  r.CCRSD);
    RAW(0x25F80108, "CCRNA",  r.CCRNA);
    RAW(0x25F8010A, "CCRNB",  r.CCRNB);
    RAW(0x25F8010C, "CCRR",   r.CCRR);
    RAW(0x25F8010E, "CCRLB",  r.CCRLB);
    RAW(0x25F80110, "CLOFEN", r.CLOFEN);
    RAW(0x25F80112, "CLOFSL", r.CLOFSL);
    RAW(0x25F80114, "COAR",   r.COAR);
    RAW(0x25F80116, "COAG",   r.COAG);
    RAW(0x25F80118, "COAB",   r.COAB);
    RAW(0x25F8011A, "COBR",   r.COBR);
    RAW(0x25F8011C, "COBG",   r.COBG);
    RAW(0x25F8011E, "COBB",   r.COBB);
#undef RAW

    pteRawRegs->setPlainText(QString::fromStdString(raw.str()));

    // -----------------------------------------------------------------------
    // Right pane: decoded
    // -----------------------------------------------------------------------
    std::ostringstream d;

    // --- TVMD ---
    d << "=== TVMD  (TV Mode) = 0x" << HEX4(r.TVMD) << " ===\n";
    {
        int hres = r.TVMD & 0x3;
        int vres = (r.TVMD >> 2) & 0x3;
        int bsmc = (r.TVMD >> 6) & 0x1;
        int lsmd = (r.TVMD >> 8) & 0x3;
        int hreso = (r.TVMD >> 10) & 0x1;  // 0=320/352, 1=640/704
        int disp  = (r.TVMD >> 15) & 0x1;

        d << "  DISP  = " << disp << (disp ? "  (Display ON)\n" : "  (Display OFF)\n");
        d << "  BSMC  = " << bsmc << (bsmc ? "  (VBlank mask off)\n" : "  (VBlank mask on)\n");
        d << "  LSMD[1:0] = " << lsmd << "  -> ";
        switch (lsmd) {
            case 0: d << "Non-interlace\n"; break;
            case 2: d << "Single-density interlace\n"; break;
            case 3: d << "Double-density interlace\n"; break;
            default: d << "Reserved\n"; break;
        }
        d << "  VRES[1:0] = " << vres << "  -> ";
        switch (vres) {
            case 0: d << "224 lines\n"; break;
            case 1: d << "240 lines\n"; break;
            case 2: d << "256 lines\n"; break;
            case 3: d << "480/448 lines (interlace)\n"; break;
        }
        d << "  HRES[1:0] = " << hres << "  -> ";
        static const char* hresNames[] = {"320","352","640","704"};
        d << hresNames[hres & 0x3] << " px";
        if (hreso) d << " (Hi-res)";
        d << "\n";
    }

    // --- EXTEN / TVSTAT ---
    d << "\n=== EXTEN = 0x" << HEX4(r.EXTEN) << " / TVSTAT = 0x" << HEX4(r.TVSTAT) << " ===\n";
    d << "  EXLTEN (ext latch) = " << (r.EXTEN & 1) << "\n";
    d << "  EXSYEN (ext sync)  = " << ((r.EXTEN >> 1) & 1) << "\n";
    d << "  ODD (field status) = " << ((r.TVSTAT >> 1) & 1) << "\n";
    d << "  VBLANK             = " << ((r.TVSTAT >> 3) & 1) << "\n";
    d << "  HBLANK             = " << ((r.TVSTAT >> 2) & 1) << "\n";

    // --- VRSIZE / RAMCTL ---
    d << "\n=== VRAM / RAMCTL ===\n";
    d << "  VRSIZE = 0x" << HEX4(r.VRSIZE)
      << (((r.VRSIZE >> 15) & 1) ? "  (8Mbit VRAM)\n" : "  (4Mbit VRAM)\n");
    d << "  RAMCTL = 0x" << HEX4(r.RAMCTL) << "\n";
    {
        int crmd = (r.RAMCTL >> 12) & 0x3;
        d << "    CRMD (Color RAM mode) = " << crmd << "  -> ";
        switch (crmd) {
            case 0: d << "1024 colours x 16bit (mode 0)\n"; break;
            case 1: d << "2048 colours x 16bit (mode 1)\n"; break;
            case 2: d << "1024 colours x 32bit (mode 2)\n"; break;
            default: d << "Reserved\n"; break;
        }
        d << "    VRBMD (VRAM B mode)   = " << ((r.RAMCTL >> 8) & 1) << "\n";
        d << "    VRAMD (VRAM A mode)   = " << ((r.RAMCTL >> 4) & 1) << "\n";
    }

    // --- BGON (Screen Display Enable) ---
    d << "\n=== BGON  (Display Enable) = 0x" << HEX4(r.BGON) << " ===\n";
    static const char* bgNames[] = {"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1"};
    for (int i = 0; i < 6; i++) {
        int transparent = (r.BGON >> (8 + i)) & 1;
        int enable = (r.BGON >> i) & 1;
        d << "  " << bgNames[i] << " : " << (enable ? "ON " : "OFF")
          << "  transparent=" << transparent << "\n";
    }

    // --- CHCTLA / CHCTLB (Character Control) ---
    d << "\n=== Character Control ===\n";
    d << "  CHCTLA = 0x" << HEX4(r.CHCTLA) << "\n";
    {
        static const char* chaColours[] = {"16 (4bpp)","16 (4bpp)","256 (8bpp)",
                                            "2048 (11bpp)","32768 (15bpp)","16M (24bpp)"};
        int n0cc = (r.CHCTLA >> 4) & 0x7;
        int n1cc = (r.CHCTLA >> 12) & 0x7;
        d << "    NBG0 char colour: " << (n0cc < 6 ? chaColours[n0cc] : "?") << "\n";
        d << "    NBG0 char size  : " << ((r.CHCTLA & 1) ? "16x16" : "8x8") << "\n";
        d << "    NBG0 bitmap     : " << ((r.CHCTLA & 2) ? "Yes" : "No") << "\n";
        d << "    NBG1 char colour: " << (n1cc < 6 ? chaColours[n1cc] : "?") << "\n";
        d << "    NBG1 char size  : " << (((r.CHCTLA >> 8) & 1) ? "16x16" : "8x8") << "\n";
    }
    d << "  CHCTLB = 0x" << HEX4(r.CHCTLB) << "\n";
    {
        int n2cc = r.CHCTLB & 0x3;
        int n3cc = (r.CHCTLB >> 8) & 0x3;
        d << "    NBG2 char colour: " << (n2cc == 0 ? "16 (4bpp)" : "256 (8bpp)") << "\n";
        d << "    NBG2 char size  : " << (((r.CHCTLB >> 2) & 1) ? "16x16" : "8x8") << "\n";
        d << "    NBG3 char colour: " << (n3cc == 0 ? "16 (4bpp)" : "256 (8bpp)") << "\n";
        d << "    NBG3 char size  : " << (((r.CHCTLB >> 10) & 1) ? "16x16" : "8x8") << "\n";
        int r0cc = (r.CHCTLB >> 4) & 0x7;
        static const char* rbgColours[] = {"16 (4bpp)","256 (8bpp)","2048","32768","16M"};
        d << "    RBG0 char colour: " << (r0cc < 5 ? rbgColours[r0cc] : "?") << "\n";
    }

    // --- PLSZ (Plane Size) ---
    d << "\n=== PLSZ  (Plane Size) = 0x" << HEX4(r.PLSZ) << " ===\n";
    {
        static const char* pszNames[] = {"1Hx1V","2Hx1V","2Hx2V","??"};
        d << "  NBG0: " << pszNames[(r.PLSZ) & 0x3] << "\n";
        d << "  NBG1: " << pszNames[(r.PLSZ >> 2) & 0x3] << "\n";
        d << "  NBG2: " << pszNames[(r.PLSZ >> 4) & 0x3] << "\n";
        d << "  NBG3: " << pszNames[(r.PLSZ >> 6) & 0x3] << "\n";
        d << "  RBG0: " << pszNames[(r.PLSZ >> 8) & 0x3] << "\n";
    }

    // --- Scroll positions ---
    d << "\n=== Scroll Positions ===\n";
    d << "  NBG0: X=" << DEC((s16)r.SCXIN0) << "." << ((r.SCXDN0 >> 8) & 0xFF)
      << "  Y=" << DEC((s16)r.SCYIN0) << "." << ((r.SCYDN0 >> 8) & 0xFF) << "\n";
    d << "  NBG1: X=" << DEC((s16)r.SCXIN1) << "." << ((r.SCXDN1 >> 8) & 0xFF)
      << "  Y=" << DEC((s16)r.SCYIN1) << "." << ((r.SCYDN1 >> 8) & 0xFF) << "\n";
    d << "  NBG2: X=" << DEC((s16)r.SCXN2)
      << "  Y=" << DEC((s16)r.SCYN2) << "\n";
    d << "  NBG3: X=" << DEC((s16)r.SCXN3)
      << "  Y=" << DEC((s16)r.SCYN3) << "\n";

    // --- Zoom ---
    d << "\n=== Zoom (ZMCTL=0x" << HEX4(r.ZMCTL) << ") ===\n";
    d << "  NBG0: " << ((r.ZMCTL & 3) == 0 ? "None"
                      : (r.ZMCTL & 3) == 1 ? "H-only"
                      : (r.ZMCTL & 3) == 2 ? "H+V" : "?") << "\n";
    d << "  NBG1: " << (((r.ZMCTL >> 2) & 3) == 0 ? "None"
                      : ((r.ZMCTL >> 2) & 3) == 1 ? "H-only" : "H+V") << "\n";
    d << "  ZMX NBG0: " << DEC(r.ZMXN0.part.I) << "." << DEC(r.ZMXN0.part.D) << "\n";
    d << "  ZMY NBG0: " << DEC(r.ZMYN0.part.I) << "." << DEC(r.ZMYN0.part.D) << "\n";
    d << "  ZMX NBG1: " << DEC(r.ZMXN1.part.I) << "." << DEC(r.ZMXN1.part.D) << "\n";
    d << "  ZMY NBG1: " << DEC(r.ZMYN1.part.I) << "." << DEC(r.ZMYN1.part.D) << "\n";

    // --- Line/vertical-cell scroll control (SCRCTL) ---
    d << "\n=== SCRCTL  (Scroll Control) = 0x" << HEX4(r.SCRCTL) << " ===\n";
    {
        static const char* scrNames[] = {"NBG0","NBG1"};
        for (int i = 0; i < 2; i++) {
            int bits = (r.SCRCTL >> (i * 8)) & 0x3F;
            d << "  " << scrNames[i] << ": ";
            if ((bits >> 0) & 1) d << "line-scroll-X ";
            if ((bits >> 1) & 1) d << "line-scroll-Y ";
            if ((bits >> 2) & 1) d << "vert-cell-scroll ";
            if ((bits >> 3) & 1) d << "line-zoom ";
            if (!(bits & 0xF))   d << "none";
            d << "\n";
        }
    }

    // --- Vertical cell / Line scroll table addresses ---
    d << "\n=== Cell/Line Scroll Table Addresses ===\n";
    {
        u32 vcsta = ((u32)r.VCSTA.part.U << 16) | r.VCSTA.part.L;
        u32 lsta0 = ((u32)r.LSTA0.part.U << 16) | r.LSTA0.part.L;
        u32 lsta1 = ((u32)r.LSTA1.part.U << 16) | r.LSTA1.part.L;
        u32 lcta  = ((u32)r.LCTA.part.U  << 16) | r.LCTA.part.L;
        d << std::hex << std::uppercase;
        d << "  VCSTA  (V-cell scroll 0x25F8009C) = 0x" << vcsta << "\n";
        d << "  LSTA0  (Line scroll  0x25F800A0) = 0x" << lsta0 << "\n";
        d << "  LSTA1  (Line scroll  0x25F800A4) = 0x" << lsta1 << "\n";
        d << "  LCTA   (Line colour  0x25F800A8) = 0x" << lcta  << "\n";
        d << std::dec;
    }

    // --- Back Screen & RBG Rotation ---
    d << "\n=== Back Screen / RBG Rotation ===\n";
    {
        u32 bkta = ((u32)(r.BKTAU & 0x7) << 16) | r.BKTAL;
        d << std::hex << std::uppercase;
        d << "  BKTA   (Back screen 0x25F800AC) = 0x" << bkta;
        d << ((r.BKTAU & 0x8000) ? "  (single colour)\n" : "  (colour per line)\n");

        d << "  RPMD   (Rot. param mode 0x25F800B0) = 0x" << HEX4(r.RPMD) << "  -> ";
        switch (r.RPMD & 0x3) {
            case 0: d << "Param A only\n"; break;
            case 1: d << "Param B only\n"; break;
            case 2: d << "Switch by window\n"; break;
            case 3: d << "Switch by sprite MSB\n"; break;
        }

        d << "  RPRCTL (Rot. read ctrl 0x25F800B2) = 0x" << HEX4(r.RPRCTL) << "\n";
        d << "    Param A: ";
        if ((r.RPRCTL >> 0) & 1) d << "Xst ";
        if ((r.RPRCTL >> 1) & 1) d << "Yst ";
        if ((r.RPRCTL >> 2) & 1) d << "KAst ";
        if ((r.RPRCTL >> 3) & 1) d << "KAst-inc ";
        d << "\n";
        d << "    Param B: ";
        if ((r.RPRCTL >> 8)  & 1) d << "Xst ";
        if ((r.RPRCTL >> 9)  & 1) d << "Yst ";
        if ((r.RPRCTL >> 10) & 1) d << "KAst ";
        if ((r.RPRCTL >> 11) & 1) d << "KAst-inc ";
        d << "\n";

        d << "  KTCTL  (Coeff table ctrl 0x25F800B4) = 0x" << HEX4(r.KTCTL) << "\n";
        d << std::dec;
        d << "    Param A coeff enable = " << (r.KTCTL & 1 ? "yes" : "no")
          << "  mode = " << ((r.KTCTL >> 2) & 0x7) << "\n";
        d << "    Param B coeff enable = " << ((r.KTCTL >> 8) & 1 ? "yes" : "no")
          << "  mode = " << ((r.KTCTL >> 10) & 0x7) << "\n";
        d << "  KTAOF  (Coeff addr off 0x25F800B6) = "
          << "A=" << (r.KTAOF & 0x7) << "  B=" << ((r.KTAOF >> 8) & 0x7) << "\n";

        d << std::hex;
        d << "  OVPNRA (Over-pattern RBG0 0x25F800B8) = 0x" << HEX4(r.OVPNRA) << "\n";
        d << "  OVPNRB (Over-pattern RBG1 0x25F800BA) = 0x" << HEX4(r.OVPNRB) << "\n";
        u32 rpta = ((u32)r.RPTA.part.U << 16) | r.RPTA.part.L;
        d << "  RPTA   (Rot. param table 0x25F800BC) = 0x" << rpta << "\n";
        d << std::dec;
    }

    // --- Windows ---
    d << "\n=== Windows ===\n";
    d << "  W0: (" << DEC((s16)r.WPSX0) << "," << DEC((s16)r.WPSY0) << ") - ("
      << DEC((s16)r.WPEX0) << "," << DEC((s16)r.WPEY0) << ")\n";
    d << "  W1: (" << DEC((s16)r.WPSX1) << "," << DEC((s16)r.WPSY1) << ") - ("
      << DEC((s16)r.WPEX1) << "," << DEC((s16)r.WPEY1) << ")\n";
    d << "  WCTLA=0x" << HEX4(r.WCTLA) << "  WCTLB=0x" << HEX4(r.WCTLB)
      << "  WCTLC=0x" << HEX4(r.WCTLC) << "  WCTLD=0x" << HEX4(r.WCTLD) << "\n";
    {
        u32 lwta0 = ((u32)r.LWTA0.part.U << 16) | r.LWTA0.part.L;
        u32 lwta1 = ((u32)r.LWTA1.part.U << 16) | r.LWTA1.part.L;
        d << std::hex << std::uppercase;
        d << "  LWTA0  (Line window A 0x25F800D8) = 0x" << lwta0 << "\n";
        d << "  LWTA1  (Line window B 0x25F800DC) = 0x" << lwta1 << "\n";
        d << std::dec;
    }

    // --- Sprite Control ---
    d << "\n=== SPCTL  (Sprite Control) = 0x" << HEX4(r.SPCTL) << " ===\n";
    {
        int sptype = r.SPCTL & 0xF;
        d << "  Sprite type      = " << DEC(sptype) << "\n";
        int spcc   = (r.SPCTL >> 4) & 0x7;
        static const char* spccNames[] = {"16 (4bpp)","256 (8bpp)","32768 (15bpp)","16M (24bpp)",
                                           "16 bank","256 bank","32768 bank","16M bank"};
        d << "  Sprite col. mode = " << (spcc < 8 ? spccNames[spcc] : "?") << "\n";
        d << "  SPWINEN (sprite window) = " << ((r.SPCTL >> 11) & 1) << "\n";
        d << "  SPCLMD  (shadow en)     = " << ((r.SPCTL >> 12) & 1) << "\n";
    }

    // --- Shadow Control ---
    d << "  SDCTL=0x" << HEX4(r.SDCTL)
      << "  (MSB shadow: " << (r.SDCTL & 1 ? "on" : "off") << ")\n";

    // --- Color Offset ---
    d << "\n=== Color Offset Enable ===\n";
    d << "  CLOFEN=0x" << HEX4(r.CLOFEN) << "  CLOFSL=0x" << HEX4(r.CLOFSL) << "\n";
    auto signedOffset = [](u16 v) -> int {
        // 9-bit signed
        int val = v & 0x1FF;
        if (val & 0x100) val -= 0x200;
        return val;
    };
    d << "  Bank A: R=" << DEC(signedOffset(r.COAR))
      << " G=" << DEC(signedOffset(r.COAG))
      << " B=" << DEC(signedOffset(r.COAB)) << "\n";
    d << "  Bank B: R=" << DEC(signedOffset(r.COBR))
      << " G=" << DEC(signedOffset(r.COBG))
      << " B=" << DEC(signedOffset(r.COBB)) << "\n";

    // --- Priority ---
    d << "\n=== Priority Numbers ===\n";
    d << "  NBG0=" << DEC(r.PRINA & 0x7)
      << "  NBG1=" << DEC((r.PRINA >> 8) & 0x7)
      << "  NBG2=" << DEC(r.PRISB & 0x7)
      << "  NBG3=" << DEC((r.PRISB >> 8) & 0x7) << "\n";
    d << "  RBG0=" << DEC(r.PRISC & 0x7)
      << "  RBG1=" << DEC((r.PRISC >> 8) & 0x7) << "\n";
    d << "  SP0=" << DEC(r.PRISA & 0x7)
      << "  SP1=" << DEC((r.PRISA >> 8) & 0x7)
      << "  SP2=" << DEC(r.PRISB & 0x7)   // note: PRISB bits repeated intentionally for Sprite 2/3
      << "  SP3=" << DEC((r.PRISB >> 8) & 0x7) << "\n";

    // --- Color Calc ---
    d << "\n=== Color Calculation (CCCTL=0x" << HEX4(r.CCCTL) << ") ===\n";
    d << "  BOKEN (Blur)   = " << ((r.CCCTL >> 9) & 1) << "\n";
    d << "  EXCCEN         = " << ((r.CCCTL >> 8) & 1) << "\n";
    d << "  Enabled planes :\n";
    static const char* ccPlanes[] = {"SP","RBG1","RBG0","NBG3","NBG2","NBG1","NBG0","Back"};
    for (int i = 0; i < 8; i++) {
        if ((r.CCCTL >> i) & 1)
            d << "    " << ccPlanes[i] << "\n";
    }
    d << "  CC Ratios (0=0%, 31=100%): "
      << "NBG0=" << DEC(r.CCRSA & 0x1F)
      << " NBG1=" << DEC((r.CCRSA >> 8) & 0x1F)
      << " NBG2=" << DEC(r.CCRSB & 0x1F)
      << " NBG3=" << DEC((r.CCRSB >> 8) & 0x1F) << "\n";
    d << "  RBG0=" << DEC(r.CCRSC & 0x1F)
      << " RBG1=" << DEC((r.CCRSC >> 8) & 0x1F)
      << " Sprite=" << DEC(r.CCRNA & 0x1F) << "\n";

    // --- MOSAIC ---
    d << "\n=== MZCTL  (Mosaic) = 0x" << HEX4(r.MZCTL) << " ===\n";
    {
        int mzh = (r.MZCTL >> 8) & 0xF;
        int mzv = (r.MZCTL >> 12) & 0xF;
        d << "  H size=" << DEC(mzh+1) << "  V size=" << DEC(mzv+1) << "\n";
        static const char* mzPlanes[] = {"NBG0","NBG1","NBG2","NBG3","RBG0"};
        for (int i = 0; i < 5; i++)
            if ((r.MZCTL >> i) & 1)
                d << "  " << mzPlanes[i] << " mosaic ON\n";
    }

    // --- RAMCTL VRAM cycle patterns summary ---
    d << "\n=== VRAM Cycle Patterns ===\n";
    auto decodeVramTiming = [](u8 nibble) -> const char* {
        switch (nibble & 0xF) {
            case 0x0: return "NBG0 PNT";
            case 0x1: return "NBG1 PNT";
            case 0x2: return "NBG2 PNT";
            case 0x3: return "NBG3 PNT";
            case 0x4: return "NBG0 CPT";
            case 0x5: return "NBG1 CPT";
            case 0x6: return "NBG2 CPT";
            case 0x7: return "NBG3 CPT";
            case 0x8: return "RBG0 PNT";
            case 0x9: return "RBG1 PNT";
            case 0xA: return "RBG0 CPT";
            case 0xB: return "RBG1 CPT";
            case 0xC: return "CPU-RW";
            case 0xD: return "VDP2-RW";
            case 0xF: return "---";
            default: return "???";
        }
    };
    auto printCyc = [&](const char* bank, u16 L, u16 U) {
        d << "  " << bank << ": T0=" << decodeVramTiming((L>>12)&0xF)
          << " T1=" << decodeVramTiming((L>>8)&0xF)
          << " T2=" << decodeVramTiming((L>>4)&0xF)
          << " T3=" << decodeVramTiming(L&0xF) << "\n";
        d << "        T4=" << decodeVramTiming((U>>12)&0xF)
          << " T5=" << decodeVramTiming((U>>8)&0xF)
          << " T6=" << decodeVramTiming((U>>4)&0xF)
          << " T7=" << decodeVramTiming(U&0xF) << "\n";
    };
    printCyc("VRAMA0", r.CYCA0L, r.CYCA0U);
    printCyc("VRAMA1", r.CYCA1L, r.CYCA1U);
    printCyc("VRAMB0", r.CYCB0L, r.CYCB0U);
    printCyc("VRAMB1", r.CYCB1L, r.CYCB1U);

    // --- H/V counters ---
    d << "\n=== H/V Counters ===\n";
    d << "  HCNT = " << DEC(r.HCNT) << "  VCNT = " << DEC(r.VCNT) << "\n";

    pteDecodedRegs->setPlainText(QString::fromStdString(d.str()));
}

// ===========================================================================
// Existing viewer methods
// ===========================================================================
void UIDebugVDP2Viewer::clearItems()
{
    while (cbScreen->count() != 0)
        cbScreen->removeItem(0);
}

void UIDebugVDP2Viewer::addItem(int id)
{
    switch(id) {
        case NBG0:   cbScreen->addItem("NBG0",   NBG0);   break;
        case NBG1:   cbScreen->addItem("NBG1",   NBG1);   break;
        case NBG2:   cbScreen->addItem("NBG2",   NBG2);   break;
        case NBG3:   cbScreen->addItem("NBG3",   NBG3);   break;
        case RBG0:   cbScreen->addItem("RBG0",   RBG0);   break;
        case RBG1:   cbScreen->addItem("RBG1",   RBG1);   break;
        case SPRITE: cbScreen->addItem("SPRITE", SPRITE); break;
        default: break;
    }
}

int UIDebugVDP2Viewer::exec()
{
    return QDialog::exec();
}

UIDebugVDP2Viewer::UIDebugVDP2Viewer( QWidget* p )
    : QDialog( p )
{
    setupUi(this);

    QGraphicsScene *scene = new QGraphicsScene(this);
    gvScreen->setScene(scene);

    vdp2texture = NULL;
    width  = 0;
    height = 0;

    QtYabause::retranslateWidget(this);
}

void UIDebugVDP2Viewer::displayCurrentScreen()
{
    if (!Vdp2Regs)
        return;

    int index = cbScreen->itemData(cbScreen->currentIndex()).toInt();

    if (vdp2texture != NULL) free(vdp2texture);

    vdp2texture = Vdp2DebugTexture(index, &width, &height);
    if (vdp2texture != NULL) {
        pbSaveAsBitmap->setEnabled(true);

        QGraphicsScene *scene = gvScreen->scene();
        QImage::Format format = cbOpaque->isChecked()
            ? QImage::Format_RGB32 : QImage::Format_ARGB32;
        bool YMirrored = (index != SPRITE);
        QImage img((uchar *)vdp2texture, width, height, format);
        QPixmap pixmap = QPixmap::fromImage(img.mirrored(false, YMirrored).rgbSwapped());
        scene->clear();
        scene->setBackgroundBrush(Qt::Dense7Pattern);
        scene->addPixmap(pixmap);
        scene->setSceneRect(scene->itemsBoundingRect());
    }
}

void UIDebugVDP2Viewer::refresh()
{
    displayCurrentScreen();
    updateVdp2Registers();
}

void UIDebugVDP2Viewer::on_cbScreen_currentIndexChanged(int)
{
    displayCurrentScreen();
}

void UIDebugVDP2Viewer::showEvent(QShowEvent *)
{
    gvScreen->fitInView(gvScreen->scene()->sceneRect());
    updateVdp2Registers();
}

void UIDebugVDP2Viewer::on_tabWidget_currentChanged(int index)
{
    if (index == 1)   // Registers tab
        updateVdp2Registers();
}

void UIDebugVDP2Viewer::on_pbSaveAsBitmap_clicked()
{
    QStringList filters;
    int index = cbScreen->itemData(cbScreen->currentIndex()).toInt();
    foreach (QByteArray ba, QImageWriter::supportedImageFormats())
        if (!filters.contains(ba, Qt::CaseInsensitive))
            filters << QString(ba).toLower();
    for (int i = 0; i < filters.count(); i++)
        filters[i] = QtYabause::translate("%1 Images (*.%2)")
                         .arg(filters[i].toUpper()).arg(filters[i]);

    if (!vdp2texture) return;

    QImage::Format format = cbOpaque->isChecked()
        ? QImage::Format_RGB32 : QImage::Format_ARGB32;
    bool YMirrored = (index != SPRITE);
    QImage img((uchar *)vdp2texture, width, height, format);
    img = img.mirrored(false, YMirrored).rgbSwapped();

    const QString s = CommonDialogs::getSaveFileName(
        QString(), QtYabause::translate("Choose a location for your bitmap"),
        filters.join(";;"));

    if (!s.isEmpty())
        if (!img.save(s))
            CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}

void UIDebugVDP2Viewer::on_cbOpaque_toggled(bool)
{
    displayCurrentScreen();
}
