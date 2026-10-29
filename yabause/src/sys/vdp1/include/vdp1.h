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

#ifndef VDP1_H
#define VDP1_H

#include "memory.h"
#include "vdp2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIDCORE_DEFAULT         -1

#define CMD_QUEUE_SIZE 2048

//#define YAB_ASYNC_RENDERING 1

typedef struct {
   u16 TVMR;
   u16 FBCR;
   u16 PTMR;
   u16 EWDR;
   u16 EWLR;
   u16 EWRR;
   u16 ENDR;
   u16 EDSR;
   u16 LOPR;
   u16 COPR;
   u16 MODR;

   u16 lCOPR;

   u32 addr;

   s16 localX;
   s16 localY;

   u16 systemclipX1;
   u16 systemclipY1;
   u16 systemclipX2;
   u16 systemclipY2;

   u16 userclipX1;
   u16 userclipY1;
   u16 userclipX2;
   u16 userclipY2;
   u16 userclipMode; // VDP1 Manual §6.3 CMDPMOD bit 9 (Cmod):
                     // 0 = inside drawing mode (draw within user clip rect)
                     // 1 = outside drawing mode (draw outside user clip rect)


} Vdp1;

// struct for Vdp1 part that shouldn't be saved
typedef struct {
   int disptoggle;
   int manualerase;
   int manualchange;
   int onecycleerase;
   int onelasterase;
   int onecyclechange;
   int useVBlankErase;
   int current_frame;
   int updateVdp1Ram;
   int checkEDSR;
   int status;
   int blocked;
   /* Set when a V-blank erase/write cannot finish inside the vertical
    * blanking period. VDP1 Manual ST-013-R3 p.49: the erase completes only
    * "if this is within the number of pixels that can be used in V-blank
    * erase" (Tables 4.4 and 4.5). Beyond that the hardware simply stops
    * where it got to and the rest of the frame buffer keeps its previous
    * contents. eraseOverrunLines holds how many display lines short the
    * blanking period fell. */
   int eraseOverrun;
   int eraseOverrunLines;
} Vdp1External_struct;

extern Vdp1External_struct Vdp1External;

typedef enum {
  VDPCT_NORMAL_SPRITE = 0,
  VDPCT_SCALED_SPRITE = 1,
  VDPCT_DISTORTED_SPRITE = 2,
  VDPCT_DISTORTED_SPRITEN = 3,
  VDPCT_POLYGON = 4,
  VDPCT_POLYLINE = 5,
  VDPCT_LINE = 6,
  VDPCT_POLYLINEN = 7,
  VDPCT_USER_CLIPPING_COORDINATES = 8,
  VDPCT_SYSTEM_CLIPPING_COORDINATES = 9,
  VDPCT_LOCAL_COORDINATES = 10,
  VDPCT_USER_CLIPPING_COORDINATESN = 11,

  VDPCT_INVALID = 12,
  VDPCT_DRAW_END

} Vdp1CommandType;

typedef struct
{
  u32 CMDCTRL;
  u32 CMDLINK;
  u32 CMDPMOD;
  u32 CMDCOLR;
  u32 CMDSRCA;
  u32 CMDSIZE;
  s32 CMDXA;
  s32 CMDYA;
  s32 CMDXB;
  s32 CMDYB;
  s32 CMDXC;
  s32 CMDYC;
  s32 CMDXD;
  s32 CMDYD;
  u32 CMDGRDA;
  u32 w;
  u32 h;
  u32 flip;
  u32 type;
  u32 nbStep;
  float uAstepx;
  float uAstepy;
  float uBstepx;
  float uBstepy;
  float G[12];
  int hss;
  int eos; // VDP1 Manual §4.2 FBCR bit 4: Even/Odd coord select for HSS
           // 0 = sample even X coords only, 1 = sample odd X coords only
           // Only meaningful when hss=1 (HSS enabled in CMDPMOD bit 12)
} vdp1cmd_struct;

typedef struct
{
   int id;
   const char *Name;
   int (*Init)(void);
   void (*DeInit)(void);
   void (*Resize)(int,int,unsigned int, unsigned int, int);
   void (*getScale)(float *xRatio, float *yRatio, int *xUp, int *yUp);
   int (*IsFullscreen)(void);
   // VDP1 specific
   int (*Vdp1Reset)(void);
   void (*Vdp1Draw)();
   void(*Vdp1NormalSpriteDraw)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   void(*Vdp1ScaledSpriteDraw)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   void(*Vdp1DistortedSpriteDraw)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   void(*Vdp1PolygonDraw)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   void(*Vdp1PolylineDraw)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   void(*Vdp1LineDraw)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   void(*Vdp1UserClipping)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   void(*Vdp1SystemClipping)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   void(*Vdp1LocalCoordinate)(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs);
   int(*Vdp1EraseWrite)(int id);
   void(*Vdp1FrameChange)(void);
   void(*Vdp1RegenerateCmd)(vdp1cmd_struct* cmd);
   // VDP2 specific
   int (*Vdp2Reset)(void);
   void (*Vdp2Draw)(void);
   void (*GetGlSize)(int *width, int *height);
   void (*SetSettingValue)(int type, int value);
   void(*Sync)();
   void(*Vdp2DispOff)(void);
   void (*composeFB)(Vdp2 *regs);
   void (*composeVDP1)(void);
   int (*setupFrame)();
   void (*FinsihDraw)(void);
   void (*Vdp1FBDraw)(void);
   pixel_t* (*getVdp2ScreenExtract)(u32 screen, int * w, int * h);
   void (*SetupVdp1Scale)(int scale);
   void (*startVdp1Render)(void);
   void (*endVdp1Render)(void);
   // Video CD Card (EXPERIMENTAL, see mpegcard.h): draws the currently
   // decoded picture on top of the frame the rest of Vdp2Draw() just
   // produced. Optional -- appended at the end of the struct so every
   // existing backend's positional initializer (which lists fewer
   // members than this struct now has) simply leaves it NULL, per
   // C99 6.7.9p21, and keeps behaving exactly as before.
   void (*Vdp2DrawMpegOverlay)(const u8 *rgba, int width, int height);
} VideoInterface_struct;

extern VideoInterface_struct *VIDCore;

extern u8 * Vdp1Ram;
extern int vdp1Ram_update_start;
extern int vdp1Ram_update_end;

u8 FASTCALL	Vdp1RamReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL	Vdp1RamReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL	Vdp1RamReadLong(SH2_struct *context, u8*, u32);
void FASTCALL	Vdp1RamWriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL	Vdp1RamWriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL	Vdp1RamWriteLong(SH2_struct *context, u8*, u32, u32);
u8 FASTCALL Vdp1FrameBuffer16bReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL Vdp1FrameBuffer16bReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL Vdp1FrameBuffer16bReadLong(SH2_struct *context, u8*, u32);
void FASTCALL Vdp1FrameBuffer16bWriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL Vdp1FrameBuffer16bWriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL Vdp1FrameBuffer16bWriteLong(SH2_struct *context, u8*, u32, u32);
u8 FASTCALL Vdp1FrameBuffer8bReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL Vdp1FrameBuffer8bReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL Vdp1FrameBuffer8bReadLong(SH2_struct *context, u8*, u32);
void FASTCALL Vdp1FrameBuffer8bWriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL Vdp1FrameBuffer8bWriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL Vdp1FrameBuffer8bWriteLong(SH2_struct *context, u8*, u32, u32);

/* hreso = VDP2 TVMD bits 2-0 (HRESO2-0). Selects the per-raster VDP1 drawing
 * budget from VDP1 Manual ST-013-R3 Table 4.4 p.49. NOT a PAL/NTSC switch. */
extern void Vdp1SetRaster(int hreso);

void Vdp1DrawCommands(u8 * ram, Vdp1 * regs);
void Vdp1FakeDrawCommands(u8 * ram, Vdp1 * regs);

extern Vdp1 * Vdp1Regs;

enum VDP1STATUS {
  VDP1_STATUS_IDLE = 0x0,
  VDP1_STATUS_RUNNING = 0x1,
  VDP1_STATUS_MASK = 0x1,
  VDP1_SWITCHING = 0x2,
  VDP1_SWITCH_REQUEST = 0x4
};

int Vdp1Init(void);
void Vdp1DeInit(void);
int VideoInit(int coreid);
int VideoChangeCore(int coreid);
void VideoDeInit(void);
void Vdp1Reset(void);
int VideoSetSetting(int type, int value);
void Vdp1SetDMAConcurrency();
void Vdp1ClearDMAConcurrency();

u8 FASTCALL	Vdp1ReadByte(SH2_struct *context, u8*, u32);
u16 FASTCALL	Vdp1ReadWord(SH2_struct *context, u8*, u32);
u32 FASTCALL	Vdp1ReadLong(SH2_struct *context, u8*, u32);
void FASTCALL	Vdp1WriteByte(SH2_struct *context, u8*, u32, u8);
void FASTCALL	Vdp1WriteWord(SH2_struct *context, u8*, u32, u16);
void FASTCALL	Vdp1WriteLong(SH2_struct *context, u8*, u32, u32);

int Vdp1SaveState(void ** stream);
int Vdp1LoadState(const void * stream, int version, int size);

u32 Vdp1DebugGetCommandAddr(u32 number);
char *Vdp1DebugGetCommandNumberName(u32 addr);
char *Vdp1DebugGetCommandRaw(u32 addr);
/* Capture de la table de commandes au declenchement du trace. L'interface de
 * debogue active la capture tant qu'elle est ouverte, puis lit l'instantane
 * plutot que la VRAM vivante : c'est la liste effectivement tracee pour la
 * derniere trame, et non ce que le jeu est en train de reconstruire.
 * Vdp1DebugGetFrameRam() renvoie NULL tant qu'aucune trame n'a ete capturee. */
void Vdp1DebugSetCapture(int enable);
u8 *Vdp1DebugGetFrameRam(void);
/* Memoire lue par toutes les fonctions Vdp1Debug* : l'instantane s'il existe,
 * la VRAM vivante sinon (jamais NULL une fois le VDP1 initialise). */
u8 *Vdp1DebugGetRam(void);
/* Parcourt la table une seule fois et range l'adresse de chaque commande
 * (Draw End compris) dans addrs[]. Renvoie le nombre d'adresses ecrites. */
int Vdp1DebugGetCommandList(u32 *addrs, int maxCount);
/* Chemin complet du code couleur 'code' d'un texel de la commande en cmdAddr
 * (banque/LUT, decalage CRAOFB, index et contenu de Color RAM), en texte. */
int Vdp1DebugTexelColorText(u32 cmdAddr, u32 code, char *out, int outSize);
/* Apercu de la trame : toutes les commandes de addrs[] assemblees dans une
 * image ARGB (0 = transparent) de la taille du clipping systeme. boxes recoit
 * 4 entiers par commande (x0, y0, x1, y1 inclus ; x1 < x0 = rien de trace).
 * L'appelant libere le buffer avec free(). */
u32 *Vdp1DebugRenderFrame(const u32 *addrs, int count, int *outW, int *outH, int *boxes);
/* Couleur (format texture du debogueur, octets R,G,B,A) et index de Color RAM
 * (-1 = RGB) d'une donnee de sprite 16 bits telle que le VDP2 la lit : memes
 * regles que l'apercu (masque DC du type SPCTL, SPCLMD, SPCAOS). */
u32 Vdp1DebugSpriteDataColor(u32 data);
int Vdp1DebugSpriteDataCramIndex(u32 data);

Vdp1CommandType Vdp1DebugGetCommandTypeAtAddr(u32 addr);
Vdp1CommandType Vdp1DebugGetCommandType(u32 number);
/* Variantes adressees : l'appelant fournit l'adresse de la commande obtenue
 * une seule fois via Vdp1DebugGetCommandAddr(). Elles evitent de reparcourir
 * la table a chaque appel et garantissent que le nom, le detail et la texture
 * portent bien sur la MEME commande, meme si le jeu reecrit sa liste entre
 * deux rafraichissements de l'interface. */
void Vdp1DebugCommandAtAddr(u32 addr, char *outstring);
u32 *Vdp1DebugTextureAtAddr(u32 addr, int *w, int *h);
u8 *Vdp1DebugRawTextureAtAddr(u32 addr, int *w, int *h, int *numBytes);

/* Variantes historiques a base de rang, conservees pour les appelants qui
 * n'ont pas l'adresse sous la main. */
void Vdp1DebugCommand(u32 number, char *outstring);
u32 *Vdp1DebugTexture(u32 number, int *w, int *h);
u8 *Vdp1DebugRawTexture(u32 number, int *w, int *h, int *numBytes);
void ToggleVDP1(void);

void Vdp1HBlankIN(void);
void Vdp1StartVisibleLine(void);
void Vdp1VBlankOUT(void);
void Vdp1VBlankIN(void);
void Vdp1VBlankIN_It(void);
void Vdp1SwitchFrame(void);

/* Parametres d'erase/write (EWDR, EWLR, EWRR) verrouilles au dernier
 * changement de frame buffer -- a utiliser pour tout effacement 1-cycle,
 * manuel ou V-blank (cf. vdp1.c, Vdp1LatchEraseParameters). */
void Vdp1GetEraseLatch(u16 *ewdr, u16 *ewlr, u16 *ewrr);

/* Statistiques de charge VDP1 pour le debogueur (fenetre VDP1).
 *
 * Ce sont les chiffres du MODELE de cycles de l'emulateur (getNormalCycles,
 * getScaledCycles, getDistortedCycles, getPolygonCycles, 16 cycles par table
 * lue, 70 cycles par commande avortee, cycles voles par les acces CPU), pas
 * une mesure du materiel. Ils donnent un ordre de grandeur de l'utilisation
 * de la capacite de trace, pas une valeur exacte.
 *
 * budgetCycles = cycles par ligne (rasterValue) x nombre de lignes de la
 * periode, lignes bloquees comprises : c'est la capacite theorique, pas ce
 * que l'emulateur a effectivement accorde.
 *
 * est* : estimation detaillee, purement observationnelle (elle ne regle pas
 * vdp1_clock). Le manuel VDP1 ST-013-R3 ne donne que "1 pixel par cycle"
 * (§2.5 p.20) ; le detail (cout par ligne, relecture du frame buffer, pixels
 * d'antialiasing, pixels hors zone parcourus) reprend le modele de Mednafen
 * (ss/vdp1*.c), faute de valeurs documentees. Voir vdp1.c, vdp1EstQuad. */
typedef struct {
   u64 cmdCycles;      /* cycles consommes par l'interpreteur de commandes */
   u64 cpuCycles;      /* cycles retires au trace par les acces CPU/DMA     */
   u64 budgetCycles;   /* capacite theorique de la periode                  */
   u64 estCycles;      /* estimation detaillee (lecture des tables comprise)*/
   u64 estPixels;      /* pixels parcourus, antialiasing compris            */
   u64 estRmwPixels;   /* dont pixels avec relecture du frame buffer        */
   u64 estLines;       /* lignes tracees                                    */
   u32 cyclesPerLine;  /* rasterValue en fin de periode                     */
   u32 lines;          /* lignes ecoulees                                   */
   u32 blockedLines;   /* lignes sans budget (concurrence DMA)              */
   u32 fields;         /* champs ecoules (1 = 60 fps, 2 = 30 fps, ...)      */
   u32 commands;       /* commandes executees (non sautees)                 */
   u32 skipped;        /* commandes sautees (bit JP skip)                   */
   u32 lists;          /* listes demarrees                                  */
   u32 listsEnded;     /* listes terminees par le bit de fin                */
   u32 listsAborted;   /* listes interrompues (PTMR, bascule PTMR=2, ...)   */
   u32 runningAtSwap;  /* image : liste encore en cours a la bascule        */
} Vdp1CycleStats;

#define VDP1_CYCLE_HISTORY 120

/* lastFrame : periode entre les deux dernieres bascules du frame buffer.
 * lastField : dernier champ complet (1/60 s ou 1/50 s).
 * avgPermille / peakPermille : moyenne et maximum, en pour mille, de
 * (cmdCycles + cpuCycles) / budgetCycles sur les VDP1_CYCLE_HISTORY
 * dernieres images (historyCount = nombre d'images reellement enregistrees).
 * Chaque pointeur peut etre NULL. */
void Vdp1DebugGetCycleStats(Vdp1CycleStats *lastFrame, Vdp1CycleStats *lastField,
                            int *avgPermille, int *peakPermille, int *historyCount);

/* Historique image par image, du plus ancien au plus recent, pour le graphe :
 * charge du modele de l'emulateur et charge estimee, en pour mille (cycles
 * des commandes + cycles voles par le CPU, divises par le budget), et 1 si la
 * liste etait encore en cours au changement de frame buffer. Chaque tableau
 * peut etre NULL. Renvoie le nombre d'images ecrites (<= maxCount). */
int Vdp1DebugGetLoadHistory(int *kronosPermille, int *estPermille, u8 *overrun, int maxCount);

/* Cout d'une commande pour le trace correspondant a l'instantane du
 * debogueur (Vdp1DebugGetFrameRam). Rempli seulement tant que la capture est
 * active. Une commande appelee plusieurs fois (CALL) cumule ses passages. */
typedef struct {
   u32 kronosCycles;   /* modele de l'emulateur                            */
   u32 estCycles;      /* estimation detaillee, lecture de la table comprise */
   u32 lines;          /* lignes tracees                                   */
   u32 pixels;         /* pixels parcourus, antialiasing compris           */
   u32 rmwPixels;      /* dont pixels avec relecture du frame buffer       */
   u32 executions;     /* nombre de passages dans ce trace                 */
} Vdp1CommandCost;

/* Renvoie 1 et remplit *out si la commande en addr a ete executee dans le
 * trace capture, 0 sinon (liste interrompue avant elle, capture inactive). */
int Vdp1DebugGetCommandCost(u32 addr, Vdp1CommandCost *out);

#ifdef __cplusplus
}
#endif

#endif
