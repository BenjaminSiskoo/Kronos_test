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

#ifndef VDP2DEBUG_H
#define VDP2DEBUG_H

#define VDP2_DEBUG_STRING_SIZE 4096

#ifdef __cplusplus
extern "C" {
#endif

void Vdp2DebugStatsRBG0(char *outstring, int *isenabled);
void Vdp2DebugStatsRBG1(char *outstring, int *isenabled);
void Vdp2DebugStatsNBG0(char *outstring, int *isenabled);
void Vdp2DebugStatsNBG1(char *outstring, int *isenabled);
void Vdp2DebugStatsNBG2(char *outstring, int *isenabled);
void Vdp2DebugStatsNBG3(char *outstring, int *isenabled);
void Vdp2DebugStatsSprite(char *outstring, int *isenabled);
void Vdp2DebugStatsGeneral(char *outstring, int *isenabled);
pixel_t *Vdp2DebugTexture(u32 screen, int * w, int * h);

/* ------------------------------------------------------------------------
 * Origine d'un pixel affiche par le VDP2 Viewer.
 *
 * Les couches extraites par Vdp2DebugTexture() sont deja converties en RGB
 * (le shader Yglprg_normal_cram_f lit la CRAM) : l'index de couleur est
 * perdu, et le retrouver en cherchant la couleur dans la CRAM est faux des
 * qu'une meme couleur figure deux fois dans la palette. On redecode donc le
 * pixel a la source, depuis la VRAM et les registres de la ligne
 * (Vdp2Lines[]), pour NBG0-3. Pour SPRITE, le mot brut du framebuffer VDP1
 * (R = octet bas, G = octet haut dans la texture extraite) est passe dans
 * 'spriteword' et decode selon SPCTL. RBG0/RBG1 ne sont pas decodes.
 * ---------------------------------------------------------------------- */
typedef struct
{
   int valid;           /* 1 : pixel decode                                  */
   int transparent;     /* 1 : point transparent (code 0 / bit MSB a 0)      */
   int direct_rgb;      /* 1 : couleur RGB directe, pas d'index CRAM         */
   u32 rgb;             /* valeur RGB directe brute (15 ou 24 bits)          */
   int colornumber;     /* 0=16 1=256 2=2048 3=32K 4=16M couleurs, -1 sprite */
   int isbitmap;        /* 1 : bitmap, 0 : cellules                          */
   u32 dot;             /* code du point lu en VRAM (ou code couleur sprite) */
   int palette;         /* numero de palette 7 bits (unites de 16), -1 sinon */
   u32 coloroffset;     /* decalage CRAOFA/CRAOFB, en index CRAM             */
   int cramindex;       /* index CRAM final, -1 si RGB direct ou transparent */
   u32 vramaddr;        /* adresse VRAM du point                             */
   int srcx, srcy;      /* coordonnees dans l'ecran de scroll / le bitmap    */
   int charnum;         /* numero de caractere (unites de 0x20), -1 bitmap   */
   u32 pnaddr;          /* adresse du pattern name, 0 en bitmap              */
   u32 pndata;          /* pattern name lu (1 ou 2 mots)                     */
   int flip;            /* bit0 = H, bit1 = V                                */
   int spritetype;      /* SPCTL.SPTYPE, -1 hors sprite                      */
   int sprprio;         /* sprite : champ PR, -1 sinon                       */
   int sprprioval;      /* sprite : priorite resultante (PRISx)              */
   int sprcc;           /* sprite : champ CC, -1 sinon                       */
   int sprccratio;      /* sprite : ratio resultant (CCRSx)                  */
   int sprshadow;       /* sprite : 1 = code d'ombre normale (0x7FE)         */
   char note[256];      /* approximations / limites du decodage              */
} vdp2debugpixel_struct;

int Vdp2DebugPixelSource(u32 screen, int x, int y, u32 spriteword,
                         vdp2debugpixel_struct *out);

#ifdef __cplusplus
}
#endif

#endif
