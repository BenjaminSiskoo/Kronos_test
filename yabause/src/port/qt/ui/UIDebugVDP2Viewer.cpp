/*  Copyright 2012 Theo Berkau <cwx@cyberwarriorx.com>
    This file is part of Yabause. (GPL v2+) */

#include "UIDebugVDP2Viewer.h"
#include "CommonDialogs.h"
#include "ygl.h"

#include <QImageWriter>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <sstream>
#include <iomanip>
#include <algorithm>

extern "C" {
#include "vdp1.h"
#include "vdp2.h"
#include "vdp2debug.h"
#include "memory.h"
#include "yabause.h"
}

// ============================================================
//  Helpers
// ============================================================
#define HEX4(v) std::hex<<std::uppercase<<std::setw(4)<<std::setfill('0')<<(unsigned)(v)
#define HEX8(v) std::hex<<std::uppercase<<std::setw(8)<<std::setfill('0')<<(unsigned)(v)
#define DEC(v)  std::dec<<(v)

static std::string decodeVramTiming(u8 n) {
    switch(n&0xF){
        case 0x0:return"NBG0PN"; case 0x1:return"NBG1PN";
        case 0x2:return"NBG2PN"; case 0x3:return"NBG3PN";
        case 0x4:return"NBG0CP"; case 0x5:return"NBG1CP";
        case 0x6:return"NBG2CP"; case 0x7:return"NBG3CP";
        case 0x8:return"RBG0PN"; case 0x9:return"RBG1PN";
        case 0xA:return"RBG0CP"; case 0xB:return"RBG1CP";
        case 0xC:return"CPU-RW"; case 0xD:return"VDP-RW";
        case 0xF:return"------"; default: return"??????";
    }
}

// (correct BGR555 -> RGB)
static QColor cramColor(int idx) {
    if (!Vdp2ColorRam || !Vdp2Regs) return Qt::black;
    int mode = (Vdp2Regs->RAMCTL>>12)&3;
    u16 w = (mode<=1) ? T2ReadWord(Vdp2ColorRam,(idx*2)&0xFFF)
                      : T2ReadWord(Vdp2ColorRam,(idx*4)&0xFFF);
    int r = ( w        & 0x1F) << 3;  // bits  4- 0 = Red
    int g = ((w >>  5) & 0x1F) << 3;  // bits  9- 5 = Green
    int b = ((w >> 10) & 0x1F) << 3;  // bits 14-10 = Blue
    return QColor(r, g, b);
}

// ============================================================
//  updateVdp2Registers  (raw + decoded)
// ============================================================
void UIDebugVDP2Viewer::updateVdp2Registers()
{
    if (!Vdp2Regs) {
        pteRawRegs->setPlainText("VDP2 not initialised");
        pteDecodedRegs->setPlainText("VDP2 not initialised");
        return;
    }
    const Vdp2 &r = *Vdp2Regs;

    /* ---- RAW pane ---- */
    std::ostringstream raw;
    raw<<"Address    Name     Value\n"
       <<"---------- -------- ------\n";
#define R(a,n,v) raw<<"0x"<<HEX8(a)<<"  "<<std::left<<std::setw(8)<<std::setfill(' ')<<(n)<<" = 0x"<<HEX4(v)<<"\n"
    R(0x25F80000,"TVMD",  r.TVMD);   R(0x25F80002,"EXTEN", r.EXTEN);
    R(0x25F80004,"TVSTAT",r.TVSTAT); R(0x25F80006,"VRSIZE",r.VRSIZE);
    R(0x25F80008,"HCNT",  r.HCNT);   R(0x25F8000A,"VCNT",  r.VCNT);
    R(0x25F8000E,"RAMCTL",r.RAMCTL);
    R(0x25F80010,"CYCA0L",r.CYCA0L); R(0x25F80012,"CYCA0U",r.CYCA0U);
    R(0x25F80014,"CYCA1L",r.CYCA1L); R(0x25F80016,"CYCA1U",r.CYCA1U);
    R(0x25F80018,"CYCB0L",r.CYCB0L); R(0x25F8001A,"CYCB0U",r.CYCB0U);
    R(0x25F8001C,"CYCB1L",r.CYCB1L); R(0x25F8001E,"CYCB1U",r.CYCB1U);
    R(0x25F80020,"BGON",  r.BGON);   R(0x25F80022,"MZCTL", r.MZCTL);
    R(0x25F80024,"SFSEL", r.SFSEL);  R(0x25F80026,"SFCODE",r.SFCODE);
    R(0x25F80028,"CHCTLA",r.CHCTLA); R(0x25F8002A,"CHCTLB",r.CHCTLB);
    R(0x25F8002C,"BMPNA", r.BMPNA);  R(0x25F8002E,"BMPNB", r.BMPNB);
    R(0x25F80030,"PNCN0", r.PNCN0);  R(0x25F80032,"PNCN1", r.PNCN1);
    R(0x25F80034,"PNCN2", r.PNCN2);  R(0x25F80036,"PNCN3", r.PNCN3);
    R(0x25F80038,"PNCR",  r.PNCR);   R(0x25F8003A,"PLSZ",  r.PLSZ);
    R(0x25F8003C,"MPOFN", r.MPOFN);  R(0x25F8003E,"MPOFR", r.MPOFR);
    R(0x25F80040,"MPABN0",r.MPABN0); R(0x25F80042,"MPCDN0",r.MPCDN0);
    R(0x25F80044,"MPABN1",r.MPABN1); R(0x25F80046,"MPCDN1",r.MPCDN1);
    R(0x25F80048,"MPABN2",r.MPABN2); R(0x25F8004A,"MPCDN2",r.MPCDN2);
    R(0x25F8004C,"MPABN3",r.MPABN3); R(0x25F8004E,"MPCDN3",r.MPCDN3);
    R(0x25F80050,"MPABRA",r.MPABRA); R(0x25F80052,"MPCDRA",r.MPCDRA);
    R(0x25F80054,"MPEFRA",r.MPEFRA); R(0x25F80056,"MPGHRA",r.MPGHRA);
    R(0x25F80058,"MPIJRA",r.MPIJRA); R(0x25F8005A,"MPKLRA",r.MPKLRA);
    R(0x25F8005C,"MPMNRA",r.MPMNRA); R(0x25F8005E,"MPOPRA",r.MPOPRA);
    R(0x25F80060,"MPABRB",r.MPABRB); R(0x25F80062,"MPCDRB",r.MPCDRB);
    R(0x25F80064,"MPEFRB",r.MPEFRB); R(0x25F80066,"MPGHRB",r.MPGHRB);
    R(0x25F80068,"MPIJRB",r.MPIJRB); R(0x25F8006A,"MPKLRB",r.MPKLRB);
    R(0x25F8006C,"MPMNRB",r.MPMNRB); R(0x25F8006E,"MPOPRB",r.MPOPRB);
    R(0x25F80070,"SCXIN0",r.SCXIN0); R(0x25F80072,"SCXDN0",r.SCXDN0);
    R(0x25F80074,"SCYIN0",r.SCYIN0); R(0x25F80076,"SCYDN0",r.SCYDN0);
    R(0x25F80078,"ZMXN0I",r.ZMXN0.part.I); R(0x25F8007A,"ZMXN0D",r.ZMXN0.part.D);
    R(0x25F8007C,"ZMYN0I",r.ZMYN0.part.I); R(0x25F8007E,"ZMYN0D",r.ZMYN0.part.D);
    R(0x25F80080,"SCXIN1",r.SCXIN1); R(0x25F80082,"SCXDN1",r.SCXDN1);
    R(0x25F80084,"SCYIN1",r.SCYIN1); R(0x25F80086,"SCYDN1",r.SCYDN1);
    R(0x25F80088,"ZMXN1I",r.ZMXN1.part.I); R(0x25F8008A,"ZMXN1D",r.ZMXN1.part.D);
    R(0x25F8008C,"ZMYN1I",r.ZMYN1.part.I); R(0x25F8008E,"ZMYN1D",r.ZMYN1.part.D);
    R(0x25F80090,"SCXN2", r.SCXN2);  R(0x25F80092,"SCYN2", r.SCYN2);
    R(0x25F80094,"SCXN3", r.SCXN3);  R(0x25F80096,"SCYN3", r.SCYN3);
    R(0x25F80098,"ZMCTL", r.ZMCTL);  R(0x25F8009A,"SCRCTL",r.SCRCTL);
    R(0x25F8009C,"VCSTAU",r.VCSTA.part.U); R(0x25F8009E,"VCSTL", r.VCSTA.part.L);
    R(0x25F800A0,"LSTA0U",r.LSTA0.part.U); R(0x25F800A2,"LSTA0L",r.LSTA0.part.L);
    R(0x25F800A4,"LSTA1U",r.LSTA1.part.U); R(0x25F800A6,"LSTA1L",r.LSTA1.part.L);
    R(0x25F800A8,"LCTAU", r.LCTA.part.U);  R(0x25F800AA,"LCTAL", r.LCTA.part.L);
    R(0x25F800AC,"BKTAU", r.BKTAU);  R(0x25F800AE,"BKTAL", r.BKTAL);
    R(0x25F800B0,"RPMD",  r.RPMD);   R(0x25F800B2,"RPRCTL",r.RPRCTL);
    R(0x25F800B4,"KTCTL", r.KTCTL);  R(0x25F800B6,"KTAOF", r.KTAOF);
    R(0x25F800B8,"OVPNRA",r.OVPNRA); R(0x25F800BA,"OVPNRB",r.OVPNRB);
    R(0x25F800BC,"RPTAU", r.RPTA.part.U);  R(0x25F800BE,"RPTAL", r.RPTA.part.L);
    R(0x25F800C0,"WPSX0", r.WPSX0);  R(0x25F800C2,"WPSY0", r.WPSY0);
    R(0x25F800C4,"WPEX0", r.WPEX0);  R(0x25F800C6,"WPEY0", r.WPEY0);
    R(0x25F800C8,"WPSX1", r.WPSX1);  R(0x25F800CA,"WPSY1", r.WPSY1);
    R(0x25F800CC,"WPEX1", r.WPEX1);  R(0x25F800CE,"WPEY1", r.WPEY1);
    R(0x25F800D0,"WCTLA", r.WCTLA);  R(0x25F800D2,"WCTLB", r.WCTLB);
    R(0x25F800D4,"WCTLC", r.WCTLC);  R(0x25F800D6,"WCTLD", r.WCTLD);
    R(0x25F800D8,"LWTA0U",r.LWTA0.part.U); R(0x25F800DA,"LWTA0L",r.LWTA0.part.L);
    R(0x25F800DC,"LWTA1U",r.LWTA1.part.U); R(0x25F800DE,"LWTA1L",r.LWTA1.part.L);
    R(0x25F800E0,"SPCTL", r.SPCTL);  R(0x25F800E2,"SDCTL", r.SDCTL);
    R(0x25F800E4,"CRAOFA",r.CRAOFA); R(0x25F800E6,"CRAOFB",r.CRAOFB);
    R(0x25F800E8,"LNCLEN",r.LNCLEN); R(0x25F800EA,"SFPRMD",r.SFPRMD);
    R(0x25F800EC,"CCCTL", r.CCCTL);  R(0x25F800EE,"SFCCMD",r.SFCCMD);
    R(0x25F800F0,"PRISA", r.PRISA);  R(0x25F800F2,"PRISB", r.PRISB);
    R(0x25F800F4,"PRISC", r.PRISC);  R(0x25F800F6,"PRISD", r.PRISD);
    R(0x25F800F8,"PRINA", r.PRINA);  R(0x25F800FA,"PRINB", r.PRINB);
    R(0x25F800FC,"PRIR",  r.PRIR);
    R(0x25F80100,"CCRSA", r.CCRSA);  R(0x25F80102,"CCRSB", r.CCRSB);
    R(0x25F80104,"CCRSC", r.CCRSC);  R(0x25F80106,"CCRSD", r.CCRSD);
    R(0x25F80108,"CCRNA", r.CCRNA);  R(0x25F8010A,"CCRNB", r.CCRNB);
    R(0x25F8010C,"CCRR",  r.CCRR);   R(0x25F8010E,"CCRLB", r.CCRLB);
    R(0x25F80110,"CLOFEN",r.CLOFEN); R(0x25F80112,"CLOFSL",r.CLOFSL);
    R(0x25F80114,"COAR",  r.COAR);   R(0x25F80116,"COAG",  r.COAG);
    R(0x25F80118,"COAB",  r.COAB);   R(0x25F8011A,"COBR",  r.COBR);
    R(0x25F8011C,"COBG",  r.COBG);   R(0x25F8011E,"COBB",  r.COBB);
#undef R
    pteRawRegs->setPlainText(QString::fromStdString(raw.str()));

    /* ---- DECODED pane ---- */
    auto s9=[](u16 v)->int{int x=v&0x1FF;if(x&0x100)x-=0x200;return x;};
    std::ostringstream d;

    // TVMD
    {static const char*hr[]={"320","352","640","704"};static const char*vr[]={"224","240","256","480/448"};static const char*lm[]={"Non-interlace","(rsvd)","Single-density interlace","Double-density interlace"};
    d<<"=== TVMD=0x"<<HEX4(r.TVMD)<<" ===\n";
    d<<"  DISP="<<((r.TVMD>>15)&1)<<(((r.TVMD>>15)&1)?"  ON":"  OFF")<<"\n";
    d<<"  BSMC="<<((r.TVMD>>8)&1)<<"  LSMD="<<lm[(r.TVMD>>6)&3]<<"\n";
    d<<"  VRES="<<vr[(r.TVMD>>4)&3]<<"  HRES="<<hr[r.TVMD&3]<<"\n";
    d<<"  Signal: "<<(Vdp2Regs->TVSTAT&1?"PAL":"NTSC")<<"\n";}

    // EXTEN/TVSTAT/counters
    d<<"\n=== EXTEN=0x"<<HEX4(r.EXTEN)<<"  TVSTAT=0x"<<HEX4(r.TVSTAT)<<" ===\n";
    d<<"  EXLTEN="<<(r.EXTEN&1)<<"  EXSYEN="<<((r.EXTEN>>1)&1)<<"\n";
    d<<"  EXLTFG="<<((r.TVSTAT>>9)&1)<<"  EXSYFG="<<((r.TVSTAT>>8)&1)<<"\n";
    d<<"  ODD="<<((r.TVSTAT>>1)&1)<<"  PAL="<<(r.TVSTAT&1)<<"\n";
    d<<"  HBLANK="<<((r.TVSTAT>>2)&1)<<"  VBLANK="<<((r.TVSTAT>>3)&1)<<"\n";
    d<<"  H="<<DEC(r.HCNT)<<"  V="<<DEC(r.VCNT)<<"\n";

    // VRSIZE/RAMCTL
    d<<"\n=== VRAM/RAMCTL ===\n";
    {static const char*crm[]={"1024x16bit(m0)","2048x16bit(m1)","1024x32bit(m2)","(rsvd)"};
    d<<"  VRSIZE=0x"<<HEX4(r.VRSIZE)<<((r.VRSIZE>>15)&1?"  (8Mbit)":"  (4Mbit)")<<"\n";
    d<<"  CRMD="<<crm[(r.RAMCTL>>12)&3]<<"\n";
    d<<"  VRBMD="<<((r.RAMCTL>>8)&1)<<"  VRAMD="<<((r.RAMCTL>>4)&1)<<"\n";}

    // Cycle patterns
    d<<"\n=== VRAM Cycle Patterns ===\n";
    auto cyc=[&](const char*n,u16 L,u16 U){
        d<<"  "<<n<<": T0="<<decodeVramTiming((L>>12)&0xF)<<" T1="<<decodeVramTiming((L>>8)&0xF)
          <<" T2="<<decodeVramTiming((L>>4)&0xF)<<" T3="<<decodeVramTiming(L&0xF)
          <<"  T4="<<decodeVramTiming((U>>12)&0xF)<<" T5="<<decodeVramTiming((U>>8)&0xF)
          <<" T6="<<decodeVramTiming((U>>4)&0xF)<<" T7="<<decodeVramTiming(U&0xF)<<"\n";};
    cyc("VRAM-A0",r.CYCA0L,r.CYCA0U); cyc("VRAM-A1",r.CYCA1L,r.CYCA1U);
    cyc("VRAM-B0",r.CYCB0L,r.CYCB0U); cyc("VRAM-B1",r.CYCB1L,r.CYCB1U);

    // BGON
    d<<"\n=== BGON=0x"<<HEX4(r.BGON)<<" (Display Enable) ===\n";
    {static const char*bn[]={"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1"};
    for(int i=0;i<6;i++) d<<"  "<<bn[i]<<": "<<((r.BGON>>i)&1?"ON ":"OFF")<<"  transparent="<<((r.BGON>>(8+i))&1)<<"\n";}

    // MZCTL
    d<<"\n=== MZCTL=0x"<<HEX4(r.MZCTL)<<" (Mosaic) ===\n";
    {static const char*mp[]={"NBG0","NBG1","NBG2","NBG3","RBG0"};
    d<<"  H="<<DEC(((r.MZCTL>>8)&0xF)+1)<<"  V="<<DEC(((r.MZCTL>>12)&0xF)+1)<<"\n";
    for(int i=0;i<5;i++) d<<"  "<<mp[i]<<"="<<((r.MZCTL>>i)&1?"ON":"off")<<"\n";}

    // SFSEL/SFCODE
    d<<"\n=== Special Function: SFSEL=0x"<<HEX4(r.SFSEL)<<"  SFCODE=0x"<<HEX4(r.SFCODE)<<" ===\n";

    // CHCTLA/B
    d<<"\n=== Character Control ===\n";
    {static const char*cc5[]={"16(4bpp)","16(4bpp)","256(8bpp)","2048(11bpp)","32768(15bpp)","16M(24bpp)","?","?"};
    static const char*cc2[]={"16(4bpp)","256(8bpp)","?","?"};
    static const char*bsz[]={"512x256","512x512","1024x256","1024x512"};
    d<<"  CHCTLA=0x"<<HEX4(r.CHCTLA)<<"\n";
    int n0bm=(r.CHCTLA>>1)&1,n0sz=r.CHCTLA&1,n0cc=(r.CHCTLA>>4)&7,n0bw=(r.CHCTLA>>2)&3;
    d<<"    NBG0: bm="<<n0bm<<"  char="<<(n0sz?"16x16":"8x8")<<"  col="<<cc5[n0cc];
    if(n0bm)d<<"  bmpSz="<<bsz[n0bw];d<<"\n";
    int n1bm=(r.CHCTLA>>9)&1,n1sz=(r.CHCTLA>>8)&1,n1cc=(r.CHCTLA>>12)&3,n1bw=(r.CHCTLA>>10)&3;
    d<<"    NBG1: bm="<<n1bm<<"  char="<<(n1sz?"16x16":"8x8")<<"  col="<<cc2[n1cc];
    if(n1bm)d<<"  bmpSz="<<bsz[n1bw];d<<"\n";
    d<<"  CHCTLB=0x"<<HEX4(r.CHCTLB)<<"\n";
    d<<"    NBG2: char="<<(((r.CHCTLB>>2)&1)?"16x16":"8x8")<<"  col="<<cc2[(r.CHCTLB>>1)&1]<<"\n";
    d<<"    NBG3: char="<<(((r.CHCTLB>>10)&1)?"16x16":"8x8")<<"  col="<<cc2[(r.CHCTLB>>9)&1]<<"\n";
    int r0bm=(r.CHCTLB>>9)&1,r0sz=(r.CHCTLB>>8)&1,r0cc=(r.CHCTLB>>12)&7,r0bw=(r.CHCTLB>>10)&3;
    d<<"    RBG0: bm="<<r0bm<<"  char="<<(r0sz?"16x16":"8x8")<<"  col="<<cc5[r0cc&7];
    if(r0bm)d<<"  bmpSz="<<bsz[r0bw];d<<"\n";}

    // BMPNA/B
    d<<"\n=== Bitmap Palette Numbers ===\n";
    d<<"  BMPNA=0x"<<HEX4(r.BMPNA)<<"  N0-pal="<<DEC(r.BMPNA&7)<<"  N1-pal="<<DEC((r.BMPNA>>8)&7)<<"\n";
    d<<"  BMPNB=0x"<<HEX4(r.BMPNB)<<"  RBG0-pal="<<DEC(r.BMPNB&7)<<"\n";

    // PNC
    d<<"\n=== Pattern Name Control ===\n";
    auto pnc=[&](const char*n,u16 v){
        d<<"  "<<n<<"=0x"<<HEX4(v)<<"  "<<((v&0x8000)?"1word":"2word");
        if(v&0x8000)d<<"  CNs="<<((v>>14)&1)<<"  SPb="<<((v>>9)&1)<<"  SCCb="<<((v>>8)&1)<<"  palS="<<((v>>5)&7)<<"  colS="<<(v&0x1F);
        d<<"\n";};
    pnc("PNCN0",r.PNCN0);pnc("PNCN1",r.PNCN1);pnc("PNCN2",r.PNCN2);pnc("PNCN3",r.PNCN3);pnc("PNCR ",r.PNCR);

    // PLSZ
    d<<"\n=== PLSZ=0x"<<HEX4(r.PLSZ)<<" (Plane Size) ===\n";
    {static const char*ps[]={"1Hx1V","2Hx1V","(rsvd)","2Hx2V"};
    d<<"  N0="<<ps[r.PLSZ&3]<<"  N1="<<ps[(r.PLSZ>>2)&3]<<"  N2="<<ps[(r.PLSZ>>4)&3]
      <<"  N3="<<ps[(r.PLSZ>>6)&3]<<"  R0A="<<ps[(r.PLSZ>>8)&3]<<"  R0B="<<ps[(r.PLSZ>>12)&3]<<"\n";}

    // Map offsets
    d<<"\n=== Map Offsets ===\n";
    d<<"  MPOFN=0x"<<HEX4(r.MPOFN)<<"  N0="<<DEC(r.MPOFN&7)<<"  N1="<<DEC((r.MPOFN>>4)&7)<<"  N2="<<DEC((r.MPOFN>>8)&7)<<"  N3="<<DEC((r.MPOFN>>12)&7)<<"\n";
    d<<"  MPOFR=0x"<<HEX4(r.MPOFR)<<"  R0A="<<DEC(r.MPOFR&7)<<"  R0B="<<DEC((r.MPOFR>>4)&7)<<"\n";

    // NBG map planes
    d<<"\n=== NBG Map Planes (A-D) ===\n";
    auto mp=[&](const char*n,u16 ab,u16 cd){d<<"  "<<n<<": A=0x"<<HEX4(ab&0xFF)<<" B=0x"<<HEX4(ab>>8)<<" C=0x"<<HEX4(cd&0xFF)<<" D=0x"<<HEX4(cd>>8)<<"\n";};
    mp("NBG0",r.MPABN0,r.MPCDN0);mp("NBG1",r.MPABN1,r.MPCDN1);mp("NBG2",r.MPABN2,r.MPCDN2);mp("NBG3",r.MPABN3,r.MPCDN3);

    // RBG maps
    d<<"\n=== RBG0 Param A planes (A-P) ===\n"<<std::hex<<std::uppercase;
    d<<"  AB=0x"<<HEX4(r.MPABRA)<<" CD=0x"<<HEX4(r.MPCDRA)<<" EF=0x"<<HEX4(r.MPEFRA)<<" GH=0x"<<HEX4(r.MPGHRA)<<"\n";
    d<<"  IJ=0x"<<HEX4(r.MPIJRA)<<" KL=0x"<<HEX4(r.MPKLRA)<<" MN=0x"<<HEX4(r.MPMNRA)<<" OP=0x"<<HEX4(r.MPOPRA)<<"\n";
    d<<"\n=== RBG0 Param B planes (A-P) ===\n";
    d<<"  AB=0x"<<HEX4(r.MPABRB)<<" CD=0x"<<HEX4(r.MPCDRB)<<" EF=0x"<<HEX4(r.MPEFRB)<<" GH=0x"<<HEX4(r.MPGHRB)<<"\n";
    d<<"  IJ=0x"<<HEX4(r.MPIJRB)<<" KL=0x"<<HEX4(r.MPKLRB)<<" MN=0x"<<HEX4(r.MPMNRB)<<" OP=0x"<<HEX4(r.MPOPRB)<<"\n";
    d<<std::dec;

    // Scroll
    d<<"\n=== Scroll Positions ===\n";
    d<<"  NBG0: X="<<DEC((s16)r.SCXIN0)<<"."<<DEC((r.SCXDN0>>8)&0xFF)<<"  Y="<<DEC((s16)r.SCYIN0)<<"."<<DEC((r.SCYDN0>>8)&0xFF)<<"\n";
    d<<"  NBG1: X="<<DEC((s16)r.SCXIN1)<<"."<<DEC((r.SCXDN1>>8)&0xFF)<<"  Y="<<DEC((s16)r.SCYIN1)<<"."<<DEC((r.SCYDN1>>8)&0xFF)<<"\n";
    d<<"  NBG2: X="<<DEC((s16)r.SCXN2)<<"  Y="<<DEC((s16)r.SCYN2)<<"\n";
    d<<"  NBG3: X="<<DEC((s16)r.SCXN3)<<"  Y="<<DEC((s16)r.SCYN3)<<"\n";

    // Zoom
    d<<"\n=== ZMCTL=0x"<<HEX4(r.ZMCTL)<<" (Zoom) ===\n";
    {static const char*zm[]={"None","H(1/2)","H+V(1/4)","H+V(1/4)"};
    d<<"  NBG0="<<zm[r.ZMCTL&3]<<"  NBG1="<<zm[(r.ZMCTL>>8)&3]<<"\n";
    d<<"  ZMX0="<<DEC(r.ZMXN0.part.I)<<"."<<DEC(r.ZMXN0.part.D)<<"  ZMY0="<<DEC(r.ZMYN0.part.I)<<"."<<DEC(r.ZMYN0.part.D)<<"\n";
    d<<"  ZMX1="<<DEC(r.ZMXN1.part.I)<<"."<<DEC(r.ZMXN1.part.D)<<"  ZMY1="<<DEC(r.ZMYN1.part.I)<<"."<<DEC(r.ZMYN1.part.D)<<"\n";}

    // SCRCTL
    d<<"\n=== SCRCTL=0x"<<HEX4(r.SCRCTL)<<" (Scroll Control) ===\n";
    {auto sb=[&](const char*n,int b,u32 la,u32 vc){
        d<<"  "<<n<<":";
        if((b>>0)&1){d<<" VertCell tbl=0x"<<std::hex<<(0x05E00000|((vc&0x7FFFE)<<1))<<std::dec;}
        if((b>>1)&1) d<<" LineScrollX";
        if((b>>2)&1) d<<" LineScrollY";
        if((b>>3)&1) d<<" LineZoom";
        if(!(b&0xF))  d<<" none";
        if(b&0x6){d<<" lnTbl=0x"<<std::hex<<(0x05E00000|((la&0x7FFFE)<<1))<<std::dec;
            static const char*iv[]={"everyLine","every2","every4","every8"};d<<" "<<iv[(b>>4)&3];}
        d<<"\n";};
    sb("NBG0",r.SCRCTL&0x3F,r.LSTA0.all,r.VCSTA.all);
    sb("NBG1",(r.SCRCTL>>8)&0x3F,r.LSTA1.all,r.VCSTA.all);}

    // Table addresses
    d<<"\n=== Cell/Line/Back Screen Addresses ===\n"<<std::hex<<std::uppercase;
    d<<"  VCSTA=0x"<<HEX8(0x05E00000|((r.VCSTA.all&0x7FFFE)<<1))<<"\n";
    d<<"  LSTA0=0x"<<HEX8(0x05E00000|((r.LSTA0.all&0x7FFFE)<<1))<<"\n";
    d<<"  LSTA1=0x"<<HEX8(0x05E00000|((r.LSTA1.all&0x7FFFE)<<1))<<"\n";
    d<<"  LCTA =0x"<<HEX8(0x05E00000|((r.LCTA.all&0x7FFFE)<<1));
    d<<(r.LCTA.part.U&0x8000?"  (per-line)":"  (single)")<<"\n";
    {u32 bk=0x05E00000|(((u32)(r.BKTAU&0x7)<<16)|r.BKTAL)*2;
    d<<"  BKTA =0x"<<HEX8(bk)<<(r.BKTAU&0x8000?"  (per-line)":"  (single)")<<"\n";}
    d<<std::dec;

    // RBG rotation
    d<<"\n=== RBG Rotation ===\n";
    {static const char*rpm[]={"Param A only","Param B only","Switch by window","Switch by sprite MSB"};
    d<<"  RPMD=0x"<<HEX4(r.RPMD)<<"  "<<rpm[r.RPMD&3]<<"\n";
    d<<"  RPRCTL=0x"<<HEX4(r.RPRCTL)<<"\n";
    auto rpr=[&](const char*n,int sh){int b=(r.RPRCTL>>sh)&0xF;d<<"    "<<n<<": ";
        if(b&1)d<<"Xst ";if(b&2)d<<"Yst ";if(b&4)d<<"KAst ";if(b&8)d<<"KAstInc ";if(!b)d<<"none";d<<"\n";};
    rpr("ParamA",0);rpr("ParamB",8);
    d<<"  KTCTL=0x"<<HEX4(r.KTCTL)<<"\n";
    auto kt=[&](const char*n,int sh){
        static const char*km[]={"16b","16b+addr","32b","32b+addr","16b/line","16b+addr/line","??","??"};
        d<<"    "<<n<<": en="<<((r.KTCTL>>sh)&1)<<"  mode="<<DEC((r.KTCTL>>(sh+2))&7)<<"("<<km[(r.KTCTL>>(sh+2))&7]<<")\n";};
    kt("ParamA",0);kt("ParamB",8);
    d<<"  KTAOF=0x"<<HEX4(r.KTAOF)<<"  A-off="<<DEC(r.KTAOF&7)<<"  B-off="<<DEC((r.KTAOF>>8)&7)<<"\n";
    d<<"  OVPNRA=0x"<<HEX4(r.OVPNRA)<<"  OVPNRB=0x"<<HEX4(r.OVPNRB)<<"\n";
    u32 rp=0x05E00000|((((u32)r.RPTA.part.U<<16)|r.RPTA.part.L)&0x7FFFE)*2;
    d<<"  RPTA=0x"<<std::hex<<std::uppercase<<HEX8(rp)<<std::dec<<"\n";}

    // Windows
    d<<"\n=== Windows ===\n";
    {auto wc=[&](const char*n,u16 wctl){
        d<<"  "<<n<<" WCTL=0x"<<HEX4(wctl)<<": ";
        if(!(wctl&0xAA)){d<<"none\n";return;}
        if(wctl&0x02)d<<"W0("<<((wctl&0x01)?"out":"in")<<") ";
        if(wctl&0x08)d<<"W1("<<((wctl&0x04)?"out":"in")<<") ";
        if(wctl&0x20)d<<"SPW("<<((wctl&0x10)?"out":"in")<<") ";
        if(wctl&0x80)d<<"AND ";d<<"\n";};
    d<<"  W0: ("<<DEC((s16)r.WPSX0)<<","<<DEC((s16)r.WPSY0)<<")->("<<DEC((s16)r.WPEX0)<<","<<DEC((s16)r.WPEY0)<<")";
    if(r.LWTA0.all&0x80000000)d<<"  [LINE 0x"<<std::hex<<std::uppercase<<(0x05E00000UL|((r.LWTA0.all&0x7FFFEUL)<<1))<<std::dec<<"]\n"; else d<<"\n";
    d<<"  W1: ("<<DEC((s16)r.WPSX1)<<","<<DEC((s16)r.WPSY1)<<")->("<<DEC((s16)r.WPEX1)<<","<<DEC((s16)r.WPEY1)<<")";
    if(r.LWTA1.all&0x80000000)d<<"  [LINE 0x"<<std::hex<<std::uppercase<<(0x05E00000UL|((r.LWTA1.all&0x7FFFEUL)<<1))<<std::dec<<"]\n"; else d<<"\n";
    wc("NBG0",r.WCTLA);wc("NBG1",r.WCTLA>>8);wc("NBG2",r.WCTLB);wc("NBG3",r.WCTLB>>8);
    wc("RBG0",r.WCTLC);wc("SPR ",r.WCTLC>>8);wc("RBG1",r.WCTLD);wc("ROT ",r.WCTLD>>8);}

    // SPCTL/SDCTL
    d<<"\n=== SPCTL=0x"<<HEX4(r.SPCTL)<<"  SDCTL=0x"<<HEX4(r.SDCTL)<<" ===\n";
    {static const char*spc[]={"16(4bpp)","256(8bpp)","32768(15bpp)","16M(24bpp)","16bank","256bank","32768bank","16Mbank"};
    static const char*spcond[]={"Prio<=CCnum","Prio==CCnum","Prio>=CCnum","MSB colour"};
    d<<"  type="<<DEC(r.SPCTL&0xF)<<"  col="<<spc[(r.SPCTL>>4)&7]<<"  SPWINEN="<<((r.SPCTL>>11)&1)<<"\n";
    d<<"  CCcond="<<spcond[(r.SPCTL>>12)&3]<<"  CCnum="<<DEC((r.SPCTL>>8)&7)<<"\n";
    d<<"  RGB+pal="<<((r.SPCTL>>5)&1)<<"  MSBshadow="<<(r.SDCTL&1)<<"  TrShadow="<<((r.SDCTL>>8)&1)<<"\n";}

    // CRAOFA/B
    d<<"\n=== Color RAM Offset ===\n"<<std::hex;
    d<<"  CRAOFA=0x"<<HEX4(r.CRAOFA)<<"  N0=0x"<<((r.CRAOFA&7)<<8)<<"  N1=0x"<<(((r.CRAOFA>>4)&7)<<8)<<"  N2=0x"<<(((r.CRAOFA>>8)&7)<<8)<<"  N3=0x"<<(((r.CRAOFA>>12)&7)<<8)<<"\n";
    d<<"  CRAOFB=0x"<<HEX4(r.CRAOFB)<<"  RBG0=0x"<<((r.CRAOFB&7)<<8)<<"  SPR=0x"<<(((r.CRAOFB>>4)&7)<<8)<<"\n"<<std::dec;

    // LNCLEN
    d<<"\n=== LNCLEN=0x"<<HEX4(r.LNCLEN)<<" (Line Color Insertion) ===\n";
    {static const char*lp[]={"NBG0","NBG1","NBG2","NBG3","RBG0","SPR"};
    for(int i=0;i<6;i++)d<<"  "<<lp[i]<<"="<<((r.LNCLEN>>i)&1?"ON":"off")<<"\n";}

    // SFPRMD
    d<<"\n=== SFPRMD=0x"<<HEX4(r.SFPRMD)<<" (Special Priority) ===\n";
    {static const char*sp[]={"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1"};
    static const char*sm[]={"none","per-tile","per-pixel","(undoc)"};
    for(int i=0;i<6;i++)d<<"  "<<sp[i]<<"="<<sm[(r.SFPRMD>>(i*2))&3]<<"\n";}

    // CCCTL/SFCCMD
    d<<"\n=== CCCTL=0x"<<HEX4(r.CCCTL)<<"  SFCCMD=0x"<<HEX4(r.SFCCMD)<<" ===\n";
    {d<<"  BOKEN="<<((r.CCCTL>>9)&1)<<"  EXCCEN="<<((r.CCCTL>>8)&1)<<"  BKCCEN="<<((r.CCCTL>>5)&1)<<"\n";
    static const char*cp[]={"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1","SPR","BACK"};
    d<<"  CC enabled: ";for(int i=0;i<8;i++)if((r.CCCTL>>i)&1)d<<cp[i]<<" ";d<<"\n";
    static const char*sl[]={"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1"};
    for(int i=0;i<6;i++)d<<"  "<<sl[i]<<" SCC="<<DEC((r.SFCCMD>>(i*2))&3)<<"\n";}

    // Priority
    d<<"\n=== Priority Numbers ===\n";
    d<<"  N0="<<DEC(r.PRINA&7)<<" N1="<<DEC((r.PRINA>>8)&7)<<" N2="<<DEC(r.PRINB&7)<<" N3="<<DEC((r.PRINB>>8)&7)<<"\n";
    d<<"  R0="<<DEC(r.PRISC&7)<<" R1="<<DEC((r.PRISC>>8)&7)<<"\n";
    {u8*sp=(u8*)&r.PRISA;d<<"  Sprite:";
    for(int i=0;i<8;i++){
#ifdef WORDS_BIGENDIAN
        d<<" SP"<<i<<"="<<DEC(sp[i^1]&7);
#else
        d<<" SP"<<i<<"="<<DEC(sp[i]&7);
#endif
    }d<<"\n";}

    // CC ratios
    d<<"\n=== Color Calculation Ratios ===\n";
    d<<"  N0="<<DEC(r.CCRNA&0x1F)<<" N1="<<DEC((r.CCRNA>>8)&0x1F)<<" N2="<<DEC(r.CCRNB&0x1F)<<" N3="<<DEC((r.CCRNB>>8)&0x1F)<<"\n";
    d<<"  R0="<<DEC(r.CCRSC&0x1F)<<" R1="<<DEC((r.CCRSC>>8)&0x1F)<<"\n";
    {u8*sc=(u8*)&r.CCRSA;d<<"  Sprite:";
    for(int i=0;i<8;i++){
#ifdef WORDS_BIGENDIAN
        d<<" SP"<<i<<"="<<DEC(sc[i^1]&0x1F);
#else
        d<<" SP"<<i<<"="<<DEC(sc[i]&0x1F);
#endif
    }d<<"\n";}
    d<<"  Line="<<DEC(r.CCRR&0x1F)<<"  Back="<<DEC((r.CCRLB>>8)&0x1F)<<"\n";

    // Color offset
    d<<"\n=== Color Offset ===\n";
    d<<"  CLOFEN=0x"<<HEX4(r.CLOFEN)<<"  CLOFSL=0x"<<HEX4(r.CLOFSL)<<"\n";
    d<<"  BankA: R="<<DEC(s9(r.COAR))<<" G="<<DEC(s9(r.COAG))<<" B="<<DEC(s9(r.COAB))<<"\n";
    d<<"  BankB: R="<<DEC(s9(r.COBR))<<" G="<<DEC(s9(r.COBG))<<" B="<<DEC(s9(r.COBB))<<"\n";
    {static const char*op[]={"NBG0","NBG1","NBG2","NBG3","RBG0","BACK","SPR","RBG1"};
    for(int i=0;i<8;i++)if((r.CLOFEN>>i)&1){
        bool useB=(r.CLOFSL>>i)&1;
        d<<"  "<<op[i]<<": bank"<<(useB?"B":"A")<<" R="<<DEC(s9(useB?r.COBR:r.COAR))<<" G="<<DEC(s9(useB?r.COBG:r.COAG))<<" B="<<DEC(s9(useB?r.COBB:r.COAB))<<"\n";}}

    pteDecodedRegs->setPlainText(QString::fromStdString(d.str()));
}

// ============================================================
//  updateStats  — aggregate the 8 VDP2 debug text functions
// ============================================================
void UIDebugVDP2Viewer::updateStats()
{
    if (!Vdp2Regs) { pteStats->setPlainText("VDP2 not initialised"); return; }
    std::ostringstream out;
    struct Layer { const char *name; void (*fn)(char*,int*); };
    static Layer layers[] = {
        {"NBG0",    Vdp2DebugStatsNBG0},
        {"NBG1",    Vdp2DebugStatsNBG1},
        {"NBG2",    Vdp2DebugStatsNBG2},
        {"NBG3",    Vdp2DebugStatsNBG3},
        {"RBG0",    Vdp2DebugStatsRBG0},
        {"RBG1",    Vdp2DebugStatsRBG1},
        {"SPRITE",  Vdp2DebugStatsSprite},
        {"GENERAL", Vdp2DebugStatsGeneral},
    };
    for (auto &l : layers) {
        char buf[4096] = {};
        int en = 0;
        l.fn(buf, &en);
        if (en || l.fn == Vdp2DebugStatsSprite || l.fn == Vdp2DebugStatsGeneral) {
            out << "========== " << l.name << " ==========\n" << buf << "\n";
        }
    }
    pteStats->setPlainText(QString::fromStdString(out.str()));
}

// ============================================================
//  updateColorRam  — draw all Color RAM palette swatches
// ============================================================
void UIDebugVDP2Viewer::updateColorRam()
{
    QGraphicsScene *scene = gvColorRam->scene();
    if (!scene) { scene = new QGraphicsScene(this); gvColorRam->setScene(scene); }
    scene->clear();
    pteColorRamHex->clear();

    if (!Vdp2ColorRam || !Vdp2Regs) {
        lCramInfo->setText("Color RAM not available");
        return;
    }

    int mode = (Vdp2Regs->RAMCTL >> 12) & 3;
    int total = (mode <= 1) ? 2048 : 1024;
    lCramInfo->setText(QString("Color RAM mode %1  —  %2 entries  (%3 bytes each)")
        .arg(mode).arg(total).arg(mode<=1?2:4));

    const int SW=10, SH=10, COLS=64;
    bool showHex = cbCramHex->isChecked();
    std::ostringstream hex;

    for (int i = 0; i < total; i++) {
        int x = (i % COLS)*(SW+1), y = (i / COLS)*(SH+1);
        scene->addRect(x,y,SW,SH, QPen(Qt::NoPen), QBrush(cramColor(i)));
        if (showHex) {
            if (i%16==0) hex<<std::hex<<std::uppercase<<std::setw(4)<<std::setfill('0')<<i<<": ";
            u16 w = (mode<=1) ? T2ReadWord(Vdp2ColorRam,(i*2)&0xFFF)
                              : T2ReadWord(Vdp2ColorRam,(i*4)&0xFFF);
            hex<<std::hex<<std::uppercase<<std::setw(4)<<std::setfill('0')<<(unsigned)w<<" ";
            if (i%16==15) hex<<"\n";
        }
    }
    scene->setSceneRect(scene->itemsBoundingRect());
    gvColorRam->fitInView(scene->sceneRect(), Qt::KeepAspectRatio);
    if (showHex) pteColorRamHex->setPlainText(QString::fromStdString(hex.str()));
}

// ============================================================
//  updateVramHex  — hex dump of a VRAM bank region
// ============================================================
void UIDebugVDP2Viewer::updateVramHex()
{
    pteVramHex->clear();
    if (!Vdp2Ram) { pteVramHex->setPlainText("VDP2 RAM not available"); return; }

    int bank = cbVramBank->currentIndex();
    u32 base=0, bSize=0;
    switch(bank){
        case 0: base=0x00000; bSize=0x40000; break;
        case 1: base=0x40000; bSize=0x40000; break;
        case 2: base=0x00000; bSize=0x20000; break;
        case 3: base=0x20000; bSize=0x20000; break;
        case 4: base=0x40000; bSize=0x20000; break;
        case 5: base=0x60000; bSize=0x20000; break;
    }

    bool ok=false;
    u32 offset = leVramOffset->text().toUInt(&ok,16);
    if (!ok) offset=0;
    offset &= ~0xFU;
    if (offset >= bSize) offset=0;

    const int ROWS=32;  // 512 bytes
    std::ostringstream s;
    s<<"VRAM @ 0x"<<std::hex<<std::uppercase<<std::setw(6)<<std::setfill('0')<<(base+offset)
     <<"  ("<<std::dec<<ROWS*16<<" bytes)\n\n"
     <<"Offset   00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  ASCII\n"
     <<"-------- ------------------------------------------------  ----------------\n";

    for (int row=0;row<ROWS;row++){
        u32 off=offset+row*16;
        if (off+16>bSize) break;
        u32 addr=base+off;
        s<<"0x"<<std::hex<<std::uppercase<<std::setw(6)<<std::setfill('0')<<addr<<"  ";
        char asc[17]={};
        for(int b=0;b<16;b++){
            u8 byte=T2ReadByte(Vdp2Ram,addr+b);
            s<<std::hex<<std::uppercase<<std::setw(2)<<std::setfill('0')<<(unsigned)byte<<" ";
            if(b==7)s<<" ";
            asc[b]=(byte>=0x20&&byte<0x7F)?(char)byte:'.';
        }
        s<<" "<<asc<<"\n";
    }
    pteVramHex->setPlainText(QString::fromStdString(s.str()));
}

// ============================================================
//  Viewer methods
// ============================================================
void UIDebugVDP2Viewer::clearItems() { while(cbScreen->count()) cbScreen->removeItem(0); }
void UIDebugVDP2Viewer::addItem(int id) {
    switch(id){
        case NBG0:   cbScreen->addItem("NBG0",  NBG0);  break;
        case NBG1:   cbScreen->addItem("NBG1",  NBG1);  break;
        case NBG2:   cbScreen->addItem("NBG2",  NBG2);  break;
        case NBG3:   cbScreen->addItem("NBG3",  NBG3);  break;
        case RBG0:   cbScreen->addItem("RBG0",  RBG0);  break;
        case RBG1:   cbScreen->addItem("RBG1",  RBG1);  break;
        case SPRITE: cbScreen->addItem("SPRITE",SPRITE);break;
        default: break;
    }
}
int UIDebugVDP2Viewer::exec() { return QDialog::exec(); }

UIDebugVDP2Viewer::UIDebugVDP2Viewer(QWidget *p) : QDialog(p)
{
    setupUi(this);
    QGraphicsScene *sc = new QGraphicsScene(this); gvScreen->setScene(sc);
    QGraphicsScene *sc2= new QGraphicsScene(this); gvColorRam->setScene(sc2);
    vdp2texture=NULL; width=0; height=0;
    QtYabause::retranslateWidget(this);
}

void UIDebugVDP2Viewer::displayCurrentScreen()
{
    if (!Vdp2Regs) return;
    int idx = cbScreen->itemData(cbScreen->currentIndex()).toInt();
    if (vdp2texture) free(vdp2texture);
    vdp2texture = Vdp2DebugTexture(idx, &width, &height);
    if (vdp2texture) {
        pbSaveAsBitmap->setEnabled(true);
        QGraphicsScene *sc=gvScreen->scene();
        QImage::Format fmt=cbOpaque->isChecked()?QImage::Format_RGB32:QImage::Format_ARGB32;
        QImage img((uchar*)vdp2texture,width,height,fmt);
        QPixmap px=QPixmap::fromImage(img.mirrored(false,idx!=SPRITE).rgbSwapped());
        sc->clear(); sc->setBackgroundBrush(Qt::Dense7Pattern);
        sc->addPixmap(px); sc->setSceneRect(sc->itemsBoundingRect());
    }
}

void UIDebugVDP2Viewer::refresh()
{
    displayCurrentScreen();
    int idx = tabWidget->currentIndex();
    switch(idx){
        case 1: updateVdp2Registers(); break;
        case 2: updateStats();         break;
        case 3: updateColorRam();      break;
        case 4: updateVramHex();       break;
        default: break;
    }
}

void UIDebugVDP2Viewer::showEvent(QShowEvent *)
{
    gvScreen->fitInView(gvScreen->scene()->sceneRect());
    updateVdp2Registers();
}

void UIDebugVDP2Viewer::on_tabWidget_currentChanged(int idx)
{
    switch(idx){
        case 1: updateVdp2Registers(); break;
        case 2: updateStats();         break;
        case 3: updateColorRam();      break;
        case 4: updateVramHex();       break;
        default: break;
    }
}

void UIDebugVDP2Viewer::on_cbScreen_currentIndexChanged(int) { displayCurrentScreen(); }
void UIDebugVDP2Viewer::on_cbOpaque_toggled(bool)             { displayCurrentScreen(); }
void UIDebugVDP2Viewer::on_cbVramBank_currentIndexChanged(int){ updateVramHex(); }
void UIDebugVDP2Viewer::on_pbVramGo_clicked()                 { updateVramHex(); }
void UIDebugVDP2Viewer::on_cbCramHex_toggled(bool)            { updateColorRam(); }

void UIDebugVDP2Viewer::on_pbSaveAsBitmap_clicked()
{
    QStringList filters;
    int idx=cbScreen->itemData(cbScreen->currentIndex()).toInt();
    foreach(QByteArray ba,QImageWriter::supportedImageFormats())
        if(!filters.contains(ba,Qt::CaseInsensitive))filters<<QString(ba).toLower();
    for(int i=0;i<filters.count();i++)
        filters[i]=QtYabause::translate("%1 Images (*.%2)").arg(filters[i].toUpper()).arg(filters[i]);
    if(!vdp2texture)return;
    QImage::Format fmt=cbOpaque->isChecked()?QImage::Format_RGB32:QImage::Format_ARGB32;
    QImage img((uchar*)vdp2texture,width,height,fmt);
    img=img.mirrored(false,idx!=SPRITE).rgbSwapped();
    const QString s=CommonDialogs::getSaveFileName(QString(),QtYabause::translate("Choose a location for your bitmap"),filters.join(";;"));
    if(!s.isEmpty())if(!img.save(s))CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}
