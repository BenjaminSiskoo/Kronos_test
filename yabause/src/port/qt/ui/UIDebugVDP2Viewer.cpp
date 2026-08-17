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
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <QWheelEvent>
#include <QFile>
#include <QTextStream>
#include <QDateTime>

// Miroir du cas spécial "screen == 0xFF" déjà implémenté côté moteur dans
// VIDCSGetVdp2ScreenExtract() (vidcs.c) : lit _Ygl->default_fbo, c'est-à-dire
// l'image finale VDP1+VDP2 déjà composée et mise à l'échelle, telle qu'
// actuellement affichée à l'écran. Volontairement hors de l'enum enBG
// (ygl.h) : cette dernière est utilisée pour dimensionner d'autres tableaux
// via enBGMAX ailleurs dans le renderer, et 0xFF a été choisi côté moteur
// précisément pour ne jamais entrer en collision avec elle.
static const int VDP2_SCREEN_FINAL = 0xFF;

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
// std::right est indispensable : l'alignement est un etat PERSISTANT du
// flux, et la macro R() ci-dessous pose std::left pour aligner la colonne
// du nom sans jamais le retablir. Sans std::right ici, les zeros de
// remplissage partaient a DROITE : BGON=0x0201 s'affichait "2010" et
// TVSTAT=0x0007 s'affichait "7000". Seules les valeurs occupant deja
// toute la largeur (TVMD=0x8120, RAMCTL=0x1327...) echappaient au bug.
#define HEX4(v) std::hex<<std::uppercase<<std::right<<std::setw(4)<<std::setfill('0')<<(unsigned)(v)
#define HEX8(v) std::hex<<std::uppercase<<std::right<<std::setw(8)<<std::setfill('0')<<(unsigned)(v)
#define DEC(v)  std::dec<<(v)

// VRAM access command decode -- ST-058-R2 Table 3.5 p.34.
//
// L'ancienne table etait decalee de deux crans sur tout le haut du nibble.
// Elle attribuait 1000B-1011B a RBG0/RBG1, alors que ces quatre codes sont
// "setting prohibited" : les fonds en rotation n'utilisent PAS les registres
// de cycle pattern (leur banque entiere leur est reservee via RAMCTL, cf.
// §3.3 et Technical Bulletin SOA-6, et vdp2.c:bankOwnedByRotation()). Les
// deux codes reellement presents a cet endroit de la table sont les tables
// de vertical cell scroll NBG0 (1100B) et NBG1 (1101B), qui manquaient
// completement -- d'ou le glissement : CPU-RW (1110B) s'affichait "(rsvd)"
// et no-access (1111B) prenait la place du precedent.
//
// Consequence concrete : sur un jeu utilisant le vertical cell scroll, les
// creneaux VCS etaient rapportes comme CPU-RW / no-access, et le creneau CPU
// reel comme reserve -- exactement le sous-systeme qu'on cherche a debugger.
// Sonic Jam (CYCA1L=0xCD45) se lisait "T0=CPU-RW T1=NoAccess T2=NBG0CP
// T3=NBG1CP" au lieu de "T0=N0VCST T1=N1VCST T2=NBG0CP T3=NBG1CP".
//
// Les libelles font tous 6 caracteres pour que les colonnes T0-T7 restent
// alignees dans l'export texte.
static std::string decodeVramTiming(u8 n) {
    switch(n&0xF){
        case 0x0:return"NBG0PN"; case 0x1:return"NBG1PN";
        case 0x2:return"NBG2PN"; case 0x3:return"NBG3PN";
        case 0x4:return"NBG0CP"; case 0x5:return"NBG1CP";
        case 0x6:return"NBG2CP"; case 0x7:return"NBG3CP";
        // 1000B-1011B : setting prohibited (Table 3.5)
        case 0x8: case 0x9:
        case 0xA: case 0xB:      return"PROHIB";
        case 0xC:return"N0VCST"; case 0xD:return"N1VCST";
        case 0xE:return"CPU-RW"; case 0xF:return"------";
        default: return"??????";
    }
}

// Saturn VDP2 Color RAM formats (ST-058-R2 §4.4) :
//   Mode 0/1 : 1024 or 2048 entries × 16-bit  BGR555  (b14-10=B, b9-5=G, b4-0=R)
//   Mode 2/3 : 1024 entries × 32-bit  XRGB888  (b23-16=R, b15-8=G, b7-0=B)
// La CRAM totale est toujours 4096 octets (0x1000).
static QColor cramColor(int idx) {
    if (!Vdp2ColorRam || !Vdp2Regs) return Qt::black;
    int mode = (Vdp2Regs->RAMCTL>>12)&3;
    if (mode <= 1) {
        // 16-bit BGR555 — adresse = idx*2, max 4095 (0xFFF)
        u16 w = T2ReadWord(Vdp2ColorRam, (idx * 2) & 0xFFF);
        int r = ( w        & 0x1F) << 3;  // bits  4- 0 = Red
        int g = ((w >>  5) & 0x1F) << 3;  // bits  9- 5 = Green
        int b = ((w >> 10) & 0x1F) << 3;  // bits 14-10 = Blue
        return QColor(r, g, b);
    } else {
        // 32-bit XRGB888 — adresse = idx*4, max 4092 (0xFFC pour le mot de base)
        u32 addr = (u32)(idx * 4) & 0xFFC;
        u16 msw = T2ReadWord(Vdp2ColorRam, addr);       // bits 31-16 : X(ignoré), R(23-16)
        u16 lsw = T2ReadWord(Vdp2ColorRam, addr + 2);   // bits 15-0  : G(15-8), B(7-0)
        int r_val = (msw & 0xFF);
        int g_val = (lsw >> 8) & 0xFF;
        int b_val =  lsw       & 0xFF;
        return QColor(r_val, g_val, b_val);
    }
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
#define R(a,n,v) raw<<"0x"<<HEX8(a)<<"  "<<std::left<<std::setw(8)<<std::setfill(' ')<<(n)<<std::right<<" = 0x"<<HEX4(v)<<"\n"
    R(0x25F80000,"TVMD",  r.TVMD);   R(0x25F80002,"EXTEN", r.EXTEN);
    R(0x25F80004,"TVSTAT",r.TVSTAT); R(0x25F80006,"VRSIZE",r.VRSIZE);
    R(0x25F80008,"HCNT",  r.HCNT);   R(0x25F8000A,"VCNT",  r.VCNT);
    R(0x25F8000C,"EWDR",  r.EWDR);
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
    R(0x25F800FC,"PRIR",  r.PRIR);   R(0x25F800FE,"(rsvd)",r.pad);
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
    // ST-058-R2 §3.1 : DISP=bit15 (display on/off), BSMC=bit8 (border color mode)
    bool disp=(r.TVMD>>15)&1;
    d<<"  DISP="<<(disp?"ON":"OFF")<<"  BSMC="<<((r.TVMD>>8)&1)<<" (border color mode)\n";
    d<<"  LSMD="<<lm[(r.TVMD>>6)&3]<<"\n";
    d<<"  VRES="<<vr[(r.TVMD>>4)&3]<<"  HRES="<<hr[r.TVMD&3]<<"\n";
    d<<"  Signal: "<<(Vdp2Regs->TVSTAT&1?"PAL":"NTSC")<<"\n";}

    // EXTEN/TVSTAT/counters
    d<<"\n=== EXTEN=0x"<<HEX4(r.EXTEN)<<"  TVSTAT=0x"<<HEX4(r.TVSTAT)<<" ===\n";
    d<<"  EXLTEN="<<(r.EXTEN&1)<<"  EXSYEN="<<((r.EXTEN>>1)&1)<<"\n";
    // ST-058-R2 §3.3 : TVSTAT bit9=EXLTFG (external latch flag), bit8=EXSYFG (external sync flag)
    d<<"  EXLTFG="<<((r.TVSTAT>>9)&1)<<"  EXSYFG="<<((r.TVSTAT>>8)&1)<<"\n";
    d<<"  ODD="<<((r.TVSTAT>>1)&1)<<"  PAL="<<(r.TVSTAT&1)<<"\n";
    // ST-058-R2 §3.3 : bit2=HBSY (H-blank busy), bit3=VBSY (V-blank busy)
    d<<"  HBSY="<<((r.TVSTAT>>2)&1)<<"  VBSY="<<((r.TVSTAT>>3)&1)<<"\n";
    // ST-058-R2 ch.2 p.23-24 + EXLTEN (p.19) : HCNT/VCNT ne sont PAS des
    // compteurs live. Avec EXLTEN=0 ils sont latches a la lecture du
    // registre EXTEN par le jeu ; avec EXLTEN=1, par un signal externe.
    // Les afficher a cote de HBSY/VBSY (qui, eux, sont live) laissait
    // croire a une incoherence quand le jeu n'avait pas relu EXTEN depuis
    // longtemps. L'etiquette dit maintenant d'ou vient la valeur.
    d<<"  HCNT="<<DEC(r.HCNT)<<"  VCNT="<<DEC(r.VCNT)
      <<"  (valeurs latchees, "<<((r.EXTEN&0x200)?"EXLTEN=1: signal externe":"EXLTEN=0: derniere lecture de EXTEN")<<")\n";

    // EWDR — External Write Data Register (ST-058-R2 §3.4 / addr 0x25F8000C)
    // Contient la donnée écrite lors d'un accès externe au VDP2. 16 bits, écriture seule.
    d<<"\n=== EWDR=0x"<<HEX4(r.EWDR)<<" (External Write Data) ===\n";
    d<<"  raw=0x"<<HEX4(r.EWDR)<<"  (write-only — valeur indéterminée en lecture)\n";

    // VRSIZE/RAMCTL
    d<<"\n=== VRAM/RAMCTL ===\n";
    {static const char*crm[]={"1024x16bit(m0)","2048x16bit(m1)","1024x32bit(m2)","(rsvd)"};
    d<<"  VRSIZE=0x"<<HEX4(r.VRSIZE)<<((r.VRSIZE>>15)&1?"  (8Mbit)":"  (4Mbit)")<<"\n";
    d<<"  CRMD="<<crm[(r.RAMCTL>>12)&3]<<"\n";
    // ST-058-R2 §3.6 : RAMCTL bit8=VRAMD (VRAM-A mode), bit9=VRBMD (VRAM-B mode)
    d<<"  VRAMD="<<((r.RAMCTL>>8)&1)<<"  VRBMD="<<((r.RAMCTL>>9)&1)<<"\n";}

    // Cycle patterns
    d<<"\n=== VRAM Cycle Patterns ===\n";
    auto cyc=[&](const char*n,u16 L,u16 U){
        d<<"  "<<n<<": T0="<<decodeVramTiming((L>>12)&0xF)<<" T1="<<decodeVramTiming((L>>8)&0xF)
          <<" T2="<<decodeVramTiming((L>>4)&0xF)<<" T3="<<decodeVramTiming(L&0xF)
          <<"  T4="<<decodeVramTiming((U>>12)&0xF)<<" T5="<<decodeVramTiming((U>>8)&0xF)
          <<" T6="<<decodeVramTiming((U>>4)&0xF)<<" T7="<<decodeVramTiming(U&0xF)<<"\n";};
    cyc("VRAM-A0",r.CYCA0L,r.CYCA0U); cyc("VRAM-A1",r.CYCA1L,r.CYCA1U);
    cyc("VRAM-B0",r.CYCB0L,r.CYCB0U); cyc("VRAM-B1",r.CYCB1L,r.CYCB1U);
    d<<"  (N0VCST/N1VCST = table de vertical cell scroll NBG0/NBG1, "
       "PROHIB = reglage interdit, ------ = no access)\n";
    // ST-058-R2 §3.2 p.32 : T0-T7 ne sont tous valides qu'en mode normal
    // (HRESO 000/001). En haute resolution ou en mode moniteur exclusif,
    // seuls T0-T3 sont pris en compte, T4-T7 sont ignores par le materiel.
    if ((r.TVMD & 0x6) != 0)
        d<<"  (HRESO hi-res / moniteur exclusif : seuls T0-T3 sont en vigueur, "
           "T4-T7 ignores)\n";
    // Une banque monopolisee par un fond en rotation ignore entierement son
    // cycle pattern (RAMCTL RDBSx, §3.3 p.149 ; RBG1 prend VRAM-B0 et B1 en
    // entier). Le signaler evite de lire un pattern qui ne s'applique pas.
    if ((r.BGON & 0x20) != 0)
        d<<"  (RBG1 affiche : VRAM-B0 et VRAM-B1 lui sont reservees, "
           "leurs cycle patterns sont ignores)\n";
    if ((r.BGON & 0x10) != 0) {
        static const char* bkn[4] = {"VRAM-A0","VRAM-A1","VRAM-B0","VRAM-B1"};
        for (int b = 0; b < 4; b++)
            if (((r.RAMCTL >> (b*2)) & 0x3) != 0)
                d<<"  (RBG0 : "<<bkn[b]<<" lui est reservee (RAMCTL), "
                   "son cycle pattern est ignore)\n";
    }

    // BGON §3.7 : bits 5-0 = display enable (NBG0-RBG1), bits 13-8 = transparency (NBG0-RBG1, tous les 6 layers)
    d<<"\n=== BGON=0x"<<HEX4(r.BGON)<<" (Display Enable) ===\n";
    {static const char*bn[]={"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1"};
    for(int i=0;i<6;i++) {
        d<<"  "<<bn[i]<<": "<<((r.BGON>>i)&1?"ON ":"OFF");
        // ST-058-R2 §3.7 : bits 13-8 = TRPN5-TRPN0 (transparency pour NBG0..RBG1, tous présents)
        d<<"  transparent="<<((r.BGON>>(8+i))&1);
        d<<"\n";
    }}

    // MZCTL
    d<<"\n=== MZCTL=0x"<<HEX4(r.MZCTL)<<" (Mosaic) ===\n";
    {static const char*mp[]={"NBG0","NBG1","NBG2","NBG3","RBG0"};
    d<<"  H="<<DEC(((r.MZCTL>>8)&0xF)+1)<<"  V="<<DEC(((r.MZCTL>>12)&0xF)+1)<<"\n";
    for(int i=0;i<5;i++) d<<"  "<<mp[i]<<"="<<((r.MZCTL>>i)&1?"ON":"off")<<"\n";}

    // SFSEL/SFCODE — RE-VÉRIFIÉ contre VDP2 User's Manual ST-058-R2 p.220-221.
    // SFSEL (§write-only, 180024H) n'a que 5 bits réels, pas 6 : bit0=N0SFCS
    // ("For NBG0 (or RBG1)" — RBG1 PARTAGE le bit 0 avec NBG0, ce n'est pas
    // un bit séparé), bit1=N1SFCS, bit2=N2SFCS, bit3=N3SFCS, bit4=R0SFCS.
    // Mon décodage précédent inventait un 6e bit pour RBG1 : bug corrigé.
    d<<"\n=== Special Function: SFSEL=0x"<<HEX4(r.SFSEL)<<"  SFCODE=0x"<<HEX4(r.SFCODE)<<" ===\n";
    {static const char*sf[]={"NBG0 (or RBG1)","NBG1","NBG2","NBG3","RBG0"};
    for(int i=0;i<5;i++) d<<"  "<<sf[i]<<": pattern "<<((r.SFSEL>>i)&1?"B":"A")<<"\n";
    // SFCODE (p.221, Fig 10.7) : CE N'EST PAS un motif spatial/damier comme
    // je l'avais supposé la dernière fois — chaque bit SFCDxN active la
    // fonction spéciale pour les dots dont les 4 bits de poids faible du
    // CODE COULEUR valent {2N, 2N+1}. SFCDA=bits7-0 (pattern A), SFCDB=
    // bits15-8 (pattern B). Décodage en plages de code couleur, pas en pixels.
    auto sfRanges=[&](const char*label,u8 byte){
        d<<"  "<<label<<": ";
        bool any=false;
        for(int b=0;b<8;b++) if((byte>>b)&1){d<<std::hex<<std::uppercase<<(2*b)<<"-"<<(2*b+1)<<"H ";any=true;}
        if(!any)d<<"none";
        d<<std::dec<<"\n";};
    sfRanges("Pattern A (color code low nibble)", r.SFCODE&0xFF);
    sfRanges("Pattern B (color code low nibble)", (r.SFCODE>>8)&0xFF);}

    // CHCTLA/B
    d<<"\n=== Character Control ===\n";
    // ST-058-R2 4.5 : N0CHCN/R0CHCN (3 bits) 000=16, 001=256, 010=2048,
    // 011=32768, 100=16.77M, 101-111 interdit. Il y avait un doublon
    // "16(4bpp)" en index 1 : toute la table etait donc decalee d'un cran a
    // partir de la, et un NBG0 en 256 couleurs (CHCTLA=0x1010, N0CHCN=1)
    // etait annonce en 16 couleurs.
    {static const char*cc5[]={"16(4bpp)","256(8bpp)","2048(11bpp)","32768(15bpp)","16M(24bpp)","(interdit)","(interdit)","(interdit)"};
    // N1CHCN (2 bits) : 00=16, 01=256, 10=2048, 11=32768. Les deux dernieres
    // valeurs sont licites, elles etaient rendues "?".
    static const char*cc2[]={"16(4bpp)","256(8bpp)","2048(11bpp)","32768(15bpp)"};
    static const char*bsz[]={"512x256","512x512","1024x256","1024x512"};
    d<<"  CHCTLA=0x"<<HEX4(r.CHCTLA)<<"\n";
    int n0bm=(r.CHCTLA>>1)&1,n0sz=r.CHCTLA&1,n0cc=(r.CHCTLA>>4)&7,n0bw=(r.CHCTLA>>2)&3;
    d<<"    NBG0: bm="<<n0bm<<"  char="<<(n0sz?"16x16":"8x8")<<"  col="<<cc5[n0cc];
    if(n0bm)d<<"  bmpSz="<<bsz[n0bw];d<<"\n";
    int n1bm=(r.CHCTLA>>9)&1,n1sz=(r.CHCTLA>>8)&1,n1cc=(r.CHCTLA>>12)&3,n1bw=(r.CHCTLA>>10)&3;
    d<<"    NBG1: bm="<<n1bm<<"  char="<<(n1sz?"16x16":"8x8")<<"  col="<<cc2[n1cc];
    if(n1bm)d<<"  bmpSz="<<bsz[n1bw];d<<"\n";
    d<<"  CHCTLB=0x"<<HEX4(r.CHCTLB)<<"\n";
    // CHCTLB (0x25F8002A) — RE-VÉRIFIÉ contre VDP2 User's Manual ST-058-R2
    // §4.5 p.59-61 (texte exact du manuel officiel, plus de "?"/doute) :
    //   bit  0     : N2CHSZ (char size NBG2, 0=8x8 1=16x16)
    //   bit  1     : N2CHCN (color NBG2, 0=16col 1=256col)
    //   bit  4     : N3CHSZ (char size NBG3)
    //   bit  5     : N3CHCN (color NBG3)
    //   bit  8     : R0CHSZ (char size RBG0)
    //   bit  9     : R0BMEN (bitmap enable RBG0)
    //   bit  10    : R0BMSZ (bitmap size RBG0 — 1 SEUL bit : 0=512x256, 1=512x512 ;
    //                différent de N0/N1BMSZ qui font 2 bits/4 tailles)
    //   bits 14-12 : R0CHCN2-0 (color count RBG0, index dans cc5[])
    // Le code précédent lisait bits 2/4/8/10/12 de façon incohérente : NBG2/
    // NBG3/RBG0 étaient en fait mélangés entre eux (RBG0 bitmap-enable lu au
    // mauvais endroit, NBG3 char size lisait en fait le bit de RBG0 bitmap
    // size, etc). Bug corrigé après vérification directe du manuel officiel.
    d<<"    NBG2: char="<<((r.CHCTLB&1)?"16x16":"8x8")<<"  col="<<cc2[(r.CHCTLB>>1)&1]<<"\n";
    d<<"    NBG3: char="<<(((r.CHCTLB>>4)&1)?"16x16":"8x8")<<"  col="<<cc2[(r.CHCTLB>>5)&1]<<"\n";
    int r0sz =(r.CHCTLB>>8) &1;   // bit  8 = R0CHSZ
    int r0bm =(r.CHCTLB>>9) &1;   // bit  9 = R0BMEN
    int r0bw =(r.CHCTLB>>10)&1;   // bit 10 = R0BMSZ (1 bit)
    int r0cc =(r.CHCTLB>>12)&7;   // bits 14-12 = R0CHCN2-0
    static const char*r0bsz[]={"512x256","512x512"};  // table dédiée : R0BMSZ n'a que 2 valeurs, pas 4
    d<<"    RBG0: bm="<<r0bm<<"  char="<<(r0sz?"16x16":"8x8")<<"  col="<<cc5[r0cc&7];
    if(r0bm)d<<"  bmpSz="<<r0bsz[r0bw&1];d<<"\n";}

    // BMPNA/B
    d<<"\n=== Bitmap Palette Numbers ===\n";
    // ST-058-R2 §3.11 : BMPNA[2:0]=N0 palette, BMPNA[10:8]=N0 special priority
    //                    BMPNA[6:4]=N1 palette, BMPNA[14:12]=N1 special priority
    d<<"  BMPNA=0x"<<HEX4(r.BMPNA)
      <<"  N0-pal="<<DEC(r.BMPNA&7)<<"  N0-spr="<<DEC((r.BMPNA>>8)&7)
      <<"  N1-pal="<<DEC((r.BMPNA>>4)&7)<<"  N1-spr="<<DEC((r.BMPNA>>12)&7)<<"\n";
    // ST-058-R2 §3.11 : BMPNB[2:0]=RBG0 palette, BMPNB[10:8]=RBG0 special priority
    d<<"  BMPNB=0x"<<HEX4(r.BMPNB)<<"  RBG0-pal="<<DEC(r.BMPNB&7)<<"  RBG0-spr="<<DEC((r.BMPNB>>8)&7)<<"\n";

    // PNC
    d<<"\n=== Pattern Name Control ===\n";
    auto pnc=[&](const char*n,u16 v){
        d<<"  "<<n<<"=0x"<<HEX4(v)<<"  "<<((v&0x8000)?"1word":"2word");
        if(v&0x8000)d<<"  CNs="<<((v>>14)&1)<<"  SPb="<<((v>>9)&1)<<"  SCCb="<<((v>>8)&1)<<"  palS="<<((v>>5)&7)<<"  colS="<<(v&0x1F);
        d<<"\n";};
    pnc("PNCN0",r.PNCN0);pnc("PNCN1",r.PNCN1);pnc("PNCN2",r.PNCN2);pnc("PNCN3",r.PNCN3);pnc("PNCR ",r.PNCR);

    // PLSZ
    d<<"\n=== PLSZ=0x"<<HEX4(r.PLSZ)<<" (Plane Size) ===\n";
    // ST-058-R2 §3.14 : N0[1:0], N1[3:2], N2[5:4], N3[7:6], R0A[9:8], R0B[11:10], R1[15:14]
    {static const char*ps[]={"1Hx1V","2Hx1V","(rsvd)","2Hx2V"};
    /* ST-58-R2 18003AH :
     *   1-0 N0PLSZ   3-2 N1PLSZ   5-4 N2PLSZ   7-6 N3PLSZ
     *   9-8 RAPLSZ   11-10 RAOVR  13-12 RBPLSZ  15-14 RBOVR
     * R0B lisait les bits 11-10 (RAOVR) et R1 les bits 15-14 (RBOVR) : deux
     * screen-over affiches comme des tailles de plan. RBPLSZ est en 13-12,
     * et RBG1 etant toujours dessine avec le parametre B (6.1), R0B et R1
     * valent tous deux RBPLSZ. */
    static const char*ov[]={"repeat","transparent","charPat","512x512"};
    d<<"  N0="<<ps[r.PLSZ&3]<<"  N1="<<ps[(r.PLSZ>>2)&3]<<"  N2="<<ps[(r.PLSZ>>4)&3]
      <<"  N3="<<ps[(r.PLSZ>>6)&3]<<"  R0A="<<ps[(r.PLSZ>>8)&3]
      <<"  R0B="<<ps[(r.PLSZ>>12)&3]<<"  R1="<<ps[(r.PLSZ>>12)&3]<<"\n";
    d<<"  screen-over: RAOVR="<<DEC((r.PLSZ>>10)&3)<<"("<<ov[(r.PLSZ>>10)&3]<<")"
      <<"  RBOVR="<<DEC((r.PLSZ>>14)&3)<<"("<<ov[(r.PLSZ>>14)&3]<<")\n";}

    // Map offsets
    d<<"\n=== Map Offsets ===\n";
    d<<"  MPOFN=0x"<<HEX4(r.MPOFN)<<"  N0="<<DEC(r.MPOFN&7)<<"  N1="<<DEC((r.MPOFN>>4)&7)<<"  N2="<<DEC((r.MPOFN>>8)&7)<<"  N3="<<DEC((r.MPOFN>>12)&7)<<"\n";
    // ST-058-R2 §3.15 : MPOFR[2:0]=R0A, MPOFR[6:4]=R0B, MPOFR[10:8]=R1
    d<<"  MPOFR=0x"<<HEX4(r.MPOFR)<<"  R0A="<<DEC(r.MPOFR&7)<<"  R0B="<<DEC((r.MPOFR>>4)&7)<<"  R1="<<DEC((r.MPOFR>>8)&7)<<"\n";

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
    // ST-058-R2 §3.16 : SCxDN bits 7-0 = fractional part (not bits 15-8)
    d<<"  NBG0: X="<<DEC((s16)r.SCXIN0)<<"."<<DEC(r.SCXDN0&0xFF)<<"  Y="<<DEC((s16)r.SCYIN0)<<"."<<DEC(r.SCYDN0&0xFF)<<"\n";
    d<<"  NBG1: X="<<DEC((s16)r.SCXIN1)<<"."<<DEC(r.SCXDN1&0xFF)<<"  Y="<<DEC((s16)r.SCYIN1)<<"."<<DEC(r.SCYDN1&0xFF)<<"\n";
    d<<"  NBG2: X="<<DEC((s16)r.SCXN2)<<"  Y="<<DEC((s16)r.SCYN2)<<"\n";
    d<<"  NBG3: X="<<DEC((s16)r.SCXN3)<<"  Y="<<DEC((s16)r.SCYN3)<<"\n";

    // Zoom
    d<<"\n=== ZMCTL=0x"<<HEX4(r.ZMCTL)<<" (Zoom) ===\n";
    // ST-058-R2 §3.18 : 0=1/1, 1=H:1/2, 2=H+V:1/4, 3=(rsvd)
    {static const char*zm[]={"1/1","H:1/2","H+V:1/4","(rsvd)"};
    d<<"  NBG0="<<zm[r.ZMCTL&3]<<"  NBG1="<<zm[(r.ZMCTL>>8)&3]<<"\n";
    /* ST-058-R2 ch.5.2 p.126, Figure 5.2 : l'increment de coordonnee est un
     * point fixe = partie entiere sur 3 bits dans ZMxINn[2:0] + partie
     * fractionnaire sur 8 bits dans ZMxDNn[15:8].
     *
     * L'ancien affichage concatenait la partie entiere, un point, puis la
     * VALEUR BRUTE du registre fractionnaire : ZMXN1I=0x0000 / ZMXN1D=0x8000
     * sortait "0.32768" au lieu de 0.5. Ce n'etait pas un nombre.
     *
     * On imprime maintenant l'increment reel, plus le facteur de zoom
     * correspondant (1/increment), qui est ce que le renderer manipule sous
     * le nom coordincx/coordincy. */
    {auto inc=[](u16 I,u16 D){ return (double)(I & 0x7) + (double)((D >> 8) & 0xFF) / 256.0; };
    auto zoom=[](double v){ return (v > 0.0) ? 1.0/v : 0.0; };
    double ix0=inc(r.ZMXN0.part.I,r.ZMXN0.part.D), iy0=inc(r.ZMYN0.part.I,r.ZMYN0.part.D);
    double ix1=inc(r.ZMXN1.part.I,r.ZMXN1.part.D), iy1=inc(r.ZMYN1.part.I,r.ZMYN1.part.D);
    d<<"  NBG0 inc: x="<<ix0<<" y="<<iy0<<"  (zoom x"<<zoom(ix0)<<" / x"<<zoom(iy0)<<")\n";
    d<<"  NBG1 inc: x="<<ix1<<" y="<<iy1<<"  (zoom x"<<zoom(ix1)<<" / x"<<zoom(iy1)<<")\n";}}

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
    // ST-058-R2 §3.20 : NBG1 line scroll table address est LSTA1 (non VCSTA)
    sb("NBG1",(r.SCRCTL>>8)&0x3F,r.LSTA1.all,r.VCSTA.all);}

    // Table addresses
    d<<"\n=== Cell/Line/Back Screen Addresses ===\n"<<std::hex<<std::uppercase;
    d<<"  VCSTA=0x"<<HEX8(0x05E00000|((r.VCSTA.all&0x7FFFE)<<1))<<"\n";
    d<<"  LSTA0=0x"<<HEX8(0x05E00000|((r.LSTA0.all&0x7FFFE)<<1))<<"\n";
    d<<"  LSTA1=0x"<<HEX8(0x05E00000|((r.LSTA1.all&0x7FFFE)<<1))<<"\n";
    // ST-058-R2 ch.7.1 p.174 : LCTA18~LCTA16 = LCTAU[2:0], LCTA15~LCTA0 =
    // LCTAL[15:0] -- LCTA0 EXISTE (contrairement a LSTA/VCSTA ou le bit 0
    // est inutilise). Le masque 0x7FFFE l'ecrasait : une table a adresse
    // impaire etait mal decodee.
    d<<"  LCTA =0x"<<HEX8(0x05E00000|((r.LCTA.all&0x7FFFF)<<1));
    d<<(r.LCTA.part.U&0x8000?"  (per-line)":"  (single)")<<"\n";
    // ST-058-R2 ch.7.2 p.176-177 : BKTA18~BKTA16 = BKTAU[2:0],
    // BKTA15~BKTA0 = BKTAL[15:0], et
    //   (adresse VRAM) = (valeur 19 bits du registre) x 2
    // L'ancienne formule ((BKTAU&7)<<17) | (BKTAL & 0xFFFE) oubliait le x2
    // sur le mot bas et perdait BKTA0, ce qui donnait une adresse fausse
    // (et en contradiction avec celle imprimee par vdp2debug.c plus bas
    // dans le meme export).
    {u32 bk = 0x05E00000 | (((((u32)(r.BKTAU & 0x7) << 16) | r.BKTAL) & 0x7FFFF) << 1);
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
    /* ST-58-R2 p.283, KTCTL (1800B4H), parametre A (parametre B decale de 8) :
     *   bit 0    RAKTE   activation
     *   bit 1    RAKDBS  taille : 0 = 2 mots (32 bits), 1 = 1 mot (16 bits)
     *   bits 3-2 RAKMD   mode : kx&ky / kx / ky / Xp
     *   bit 4    RAKLCE  donnee de couleur de ligne
     * L'ancien decodage lisait 3 bits a partir du bit 2, melangeant RAKMD
     * et RAKLCE, et ignorait RAKDBS alors que sa table pretendait afficher
     * une taille. */
    auto kt=[&](const char*n,int sh){
        static const char*kmd[]={"kx&ky","kx","ky","Xp"};
        d<<"    "<<n<<": en="<<((r.KTCTL>>sh)&1)
         <<"  taille="<<(((r.KTCTL>>(sh+1))&1)?"1 mot (16b)":"2 mots (32b)")
         <<"  mode="<<DEC((r.KTCTL>>(sh+2))&3)<<"("<<kmd[(r.KTCTL>>(sh+2))&3]<<")"
         <<"  lineCol="<<((r.KTCTL>>(sh+4))&1)<<"\n";};
    kt("ParamA",0);kt("ParamB",8);
    d<<"  KTAOF=0x"<<HEX4(r.KTAOF)<<"  A-off="<<DEC(r.KTAOF&7)<<"  B-off="<<DEC((r.KTAOF>>8)&7)<<"\n";
    d<<"  OVPNRA=0x"<<HEX4(r.OVPNRA)<<"  OVPNRB=0x"<<HEX4(r.OVPNRB)<<"\n";
    // ST-058-R2 §3.21 : RPTA addr = 0x05E00000 | (RPTAU[14:0] << 17) | (RPTAL & 0xFFFE)
    u32 rp = 0x05E00000 | (((u32)(r.RPTA.part.U & 0x7FFF) << 17) | ((u32)(r.RPTA.part.L & 0xFFFE)));
    d<<"  RPTA=0x"<<std::hex<<std::uppercase<<HEX8(rp)<<std::dec<<"\n";}

    // Windows
    d<<"\n=== Windows ===\n";
    {auto wc=[&](const char*n,u16 wctl){
        d<<"  "<<n<<" WCTL=0x"<<HEX4(wctl)<<": ";
        // WCTLA/B/C/D (§3.28) : bit1=W0en, bit3=W1en, bit5=SPWen, bit7=logic(AND/OR)
        // Seuls les bits 1, 3, 5 sont des enables de fenêtre.
        if(!(wctl&0x2A)){d<<"none\n";return;}
        if(wctl&0x02)d<<"W0("<<((wctl&0x01)?"out":"in")<<") ";
        if(wctl&0x08)d<<"W1("<<((wctl&0x04)?"out":"in")<<") ";
        if(wctl&0x20)d<<"SPW("<<((wctl&0x10)?"out":"in")<<") ";
        if(wctl&0x80)d<<"AND ";d<<"\n";};
    d<<"  W0: ("<<DEC((s16)r.WPSX0)<<","<<DEC((s16)r.WPSY0)<<")->("<<DEC((s16)r.WPEX0)<<","<<DEC((s16)r.WPEY0)<<")";
    if(r.LWTA0.all&0x80000000)d<<"  [LINE 0x"<<std::hex<<std::uppercase<<(0x05E00000UL|((r.LWTA0.all&0x7FFFEUL)<<1))<<std::dec<<"]\n"; else d<<"\n";
    d<<"  W1: ("<<DEC((s16)r.WPSX1)<<","<<DEC((s16)r.WPSY1)<<")->("<<DEC((s16)r.WPEX1)<<","<<DEC((s16)r.WPEY1)<<")";
    if(r.LWTA1.all&0x80000000)d<<"  [LINE 0x"<<std::hex<<std::uppercase<<(0x05E00000UL|((r.LWTA1.all&0x7FFFEUL)<<1))<<std::dec<<"]\n"; else d<<"\n";
    wc("NBG0",r.WCTLA);wc("NBG1",r.WCTLA>>8);wc("NBG2",r.WCTLB);wc("NBG3",r.WCTLB>>8);
    // ST-058-R2 §3.27 : WCTLC[7:0]=RBG0, WCTLC[15:8]=SPR, WCTLD[7:0]=RBG1, WCTLD[15:8]=CC
    wc("RBG0",r.WCTLC);wc("SPR ",r.WCTLC>>8);wc("RBG1",r.WCTLD);wc("CC  ",r.WCTLD>>8);}

    // SPCTL/SDCTL
    d<<"\n=== SPCTL=0x"<<HEX4(r.SPCTL)<<"  SDCTL=0x"<<HEX4(r.SDCTL)<<" ===\n";
    {static const char*spcond[]={"Prio<=CCnum","Prio==CCnum","Prio>=CCnum","MSB colour"};
    // ST-058-R2 ch.9 "Sprite Control Register" (p.207) : SPCCCS=bits13-12,
    // SPCCN=bits10-8, SPCLMD=bit5, SPWINEN=bit4, SPTYPE=bits3-0.
    // Les bits 7-6, 11 et 15-14 sont réservés : SPCTL ne contient AUCUN
    // champ de profondeur couleur (le "col[6:4]" affiché ici était
    // inventé), et SPWINEN était lu au bit 11 au lieu du bit 4.
    // Les formats de données sprite sont Figure 9.1 de ST-058-R2, pas
    // "table 4.1" de ST-013-R3 (qui est le manuel du VDP1).
    d<<"  SPTYPE="<<DEC(r.SPCTL&0xF)<<" (ST-058-R2 fig.9.1)"
      <<"  SPCLMD="<<((r.SPCTL>>5)&1)<<((r.SPCTL>>5)&1?" (palette+RGB)":" (palette only)")
      <<"  SPWINEN="<<((r.SPCTL>>4)&1)<<"\n";
    d<<"  CCcond="<<spcond[(r.SPCTL>>12)&3]<<"  CCnum="<<DEC((r.SPCTL>>8)&7)<<"\n";
    // ST-058-R2 ch.14 "Shadow Control Register" (p.259) : bit0=N0SDEN,
    // bit1=N1SDEN, bit2=N2SDEN, bit3=N3SDEN, bit4=R0SDEN, bit5=BKSDEN,
    // bit8=TPSDSL. Le bit 0 était étiqueté "MSBshadow", ce qui n'existe
    // pas dans ce registre : c'est l'activation d'ombre sur NBG0.
    {static const char*sd[]={"N0","N1","N2","N3","R0","BK"};
    d<<"  SDEN:";
    for(int i=0;i<6;i++)d<<" "<<sd[i]<<"="<<((r.SDCTL>>i)&1);
    d<<"  TPSDSL="<<((r.SDCTL>>8)&1)<<"\n";}}

    // CRAOFA/B
    d<<"\n=== Color RAM Offset ===\n"<<std::hex;
    d<<"  CRAOFA=0x"<<HEX4(r.CRAOFA)<<"  N0=0x"<<((r.CRAOFA&7)<<8)<<"  N1=0x"<<(((r.CRAOFA>>4)&7)<<8)<<"  N2=0x"<<(((r.CRAOFA>>8)&7)<<8)<<"  N3=0x"<<(((r.CRAOFA>>12)&7)<<8)<<"\n";
    // ST-058-R2 §3.30 : CRAOFB[2:0]=RBG0, CRAOFB[6:4]=SPR, CRAOFB[10:8]=RBG1
    d<<"  CRAOFB=0x"<<HEX4(r.CRAOFB)<<"  RBG0=0x"<<((r.CRAOFB&7)<<8)<<"  SPR=0x"<<(((r.CRAOFB>>4)&7)<<8)<<"  RBG1=0x"<<(((r.CRAOFB>>8)&7)<<8)<<"\n"<<std::dec;

    // LNCLEN
    d<<"\n=== LNCLEN=0x"<<HEX4(r.LNCLEN)<<" (Line Color Insertion) ===\n";
    // ST-058-R2 ch.11.3 "Line Color Screen Enable Register" (p.231) :
    // bit0=N0LCEN (ou RBG1), bit1=N1LCEN (ou EXBG), bit2=N2LCEN,
    // bit3=N3LCEN, bit4=R0LCEN, bit5=SPLCEN. Il n'y a que 6 bits : RBG1
    // n'a pas d'entrée propre (il partage celle de NBG0), et SPR est au
    // bit 5. L'ancienne table de 7 entrées décalait SPR au bit 6.
    {static const char*lp[]={"NBG0/RBG1","NBG1/EXBG","NBG2","NBG3","RBG0","SPR"};
    for(int i=0;i<6;i++)d<<"  "<<lp[i]<<"="<<((r.LNCLEN>>i)&1?"ON":"off")<<"\n";}

    // SFPRMD
    d<<"\n=== SFPRMD=0x"<<HEX4(r.SFPRMD)<<" (Special Priority) ===\n";
    // ST-058-R2 §3.32 : bits 11-0 = 6 layers x 2 bits, bit12 = SPR (single bit)
    {static const char*sp[]={"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1"};
    static const char*sm[]={"none","per-tile","per-pixel","(undoc)"};
    for(int i=0;i<6;i++)d<<"  "<<sp[i]<<"="<<sm[(r.SFPRMD>>(i*2))&3]<<"\n";
    d<<"  SPR=per-pixel="<<((r.SFPRMD>>12)&1)<<"\n";}

    // CCCTL/SFCCMD
    d<<"\n=== CCCTL=0x"<<HEX4(r.CCCTL)<<"  SFCCMD=0x"<<HEX4(r.SFCCMD)<<" ===\n";
    // ST-058-R2 §3.33 : BOKEN=bit15, EXCCEN=bit9, BKCCEN=bit8
    {d<<"  BOKEN="<<((r.CCCTL>>15)&1)<<"  EXCCEN="<<((r.CCCTL>>9)&1)<<"  BKCCEN="<<((r.CCCTL>>8)&1)<<"\n";
    static const char*cp[]={"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1","SPR","BACK"};
    d<<"  CC enabled: ";for(int i=0;i<8;i++)if((r.CCCTL>>i)&1)d<<cp[i]<<" ";d<<"\n";
    static const char*sl[]={"NBG0","NBG1","NBG2","NBG3","RBG0","RBG1"};
    for(int i=0;i<6;i++)d<<"  "<<sl[i]<<" SCC="<<DEC((r.SFCCMD>>(i*2))&3)<<"\n";}

    // Priority numbers (§3.29) :
    //   PRINA : NBG0 (bits 2-0), NBG1 (bits 10-8)
    //   PRINB : NBG2 (bits 2-0), NBG3 (bits 10-8)
    //   PRIR  : RBG0 (bits 2-0), RBG1 (bits 10-8)  ← registre dédié
    //   PRISA/B/C/D : sprites SP0-SP7
    d<<"\n=== Priority Numbers ===\n";
    d<<"  N0="<<DEC(r.PRINA&7)<<" N1="<<DEC((r.PRINA>>8)&7)<<" N2="<<DEC(r.PRINB&7)<<" N3="<<DEC((r.PRINB>>8)&7)<<"\n";
    d<<"  R0="<<DEC(r.PRIR&7)<<" R1="<<DEC((r.PRIR>>8)&7)<<"\n";
    // PRISA/B/C/D (§3.29) : SP0-SP7, 4 bits par sprite dans 4 registres 16-bit
    // Lecture explicite pour éviter tout problème d'alignement/padding
    {d<<"  Sprite:";
    d<<" SP0="<<DEC(r.PRISA&7)   <<" SP1="<<DEC((r.PRISA>>8)&7);
    d<<" SP2="<<DEC(r.PRISB&7)   <<" SP3="<<DEC((r.PRISB>>8)&7);
    d<<" SP4="<<DEC(r.PRISC&7)   <<" SP5="<<DEC((r.PRISC>>8)&7);
    d<<" SP6="<<DEC(r.PRISD&7)   <<" SP7="<<DEC((r.PRISD>>8)&7);
    d<<"\n";}

    // CC ratios
    d<<"\n=== Color Calculation Ratios ===\n";
    // CCRNA/B : N0-N3, CCRR : RBG0(bits4-0) + RBG1(bits12-8), CCRLB : Back(bits12-8)
    d<<"  N0="<<DEC(r.CCRNA&0x1F)<<" N1="<<DEC((r.CCRNA>>8)&0x1F)<<" N2="<<DEC(r.CCRNB&0x1F)<<" N3="<<DEC((r.CCRNB>>8)&0x1F)<<"\n";
    d<<"  R0="<<DEC(r.CCRR&0x1F)<<" R1="<<DEC((r.CCRR>>8)&0x1F)<<"\n";
    // CCRSA/B/C/D (§3.34) : CCR0-CCR7, 5 bits par sprite dans 4 registres 16-bit
    {d<<"  Sprite:";
    d<<" SP0="<<DEC(r.CCRSA&0x1F)    <<" SP1="<<DEC((r.CCRSA>>8)&0x1F);
    d<<" SP2="<<DEC(r.CCRSB&0x1F)    <<" SP3="<<DEC((r.CCRSB>>8)&0x1F);
    d<<" SP4="<<DEC(r.CCRSC&0x1F)    <<" SP5="<<DEC((r.CCRSC>>8)&0x1F);
    d<<" SP6="<<DEC(r.CCRSD&0x1F)    <<" SP7="<<DEC((r.CCRSD>>8)&0x1F);
    d<<"\n";}
    // CCRLB (§3.34) : bits 4-0 = Line color screen CC ratio, bits 12-8 = Back screen CC ratio
    d<<"  Line="<<DEC(r.CCRLB&0x1F)<<"  Back="<<DEC((r.CCRLB>>8)&0x1F)<<"\n";

    // Color offset
    d<<"\n=== Color Offset ===\n";
    d<<"  CLOFEN=0x"<<HEX4(r.CLOFEN)<<"  CLOFSL=0x"<<HEX4(r.CLOFSL)<<"\n";
    d<<"  BankA: R="<<DEC(s9(r.COAR))<<" G="<<DEC(s9(r.COAG))<<" B="<<DEC(s9(r.COAB))<<"\n";
    d<<"  BankB: R="<<DEC(s9(r.COBR))<<" G="<<DEC(s9(r.COBG))<<" B="<<DEC(s9(r.COBB))<<"\n";
    // ST-058-R2 ch.13.1 "Color Offset Enable Register" (p.251) :
    // bit0=N0COEN (ou RBG1), bit1=N1COEN, bit2=N2COEN, bit3=N3COEN,
    // bit4=R0COEN, bit5=BKCOEN, bit6=SPCOEN. Les bits 15-7 sont réservés
    // (l'ancien commentaire plaçait un "RBG1" inexistant au bit 7).
    {static const char*op[]={"NBG0","NBG1","NBG2","NBG3","RBG0","BACK","SPR","RBG1"};
    for(int i=0;i<8;i++)if((r.CLOFEN>>i)&1){
        bool useB=(r.CLOFSL>>i)&1;
        d<<"  "<<op[i]<<": bank"<<(useB?"B":"A")<<" R="<<DEC(s9(useB?r.COBR:r.COAR))<<" G="<<DEC(s9(useB?r.COBG:r.COAG))<<" B="<<DEC(s9(useB?r.COBB:r.COAB))<<"\n";}}

    pteDecodedRegs->setPlainText(QString::fromStdString(d.str()));
}

// ============================================================
//  Screen viewer : use CTRL + mouse to zoom & dezoom
// ============================================================



void UIDebugVDP2Viewer::wheelEvent(QWheelEvent *event)
{
    // Vérifie si la touche CTRL est enfoncée
    if (event->modifiers() & Qt::ControlModifier) {
        // Vérifie si le curseur est au-dessus du QGraphicsView du Screen Viewer
        if (gvScreen->underMouse()) {
            const double scaleFactor = 1.15; // Facteur de zoom
            if (event->angleDelta().y() > 0) {
                // Zoom avant
                gvScreen->scale(scaleFactor, scaleFactor);
            } else {
                // Zoom arrière
                gvScreen->scale(1.0 / scaleFactor, 1.0 / scaleFactor);
            }
            event->accept();
            return;
        }
    }
    QDialog::wheelEvent(event);
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
    // ST-058-R2 §3.6 / RAMCTL[13:12] = CRMD :
    //   0=RGB555 word (2048 entries x 2 bytes)
    //   1=RGB555 banked (2048 entries x 2 bytes, banked access)
    //   2=RGB888 (1024 entries x 4 bytes)
    //   3=(rsvd)
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
            if (mode <= 1) {
                // 16-bit BGR555
                u16 w = T2ReadWord(Vdp2ColorRam, (i*2) & 0xFFF);
                hex<<std::hex<<std::uppercase<<std::setw(4)<<std::setfill('0')<<(unsigned)w<<" ";
            } else {
                // 32-bit XRGB888 — on affiche les deux mots (MSW puis LSW)
                u32 addr = (u32)(i*4) & 0xFFC;
                u16 msw = T2ReadWord(Vdp2ColorRam, addr);
                u16 lsw = T2ReadWord(Vdp2ColorRam, addr+2);
                hex<<std::hex<<std::uppercase
                   <<std::setw(4)<<std::setfill('0')<<(unsigned)msw
                   <<std::setw(4)<<std::setfill('0')<<(unsigned)lsw<<" ";
            }
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
// ============================================================
//  vramBankRange - base et taille de la banque selectionnee.
//
//  L'ordre des cas doit suivre celui des entrees de cbVramBank dans le
//  .ui, qui est A, B, A0, A1, B0, B1. L'ancien code supposait
//  A0, A1, B0, B1, A, B : cinq entrees sur six pointaient sur la mauvaise
//  banque. Choisir "VRAM-B1 (0x60000-0x7FFFF)" affichait 0x40000 et
//  "VRAM-B0 (0x40000-0x5FFFF)" affichait 0x00000, l'en-tete "VRAM @ ..."
//  reportant cette fausse adresse sans rien signaler.
//
//  Les libelles decrivent la configuration 4 Mbit (VRSIZE bit 15 a 0) :
//  A = 0x00000-0x3FFFF, B = 0x40000-0x7FFFF, chacune divisible en deux
//  moities de 128 Ko. En 8 Mbit tout double, d'ou le facteur ci-dessous ;
//  les libelles du .ui, eux, restent ceux du 4 Mbit.
// ============================================================
void UIDebugVDP2Viewer::vramBankRange(u32 *base, u32 *size) const
{
    const u32 mul = (Vdp2Regs && ((Vdp2Regs->VRSIZE >> 15) & 1)) ? 2u : 1u;
    switch (cbVramBank->currentIndex()) {
        case 0: *base=0x00000;     *size=0x40000*mul; break; // VRAM-A
        case 1: *base=0x40000*mul; *size=0x40000*mul; break; // VRAM-B
        case 2: *base=0x00000;     *size=0x20000*mul; break; // VRAM-A0
        case 3: *base=0x20000*mul; *size=0x20000*mul; break; // VRAM-A1
        case 4: *base=0x40000*mul; *size=0x20000*mul; break; // VRAM-B0
        case 5: *base=0x60000*mul; *size=0x20000*mul; break; // VRAM-B1
        default: *base=0x00000;    *size=0x40000*mul; break;
    }
}

void UIDebugVDP2Viewer::updateVramHex()
{
    pteVramHex->clear();
    if (!Vdp2Ram) { pteVramHex->setPlainText("VDP2 RAM not available"); return; }

    // VRAM layout (Saturn HW Manual §3.2 / VRSIZE) :
    //   Total VRAM = 512KB (0x80000 octets) — toujours présent
    //   VRSIZE bit 15 = 0 → 4Mbit (A0=256KB, B0=256KB, pas de A1/B1)
    //   VRSIZE bit 15 = 1 → 8Mbit (A0=A1=B0=B1=128KB chacun)
    // Les banques 2-5 (divisions en 128KB) ne sont valides qu'en 8Mbit.
    u32 base=0, bSize=0;
    vramBankRange(&base, &bSize);

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
void UIDebugVDP2Viewer::clearItems()
{
    // "Final" (index 0) + son séparateur (index 1) sont fixes, ajoutés une
    // seule fois dans le constructeur : ils ne font jamais partie du
    // rebuild par-frame et ne sont donc jamais retirés ici.
    //
    // Mémorise la couche individuelle actuellement sélectionnée (si elle
    // n'est pas "Final") pour pouvoir la restaurer une fois la liste
    // reconstruite par les addItem() qui suivent. UIDebugVDP2::updateScreenInfos()
    // appelle clearItems() puis addItem() à CHAQUE frame ("Next Frame") pour
    // tenir la liste des couches actives à jour ; sans cette restauration,
    // la sélection de l'utilisateur (ex: RBG1) revenait systématiquement à
    // la première couche active (ex: NBG0) à chaque frame. Si "Final" était
    // sélectionnée (index 0), elle reste en place automatiquement puisqu'on
    // ne touche jamais à cet index.
    mRestoreScreenId = (cbScreen->currentIndex() > 1);
    if (mRestoreScreenId)
        mScreenIdToRestore = cbScreen->itemData(cbScreen->currentIndex()).toInt();

    while (cbScreen->count() > 2)
        cbScreen->removeItem(2);
}

void UIDebugVDP2Viewer::addItem(int id)
{
    const char *label = NULL;
    switch (id) {
        case NBG0:   label = "NBG0";   break;
        case NBG1:   label = "NBG1";   break;
        case NBG2:   label = "NBG2";   break;
        case NBG3:   label = "NBG3";   break;
        case RBG0:   label = "RBG0";   break;
        case RBG1:   label = "RBG1";   break;
        case SPRITE: label = "SPRITE"; break;
        default: return;
    }
    cbScreen->addItem(label, id);

    // Restaure la sélection mémorisée par clearItems() si la couche qu'on
    // vient d'ajouter est celle qui était affichée avant le rebuild.
    if (mRestoreScreenId && id == mScreenIdToRestore) {
        cbScreen->setCurrentIndex(cbScreen->count() - 1);
        mRestoreScreenId = false; // restauration faite
    }
}

int UIDebugVDP2Viewer::exec() { return QDialog::exec(); }

UIDebugVDP2Viewer::UIDebugVDP2Viewer(QWidget *p) : QDialog(p)
{
    setupUi(this);
    QGraphicsScene *sc = new QGraphicsScene(this); gvScreen->setScene(sc);
    QGraphicsScene *sc2= new QGraphicsScene(this); gvColorRam->setScene(sc2);
    vdp2texture=NULL; width=0; height=0;

    // "Final" (sortie déjà composée VDP1+VDP2, cf. VDP2_SCREEN_FINAL) reste
    // toujours pertinente tant que le VDP2 est initialisé, contrairement aux
    // couches individuelles (NBG0...SPRITE) qui dépendent de ce qui est
    // actuellement actif. On l'ajoute donc une seule fois ici, en position
    // fixe à l'index 0, plutôt que via le rebuild par-frame de
    // clearItems()/addItem() (voir ces deux méthodes plus bas).
    cbScreen->addItem(tr("Final (Composite)"), VDP2_SCREEN_FINAL);
    // Séparateur visuel : "Final" n'est pas une couche parmi d'autres, on le
    // distingue clairement des entrées NBG0...SPRITE qui suivent.
    cbScreen->insertSeparator(1);

    QtYabause::retranslateWidget(this);
}

UIDebugVDP2Viewer::~UIDebugVDP2Viewer()
{
    // vdp2texture est allouée via Vdp2DebugTexture() (malloc) et n'était
    // jamais libérée à la fermeture du viewer : fuite mémoire de la
    // dernière texture générée. On la libère ici explicitement.
    if (vdp2texture) {
        free(vdp2texture);
        vdp2texture = NULL;
    }
}

void UIDebugVDP2Viewer::displayCurrentScreen()
{
    if (!Vdp2Regs) {
        // Libérer la texture précédente avant de rendre le viewer vide
        if (vdp2texture) { free(vdp2texture); vdp2texture = NULL; }
        gvScreen->scene()->clear();
        pbSaveAsBitmap->setEnabled(false);
        return;
    }
    int idx = cbScreen->itemData(cbScreen->currentIndex()).toInt();
    if (vdp2texture) { free(vdp2texture); vdp2texture = NULL; }
    vdp2texture = Vdp2DebugTexture(idx, &width, &height);
    if (vdp2texture) {
        pbSaveAsBitmap->setEnabled(true);
        QGraphicsScene *sc=gvScreen->scene();
        /* Les extracteurs de vidcs.c remplissent le tampon via
         * glGetTexImage/glReadPixels avec GL_RGBA + GL_UNSIGNED_BYTE :
         * l'ordre en memoire est donc [R][G][B][A], quel que soit
         * l'endianness de la machine.
         *
         * Format_ARGB32 / Format_RGB32 sont des formats *entiers* : un pixel
         * y vaut 0xAARRGGBB, dont la disposition en octets depend de
         * l'endianness. En little-endian cela donne [B][G][R][A], soit R et B
         * inverses par rapport a ce que GL a ecrit -- ce que le .rgbSwapped()
         * plus bas venait rattraper. Les deux erreurs s'annulaient, mais
         * seulement sur little-endian : en big-endian la lecture donne
         * [A][R][G][B] et le rgbSwapped() aggrave le probleme au lieu de le
         * corriger.
         *
         * Format_RGBA8888 / Format_RGBX8888 sont definis par leur ordre
         * d'OCTETS ([R][G][B][A]) et correspondent donc directement a la
         * sortie de GL, sur toutes les plateformes. Le .rgbSwapped() devient
         * inutile et est retire. */
        QImage::Format fmt=cbOpaque->isChecked()?QImage::Format_RGBX8888:QImage::Format_RGBA8888;
        QImage img((uchar*)vdp2texture,width,height,fmt);
        // NOTE : idx != SPRITE couvre aussi VDP2_SCREEN_FINAL (donc miroir
        // vertical appliqué, comme pour NBG/RBG). C'est une hypothèse : le
        // cas Final lit via glReadPixels sur _Ygl->default_fbo (vidcs.c),
        // pas via glGetTexImage sur une texture NBG/RBG classique — même
        // convention "bas en haut" attendue côté OpenGL, mais pas vérifiée
        // sur une capture réelle. Si "Final" apparaît à l'envers à l'usage,
        // inverser explicitement pour VDP2_SCREEN_FINAL ici.
        QPixmap px=QPixmap::fromImage(img.mirrored(false,idx!=SPRITE));
        sc->clear(); sc->setBackgroundBrush(Qt::Dense7Pattern);
        sc->addPixmap(px); sc->setSceneRect(sc->itemsBoundingRect());
    } else {
        // La couche sélectionnée n'a pas produit de texture (désactivée,
        // pas encore de sélection valide pendant un rebuild, etc.) : on vide
        // la vue au lieu de laisser l'ancienne image affichée, ce qui serait
        // trompeur (le bouton "Save As Bitmap" est aussi désactivé, cf.
        // on_pbSaveAsBitmap_clicked qui refusait déjà silencieusement dans
        // ce cas — il ne pouvait juste jamais être désactivé auparavant).
        gvScreen->scene()->clear();
        pbSaveAsBitmap->setEnabled(false);
    }
}

// ============================================================
//  refreshActiveTab — met à jour les données de l'onglet actuellement
//  affiché. Point d'entrée commun à refresh() (pas de frame), showEvent()
//  (ré-ouverture du viewer) et on_tabWidget_currentChanged() (changement
//  manuel d'onglet), pour que les trois se comportent de la même façon.
//  Avant ce refactor, showEvent() appelait toujours updateVdp2Registers()
//  sans regarder l'onglet réellement actif : rouvrir le viewer sur
//  l'onglet Debug (ou Color RAM / VRAM Hex) affichait des données
//  potentiellement obsolètes tant qu'on ne changeait pas d'onglet.
// ============================================================
void UIDebugVDP2Viewer::refreshActiveTab()
{
    switch (tabWidget->currentIndex()) {
        case 1: updateVdp2Registers(); break;
        case 2: updateStats();         break;
        case 3: updateColorRam();      break;
        case 4: updateVramHex();       break;
        default: break;
    }
}

void UIDebugVDP2Viewer::refresh()
{
    displayCurrentScreen();
    refreshActiveTab();
}

void UIDebugVDP2Viewer::showEvent(QShowEvent *)
{
    gvScreen->fitInView(gvScreen->scene()->sceneRect());
    refreshActiveTab();
}

void UIDebugVDP2Viewer::on_tabWidget_currentChanged(int)
{
    refreshActiveTab();
}

void UIDebugVDP2Viewer::on_cbScreen_currentIndexChanged(int index)
{
    if (index < 0)
        return; // combo momentanément vide (ex: reconstruction de la liste par clearItems())
    displayCurrentScreen();
}
void UIDebugVDP2Viewer::on_cbOpaque_toggled(bool)             { displayCurrentScreen(); }
void UIDebugVDP2Viewer::on_cbVramBank_currentIndexChanged(int){ updateVramHex(); }
void UIDebugVDP2Viewer::on_pbVramGo_clicked()                 { updateVramHex(); }

// ============================================================
//  on_pbVramExport_clicked - enregistre la banque VRAM selectionnee dans
//  un fichier binaire brut.
//
//  L'onglet hexa n'affiche que 512 octets a la fois, ce qui suffit pour
//  jeter un oeil mais pas pour comparer une table de noms de motifs ou un
//  jeu de tuiles entier. Le fichier brut permet de traiter la banque avec
//  n'importe quel outil externe.
//
//  La base et la taille viennent de vramBankRange(), la meme fonction que
//  l'affichage hexa : les deux ne peuvent pas diverger.
// ============================================================
void UIDebugVDP2Viewer::on_pbVramExport_clicked()
{
    if (!Vdp2Ram) {
        CommonDialogs::error(QtYabause::translate("VDP2 RAM not available."));
        return;
    }

    u32 base=0, bSize=0;
    vramBankRange(&base, &bSize);

    /* Le tableau fait 512 Ko en 4 Mbit et 1 Mo en 8 Mbit ; on borne pour ne
     * jamais lire au-dela, y compris si VRSIZE a change entre-temps. */
    const u32 ramSize = (Vdp2Regs && ((Vdp2Regs->VRSIZE >> 15) & 1)) ? 0x100000u : 0x80000u;
    if (base >= ramSize) return;
    if (base + bSize > ramSize) bSize = ramSize - base;

    /* Nom par defaut portant la banque et son adresse : en comparant
     * plusieurs captures on retrouve tout de suite laquelle est laquelle. */
    const QString label = cbVramBank->currentText().section(' ', 0, 0);
    const QString suggested = QString("vdp2_vram_%1_0x%2_%3.bin")
        .arg(label)
        .arg(base, 5, 16, QChar('0'))
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

    QString path = CommonDialogs::getSaveFileName(suggested,
        QtYabause::translate("Choose a location for the VRAM dump"),
        QtYabause::translate("Binary files (*.bin)"));
    if (path.isEmpty())
        return;

    if (!path.endsWith(".bin", Qt::CaseInsensitive))
        path += ".bin";

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
        return;
    }
    const qint64 written = f.write(reinterpret_cast<const char *>(Vdp2Ram + base), bSize);
    f.close();
    if (written != (qint64)bSize)
        CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}
void UIDebugVDP2Viewer::on_cbCramHex_toggled(bool)            { updateColorRam(); }

void UIDebugVDP2Viewer::on_pbSaveAsBitmap_clicked()
{
    QStringList filters;
    int idx=cbScreen->itemData(cbScreen->currentIndex()).toInt();
    foreach(QByteArray ba, QImageWriter::supportedImageFormats()) {
        QString fmt = QString(ba).toLower();
        if (!filters.contains(fmt, Qt::CaseInsensitive))
            filters << QString("%1 Images (*.%2)").arg(fmt.toUpper()).arg(fmt);
    }
    if(!vdp2texture)return;
    /* Meme convention d'octets que displayCurrentScreen() : GL a ecrit
     * [R][G][B][A], donc format Qt a ordre d'octets explicite et pas de
     * rgbSwapped(). L'image enregistree etait jusqu'ici correcte par
     * compensation de deux erreurs, uniquement en little-endian. */
    QImage::Format fmt=cbOpaque->isChecked()?QImage::Format_RGBX8888:QImage::Format_RGBA8888;
    QImage img((uchar*)vdp2texture,width,height,fmt);
    img=img.mirrored(false,idx!=SPRITE);
    const QString s=CommonDialogs::getSaveFileName(QString(),QtYabause::translate("Choose a location for your bitmap"),filters.join(";;"));
    if(!s.isEmpty())if(!img.save(s))CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}


// ============================================================
//  Vertical cell scroll -- dump par ligne de champ
// ============================================================
// Pourquoi ce dump ne peut pas etre remplace par une lecture de la VRAM :
// les registres et la VRAM ne donnent que le DERNIER etat ecrit. Un jeu qui
// reecrit la table de vertical cell scroll en cours de trame -- typiquement
// un ecran splitte, ou chaque moitie a ses propres offsets verticaux -- ne
// laisse a l'instant de la pause que les valeurs de la derniere moitie
// balayee. La seule trace de ce qui s'est reellement applique ligne par
// ligne est cell_scroll_data[], rempli par Vdp2HBlankIN().
//
// Attention a l'unite : cell_scroll_data[] est indexe par ligne de CHAMP
// (0..VBlankLineCount-1), comme tous les instantanes pris en H-blank, alors
// que la geometrie de plan est lue en lignes d'AFFICHAGE. En double-density
// interlace les deux different d'un facteur 2 -- c'est exactement le genre
// de decalage que ce dump sert a mettre en evidence.
//
// On n'imprime que les lignes ou le contenu CHANGE, sous forme de plages :
// une table reecrite en milieu de trame produit alors deux plages, et la
// ligne de bascule saute aux yeux.
//
// Format des valeurs : format standard des tables de scroll (partie entiere
// bits 26-16, partie fractionnaire bits 15-8) ; le vertical cell scroll
// n'utilise pas la partie fractionnaire. Le brut est affiche a cote pour ne
// rien masquer si l'interpretation ci-dessus ne colle pas.
static std::string buildVCellScrollDump()
{
    char b[512];
    std::string o;

    if (!Vdp2Regs) return std::string("  (VDP2 non initialise)\n");
    const Vdp2 &r = *Vdp2Regs;

    // SCRCTL (ST-058-R2 3.20) : bit 0 = N0VCSC, bit 8 = N1VCSC.
    const int n0 = (r.SCRCTL & 0x0001) ? 1 : 0;
    const int n1 = (r.SCRCTL & 0x0100) ? 1 : 0;
    const u32 base = 0x05E00000u | ((r.VCSTA.all & 0x7FFFEu) << 1);

    snprintf(b, sizeof b, "  VCSTA = 0x%08X    NBG0 VCS = %s    NBG1 VCS = %s\n",
             (unsigned)base, n0 ? "ON" : "off", n1 ? "ON" : "off");
    o += b;

    /* Timings deduits des cycle patterns, pas de SCRCTL seul. Le creneau ou
     * tombe la commande VCS determine si la lecture arrive trop tard pour la
     * cellule visee : NBG0 est retarde des T3 et repete des T2, NBG1 retarde
     * des T4 (pas de repetition -- l'asymetrie est materielle). Les jeux qui
     * programment des patterns "illegaux" dependent de ce comportement. */
    {
        Vdp2VCellScrollTiming t;
        Vdp2VCellScrollTiming_Current(&t);
        snprintf(b, sizeof b,
                 "  Acces (cycle patterns) : pas=%d o    NBG0 offset=+%d o    NBG1 offset=+%d o\n"
                 "                           NBG0 delay=%s repeat=%s    NBG1 delay=%s\n",
                 t.inc, t.offset[0], t.offset[1],
                 t.delay[0] ? "OUI" : "non", t.repeat[0] ? "OUI" : "non",
                 t.delay[1] ? "OUI" : "non");
        o += b;
        if (n0 + n1 > 0 && t.inc != (n0 + n1) * 4)
            o += "  ATTENTION : SCRCTL active une couche dont aucune commande VCS\n"
                 "              n'est programmee dans les cycle patterns\n";
    }

    if (!n0 && !n1) {
        o += "  (vertical cell scroll desactive sur les deux couches)\n";
        return o;
    }

    // ST-058-R2 5.3 Fig 5.6 p.134 : une entree par colonne de cellule
    // AFFICHEE, dans l'ordre des cellules a partir du bord GAUCHE de l'ecran
    // (et non a partir de la position scrollee). Fig 5.8 p.136 : quand les
    // deux couches partagent la table, les entrees alternent, NBG0 en tete,
    // d'ou l'offset de +1 longword pour NBG1.
    // Colonnes affichees par HRESO (2.1) : 320/352/640/704 dots, 8 dots par
    // cellule, les quatre memes largeurs se repetant pour les modes moniteur
    // exclusif (HRESO 100-111).
    static const int colsForHreso[8] = { 40, 44, 80, 88, 40, 44, 80, 88 };
    const int cols = colsForHreso[r.TVMD & 0x7];
    const int nlay = n0 + n1;
    const int cap  = (int)(sizeof(cell_scroll_data[0].data)
                         / sizeof(cell_scroll_data[0].data[0]));

    int need = cols * nlay;
    snprintf(b, sizeof b, "  %d colonnes x %d couche(s) = %d entrees utiles"
                          " (capacite cell_scroll_data[].data = %d)\n",
             cols, nlay, need, cap);
    o += b;
    if (need > cap) {
        snprintf(b, sizeof b, "  ATTENTION : la table depasse la capacite du"
                              " tampon, %d entrees non capturees\n", need - cap);
        o += b;
        need = cap;
    }

    int lines = yabsys.VBlankLineCount;
    const int maxLines = (int)(sizeof(cell_scroll_data)
                             / sizeof(cell_scroll_data[0]));
    if (lines > maxLines) lines = maxLines;
    if (lines <= 0) { o += "  (aucune ligne capturee)\n"; return o; }

    // Rend une liste de valeurs par colonne, compressee par plages de valeurs
    // identiques consecutives -- sans perte, mais lisible quand les 40
    // colonnes portent le meme offset.
    auto renderLayer = [&](const char *name, int lane, int line) {
        std::string row;
        int c = 0;
        while (c < cols) {
            const int idx = c * nlay + lane;
            if (idx >= need) break;
            const u32 v = cell_scroll_data[line].data[idx];
            int run = 1;
            while (c + run < cols) {
                const int j = (c + run) * nlay + lane;
                if (j >= need || cell_scroll_data[line].data[j] != v) break;
                run++;
            }
            // partie entiere = bits 26-16, signee sur 11 bits
            int ip = (int)((v >> 16) & 0x7FF);
            if (ip & 0x400) ip -= 0x800;
            if (!row.empty()) row += "  |  ";
            if (run > 1) snprintf(b, sizeof b, "0x%08X(y=%d) x%d", (unsigned)v, ip, run);
            else         snprintf(b, sizeof b, "0x%08X(y=%d)",     (unsigned)v, ip);
            row += b;
            c += run;
        }
        snprintf(b, sizeof b, "      %s: %s\n", name, row.c_str());
        return std::string(b);
    };

    const size_t cmpBytes = (size_t)need * sizeof(cell_scroll_data[0].data[0]);
    int runs = 0;
    const int maxRuns = 64;
    int start = 0;

    for (int l = 1; l <= lines; l++) {
        const bool last = (l == lines);
        if (!last && memcmp(cell_scroll_data[l].data,
                            cell_scroll_data[start].data, cmpBytes) == 0)
            continue;

        if (runs == maxRuns) {
            snprintf(b, sizeof b, "  ... (plus de %d changements, sortie tronquee ;"
                                  " la table change quasiment a chaque ligne)\n", maxRuns);
            o += b;
            return o;
        }
        runs++;

        if (l - 1 == start) snprintf(b, sizeof b, "  ligne de champ %3d :\n", start);
        else                snprintf(b, sizeof b, "  lignes de champ %3d-%3d :\n", start, l - 1);
        o += b;
        if (n0) o += renderLayer("NBG0", 0, start);
        if (n1) o += renderLayer("NBG1", n0 ? 1 : 0, start);

        start = l;
    }

    snprintf(b, sizeof b, "  -> %d plage(s) distincte(s) sur %d lignes de champ\n",
             runs, lines);
    o += b;
    if (runs == 1)
        o += "  (table constante sur toute la trame : aucune reecriture en cours"
             " de balayage)\n";
    return o;
}


// ============================================================
//  Defilement horizontal -- dump par ligne de champ
// ============================================================
// Meme lecture que le dump de vertical cell scroll ci-dessus, sur l'autre
// axe. Trois sources se combinent pour donner le decalage horizontal d'une
// ligne, et elles ne sont pas capturees au meme endroit :
//
//   SCXIN/SCXDN : registres, capturés par ligne dans Vdp2Lines[] ;
//   table de line scroll : capturee par ligne dans line_scroll_data[] ;
//   zoom (LZMX)  : idem, quand LSS le prevoit.
//
// Le total est ce que le renderer appelle sx. Si les deux moities d'un ecran
// splitte doivent avoir des positions horizontales distinctes, la bascule
// doit apparaitre ici, a la meme ligne que celle du dump VCS.
//
// On n'imprime que les lignes ou le total CHANGE, sous forme de plages.
static std::string buildLineScrollDump()
{
    char b[512];
    std::string o;

    if (!Vdp2Regs) return std::string("  (VDP2 non initialise)\n");
    const Vdp2 &r = *Vdp2Regs;

    // SCRCTL (ST-058-R2 3.20) : bit sh+1 = LSCX, sh+2 = LSCY, sh+3 = LZMX,
    // bits sh+5..sh+4 = LSS. sh = 0 pour NBG0, 8 pour NBG1.
    const int sx0 = (r.SCRCTL & 0x0002) ? 1 : 0;
    const int sx1 = (r.SCRCTL & 0x0200) ? 1 : 0;

    snprintf(b, sizeof b,
             "  LSTA0 = 0x%08X   LSTA1 = 0x%08X\n"
             "  NBG0 LineScrollX = %s   NBG1 LineScrollX = %s\n",
             (unsigned)(0x05E00000u | ((r.LSTA0.all & 0x7FFFEu) << 1)),
             (unsigned)(0x05E00000u | ((r.LSTA1.all & 0x7FFFEu) << 1)),
             sx0 ? "ON" : "off", sx1 ? "ON" : "off");
    o += b;

    int lines = yabsys.VBlankLineCount;
    const int maxLines = (int)(sizeof(line_scroll_data)
                             / sizeof(line_scroll_data[0]));
    if (lines > maxLines) lines = maxLines;
    if (lines <= 0) { o += "  (aucune ligne capturee)\n"; return o; }

    // Partie entiere du scroll : SCXIN sur 11 bits (bits 10-0), et pour la
    // table de line scroll le meme format que partout ailleurs (26-16).
    auto reg = [](u16 v) { int x = v & 0x7FF; if (x & 0x400) x -= 0x800; return x; };
    auto tbl = [](u32 v) { int x = (int)((v >> 16) & 0x7FF); if (x & 0x400) x -= 0x800; return x; };

    struct Row { int n0r, n1r, n0t, n1t; };
    auto rowAt = [&](int l) {
        Row w;
        w.n0r = reg(Vdp2Lines[l].SCXIN0);
        w.n1r = reg(Vdp2Lines[l].SCXIN1);
        w.n0t = sx0 ? tbl(line_scroll_data[l].n0[0][0]) : 0;
        w.n1t = sx1 ? tbl(line_scroll_data[l].n1[0][0]) : 0;
        return w;
    };
    auto same = [](const Row &a, const Row &c) {
        return a.n0r == c.n0r && a.n1r == c.n1r && a.n0t == c.n0t && a.n1t == c.n1t;
    };

    int runs = 0, start = 0;
    const int maxRuns = 64;

    for (int l = 1; l <= lines; l++) {
        const bool last = (l == lines);
        if (!last && same(rowAt(l), rowAt(start))) continue;

        if (runs == maxRuns) {
            o += "  ... (plus de 64 changements, sortie tronquee)\n";
            return o;
        }
        runs++;

        const Row w = rowAt(start);
        if (l - 1 == start) snprintf(b, sizeof b, "  ligne de champ %3d :\n", start);
        else                snprintf(b, sizeof b, "  lignes de champ %3d-%3d :\n", start, l - 1);
        o += b;
        snprintf(b, sizeof b,
                 "      NBG0: SCXIN=%5d  table=%5d  total=%5d\n"
                 "      NBG1: SCXIN=%5d  table=%5d  total=%5d\n",
                 w.n0r, w.n0t, w.n0r + w.n0t,
                 w.n1r, w.n1t, w.n1r + w.n1t);
        o += b;

        start = l;
    }

    snprintf(b, sizeof b, "  -> %d plage(s) distincte(s) sur %d lignes de champ\n",
             runs, lines);
    o += b;
    if (runs == 1)
        o += "  (defilement horizontal constant sur toute la trame)\n";
    return o;
}


// ============================================================
//  Back screen et line color screen -- dump par ligne
// ============================================================
// BKTA bit 31 (BKCLMD, ST-058-R2 3.13) : 0 = couleur unique, 1 = une couleur
// PAR LIGNE lue dans une table. LCTA bit 31 fait de meme pour le line color
// screen (3.14). Ces deux tables sont voisines de la table de vertical cell
// scroll -- dans Sonic Jam, BKTA = VCSTA + 0x150 -- et elles souffrent du
// meme piege : le renderer qui les relit en fin de trame ne voit que le
// dernier etat ecrit.
//
// C'est ce qui distingue une bordure NOIRE d'une bordure BLEUE autour d'un
// ecran splitte : le jeu ecrit du noir dans les entrees hors cadre et la
// couleur du ciel ailleurs. Une couleur unique etalee sur toute la trame
// donne du bleu partout.
//
// Couleurs en 5:5:5 RGB (bit 15 ignore pour le back screen).
static std::string buildBackScreenDump()
{
    char b[256];
    std::string o;

    if (!Vdp2Regs) return std::string("  (VDP2 non initialise)\n");
    const Vdp2 &r = *Vdp2Regs;

    auto section = [&](const char *name, u32 hi, u32 lo, int perLineBit) {
        const u32 all  = ((u32)hi << 16) | (u32)lo;
        const u32 addr = (all & 0x7FFFEu) << 1;
        const int per  = (all >> perLineBit) & 1;
        snprintf(b, sizeof b, "  %s = 0x%08X   table = 0x%08X   mode = %s\n",
                 name, (unsigned)all, (unsigned)(0x05E00000u | addr),
                 per ? "une couleur PAR LIGNE" : "couleur unique");
        o += b;

        if (!per) {
            const u16 c = Vdp2RamReadWord(NULL, Vdp2Ram, addr);
            snprintf(b, sizeof b, "      couleur = 0x%04X  (R=%2d G=%2d B=%2d)\n",
                     c, c & 0x1F, (c >> 5) & 0x1F, (c >> 10) & 0x1F);
            o += b;
            return;
        }

        int lines = yabsys.VBlankLineCount;
        if (lines > VDP2_LINE_SNAPSHOT_MAX) lines = VDP2_LINE_SNAPSHOT_MAX;
        if (lines <= 0) { o += "      (aucune ligne)\n"; return; }

        // Plages de lignes consecutives de meme couleur.
        int runs = 0, start = 0;
        for (int l = 1; l <= lines; l++) {
            const bool last = (l == lines);
            const u16 cs = Vdp2RamReadWord(NULL, Vdp2Ram, addr + (u32)start * 2);
            if (!last && Vdp2RamReadWord(NULL, Vdp2Ram, addr + (u32)l * 2) == cs)
                continue;
            if (runs == 48) { o += "      ... (tronque)\n"; return; }
            runs++;
            if (l - 1 == start)
                snprintf(b, sizeof b, "      ligne %3d      : 0x%04X (R=%2d G=%2d B=%2d)\n",
                         start, cs, cs & 0x1F, (cs >> 5) & 0x1F, (cs >> 10) & 0x1F);
            else
                snprintf(b, sizeof b, "      lignes %3d-%3d : 0x%04X (R=%2d G=%2d B=%2d)\n",
                         start, l - 1, cs, cs & 0x1F, (cs >> 5) & 0x1F, (cs >> 10) & 0x1F);
            o += b;
            start = l;
        }
        snprintf(b, sizeof b, "      -> %d plage(s) sur %d lignes\n", runs, lines);
        o += b;
        if (runs == 1)
            o += "      ATTENTION : mode par ligne mais une seule couleur sur toute\n"
                 "                  la trame -- table lue trop tard ?\n";
    };

    section("BKTA", r.BKTAU, r.BKTAL, 31);
    o += "\n";
    section("LCTA", r.LCTA.part.U, r.LCTA.part.L, 31);
    return o;
}


// ============================================================
//  Color RAM -- entrees utiles
// ============================================================
// L'export ne donnait que "Color RAM Mode = 0", sans aucune valeur. Or
// l'entree 0 est celle que remontent tous les pixels dont le code de couleur
// utile est nul -- typiquement un polygone VDP1 ecrit avec CMDCOLR = 0xF000
// en sprite type 7 palette-only, ou 0xF000 & 0x1FF vaut 0. Sa couleur decide
// donc de ce que voit le joueur a la place, et il faut pouvoir la lire.
//
// Modes (RAMCTL bits 13-12, ST-058-R2 3.2) :
//   0 = RGB 5:5:5, 1024 mots, 2 banques
//   1 = RGB 5:5:5, 2048 mots, 1 banque
//   2 = RGB 8:8:8, 1024 longwords
static std::string buildColorRamDump()
{
    char b[256];
    std::string o;

    if (!Vdp2ColorRam || !Vdp2Regs) return std::string("  (Color RAM indisponible)\n");
    const int mode = (Vdp2Regs->RAMCTL >> 12) & 0x3;
    static const char *modeName[4] = {
        "RGB 5:5:5, 1024 mots, 2 banques",
        "RGB 5:5:5, 2048 mots, 1 banque",
        "RGB 8:8:8, 1024 longwords",
        "(reglage interdit)"
    };
    snprintf(b, sizeof b, "  Mode = %d  (%s)\n", mode, modeName[mode]);
    o += b;

    auto entry = [&](int idx, unsigned *raw, int *r, int *g, int *bl) {
        if (mode == 2) {
            const u32 addr = (u32)(idx * 4) & 0xFFF;
            const u16 msw = T2ReadWord(Vdp2ColorRam, addr);
            const u16 lsw = T2ReadWord(Vdp2ColorRam, addr + 2);
            *raw = ((unsigned)msw << 16) | lsw;
            *r = msw & 0xFF; *g = (lsw >> 8) & 0xFF; *bl = lsw & 0xFF;
        } else {
            const u16 w = T2ReadWord(Vdp2ColorRam, (u32)(idx * 2) & 0xFFF);
            *raw = w;
            *r = w & 0x1F; *g = (w >> 5) & 0x1F; *bl = (w >> 10) & 0x1F;
        }
    };

    /* L'entree 0 en premier et isolee : c'est la plus souvent en cause. */
    {
        unsigned raw; int r, g, bl;
        entry(0, &raw, &r, &g, &bl);
        snprintf(b, sizeof b,
                 "  Entree 0 = 0x%0*X   R=%3d G=%3d B=%3d   %s\n",
                 mode == 2 ? 8 : 4, raw, r, g, bl,
                 (r == 0 && g == 0 && bl == 0) ? "(noir)" : "(NON NOIRE)");
        o += b;
    }

    o += "  Les 32 premieres entrees :\n";
    for (int row = 0; row < 4; row++) {
        std::string line = "   ";
        for (int c = 0; c < 8; c++) {
            unsigned raw; int r, g, bl;
            entry(row * 8 + c, &raw, &r, &g, &bl);
            snprintf(b, sizeof b, " %0*X", mode == 2 ? 8 : 4, raw);
            line += b;
        }
        snprintf(b, sizeof b, "  %3d-%3d :%s\n", row * 8, row * 8 + 7, line.c_str());
        o += b;
    }

    /* Offsets par couche : une couche peut viser une autre zone de la CRAM. */
    snprintf(b, sizeof b,
             "  CRAOFA = 0x%04X  (NBG0=0x%03X NBG1=0x%03X NBG2=0x%03X NBG3=0x%03X)\n"
             "  CRAOFB = 0x%04X  (RBG0=0x%03X SPR=0x%03X RBG1=0x%03X)\n",
             Vdp2Regs->CRAOFA,
             (Vdp2Regs->CRAOFA & 7) << 8, ((Vdp2Regs->CRAOFA >> 4) & 7) << 8,
             ((Vdp2Regs->CRAOFA >> 8) & 7) << 8, ((Vdp2Regs->CRAOFA >> 12) & 7) << 8,
             Vdp2Regs->CRAOFB,
             (Vdp2Regs->CRAOFB & 7) << 8, ((Vdp2Regs->CRAOFB >> 4) & 7) << 8,
             ((Vdp2Regs->CRAOFB >> 8) & 7) << 8);
    o += b;
    return o;
}

// ============================================================
//  on_pbExportDebugInfo_clicked
//  Sauvegarde le contenu texte des onglets "Registers" (raw + decoded)
//  et "Debug" dans un unique fichier .txt choisi par l'utilisateur.
// ============================================================
void UIDebugVDP2Viewer::on_pbExportDebugInfo_clicked()
{
    // On rafraîchit les deux onglets avant export pour être sûr d'écrire
    // l'état courant, même si l'utilisateur n'a pas visité ces onglets
    // depuis le dernier "Next Frame".
    updateVdp2Registers();
    updateStats();

    const QString suggested = QString("vdp2_registers_debug_%1.txt")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

    QString s = CommonDialogs::getSaveFileName(suggested,
        QtYabause::translate("Choose a location for the text file"),
        QtYabause::translate("Text files (*.txt)"));
    if (s.isEmpty())
        return;

    // Certains dialogues natifs n'ajoutent pas automatiquement l'extension
    // du filtre sélectionné : on la garantit nous-mêmes plutôt que de
    // dépendre de ce comportement, qui varie selon la plateforme.
    if (!s.endsWith(".txt", Qt::CaseInsensitive))
        s += ".txt";

    QFile f(s);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
        return;
    }

    QTextStream ts(&f);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    ts.setCodec("UTF-8");
#endif

    ts << "Yabause VDP2 Debug Export\n";
    ts << "Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << "==================================================\n\n";

    ts << "########## REGISTERS - RAW VALUES ##########\n\n";
    ts << pteRawRegs->toPlainText() << "\n\n";

    ts << "########## REGISTERS - DECODED ##########\n\n";
    ts << pteDecodedRegs->toPlainText() << "\n\n";

    ts << "########## VERTICAL CELL SCROLL (par ligne de champ) ##########\n\n";
    ts << QString::fromStdString(buildVCellScrollDump()) << "\n\n";

    ts << "########## DEFILEMENT HORIZONTAL (par ligne de champ) ##########\n\n";
    ts << QString::fromStdString(buildLineScrollDump()) << "\n\n";

    ts << "########## BACK SCREEN / LINE COLOR (par ligne) ##########\n\n";
    ts << QString::fromStdString(buildBackScreenDump()) << "\n\n";

    ts << "########## COLOR RAM ##########\n\n";
    ts << QString::fromStdString(buildColorRamDump()) << "\n\n";

    ts << "########## DEBUG ##########\n\n";
    ts << pteStats->toPlainText() << "\n";

    ts.flush();
    f.close();

    if (f.error() != QFile::NoError)
        CommonDialogs::error(QtYabause::translate("An error occured while writing file."));
}
