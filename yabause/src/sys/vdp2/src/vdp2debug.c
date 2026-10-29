/*  Copyright 2005-2008 Theo Berkau

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

/*! \file vdp2debug.c
    \brief VDP2 debug functions
*/

#include <string.h>
#include "vdp2.h"
#include "vdp1.h"
#include "ygl.h"
#include "vdp2debug.h"
#include "vidshared.h"
#include "yabause.h"

//////////////////////////////////////////////////////////////////////////////

static INLINE void Vdp2GetPlaneSize(int planedata, int *planew, int *planeh)
{
   switch(planedata)
   {
      case 0:
         *planew = *planeh = 1;
         break;
      case 1:
         *planew = 2;
         *planeh = 1;
         break;
      case 3:
         *planew = *planeh = 2;
         break;
      default:
         *planew = *planeh = 1;
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE char *AddBppString(char *outstring, int bpp)
{
   switch (bpp)
   {
      case 0:
         AddString(outstring, "4-bit(16 colors)\r\n");
         break;
      case 1:
         AddString(outstring, "8-bit(256 colors)\r\n");
         break;
      case 2:
         AddString(outstring, "16-bit(2048 colors)\r\n");
         break;
      case 3:
         AddString(outstring, "16-bit(32,768 colors)\r\n");
         break;
      case 4:
         AddString(outstring, "32-bit(16.7 mil colors)\r\n");
         break;
      default:
         AddString(outstring, "Unsupported BPP\r\n");
         break;
   }

   return outstring;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE char *AddMosaicString(char *outstring, int mask)
{
   if (Vdp2Regs->MZCTL & mask)
   {
      AddString(outstring, "Mosaic Size = width %d height %d\r\n", ((Vdp2Regs->MZCTL >> 8) & 0xf) + 1, (Vdp2Regs->MZCTL >> 12) + 1);
   }

   return outstring;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE char *AddBitmapInfoString(char *outstring, int wh, int palnum, int mapofn)
{
   int cellw=0, cellh=0;

   // Bitmap
   switch(wh)
   {
      case 0:
         cellw = 512;
         cellh = 256;
         break;
      case 1:
         cellw = 512;
         cellh = 512;
         break;
      case 2:
         cellw = 1024;
         cellh = 256;
         break;
      case 3:
         cellw = 1024;
         cellh = 512;
         break;
   }

   AddString(outstring, "Bitmap(%dx%d)\r\n", cellw, cellh);

   if (palnum & 0x20)
   {
      AddString(outstring, "Bitmap Special Priority enabled\r\n");
   }

   if (palnum & 0x10)
   {
      AddString(outstring, "Bitmap Special Color Calculation enabled\r\n");
   }

   AddString(outstring, "Bitmap Address = %X\r\n", (mapofn & 0x7) * 0x20000);
   /* Le champ NxBMP (3 bits) donne les bits 6-4 du numero de palette, soit
    * les bits 10-8 de l'index CRAM (cf. vidcs.c : paladdr = NxBMP << 4, puis
    * index = (paladdr << 4) | dot). L'ancien libelle "Palette Address"
    * affichait paladdr (NxBMP << 4), qui n'est ni une adresse ni un index :
    * NxBMP=1 s'affichait "10" alors que la palette commence a l'index 0x100.
    * Le decalage CRAOFA s'ajoute a cet index. */
   AddString(outstring, "Bitmap Palette Number = %d (Color RAM index 0x%03X)\r\n",
             (palnum & 0x7), (palnum & 0x7) << 8);

   return outstring;
}

//////////////////////////////////////////////////////////////////////////////

static void CalcWindowCoordinates(int num, int *hstart, int *vstart, int *hend, int *vend)
{
   clipping_struct clip;
   ReadWindowCoordinates(num, &clip, Vdp2Regs);
   *hstart = clip.xstart;
   *vstart = clip.ystart;
   *hend = clip.xend;
   *vend = clip.yend;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE char *AddWindowInfoString(char *outstring, int wctl, int issprite)
{
   if (wctl & 0x2)
   {
      int hstart=0, vstart=0, hend=0, vend=0;

      AddString(outstring, "Using Window W0:\r\n");

      // Retrieve Window Points
      if (Vdp2Regs->LWTA0.all & 0x80000000)
      {
         // Line Window
         AddString(outstring, "Line Window Table Address = %08lX\r\n", 0x05E00000UL | ((Vdp2Regs->LWTA0.all & 0x7FFFEUL) << 1));
      }
      else
      {
         // Normal Window
         CalcWindowCoordinates(0, &hstart, &vstart, &hend, &vend);
         AddString(outstring, "Horizontal start = %d\r\n", hstart);
         AddString(outstring, "Vertical start = %d\r\n", vstart);
         AddString(outstring, "Horizontal end = %d\r\n", hend);
         AddString(outstring, "Vertical end = %d\r\n", vend);
      }

      AddString(outstring, "Valid area of Window is %s\r\n", (wctl & 0x1) ? "outside" : "inside");
   }

   if (wctl & 0x8)
   {
      int hstart=0, vstart=0, hend=0, vend=0;

      AddString(outstring, "Using Window W1:\r\n");

      // Retrieve Window Points
      if (Vdp2Regs->LWTA1.all & 0x80000000)
      {
         // Line Window
         AddString(outstring, "Line Table address = %08lX\r\n", 0x05E00000UL | ((Vdp2Regs->LWTA1.all & 0x7FFFEUL) << 1));
      }
      else
      {
         // Normal Window
         CalcWindowCoordinates(1, &hstart, &vstart, &hend, &vend);
         AddString(outstring, "Horizontal start = %d\r\n", hstart);
         AddString(outstring, "Vertical start = %d\r\n", vstart);
         AddString(outstring, "Horizontal end = %d\r\n", hend);
         AddString(outstring, "Vertical end = %d\r\n", vend);
      }

      AddString(outstring, "Valid area of Window is %s\r\n", (wctl & 0x4) ? "outside" : "inside");
   }

   if (wctl & 0x20)
   {
      AddString(outstring, "Using Sprite Window:\r\n");
      AddString(outstring, "Valid area of Window is %s\r\n", (wctl & 0x10) ? "outside" : "inside");
   }

   if (wctl & 0x2A)
   {
      AddString(outstring, "Window Overlap Logic: %s\r\n", (wctl & 0x80) ? "AND" : "OR");
   }
   else
   {
      if (wctl & 0x80)
      {
         // Whole screen window enabled
         AddString(outstring, "Window enabled whole screen\r\n");
      }
      else
      {
         // Whole screen window disabled
         AddString(outstring, "Window disabled whole screen\r\n");
      }
   }

   return outstring;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE char *AddMapInfo(char *outstring, int patternwh, u16 PNC, u8 PLSZ, int mapoffset, int numplanes, u8 *map)
{
   int deca;
   int multi;
   int i;
   int patterndatasize;
   int planew, planeh;
   u32 tmp=0;
   u32 addr;

   if(PNC & 0x8000)
      patterndatasize = 1;
   else
      patterndatasize = 2;

   Vdp2GetPlaneSize(PLSZ, &planew, &planeh);
   deca = planeh + planew - 2;
   multi = planeh * planew;

   // Map Planes A-D
   for (i = 0; i < numplanes; i++)
   {
      tmp = mapoffset | map[i];

      if (patterndatasize == 1)
      {
         if (patternwh == 1)
            addr = ((tmp & 0x3F) >> deca) * (multi * 0x2000);
         else
            addr = (tmp >> deca) * (multi * 0x800);
      }
      else
      {
         if (patternwh == 1)
            addr = ((tmp & 0x1F) >> deca) * (multi * 0x4000);
         else
            addr = ((tmp & 0x7F) >> deca) * (multi * 0x1000);
      }

      AddString(outstring, "Plane %C Address = %08X\r\n", 0x41+i, (unsigned int)addr);
   }

   return outstring;
}

//////////////////////////////////////////////////////////////////////////////

/* CCCTL (ST-058-R2, registre 1800ECH) :
 *   bits 6-0  : xxCCEN, calcul de couleur par ecran (N0 N1 N2 N3 R0 LC SP)
 *   bit 8     : CCMD   (0 = selon le taux, 1 = addition)
 *   bit 9     : CCRTMD (0 = taux de l'ecran du dessus, 1 = du second ecran)
 *   bit 10    : EXCCEN (calcul de couleur etendu)
 *   bits 14-12: BOKN   (ecran de gradation : 0 SPR, 1 RBG0, 2 NBG0/RBG1,
 *                       4 NBG1, 5 NBG2, 6 NBG3)
 *   bit 15    : BOKEN  (gradation)
 * L'ancien code comparait les bits 10-8 au numero d'ecran (jamais la
 * gradation, et EXCCEN pris pour elle quand le numero valait 4), et
 * n'affichait plus du tout l'etat "active" des que CCRTMD valait 1. */
static INLINE char *AddColorCalcInfo(char *outstring, u16 calcenab, u16 gradnum, u16 calcratio, u16 sfcnum)
{
   const u16 cc = Vdp2Regs->CCCTL;
   if (cc & calcenab)
   {
      AddString(outstring, "Color Calculation Enabled (%s)\r\n", (cc & 0x100) ? "add" : "ratio");
      /* La gradation (BOKEN/BOKN, comparee a gradnum) est deja affichee
       * par chaque ecran via getBlur(), qui tient compte du mode CRAM. */
      (void)gradnum;
      if (cc & 0x0400)
         AddString(outstring, "Extended Color Calculation Enabled\r\n");
      AddString(outstring, "Special Color Calculation Mode = %d\r\n", sfcnum);
      if (cc & 0x200)
         AddString(outstring, "Ratio taken from the second screen (CCRTMD=1)\r\n");
   }
   else
   {
      AddString(outstring, "Color Calculation Disabled\r\n");
   }
   AddString(outstring, "Color Calculation Ratio = %d:%d\r\n", 31 - calcratio, 1 + calcratio);

   return outstring;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE char *AddColorOffsetInfo(char *outstring, u16 offsetselectenab)
{
   s32 r, g, b;

   if (Vdp2Regs->CLOFEN & offsetselectenab)
   {
      if (Vdp2Regs->CLOFSL & offsetselectenab)
      {
         r = Vdp2Regs->COBR & 0xFF;
         if (Vdp2Regs->COBR & 0x100)
            r |= 0xFFFFFF00;

         g = Vdp2Regs->COBG & 0xFF;
         if (Vdp2Regs->COBG & 0x100)
            g |= 0xFFFFFF00;

         b = Vdp2Regs->COBB & 0xFF;
         if (Vdp2Regs->COBB & 0x100)
            b |= 0xFFFFFF00;

         AddString(outstring, "Color Offset B Enabled\r\n");
         AddString(outstring, "R = %ld, G = %ld, B = %ld\r\n",
		   (long)r, (long)g, (long)b);
      }
      else
      {
         r = Vdp2Regs->COAR & 0xFF;
         if (Vdp2Regs->COAR & 0x100)
            r |= 0xFFFFFF00;

         g = Vdp2Regs->COAG & 0xFF;
         if (Vdp2Regs->COAG & 0x100)
            g |= 0xFFFFFF00;

         b = Vdp2Regs->COAB & 0xFF;
         if (Vdp2Regs->COAB & 0x100)
            b |= 0xFFFFFF00;

         AddString(outstring, "Color Offset A Enabled\r\n");
         AddString(outstring, "R = %ld, G = %ld, B = %ld\r\n",
		   (long)r, (long)g, (long)b);
      }
   }

   return outstring;
}

//////////////////////////////////////////////////////////////////////////////

static INLINE char *AddSpecialPriorityInfo(char *outstring, u16 spriority)
{
   if (spriority & 0x3)
   {
      AddString(outstring, "Special Priority Mode %d used", spriority & 0x3);

      switch (spriority & 0x3)
      {
         case 1:
            AddString(outstring, "(per tile)\r\n");
            break;
         case 2:
            AddString(outstring, "(per pixel)\r\n");
            break;
         case 3:
            AddString(outstring, "(undocumented)\r\n");
            break;
         default: break;
      }
   }

   return outstring;
}
//////////////////////////////////////////////////////////////////////////////

static int getBlur(Vdp2 *varVdp2Regs, int layer) {
  int val = (varVdp2Regs->CCCTL & 0xF000) >> 12;
  if (Vdp2Internal.ColorMode != 0) return 0;
  if ((val & 0x8) == 0) return 0;
  switch (layer) {
    case RBG0:
      return ((val & 0x7) == 1);
    break;
    case RBG1:
      return ((val & 0x7) == 2);
    break;
    case NBG0:
      return ((val & 0x7) == 2);
    break;
    case NBG1:
      return ((val & 0x7) == 4);
    break;
    case NBG2:
      return ((val & 0x7) == 5);
    break;
    case NBG3:
      return ((val & 0x7) == 6);
    break;
    case SPRITE:
      return ((val & 0x7) == 0);
    break;
    default:
      return 0;
  }
  return 0;
}
//////////////////////////////////////////////////////////////////////////////

/* ST-058-R2 6.4 : KTCTL (1800B4H) bit 0 = RAKTE, bits 3-2 = RAKMD,
 * bit 8 = RBKTE, bits 11-10 = RBKMD. Necessaire pour RPMD 2 et 3,
 * ou les deux jeux de parametres sont actifs simultanement. */
static char *AddRotCoefInfo(char *outstring)
{
   if (Vdp2Regs->KTCTL & 0x1)
      AddString(outstring, "Parameter A Coefficient Table Enabled(Mode %d)\r\n", (Vdp2Regs->KTCTL >> 2) & 0x3);
   if (Vdp2Regs->KTCTL & 0x100)
      AddString(outstring, "Parameter B Coefficient Table Enabled(Mode %d)\r\n", (Vdp2Regs->KTCTL >> 10) & 0x3);
   return outstring;
}

//////////////////////////////////////////////////////////////////////////////

/* Raisons pour lesquelles un ecran dont le bit BGON est a 1 n'est pourtant
 * pas affiche. Memes regles que isEnabled() du renderer (vidcs.c,
 * ST-058-R2 sec. 4.1 et p.61) plus la priorite 0 (ecran transparent). Sans
 * cette ligne le panneau decrivait comme actif un ecran que le renderer
 * ne dessine pas (ex. NBG2 sous un NBG0 en 2048 couleurs). */
static char *AddDisplayConflict(char *outstring, int screen, int priority)
{
   const u16 bgon = Vdp2Regs->BGON;
   const int r0on = (bgon & 0x10) != 0, r1on = (bgon & 0x20) != 0;
   const int n0on = (bgon & 0x1) != 0, n1on = (bgon & 0x2) != 0;
   const int n0color = (Vdp2Regs->CHCTLA >> 4) & 0x7;
   const int n1color = (Vdp2Regs->CHCTLA >> 12) & 0x3;

   if (screen >= NBG0 && screen <= NBG3 && r0on && r1on)
      AddString(outstring, "*** NOT DISPLAYED: RBG0 and RBG1 both on ***\r\n");
   if (screen == NBG0 && r1on && !r0on)
      AddString(outstring, "*** RBG1 without RBG0: prohibited setting ***\r\n");
   if (screen == RBG1 && !r0on)
      AddString(outstring, "*** NOT DISPLAYED: RBG1 requires RBG0 ***\r\n");
   /* AddString est un bloc { } : accolades obligatoires devant un else. */
   if ((screen == NBG1 || screen == NBG2 || screen == NBG3) && n0on && n0color >= 4) {
      AddString(outstring, "*** NOT DISPLAYED: NBG0 uses 16M colors ***\r\n");
   } else if (screen == NBG2 && n0on && n0color >= 2) {
      AddString(outstring, "*** NOT DISPLAYED: NBG0 uses 2048 or more colors ***\r\n");
   }
   if (screen == NBG3 && n1on && n1color >= 2)
      AddString(outstring, "*** NOT DISPLAYED: NBG1 uses 2048 or more colors ***\r\n");
   if (priority == 0)
      AddString(outstring, "*** NOT DISPLAYED: priority 0 ***\r\n");
   return outstring;
}

/* Position de scroll brute : partie entiere 11 bits + fraction 8 bits.
 * L'ancien affichage -(SCxIN % 512) etait signe a l'envers, ignorait la
 * fraction et repliait sur 512 quelle que soit la taille de l'ecran (un
 * bitmap 512x256 scrolle de 240 lignes s'affichait "y = -240", un
 * scroll de 496 sur un bitmap de 256 lignes "y = -496"). */
static char *AddScrollString(char *outstring, u16 xi, u16 xd, u16 yi, u16 yd)
{
   AddString(outstring, "Screen Scroll x = %.3f, y = %.3f\r\n",
             (xi & 0x7FF) + ((xd >> 8) & 0xFF) / 256.0,
             (yi & 0x7FF) + ((yd >> 8) & 0xFF) / 256.0);
   return outstring;
}

/* Increment de coordonnee (registre) et facteur de zoom affiche (son
 * inverse). Un increment nul donnait "inf" ; le renderer n'affiche alors
 * pas l'ecran (Vdp2DrawNBG0/1 retournent). */
static char *AddZoomString(char *outstring, u32 zx, u32 zy)
{
   zx &= 0x7FF00; zy &= 0x7FF00;
   AddString(outstring, "Coordinate inc x = %.4f, y = %.4f\r\n", zx / 65536.0, zy / 65536.0);
   if (zx == 0 || zy == 0) {
      AddString(outstring, "*** Coordinate increment 0: screen not drawn ***\r\n");
   } else {
      AddString(outstring, "Zoom factor x = %.4f, y = %.4f\r\n", 65536.0 / zx, 65536.0 / zy);
   }
   return outstring;
}

/* Line scroll / line zoom / vertical cell scroll (SCRCTL, 6 bits par ecran).
 * La table et l'intervalle etaient affiches seulement pour le line scroll
 * (test & 0x6) : avec le line zoom seul (bit 3), on ne voyait pas ou etait
 * la table qu'il lit. L'intervalle est double en single-density interlace
 * (ReadLineScrollData, vidshared.h). */
static char *AddLineScrollString(char *outstring, u16 reg, u32 lsta)
{
   if (reg & 0x8) AddString(outstring, "Line Zoom enabled\r\n");
   if (reg & 0x4) AddString(outstring, "Line Scroll Vertical enabled\r\n");
   if (reg & 0x2) AddString(outstring, "Line Scroll Horizontal enabled\r\n");
   if (reg & 0xE)
   {
      int inc = 1 << ((reg >> 4) & 0x3);
      if (((Vdp2Regs->TVMD >> 6) & 0x3) == 0x2) inc *= 2;
      AddString(outstring, "Line Scroll Table Address = %08X\r\n", (int)(0x05E00000 + ((lsta & 0x7FFFE) << 1)));
      AddString(outstring, "Line Scroll Interval = every %d line(s)\r\n", inc);
   }
   if (reg & 0x1)
   {
      AddString(outstring, "Vertical Cell Scroll enabled\r\n");
      AddString(outstring, "Vertical Cell Scroll Table Address = %08X\r\n", (int)(0x05E00000 + ((Vdp2Regs->VCSTA.all & 0x7FFFE) << 1)));
   }
   return outstring;
}

void Vdp2DebugStatsRBG0(char *outstring, int *isenabled)
{
   int patternwh=((Vdp2Regs->CHCTLB & 0x100) >> 8) + 1;
   u8 map[16];
   int hstart, vstart, hend, vend;

   if (Vdp2Regs->BGON & 0x10)
   {
      // enabled
      int rotatenum=0;
      int coeftbl=0, coefmode=0;

      *isenabled = 1;

      outstring = AddDisplayConflict(outstring, RBG0, Vdp2Regs->PRIR & 0x7);

      // Which Rotation Parameter is being used
      switch (Vdp2Regs->RPMD & 0x3)
      {
         case 0:
            // Parameter A
            rotatenum = 0;
            coeftbl = Vdp2Regs->KTCTL & 0x1;
            coefmode = (Vdp2Regs->KTCTL >> 2) & 0x3;
            AddString(outstring, "Using Parameter %C\r\n", 'A' + rotatenum);
            break;
         case 1:
            // Parameter B
            rotatenum = 1;
            coeftbl = Vdp2Regs->KTCTL & 0x100;
            coefmode = (Vdp2Regs->KTCTL >> 10) & 0x3;
            AddString(outstring, "Using Parameter B\r\n");
            break;
         case 2:
            // Parameter A+B switched via coefficients
            AddString(outstring, "Parameter A/B switched via coefficients\r\n");
            /* ST-058-R2 6.3/6.4 : en RPMD 2 et 3 les deux jeux de
             * parametres ont chacun leur table (RAKTE / RBKTE).
             * coeftbl restait a 0 : rien n'etait affiche. */
            outstring = AddRotCoefInfo(outstring);
            break;
         case 3:
            // Parameter A+B switched via rotation parameter window
            AddString(outstring, "Parameter A/B switched parameter window\r\n");
            outstring = AddRotCoefInfo(outstring);
            /* ST-058-R2 1800D6H (WCTLD) : W0E bit 1 / W0A bit 0, W1E bit 3 /
             * W1A bit 2, LOG bit 7. Les deux fenetres peuvent etre actives
             * ensemble : l'ancien "else if" masquait W1 des que W0 l'etait,
             * et la zone (dedans/dehors) n'etait jamais affichee. */
            if (Vdp2Regs->WCTLD & 0x2)
            {
               CalcWindowCoordinates(0, &hstart, &vstart, &hend, &vend);
               AddString(outstring, "Rotation Window 0 (%s): (%d,%d)-(%d,%d)\r\n",
                         (Vdp2Regs->WCTLD & 0x1) ? "outside" : "inside", hstart, vstart, hend, vend);
            }
            if (Vdp2Regs->WCTLD & 0x8)
            {
               CalcWindowCoordinates(1, &hstart, &vstart, &hend, &vend);
               AddString(outstring, "Rotation Window 1 (%s): (%d,%d)-(%d,%d)\r\n",
                         (Vdp2Regs->WCTLD & 0x4) ? "outside" : "inside", hstart, vstart, hend, vend);
            }
            if ((Vdp2Regs->WCTLD & 0xA) == 0xA)
               AddString(outstring, "Rotation Window Logic: %s\r\n", (Vdp2Regs->WCTLD & 0x80) ? "AND" : "OR");
            break;
      }

      if (coeftbl)
      {
         AddString(outstring, "Coefficient Table Enabled(Mode %d)\r\n", coefmode);
      }

      // Mosaic
      outstring = AddMosaicString(outstring, 0x10);

      // BPP
      outstring = AddBppString(outstring, (Vdp2Regs->CHCTLB >> 12) & 0x7);

      // Bitmap or Tile mode?
      if (Vdp2Regs->CHCTLB & 0x200)
      {
         // Bitmap mode
         if (rotatenum == 0)
         {
            // Parameter A
            outstring = AddBitmapInfoString(outstring, (Vdp2Regs->CHCTLB & 0x400) >> 10, Vdp2Regs->BMPNB, Vdp2Regs->MPOFR);
         }
         else
         {
            // Parameter B
            outstring = AddBitmapInfoString(outstring, (Vdp2Regs->CHCTLB & 0x400) >> 10, Vdp2Regs->BMPNB, Vdp2Regs->MPOFR >> 4);
         }
      }
      else
      {
         // Tile mode
         int patterndatasize;
         u16 supplementdata=Vdp2Regs->PNCR & 0x3FF;
         int planew=0, planeh=0;

         if(Vdp2Regs->PNCR & 0x8000)
            patterndatasize = 1;
         else
            patterndatasize = 2;

         AddString(outstring, "Tile(%dH x %dV)\r\n", patternwh, patternwh);

         if (rotatenum == 0)
         {
            // Parameter A
            Vdp2GetPlaneSize((Vdp2Regs->PLSZ & 0x300) >> 8, &planew, &planeh);
         }
         else
         {
            // Parameter B
            /* ST-058-R2 18003AH : RBPLSZ = PLSZ bits 13-12. */
            Vdp2GetPlaneSize((Vdp2Regs->PLSZ & 0x3000) >> 12, &planew, &planeh);
         }

         AddString(outstring, "Plane Size = %dH x %dV\r\n", planew, planeh);

         // Pattern Name Control stuff
         if (patterndatasize == 2)
         {
            AddString(outstring, "Pattern Name data size = 2 words\r\n");
         }
         else
         {
            AddString(outstring, "Pattern Name data size = 1 word\r\n");
            AddString(outstring, "Character Number Supplement bit = %d\r\n", (Vdp2Regs->PNCR >> 14) & 0x1);
            AddString(outstring, "Special Priority bit = %d\r\n", (supplementdata >> 9) & 0x1);
            AddString(outstring, "Special Color Calculation bit = %d\r\n", (supplementdata >> 8) & 0x1);
            AddString(outstring, "Supplementary Palette number = %d\r\n", (supplementdata >> 5) & 0x7);
            AddString(outstring, "Supplementary Color number = %d\r\n", supplementdata & 0x1f);
         }

         if (rotatenum == 0)
         {
            // Parameter A
            map[0] = Vdp2Regs->MPABRA & 0xFF;
            map[1] = Vdp2Regs->MPABRA >> 8;
            map[2] = Vdp2Regs->MPCDRA & 0xFF;
            map[3] = Vdp2Regs->MPCDRA >> 8;
            map[4] = Vdp2Regs->MPEFRA & 0xFF;
            map[5] = Vdp2Regs->MPEFRA >> 8;
            map[6] = Vdp2Regs->MPGHRA & 0xFF;
            map[7] = Vdp2Regs->MPGHRA >> 8;
            map[8] = Vdp2Regs->MPIJRA & 0xFF;
            map[9] = Vdp2Regs->MPIJRA >> 8;
            map[10] = Vdp2Regs->MPKLRA & 0xFF;
            map[11] = Vdp2Regs->MPKLRA >> 8;
            map[12] = Vdp2Regs->MPMNRA & 0xFF;
            map[13] = Vdp2Regs->MPMNRA >> 8;
            map[14] = Vdp2Regs->MPOPRA & 0xFF;
            map[15] = Vdp2Regs->MPOPRA >> 8;
            outstring = AddMapInfo(outstring, patternwh, Vdp2Regs->PNCR, (Vdp2Regs->PLSZ >> 8) & 0x3, (Vdp2Regs->MPOFR & 0x7) << 6, 16, map);
         }
         else
         {
            // Parameter B
            map[0] = Vdp2Regs->MPABRB & 0xFF;
            map[1] = Vdp2Regs->MPABRB >> 8;
            map[2] = Vdp2Regs->MPCDRB & 0xFF;
            map[3] = Vdp2Regs->MPCDRB >> 8;
            map[4] = Vdp2Regs->MPEFRB & 0xFF;
            map[5] = Vdp2Regs->MPEFRB >> 8;
            map[6] = Vdp2Regs->MPGHRB & 0xFF;
            map[7] = Vdp2Regs->MPGHRB >> 8;
            map[8] = Vdp2Regs->MPIJRB & 0xFF;
            map[9] = Vdp2Regs->MPIJRB >> 8;
            map[10] = Vdp2Regs->MPKLRB & 0xFF;
            map[11] = Vdp2Regs->MPKLRB >> 8;
            map[12] = Vdp2Regs->MPMNRB & 0xFF;
            map[13] = Vdp2Regs->MPMNRB >> 8;
            map[14] = Vdp2Regs->MPOPRB & 0xFF;
            map[15] = Vdp2Regs->MPOPRB >> 8;
            outstring = AddMapInfo(outstring, patternwh, Vdp2Regs->PNCR, (Vdp2Regs->PLSZ >> 12) & 0x3, (Vdp2Regs->MPOFR & 0x70) << 2, 16, map);
         }

/*
         // Figure out Cell start address
         switch(patterndatasize)
         {
            case 1:
            {
               tmp = readWord(vram, addr);

               switch(auxMode)
               {
                  case 0:
                     switch(patternwh)
                     {
                        case 1:
                           charAddr = (tmp & 0x3FF) | ((supplementdata & 0x1F) << 10);
                           break;
                        case 2:
                           charAddr = ((tmp & 0x3FF) << 2) |  (supplementdata & 0x3) | ((supplementdata & 0x1C) << 10);
                           break;
                     }
                     break;
                  case 1:
                     switch(patternwh)
                     {
                        case 1:
                           charAddr = (tmp & 0xFFF) | ((supplementdata & 0x1C) << 10);
                           break;
                        case 4:
                           charAddr = ((tmp & 0xFFF) << 2) |  (supplementdata & 0x3) | ((supplementdata & 0x10) << 10);
                           break;
                     }
                     break;
               }
               break;
            }
            case 2:
            {
               unsigned short tmp1 = readWord(vram, addr);
               unsigned short tmp2 = readWord(vram, addr+2);

               charAddr = tmp2 & 0x7FFF;
               break;
            }
         }

         if (!(readWord(reg, 0x6) & 0x8000))
            charAddr &= 0x3FFF;

         charAddr *= 0x20; // selon Runik

         AddString(outstring, "Cell Data Address = %X\r\n", charAddr);
*/
      }
      AddString(outstring, "Gradation Calculation %s\n", (getBlur(Vdp2Regs, RBG0)==0)?"Disabled":"Enabled");
      // Window Control
      outstring = AddWindowInfoString(outstring, Vdp2Regs->WCTLC, 0);

      // Shadow Control here

      // Color Ram Address Offset
      AddString(outstring, "Color Ram Address Offset = %X\r\n", (Vdp2Regs->CRAOFB & 0x7) << 8);

      // Special Priority Mode
      outstring = AddSpecialPriorityInfo(outstring, Vdp2Regs->SFPRMD >> 8);

      // Color Calculation Control here

      // Special Color Calculation Mode here

      // Priority Number
      AddString(outstring, "Priority = %d\r\n", Vdp2Regs->PRIR & 0x7);

      AddString(outstring, "Line Color Screen insertion = %d\r\n", (Vdp2Regs->LNCLEN & 0x10)!=0);

      // Color Calculation
      outstring = AddColorCalcInfo(outstring, 0x0010, 0x0001, Vdp2Regs->CCRR & 0x1F, (Vdp2Regs->SFCCMD >> 8) & 0x3);

      // Color Offset
      outstring = AddColorOffsetInfo(outstring, 0x0010);

   }
   else
   {
      // disabled
      *isenabled = 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2DebugStatsNBG0(char *outstring, int *isenabled)
{
   u16 lineVerticalScrollReg = Vdp2Regs->SCRCTL & 0x3F;
   int isbitmap=Vdp2Regs->CHCTLA & 0x2;
   int patternwh=(Vdp2Regs->CHCTLA & 0x1) + 1;
   u8 map[4];
   int layer;

   if (Vdp2Regs->BGON & 0x1)
   {
      // enabled
      *isenabled = 1;

      outstring = AddDisplayConflict(outstring, NBG0, Vdp2Regs->PRINA & 0x7);

      // Mosaic
      outstring = AddMosaicString(outstring, 0x1);

      // BPP
      outstring = AddBppString(outstring, (Vdp2Regs->CHCTLA & 0x70) >> 4);

      // Bitmap or Tile mode?(RBG1 can only do Tile mode)
      if (isbitmap)
      {
         // Bitmap
         outstring = AddBitmapInfoString(outstring, (Vdp2Regs->CHCTLA & 0xC) >> 2, Vdp2Regs->BMPNA, Vdp2Regs->MPOFN);
      }
      else
      {
         // Tile
         int patterndatasize;
         u16 supplementdata=Vdp2Regs->PNCN0 & 0x3FF;
         int planew=0, planeh=0;

         if(Vdp2Regs->PNCN0 & 0x8000)
            patterndatasize = 1;
         else
            patterndatasize = 2;

         AddString(outstring, "Tile(%dH x %dV)\r\n", patternwh, patternwh);

         Vdp2GetPlaneSize(Vdp2Regs->PLSZ & 0x3, &planew, &planeh);
         AddString(outstring, "Plane Size = %dH x %dV\r\n", planew, planeh);

         // Pattern Name Control stuff
         if (patterndatasize == 2)
         {
            AddString(outstring, "Pattern Name data size = 2 words\r\n");
         }
         else
         {
            AddString(outstring, "Pattern Name data size = 1 word\r\n");
            AddString(outstring, "Character Number Supplement bit = %d\r\n", (Vdp2Regs->PNCN0 >> 14) & 0x1);
            AddString(outstring, "Special Priority bit = %d\r\n", (supplementdata >> 9) & 0x1);
            AddString(outstring, "Special Color Calculation bit = %d\r\n", (supplementdata >> 8) & 0x1);
            AddString(outstring, "Supplementary Palette number = %d\r\n", (supplementdata >> 5) & 0x7);
            AddString(outstring, "Supplementary Color number = %d\r\n", supplementdata & 0x1f);
         }

         map[0] = Vdp2Regs->MPABN0 & 0xFF;
         map[1] = Vdp2Regs->MPABN0 >> 8;
         map[2] = Vdp2Regs->MPCDN0 & 0xFF;
         map[3] = Vdp2Regs->MPCDN0 >> 8;
         outstring = AddMapInfo(outstring, patternwh, Vdp2Regs->PNCN0, Vdp2Regs->PLSZ & 0x3, (Vdp2Regs->MPOFN & 0x7) << 6, 4, map);

      }
     // NBG0
     layer = NBG0;
/*
     // Screen scroll values
     AddString(outstring, "Screen Scroll x = %f, y = %f\r\n", (float)(reg->getLong(0x70) & 0x7FFFF00) / 65536, (float)(reg->getLong(0x74) & 0x7FFFF00) / 65536);
*/
     outstring = AddScrollString(outstring, Vdp2Regs->SCXIN0, Vdp2Regs->SCXDN0, Vdp2Regs->SCYIN0, Vdp2Regs->SCYDN0);

     // Coordinate Increments
     /* ST-058-R2 ch.5.2 p.126 : ce que l'on imprime ici est 1/increment,
      * c'est-a-dire le FACTEUR DE ZOOM (pixels ecran par pixel de motif), pas
      * l'increment de coordonnee lui-meme. Le libelle disait l'inverse. */
     outstring = AddZoomString(outstring, Vdp2Regs->ZMXN0.all, Vdp2Regs->ZMYN0.all);

     // Reduction Enable
     switch (Vdp2Regs->ZMCTL & 3)
     {
        case 1:
           AddString(outstring, "Horizontal Reduction = 1/2\r\n");
           break;
        case 2:
        case 3:
           AddString(outstring, "Horizontal Reduction = 1/4\r\n");
           break;
        default: break;
     }

     outstring = AddLineScrollString(outstring, lineVerticalScrollReg, Vdp2Regs->LSTA0.all);
      AddString(outstring, "Gradation Calculation %s\n", (getBlur(Vdp2Regs, layer)==0)?"Disabled":"Enabled");
      // Window Control
      outstring = AddWindowInfoString(outstring, Vdp2Regs->WCTLA, 0);

      // Shadow Control here

      // Color Ram Address Offset
      AddString(outstring, "Color Ram Address Offset = %X\r\n", (Vdp2Regs->CRAOFA & 0x7) << 8);

      // Special Priority Mode
      outstring = AddSpecialPriorityInfo(outstring, Vdp2Regs->SFPRMD);

      // Color Calculation Control here

      // Special Color Calculation Mode here

      // Priority Number
      AddString(outstring, "Priority = %d\r\n", Vdp2Regs->PRINA & 0x7);

      AddString(outstring, "Line Color Screen insertion = %d\r\n", (Vdp2Regs->LNCLEN & 0x1)!=0);

      // Color Calculation
      outstring = AddColorCalcInfo(outstring, 0x0001, 0x0002, Vdp2Regs->CCRNA & 0x1F, Vdp2Regs->SFCCMD & 0x3);

      // Color Offset
      outstring = AddColorOffsetInfo(outstring, 0x0001);


   }
   else
   {
      // disabled
      *isenabled = 0;
   }
}
void Vdp2DebugStatsRBG1(char *outstring, int *isenabled)
{
   int patternwh=(Vdp2Regs->CHCTLA & 0x1) + 1;
   u8 map[16];
   int layer;

   if (Vdp2Regs->BGON & 0x20)
   {
      // enabled
      *isenabled = 1;

     if (Vdp2Regs->KTCTL & 0x100)
     {
        AddString(outstring, "Coefficient Table Enabled(Mode %d)\r\n", (Vdp2Regs->KTCTL >> 10) & 0x3);
     }

      outstring = AddDisplayConflict(outstring, RBG1, Vdp2Regs->PRINA & 0x7);

      // Mosaic
      outstring = AddMosaicString(outstring, 0x1);

      // BPP
      outstring = AddBppString(outstring, (Vdp2Regs->CHCTLA & 0x70) >> 4);

       // Tile
       int patterndatasize;
       u16 supplementdata=Vdp2Regs->PNCN0 & 0x3FF;
       int planew=0, planeh=0;

       if(Vdp2Regs->PNCN0 & 0x8000)
          patterndatasize = 1;
       else
          patterndatasize = 2;

       AddString(outstring, "Tile(%dH x %dV)\r\n", patternwh, patternwh);

       /* RBG1 est toujours dessine avec le parametre de rotation B
        * (ST-058-R2 6.1) : sa taille de plan est RBPLSZ (PLSZ bits
        * 13-12), pas N0PLSZ. */
       Vdp2GetPlaneSize((Vdp2Regs->PLSZ >> 12) & 0x3, &planew, &planeh);
       AddString(outstring, "Plane Size = %dH x %dV\r\n", planew, planeh);

       // Pattern Name Control stuff
       if (patterndatasize == 2)
       {
          AddString(outstring, "Pattern Name data size = 2 words\r\n");
       }
       else
       {
          AddString(outstring, "Pattern Name data size = 1 word\r\n");
          AddString(outstring, "Character Number Supplement bit = %d\r\n", (Vdp2Regs->PNCN0 >> 14) & 0x1);
          AddString(outstring, "Special Priority bit = %d\r\n", (supplementdata >> 9) & 0x1);
          AddString(outstring, "Special Color Calculation bit = %d\r\n", (supplementdata >> 8) & 0x1);
          AddString(outstring, "Supplementary Palette number = %d\r\n", (supplementdata >> 5) & 0x7);
          AddString(outstring, "Supplementary Color number = %d\r\n", supplementdata & 0x1f);
       }

       /* RBG1 utilise le parametre de rotation B : 16 plans MPABRB..MPOPRB,
        * taille RBPLSZ et decalage RBMP (MPOFR bits 6-4), comme le
        * renderer (Vdp2DrawRBG1_part, vidcs.c). L'ancien code affichait les
        * plans A-D de NBG0 (MPABN0/MPCDN0, N0PLSZ, MPOFN), sans rapport
        * avec ce que RBG1 lit. Le pattern name, lui, est bien celui de
        * NBG0 (PNCN0). */
       {
          const u16 r[8] = { Vdp2Regs->MPABRB, Vdp2Regs->MPCDRB, Vdp2Regs->MPEFRB, Vdp2Regs->MPGHRB,
                             Vdp2Regs->MPIJRB, Vdp2Regs->MPKLRB, Vdp2Regs->MPMNRB, Vdp2Regs->MPOPRB };
          int k;
          for (k = 0; k < 8; k++) { map[2 * k] = r[k] & 0xFF; map[2 * k + 1] = r[k] >> 8; }
       }
       outstring = AddMapInfo(outstring, patternwh, Vdp2Regs->PNCN0, (Vdp2Regs->PLSZ >> 12) & 0x3, (Vdp2Regs->MPOFR & 0x70) << 2, 16, map);


      layer = RBG1;
//         unsigned long mapOffsetReg=(readWord(reg, 0x3E) & 0x70) << 2;

       // RBG1

       // Map Planes A-P here

       // Rotation Parameter Read Control
       if (Vdp2Regs->RPRCTL & 0x400)
       {
          AddString(outstring, "Read KAst Parameter = TRUE\r\n");
       }
       else
       {
          AddString(outstring, "Read KAst Parameter = FALSE\r\n");
       }

       if (Vdp2Regs->RPRCTL & 0x200)
       {
          AddString(outstring, "Read Yst Parameter = TRUE\r\n");
       }
       else
       {
          AddString(outstring, "Read Yst Parameter = FALSE\r\n");
       }

       if (Vdp2Regs->RPRCTL & 0x100)
       {
          AddString(outstring, "Read Xst Parameter = TRUE\r\n");
       }
       else
       {
          AddString(outstring, "Read Xst Parameter = FALSE\r\n");
       }

       // Coefficient Table Control

       // Coefficient Table Address Offset

       // Screen Over Pattern Name(should this be moved?)

       // Rotation Parameter Table Address

      AddString(outstring, "Gradation Calculation %s\n", (getBlur(Vdp2Regs, layer)==0)?"Disabled":"Enabled");
      // Window Control
      outstring = AddWindowInfoString(outstring, Vdp2Regs->WCTLA, 0);

      // Shadow Control here

      // Color Ram Address Offset
      AddString(outstring, "Color Ram Address Offset = %X\r\n", (Vdp2Regs->CRAOFA & 0x7) << 8);

      // Special Priority Mode
      outstring = AddSpecialPriorityInfo(outstring, Vdp2Regs->SFPRMD);

      // Color Calculation Control here

      // Special Color Calculation Mode here

      // Priority Number
      AddString(outstring, "Priority = %d\r\n", Vdp2Regs->PRINA & 0x7);

      AddString(outstring, "Line Color Screen insertion = %d\r\n", (Vdp2Regs->LNCLEN & 0x1)!=0);

      // Color Calculation
      outstring = AddColorCalcInfo(outstring, 0x0001, 0x0002, Vdp2Regs->CCRNA & 0x1F, Vdp2Regs->SFCCMD & 0x3);

      // Color Offset
      outstring = AddColorOffsetInfo(outstring, 0x0001);


   }
   else
   {
      // disabled
      *isenabled = 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2DebugStatsNBG1(char *outstring, int *isenabled)
{
   u16 lineVerticalScrollReg = (Vdp2Regs->SCRCTL >> 8) & 0x3F;
   int isbitmap=Vdp2Regs->CHCTLA & 0x200;
   int patternwh=((Vdp2Regs->CHCTLA & 0x100) >> 8) + 1;
   u8 map[4];

   if (Vdp2Regs->BGON & 0x2)
   {
      // enabled
      *isenabled = 1;

      outstring = AddDisplayConflict(outstring, NBG1, (Vdp2Regs->PRINA >> 8) & 0x7);

      // Mosaic
      outstring = AddMosaicString(outstring, 0x2);

      // BPP
      outstring = AddBppString(outstring, (Vdp2Regs->CHCTLA & 0x3000) >> 12);

      // Bitmap or Tile mode?
      if (isbitmap)
      {
         // Bitmap
         outstring = AddBitmapInfoString(outstring, (Vdp2Regs->CHCTLA & 0xC00) >> 10, Vdp2Regs->BMPNA >> 8, Vdp2Regs->MPOFN >> 4);
      }
      else
      {
         int patterndatasize;
         u16 supplementdata=Vdp2Regs->PNCN1 & 0x3FF;
         int planew=0, planeh=0;

         if(Vdp2Regs->PNCN1 & 0x8000)
           patterndatasize = 1;
         else
           patterndatasize = 2;

         // Tile
         AddString(outstring, "Tile(%dH x %dV)\r\n", patternwh, patternwh);

         Vdp2GetPlaneSize((Vdp2Regs->PLSZ & 0xC) >> 2, &planew, &planeh);
         AddString(outstring, "Plane Size = %dH x %dV\r\n", planew, planeh);

         // Pattern Name Control stuff
         if (patterndatasize == 2)
         {
            AddString(outstring, "Pattern Name data size = 2 words\r\n");
         }
         else
         {
            AddString(outstring, "Pattern Name data size = 1 word\r\n");
            AddString(outstring, "Character Number Supplement bit = %d\r\n", (Vdp2Regs->PNCN1 >> 14) & 0x1);
            AddString(outstring, "Special Priority bit = %d\r\n", (supplementdata >> 9) & 0x1);
            AddString(outstring, "Special Color Calculation bit = %d\r\n", (supplementdata >> 8) & 0x1);
            AddString(outstring, "Supplementary Palette number = %d\r\n", (supplementdata >> 5) & 0x7);
            AddString(outstring, "Supplementary Color number = %d\r\n", supplementdata & 0x1f);
         }

         map[0] = Vdp2Regs->MPABN1 & 0xFF;
         map[1] = Vdp2Regs->MPABN1 >> 8;
         map[2] = Vdp2Regs->MPCDN1 & 0xFF;
         map[3] = Vdp2Regs->MPCDN1 >> 8;
         outstring = AddMapInfo(outstring, patternwh, Vdp2Regs->PNCN1, (Vdp2Regs->PLSZ & 0xC) >> 2, (Vdp2Regs->MPOFN & 0x70) << 2, 4, map);

/*
         // Figure out Cell start address
         switch(patterndatasize)
         {
            case 1:
            {
               tmp = readWord(vram, addr);

               switch(auxMode)
               {
                  case 0:
                     switch(patternwh)
                     {
                        case 1:
                           charAddr = (tmp & 0x3FF) | ((supplementdata & 0x1F) << 10);
                           break;
                        case 2:
                           charAddr = ((tmp & 0x3FF) << 2) |  (supplementdata & 0x3) | ((supplementdata & 0x1C) << 10);
                           break;
                     }
                     break;
                  case 1:
                     switch(patternwh)
                     {
                        case 1:
                           charAddr = (tmp & 0xFFF) | ((supplementdata & 0x1C) << 10);
                           break;
                        case 4:
                           charAddr = ((tmp & 0xFFF) << 2) |  (supplementdata & 0x3) | ((supplementdata & 0x10) << 10);
                           break;
                     }
                     break;
               }
               break;
            }
            case 2:
            {
               unsigned short tmp1 = readWord(vram, addr);
               unsigned short tmp2 = readWord(vram, addr+2);

               charAddr = tmp2 & 0x7FFF;
               break;
            }
         }
         if (!(readWord(reg, 0x6) & 0x8000))
            charAddr &= 0x3FFF;

         charAddr *= 0x20; // selon Runik

         AddString(outstring, "Cell Data Address = %X\r\n", charAddr);
*/
      }

/*
      // Screen scroll values
      AddString(outstring, "Screen Scroll x = %f, y = %f\r\n", (float)(reg->getLong(0x80) & 0x7FFFF00) / 65536, (float)(reg->getLong(0x84) & 0x7FFFF00) / 65536);
*/
         outstring = AddScrollString(outstring, Vdp2Regs->SCXIN1, Vdp2Regs->SCXDN1, Vdp2Regs->SCYIN1, Vdp2Regs->SCYDN1);

      // Coordinate Increments
      /* ST-058-R2 ch.5.2 p.126 : valeur imprimee = 1/increment, soit le
       * facteur de zoom. Le libelle disait "Coordinate Increments", l'inverse.
       * Et l'axe Y lisait ZMXN1 au lieu de ZMYN1 : NBG1 affichait toujours le
       * meme zoom sur les deux axes, quel que soit ZMYN1. */
      outstring = AddZoomString(outstring, Vdp2Regs->ZMXN1.all, Vdp2Regs->ZMYN1.all);

      // Reduction Enable
      switch ((Vdp2Regs->ZMCTL >> 8) & 3)
      {
         case 1:
            AddString(outstring, "Horizontal Reduction = 1/2\r\n");
            break;
         case 2:
         case 3:
            AddString(outstring, "Horizontal Reduction = 1/4\r\n");
            break;
         default: break;
      }

      outstring = AddLineScrollString(outstring, lineVerticalScrollReg, Vdp2Regs->LSTA1.all);
      AddString(outstring, "Gradation Calculation %s\n", (getBlur(Vdp2Regs, NBG1)==0)?"Disabled":"Enabled");
      // Window Control
      outstring = AddWindowInfoString(outstring, Vdp2Regs->WCTLA >> 8, 0);

      // Shadow Control here

      // Color Ram Address Offset
      AddString(outstring, "Color Ram Address Offset = %X\r\n", (Vdp2Regs->CRAOFA & 0x70) << 4);

      // Special Priority Mode
      outstring = AddSpecialPriorityInfo(outstring, Vdp2Regs->SFPRMD >> 2);

      // Color Calculation Control here

      // Special Color Calculation Mode here

      // Priority Number
      AddString(outstring, "Priority = %d\r\n", (Vdp2Regs->PRINA >> 8) & 0x7);

      AddString(outstring, "Line Color Screen insertion = %d\r\n", (Vdp2Regs->LNCLEN & 0x2)!=0);

      // Color Calculation
      outstring = AddColorCalcInfo(outstring, 0x0002, 0x0004, (Vdp2Regs->CCRNA >> 8) & 0x1F, (Vdp2Regs->SFCCMD >> 2) & 0x3);

      // Color Offset
      outstring = AddColorOffsetInfo(outstring, 0x0002);

   }
   else
     // disabled
     *isenabled = 0;
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2DebugStatsNBG2(char *outstring, int *isenabled)
{
   u8 map[4];

   if (Vdp2Regs->BGON & 0x4)
   {
      int patterndatasize;
      u16 supplementdata=Vdp2Regs->PNCN2 & 0x3FF;
      int planew=0, planeh=0;
      int patternwh=(Vdp2Regs->CHCTLB & 0x1) + 1;

      // enabled
      *isenabled = 1;

      outstring = AddDisplayConflict(outstring, NBG2, Vdp2Regs->PRINB & 0x7);

      // Mosaic
      outstring = AddMosaicString(outstring, 0x4);

      // BPP
      outstring = AddBppString(outstring, (Vdp2Regs->CHCTLB & 0x2) >> 1);

      if(Vdp2Regs->PNCN2 & 0x8000)
         patterndatasize = 1;
      else
         patterndatasize = 2;

      AddString(outstring, "Tile(%dH x %dV)\r\n", patternwh, patternwh);

      Vdp2GetPlaneSize((Vdp2Regs->PLSZ & 0x30) >> 4, &planew, &planeh);
      AddString(outstring, "Plane Size = %dH x %dV\r\n", planew, planeh);

      // Pattern Name Control stuff
      if (patterndatasize == 2)
      {
         AddString(outstring, "Pattern Name data size = 2 words\r\n");
      }
      else
      {
         AddString(outstring, "Pattern Name data size = 1 word\r\n");
         AddString(outstring, "Character Number Supplement bit = %d\r\n", (Vdp2Regs->PNCN2 >> 14) & 0x1);
         AddString(outstring, "Special Priority bit = %d\r\n", (supplementdata >> 9) & 0x1);
         AddString(outstring, "Special Color Calculation bit = %d\r\n", (supplementdata >> 8) & 0x1);
         AddString(outstring, "Supplementary Palette number = %d\r\n", (supplementdata >> 5) & 0x7);
         AddString(outstring, "Supplementary Color number = %d\r\n", supplementdata & 0x1f);
      }

      map[0] = Vdp2Regs->MPABN2 & 0xFF;
      map[1] = Vdp2Regs->MPABN2 >> 8;
      map[2] = Vdp2Regs->MPCDN2 & 0xFF;
      map[3] = Vdp2Regs->MPCDN2 >> 8;
      outstring = AddMapInfo(outstring, patternwh, Vdp2Regs->PNCN2, (Vdp2Regs->PLSZ >> 4) & 0x3, (Vdp2Regs->MPOFN & 0x700) >> 2, 4, map);
/*
      // Figure out Cell start address
      switch(patterndatasize)
      {
         case 1:
         {
            tmp = readWord(vram, addr);

            switch(auxMode)
            {
               case 0:
                  switch(patternwh)
                  {
                     case 1:
                        charAddr = (tmp & 0x3FF) | ((supplementdata & 0x1F) << 10);
                        break;
                     case 2:
                        charAddr = ((tmp & 0x3FF) << 2) |  (supplementdata & 0x3) | ((supplementdata & 0x1C) << 10);
                        break;
                  }
                  break;
               case 1:
                  switch(patternwh)
                  {
                     case 1:
                        charAddr = (tmp & 0xFFF) | ((supplementdata & 0x1C) << 10);
                        break;
                     case 4:
                        charAddr = ((tmp & 0xFFF) << 2) |  (supplementdata & 0x3) | ((supplementdata & 0x10) << 10);
                        break;
                  }
                  break;
            }
            break;
         }
         case 2:
         {
            unsigned short tmp1 = readWord(vram, addr);
            unsigned short tmp2 = readWord(vram, addr+2);

            charAddr = tmp2 & 0x7FFF;
            break;
         }
      }
      if (!(readWord(reg, 0x6) & 0x8000)) charAddr &= 0x3FFF;

      charAddr *= 0x20; // selon Runik

      AddString(outstring, "Cell Data Address = %X\r\n", charAddr);
*/
      // Screen scroll values
      outstring = AddScrollString(outstring, Vdp2Regs->SCXN2, 0, Vdp2Regs->SCYN2, 0);

      AddString(outstring, "Gradation Calculation %s\n", (getBlur(Vdp2Regs, NBG2)==0)?"Disabled":"Enabled");
      // Window Control
      outstring = AddWindowInfoString(outstring, Vdp2Regs->WCTLB, 0);

      // Shadow Control here

      // Color Ram Address Offset
      AddString(outstring, "Color Ram Address Offset = %X\r\n", Vdp2Regs->CRAOFA & 0x700);

      // Special Priority Mode
      outstring = AddSpecialPriorityInfo(outstring, Vdp2Regs->SFPRMD >> 4);

      // Color Calculation Control here

      // Special Color Calculation Mode here

      // Priority Number
      AddString(outstring, "Priority = %d\r\n", Vdp2Regs->PRINB & 0x7);

      AddString(outstring, "Line Color Screen insertion = %d\r\n", (Vdp2Regs->LNCLEN & 0x4)!=0);

      // Color Calculation
      outstring = AddColorCalcInfo(outstring, 0x0004, 0x0005, Vdp2Regs->CCRNB & 0x1F, (Vdp2Regs->SFCCMD >> 4) & 0x3);

      // Color Offset
      outstring = AddColorOffsetInfo(outstring, 0x0004);

   }
   else
   {
     // disabled
     *isenabled = 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2DebugStatsNBG3(char *outstring, int *isenabled)
{
   u8 map[4];

   if (Vdp2Regs->BGON & 0x8)
   {
      int patterndatasize;
      u16 supplementdata=Vdp2Regs->PNCN3 & 0x3FF;
      int planew=0, planeh=0;
      int patternwh=((Vdp2Regs->CHCTLB & 0x10) >> 4) + 1;

      // enabled
      *isenabled = 1;

      outstring = AddDisplayConflict(outstring, NBG3, (Vdp2Regs->PRINB >> 8) & 0x7);

      // Mosaic
      outstring = AddMosaicString(outstring, 0x8);

      // BPP
      outstring = AddBppString(outstring, (Vdp2Regs->CHCTLB & 0x20) >> 5);

      if(Vdp2Regs->PNCN3 & 0x8000)
         patterndatasize = 1;
      else
         patterndatasize = 2;

      AddString(outstring, "Tile(%dH x %dV)\r\n", patternwh, patternwh);

      Vdp2GetPlaneSize((Vdp2Regs->PLSZ & 0xC0) >> 6, &planew, &planeh);
      AddString(outstring, "Plane Size = %dH x %dV\r\n", planew, planeh);

      // Pattern Name Control stuff
      if (patterndatasize == 2)
      {
         AddString(outstring, "Pattern Name data size = 2 words\r\n");
      }
      else
      {
         AddString(outstring, "Pattern Name data size = 1 word\r\n");
         AddString(outstring, "Character Number Supplement bit = %d\r\n", (Vdp2Regs->PNCN3 >> 14) & 0x1);
         AddString(outstring, "Special Priority bit = %d\r\n", (supplementdata >> 9) & 0x1);
         AddString(outstring, "Special Color Calculation bit = %d\r\n", (supplementdata >> 8) & 0x1);
         AddString(outstring, "Supplementary Palette number = %d\r\n", (supplementdata >> 5) & 0x7);
         AddString(outstring, "Supplementary Color number = %d\r\n", supplementdata & 0x1f);
      }

      map[0] = Vdp2Regs->MPABN3 & 0xFF;
      map[1] = Vdp2Regs->MPABN3 >> 8;
      map[2] = Vdp2Regs->MPCDN3 & 0xFF;
      map[3] = Vdp2Regs->MPCDN3 >> 8;
      outstring = AddMapInfo(outstring, patternwh, Vdp2Regs->PNCN3, (Vdp2Regs->PLSZ & 0xC0) >> 6, (Vdp2Regs->MPOFN & 0x7000) >> 6, 4, map);

/*
      // Figure out Cell start address
      switch(patterndatasize)
      {
         case 1:
         {
            tmp = readWord(vram, addr);

            switch(auxMode)
            {
               case 0:
                  switch(patternwh)
                  {
                     case 1:
                        charAddr = (tmp & 0x3FF) | ((supplementdata & 0x1F) << 10);
                        break;
                     case 2:
                        charAddr = ((tmp & 0x3FF) << 2) |  (supplementdata & 0x3) | ((supplementdata & 0x1C) << 10);
                        break;
                  }
                  break;
               case 1:
                  switch(patternwh)
                  {
                     case 1:
                        charAddr = (tmp & 0xFFF) | ((supplementdata & 0x1C) << 10);
                        break;
                     case 4:
                        charAddr = ((tmp & 0xFFF) << 2) |  (supplementdata & 0x3) | ((supplementdata & 0x10) << 10);
                        break;
                  }
                  break;
            }
            break;
         }
         case 2:
         {
            unsigned short tmp1 = readWord(vram, addr);
            unsigned short tmp2 = readWord(vram, addr+2);

            charAddr = tmp2 & 0x7FFF;
            break;
         }
      }

      if (!(readWord(reg, 0x6) & 0x8000))
         charAddr &= 0x3FFF;

      charAddr *= 0x20; // selon Runik

      AddString(outstring, "Cell Data Address = %X\r\n", charAddr);
*/
      // Screen scroll values
      outstring = AddScrollString(outstring, Vdp2Regs->SCXN3, 0, Vdp2Regs->SCYN3, 0);

      AddString(outstring, "Gradation Calculation %s\n", (getBlur(Vdp2Regs, NBG3)==0)?"Disabled":"Enabled");
      // Window Control
      outstring = AddWindowInfoString(outstring, Vdp2Regs->WCTLB >> 8, 0);

      // Shadow Control here

      // Color Ram Address Offset
      AddString(outstring, "Color Ram Address Offset = %X\r\n", Vdp2Regs->CRAOFA & 0x7000);

      // Special Priority Mode
      outstring = AddSpecialPriorityInfo(outstring, Vdp2Regs->SFPRMD >> 6);

      // Special Color Calculation Mode here

      // Priority Number
      AddString(outstring, "Priority = %d\r\n", (Vdp2Regs->PRINB >> 8) & 0x7);

      AddString(outstring, "Line Color Screen insertion = %d\r\n", (Vdp2Regs->LNCLEN & 0x8)!=0);

      // Color Calculation
      outstring = AddColorCalcInfo(outstring, 0x0008, 0x0006, (Vdp2Regs->CCRNB >> 8) & 0x1F, (Vdp2Regs->SFCCMD >> 6) & 0x3);

      // Color Offset
      outstring = AddColorOffsetInfo(outstring, 0x0008);

   }
   else
   {
      // disabled
      *isenabled = 0;
   }
}

//////////////////////////////////////////////////////////////////////////////

void Vdp2DebugStatsSprite(char *outstring, int *isenabled)
{
  u8 *sprprilist = (u8 *)&Vdp2Regs->PRISA;
  u8 *sprccrlist = (u8 *)&Vdp2Regs->CCRSA;

  *isenabled = 1;

  // Sprite stuff
  AddString(outstring, "Sprite Type = %X\r\n", Vdp2Regs->SPCTL & 0xF);
  AddString(outstring, "Screen Mode TVM = ");
  switch (Vdp1Regs->TVMR & 0x7) {
    case 0:
      AddString(outstring, "Normal\n");
      break;
    case 1:
      AddString(outstring, "High Resolution\n");
      break;
    case 2:
      AddString(outstring, "Rotation 16\n");
      break;
    case 3:
      AddString(outstring, "Rotation 8\n");
      break;
    case 4:
      AddString(outstring, "HDTV\n");
      break;
    default:
      AddString(outstring, "Prohibited 0x%x\n",Vdp1Regs->TVMR & 0x7);
      break;
  }
  AddString(outstring, "VDP1 Framebuffer Data Format = %s\r\n", Vdp2Regs->SPCTL & 0x20 ? "RGB and palette" : "Palette only");
  AddString(outstring, "Erased Area: EWLR 0x%x EWRR 0x%x\n", Vdp1Regs->EWLR, Vdp1Regs->EWRR);
  {
    int shift = ((Vdp1Regs->TVMR & 0x1) == 1)?4:3;
    int limits[4] = {0};
    limits[0] = ((Vdp1Regs->EWLR>>9)&0x3F)<<shift;
    limits[1] = ((Vdp1Regs->EWLR)&0x1FF); //TODO: manage double interlace
    limits[2] = (((Vdp1Regs->EWRR>>9)&0x7F)<<shift) - 1;
    limits[3] = ((Vdp1Regs->EWRR)&0x1FF); //TODO: manage double interlace

    //Prohibited value - Example Quake first screens
    if ((limits[2] == -1)||(limits[3] == 0)) {
      AddString(outstring, "  Prohibited value\n");
    }

    AddString(outstring, "  Erase from (%d,%d) to (%d,%d)\n", limits[0], limits[1], limits[2], limits[3]);
    if ((limits[0]>=limits[2])||(limits[1]>limits[3])) {
      AddString(outstring, "    Invalid values\n");
    }
  }
  if (Vdp2Regs->SDCTL & 0x100)
  {
     AddString(outstring, "Transparent Shadow Enabled\r\n");
  }

  if (Vdp2Regs->SPCTL & 0x10)
  {
     AddString(outstring, "Sprite Window Enabled\r\n");
  }
  /* La gradation ne depend pas de la fenetre sprite : elle n'etait
   * affichee que si SPWINEN valait 1. */
  AddString(outstring, "Sprite Gradation Calculation %s\r\n", (getBlur(Vdp2Regs, SPRITE)==0)?"Disabled":"Enabled");

  if (Vdp2Regs->LNCLEN & 0x20)
  {
    AddString(outstring, "Line Color Screen insertion enabled\n");
  }

  outstring = AddWindowInfoString(outstring, Vdp2Regs->WCTLC >> 8, 1);

  /* SPCAOS (CRAOFB bits 6-4) en index CRAM, comme les autres ecrans
   * ("Color Ram Address Offset = 100" pour 1) ; il s'affichait brut. */
  AddString(outstring, "Color Ram Address Offset = %X\r\n", ((Vdp2Regs->CRAOFB >> 4) & 0x7) << 8);
  if (Vdp2Regs->CCCTL & 0x40)
  {
     AddString(outstring, "Color Calculation Enabled\r\n");

     /* BOKEN = bit 15, BOKN = bits 14-12 (0 = sprite) ; l'ancien test
      * portait sur les bits 10-8 (EXCCEN/CCRTMD/CCMD). */
     if ((Vdp2Regs->CCCTL & 0x8000) && ((Vdp2Regs->CCCTL >> 12) & 0x7) == 0)
     {
        AddString(outstring, "Gradation Calculation Enabled\r\n");
     }
     if (Vdp2Regs->CCCTL & 0x0400)
     {
        AddString(outstring, "Extended Color Calculation Enabled\r\n");
     }

     AddString(outstring, "Color Calculation Condition = ");

     switch ((Vdp2Regs->SPCTL >> 12) & 0x3)
     {
         case 0:
            AddString(outstring, "Priority <= CC Condition Number");
            break;
         case 1:
            AddString(outstring, "Priority == CC Condition Number");
            break;
         case 2:
            AddString(outstring, "Priority >= CC Condition Number");
            break;
         case 3:
            AddString(outstring, "Color Data MSB");
            break;
         default: break;
     }
     AddString(outstring, "\r\n");

     if (((Vdp2Regs->SPCTL >> 12) & 0x3) != 0x3)
     {
        AddString(outstring, "Color Calculation Condition Number = %d\r\n", (Vdp2Regs->SPCTL >> 8) & 0x7);
     }

     for (int i = 0; i < 8; i++)
     {
#ifdef WORDS_BIGENDIAN
        u8 ratio = sprccrlist[i ^ 1] & 0x1F;
#else
        u8 ratio = sprccrlist[i] & 0x1F;
#endif
        AddString(outstring, "Color Calculation Ratio %d = %d:%d\r\n", i, 31 - ratio, 1 + ratio);
     }
  }

  for (int i = 0; i < 8; i++)
  {
#ifdef WORDS_BIGENDIAN
     int priority = sprprilist[i ^ 1] & 0x7;
#else
     int priority = sprprilist[i] & 0x7;
#endif
     AddString(outstring, "Priority %d = %d\r\n", i, priority);
  }

  outstring = AddColorOffsetInfo(outstring, 0x0040);
}

void Vdp2DebugStatsGeneral(char *outstring, int *isenabled)
{
   int i;

   AddString(outstring, "RAMCTL 0x%x\r\n", Vdp2Regs->RAMCTL);
   *isenabled = 1;

    AddString(outstring, "Screen is %s\n", ((Vdp2Regs->TVMD & 0x8000)!=0)?"enabled":"disabled");
    // TVMD stuff
    AddString(outstring, "Border Color Mode = %s\r\n", Vdp2Regs->TVMD & 0x100 ? "Back screen" : "Black");

    AddString(outstring, "Display Resolution = ");
    switch (Vdp2Regs->TVMD & 0x7)
    {
       case 0:
       case 4:
          AddString(outstring, "320");
          break;
       case 1:
       case 5:
          AddString(outstring, "352");
          break;
       case 2:
       case 6:
          AddString(outstring, "640");
          break;
       case 3:
       case 7:
          AddString(outstring, "704");
          break;
       default:
          AddString(outstring, "Invalid");
          break;
    }

    AddString(outstring, " x ");

    switch ((Vdp2Regs->TVMD >> 4) & 0x3)
    {
       case 0:
          AddString(outstring, "224");
          break;
       case 1:
          AddString(outstring, "240");
          break;
       case 2:
          if (yabsys.IsPal){
            AddString(outstring, "256");
          } else {
            AddString(outstring, "224 - due to invalid");
          }
          break;
       default:
          AddString(outstring, "Invalid");
          break;
    }

    if (Vdp2Regs->TVSTAT & 0x1)
    {
       AddString(outstring, "(PAL)\r\n");
    }
    else
    {
       AddString(outstring, "(NTSC)\r\n");
    }

    AddString(outstring, "Interlace Mode = ");
    switch ((Vdp2Regs->TVMD >> 6) & 0x3)
    {
       case 0:
          AddString(outstring, "Non-Interlace\r\n");
          break;
       case 2:
          AddString(outstring, "Single-Density Interlace\r\n");
          break;
       case 3:
          AddString(outstring, "Double-Density Interlace\r\n");
          break;
       default:
          AddString(outstring, "Invalid\r\n");
          break;
    }

    // Latch stuff
    AddString(outstring, "Latches HV counter when %s\r\n", Vdp2Regs->EXTEN & 0x200 ? "external signal triggers it" : "external latch flag is read");
    /* EXTEN bit 8 = EXSYEN (synchro externe autorisee) ; la presence du
     * signal est TVSTAT bit 8 (EXSYFG). L'ancien libelle confondait les
     * deux. */
    if (Vdp2Regs->EXTEN & 0x100)
    {
       AddString(outstring, "External Sync enabled%s\r\n", (Vdp2Regs->TVSTAT & 0x100) ? " (signal present)" : "");
    }

    // Screen status stuff
    if (Vdp2Regs->TVSTAT & 0x200)
    {
       AddString(outstring, "HV is latched\r\n");
    }

    if (Vdp2Regs->TVSTAT & 0x4)
    {
       AddString(outstring, "During H-Blank\r\n");
    }

    if (Vdp2Regs->TVSTAT & 0x8)
    {
       AddString(outstring, "During V-Blank\r\n");
    }

    if ((Vdp2Regs->TVMD >> 6) & 0x2)
    {
       AddString(outstring, "During %s Field\r\n", Vdp2Regs->TVSTAT & 0x2 ? "Odd" : "Even");
    }

    AddString(outstring, "H Counter = %d\r\n", Vdp2Regs->HCNT);
    AddString(outstring, "V Counter = %d\r\n", Vdp2Regs->VCNT);
    AddString(outstring, "\r\n");

    // Line color screen stuff
    AddString(outstring, "Line Color Screen Stuff\r\n");
    AddString(outstring, "-----------------------\r\n");
    AddString(outstring, "Mode = %s\r\n", Vdp2Regs->LCTA.part.U & 0x8000 ? "Color per line" : "Single color");
    AddString(outstring, "Address = %08lX\r\n", 0x05E00000UL | ((Vdp2Regs->LCTA.all & 0x7FFFFUL) * 2));
    /* LCCCEN = CCCTL bit 5 ; CCRTMD (bit 9) ne change que l'origine du
     * taux, pas l'activation, qui n'etait plus affichee des qu'il valait 1. */
    AddString(outstring, "Color Calculation %s\r\n", (Vdp2Regs->CCCTL & 0x0020) ? "Enabled" : "Disabled");
    {
      u16 calcratio = Vdp2Regs->CCRLB & 0x1F;
      AddString(outstring, "Color Calculation Ratio = %d:%d\r\n", 31 - calcratio, 1 + calcratio);
    }
    AddString(outstring, "\r\n");

    // Back screen stuff
    AddString(outstring, "Back Screen Stuff\r\n");
    AddString(outstring, "-----------------\r\n");
    AddString(outstring, "Mode = %s\r\n", Vdp2Regs->BKTAU & 0x8000 ? "Color per line" : "Single color");
    AddString(outstring, "Address = %08X\r\n", 0x05E00000 | (((Vdp2Regs->BKTAU & 0x7) << 16)  | Vdp2Regs->BKTAL) * 2);
    outstring = AddColorOffsetInfo(outstring, 0x0020);
    if ((Vdp2Regs->CCCTL & 0x200) != 0) {
       AddString(outstring, "Color calculation per second screen\r\n");
    }
    {
      u16 calcratio = (Vdp2Regs->CCRLB>>8) & 0x1F;
      AddString(outstring, "Color Calculation Ratio = %d:%d\r\n", 31 - calcratio, 1 + calcratio);
    }
    AddString(outstring, "\r\n");

    // Cycle patterns here
    AddString(outstring, "Cycle Pattern\r\n");
    AddString(outstring, "-----------------\r\n");
    AddString(outstring, "A0 = %08X\r\n", (Vdp2Regs->CYCA0L << 16) | (Vdp2Regs->CYCA0U));
    AddString(outstring, "A1 = %08X\r\n", (Vdp2Regs->CYCA1L << 16) | (Vdp2Regs->CYCA1U));
    AddString(outstring, "B0 = %08X\r\n", (Vdp2Regs->CYCB0L << 16) | (Vdp2Regs->CYCB0U));
    AddString(outstring, "B1 = %08X\r\n", (Vdp2Regs->CYCB1L << 16) | (Vdp2Regs->CYCB1U));
    AddString(outstring, "\r\n");

    // Cycle patterns here
    AddString(outstring, "Color RAM\r\n");
    AddString(outstring, "-----------------\r\n");
    AddString(outstring, "Color RAM Mode = %X\r\n", (Vdp2Regs->RAMCTL >> 12) & 0x3);

}

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL DoNothing(UNUSED void *info, u32 pixel)
{
   return pixel;
}

//////////////////////////////////////////////////////////////////////////////

static void ClearTextureToColor(u32 *texture, u32 color, int w, int h)
{
   int i;

   for (i = 0; i < (w * h); i++)
      texture[i] = color;
}

//////////////////////////////////////////////////////////////////////////////

pixel_t *Vdp2DebugTexture(u32 screen, int * w, int * h)
{

   pixel_t * bitmap = VIDCore->getVdp2ScreenExtract(screen, w, h);

   return bitmap;
}

//////////////////////////////////////////////////////////////////////////////
//  Vdp2DebugPixelSource -- index de couleur d'un pixel du VDP2 Viewer
//
//  Voir vdp2debug.h. Le decodage suit les memes regles que le renderer
//  (vidshared.[ch] : CalcPlaneAddr/ReadPatternData, vidcs.c :
//  Vdp2PatternAddrPos, Vdp2GenLineinfo, decodeurs de sprite de
//  common_glshader.c), elles-memes tirees de ST-058-R2. Deux differences
//  voulues avec le chemin bitmap/zoom du renderer :
//    - les fractions de scroll (SCXDNx/SCYDNx) et de line scroll (bits 15-8)
//      sont conservees, alors que vidcs.c ne garde que la partie entiere ;
//    - l'index est toujours replie sur la taille de la CRAM (11 ou 10 bits).
//  Le VDP2 Viewer compare ensuite la couleur CRAM obtenue a celle affichee
//  et signale tout ecart : un ecart designe soit l'une de ces differences,
//  soit une CRAM modifiee en cours de trame, soit un defaut du renderer.
//////////////////////////////////////////////////////////////////////////////

static INLINE u8 PixDbgByte(u32 addr)  { return T1ReadByte(Vdp2Ram, addr & 0x7FFFF); }
static INLINE u16 PixDbgWord(u32 addr) { return T1ReadWord(Vdp2Ram, addr & 0x7FFFE); }
static INLINE u32 PixDbgLong(u32 addr)
{
   return ((u32)PixDbgWord(addr) << 16) | PixDbgWord(addr + 2);
}

static INLINE int PixDbgCramWrap(const Vdp2 *regs, int idx)
{
   /* RAMCTL.CRMD = 2 : 1024 entrees de 32 bits ; sinon 2048 de 16 bits
    * (meme repli que Vdp2CramIndexWrap, vidcs.c). */
   return (((regs->RAMCTL >> 12) & 3) == 2) ? (idx & 0x3FF) : (idx & 0x7FF);
}

static void PixDbgNote(vdp2debugpixel_struct *out, const char *txt)
{
   size_t len = strlen(out->note);
   if (len + strlen(txt) + 3 >= sizeof(out->note)) return;
   if (len) { strcat(out->note, "; "); }
   strcat(out->note, txt);
}

/* Registres en vigueur sur la ligne d'affichage y (instantane H-blank). */
static const Vdp2 *PixDbgRegsForLine(int y, int ilace)
{
   int fl = y >> ilace;
   if (fl < 0) fl = 0;
   if (fl >= VDP2_LINE_SNAPSHOT_MAX) return Vdp2Regs;
   return &Vdp2Lines[fl];
}

/* Lit un point de 'colornumber' a l'index lineaire 'pos' (en points) depuis
 * 'base'. Retourne l'adresse lue dans *addr. */
static u32 PixDbgReadDot(u32 base, u32 pos, int colornumber, u32 *addr)
{
   switch (colornumber)
   {
      case 0: /* 16 couleurs : 2 points par octet, pair = quartet haut */
         *addr = base + (pos >> 1);
         return (pos & 1) ? (PixDbgByte(*addr) & 0xF) : (PixDbgByte(*addr) >> 4);
      case 1: /* 256 couleurs */
         *addr = base + pos;
         return PixDbgByte(*addr);
      case 2: /* 2048 couleurs (11 bits utiles) */
      case 3: /* RGB 32K */
         *addr = base + pos * 2;
         return PixDbgWord(*addr);
      default: /* RGB 16M */
         *addr = base + pos * 4;
         return PixDbgLong(*addr);
   }
}

static int PixDbgNBG(int n, int x, int y, vdp2debugpixel_struct *out)
{
   const int ilace = (((Vdp2Regs->TVMD >> 6) & 3) == 3) ? 1 : 0;
   const Vdp2 *R = PixDbgRegsForLine(y, ilace);
   int colornumber, isbitmap, bmsz = 0, chsz, plsz = 0, tpon;
   u16 pnc = 0, mpab = 0, mpcd = 0;
   u32 mapofs, coloroffset, bmppal = 0;
   u32 u, v;

   /* Mosaique (MZCTL bits 3-0 = N0..N3) : le point affiche est celui du
    * coin haut-gauche du bloc. */
   if (R->MZCTL & (1 << n))
   {
      int mzx = ((R->MZCTL >> 8) & 0xF) + 1;
      int mzy = ((R->MZCTL >> 12) & 0xF) + 1;
      x -= x % mzx;
      y -= y % mzy;
      R = PixDbgRegsForLine(y, ilace);
      PixDbgNote(out, "mosaique appliquee");
   }

   switch (n)
   {
      case 0:
         colornumber = (R->CHCTLA >> 4) & 0x7;
         isbitmap    = (R->CHCTLA >> 1) & 0x1;
         bmsz        = (R->CHCTLA >> 2) & 0x3;
         chsz        =  R->CHCTLA & 0x1;
         pnc = R->PNCN0; plsz = R->PLSZ & 0x3;
         mpab = R->MPABN0; mpcd = R->MPCDN0;
         mapofs = R->MPOFN & 0x7;
         coloroffset = (R->CRAOFA & 0x7) << 8;
         bmppal = R->BMPNA & 0x7;
         tpon = (R->BGON >> 8) & 1;
         break;
      case 1:
         colornumber = (R->CHCTLA >> 12) & 0x3;
         isbitmap    = (R->CHCTLA >> 9) & 0x1;
         bmsz        = (R->CHCTLA >> 10) & 0x3;
         chsz        = (R->CHCTLA >> 8) & 0x1;
         pnc = R->PNCN1; plsz = (R->PLSZ >> 2) & 0x3;
         mpab = R->MPABN1; mpcd = R->MPCDN1;
         mapofs = (R->MPOFN >> 4) & 0x7;
         coloroffset = ((R->CRAOFA >> 4) & 0x7) << 8;
         bmppal = (R->BMPNA >> 8) & 0x7;
         tpon = (R->BGON >> 9) & 1;
         break;
      case 2:
         colornumber = (R->CHCTLB >> 1) & 0x1;
         isbitmap = 0;
         chsz = R->CHCTLB & 0x1;
         pnc = R->PNCN2; plsz = (R->PLSZ >> 4) & 0x3;
         mpab = R->MPABN2; mpcd = R->MPCDN2;
         mapofs = (R->MPOFN >> 8) & 0x7;
         coloroffset = ((R->CRAOFA >> 8) & 0x7) << 8;
         tpon = (R->BGON >> 10) & 1;
         break;
      default:
         colornumber = (R->CHCTLB >> 5) & 0x1;
         isbitmap = 0;
         chsz = (R->CHCTLB >> 4) & 0x1;
         pnc = R->PNCN3; plsz = (R->PLSZ >> 6) & 0x3;
         mpab = R->MPABN3; mpcd = R->MPCDN3;
         mapofs = (R->MPOFN >> 12) & 0x7;
         coloroffset = ((R->CRAOFA >> 12) & 0x7) << 8;
         tpon = (R->BGON >> 11) & 1;
         break;
   }

   if (colornumber > 4) { strcpy(out->note, "nombre de couleurs reserve (CHCTLA)"); return 0; }

   out->colornumber = colornumber;
   out->isbitmap = isbitmap;
   out->coloroffset = coloroffset;

   /* ---- Coordonnees dans l'ecran de scroll, en virgule fixe 8 bits ---- */
   if (n <= 1)
   {
      const u16 scrctl = (n == 0) ? (R->SCRCTL & 0x3F) : ((R->SCRCTL >> 8) & 0x3F);
      const u32 scx8 = ((u32)((n == 0) ? R->SCXIN0 : R->SCXIN1) & 0x7FF) << 8
                     | (((n == 0) ? R->SCXDN0 : R->SCXDN1) >> 8);
      const u32 scy8 = ((u32)((n == 0) ? R->SCYIN0 : R->SCYIN1) & 0x7FF) << 8
                     | (((n == 0) ? R->SCYDN0 : R->SCYDN1) >> 8);
      u32 zx8 = (n == 0) ? (((R->ZMXN0.part.I & 7) << 8) | (R->ZMXN0.part.D >> 8))
                         : (((R->ZMXN1.part.I & 7) << 8) | (R->ZMXN1.part.D >> 8));
      const u32 zy8 = (n == 0) ? (((R->ZMYN0.part.I & 7) << 8) | (R->ZMYN0.part.D >> 8))
                               : (((R->ZMYN1.part.I & 7) << 8) | (R->ZMYN1.part.D >> 8));
      u32 lsx8 = 0, lsy8 = 0;
      int lscx = (scrctl >> 1) & 1, lscy = (scrctl >> 2) & 1, lzmx = (scrctl >> 3) & 1;
      int b = y;

      if (lscx || lscy || lzmx)
      {
         /* Intervalle 2^LSS, double en single-density interlace (LSMD=2),
          * comme ReadLineScrollData (vidshared.h). La table est lue au debut
          * de chaque bloc et tenue sur le bloc (Vdp2GenLineinfo). */
         int lineinc = 1 << ((scrctl >> 4) & 3);
         int fl, sub, k = 0;
         const u32 *snap;
         if (((Vdp2Regs->TVMD >> 6) & 3) == 2) lineinc *= 2;
         b = y - (y % lineinc);
         fl = b >> ilace;
         sub = ilace ? (b & 1) : 0;
         if (fl >= VDP2_LINE_SNAPSHOT_MAX) fl = VDP2_LINE_SNAPSHOT_MAX - 1;
         snap = (n == 1) ? line_scroll_data[fl].n1[sub] : line_scroll_data[fl].n0[sub];
         /* Longwords tasses dans l'ordre SX, SY, SZ (seuls les actifs).
          * SX/SY : entier bits 26-16, fraction bits 15-8.
          * SZ    : entier bits 18-16, fraction bits 15-8. */
         if (lscx) { lsx8 = (((snap[k] >> 16) & 0x7FF) << 8) | ((snap[k] >> 8) & 0xFF); k++; }
         if (lscy) { lsy8 = (((snap[k] >> 16) & 0x7FF) << 8) | ((snap[k] >> 8) & 0xFF); k++; }
         if (lzmx) { zx8  = (((snap[k] >> 16) & 0x7) << 8)   | ((snap[k] >> 8) & 0xFF); }
      }

      u = (scx8 + lsx8 + (u32)x * zx8) >> 8;
      if (lscy) v = (scy8 + lsy8 + (u32)(y - b) * zy8) >> 8;
      else      v = (scy8 + (u32)y * zy8) >> 8;

      if (scrctl & 1) PixDbgNote(out, "vertical cell scroll actif : non pris en compte");
   }
   else
   {
      int fl = y >> ilace;
      if (fl >= VDP2_LINE_SNAPSHOT_MAX) fl = VDP2_LINE_SNAPSHOT_MAX - 1;
      u = (u32)((((n == 2) ? R->SCXN2 : R->SCXN3) & 0x7FF) + x);
      v = (u32)(Vdp2Nbg23LineScrollY[n - 2][fl] + y);
   }

   /* ---- Lecture du point ---- */
   if (isbitmap)
   {
      static const int bw[4] = { 512, 512, 1024, 1024 };
      static const int bh[4] = { 256, 512, 256, 512 };
      const u32 w = (u32)bw[bmsz], h = (u32)bh[bmsz];
      const u32 base = mapofs * 0x20000;
      u &= w - 1; v &= h - 1;
      out->srcx = (int)u; out->srcy = (int)v;
      out->charnum = -1;
      out->dot = PixDbgReadDot(base, v * w + u, colornumber, &out->vramaddr);
      if (colornumber <= 1) out->palette = (int)(bmppal << 4);
   }
   else
   {
      const int patternwh = chsz ? 2 : 1;
      const int pnwords   = (pnc & 0x8000) ? 1 : 2;
      const int auxmode   = (pnc >> 14) & 1;
      const u32 supp      = pnc & 0x3FF;
      int planew = 1, planeh = 1, pagewh, planeidx, ox, oy, k;
      u32 pagebytes, mask, tmp, planeaddr, px, py, pnaddr, charaddr, cellbytes;
      static const int bits[5] = { 4, 8, 16, 16, 32 };

      if (plsz == 1) { planew = 2; }
      else if (plsz == 3) { planew = planeh = 2; }
      else if (plsz == 2) { PixDbgNote(out, "PLSZ=2 (reserve) traite comme 1x1"); }

      /* Carte NBG = 2x2 plans (A B / C D). */
      u &= (u32)(2 * planew * 512 - 1);
      v &= (u32)(2 * planeh * 512 - 1);
      out->srcx = (int)u; out->srcy = (int)v;
      planeidx = (int)((v / (u32)(planeh * 512)) * 2 + (u / (u32)(planew * 512)));
      switch (planeidx)
      {
         case 0:  tmp = mpab & 0xFF; break;
         case 1:  tmp = mpab >> 8;   break;
         case 2:  tmp = mpcd & 0xFF; break;
         default: tmp = mpcd >> 8;   break;
      }
      tmp |= mapofs << 6;

      /* Equivalent de CalcPlaneAddr (vidshared.h) : numero de page masque a
       * la taille de la VRAM, bits bas ignores selon la taille du plan. */
      pagewh = 64 / patternwh;
      pagebytes = (u32)(pagewh * pagewh * pnwords * 2);
      mask = 0x80000 / pagebytes - 1;
      planeaddr = (((tmp & mask) >> (planew + planeh - 2)) * (u32)(planew * planeh)) * pagebytes;

      px = u % (u32)(planew * 512);
      py = v % (u32)(planeh * 512);
      pnaddr = planeaddr
             + (((py / 512) * (u32)planew + (px / 512)) * (u32)(pagewh * pagewh)
                + ((py % 512) / (u32)(8 * patternwh)) * (u32)pagewh
                + ((px % 512) / (u32)(8 * patternwh))) * (u32)(pnwords * 2);
      out->pnaddr = pnaddr & 0x7FFFF;

      /* Decodage du pattern name : meme logique que Vdp2PatternAddrPos. */
      if (pnwords == 1)
      {
         u16 pn = PixDbgWord(pnaddr);
         out->pndata = pn;
         out->palette = (colornumber == 0)
            ? (int)(((pn & 0xF000) >> 12) | ((supp & 0xE0) >> 1))
            : (int)((pn & 0x7000) >> 8);
         if (auxmode == 0)
         {
            out->flip = (pn & 0xC00) >> 10;
            charaddr = (patternwh == 1)
               ? ((pn & 0x3FF) | ((supp & 0x1F) << 10))
               : (((pn & 0x3FF) << 2) | (supp & 0x3) | ((supp & 0x1C) << 10));
         }
         else
         {
            out->flip = 0;
            charaddr = (patternwh == 1)
               ? ((pn & 0xFFF) | ((supp & 0x1C) << 10))
               : (((pn & 0xFFF) << 2) | (supp & 0x3) | ((supp & 0x10) << 10));
         }
      }
      else
      {
         u16 pn1 = PixDbgWord(pnaddr), pn2 = PixDbgWord(pnaddr + 2);
         out->pndata = ((u32)pn1 << 16) | pn2;
         charaddr = pn2 & 0x7FFF;
         out->flip = (pn1 & 0xC000) >> 14;
         out->palette = (colornumber == 0) ? (int)(pn1 & 0x7F) : (int)(pn1 & 0x70);
      }
      if (!(R->VRSIZE & 0x8000)) charaddr &= 0x3FFF;
      out->charnum = (int)charaddr;
      if (colornumber >= 2) out->palette = -1;

      /* Point dans le motif (1x1 ou 2x2 cellules, retournement global),
       * cellules d'un motif 2x2 dans l'ordre HG, HD, BG, BD. */
      ox = (int)((px % 512) % (u32)(8 * patternwh));
      oy = (int)((py % 512) % (u32)(8 * patternwh));
      if (out->flip & 1) ox = 8 * patternwh - 1 - ox;
      if (out->flip & 2) oy = 8 * patternwh - 1 - oy;
      k = (oy >> 3) * patternwh + (ox >> 3);
      cellbytes = (u32)(64 * bits[colornumber] / 8);
      out->dot = PixDbgReadDot(charaddr * 0x20 + (u32)k * cellbytes,
                               (u32)((oy & 7) * 8 + (ox & 7)), colornumber, &out->vramaddr);
   }
   out->vramaddr &= 0x7FFFF;

   /* ---- Transparence et index CRAM ---- */
   switch (colornumber)
   {
      case 0:
      case 1:
         out->transparent = (out->dot == 0) && !tpon;
         if (!out->transparent)
            out->cramindex = PixDbgCramWrap(R, (int)(coloroffset + (((u32)out->palette << 4) | out->dot)));
         break;
      case 2:
         out->dot &= 0x7FF;
         out->transparent = (out->dot == 0) && !tpon;
         if (!out->transparent)
            out->cramindex = PixDbgCramWrap(R, (int)(coloroffset + out->dot));
         break;
      case 3:
         out->direct_rgb = 1; out->rgb = out->dot & 0xFFFF;
         out->transparent = !(out->dot & 0x8000) && !tpon;
         break;
      default:
         out->direct_rgb = 1; out->rgb = out->dot;
         out->transparent = !(out->dot & 0x80000000) && !tpon;
         break;
   }

   PixDbgNote(out, "VRAM lue en fin de trame");
   out->valid = 1;
   return 1;
}

static int PixDbgSprite(u32 word, vdp2debugpixel_struct *out)
{
   const Vdp2 *R = Vdp2Regs;
   const int type = R->SPCTL & 0xF;
   int pr = 0, cc = 0, prbits = 0, ccbits = 0;
   u32 dc;

   out->colornumber = -1;
   out->spritetype = type;
   out->dot = word;

   if (word == 0) { out->transparent = 1; out->valid = 1; return 1; }
   /* Types 8-F : seul l'octet bas est significatif (getCode_8_16). */
   if (type >= 8) word &= 0xFF;

   /* SPCLMD (bit 5) : mot a MSB=1 = RGB direct (types 0-7, mot 16 bits). */
   if (type < 8 && (R->SPCTL & 0x20) && (word & 0x8000))
   {
      out->direct_rgb = 1;
      out->rgb = word & 0x7FFF;
      out->valid = 1;
      return 1;
   }

   /* Champs PR / CC / DC par type : memes masques que les decodeurs
    * Yglprg_vdp2_sprite_type_0..F (common_glshader.c). Les types 8-F ne
    * portent que l'octet bas. */
   switch (type)
   {
      case 0x0: pr = (word >> 14) & 3; cc = (word >> 11) & 7; dc = word & 0x7FF; prbits = 2; ccbits = 3; break;
      case 0x1: pr = (word >> 13) & 7; cc = (word >> 11) & 3; dc = word & 0x7FF; prbits = 3; ccbits = 2; break;
      case 0x2: pr = (word >> 14) & 1; cc = (word >> 11) & 7; dc = word & 0x7FF; prbits = 1; ccbits = 3; break;
      case 0x3: pr = (word >> 13) & 3; cc = (word >> 11) & 3; dc = word & 0x7FF; prbits = 2; ccbits = 2; break;
      case 0x4: pr = (word >> 13) & 3; cc = (word >> 10) & 7; dc = word & 0x3FF; prbits = 2; ccbits = 3; break;
      case 0x5: pr = (word >> 12) & 7; cc = (word >> 11) & 1; dc = word & 0x7FF; prbits = 3; ccbits = 1; break;
      case 0x6: pr = (word >> 12) & 7; cc = (word >> 10) & 3; dc = word & 0x3FF; prbits = 3; ccbits = 2; break;
      case 0x7: pr = (word >> 12) & 7; cc = (word >>  9) & 7; dc = word & 0x1FF; prbits = 3; ccbits = 3; break;
      case 0x8: pr = (word >> 7) & 1; dc = word & 0x7F; prbits = 1; break;
      case 0x9: pr = (word >> 7) & 1; cc = (word >> 6) & 1; dc = word & 0x3F; prbits = 1; ccbits = 1; break;
      case 0xA: pr = (word >> 6) & 3; dc = word & 0x3F; prbits = 2; break;
      case 0xB: cc = (word >> 6) & 3; dc = word & 0x3F; ccbits = 2; break;
      case 0xC: pr = (word >> 7) & 1; dc = word & 0xFF; prbits = 1; break;
      case 0xD: pr = (word >> 7) & 1; cc = (word >> 6) & 1; dc = word & 0xFF; prbits = 1; ccbits = 1; break;
      case 0xE: pr = (word >> 6) & 3; dc = word & 0xFF; prbits = 2; break;
      default:  cc = (word >> 6) & 3; dc = word & 0xFF; ccbits = 2; break;
   }
   (void)prbits; (void)ccbits;

   out->dot = dc;
   out->sprprio = pr;
   out->sprcc = cc;
   {
      const u16 pris[4] = { R->PRISA, R->PRISB, R->PRISC, R->PRISD };
      const u16 ccrs[4] = { R->CCRSA, R->CCRSB, R->CCRSC, R->CCRSD };
      out->sprprioval = (pris[(pr >> 1) & 3] >> ((pr & 1) * 8)) & 0x7;
      out->sprccratio = (ccrs[(cc >> 1) & 3] >> ((cc & 1) * 8)) & 0x1F;
   }
   if (type < 8 && dc == 0x7FE) out->sprshadow = 1;

   /* Decalage CRAOFB.SPCAOS (bits 6-4). */
   out->coloroffset = ((R->CRAOFB >> 4) & 0x7) << 8;
   out->cramindex = PixDbgCramWrap(R, (int)(out->coloroffset + dc));
   out->palette = -1;
   out->valid = 1;
   return 1;
}

int Vdp2DebugPixelSource(u32 screen, int x, int y, u32 spriteword,
                         vdp2debugpixel_struct *out)
{
   if (out == NULL) return 0;
   memset(out, 0, sizeof(*out));
   out->palette = -1;
   out->cramindex = -1;
   out->charnum = -1;
   out->spritetype = -1;
   out->sprprio = -1;
   out->sprcc = -1;

   if (Vdp2Regs == NULL || Vdp2Ram == NULL || x < 0 || y < 0)
   {
      strcpy(out->note, "VDP2 non initialise");
      return 0;
   }

   switch (screen)
   {
      case NBG0: return PixDbgNBG(0, x, y, out);
      case NBG1: return PixDbgNBG(1, x, y, out);
      case NBG2: return PixDbgNBG(2, x, y, out);
      case NBG3: return PixDbgNBG(3, x, y, out);
      case SPRITE: return PixDbgSprite(spriteword & 0xFFFF, out);
      default:
         strcpy(out->note, "RBG : decodage par parametres de rotation non disponible");
         return 0;
   }
}

//////////////////////////////////////////////////////////////////////////////
