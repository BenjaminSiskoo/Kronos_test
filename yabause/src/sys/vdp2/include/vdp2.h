/*  Copyright 2003-2005 Guillaume Duhamel
    Copyright 2004-2006 Theo Berkau

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

#ifndef VDP2_H
#define VDP2_H

#include "memory.h"
/* This include is not *needed*, it's here to avoid breaking ports */
#include "osdcore.h"

#ifdef __cplusplus
extern "C" {
#endif

extern u8 * Vdp2Ram;
extern u8 * Vdp2ColorRam;
extern u8 Vdp2ColorRamUpdated[];
extern u8 Vdp2ColorRamToSync[];
/* Kronos#520 (True Pinball flipper table): a game can free a VRAM bank for
 * CPU/SCU writes and hand it back to the VDP2 for display, all within a
 * single active frame, purely by rewriting the cycle pattern registers
 * (VDP2 manual ST-58-R2 p.36 "VRAM access by the CPU...", Table 3.5 p.40).
 * When that happens, the bytes NBG0/1/2/3 actually read for a given
 * display line are whatever the CPU had written by that point in *real
 * time*, not whatever is left in VRAM once the whole frame has finished
 * (which is when composeFB actually runs). vdp2RamAccessCPUCheck() already
 * tracks, live and per bank, whether the VDP2 currently owns a bank
 * (Vdp2External.vdp2_blocked[]); the moment a bank flips back from
 * CPU-owned to VDP2-owned we freeze its content here so the deferred
 * render can use the historically correct bytes instead of the live
 * end-of-frame buffer. Every game that never does this mid-frame handoff
 * pays nothing: Vdp2VramSnapshotCount[][bank] simply stays 0 and callers
 * keep reading Vdp2Ram directly. */
#define VDP2_VRAM_BANK_SIZE     0x20000  /* physical size of VRAM-A0/A1/B0/B1 */
#define VDP2_MAX_VRAM_SNAPSHOTS 40       /* generous: far more handoffs per frame
                                            than any known game needs */

typedef struct {
   int line;                          /* yabsys.LineCount at capture time */
   u8  data[VDP2_VRAM_BANK_SIZE];
} Vdp2VramBankSnapshot;

/* Slot 'Vdp2VramCaptureSlot' is being written to in real time by this
 * frame's Vdp2HBlankIN/vdp2RamAccessCPUCheck calls. Slot
 * '1 - Vdp2VramCaptureSlot' holds the *previous* frame's finished
 * captures, exactly what composeFB and its queued async NBG0 cell jobs
 * should be reading for as long as they're still in flight (the
 * compute-shader renderer queues NBG0 cell decoding onto a background
 * thread by default - CELL_ASYNC / YAB_WANT_ASYNC_CELL - so a frame's
 * captures must outlive that frame's own composeFB call). */
extern Vdp2VramBankSnapshot Vdp2VramSnapshots[2][4][VDP2_MAX_VRAM_SNAPSHOTS];
extern int Vdp2VramSnapshotCount[2][4];
extern int Vdp2VramCaptureSlot;

/* Returns a pointer to the VDP2_VRAM_BANK_SIZE bytes 'bank' (0=A0, 1=A1,
 * 2=B0, 3=B1) actually held at display line 'atLine' during the frame
 * currently being composed, or NULL if that bank was never handed back
 * and forth this frame (the common case - caller should keep using the
 * live Vdp2Ram in that case). */
const u8 * Vdp2GetVramBankSnapshot(int bank, int atLine);

/* Must be called exactly once per frame, after this frame's previously-
 * pending async NBG0 cell jobs are confirmed drained (i.e. right after
 * WaitVdp2Async()) and before composeFB starts consuming/queuing new
 * ones. Flips which slot is being captured into and clears the new
 * capture slot's counts, without ever touching the slot composeFB (and
 * its async jobs) are about to read from. */
void Vdp2VramSnapshotSwap(void);

u8 FASTCALL     Vdp2RamReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL    Vdp2RamReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL    Vdp2RamReadLong(SH2_struct *context, u8*, u32);
void FASTCALL   Vdp2RamWriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL   Vdp2RamWriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL   Vdp2RamWriteLong(SH2_struct *context, u8*, u32, u32);

u8 FASTCALL     Vdp2ColorRamReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL    Vdp2ColorRamReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL    Vdp2ColorRamReadLong(SH2_struct *context, u8*, u32);
void FASTCALL   Vdp2ColorRamWriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL   Vdp2ColorRamWriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL   Vdp2ColorRamWriteLong(SH2_struct *context, u8*, u32, u32);

typedef struct {
   u16 TVMD;   // 0x25F80000
   u16 EXTEN;  // 0x25F80002
   u16 TVSTAT; // 0x25F80004
   u16 VRSIZE; // 0x25F80006
   u16 HCNT;   // 0x25F80008
   u16 VCNT;   // 0x25F8000A
   u16 EWDR;   // 0x25F8000C — External Write Data Register (ST-013-R3 §3.4, write-only)
   u16 RAMCTL; // 0x25F8000E
   u16 CYCA0L; // 0x25F80010
   u16 CYCA0U; // 0x25F80012
   u16 CYCA1L; // 0x25F80014
   u16 CYCA1U; // 0x25F80016
   u16 CYCB0L; // 0x25F80018
   u16 CYCB0U; // 0x25F8001A
   u16 CYCB1L; // 0x25F8001C
   u16 CYCB1U; // 0x25F8001E
   u16 BGON;   // 0x25F80020
   u16 MZCTL;  // 0x25F80022
   u16 SFSEL;  // 0x25F80024
   u16 SFCODE; // 0x25F80026
   u16 CHCTLA; // 0x25F80028
   u16 CHCTLB; // 0x25F8002A
   u16 BMPNA;  // 0x25F8002C
   u16 BMPNB;  // 0x25F8002E
   u16 PNCN0;  // 0x25F80030
   u16 PNCN1;  // 0x25F80032
   u16 PNCN2;  // 0x25F80034
   u16 PNCN3;  // 0x25F80036
   u16 PNCR;   // 0x25F80038
   u16 PLSZ;   // 0x25F8003A
   u16 MPOFN;  // 0x25F8003C
   u16 MPOFR;  // 0x25F8003E
   u16 MPABN0; // 0x25F80040
   u16 MPCDN0; // 0x25F80042
   u16 MPABN1; // 0x25F80044
   u16 MPCDN1; // 0x25F80046
   u16 MPABN2; // 0x25F80048
   u16 MPCDN2; // 0x25F8004A
   u16 MPABN3; // 0x25F8004C
   u16 MPCDN3; // 0x25F8004E
   u16 MPABRA; // 0x25F80050
   u16 MPCDRA; // 0x25F80052
   u16 MPEFRA; // 0x25F80054
   u16 MPGHRA; // 0x25F80056
   u16 MPIJRA; // 0x25F80058
   u16 MPKLRA; // 0x25F8005A
   u16 MPMNRA; // 0x25F8005C
   u16 MPOPRA; // 0x25F8005E
   u16 MPABRB; // 0x25F80060
   u16 MPCDRB; // 0x25F80062
   u16 MPEFRB; // 0x25F80064
   u16 MPGHRB; // 0x25F80066
   u16 MPIJRB; // 0x25F80068
   u16 MPKLRB; // 0x25F8006A
   u16 MPMNRB; // 0x25F8006C
   u16 MPOPRB; // 0x25F8006E
   u16 SCXIN0; // 0x25F80070
   u16 SCXDN0; // 0x25F80072
   u16 SCYIN0; // 0x25F80074
   u16 SCYDN0; // 0x25F80076

#ifdef WORDS_BIGENDIAN
  union {
    struct {
      u32 I:16; // 0x25F80078
      u32 D:16; // 0x25F8007A
    } part;
    u32 all;
  } ZMXN0;

  union {
    struct {
      u32 I:16; // 0x25F8007C
      u32 D:16; // 0x25F8007E
    } part;
    u32 all;
  } ZMYN0;
#else
  union {
    struct {
      u32 D:16; // 0x25F8007A
      u32 I:16; // 0x25F80078
    } part;
    u32 all;
  } ZMXN0;

  union {
    struct {
      u32 D:16; // 0x25F8007E
      u32 I:16; // 0x25F8007C
    } part;
    u32 all;
  } ZMYN0;
#endif

   u16 SCXIN1; // 0x25F80080
   u16 SCXDN1; // 0x25F80082
   u16 SCYIN1; // 0x25F80084
   u16 SCYDN1; // 0x25F80086

#ifdef WORDS_BIGENDIAN
  union {
    struct {
      u32 I:16; // 0x25F80088
      u32 D:16; // 0x25F8008A
    } part;
    u32 all;
  } ZMXN1;

  union {
    struct {
      u32 I:16; // 0x25F8008C
      u32 D:16; // 0x25F8008E
    } part;
    u32 all;
  } ZMYN1;
#else
  union {
    struct {
      u32 D:16; // 0x25F8008A
      u32 I:16; // 0x25F80088
    } part;
    u32 all;
  } ZMXN1;

  union {
    struct {
      u32 D:16; // 0x25F8008E
      u32 I:16; // 0x25F8008C
    } part;
    u32 all;
  } ZMYN1;
#endif

   u16 SCXN2;  // 0x25F80090
   u16 SCYN2;  // 0x25F80092
   u16 SCXN3;  // 0x25F80094
   u16 SCYN3;  // 0x25F80096
   u16 ZMCTL;  // 0x25F80098
   u16 SCRCTL; // 0x25F8009A
#ifdef WORDS_BIGENDIAN
   union {
      struct {
         u32 U:16; // 0x25F8009C
         u32 L:16; // 0x25F8009E
      } part;
      u32 all;
   } VCSTA;

   union {
      struct {
         u32 U:16; // 0x25F800A0
         u32 L:16; // 0x25F800A2
      } part;
      u32 all;
   } LSTA0;

   union {
      struct {
         u32 U:16; // 0x25F800A4
         u32 L:16; // 0x25F800A6
      } part;
      u32 all;
   } LSTA1;

   union {
      struct {
         u32 U:16; // 0x25F800A8
         u32 L:16; // 0x25F800AA
      } part;
      u32 all;
   } LCTA;
#else
   union {
      struct {
         u32 L:16; // 0x25F8009E
         u32 U:16; // 0x25F8009C
      } part;
      u32 all;
   } VCSTA;

   union {
      struct {
         u32 L:16; // 0x25F800A2
         u32 U:16; // 0x25F800A0
      } part;
      u32 all;
   } LSTA0;

   union {
      struct {
         u32 L:16; // 0x25F800A6
         u32 U:16; // 0x25F800A4
      } part;
      u32 all;
   } LSTA1;

   union {
      struct {
         u32 L:16; // 0x25F800AA
         u32 U:16; // 0x25F800A8
      } part;
      u32 all;
   } LCTA;
#endif

   u16 BKTAU;  // 0x25F800AC
   u16 BKTAL;  // 0x25F800AE
   u16 RPMD;   // 0x25F800B0
   u16 RPRCTL; // 0x25F800B2
   u16 KTCTL;  // 0x25F800B4
   u16 KTAOF;  // 0x25F800B6
   u16 OVPNRA; // 0x25F800B8
   u16 OVPNRB; // 0x25F800BA
#ifdef WORDS_BIGENDIAN
   union {
      struct {
         u32 U:16; // 0x25F800BC
         u32 L:16; // 0x25F800BE
      } part;
      u32 all;
   } RPTA;
#else
   union {
      struct {
         u32 L:16; // 0x25F800BE
         u32 U:16; // 0x25F800BC
      } part;
      u32 all;
   } RPTA;
#endif
   u16 WPSX0;  // 0x25F800C0
   u16 WPSY0;  // 0x25F800C2
   u16 WPEX0;  // 0x25F800C4
   u16 WPEY0;  // 0x25F800C6
   u16 WPSX1;  // 0x25F800C8
   u16 WPSY1;  // 0x25F800CA
   u16 WPEX1;  // 0x25F800CC
   u16 WPEY1;  // 0x25F800CE
   u16 WCTLA;  // 0x25F800D0
   u16 WCTLB;  // 0x25F800D2
   u16 WCTLC;  // 0x25F800D4
   u16 WCTLD;  // 0x25F800D6
#ifdef WORDS_BIGENDIAN
  union {
    struct {
      u32 U:16; // 0x25F800D8
      u32 L:16; // 0x25F800DA
    } part;
    u32 all;
  } LWTA0;

  union {
    struct {
      u32 U:16; // 0x25F800DC
      u32 L:16; // 0x25F800DE
    } part;
    u32 all;
  } LWTA1;
#else
  union {
    struct {
      u32 L:16; // 0x25F800D8
      u32 U:16; // 0x25F800DA
    } part;
    u32 all;
  } LWTA0;

  union {
    struct {
      u32 L:16; // 0x25F800DC
      u32 U:16; // 0x25F800DE
    } part;
    u32 all;
  } LWTA1;
#endif

   u16 SPCTL;  // 0x25F800E0
   u16 SDCTL;  // 0x25F800E2
   u16 CRAOFA; // 0x25F800E4
   u16 CRAOFB; // 0x25F800E6
   u16 LNCLEN; // 0x25F800E8
   u16 SFPRMD; // 0x25F800EA
   u16 CCCTL;  // 0x25F800EC
   u16 SFCCMD; // 0x25F800EE
   u16 PRISA;  // 0x25F800F0
   u16 PRISB;  // 0x25F800F2
   u16 PRISC;  // 0x25F800F4
   u16 PRISD;  // 0x25F800F6
   u16 PRINA;  // 0x25F800F8
   u16 PRINB;  // 0x25F800FA
   u16 PRIR;   // 0x25F800FC
   u16 pad;    // 0x25F800FE — reserved (ST-013-R3 §3.29, must not be accessed)
   u16 CCRSA;  // 0x25F80100
   u16 CCRSB;  // 0x25F80102
   u16 CCRSC;  // 0x25F80104
   u16 CCRSD;  // 0x25F80106
   u16 CCRNA;  // 0x25F80108
   u16 CCRNB;  // 0x25F8010A
   u16 CCRR;   // 0x25F8010C
   u16 CCRLB;  // 0x25F8010E
   u16 CLOFEN; // 0x25F80110
   u16 CLOFSL; // 0x25F80112
   u16 COAR;   // 0x25F80114
   u16 COAG;   // 0x25F80116
   u16 COAB;   // 0x25F80118
   u16 COBR;   // 0x25F8011A
   u16 COBG;   // 0x25F8011C
   u16 COBB;   // 0x25F8011E
} Vdp2;

extern Vdp2 * Vdp2Regs;

typedef struct {
   int ColorMode;
} Vdp2Internal_struct;

extern Vdp2Internal_struct Vdp2Internal;
extern int vdp2_is_odd_frame;

/* Profondeur des instantanes pris a chaque H-blank. Elle borne a la fois
 * Vdp2Lines[] et cell_scroll_data[], et sert de plafond a VBlankLineCount
 * (cf. Vdp2VBlankLine() dans vdp2.c) : Vdp2HBlankIN() indexe les deux
 * tableaux par LineCount pour toute ligne sous VBlankLineCount, sans borne
 * propre. Le renderer repete deja cette valeur dans une dizaine de clamps
 * "line_max" ; la nommer ici evite qu'un seul site diverge de la
 * declaration. */
#define VDP2_LINE_SNAPSHOT_MAX 270

extern Vdp2 Vdp2Lines[VDP2_LINE_SNAPSHOT_MAX];

struct CellScrollData
{
   /* Une entree par colonne de cellule AFFICHEE, dans l'ordre des cellules
    * depuis le bord gauche de l'ecran (ST-058-R2 §5.3 p.134, Fig 5.6). Le
    * facteur 2 n'est pas un nombre de banques VRAM mais le nombre de
    * COUCHES partageant la table : quand NBG0 et NBG1 font tous deux du
    * vertical cell scroll, leurs entrees alternent, NBG0 en tete, d'ou
    * l'offset de +1 longword pour NBG1 (Fig 5.8 p.136).
    *
    * 88 = 44 colonnes (352 px / 8) x 2 couches : suffisant jusqu'en 352 px
    * avec les deux couches actives. En 640/704 il en faudrait 160/176 --
    * limite connue, signalee par Vdp2VCellScrollLongwordsFor() qui borne
    * son resultat a cette capacite. */
   u32 data[88];
};

extern struct CellScrollData cell_scroll_data[VDP2_LINE_SNAPSHOT_MAX];

/* Instantane par ligne des tables de line scroll NBG0/NBG1. Meme raison
 * d'etre que cell_scroll_data[] : les jeux reecrivent ces tables en cours de
 * trame, alors que le renderer dessine en fin de trame et n'y verrait sinon
 * que le dernier etat ecrit.
 *
 * Une seule entree par ligne, celle que le materiel utilise a cette ligne --
 * inutile de recopier les 224 entrees a chaque ligne. Les deux sous-slots
 * couvrent LSS=0 (une entree par ligne d'affichage) en double-density, ou
 * une ligne de champ porte deux lignes d'affichage aux entrees distinctes.
 *
 * Les trois longwords sont SX, SY puis SZ, tasses : seuls les champs actifs
 * dans SCRCTL sont presents, dans cet ordre (ST-058-R2 5.2 Fig 5.3 p.130).
 * Les emplacements inutilises valent 0. */
struct LineScrollData
{
   u32 n0[2][3];
   u32 n1[2][3];
};

extern struct LineScrollData line_scroll_data[VDP2_LINE_SNAPSHOT_MAX];

// struct for Vdp2 part that shouldn't be saved
typedef struct {
   int disptoggle;
   int cpu_cycle_a;
   int cpu_cycle_b;
   u8 AC_VRAM[4][8];
   u8 vdp2_blocked[4];
} Vdp2External_struct;

extern Vdp2External_struct Vdp2External;

int Vdp2Init(void);
void Vdp2DeInit(void);
void Vdp2Reset(void);
void Vdp2VBlankIN(void);
void Vdp2VBlankIN_It(void);
void Vdp2HBlankIN(void);
void Vdp2HBlankIN_It(void);
void Vdp2StartVisibleLine(void);
void Vdp2VBlankOUT(void);
void Vdp2VBlankOUT_It(void);
void Vdp2SendExternalLatch(int valid, int hcnt, int vcnt);
void SpeedThrottleEnable(void);
void SpeedThrottleDisable(void);

u8 Vdp2RamIsUpdated(void);

u8 FASTCALL     Vdp2ReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL    Vdp2ReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL    Vdp2ReadLong(SH2_struct *context, u8*, u32);
void FASTCALL   Vdp2WriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL   Vdp2WriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL   Vdp2WriteLong(SH2_struct *context, u8*, u32, u32);

int Vdp2SaveState(void ** stream);
int Vdp2LoadState(const void * stream, int version, int size);

void ToggleNBG0(void);
void ToggleNBG1(void);
void ToggleNBG2(void);
void ToggleNBG3(void);
void ToggleRBG0(void);
void ToggleRBG1(void);
void ToggleFullScreen(void);

Vdp2 * Vdp2RestoreRegs(int line, Vdp2* lines);

/* Nombre de longwords reellement occupes par la table de vertical cell
 * scroll dans la configuration courante (colonnes affichees x couches avec
 * VCSC actif), borne par la capacite de cell_scroll_data[].data.
 * Partage avec le renderer pour que la capture (Vdp2HBlankIN) et la lecture
 * (Vdp2DrawMapPerLine) utilisent exactement la meme borne. */
int Vdp2VCellScrollLongwords(void);

/* Meme calcul, sur un jeu de registres donne. Le renderer dessine en fin de
 * trame depuis un instantane de zone (Vdp2Lines[]) et doit se borner avec
 * les valeurs en vigueur au moment de la capture, pas avec les registres
 * vivants, qui ont pu changer depuis. Vdp2VCellScrollLongwords() n'est que
 * le wrapper sur Vdp2Regs utilise par la capture au H-blank. */
int Vdp2VCellScrollLongwordsFor(Vdp2 *regs);

/* Timings d'acces a la table de vertical cell scroll, deduits des cycle
 * patterns (CYCxnL/U) et non de SCRCTL seul.
 *
 *   inc      : pas entre deux entrees consecutives, en octets (4 ou 8) --
 *              le nombre de commandes VCS reellement programmees ;
 *   offset[] : position de l'entree de NBG0 / NBG1 dans le groupe, en octets.
 *              NBG1 est a +4 uniquement parce que sa commande vient d'ordinaire
 *              apres celle de NBG0 ; l'ordre des creneaux fait foi ;
 *   delay[]  : la lecture tombe trop tard dans la ligne, la valeur s'applique
 *              une cellule plus loin (NBG0 des T3, NBG1 des T4) ;
 *   repeat[] : la valeur de la cellule precedente est reutilisee (NBG0 des T2).
 *              Il n'y a pas d'equivalent pour NBG1 : l'asymetrie est materielle.
 *
 * Modele repris de Ymir (vdp_state.hpp, CalcVCellScrollDelay), etabli par test
 * sur machine reelle. Certains jeux programment des patterns "illegaux" qu'il
 * faut honorer. */
typedef struct {
   int inc;
   int offset[2];
   int delay[2];
   int repeat[2];
} Vdp2VCellScrollTiming;

void Vdp2VCellScrollTimingFor(Vdp2 *regs, Vdp2VCellScrollTiming *out);
void Vdp2VCellScrollTiming_Current(Vdp2VCellScrollTiming *out);

#include "threads.h"

int VideoSetFilterType( int video_filter_type );
void vdp2ReqDump();
void vdp2ReqRestore();

#ifdef __cplusplus
}
#endif

#endif/*  Copyright 2003-2005 Guillaume Duhamel
    Copyright 2004-2006 Theo Berkau

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

#ifndef VDP2_H
#define VDP2_H

#include "memory.h"
/* This include is not *needed*, it's here to avoid breaking ports */
#include "osdcore.h"

#ifdef __cplusplus
extern "C" {
#endif

extern u8 * Vdp2Ram;
extern u8 * Vdp2ColorRam;
extern u8 Vdp2ColorRamUpdated[];
extern u8 Vdp2ColorRamToSync[];
/* Kronos#520 (True Pinball flipper table): a game can free a VRAM bank for
 * CPU/SCU writes and hand it back to the VDP2 for display, all within a
 * single active frame, purely by rewriting the cycle pattern registers
 * (VDP2 manual ST-58-R2 p.36 "VRAM access by the CPU...", Table 3.5 p.40).
 * When that happens, the bytes NBG0/1/2/3 actually read for a given
 * display line are whatever the CPU had written by that point in *real
 * time*, not whatever is left in VRAM once the whole frame has finished
 * (which is when composeFB actually runs). vdp2RamAccessCPUCheck() already
 * tracks, live and per bank, whether the VDP2 currently owns a bank
 * (Vdp2External.vdp2_blocked[]); the moment a bank flips back from
 * CPU-owned to VDP2-owned we freeze its content here so the deferred
 * render can use the historically correct bytes instead of the live
 * end-of-frame buffer. Every game that never does this mid-frame handoff
 * pays nothing: Vdp2VramSnapshotCount[][bank] simply stays 0 and callers
 * keep reading Vdp2Ram directly. */
#define VDP2_VRAM_BANK_SIZE     0x20000  /* physical size of VRAM-A0/A1/B0/B1 */
#define VDP2_MAX_VRAM_SNAPSHOTS 40       /* generous: far more handoffs per frame
                                            than any known game needs */

typedef struct {
   int line;                          /* yabsys.LineCount at capture time */
   u8  data[VDP2_VRAM_BANK_SIZE];
} Vdp2VramBankSnapshot;

/* Slot 'Vdp2VramCaptureSlot' is being written to in real time by this
 * frame's Vdp2HBlankIN/vdp2RamAccessCPUCheck calls. Slot
 * '1 - Vdp2VramCaptureSlot' holds the *previous* frame's finished
 * captures, exactly what composeFB and its queued async NBG0 cell jobs
 * should be reading for as long as they're still in flight (the
 * compute-shader renderer queues NBG0 cell decoding onto a background
 * thread by default - CELL_ASYNC / YAB_WANT_ASYNC_CELL - so a frame's
 * captures must outlive that frame's own composeFB call). */
extern Vdp2VramBankSnapshot Vdp2VramSnapshots[2][4][VDP2_MAX_VRAM_SNAPSHOTS];
extern int Vdp2VramSnapshotCount[2][4];
extern int Vdp2VramCaptureSlot;

/* Returns a pointer to the VDP2_VRAM_BANK_SIZE bytes 'bank' (0=A0, 1=A1,
 * 2=B0, 3=B1) actually held at display line 'atLine' during the frame
 * currently being composed, or NULL if that bank was never handed back
 * and forth this frame (the common case - caller should keep using the
 * live Vdp2Ram in that case). */
const u8 * Vdp2GetVramBankSnapshot(int bank, int atLine);

/* Must be called exactly once per frame, after this frame's previously-
 * pending async NBG0 cell jobs are confirmed drained (i.e. right after
 * WaitVdp2Async()) and before composeFB starts consuming/queuing new
 * ones. Flips which slot is being captured into and clears the new
 * capture slot's counts, without ever touching the slot composeFB (and
 * its async jobs) are about to read from. */
void Vdp2VramSnapshotSwap(void);

u8 FASTCALL     Vdp2RamReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL    Vdp2RamReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL    Vdp2RamReadLong(SH2_struct *context, u8*, u32);
void FASTCALL   Vdp2RamWriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL   Vdp2RamWriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL   Vdp2RamWriteLong(SH2_struct *context, u8*, u32, u32);

u8 FASTCALL     Vdp2ColorRamReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL    Vdp2ColorRamReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL    Vdp2ColorRamReadLong(SH2_struct *context, u8*, u32);
void FASTCALL   Vdp2ColorRamWriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL   Vdp2ColorRamWriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL   Vdp2ColorRamWriteLong(SH2_struct *context, u8*, u32, u32);

typedef struct {
   u16 TVMD;   // 0x25F80000
   u16 EXTEN;  // 0x25F80002
   u16 TVSTAT; // 0x25F80004
   u16 VRSIZE; // 0x25F80006
   u16 HCNT;   // 0x25F80008
   u16 VCNT;   // 0x25F8000A
   u16 EWDR;   // 0x25F8000C — External Write Data Register (ST-013-R3 §3.4, write-only)
   u16 RAMCTL; // 0x25F8000E
   u16 CYCA0L; // 0x25F80010
   u16 CYCA0U; // 0x25F80012
   u16 CYCA1L; // 0x25F80014
   u16 CYCA1U; // 0x25F80016
   u16 CYCB0L; // 0x25F80018
   u16 CYCB0U; // 0x25F8001A
   u16 CYCB1L; // 0x25F8001C
   u16 CYCB1U; // 0x25F8001E
   u16 BGON;   // 0x25F80020
   u16 MZCTL;  // 0x25F80022
   u16 SFSEL;  // 0x25F80024
   u16 SFCODE; // 0x25F80026
   u16 CHCTLA; // 0x25F80028
   u16 CHCTLB; // 0x25F8002A
   u16 BMPNA;  // 0x25F8002C
   u16 BMPNB;  // 0x25F8002E
   u16 PNCN0;  // 0x25F80030
   u16 PNCN1;  // 0x25F80032
   u16 PNCN2;  // 0x25F80034
   u16 PNCN3;  // 0x25F80036
   u16 PNCR;   // 0x25F80038
   u16 PLSZ;   // 0x25F8003A
   u16 MPOFN;  // 0x25F8003C
   u16 MPOFR;  // 0x25F8003E
   u16 MPABN0; // 0x25F80040
   u16 MPCDN0; // 0x25F80042
   u16 MPABN1; // 0x25F80044
   u16 MPCDN1; // 0x25F80046
   u16 MPABN2; // 0x25F80048
   u16 MPCDN2; // 0x25F8004A
   u16 MPABN3; // 0x25F8004C
   u16 MPCDN3; // 0x25F8004E
   u16 MPABRA; // 0x25F80050
   u16 MPCDRA; // 0x25F80052
   u16 MPEFRA; // 0x25F80054
   u16 MPGHRA; // 0x25F80056
   u16 MPIJRA; // 0x25F80058
   u16 MPKLRA; // 0x25F8005A
   u16 MPMNRA; // 0x25F8005C
   u16 MPOPRA; // 0x25F8005E
   u16 MPABRB; // 0x25F80060
   u16 MPCDRB; // 0x25F80062
   u16 MPEFRB; // 0x25F80064
   u16 MPGHRB; // 0x25F80066
   u16 MPIJRB; // 0x25F80068
   u16 MPKLRB; // 0x25F8006A
   u16 MPMNRB; // 0x25F8006C
   u16 MPOPRB; // 0x25F8006E
   u16 SCXIN0; // 0x25F80070
   u16 SCXDN0; // 0x25F80072
   u16 SCYIN0; // 0x25F80074
   u16 SCYDN0; // 0x25F80076

#ifdef WORDS_BIGENDIAN
  union {
    struct {
      u32 I:16; // 0x25F80078
      u32 D:16; // 0x25F8007A
    } part;
    u32 all;
  } ZMXN0;

  union {
    struct {
      u32 I:16; // 0x25F8007C
      u32 D:16; // 0x25F8007E
    } part;
    u32 all;
  } ZMYN0;
#else
  union {
    struct {
      u32 D:16; // 0x25F8007A
      u32 I:16; // 0x25F80078
    } part;
    u32 all;
  } ZMXN0;

  union {
    struct {
      u32 D:16; // 0x25F8007E
      u32 I:16; // 0x25F8007C
    } part;
    u32 all;
  } ZMYN0;
#endif

   u16 SCXIN1; // 0x25F80080
   u16 SCXDN1; // 0x25F80082
   u16 SCYIN1; // 0x25F80084
   u16 SCYDN1; // 0x25F80086

#ifdef WORDS_BIGENDIAN
  union {
    struct {
      u32 I:16; // 0x25F80088
      u32 D:16; // 0x25F8008A
    } part;
    u32 all;
  } ZMXN1;

  union {
    struct {
      u32 I:16; // 0x25F8008C
      u32 D:16; // 0x25F8008E
    } part;
    u32 all;
  } ZMYN1;
#else
  union {
    struct {
      u32 D:16; // 0x25F8008A
      u32 I:16; // 0x25F80088
    } part;
    u32 all;
  } ZMXN1;

  union {
    struct {
      u32 D:16; // 0x25F8008E
      u32 I:16; // 0x25F8008C
    } part;
    u32 all;
  } ZMYN1;
#endif

   u16 SCXN2;  // 0x25F80090
   u16 SCYN2;  // 0x25F80092
   u16 SCXN3;  // 0x25F80094
   u16 SCYN3;  // 0x25F80096
   u16 ZMCTL;  // 0x25F80098
   u16 SCRCTL; // 0x25F8009A
#ifdef WORDS_BIGENDIAN
   union {
      struct {
         u32 U:16; // 0x25F8009C
         u32 L:16; // 0x25F8009E
      } part;
      u32 all;
   } VCSTA;

   union {
      struct {
         u32 U:16; // 0x25F800A0
         u32 L:16; // 0x25F800A2
      } part;
      u32 all;
   } LSTA0;

   union {
      struct {
         u32 U:16; // 0x25F800A4
         u32 L:16; // 0x25F800A6
      } part;
      u32 all;
   } LSTA1;

   union {
      struct {
         u32 U:16; // 0x25F800A8
         u32 L:16; // 0x25F800AA
      } part;
      u32 all;
   } LCTA;
#else
   union {
      struct {
         u32 L:16; // 0x25F8009E
         u32 U:16; // 0x25F8009C
      } part;
      u32 all;
   } VCSTA;

   union {
      struct {
         u32 L:16; // 0x25F800A2
         u32 U:16; // 0x25F800A0
      } part;
      u32 all;
   } LSTA0;

   union {
      struct {
         u32 L:16; // 0x25F800A6
         u32 U:16; // 0x25F800A4
      } part;
      u32 all;
   } LSTA1;

   union {
      struct {
         u32 L:16; // 0x25F800AA
         u32 U:16; // 0x25F800A8
      } part;
      u32 all;
   } LCTA;
#endif

   u16 BKTAU;  // 0x25F800AC
   u16 BKTAL;  // 0x25F800AE
   u16 RPMD;   // 0x25F800B0
   u16 RPRCTL; // 0x25F800B2
   u16 KTCTL;  // 0x25F800B4
   u16 KTAOF;  // 0x25F800B6
   u16 OVPNRA; // 0x25F800B8
   u16 OVPNRB; // 0x25F800BA
#ifdef WORDS_BIGENDIAN
   union {
      struct {
         u32 U:16; // 0x25F800BC
         u32 L:16; // 0x25F800BE
      } part;
      u32 all;
   } RPTA;
#else
   union {
      struct {
         u32 L:16; // 0x25F800BE
         u32 U:16; // 0x25F800BC
      } part;
      u32 all;
   } RPTA;
#endif
   u16 WPSX0;  // 0x25F800C0
   u16 WPSY0;  // 0x25F800C2
   u16 WPEX0;  // 0x25F800C4
   u16 WPEY0;  // 0x25F800C6
   u16 WPSX1;  // 0x25F800C8
   u16 WPSY1;  // 0x25F800CA
   u16 WPEX1;  // 0x25F800CC
   u16 WPEY1;  // 0x25F800CE
   u16 WCTLA;  // 0x25F800D0
   u16 WCTLB;  // 0x25F800D2
   u16 WCTLC;  // 0x25F800D4
   u16 WCTLD;  // 0x25F800D6
#ifdef WORDS_BIGENDIAN
  union {
    struct {
      u32 U:16; // 0x25F800D8
      u32 L:16; // 0x25F800DA
    } part;
    u32 all;
  } LWTA0;

  union {
    struct {
      u32 U:16; // 0x25F800DC
      u32 L:16; // 0x25F800DE
    } part;
    u32 all;
  } LWTA1;
#else
  union {
    struct {
      u32 L:16; // 0x25F800D8
      u32 U:16; // 0x25F800DA
    } part;
    u32 all;
  } LWTA0;

  union {
    struct {
      u32 L:16; // 0x25F800DC
      u32 U:16; // 0x25F800DE
    } part;
    u32 all;
  } LWTA1;
#endif

   u16 SPCTL;  // 0x25F800E0
   u16 SDCTL;  // 0x25F800E2
   u16 CRAOFA; // 0x25F800E4
   u16 CRAOFB; // 0x25F800E6
   u16 LNCLEN; // 0x25F800E8
   u16 SFPRMD; // 0x25F800EA
   u16 CCCTL;  // 0x25F800EC
   u16 SFCCMD; // 0x25F800EE
   u16 PRISA;  // 0x25F800F0
   u16 PRISB;  // 0x25F800F2
   u16 PRISC;  // 0x25F800F4
   u16 PRISD;  // 0x25F800F6
   u16 PRINA;  // 0x25F800F8
   u16 PRINB;  // 0x25F800FA
   u16 PRIR;   // 0x25F800FC
   u16 pad;    // 0x25F800FE — reserved (ST-013-R3 §3.29, must not be accessed)
   u16 CCRSA;  // 0x25F80100
   u16 CCRSB;  // 0x25F80102
   u16 CCRSC;  // 0x25F80104
   u16 CCRSD;  // 0x25F80106
   u16 CCRNA;  // 0x25F80108
   u16 CCRNB;  // 0x25F8010A
   u16 CCRR;   // 0x25F8010C
   u16 CCRLB;  // 0x25F8010E
   u16 CLOFEN; // 0x25F80110
   u16 CLOFSL; // 0x25F80112
   u16 COAR;   // 0x25F80114
   u16 COAG;   // 0x25F80116
   u16 COAB;   // 0x25F80118
   u16 COBR;   // 0x25F8011A
   u16 COBG;   // 0x25F8011C
   u16 COBB;   // 0x25F8011E
} Vdp2;

extern Vdp2 * Vdp2Regs;

typedef struct {
   int ColorMode;
} Vdp2Internal_struct;

extern Vdp2Internal_struct Vdp2Internal;
extern int vdp2_is_odd_frame;

extern Vdp2 Vdp2Lines[270];

struct CellScrollData
{
   u32 data[88]; /* 44 cells H (352px / 8px per cell) x 2 VRAM banks */
};

extern struct CellScrollData cell_scroll_data[270];

// struct for Vdp2 part that shouldn't be saved
typedef struct {
   int disptoggle;
   int cpu_cycle_a;
   int cpu_cycle_b;
   u8 AC_VRAM[4][8];
   u8 vdp2_blocked[4];
} Vdp2External_struct;

extern Vdp2External_struct Vdp2External;

int Vdp2Init(void);
void Vdp2DeInit(void);
void Vdp2Reset(void);
void Vdp2VBlankIN(void);
void Vdp2VBlankIN_It(void);
void Vdp2HBlankIN(void);
void Vdp2HBlankIN_It(void);
void Vdp2StartVisibleLine(void);
void Vdp2VBlankOUT(void);
void Vdp2VBlankOUT_It(void);
void Vdp2SendExternalLatch(int valid, int hcnt, int vcnt);
void SpeedThrottleEnable(void);
void SpeedThrottleDisable(void);

u8 Vdp2RamIsUpdated(void);

u8 FASTCALL     Vdp2ReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL    Vdp2ReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL    Vdp2ReadLong(SH2_struct *context, u8*, u32);
void FASTCALL   Vdp2WriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL   Vdp2WriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL   Vdp2WriteLong(SH2_struct *context, u8*, u32, u32);

int Vdp2SaveState(void ** stream);
int Vdp2LoadState(const void * stream, int version, int size);

void ToggleNBG0(void);
void ToggleNBG1(void);
void ToggleNBG2(void);
void ToggleNBG3(void);
void ToggleRBG0(void);
void ToggleRBG1(void);
void ToggleFullScreen(void);

Vdp2 * Vdp2RestoreRegs(int line, Vdp2* lines);

#include "threads.h"

int VideoSetFilterType( int video_filter_type );
void vdp2ReqDump();
void vdp2ReqRestore();

#ifdef __cplusplus
}
#endif

#endif
