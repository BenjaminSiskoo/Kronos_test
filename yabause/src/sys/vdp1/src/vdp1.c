/*
    Copyright 2004 Lawrence Sebald
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
 
/*! \file vdp1.c
    \brief VDP1 emulation functions.
*/
 
 
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "yabause.h"
#include "vdp1.h"
#include "debug.h"
#include "scu.h"
#include "vdp2.h"
#include "threads.h"
#include "sh2core.h"
#include "ygl.h"
#include "yui.h"
#include "vdpedit.h"
 
// #define DEBUG_CMD_LIST
// #define FRAMELOG printf
#define FRAMELOG_CMD //printf
#define PRINT_FB //printf
 
u8 * Vdp1Ram;
int vdp1Ram_update_start;
int vdp1Ram_update_end;
int VDP1_MASK = 0xFFFF;
 
extern u32* getVDP1WriteFramebuffer(int frame);
extern u32* getVDP1ReadFramebuffer();
extern void updateVdp1DrawingFBMem(int frame);
extern void YglGenerate();
extern void syncVdp1FBBuffer(u32 addr);
 
static int getVdp1CyclesPerLine(void);
 
VideoInterface_struct *VIDCore=NULL;
extern VideoInterface_struct *VIDCoreList[];
 
Vdp1 * Vdp1Regs;
Vdp1External_struct Vdp1External;
 
/* ------------------------------------------------------------------------
 * Parametres d'erase/write verrouilles au changement de frame buffer.
 *
 * VDP1 User's Manual ST-013-R3 Table 4.1 p.34 : la periode de mise a jour
 * interne de EWDR, EWLR et EWRR est "Frame buffer SW timing" -- une valeur
 * ecrite ne prend effet qu'au changement de frame buffer suivant (p.46-47
 * pour le format des registres). L'erase/write du mode 1-cycle et du mode
 * manuel porte sur le frame buffer AFFICHE pendant le champ qui suit le
 * changement ; l'erase V-blank porte sur ce meme buffer pendant le
 * blanking. Mednafen (ss/vdp1.c, EraseParams rempli au swap) comme Ymir
 * (VDP1Regs::LatchEraseParameters, appele depuis VDP::VDP1SwapFramebuffer)
 * font de meme.
 *
 * Kronos differe l'effacement : Vdp1EraseWrite() ne fait que lever
 * shallVdp1Erase[readframe], et VIDCSEraseWriteVdp1() s'execute au swap
 * SUIVANT (ou a la premiere relecture CPU du buffer), soit une trame plus
 * tard. Il lisait alors les registres courants : un jeu qui reprogramme la
 * zone d'effacement entre deux ecrans (zone reduite, X3 = 0 pour couper
 * l'erase, couleur differente) voyait le dernier effacement du buffer de
 * l'ecran precedent fait avec les NOUVEAUX parametres.
 * ------------------------------------------------------------------------ */
static u16 Vdp1EraseLatchEWDR = 0;
static u16 Vdp1EraseLatchEWLR = 0;
static u16 Vdp1EraseLatchEWRR = 0;
 
static void Vdp1LatchEraseParameters(void) {
  if (Vdp1Regs == NULL) return;
  Vdp1EraseLatchEWDR = Vdp1Regs->EWDR;
  Vdp1EraseLatchEWLR = Vdp1Regs->EWLR;
  Vdp1EraseLatchEWRR = Vdp1Regs->EWRR;
}
 
void Vdp1GetEraseLatch(u16 *ewdr, u16 *ewlr, u16 *ewrr) {
  if (ewdr) *ewdr = Vdp1EraseLatchEWDR;
  if (ewlr) *ewlr = Vdp1EraseLatchEWLR;
  if (ewrr) *ewrr = Vdp1EraseLatchEWRR;
}
 
int vdp1_clock = 0;
 
static int nbCmdToProcess = 0;
static int CmdListInLoop = 0;
 
static int needVdp1draw = 0;
static int oldNeedVdp1draw = 0;
static void Vdp1NoDraw(void);
static int Vdp1Draw(void);
static void FASTCALL Vdp1ReadCommand(vdp1cmd_struct *cmd, u32 addr, u8* ram);
 
extern void addVdp1Framecount ();
 
static void checkFBSync();
 
#define DEBUG_BAD_COORD //YuiMsg
 
int CONVERTCMD(s32 *A) {
  /* Vertex coordinates are decoded as 13-bit signed values: bits [12:0]
   * are kept and bit 12 is the sign, bits 15:13 are ignored. This is the
   * width the VDP1 vertex arithmetic actually uses (Mednafen and Ymir
   * decode CMDXA..CMDYD the same way). The [-1024, +1023] range given in
   * the VDP1 manual is the range a title should stay in, not the width of
   * the register: nothing is truncated to 11 bits in the hardware.
   *
   * The previous code sign-extended from bit 10, which wrapped any
   * coordinate beyond +/-1024 to the opposite side of the screen. Gale
   * Racer places road-side building quads well past the screen edges,
   * as mirrored left/right pairs, for instance X = 0x036E / 0x045F
   * (+878 / +1119) and X = 0xFB6E / 0xFC5F (-1170 / -929). Read on 11 bits,
   * 0x045F became -929 and 0x036E stayed +878: a 1808-pixel-wide distorted
   * sprite spanning the whole screen, stretching a handful of building
   * texels into the large flat colour blocks seen over the road. Read on
   * 13 bits, every one of the 50 such quads captured in the trace is a
   * consistent mirror pair lying entirely off screen, as the game intends.
   *
   * Commands are still never rejected on their upper bits: the real VDP1
   * accepts them. */
  if ((*A) & 0x1000) (*A) |= (s32)0xFFFFE000;
  else               (*A) &= 0x00001FFF;
  return 0;
}
 
/* ---------------------------------------------------------------------------
 * Statistiques de charge VDP1 (debogueur, Vdp1DebugGetCycleStats).
 *
 * Purement observationnel : rien ici ne modifie vdp1_clock ni le deroulement
 * du trace. Deux mesures sont tenues en parallele :
 *
 *  - cmdCycles : le modele de l'emulateur (yabsys.vdp1cycles, consomme en un
 *    seul point dans Vdp1DrawCommands), celui qui regle vdp1_clock ;
 *  - estCycles : une estimation detaillee (vdp1EstQuad / vdp1EstLines plus
 *    bas). Le manuel VDP1 ST-013-R3 ne donne que "1 pixel par cycle" (§2.5
 *    p.20) et aucun cout par ligne ni par mode de calcul couleur ; le detail
 *    reprend donc le modele de Mednafen (ss/vdp1.c, vdp1_common.h,
 *    vdp1_sprite.c, vdp1_poly.c, vdp1_line.c).
 *
 * Les cycles retires par vdp1BusAccess / vdp1BusRead pendant le trace
 * (cpuCycles) s'ajoutent aux deux.
 *
 * Deux periodes :
 *  - le champ : d'une ligne MaxLineCount-1 a la suivante (Vdp1HBlankIN) ;
 *  - l'image  : entre deux bascules du frame buffer (Vdp1SwitchFrame), soit
 *    1 champ a 60 fps, 2 champs a 30 fps, etc.
 * ------------------------------------------------------------------------- */
static Vdp1CycleStats vdp1StatField;      /* champ en cours        */
static Vdp1CycleStats vdp1StatFrame;      /* image en cours        */
static Vdp1CycleStats vdp1StatLastField;  /* dernier champ complet */
static Vdp1CycleStats vdp1StatLastFrame;  /* derniere image        */
static int vdp1StatHistory[VDP1_CYCLE_HISTORY];    /* modele emulateur, pour mille */
static int vdp1StatEstHistory[VDP1_CYCLE_HISTORY]; /* estimation, pour mille       */
static u8  vdp1StatOverrunHistory[VDP1_CYCLE_HISTORY];
static int vdp1StatHistoryPos = 0;
static int vdp1StatHistoryCount = 0;

/* Estimation de la commande en cours : remplie par les fonctions de trace,
 * consommee (et remise a zero) par vdp1StatsCmd. */
typedef struct {
  u32 cycles;
  u32 lines;
  u32 pixels;
  u32 rmwPixels;
} Vdp1Est;
static Vdp1Est vdp1EstCur;

/* Cout par commande du trace capture par le debogueur : une entree par
 * adresse de table possible (CMDLINK x 8 -> pas de 8 octets, 512 Ko de VRAM).
 * N'existe que pendant la capture (Vdp1DebugSetCapture). */
#define VDP1_CMD_COST_ENTRIES (0x80000 >> 3)
static Vdp1CommandCost *vdp1CmdCost = NULL;

static void vdp1CmdCostEnable(int enable) {
  if (enable) {
    if (vdp1CmdCost == NULL)
      vdp1CmdCost = (Vdp1CommandCost *)calloc(VDP1_CMD_COST_ENTRIES, sizeof(Vdp1CommandCost));
  } else {
    free(vdp1CmdCost);
    vdp1CmdCost = NULL;
  }
}

static void vdp1CmdCostClear(void) {
  if (vdp1CmdCost != NULL)
    memset(vdp1CmdCost, 0, VDP1_CMD_COST_ENTRIES * sizeof(Vdp1CommandCost));
}

int Vdp1DebugGetCommandCost(u32 addr, Vdp1CommandCost *out) {
  const Vdp1CommandCost *c;
  if (vdp1CmdCost == NULL) return 0;
  c = &vdp1CmdCost[(addr & 0x7FFFF) >> 3];
  if (c->executions == 0) return 0;
  if (out) *out = *c;
  return 1;
}

static void vdp1StatsReset(void) {
  memset(&vdp1StatField, 0, sizeof(vdp1StatField));
  memset(&vdp1StatFrame, 0, sizeof(vdp1StatFrame));
  memset(&vdp1StatLastField, 0, sizeof(vdp1StatLastField));
  memset(&vdp1StatLastFrame, 0, sizeof(vdp1StatLastFrame));
  memset(vdp1StatHistory, 0, sizeof(vdp1StatHistory));
  memset(vdp1StatEstHistory, 0, sizeof(vdp1StatEstHistory));
  memset(vdp1StatOverrunHistory, 0, sizeof(vdp1StatOverrunHistory));
  memset(&vdp1EstCur, 0, sizeof(vdp1EstCur));
  vdp1StatHistoryPos = 0;
  vdp1StatHistoryCount = 0;
  vdp1CmdCostClear();
}

/* Fin d'une commande (sautee ou non). addr = adresse de sa table. */
static INLINE void vdp1StatsCmd(u32 addr, int cycles, int skipped) {
  /* Lecture de la table de commande : 16 cycles, commande sautee comprise
   * (Mednafen ss/vdp1.c DoDrawing, VDP1_EAT_CLOCKS(16) avant tout test). */
  const u32 est = vdp1EstCur.cycles + 16;

  vdp1StatField.cmdCycles += (u64)cycles;
  vdp1StatFrame.cmdCycles += (u64)cycles;
  vdp1StatField.estCycles += est;
  vdp1StatFrame.estCycles += est;
  vdp1StatField.estPixels += vdp1EstCur.pixels;
  vdp1StatFrame.estPixels += vdp1EstCur.pixels;
  vdp1StatField.estRmwPixels += vdp1EstCur.rmwPixels;
  vdp1StatFrame.estRmwPixels += vdp1EstCur.rmwPixels;
  vdp1StatField.estLines += vdp1EstCur.lines;
  vdp1StatFrame.estLines += vdp1EstCur.lines;
  if (skipped) { vdp1StatField.skipped++;  vdp1StatFrame.skipped++;  }
  else         { vdp1StatField.commands++; vdp1StatFrame.commands++; }

  if (vdp1CmdCost != NULL) {
    Vdp1CommandCost *c = &vdp1CmdCost[(addr & 0x7FFFF) >> 3];
    c->kronosCycles += (u32)cycles;
    c->estCycles    += est;
    c->lines        += vdp1EstCur.lines;
    c->pixels       += vdp1EstCur.pixels;
    c->rmwPixels    += vdp1EstCur.rmwPixels;
    c->executions++;
  }
  memset(&vdp1EstCur, 0, sizeof(vdp1EstCur));
}

static INLINE void vdp1StatsCpu(int cycles) {
  vdp1StatField.cpuCycles += (u64)cycles;
  vdp1StatFrame.cpuCycles += (u64)cycles;
}

static INLINE void vdp1StatsListStart(void) {
  vdp1StatField.lists++;
  vdp1StatFrame.lists++;
  memset(&vdp1EstCur, 0, sizeof(vdp1EstCur));
}

static INLINE void vdp1StatsListEnd(void) {
  vdp1StatField.listsEnded++;
  vdp1StatFrame.listsEnded++;
}

static INLINE void vdp1StatsListAbort(void) {
  vdp1StatField.listsAborted++;
  vdp1StatFrame.listsAborted++;
}

/* Budget d'une ligne : raster = cycles par ligne theoriques (rasterValue),
 * blocked = Vdp1External.blocked (la ligne n'accorde alors aucun cycle). */
static void vdp1StatsLine(int raster, int blocked) {
  vdp1StatField.lines++;
  vdp1StatFrame.lines++;
  vdp1StatField.budgetCycles += (u64)raster;
  vdp1StatFrame.budgetCycles += (u64)raster;
  vdp1StatField.cyclesPerLine = (u32)raster;
  vdp1StatFrame.cyclesPerLine = (u32)raster;
  if (blocked) { vdp1StatField.blockedLines++; vdp1StatFrame.blockedLines++; }
}

static int vdp1StatsPermilleOf(u64 used, u64 budget) {
  if (budget == 0) return 0;
  return (int)((used * 1000 + budget / 2) / budget);
}

static int vdp1StatsPermille(const Vdp1CycleStats *st) {
  return vdp1StatsPermilleOf(st->cmdCycles + st->cpuCycles, st->budgetCycles);
}

static void vdp1StatsEndField(void) {
  vdp1StatField.fields = 1;
  vdp1StatLastField = vdp1StatField;
  memset(&vdp1StatField, 0, sizeof(vdp1StatField));
  vdp1StatFrame.fields++;
}

static void vdp1StatsEndFrame(int runningAtSwap) {
  vdp1StatFrame.runningAtSwap = (u32)(runningAtSwap != 0);
  vdp1StatLastFrame = vdp1StatFrame;
  memset(&vdp1StatFrame, 0, sizeof(vdp1StatFrame));
  if (vdp1StatLastFrame.budgetCycles != 0) {
    vdp1StatHistory[vdp1StatHistoryPos] = vdp1StatsPermille(&vdp1StatLastFrame);
    vdp1StatEstHistory[vdp1StatHistoryPos] =
        vdp1StatsPermilleOf(vdp1StatLastFrame.estCycles + vdp1StatLastFrame.cpuCycles,
                            vdp1StatLastFrame.budgetCycles);
    vdp1StatOverrunHistory[vdp1StatHistoryPos] = (u8)vdp1StatLastFrame.runningAtSwap;
    vdp1StatHistoryPos = (vdp1StatHistoryPos + 1) % VDP1_CYCLE_HISTORY;
    if (vdp1StatHistoryCount < VDP1_CYCLE_HISTORY) vdp1StatHistoryCount++;
  }
}

void Vdp1DebugGetCycleStats(Vdp1CycleStats *lastFrame, Vdp1CycleStats *lastField,
                            int *avgPermille, int *peakPermille, int *historyCount) {
  int i, sum = 0, peak = 0;
  if (lastFrame) *lastFrame = vdp1StatLastFrame;
  if (lastField) *lastField = vdp1StatLastField;
  for (i = 0; i < vdp1StatHistoryCount; i++) {
    sum += vdp1StatHistory[i];
    if (vdp1StatHistory[i] > peak) peak = vdp1StatHistory[i];
  }
  if (avgPermille)  *avgPermille  = vdp1StatHistoryCount ? (sum + vdp1StatHistoryCount / 2) / vdp1StatHistoryCount : 0;
  if (peakPermille) *peakPermille = peak;
  if (historyCount) *historyCount = vdp1StatHistoryCount;
}

int Vdp1DebugGetLoadHistory(int *kronosPermille, int *estPermille, u8 *overrun, int maxCount) {
  int i, n = vdp1StatHistoryCount;
  /* Indice du plus ancien : si le tampon n'est pas plein, c'est 0. */
  const int first = (vdp1StatHistoryCount < VDP1_CYCLE_HISTORY) ? 0 : vdp1StatHistoryPos;
  int skip;
  if (maxCount <= 0) return 0;
  skip = (n > maxCount) ? (n - maxCount) : 0;   /* garder les plus recents */
  for (i = skip; i < n; i++) {
    const int src = (first + i) % VDP1_CYCLE_HISTORY;
    if (kronosPermille) kronosPermille[i - skip] = vdp1StatHistory[src];
    if (estPermille)    estPermille[i - skip]    = vdp1StatEstHistory[src];
    if (overrun)        overrun[i - skip]        = vdp1StatOverrunHistory[src];
  }
  return n - skip;
}

/* ---------------------------------------------------------------------------
 * Estimation detaillee du cout d'une commande de trace.
 *
 * Le manuel ne documente pas ces couts : chaque valeur ci-dessous est celle de
 * Mednafen (references entre parentheses). Le VDP1 trace un quadrilatere
 * (sprite normal, agrandi, deforme, polygone) comme une suite de LIGNES entre
 * les aretes AD et BC, et non comme une surface :
 *
 *  - nombre de lignes = max(|dx|,|dy|) des aretes AD et BC, + 1
 *    (vdp1_sprite.c / vdp1_poly.c, dmax) ;
 *  - par ligne : 8 cycles, + 4 si le pre-clipping est actif (Pclp = 0)
 *    (vdp1.c VDP1_SetupDrawLine) ;
 *  - une ligne entierement hors de la zone de clipping, pre-clipping actif,
 *    n'est pas tracee (sauf la derniere, reduite a un point) ;
 *  - par pixel : 1 cycle, 6 si le pixel du frame buffer doit etre relu --
 *    MSB On, ombre, demi-transparence, Gouraud + demi-transparence, c'est-a-
 *    dire CCB bit 0 = 1 (vdp1_common.h VDP1_PlotPixel, "ret += 5") ;
 *  - les pixels hors zone sont parcourus jusqu'a la sortie de la zone ; une
 *    ligne qui ne la traverse jamais est parcourue en entier (VDP1_PBODY) ;
 *  - quadrilateres : un pixel d'antialiasing a chaque pas en diagonale, soit
 *    min(|dx|,|dy|) pixels de plus par ligne ; pas pour les lignes et
 *    polylignes (AA = 0 dans vdp1_line.c) ;
 *  - surcout de +48/256 (frame buffer 16 bits) ou +24/256 (8 bits) sur les
 *    cycles de pixels (vdp1_common.h VDP1_AdjustDrawTiming) ;
 *  - par commande : 16 cycles de lecture de table (ajoutes par
 *    vdp1StatsCmd), + 4 pour la table Gouraud (sprites, polygones), + 16
 *    pour une table de couleurs (mode couleur 1, sprites), + 2 par ligne de
 *    polyligne en Gouraud.
 *
 * Ce qui n'est pas modelise : les codes de fin (ECD) qui abregent une ligne,
 * le double entrelacement, et l'attente du VDP1 quand le CPU accede a la VRAM
 * (deja comptee dans cpuCycles).
 * ------------------------------------------------------------------------- */
typedef struct { int x0, y0, x1, y1; } Vdp1EstRect;

/* Zone de clipping du trace : la zone utilisateur si le clipping
 * utilisateur est actif en mode "dedans" (Clip = 1, Cmod = 0), la zone
 * systeme sinon (Mednafen, VDP1_SetupDrawLine et VDP1_PBODY). */
static void vdp1EstClipRect(u16 pmod, Vdp1EstRect *r) {
  if ((pmod & 0x0400) && !(pmod & 0x0200)) {
    r->x0 = Vdp1Regs->userclipX1; r->y0 = Vdp1Regs->userclipY1;
    r->x1 = Vdp1Regs->userclipX2; r->y1 = Vdp1Regs->userclipY2;
  } else {
    r->x0 = 0; r->y0 = 0;
    r->x1 = Vdp1Regs->systemclipX2; r->y1 = Vdp1Regs->systemclipY2;
  }
}

static INLINE int vdp1EstOutside(int x0, int y0, int x1, int y1, const Vdp1EstRect *r) {
  return (x0 < r->x0 && x1 < r->x0) || (x0 > r->x1 && x1 > r->x1) ||
         (y0 < r->y0 && y1 < r->y0) || (y0 > r->y1 && y1 > r->y1);
}

static INLINE int vdp1EstInside(int x, int y, const Vdp1EstRect *r) {
  return (x >= r->x0) && (x <= r->x1) && (y >= r->y0) && (y <= r->y1);
}

/* Pixels parcourus sur une ligne de n pixels qui coupe la zone : jusqu'au
 * premier pixel hors zone apres la sortie (Liang-Barsky sur le segment). */
static u32 vdp1EstWalked(int x0, int y0, int x1, int y1, const Vdp1EstRect *r, u32 n) {
  const double dx = (double)(x1 - x0), dy = (double)(y1 - y0);
  const double p[4] = { -dx, dx, -dy, dy };
  const double q[4] = { (double)(x0 - r->x0), (double)(r->x1 - x0),
                        (double)(y0 - r->y0), (double)(r->y1 - y0) };
  double t0 = 0.0, t1 = 1.0;
  u32 walked;
  int k;
  for (k = 0; k < 4; k++) {
    if (p[k] == 0.0) {
      if (q[k] < 0.0) return n;          /* parallele et dehors : jamais dedans */
    } else {
      const double t = q[k] / p[k];
      if (p[k] < 0.0) { if (t > t1) return n; if (t > t0) t0 = t; }
      else            { if (t < t0) return n; if (t < t1) t1 = t; }
    }
  }
  if (t1 >= 1.0) return n;               /* finit dans la zone */
  walked = (u32)(t1 * (double)(n - 1)) + 2;
  return (walked < n) ? walked : n;
}

/* Une ligne : ajoute son cout a vdp1EstCur. Renvoie les cycles de pixels
 * (avant le surcout VDP1_AdjustDrawTiming, applique par l'appelant). */
static u32 vdp1EstOneLine(int x0, int y0, int x1, int y1, int aa, int isQuad, int last,
                          int preclip, int fullyInside, int perPix, const Vdp1EstRect *r) {
  u32 n, aaPix;
  int adx, ady;
  vdp1EstCur.cycles += 8 + (preclip ? 4 : 0);
  vdp1EstCur.lines++;
  if (preclip && vdp1EstOutside(x0, y0, x1, y1, r)) {
    /* Reduite a un point ; un quadrilatere ne trace pas une ligne
     * pre-clippee, sauf la derniere (vdp1_sprite.c, "|| !iter"). */
    if (isQuad && !last) return 0;
    vdp1EstCur.pixels += 1;
    if (perPix > 1) vdp1EstCur.rmwPixels += 1;
    return (u32)perPix;
  }
  adx = abs(x1 - x0);
  ady = abs(y1 - y0);
  n = (u32)((adx > ady) ? adx : ady) + 1;
  aaPix = aa ? (u32)((adx < ady) ? adx : ady) : 0;
  if (!fullyInside && !(vdp1EstInside(x0, y0, r) && vdp1EstInside(x1, y1, r))) {
    const u32 walked = vdp1EstWalked(x0, y0, x1, y1, r, n);
    aaPix = (u32)(((u64)aaPix * walked) / n);
    n = walked;
  }
  n += aaPix;
  vdp1EstCur.pixels += n;
  if (perPix > 1) vdp1EstCur.rmwPixels += n;
  return n * (u32)perPix;
}

static INLINE int vdp1EstPerPixel(u16 pmod) {
  return ((pmod & 0x8000) || (pmod & 0x0001)) ? 6 : 1;
}

static INLINE u32 vdp1EstAdjust(u32 pixCycles) {
  /* VDP1_AdjustDrawTiming : +48/256 en 16 bits, +24/256 en 8 bits. */
  return pixCycles + (u32)(((u64)pixCycles * ((Vdp1Regs->TVMR & 0x1) ? 24 : 48)) >> 8);
}

/* Quadrilatere A B C D (coordonnees finales, coordonnees locales appliquees).
 * textured = sprite (normal, agrandi, deforme), 0 = polygone. */
static void vdp1EstQuad(const vdp1cmd_struct *cmd, int textured) {
  const u16 pmod = cmd->CMDPMOD;
  const int preclip = !(pmod & 0x0800);
  const int perPix = vdp1EstPerPixel(pmod);
  const int xa = cmd->CMDXA, ya = cmd->CMDYA, xb = cmd->CMDXB, yb = cmd->CMDYB;
  const int xc = cmd->CMDXC, yc = cmd->CMDYC, xd = cmd->CMDXD, yd = cmd->CMDYD;
  Vdp1EstRect r;
  int dmax, i, fullyInside, minx, maxx, miny, maxy;
  u32 pix = 0;

  vdp1EstClipRect(pmod, &r);
  dmax = abs(xd - xa);
  if (abs(yd - ya) > dmax) dmax = abs(yd - ya);
  if (abs(xc - xb) > dmax) dmax = abs(xc - xb);
  if (abs(yc - yb) > dmax) dmax = abs(yc - yb);
  dmax &= 0xFFF;

  minx = MIN(MIN(xa, xb), MIN(xc, xd)); maxx = MAX(MAX(xa, xb), MAX(xc, xd));
  miny = MIN(MIN(ya, yb), MIN(yc, yd)); maxy = MAX(MAX(ya, yb), MAX(yc, yd));
  fullyInside = (minx >= r.x0) && (maxx <= r.x1) && (miny >= r.y0) && (maxy <= r.y1);

  if ((pmod & 0x8004) == 0x0004) vdp1EstCur.cycles += 4;              /* table Gouraud */
  if (textured && (((pmod >> 3) & 0x7) == 1)) vdp1EstCur.cycles += 16; /* table de couleurs */

  if (preclip && (maxx < r.x0 || minx > r.x1 || maxy < r.y0 || miny > r.y1)) {
    /* Tout le quadrilatere est hors zone : toutes les lignes sont
     * pre-clippees, seule la derniere est tracee (un point). */
    vdp1EstCur.cycles += (u32)(dmax + 1) * 12;
    vdp1EstCur.lines  += (u32)(dmax + 1);
    vdp1EstCur.pixels += 1;
    if (perPix > 1) vdp1EstCur.rmwPixels += 1;
    vdp1EstCur.cycles += vdp1EstAdjust((u32)perPix);
    return;
  }

  for (i = 0; i <= dmax; i++) {
    int x0, y0, x1, y1;
    if (dmax == 0) { x0 = xa; y0 = ya; x1 = xb; y1 = yb; }
    else {
      x0 = xa + (int)(((s64)(xd - xa) * i) / dmax);
      y0 = ya + (int)(((s64)(yd - ya) * i) / dmax);
      x1 = xb + (int)(((s64)(xc - xb) * i) / dmax);
      y1 = yb + (int)(((s64)(yc - yb) * i) / dmax);
    }
    pix += vdp1EstOneLine(x0, y0, x1, y1, 1, 1, i == dmax, preclip, fullyInside, perPix, &r);
  }
  vdp1EstCur.cycles += vdp1EstAdjust(pix);
}

/* Ligne (1 segment A-B) ou polyligne (A-B, B-C, C-D, D-A), sans antialiasing. */
static void vdp1EstLines(const vdp1cmd_struct *cmd, int nlines) {
  const u16 pmod = cmd->CMDPMOD;
  const int preclip = !(pmod & 0x0800);
  const int perPix = vdp1EstPerPixel(pmod);
  const int X[4] = { cmd->CMDXA, cmd->CMDXB, cmd->CMDXC, cmd->CMDXD };
  const int Y[4] = { cmd->CMDYA, cmd->CMDYB, cmd->CMDYC, cmd->CMDYD };
  Vdp1EstRect r;
  u32 pix = 0;
  int i;
  vdp1EstClipRect(pmod, &r);
  for (i = 0; i < nlines; i++) {
    const int j = (i + 1) & 3;
    if ((pmod & 0x8004) == 0x0004) vdp1EstCur.cycles += 2;           /* Gouraud */
    pix += vdp1EstOneLine(X[i], Y[i], X[j], Y[j], 0, 0, 1, preclip, 0, perPix, &r);
  }
  vdp1EstCur.cycles += vdp1EstAdjust(pix);
}
 
static void RequestVdp1ToDraw() {
  if (needVdp1draw == 0){
    needVdp1draw = 1;
    CmdListInLoop = 0;
  }
}
 
 
void Vdp1SetDMAConcurrency() {
  Vdp1External.blocked = 1;
}
void Vdp1ClearDMAConcurrency() {
  Vdp1External.blocked = 0;
}
 
static void abortVdp1() {
  if ((Vdp1External.status&VDP1_STATUS_MASK) == VDP1_STATUS_RUNNING) {
    FRAMELOG("Aborting VDP1 %d\n", yabsys.LineCount);
    vdp1StatsListAbort();
    // The vdp1 is still running and a new draw command request has been received
    // Abort the current command list
    Vdp1External.status &= ~VDP1_STATUS_MASK;
    Vdp1External.status |= VDP1_STATUS_IDLE;
    if (VIDCore->endVdp1Render) VIDCore->endVdp1Render();
    CmdListInLoop = 0;
    vdp1_clock = 0;
    nbCmdToProcess = 0;
    needVdp1draw = 0;
  }
}
//////////////////////////////////////////////////////////////////////////////
static u32 lastVRamBankCol = 0;
 
/* ---------------------------------------------------------------------------
 * VDP1 busy / idle and the cost of touching its memories.
 *
 * VDP1 User's Manual ST-013-R3 p.19 (VRAM) and p.20 (frame buffer) describe
 * one arbitration rule for both memories:
 *
 *   "The order of priority of access of the VRAM is always: system controller
 *    (system controller IC) > drawing."
 *   "The order of priority of access of the frame buffer is always: system
 *    controller IC > drawing."
 *   "When access from the system controller is performed during drawing,
 *    drawing is interrupted and must wait."
 *   "Because access is performed after assigning an order of priority, there
 *    may be more than 10 wait cycles according to that timing, depending on
 *    the operating clock of the CPU."
 *   "Perform access from the CPU when drawing is not being performed in order
 *    to prevent interruption of drawing. To determine if drawing is being
 *    performed, poll the system registers, or use an interrupt signal."
 *
 * §4.4 p.52 states the idle half of the same rule from the other side: while
 * CEF = 1, "VRAM can be accessed without the overhead for stopping drawing
 * and without causing the CPU to wait."
 *
 * So both effects -- the VDP1 losing drawing cycles, and the CPU stalling --
 * exist only while the VDP1 is actually drawing. Neither happens when it is
 * idle. The rule applies to reads as well as writes ("read-write access"),
 * and to the SCU as much as the CPU, since the manual names the system
 * controller IC rather than the CPU as the higher-priority agent.
 * ------------------------------------------------------------------------- */
 
static INLINE int vdp1IsDrawing(void) {
  return (Vdp1External.status & VDP1_STATUS_MASK) == VDP1_STATUS_RUNNING;
}
 
/* p.19 gives "more than 10 wait cycles" as the figure for an access that
 * collides with drawing, without an exact number. 10 is the manual's own
 * lower bound and is used here as a conservative value; override at build
 * time to experiment. */
#ifndef VDP1_CPU_WAIT_WHILE_DRAWING
#define VDP1_CPU_WAIT_WHILE_DRAWING 10
#endif
 
/* Charge one access to the VDP1 VRAM or frame buffer.
 * `cycles` is the bus occupancy of the access itself, always paid.
 * A collision with drawing adds the arbitration stall on top, and steals
 * the same occupancy from the VDP1's own per-line budget. */
static INLINE void vdp1BusAccess(SH2_struct *context, int cycles) {
  int drawing = vdp1IsDrawing();
  if (context != NULL) {
    context->cycles += cycles;
    if (drawing) context->cycles += VDP1_CPU_WAIT_WHILE_DRAWING;
  }
  if (drawing) {
    vdp1_clock -= cycles;
    vdp1StatsCpu(cycles);
  }
}
 
/* Read variant. The row-bank crossing penalty at each read site already
 * accounts for the SH2-side bus cost, so only the drawing collision is added
 * here. A cached read hides the latency from the SH2 but not the bus
 * transaction from the VDP1, so the drawing budget is charged either way. */
static INLINE void vdp1BusRead(SH2_struct *context, int cycles) {
  if (!vdp1IsDrawing()) return;
  vdp1_clock -= cycles;
  vdp1StatsCpu(cycles);
  if ((context != NULL) && (!context->cacheOn))
    context->cycles += VDP1_CPU_WAIT_WHILE_DRAWING;
}
 
u8 FASTCALL Vdp1RamReadByte(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0x7FFFF;
   int rowBank = ((addr>>9)&0x3FF);
   if (rowBank != lastVRamBankCol) {
    lastVRamBankCol = rowBank;
    if ((context) && (!context->cacheOn)) context->cycles += 2;
   }
   /* p.19 covers "read-write access": a read collides with drawing exactly
    * as a write does. This path stole no drawing cycles at all. */
   vdp1BusRead(context, 2);
   return T1ReadByte(mem, addr);
}
 
//////////////////////////////////////////////////////////////////////////////
 
u16 FASTCALL Vdp1RamReadWord(SH2_struct *context, u8* mem, u32 addr) {
    addr &= 0x07FFFF;
    int rowBank = ((addr>>9)&0x3FF);
    if (rowBank != lastVRamBankCol) {
     lastVRamBankCol = rowBank;
     if ((context) && (!context->cacheOn)) context->cycles += 2;
    }
    /* p.19 covers "read-write access": a read collides with drawing exactly
    * as a write does. This path stole no drawing cycles at all. */
   vdp1BusRead(context, 2);
   return T1ReadWord(mem, addr);
}
 
//////////////////////////////////////////////////////////////////////////////
 
u32 FASTCALL Vdp1RamReadLong(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0x7FFFF;
   int rowBank = ((addr>>9)&0x3FF);
   if (rowBank != lastVRamBankCol) {
    lastVRamBankCol = rowBank;
    if ((context) && (!context->cacheOn)) context->cycles += 2;
   }
   /* p.19 covers "read-write access": a read collides with drawing exactly
    * as a write does. This path stole no drawing cycles at all. */
   vdp1BusRead(context, 2);
   return T1ReadLong(mem, addr);
}
 
//////////////////////////////////////////////////////////////////////////////
 
static int Vdp1LoopAddr = -1;
void FASTCALL Vdp1RamWriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val) {
   addr &= 0x7FFFF;
   int rowBank = ((addr>>9)&0x3FF);
   if (rowBank != lastVRamBankCol) {
    lastVRamBankCol = rowBank;
    if (context) context->cycles += 2;
   }
   if (context)context->cycles += 1;
 
   // printf("Write 0x%x @ 0x%x (%d %d)\n", val, addr, yabsys.LineCount, yabsys.DecilineCount);
   Vdp1External.updateVdp1Ram = 1;
   vdp1BusAccess(context, 1);
   if (vdp1Ram_update_start > addr) vdp1Ram_update_start = addr;
   if (vdp1Ram_update_end < addr+1) vdp1Ram_update_end = addr + 1;
   T1WriteByte(mem, addr, val);
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1RamWriteWord(SH2_struct *context, u8* mem, u32 addr, u16 val) {
   addr &= 0x7FFFF;
   int rowBank = ((addr>>9)&0x3FF);
   if (rowBank != lastVRamBankCol) {
    lastVRamBankCol = rowBank;
    if (context) context->cycles += 2;
   }
   if (context)context->cycles += 1;
   // printf("Write 0x%x @ 0x%x (%d %d)\n", val, addr, yabsys.LineCount, yabsys.DecilineCount);
   Vdp1External.updateVdp1Ram = 1;
   vdp1BusAccess(context, 2);
   if (vdp1Ram_update_start > addr) vdp1Ram_update_start = addr;
   if (vdp1Ram_update_end < addr+2) vdp1Ram_update_end = addr + 2;
   T1WriteWord(mem, addr, val);
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1RamWriteLong(SH2_struct *context, u8* mem, u32 addr, u32 val) {
   addr &= 0x7FFFF;
   int rowBank = ((addr>>9)&0x3FF);
   if (rowBank != lastVRamBankCol) {
    lastVRamBankCol = rowBank;
    if (context) context->cycles += 2;
   }
   if (context)context->cycles += 3;
   // printf("Write 0x%x @ 0x%x (%d %d)\n", val, addr, yabsys.LineCount, yabsys.DecilineCount);
   Vdp1External.updateVdp1Ram = 1;
   vdp1BusAccess(context, 4);
   if (vdp1Ram_update_start > addr) vdp1Ram_update_start = addr;
   if (vdp1Ram_update_end < addr+4) vdp1Ram_update_end = addr + 4;
   T1WriteLong(mem, addr, val);
}
 
//////////////////////////////////////////////////////////////////////////////
 
/* VDP1 Manual §1.2 Table 1.2 p.13 + §4.1 Table 4.2 p.42:
 * The frame-buffer dimensions are determined by TVMR.TVM (bits 2:0):
 *
 *   TVM=000 (Normal NTSC/PAL, 16bpp)  : 512H × 256V
 *   TVM=001 (Hi-Res NTSC/PAL,  8bpp)  : 1024H × 256V
 *   TVM=010 (Rotation 16bpp)          : 512H × 256V
 *   TVM=011 (Rotation 8bpp)           : 512H × 512V   <- only 512-tall mode
 *   TVM=100 (HDTV, 16bpp)             : 512H × 256V
 *
 * Per §4.2 p.41 DIE/DIL note: "even and odd fields are rendered in
 * different frame buffers" — DIE=1 (double interlace) does NOT enlarge
 * the frame buffer. Each FB stays 256 lines tall; the doubled vertical
 * resolution is achieved by alternating two FBs across fields.
 *
 * Therefore the height depends on TVM, not FBCR.DIE. */
static INLINE u32 vdp1FBHeight(void) {
    return ((Vdp1Regs->TVMR & 0x7) == 0x3) ? 512 : 256;
}
 
/* §1.2 / §4.1: only TVM=001 (Hi-Res 8bpp) is 1024 wide. All other
 * modes (including Rotation 8bpp TVM=011 which is 512×512) are 512. */
static INLINE u32 vdp1FBWidth(void) {
    return ((Vdp1Regs->TVMR & 0x7) == 0x1) ? 1024 : 512;
}
 
/* BUG CORRIGE : cette capacité était figée à 512*256 (131072), soit
 * exactement la moitié du vrai framebuffer Hi-Res (1024*256=262144,
 * cf. vdp1FBWidth() ci-dessus qui renvoie bien 1024 pour TVM=001).
 * Résultat : en Hi-Res, toute écriture (commande de dessin OU écriture
 * directe en mémoire par le jeu) au-delà du pixel 131072 était
 * silencieusement rejetée par vdp1FBPixInBounds(), tronquant à tort
 * la moitié du framebuffer alors que cette zone est parfaitement
 * valide en Hi-Res. On utilise maintenant vdp1FBWidth()*vdp1FBHeight()
 * dynamiquement, cohérent avec le TVM courant. */
static INLINE u32 vdp1FBPixCapacity(void) {
    return vdp1FBWidth() * vdp1FBHeight();
}
static INLINE int vdp1FBPixInBounds(u32 pixIdx) {
    return pixIdx < vdp1FBPixCapacity();
}
 
u8 FASTCALL Vdp1FrameBuffer16bReadByte(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr>>1;
   if (!vdp1FBPixInBounds(pixIdx)) return 0;
   u32* buf = getVDP1ReadFramebuffer();
   vdp1BusAccess(context, 2);
   /* Bus big-endian (SH-2) : dans un pixel 16 bits, l'octet a l'adresse
    * paire est le poids FORT, l'octet a l'adresse impaire le poids faible.
    * L'ancien code renvoyait toujours le poids faible. */
   u16 pix = T1ReadLong((u8*)buf, pixIdx*4) & 0xFFFF;
   u8 ret = (addr & 1) ? (u8)(pix & 0xFF) : (u8)(pix >> 8);
   PRINT_FB("R B 0x%x@0x%x\n", ret, addr);
   return ret;
}
 
//////////////////////////////////////////////////////////////////////////////
 
u16 FASTCALL Vdp1FrameBuffer16bReadWord(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr>>1;
   if (!vdp1FBPixInBounds(pixIdx)) return 0;
   u32* buf = getVDP1ReadFramebuffer();
   vdp1BusAccess(context, 2);
   PRINT_FB("R W 0x%x@0x%x (%d, %d)\n", T1ReadLong((u8*)buf, pixIdx*4) & 0xFFFF, addr, yabsys.LineCount, yabsys.DecilineCount);
   return T1ReadLong((u8*)buf, pixIdx*4) & 0xFFFF;
}
 
//////////////////////////////////////////////////////////////////////////////
 
u32 FASTCALL Vdp1FrameBuffer16bReadLong(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr>>1;
   u32* buf = getVDP1ReadFramebuffer();
   vdp1BusAccess(context, 4);
   /* Validation par pixel : un accès long au bord bas du FB peut avoir
    * pixIdx valide mais pixIdx+1 hors limites. */
   u16 val1 = vdp1FBPixInBounds(pixIdx)   ? (T1ReadLong((u8*)buf, (pixIdx  )*4) & 0xFFFF) : 0;
   u16 val2 = vdp1FBPixInBounds(pixIdx+1) ? (T1ReadLong((u8*)buf, (pixIdx+1)*4) & 0xFFFF) : 0;
   return (val1<<16) | val2;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1FrameBuffer16bWriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr>>1;
   if (!vdp1FBPixInBounds(pixIdx)) return;
   /* Une ecriture octet ne modifie que la moitie du pixel 16 bits qu'elle
    * vise (adresse paire = poids fort, impaire = poids faible, bus SH-2
    * big-endian). L'ancien code ecrasait tout le pixel avec l'octet place
    * en poids faible. On part donc de la valeur courante du pixel : celle
    * du tampon d'ecriture CPU s'il a deja ete ecrit (alpha != 0), sinon
    * celle du frame buffer relu. La relecture doit preceder
    * getVDP1WriteFramebuffer() : elle peut declencher l'effacement
    * differe du frame buffer, qui remappe le tampon d'ecriture. */
   u16 old;
   {
     u32* wb = _Ygl->vdp1fb_write_buf[_Ygl->drawframe];
     if ((wb != NULL) && ((wb[pixIdx] & 0xFF000000) != 0)) {
       old = wb[pixIdx] & 0xFFFF;
     } else {
       u32* rb = getVDP1ReadFramebuffer();
       old = (rb != NULL) ? (T1ReadLong((u8*)rb, pixIdx*4) & 0xFFFF) : 0;
     }
   }
   u16 nv = (addr & 1) ? (u16)((old & 0xFF00) | val) : (u16)((old & 0x00FF) | ((u16)val << 8));
   u32* buf = getVDP1WriteFramebuffer(_Ygl->drawframe);
   PRINT_FB("W B 0x%x@0x%x line %d(%d) frame %d\n", val, pixIdx, yabsys.LineCount, yabsys.DecilineCount, _Ygl->drawframe);
   buf[pixIdx] = nv|0xFF000000;
   syncVdp1FBBuffer(pixIdx);
   vdp1BusAccess(context, 2);
   _Ygl->FBDirty[_Ygl->drawframe] = 1;
   _Ygl->vdp1IsNotEmpty[_Ygl->drawframe] = yabsys.LineCount;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1FrameBuffer16bWriteWord(SH2_struct *context, u8* mem, u32 addr, u16 val) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr>>1;
   if (!vdp1FBPixInBounds(pixIdx)) return;
   u32* buf = getVDP1WriteFramebuffer(_Ygl->drawframe);
   PRINT_FB("W W 0x%x@0x%x line %d(%d) frame %d\n", val, pixIdx, yabsys.LineCount, yabsys.DecilineCount, _Ygl->drawframe);
   buf[pixIdx] = (val&0xFFFF)|0xFF000000;
   syncVdp1FBBuffer(pixIdx);
   vdp1BusAccess(context, 2);
   _Ygl->FBDirty[_Ygl->drawframe] = 1;
   _Ygl->vdp1IsNotEmpty[_Ygl->drawframe] = yabsys.LineCount;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1FrameBuffer16bWriteLong(SH2_struct *context, u8* mem, u32 addr, u32 val) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr>>1;
   u32* buf = getVDP1WriteFramebuffer(_Ygl->drawframe);
   PRINT_FB("W L 0x%x@0x%x line %d(%d) frame %d %s\n", val, addr, yabsys.LineCount, yabsys.DecilineCount, _Ygl->drawframe, (context==NULL)?"DMA":"CPU");
   if (vdp1FBPixInBounds(pixIdx)) {
     buf[pixIdx]   = ((val>>16)&0xFFFF)|0xFF000000;
     syncVdp1FBBuffer(pixIdx);
   }
   if (vdp1FBPixInBounds(pixIdx+1)) {
     buf[pixIdx+1] = ((val    )&0xFFFF)|0xFF000000;
     syncVdp1FBBuffer(pixIdx+1);
   }
   vdp1BusAccess(context, 4);
   _Ygl->FBDirty[_Ygl->drawframe] = 1;
   _Ygl->vdp1IsNotEmpty[_Ygl->drawframe] = yabsys.LineCount;
}
 
/* ====================== 8 bpp ====================================== */
 
u8 FASTCALL Vdp1FrameBuffer8bReadByte(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr;                       /* 8 bpp : 1 octet = 1 pixel */
   if (!vdp1FBPixInBounds(pixIdx)) return 0;
   u32* buf = getVDP1ReadFramebuffer();
   vdp1BusAccess(context, 2);
   PRINT_FB("R B 0x%x@0x%x\n", buf[pixIdx]&0xFF, addr);
   return T1ReadLong((u8*)buf, pixIdx*4) & 0xFF;
}
 
//////////////////////////////////////////////////////////////////////////////
 
u16 FASTCALL Vdp1FrameBuffer8bReadWord(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr;
   u32* buf = getVDP1ReadFramebuffer();
   vdp1BusAccess(context, 2);
   PRINT_FB("R W 0x%x@0x%x (%d, %d)\n", T1ReadLong((u8*)buf, pixIdx*4) & 0xFFFF, addr, yabsys.LineCount, yabsys.DecilineCount);
   u8 val1 = vdp1FBPixInBounds(pixIdx)   ? (T1ReadLong((u8*)buf, (pixIdx  )*4) & 0xFF) : 0;
   u8 val2 = vdp1FBPixInBounds(pixIdx+1) ? (T1ReadLong((u8*)buf, (pixIdx+1)*4) & 0xFF) : 0;
   return (val1<<8) | val2;
}
 
//////////////////////////////////////////////////////////////////////////////
 
u32 FASTCALL Vdp1FrameBuffer8bReadLong(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr;
   u32* buf = getVDP1ReadFramebuffer();
   vdp1BusAccess(context, 4);
   u8 val1 = vdp1FBPixInBounds(pixIdx)   ? (T1ReadLong((u8*)buf, (pixIdx  )*4) & 0xFF) : 0;
   u8 val2 = vdp1FBPixInBounds(pixIdx+1) ? (T1ReadLong((u8*)buf, (pixIdx+1)*4) & 0xFF) : 0;
   u8 val3 = vdp1FBPixInBounds(pixIdx+2) ? (T1ReadLong((u8*)buf, (pixIdx+2)*4) & 0xFF) : 0;
   u8 val4 = vdp1FBPixInBounds(pixIdx+3) ? (T1ReadLong((u8*)buf, (pixIdx+3)*4) & 0xFF) : 0;
   PRINT_FB("R L 0x%x@0x%x\n", (val1<<24)|(val2<<16)|(val3<<8)|val4, addr);
   return (val1<<24) | (val2<<16) | (val3<<8) | val4;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1FrameBuffer8bWriteByte(SH2_struct *context, u8* mem, u32 addr, u8 val) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr;
   if (!vdp1FBPixInBounds(pixIdx)) return;
   u32* buf = getVDP1WriteFramebuffer(_Ygl->drawframe);
   PRINT_FB("W B 0x%x@0x%x line %d(%d) frame %d\n", val, pixIdx, yabsys.LineCount, yabsys.DecilineCount, _Ygl->drawframe);
   buf[pixIdx] = (val&0xFF)|0xFF000000;
   syncVdp1FBBuffer(pixIdx);
   vdp1BusAccess(context, 2);
   _Ygl->FBDirty[_Ygl->drawframe] = 1;
   _Ygl->vdp1IsNotEmpty[_Ygl->drawframe] = yabsys.LineCount;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1FrameBuffer8bWriteWord(SH2_struct *context, u8* mem, u32 addr, u16 val) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr;
   u32* buf = getVDP1WriteFramebuffer(_Ygl->drawframe);
   PRINT_FB("W W 0x%x@0x%x line %d(%d) frame %d\n", val, pixIdx, yabsys.LineCount, yabsys.DecilineCount, _Ygl->drawframe);
   if (vdp1FBPixInBounds(pixIdx)) {
     buf[pixIdx]   = ((val>>8)&0xFF)|0xFF000000;
     syncVdp1FBBuffer(pixIdx);
   }
   if (vdp1FBPixInBounds(pixIdx+1)) {
     buf[pixIdx+1] = ( val    &0xFF)|0xFF000000;
     syncVdp1FBBuffer(pixIdx+1);
   }
   vdp1BusAccess(context, 2);
   _Ygl->FBDirty[_Ygl->drawframe] = 1;
   _Ygl->vdp1IsNotEmpty[_Ygl->drawframe] = yabsys.LineCount;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1FrameBuffer8bWriteLong(SH2_struct *context, u8* mem, u32 addr, u32 val) {
   addr &= 0x3FFFF;
   u32 pixIdx = addr;
   u32* buf = getVDP1WriteFramebuffer(_Ygl->drawframe);
   PRINT_FB("W L 0x%x@0x%x line %d(%d) frame %d %s\n", val, pixIdx, yabsys.LineCount, yabsys.DecilineCount, _Ygl->drawframe, (context==NULL)?"DMA":"CPU");
   if (vdp1FBPixInBounds(pixIdx)) {
     buf[pixIdx]   = ((val>>24)&0xFF)|0xFF000000;
     syncVdp1FBBuffer(pixIdx);
   }
   if (vdp1FBPixInBounds(pixIdx+1)) {
     buf[pixIdx+1] = ((val>>16)&0xFF)|0xFF000000;
     syncVdp1FBBuffer(pixIdx+1);
   }
   if (vdp1FBPixInBounds(pixIdx+2)) {
     buf[pixIdx+2] = ((val>> 8)&0xFF)|0xFF000000;
     syncVdp1FBBuffer(pixIdx+2);
   }
   if (vdp1FBPixInBounds(pixIdx+3)) {
     buf[pixIdx+3] = ( val     &0xFF)|0xFF000000;
     syncVdp1FBBuffer(pixIdx+3);
   }
   vdp1BusAccess(context, 4);
   _Ygl->FBDirty[_Ygl->drawframe] = 1;
   _Ygl->vdp1IsNotEmpty[_Ygl->drawframe] = yabsys.LineCount;
}
 
//////////////////////////////////////////////////////////////////////////////
 
//////////////////////////////////////////////////////////////////////////////
 
int Vdp1Init(void) {
   if ((Vdp1Regs = (Vdp1 *) malloc(sizeof(Vdp1))) == NULL)
      return -1;
 
   if ((Vdp1Ram = T1MemoryInit(0x80000)) == NULL)
      return -1;
 
   Vdp1External.disptoggle = 1;
   Vdp1External.blocked = 0;
 
   Vdp1Regs->TVMR = 0;
   Vdp1Regs->FBCR = 0;
   Vdp1Regs->PTMR = 0;
 
   /* Chapitre 9 du manuel VDP1 : le coin haut-gauche du clipping systeme est
    * fixe a (0,0) par le materiel, et la plage de coordonnees de clipping va
    * de (0,0) a (1023,511) -- bornes INCLUSIVES, comme le confirme l'usage
    * en pre-clipping (x1 > scx2).
    *
    * Ces champs n'etaient ecrits nulle part dans vdp1.c : les seules
    * affectations de l'arbre sont dans VIDCSVdp1SystemClipping (vidcs.c),
    * donc apres la premiere commande de clipping systeme du jeu. Comme
    * Vdp1Init() alloue Vdp1Regs par malloc() sans memset, le pre-clipping
    * lisait jusque-la des valeurs de tas : selon leur contenu, tout etait
    * rejete ou rien ne l'etait, de facon non deterministe. */
   Vdp1Regs->systemclipX1 = 0;
   Vdp1Regs->systemclipY1 = 0;
   Vdp1Regs->systemclipX2 = 1023;
   Vdp1Regs->systemclipY2 = 511;
 
   Vdp1Regs->userclipX1 = 0;
   Vdp1Regs->userclipY1 = 0;
   /* Bornes INCLUSIVES : ces champs recoivent ensuite cmd->CMDXC / CMDYC,
    * qui sont inclusives, et sont consommes comme telles par CAP(). Le reset
    * a 1024/512 melangeait donc deux conventions et pouvait borner une part
    * a x=1024, une colonne au-dela du dernier index d'un frame buffer 1024
    * de large. Chapitre 9 : plage de clipping (0,0)-(1023,511). */
   Vdp1Regs->userclipX2 = 1023;
   Vdp1Regs->userclipY2 = 511;
   Vdp1Regs->userclipMode = 0; // VDP1 Manual §6.3 Cmod: default = inside drawing mode
 
   Vdp1Regs->localX = 0;
   Vdp1Regs->localY = 0;
 
   VDP1_MASK = 0xFFFF;
 
   vdp1Ram_update_start = 0x80000;
   vdp1Ram_update_end = 0x0;
 
   _Ygl->shallVdp1Erase[0] = 1;
   _Ygl->shallVdp1Erase[1] = 1;
 
   Vdp1EraseLatchEWDR = 0;
   Vdp1EraseLatchEWLR = 0;
   Vdp1EraseLatchEWRR = 0;
 
   return 0;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void Vdp1DeInit(void) {
   if (Vdp1Regs)
      free(Vdp1Regs);
   Vdp1Regs = NULL;
 
   if (Vdp1Ram)
      T1MemoryDeInit(Vdp1Ram);
   Vdp1Ram = NULL;
 
}
 
//////////////////////////////////////////////////////////////////////////////
 
int VideoInit(int coreid) {
   return VideoChangeCore(coreid);
}
 
//////////////////////////////////////////////////////////////////////////////
 
int VideoChangeCore(int coreid)
{
   int i;
 
   // Make sure the old core is freed
   VideoDeInit();
 
   // So which core do we want?
   if (coreid == VIDCORE_DEFAULT)
      coreid = 0; // Assume we want the first one
 
   // Go through core list and find the id
   for (i = 0; VIDCoreList[i] != NULL; i++)
   {
      if (VIDCoreList[i]->id == coreid)
      {
         // Set to current core
         VIDCore = VIDCoreList[i];
         break;
      }
   }
 
   if (VIDCore == NULL)
      return -1;
 
   if (VIDCore->Init() != 0)
      return -1;
 
   // Reset resolution/priority variables
   if (Vdp2Regs)
   {
      VIDCore->Vdp1Reset();
   }
 
   return 0;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void VideoDeInit(void) {
   if (VIDCore)
      VIDCore->DeInit();
   VIDCore = NULL;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void Vdp1Reset(void) {
   FRAMELOG("Reset Vdp1\n");
   Vdp1Regs->PTMR = 0;
   Vdp1Regs->MODR = 0x1000; // VDP1 Version 1
   Vdp1Regs->TVMR = 0;
   switchFB16bit();
   Vdp1Regs->ENDR = 0;
   VDP1_MASK = 0xFFFF;
   VIDCore->Vdp1Reset();
   vdp1_clock = 0;
   Vdp1LatchEraseParameters();
   vdp1StatsReset();
}
 
int VideoSetSetting( int type, int value )
{
	if (VIDCore) VIDCore->SetSettingValue( type, value );
	return 0;
}
 
 
//////////////////////////////////////////////////////////////////////////////
 
u8 FASTCALL Vdp1ReadByte(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0xFF;
   LOG("trying to byte-read a Vdp1 register\n");
   return 0;
}
 
//////////////////////////////////////////////////////////////////////////////
u16 FASTCALL Vdp1ReadWord(SH2_struct *context, u8* mem, u32 addr) {
   addr &= 0xFF;
   switch(addr) {
      case 0x10:
        FRAMELOG("Read EDSR %X line = %d (%d)\n", Vdp1Regs->EDSR, yabsys.LineCount, yabsys.DecilineCount);
        if (Vdp1External.checkEDSR == 0) {
          if (VIDCore != NULL)
            if (VIDCore->FinsihDraw != NULL)
              VIDCore->FinsihDraw();
        }
        Vdp1External.checkEDSR = 1;
        return Vdp1Regs->EDSR;
      case 0x12:
        FRAMELOG("Read LOPR %X line = %d\n", Vdp1Regs->LOPR, yabsys.LineCount);
         return Vdp1Regs->LOPR;
      case 0x14:
        FRAMELOG("Read COPR %X line = %d\n", Vdp1Regs->COPR, yabsys.LineCount);
         return Vdp1Regs->COPR;
      case 0x16:
         return 0x1000 | ((Vdp1Regs->PTMR & 2) << 7) | ((Vdp1Regs->FBCR & 0x1E) << 3) | (Vdp1Regs->TVMR & 0xF);
      default:
         LOG("trying to read a Vdp1 write-only register\n");
   }
   return 0;
}
 
//////////////////////////////////////////////////////////////////////////////
 
u32 FASTCALL Vdp1ReadLong(SH2_struct *context, u8* mem, u32 addr) {
   /* VDP1 registers are 16-bit; a 32-bit bus access spans two adjacent
    * registers. The Saturn SH2 bus is big-endian, so the word at the lower
    * address is the upper 16 bits (identical convention to Vdp2ReadLong).
    * The previous stub returned 0, so any 32-bit status poll (e.g. reading
    * EDSR:LOPR at 0x10 as a long) got zero instead of the real status. */
   u16 hi = Vdp1ReadWord(context, mem, addr);
   u16 lo = Vdp1ReadWord(context, mem, addr + 2);
   return ((u32)hi << 16) | lo;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1WriteByte(SH2_struct *context, u8* mem, u32 addr, UNUSED u8 val) {
   addr &= 0xFF;
   LOG("trying to byte-write a Vdp1 register - %08X\n", addr);
}
 
//////////////////////////////////////////////////////////////////////////////
 
static u8 FBCRChangeUpdated = 0;
/* VDP1 Manual §4.2 Table 4.3 p.41-42: frame buffer mode from
 * (VBE=TVMR[3], FCT=FBCR[1], FCM=FBCR[0]). Returns a bitfield:
 *   bit 0 = useVBlankErase
 *   bit 1 = manualchange
 *   bit 2 = onecyclechange
 *   bit 3 = manualerase
 *   bit 4 = onecycleerase
 * Prohibited combinations (VBE=1 without FCT=FCM=1) are treated
 * as if all three were 1 — matches hardware observed behaviour. */
 
static u8 decodeFBCRMode(void) {
    int vbe = (Vdp1Regs->TVMR >> 3) & 0x1;
    int fcm = (Vdp1Regs->FBCR >> 1) & 0x1;
    int fct =  Vdp1Regs->FBCR       & 0x1;
 
    /* VBE=1 : V-blank erase + bascule manuelle. Les 3 combos VBE=1
     * interdites (FCM/FCT != 11) se comportent comme (1,1,1) sur HW. */
    if (vbe == 1) return 0x03;
 
    /* VBE=0, FCM=0 : mode 1-cycle (erase + change auto). FCT est ignoré
     * (sélecteur = FCM), ce qui couvre (0,0,0) légal et (0,0,1) interdit. */
    if (fcm == 0) return 0x14;
 
    /* VBE=0, FCM=1 : mode manuel. FCT distingue erase / change. */
    if (fct == 0) return 0x08;   /* (0,1,0) manual erase  */
    return 0x02;                 /* (0,1,1) manual change */
}
 
 
static void updateFBCRChange() {
  if (FBCRChangeUpdated == 0) return;
  u8 m = decodeFBCRMode();
  Vdp1External.manualchange   = (m >> 1) & 0x1;
  Vdp1External.onecyclechange = (m >> 2) & 0x1;
  FBCRChangeUpdated = 0;
}
 
static u8 FBCREraseUpdated = 0;
/* Effacement programme au changement de frame buffer.
 *
 * VDP1 User's Manual ST-013-R3 §4.2, Table 4.3(a) p.41 : au passage du mode
 * 1-cycle au mode manuel (change) (note 3), la ligne ou FCM = FCT = 1 est
 * ecrit montre encore "Display and erase/write" : c'est le champ en cours,
 * regi par le mode 1-cycle en vigueur lors du changement precedent. Le champ
 * suivant, premier champ en mode manuel, est "Display / Draw" : le buffer
 * affiche n'est PAS efface. p.39, Change (Manual Mode) : "Because erase/write
 * is not performed, it is necessary to specify erase in the prior field".
 *
 * Mednafen (ss/vdp1.c, fin de V-blank : effacement seulement si FCM = 0 ou
 * si FCM = 1, FCT = 0 est en attente) et Ymir (VDP::BeginHPhaseLeftBorder,
 * erase = !fbSwapMode ou declenchement manuel avec FCT = 0) font de meme :
 * aucun effacement supplementaire a la transition.
 *
 * Kronos ajoutait ici un "dernier effacement" (onelasterase) quand FCM
 * passait de 0 a 1, introduit pour Return Fire (f692be74d). Il efface le
 * buffer affiche au premier champ manuel, que le jeu a deja trace. Gex, en
 * pause, passe de FBCR = 00 a FBCR = 03 et trace la scene figee dans les
 * DEUX buffers (une fois dans chacun) avant de ne plus tracer que le menu :
 * l'effacement en trop detruisait la scene dans l'un d'eux, et l'affichage
 * alternait a chaque champ entre la scene + menu et le menu seul.
 *
 * Le champ onelasterase reste dans Vdp1External_struct (taille des
 * sauvegardes d'etat, affichage de debogage) mais n'est plus arme. */
static void updateFBCRErase() {
  if (FBCREraseUpdated == 0) return;
  u8 m = decodeFBCRMode();
  Vdp1External.onecycleerase = (m >> 4) & 0x1;
  Vdp1External.onelasterase  = 0;
  Vdp1External.manualerase   = (m >> 3) & 0x1;
  FBCREraseUpdated = 0;
}
static void updateFBCRVBE() {
	Vdp1External.useVBlankErase = decodeFBCRMode() & 0x1;
}
 
/* Instantane de la VRAM VDP1 pris au declenchement du trace.
 *
 * Un debogueur doit montrer la liste de commandes REELLEMENT tracee, pas
 * l'etat de la VRAM a l'instant ou l'on ouvre la fenetre. Un jeu qui
 * reconstruit sa table a chaque trame -- Doom, par exemple -- a souvent,
 * au moment ou on regarde, deja remis sa liste a zero : on ne voit plus
 * qu'un polygone d'effacement suivi d'un END, alors que la trame affichee
 * a l'ecran a bien ete tracee a partir de plusieurs dizaines de commandes.
 *
 * La copie n'est faite que si l'interface de debogue l'a demandee
 * (Vdp1DebugSetCapture), pour ne pas payer un memcpy de 512 Ko par trame
 * en fonctionnement normal. */
/* Edits de l'editeur graphique (vdpedit.h) : NULL tant qu'aucun edit
 * n'existe ; vdpedit.c l'installe. */
void (*VdpEditHookVdp1)(void) = NULL;

static u8 *Vdp1DebugFrameRam = NULL;
static int Vdp1DebugCaptureEnabled = 0;
static int Vdp1DebugFrameValid = 0;
 
/* Compteur d'utilisateurs : le debogueur VDP1 et l'editeur graphique peuvent
 * etre ouverts ensemble ; fermer l'un ne doit pas liberer l'instantane que
 * l'autre lit encore. Chaque Vdp1DebugSetCapture(1) est equilibre par un
 * Vdp1DebugSetCapture(0). */
void Vdp1DebugSetCapture(int enable)
{
   if (enable)
      Vdp1DebugCaptureEnabled++;
   else if (Vdp1DebugCaptureEnabled > 0)
      Vdp1DebugCaptureEnabled--;
   /* Couts par commande du trace capture (vdp1StatsCmd). */
   vdp1CmdCostEnable(Vdp1DebugCaptureEnabled > 0);
   if (Vdp1DebugCaptureEnabled == 0) {
      free(Vdp1DebugFrameRam);
      Vdp1DebugFrameRam = NULL;
      Vdp1DebugFrameValid = 0;
   }
}
 
u8 *Vdp1DebugGetFrameRam(void)
{
   return Vdp1DebugFrameValid ? Vdp1DebugFrameRam : NULL;
}
 
/* Memoire que lisent TOUTES les fonctions Vdp1Debug* : l'instantane pris au
 * debut du dernier trace s'il existe, la VRAM vivante sinon.
 *
 * La capture existait (Vdp1DebugSetCapture / Vdp1DebugCaptureFrame) mais
 * personne ne l'activait ni ne la lisait : l'interface parcourait toujours
 * Vdp1Ram, c'est-a-dire l'etat de la VRAM a l'instant ou l'emulation a ete
 * mise en pause, pas la liste effectivement tracee. Un jeu qui reecrit ses
 * caracteres ou sa table pendant la trame montrait alors des textures qui
 * n'etaient pas celles du trace (sprites vides, mauvais caracteres). */
static u8 *Vdp1DebugMem(void)
{
   return Vdp1DebugFrameValid && (Vdp1DebugFrameRam != NULL)
          ? Vdp1DebugFrameRam : Vdp1Ram;
}
 
u8 *Vdp1DebugGetRam(void)
{
   return Vdp1DebugMem();
}
 
static void Vdp1DebugCaptureFrame(void)
{
   if (!Vdp1DebugCaptureEnabled || (Vdp1Ram == NULL))
      return;
   if (Vdp1DebugFrameRam == NULL)
      Vdp1DebugFrameRam = (u8 *)malloc(0x80000);
   if (Vdp1DebugFrameRam == NULL)
      return;
   memcpy(Vdp1DebugFrameRam, Vdp1Ram, 0x80000);
   Vdp1DebugFrameValid = 1;
   /* Nouveau trace : les couts par commande repartent de zero, pour
    * correspondre a la table qui vient d'etre capturee. */
   vdp1CmdCostClear();
}
 
static void Vdp1TryDraw(void) {
  if ((yabsys.LineCount >= yabsys.MaxLineCount-2) && (yabsys.LineCount <= yabsys.MaxLineCount-1)) return;
  if ((oldNeedVdp1draw == 0) && (needVdp1draw != 0)) {
    /* Transition 0 -> 1 : une nouvelle passe de trace commence, la table de
     * commandes est dans son etat definitif pour cette trame. */
    /* Edits de l'editeur graphique (vdpedit.c) : remis en VRAM avant le
     * trace -- et avant la capture, pour que le debogueur montre la meme
     * chose que l'ecran. Pointeur NULL tant qu'aucun edit n'existe. */
    if (VdpEditHookVdp1) VdpEditHookVdp1();
    Vdp1DebugCaptureFrame();
    FRAMELOG("Shift EDSR\n");
    Vdp1Regs->EDSR >>= 1;
    checkFBSync();
    FRAMELOG("Will drawn on frame %d\n",_Ygl->drawframe);
  }
  oldNeedVdp1draw = needVdp1draw;
  if (needVdp1draw == 1) {
    CmdListInLoop = 0;
    needVdp1draw = Vdp1Draw();
  }
}
 
static int Vdp1FBDraw(void) {
  if (VIDCore->Vdp1FBDraw){
    VIDCore->Vdp1FBDraw();
  }
  return 1;
}
 
static void checkFBSync() {
  int needClearFB = 0;
  if (_Ygl->vdp1IsNotEmpty[_Ygl->drawframe] != -1) {
    //FB has been accessed
    FRAMELOG("Update FB Direct access on frame %d line %d(%d)\n", _Ygl->drawframe, yabsys.LineCount, yabsys.DecilineCount);
    updateVdp1DrawingFBMem(_Ygl->drawframe);
    needClearFB = 1;
    Vdp1FBDraw();
    _Ygl->vdp1IsNotEmpty[_Ygl->drawframe] = -1;
    if (needClearFB != 0) clearVDP1Framebuffer(_Ygl->drawframe);
  }
}
 
void FASTCALL Vdp1WriteWord(SH2_struct *context, u8* mem, u32 addr, u16 val) {
  addr &= 0xFF;
 
  switch(addr) {
    case 0x00: // TVMR
      /* Le BIOS peut écrire des bits de test ici. 
         On applique le masque mais on garde la logique de switch FB */
      if ((val & 0x1) != (Vdp1Regs->TVMR & 0x1)) {
        if (val & 0x1) switchFB8bit();
        else switchFB16bit();
        if (VIDCore->startVdp1Render) VIDCore->startVdp1Render();
      }
      Vdp1Regs->TVMR = val & 0x000F; // Masque hardware strict
      updateFBCRVBE();
      break;
 
    case 0x02: // FBCR
      /* IMPORTANT : Le BIOS utilise le mode manuel pour l'animation des cristaux.
         On ne doit pas filtrer trop agressivement ici.
         Pas d'effacement supplementaire au passage 1-cycle -> manuel :
         voir updateFBCRErase(). */
      Vdp1Regs->FBCR = val & 0x001F; 
      FBCREraseUpdated = 1;
      FBCRChangeUpdated = 1;
      break;
 
	case 0x04: // PTMR
      val &= 0x0003;
      if (val == 0x3) val = 0x2;
      Vdp1Regs->PTMR = val;
      if (val == 1){
        /* Do NOT clear CEF here.
         *
         * VDP1 Manual §4.4 pp.52-53 defines a single atomic action at the
         * start of drawing: BEF takes the value of CEF, and CEF is reset
         * to 0. Vdp1TryDraw() already performs exactly that, as EDSR >>= 1,
         * on the idle -> drawing transition -- and it is called three lines
         * below, in the same register write.
         *
         * Clearing CEF first made that shift copy an already-zeroed CEF into
         * BEF, so on every PTMR = 1 trigger BEF came out 0 no matter what the
         * previous frame had done. BEF = 0 means "the end bit in previous
         * frame has not been fetched", i.e. transfer-over, so the register
         * permanently reported an overrun to any title polling it. The
         * comment that stood here asserted BEF was unaffected; the call to
         * Vdp1TryDraw() on the next lines is what made that untrue. */
        checkFBSync();
        abortVdp1();
        vdp1_clock += getVdp1CyclesPerLine();
        RequestVdp1ToDraw();
        Vdp1TryDraw();
      }
      break;
 
    case 0x06: // EWDR
      Vdp1Regs->EWDR = val;
      break;
 
    case 0x08: // EWLR
    case 0x0A: // EWRR
      /* On retire le masque 0x3F3F qui cassait probablement l'effacement du BIOS.
         Certains jeux utilisent ces registres pour des effets de fondu. */
      if (addr == 0x08) Vdp1Regs->EWLR = val;
      else Vdp1Regs->EWRR = val;
      break;
 
    case 0x0C: // ENDR
      Vdp1Regs->ENDR = val;
      /* ENDR only stops a drawing in progress (VDP1 manual ST-013-R3, Draw
       * Forced Termination Register). It does not touch EDSR: Mednafen
       * (ss/vdp1.c, case 0x6) just ends DrawingActive, and EDSR only changes
       * at the start of a drawing (CEF <- 0), at the draw end command
       * (CEF <- 1) and at the frame buffer swap (BEF <- CEF, CEF <- 0).
       * Clearing CEF here, which a comment presented as "so that the BIOS
       * sees the VDP1 ready", made it look NOT finished: Thunderstrike 2
       * writes ENDR in its V-Blank IN handler every frame (0600425C), so
       * the CEF set by the end of its one-shot list (PTMR = 1) was wiped
       * before its main loop polled it, and the loop at 060047A4-060047AE
       * waited for CEF forever (black screen, VDP1 at 0 fps). */
      abortVdp1();
      break;
 
    default:
      break;
  }
}
 
//////////////////////////////////////////////////////////////////////////////
 
void FASTCALL Vdp1WriteLong(SH2_struct *context, u8* mem, u32 addr, u32 val) {
   /* VDP1 registers are 16-bit; a 32-bit bus access targets two adjacent
    * registers. Big-endian SH2: the word at the lower address holds the
    * upper 16 bits (mirrors Vdp2WriteLong). Delegating to Vdp1WriteWord
    * preserves every side effect the register write triggers — FB 8/16-bit
    * switch + startVdp1Render on TVMR, draw trigger + EDSR.CEF clear on PTMR,
    * erase-coordinate updates on EWLR/EWRR, ENDR abort. The previous no-op
    * silently dropped all of it; a game or BIOS storing the erase pair
    * (EWLR:EWRR) or TVMR:FBCR as a single long lost the write entirely. */
   Vdp1WriteWord(context, mem, addr,     (val >> 16) & 0xFFFF);
   Vdp1WriteWord(context, mem, addr + 2,  val        & 0xFFFF);
}
 
static void printCommand(vdp1cmd_struct *cmd) {
  printf("===== CMD =====\n");
  printf("CMDCTRL = 0x%x\n",cmd->CMDCTRL );
  printf("CMDLINK = 0x%x\n",cmd->CMDLINK );
  printf("CMDPMOD = 0x%x\n",cmd->CMDPMOD );
  printf("CMDCOLR = 0x%x\n",cmd->CMDCOLR );
  printf("CMDSRCA = 0x%x\n",cmd->CMDSRCA );
  printf("CMDSIZE = 0x%x\n",cmd->CMDSIZE );
  printf("CMDXA = 0x%x\n",cmd->CMDXA );
  printf("CMDYA = 0x%x\n",cmd->CMDYA );
  printf("CMDXB = 0x%x\n",cmd->CMDXB );
  printf("CMDYB = 0x%x\n",cmd->CMDYB );
  printf("CMDXC = 0x%x\n",cmd->CMDXC );
  printf("CMDYC = 0x%x\n",cmd->CMDYC );
  printf("CMDXD = 0x%x\n",cmd->CMDXD );
  printf("CMDYD = 0x%x\n",cmd->CMDYD );
  printf("CMDGRDA = 0x%x\n",cmd->CMDGRDA );
}
 
static int emptyCmd(vdp1cmd_struct *cmd) {
  return (
    (cmd->CMDCTRL == 0) &&
    (cmd->CMDLINK == 0) &&
    (cmd->CMDPMOD == 0) &&
    (cmd->CMDCOLR == 0) &&
    (cmd->CMDSRCA == 0) &&
    (cmd->CMDSIZE == 0) &&
    (cmd->CMDXA == 0) &&
    (cmd->CMDYA == 0) &&
    (cmd->CMDXB == 0) &&
    (cmd->CMDYB == 0) &&
    (cmd->CMDXC == 0) &&
    (cmd->CMDYC == 0) &&
    (cmd->CMDXD == 0) &&
    (cmd->CMDYD == 0) &&
    (cmd->CMDGRDA == 0));
}
 
//////////////////////////////////////////////////////////////////////////////
 
/* ---------------------------------------------------------------------------
 * Modele de cycles de l'emulateur : c'est lui qui regle vdp1_clock, donc la
 * duree du trace emule (transfer-over, fin de liste, EDSR.CEF...).
 *
 * Base : VDP1 Manual ST-013-R3 §2.5 p.20, "the data for 1 pixel is drawn in
 * sync with" l'horloge de 28 MHz -> 1 cycle par pixel ecrit.
 *
 * Corrections :
 *
 *  - Frame buffer 8 bits : la largeur n'est plus divisee par 2. Le "erase/
 *    write is performed 2 pixels at a time when the frame buffer depth is 8
 *    bits/pixel" du manuel (p.46) concerne l'EFFACEMENT seulement ; rien
 *    n'indique que le trace ecrive deux pixels par cycle (Mednafen compte
 *    1 cycle par pixel dans les deux profondeurs, vdp1_common.h
 *    VDP1_PlotPixel). Les jeux en 8 bits (haute resolution, rotation)
 *    etaient sous-estimes de moitie.
 *
 *  - Pre-clipping : la zone de reference est la zone de trace "specified by
 *    the CPU" (ST-013-SP1 p.2-3, Pre-Clipping Disable) : la zone utilisateur
 *    quand le clipping utilisateur est actif en mode interieur (CMDPMOD
 *    Clip = 1, Cmod = 0), la zone systeme sinon. Les sommets etaient bornes
 *    par la zone utilisateur meme clipping utilisateur desactive : un jeu qui
 *    laissait une petite zone utilisateur rendait ses commandes presque
 *    gratuites.
 *
 *  - Calcul couleur : MSB On, ombre, demi-transparence et Gouraud + demi-
 *    transparence (CCB bit 0 = 1) relisent le pixel du frame buffer avant de
 *    l'ecrire. Le manuel ne chiffre pas ce cout ; la valeur est celle de
 *    Mednafen (VDP1_PlotPixel, "ret += 5" : 6 cycles par pixel au lieu de 1).
 *
 *  - Lignes et polylignes : comptees (getLineCycles) ; elles ne coutaient
 *    aucun cycle.
 *
 * Non modelise ici (voir l'estimation detaillee vdp1EstQuad, observationnelle) :
 * le cout fixe par ligne (dont "up to five CPU clock cycles for one line" de
 * detection de pre-clipping, ST-013-SP1 p.3), les pixels d'antialiasing et
 * le parcours des pixels hors zone.
 * ------------------------------------------------------------------------- */
#define CAP(L,A,H) (((A)<(L))?(L):(((A)>(H))?(H):(A)))

/* Zone de trace de la commande (pre-clipping, lignes). */
static void getDrawArea(const vdp1cmd_struct *cmd, int *lx, int *hx, int *ly, int *hy) {
    if ((cmd->CMDPMOD & 0x0400) && !(cmd->CMDPMOD & 0x0200)) {
        *lx = Vdp1Regs->userclipX1; *hx = Vdp1Regs->userclipX2;
        *ly = Vdp1Regs->userclipY1; *hy = Vdp1Regs->userclipY2;
    } else {
        *lx = 0; *hx = Vdp1Regs->systemclipX2;
        *ly = 0; *hy = Vdp1Regs->systemclipY2;
    }
}

/* Cycles par pixel ecrit : 6 si le frame buffer est relu (MSB On, ou
 * CCB bit 0 = 1 : ombre, demi-transparence, Gouraud + demi-transparence). */
static INLINE int getPixelCycles(const vdp1cmd_struct *cmd) {
    return ((cmd->CMDPMOD & 0x8000) || (cmd->CMDPMOD & 0x0001)) ? 6 : 1;
}

/* Diviseur de lecture de texture (CMDPMOD bits 5-3) : mots VRAM de 16 bits.
 * 4 bpp -> 4 texels par mot, 8 bpp -> 2, 16 bpp -> 1. */
static INLINE int getTextureWords(const vdp1cmd_struct *cmd, int texels) {
    switch ((cmd->CMDPMOD >> 3) & 0x7) {
        case 0: case 1: return texels >> 2;
        case 2: case 3: case 4: return texels >> 1;
        default: return texels;
    }
}

static int getNormalCycles(vdp1cmd_struct *cmd) {
    /* Sprite normal : w x h pixels ecrits, sans agrandissement. */
    return MAX(cmd->w, 1) * MAX(cmd->h, 1) * getPixelCycles(cmd);
}

static int getScaledCycles(vdp1cmd_struct *cmd) {
    /* Sprite agrandi : rw x rh pixels ecrits. La lecture de texture (cmdW
     * mots par ligne, divisee par 2 en reduction avec HSS = 1, ST-013-SP1
     * p.2) borne le cout par le bas quand le sprite est fortement reduit. */
    int ax = cmd->CMDXA;
    int ay = cmd->CMDYA;
    int bx = cmd->CMDXB;
    int dy = cmd->CMDYD;
    int cmdW, rw, rh;
    if (!(cmd->CMDPMOD & 0x800)) {   /* pre-clipping actif (Pclp = 0) */
        int lx, hx, ly, hy;
        getDrawArea(cmd, &lx, &hx, &ly, &hy);
        ax = CAP(lx,ax,hx);
        bx = CAP(lx,bx,hx);
        ay = CAP(ly,ay,hy);
        dy = CAP(ly,dy,hy);
    }
    cmdW = getTextureWords(cmd, MAX(cmd->w, 1));
    rh = abs(dy - ay);
    rw = abs(bx - ax);
    if (((cmd->CMDPMOD >> 12) & 0x1) && (rw < cmd->w)) cmdW >>= 1;
    return MAX(rw * getPixelCycles(cmd), cmdW) * MAX(rh, 1);
}

static int getDistortedCycles(vdp1cmd_struct *cmd) {
    /* Sprite deforme : largeur moyenne des aretes AB et DC x hauteur
     * maximale ; meme borne de lecture de texture que le sprite agrandi. */
    int ax = cmd->CMDXA, ay = cmd->CMDYA;
    int bx = cmd->CMDXB, by = cmd->CMDYB;
    int cx = cmd->CMDXC, cy = cmd->CMDYC;
    int dx = cmd->CMDXD, dy = cmd->CMDYD;
    int rw_screen, cmdW, rw, rh;
    if (!(cmd->CMDPMOD & 0x800)) {
        int lx, hx, ly, hy;
        getDrawArea(cmd, &lx, &hx, &ly, &hy);
        ax = CAP(lx,ax,hx); bx = CAP(lx,bx,hx);
        cx = CAP(lx,cx,hx); dx = CAP(lx,dx,hx);
        ay = CAP(ly,ay,hy); by = CAP(ly,by,hy);
        cy = CAP(ly,cy,hy); dy = CAP(ly,dy,hy);
    }
    rw_screen = (abs(bx-ax) + abs(cx-dx)) / 2;
    cmdW = getTextureWords(cmd, MAX(cmd->w, 1));
    if (((cmd->CMDPMOD >> 12) & 0x1) && (rw_screen < cmd->w))
        cmdW >>= 1;
    rw = MAX(rw_screen * getPixelCycles(cmd), cmdW);
    rh = MAX(abs(ay - dy), abs(cy - by));
    return MAX(rw, 1) * MAX(rh, 1);
}

static int getPolygonCycles(vdp1cmd_struct *cmd) {
    /* Polygone : pas de texture, seulement l'ecriture des pixels. */
    int ax = cmd->CMDXA, ay = cmd->CMDYA;
    int bx = cmd->CMDXB, by = cmd->CMDYB;
    int cx = cmd->CMDXC, cy = cmd->CMDYC;
    int dx = cmd->CMDXD, dy = cmd->CMDYD;
    int rw, rh;
    if (!(cmd->CMDPMOD & 0x800)) {
        int lx, hx, ly, hy;
        getDrawArea(cmd, &lx, &hx, &ly, &hy);
        ax = CAP(lx,ax,hx); bx = CAP(lx,bx,hx);
        cx = CAP(lx,cx,hx); dx = CAP(lx,dx,hx);
        ay = CAP(ly,ay,hy); by = CAP(ly,by,hy);
        cy = CAP(ly,cy,hy); dy = CAP(ly,dy,hy);
    }
    rw = (abs(bx - ax) + abs(cx - dx)) / 2;
    rh = MAX(abs(ay - dy), abs(cy - by));
    return MAX(rw, 1) * MAX(rh, 1) * getPixelCycles(cmd);
}

static int getLineCycles(vdp1cmd_struct *cmd, int nlines) {
    /* Ligne (1 segment A-B) ou polyligne (A-B, B-C, C-D, D-A) : chaque
     * segment ecrit max(|dx|, |dy|) + 1 pixels. Avec le pre-clipping actif,
     * un segment entierement d'un meme cote de la zone de trace n'est pas
     * trace (ST-013-SP1 p.3 : "can be detected in advance ... by not
     * allowing the drawing to be started") ; on lui compte 1 cycle. */
    const int X[4] = { cmd->CMDXA, cmd->CMDXB, cmd->CMDXC, cmd->CMDXD };
    const int Y[4] = { cmd->CMDYA, cmd->CMDYB, cmd->CMDYC, cmd->CMDYD };
    const int preclip = !(cmd->CMDPMOD & 0x800);
    int lx, hx, ly, hy, i, total = 0;
    getDrawArea(cmd, &lx, &hx, &ly, &hy);
    for (i = 0; i < nlines; i++) {
        const int j = (i + 1) & 3;
        if (preclip &&
            ((X[i] < lx && X[j] < lx) || (X[i] > hx && X[j] > hx) ||
             (Y[i] < ly && Y[j] < ly) || (Y[i] > hy && Y[j] > hy))) {
            total += 1;
            continue;
        }
        total += MAX(abs(X[j] - X[i]), abs(Y[j] - Y[i])) + 1;
    }
    return total * getPixelCycles(cmd);
}
 
/* NOTE: the VDP1 Manual does NOT prohibit the ECD=0 / SPD=1 combination,
 * and there is no "when ECD=0, SPD must equal 0" rule anywhere in the
 * spec. ECD (End Code Disable, bit 7, p.85-86) and SPD (Transparent Pixel
 * Disable, bit 6, p.88) are independent: end-code handling and transparent-
 * pixel handling are evaluated separately per pixel. SPD must therefore be
 * honoured as-is (it is applied normally in the texture/draw paths); do NOT
 * force it to 0. */
 
/* Valeurs de retour de Vdp1NormalSpriteDraw() :
 *    1 : sprite transmis au moteur de rendu ;
 *    0 : commande valide qui ne dessine rien (taille nulle, hors ecran) ;
 *   -1 : commande invalide -- l'appelant abandonne la fin de la ligne.
 *
 * Une table de commande entierement a zero est un sprite normal VALIDE
 * (CMDCTRL = 0000h : Comm = 0, sprite normal ; ST-013-R3 §6.1). Rien dans
 * le manuel VDP1 n'en fait une commande invalide. Taille 0, mode couleur 0
 * (banque 16 couleurs) et code couleur 0, qui est transparent (SPD = 0,
 * §6.3) : le materiel ne dessine rien et passe a la table suivante apres
 * l'avoir lue. Mednafen (ss/vdp1_sprite.c SpriteBase) et Ymir
 * (VDP1Cmd_DrawNormalSprite, cout simpleQuadTiming(max(w,1), max(h,1)))
 * n'ont aucun cas particulier pour elle et continuent la liste.
 *
 * Densetsu no Ogre Battle remplit sa table de pres de 350 de ces commandes
 * vides avant les sprites de Warren et du texte (cmd 356 et suivantes).
 * Chaque commande vide renvoyait -1, ce qui remettait vdp1_clock a 0 et
 * coutait une ligne d'affichage entiere : la liste ne depassait jamais
 * ~260 commandes avant la trame suivante, et Warren et le texte n'etaient
 * jamais dessines. */
static int Vdp1NormalSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs){
  Vdp2 *varVdp2Regs = &Vdp2Lines[0];
  int ret = 1;
  if (emptyCmd(cmd)) {
    /* Lecture de la table de 32 octets : 16 cycles, comme les commandes
     * de clipping et de coordonnees locales plus bas. Rien a dessiner. */
    yabsys.vdp1cycles += 16;
    return 0;
  }
 
  if ((cmd->CMDSIZE & 0x8000)) {
	/* VDP1 Manual §4.5: ENDR forces termination within ~30 clock cycles.
     * For invalid/malformed commands the hardware aborts after fetching
     * the full 32-byte command table (16 cycles) plus abort overhead.
     * 70 cycles = empirical value matching hardware measurement;
     * no exact figure given in VDP1 Manual for malformed-command penalty. */  
    yabsys.vdp1cycles += 70;
    return -1; // BAD Command
  }
  if (((cmd->CMDPMOD >> 3) & 0x7) > 5) {
    // damaged data
    /* VDP1 Manual §4.5: ENDR forces termination within ~30 clock cycles.
     * For invalid/malformed commands the hardware aborts after fetching
     * the full 32-byte command table (16 cycles) plus abort overhead.
     * 70 cycles = empirical value matching hardware measurement;
     * no exact figure given in VDP1 Manual for malformed-command penalty. */
    yabsys.vdp1cycles += 70;
    return -1;
  }
  cmd->w = ((cmd->CMDSIZE >> 8) & 0x3F) * 8;
  cmd->h = cmd->CMDSIZE & 0xFF;
  if ((cmd->w == 0) || (cmd->h == 0)) {
    /* Taille nulle : commande valide qui ne produit pas de texture
     * exploitable. Cout de lecture de la table plus un pixel (Ymir compte
     * max(w,1) x max(h,1)), puis table suivante, sans perdre la ligne. */
    yabsys.vdp1cycles += 16 + 1;
    return 0;
  }
 
  cmd->flip = (cmd->CMDCTRL & 0x30) >> 4;
 
  if ( CONVERTCMD(&cmd->CMDXA) ||
       CONVERTCMD(&cmd->CMDYA)) {
         // damaged data
    /* VDP1 Manual §4.5: ENDR forces termination within ~30 clock cycles.
     * For invalid/malformed commands the hardware aborts after fetching
     * the full 32-byte command table (16 cycles) plus abort overhead.
     * 70 cycles = empirical value matching hardware measurement;
     * no exact figure given in VDP1 Manual for malformed-command penalty. */
         yabsys.vdp1cycles += 70;
         return -1;
       }
 
  cmd->CMDXA += regs->localX;
  cmd->CMDYA += regs->localY;
 
  cmd->CMDXB = cmd->CMDXA + MAX(1,cmd->w)-1;
  cmd->CMDYB = cmd->CMDYA;
  cmd->CMDXC = cmd->CMDXA + MAX(1,cmd->w)-1;
  cmd->CMDYC = cmd->CMDYA + MAX(1,cmd->h)-1;
  cmd->CMDXD = cmd->CMDXA;
  cmd->CMDYD = cmd->CMDYA + MAX(1,cmd->h)-1;
  
 
  yabsys.vdp1cycles+= getNormalCycles(cmd);
  vdp1EstQuad(cmd, 1);
  
  /* VDP1 Manual §6.3 p.81 (HSS bit 12) and §4.2 p.37 (FBCR bit 4 EOS):
   * HSS must be propagated to the renderer regardless of sprite flavor;
   * Vdp1ScaledSpriteDraw did it, but normal-sprite path omitted it,
   * causing HSS=1 scaled-to-same-size sprites (common UI optimization)
   * to render end codes as transparent pixels. §10 p.155: the hardware
   * internally ignores end codes when HSS=1 but does NOT rewrite
   * CMDPMOD; the renderer relies on cmd->hss to know this. */
  /* ST-013-R3 §6.3 p.86, tableau HSS/ECD : HSS=1 ne neutralise les end
   * codes QU'EN REDUCTION horizontale.
   *   HSS=0           , ECD=0 -> end code actif
   *   HSS=1 & enlarge , ECD=0 -> end code ACTIF
   *   HSS=1 & reduce  , ECD=0 -> end code neutralise
   *   HSS=1           , ECD=1 -> end code neutralise
   * Le code recopiait CMDPMOD bit 12 tel quel, ce qui neutralisait les
   * end codes aussi en agrandissement : les pixels de code de fin
   * etaient alors dessines comme des pixels de couleur. */
  cmd->hss = (cmd->CMDPMOD >> 12) & 0x1;
  /* Un sprite normal est trace a l'echelle 1:1 : jamais de reduction
   * horizontale, donc les end codes restent actifs. */
  if (cmd->hss) cmd->hss = 0;
  cmd->eos = (Vdp1Regs->FBCR >> 4) & 0x1;
 
 
  memset(cmd->G, 0, sizeof(float)*12);
  /* VDP1 Manual §5.3 p.65: "Gouraud shading [...] is only effective on
   * RGB color codes. The color cannot be guaranteed when Gouraud
   * shading is specified for color bank color codes."
   * Skip the Gouraud table read entirely for palette-bank color modes
   * (0, 2, 3, 4) -- they would produce garbage palette-index shifts.
   * Mode 1 (LUT) and mode 5 (RGB) may legitimately use Gouraud.
   * Table 5.3: correction = value - 0x10, range [-16,+15].
   *
   * The correction is expressed in 5-bit colour steps and is added to the
   * 5-bit component (ST-013-R3 §5.3). Every Gouraud shader of the compute
   * renderer (vdp1_prog_compute.h, vdp1_prog_compute_upscale.h) first
   * normalises the component as c / 31.0 and then adds G, so G has to use
   * the same scale: (value - 0x10) / 31.0. Dividing by 16.0 made every
   * correction 31/16 = 1.94 times too strong -- a +15 step pushed a
   * component by about +29, which is why Virtual Hydlide's distance fog
   * (bright Gouraud tables on the far polygons) saturated to white and the
   * near polygons came out too dark.
   *
   * NOTE: this block was accidentally dropped here when the
   * pre-clipping bounding-box check below was added; restored using
   * the same corrected formula already used in Vdp1ScaledSpriteDraw. */
  {
    u32 _scm = (cmd->CMDPMOD >> 3) & 0x7u;
    if ((cmd->CMDPMOD & 4) && (_scm == 1 || _scm == 5))
    {
      u32 gouraud_base = (u32)cmd->CMDGRDA << 3;
      for (int i = 0; i < 4; i++){
        u16 color2 = Vdp1RamReadWord(NULL, ram,
            (gouraud_base + (i << 1)) & 0x7FFFF);
        cmd->G[(i * 3) + 0] = (float)((int)((color2 & 0x001F))       - 0x10) / 31.0f;
        cmd->G[(i * 3) + 1] = (float)((int)((color2 & 0x03E0) >> 5)  - 0x10) / 31.0f;
        cmd->G[(i * 3) + 2] = (float)((int)((color2 & 0x7C00) >> 10) - 0x10) / 31.0f;
      }
    }
  }
// Spec §6.3 Pre-Clipping: reject only if the entire bounding box is outside
// system clip area. Partial overlap is handled per-line in the draw engine.
if (!(cmd->CMDPMOD & 0x800)) { // pre-clipping enabled (Pclp = 0 means enabled)
  s16 scx2 = (s16)regs->systemclipX2;
  s16 scy2 = (s16)regs->systemclipY2;
  // Bounding box of the normal sprite: [XA, XA+w-1] × [YA, YA+h-1]
  // after localX/Y has been added
  s16 x1 = (s16)cmd->CMDXA;
  s16 y1 = (s16)cmd->CMDYA;
  s16 x2 = (s16)cmd->CMDXC; // set to CMDXA + w - 1 above
  s16 y2 = (s16)cmd->CMDYC; // set to CMDYA + h - 1 above
  // Entirely outside → skip
  if (x1 > scx2 || y1 > scy2 || x2 < 0 || y2 < 0) { return 0; }
	}
  /* Chapitre 9 du manuel VDP1 -- combinaisons interdites avec un frame
   * buffer 8 bits/pixel (TVMR bit 0 = 1). Le materiel n'a pas de
   * comportement defini pour elles ; on les signale sans changer le rendu.
   *
   *  - MON = 1 (CMDPMOD bit 15) est interdit en 8bpp : le bit de poids
   *    fort ecrit dans le frame buffer pour l'ombre/fenetre VDP2 n'a pas
   *    la meme signification dans ce format.
   *  - le mode couleur RGB (CMDPMOD bits 5-3 = 5) est interdit en 8bpp :
   *    seuls les modes 0 a 4 sont possibles. Le shader tronque alors la
   *    valeur RGB 16 bits a son octet de poids faible. */
  if (Vdp1Regs->TVMR & 0x1) {
    if (cmd->CMDPMOD & 0x8000)
      LOG("VDP1: MON=1 interdit en frame buffer 8bpp (CMDPMOD=%04X)\n",
          cmd->CMDPMOD);
    if (((cmd->CMDPMOD >> 3) & 0x7) == 5)
      LOG("VDP1: mode couleur RGB interdit en frame buffer 8bpp (CMDPMOD=%04X)\n",
          cmd->CMDPMOD);
  }
 
  /* VDP1 Manual §6.3 Color Calculation bits 2~0:
   * Mode 101B (5) = "Setting prohibited (do not set)"
   * Skip the command silently to avoid undefined behavior. */
  if ((cmd->CMDPMOD & 0x7) == 5) {    // ← GARDER CE BLOC (CHECK 2, bon endroit)
    yabsys.vdp1cycles += 70;
   return -1;                         // ← doit être -1 comme les autres erreurs
  }
  VIDCore->Vdp1NormalSpriteDraw(cmd, ram, regs);
  return ret;
}
 
static int Vdp1ScaledSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs) {
  Vdp2 *varVdp2Regs = &Vdp2Lines[0];
  s16 rw = 0, rh = 0;
  s16 x, y;
  int ret = 1;
 
  if (emptyCmd(cmd)) {
    // damaged data
    yabsys.vdp1cycles += 70;
    return -1;
  }
  
  /* VDP1 Manual §6.3 p.89-92 Color Mode: only bits 5:3 values 000..101
   * are defined; 110 and 111 are reserved. Reject like normal-sprite
   * path to avoid reading texture bytes at an undefined bit depth. */
  if (((cmd->CMDPMOD >> 3) & 0x7) > 5) {
    /* §4.5 p.57: malformed commands abort with the 70-cycle penalty. */
    yabsys.vdp1cycles += 70;
    return -1;
  }
 
  cmd->w = ((cmd->CMDSIZE >> 8) & 0x3F) * 8;
  cmd->h = cmd->CMDSIZE & 0xFF;
  if ((cmd->w == 0) || (cmd->h == 0)) {
    yabsys.vdp1cycles += 70;
    ret = 0;
  }
 
  cmd->flip = (cmd->CMDCTRL & 0x30) >> 4;
 
  /* VDP1 Manual §10 p.155: when HSS=1 the hardware internally ignores
   * end codes but does NOT rewrite CMDPMOD. The downstream renderer
   * uses cmd->hss (set below) to skip end-code processing. */
 
  switch ((cmd->CMDCTRL & 0xF00) >> 8)
  {
      case 0x0:
        // Deux coordonnées : XA,YA = haut-gauche, XC,YC = bas-droit
		  if ( CONVERTCMD(&cmd->CMDXA) ||
			   CONVERTCMD(&cmd->CMDYA) ||
			   CONVERTCMD(&cmd->CMDXC) ||
			   CONVERTCMD(&cmd->CMDYC)) {
				 yabsys.vdp1cycles += 70;
				 return -1;
			   }
		  // Reconstruction immédiate des 4 coins (VDP1 Manual §4.4)
		  cmd->CMDXB = cmd->CMDXC;
		  cmd->CMDYB = cmd->CMDYA;
		  cmd->CMDXD = cmd->CMDXA;
		  cmd->CMDYD = cmd->CMDYC;
		  break;
		default:
		  if ( CONVERTCMD(&cmd->CMDXA) ||
			   CONVERTCMD(&cmd->CMDYA) ||
			   CONVERTCMD(&cmd->CMDXB) ||
			   CONVERTCMD(&cmd->CMDYB)) {
				 yabsys.vdp1cycles += 70;
				 return -1;
			   }
		   break;
	}
 
 
  x = cmd->CMDXA;
  y = cmd->CMDYA;
  /* Chapitre 9 du manuel VDP1 : seules les valeurs 0H, 5H, 6H, 7H, 9H, AH,
   * BH, DH, EH, FH sont possibles pour le zoom point ; les autres sont
   * interdites. Le champ se decompose en deux sous-champs de 2 bits (X en
   * bits 9-8, Y en bits 11-10) et la regle revient a : les deux sont nuls,
   * ou aucun ne l'est. Les six valeurs interdites (1, 2, 3, 4, 8, CH) sont
   * exactement celles ou un seul des deux est nul.
   *
   * Les deux switch ci-dessous ont chacun un "default: break;", donc ces
   * combinaisons recoivent un comportement defini que le materiel n'a pas.
   * On ne change pas ce comportement -- il est peut-etre porteur pour un
   * titre qui produit ces valeurs -- on se contente de les signaler. */
  {
    const u32 zp = (cmd->CMDCTRL >> 8) & 0xF;
    if (((zp & 0x3) == 0) != (((zp >> 2) & 0x3) == 0))
      LOG("VDP1: zoom point interdit %XH (CMDCTRL=%04X)\n", zp, cmd->CMDCTRL);
  }
 
  // Setup Zoom Point
  switch ((cmd->CMDCTRL & 0x300) >> 8) {
    case 1: //Left
    rw = cmd->CMDXB;
        /* negative display width = horizontally inverted scaled sprite.
         * VDP1 manual marks it "not guaranteed", but real HW/Yaba draw the
         * mirror; the arithmetic below yields a reversed quad the QUAD
         * rasterizer maps correctly. Do NOT drop it (Golden Axe 2P sprite). */
        cmd->CMDXB = cmd->CMDXA + rw;
        cmd->CMDXC = cmd->CMDXA + rw;
        cmd->CMDXD = cmd->CMDXA;
    break;
    case 2: //center
    rw = cmd->CMDXB;
        /* negative display width = horizontally inverted scaled sprite.
         * VDP1 manual marks it "not guaranteed", but real HW/Yaba draw the
         * mirror; the arithmetic below yields a reversed quad the QUAD
         * rasterizer maps correctly. Do NOT drop it (Golden Axe 2P sprite). */
        cmd->CMDXA = x - rw/2;
        cmd->CMDXB = x + (rw+1)/2;
        cmd->CMDXD = x - rw/2;
        cmd->CMDXC = x + (rw+1)/2;
    break;
    case 3: //right
    rw = cmd->CMDXB;
        /* negative display width = horizontally inverted scaled sprite.
         * VDP1 manual marks it "not guaranteed", but real HW/Yaba draw the
         * mirror; the arithmetic below yields a reversed quad the QUAD
         * rasterizer maps correctly. Do NOT drop it (Golden Axe 2P sprite). */
        cmd->CMDXA = x - rw;
        cmd->CMDXB = x;
        cmd->CMDXC = x;
        cmd->CMDXD = x - rw;
    break;
    default:
        break;
    }
  switch ((cmd->CMDCTRL & 0xC00) >> 10) {
	case 0: //none (two-coordinates mode) — quad déjà reconstruit dans le switch X
    // Rien : CMDYB et CMDYD déjà assignés ci-dessus
    break;
    case 1: //Top
    rh = cmd->CMDYB;
        /* negative display height = vertically inverted scaled sprite (see
         * note above for the horizontal case). Keep it; do not drop. */
        cmd->CMDYB = cmd->CMDYA;
        cmd->CMDYC = cmd->CMDYA + rh;
        cmd->CMDYD = cmd->CMDYA + rh;
    break;
    case 2: //center
    rh = cmd->CMDYB;
        /* negative display height = vertically inverted scaled sprite (see
         * note above for the horizontal case). Keep it; do not drop. */
        cmd->CMDYA = y - rh/2;
        cmd->CMDYB = y - rh/2;
        cmd->CMDYC = y + (rh+1)/2;
        cmd->CMDYD = y + (rh+1)/2;
    break;
    case 3: //bottom
    rh = cmd->CMDYB;
        /* negative display height = vertically inverted scaled sprite (see
         * note above for the horizontal case). Keep it; do not drop. */
        cmd->CMDYA = y - rh;
        cmd->CMDYB = y - rh;
        cmd->CMDYC = y;
        cmd->CMDYD = y;
    break;
    default:
    break;
  }
 
  cmd->CMDXA += regs->localX;
  cmd->CMDYA += regs->localY;
  cmd->CMDXB += regs->localX;
  cmd->CMDYB += regs->localY;
  cmd->CMDXC += regs->localX;
  cmd->CMDYC += regs->localY;
  cmd->CMDXD += regs->localX;
  cmd->CMDYD += regs->localY;
 
  //mission 1 of burning rangers is loading a lot the vdp1.
  yabsys.vdp1cycles+= getScaledCycles(cmd);
  vdp1EstQuad(cmd, 1);
  
  /* VDP1 Manual §6.3 p.83 Pre-Clipping Disable (Pclp bit 11):
   * when Pclp=0 (pre-clipping enabled), reject if the bounding box
   * of the final 4-corner polygon lies entirely outside the system
   * clip rectangle. Matches the test already present in
   * Vdp1NormalSpriteDraw for consistency across sprite flavors.
   * §6.3: "For lines that are completely separated from the drawing
   * area [...] drawing efficiency can be raised by specifying the
   * drawing not be started." */
  if (!(cmd->CMDPMOD & 0x800)) {
    s16 scx2 = (s16)regs->systemclipX2;
    s16 scy2 = (s16)regs->systemclipY2;
    s16 bx1 = (s16)MIN(MIN(cmd->CMDXA,cmd->CMDXB), MIN(cmd->CMDXC,cmd->CMDXD));
    s16 by1 = (s16)MIN(MIN(cmd->CMDYA,cmd->CMDYB), MIN(cmd->CMDYC,cmd->CMDYD));
    s16 bx2 = (s16)MAX(MAX(cmd->CMDXA,cmd->CMDXB), MAX(cmd->CMDXC,cmd->CMDXD));
    s16 by2 = (s16)MAX(MAX(cmd->CMDYA,cmd->CMDYB), MAX(cmd->CMDYC,cmd->CMDYD));
    if (bx1 > scx2 || by1 > scy2 || bx2 < 0 || by2 < 0) return 0;
  }
 
  //gouraud
  memset(cmd->G, 0, sizeof(float)*12);
	/* VDP1 Manual §5.3 p.65: "Gouraud shading [...] is only effective on
	 * RGB color codes. The color cannot be guaranteed when Gouraud
	 * shading is specified for color bank color codes."
	 * Skip the Gouraud table read entirely for palette-bank color modes
	 * (0, 2, 3, 4) -- they would produce garbage palette-index shifts.
	 * Mode 1 (LUT) and mode 5 (RGB) may legitimately use Gouraud;
	 * mode 1 is further per-pixel-gated in the shader (RGB only when
	 * LUT entry MSB=1). Table 5.3: correction = value - 0x10, range [-16,+15]. */
	u32 _scm = (cmd->CMDPMOD >> 3) & 0x7u;
	if ((cmd->CMDPMOD & 4) && (_scm == 1 || _scm == 5))
	{
		u32 gouraud_base = (u32)cmd->CMDGRDA << 3;
		for (int i = 0; i < 4; i++){
			u16 color2 = Vdp1RamReadWord(NULL, ram,
				(gouraud_base + (i << 1)) & 0x7FFFF);
			/* VDP1 Manual §5.3 Table 5.3:
			 * correction = table_value - 0x10
			 * 0x00 → -16, 0x10 → 0, 0x1F → +15
			 * The correction is added to the 5-bit colour component, in
			 * the same unit. The shaders normalise that component as
			 * c / 31.0, so the correction must be divided by 31.0 too
			 * (see the note in Vdp1NormalSpriteDraw). */
			cmd->G[(i * 3) + 0] = (float)((int)((color2 & 0x001F))       - 0x10) / 31.0f;
			cmd->G[(i * 3) + 1] = (float)((int)((color2 & 0x03E0) >> 5)  - 0x10) / 31.0f;
			cmd->G[(i * 3) + 2] = (float)((int)((color2 & 0x7C00) >> 10) - 0x10) / 31.0f;
		}
	}
	// VDP1 Manual §4.2 EOS bit (FBCR bit 4):
	// When HSS=1, EOS selects even(0) or odd(1) pixel sampling
	// cmd->hss already set; add eos field:
  /* ST-013-R3 §6.3 p.86, tableau HSS/ECD : HSS=1 ne neutralise les end
   * codes QU'EN REDUCTION horizontale.
   *   HSS=0           , ECD=0 -> end code actif
   *   HSS=1 & enlarge , ECD=0 -> end code ACTIF
   *   HSS=1 & reduce  , ECD=0 -> end code neutralise
   *   HSS=1           , ECD=1 -> end code neutralise
   * Le code recopiait CMDPMOD bit 12 tel quel, ce qui neutralisait les
   * end codes aussi en agrandissement : les pixels de code de fin
   * etaient alors dessines comme des pixels de couleur. */
	cmd->hss = (cmd->CMDPMOD >> 12) & 0x1;
	if (cmd->hss) {
		int dispw = abs((int)cmd->CMDXB - (int)cmd->CMDXA) + 1;
		if (dispw >= (int)MAX(1u, cmd->w)) cmd->hss = 0; /* agrandissement ou 1:1 */
	}
	// EOS is only meaningful when HSS=1
	cmd->eos = (Vdp1Regs->FBCR >> 4) & 0x1; // 0=even coords, 1=odd coords
	 /* VDP1 §6.3: color calculation mode 101B is prohibited */
	 if ((cmd->CMDPMOD & 0x7) == 5) {
	   yabsys.vdp1cycles += 70;
	   return -1;
	 }
	VIDCore->Vdp1ScaledSpriteDraw(cmd, ram, regs);
  return ret;
}
 
static int Vdp1DistortedSpriteDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs) {
  Vdp2 *varVdp2Regs = &Vdp2Lines[0];
  int ret = 1;
 
  if (emptyCmd(cmd)) {
    /* §4.5: malformed command. Keep return 0 (original behavior) for
     * compatibility — the dispatcher resets vdp1_clock either way. */
    yabsys.vdp1cycles += 70;
    return 0;
  }
 
  /* §6.3 p.89-92: color mode bits 5:3 values 110/111 are reserved.
   * Keep return 0 to match original Vdp1DistortedSpriteDraw convention. */
  if (((cmd->CMDPMOD >> 3) & 0x7) > 5) {
    yabsys.vdp1cycles += 70;
    return 0;
  }
 
  cmd->w = ((cmd->CMDSIZE >> 8) & 0x3F) * 8;
  cmd->h = cmd->CMDSIZE & 0xFF;
  if ((cmd->w == 0) || (cmd->h == 0)) {
    yabsys.vdp1cycles += 70;
    ret = 0;
  }
 
  cmd->flip = (cmd->CMDCTRL & 0x30) >> 4;
 
  /* VDP1 Manual §6.3 p.81 (HSS, CMDPMOD bit 12) + §4.2 p.37 (FBCR bit 4 EOS):
   * a distorted sprite is textured, so — exactly like normal and scaled
   * sprites — HSS/EOS must be propagated to the renderer (the hardware
   * ignores end codes when HSS=1 but does NOT rewrite CMDPMOD, so the
   * renderer relies on cmd->hss). This was only described in a comment here
   * and never assigned: since 'cmd' is reused across the command loop, a
   * distorted sprite inherited the previous command's hss/eos, mis-handling
   * end codes on HSS=1 distorted sprites. */
  /* ST-013-R3 §6.3 p.86, tableau HSS/ECD : HSS=1 ne neutralise les end
   * codes QU'EN REDUCTION horizontale.
   *   HSS=0           , ECD=0 -> end code actif
   *   HSS=1 & enlarge , ECD=0 -> end code ACTIF
   *   HSS=1 & reduce  , ECD=0 -> end code neutralise
   *   HSS=1           , ECD=1 -> end code neutralise
   * Le code recopiait CMDPMOD bit 12 tel quel, ce qui neutralisait les
   * end codes aussi en agrandissement : les pixels de code de fin
   * etaient alors dessines comme des pixels de couleur. */
  cmd->hss = (cmd->CMDPMOD >> 12) & 0x1;
  if (cmd->hss) {
    /* Quadrilatere quelconque : on compare la plus grande arete
     * horizontale affichee a la largeur du caractere. */
    int wAB = abs((int)cmd->CMDXB - (int)cmd->CMDXA) + 1;
    int wDC = abs((int)cmd->CMDXC - (int)cmd->CMDXD) + 1;
    if (MAX(wAB, wDC) >= (int)MAX(1u, cmd->w)) cmd->hss = 0;
  }
  cmd->eos = (Vdp1Regs->FBCR >> 4) & 0x1;
 
  if ( CONVERTCMD(&cmd->CMDXA) ||
       CONVERTCMD(&cmd->CMDYA) ||
       CONVERTCMD(&cmd->CMDXB) ||
       CONVERTCMD(&cmd->CMDYB) ||
       CONVERTCMD(&cmd->CMDXC) ||
       CONVERTCMD(&cmd->CMDYC) ||
       CONVERTCMD(&cmd->CMDXD) ||
       CONVERTCMD(&cmd->CMDYD)) {
         yabsys.vdp1cycles += 70;
         return 0;
       }
 
  cmd->CMDXA += regs->localX;
  cmd->CMDYA += regs->localY;
  cmd->CMDXB += regs->localX;
  cmd->CMDYB += regs->localY;
  cmd->CMDXC += regs->localX;
  cmd->CMDYC += regs->localY;
  cmd->CMDXD += regs->localX;
  cmd->CMDYD += regs->localY;
 
  //mission 1 of burning rangers is loading a lot the vdp1.
  yabsys.vdp1cycles += getDistortedCycles(cmd);
  vdp1EstQuad(cmd, 1);
 
  memset(cmd->G, 0, sizeof(float)*12);
  /* (VDP1 Manual §5.3 Table 5.3 : correction = value - 0x10, range [-16,+15]) */
  if ((cmd->CMDPMOD & 4))
  {
    u32 gouraud_base = (u32)cmd->CMDGRDA << 3;
    for (int i = 0; i < 4; i++) {
      u16 color2 = Vdp1RamReadWord(NULL, ram,
        (gouraud_base + (i << 1)) & 0x7FFFF);
      cmd->G[(i * 3) + 0] = (float)((int)((color2 & 0x001F))       - 0x10) / 31.0f;
      cmd->G[(i * 3) + 1] = (float)((int)((color2 & 0x03E0) >> 5)  - 0x10) / 31.0f;
      cmd->G[(i * 3) + 2] = (float)((int)((color2 & 0x7C00) >> 10) - 0x10) / 31.0f;
    }
  }
 
  VIDCore->Vdp1DistortedSpriteDraw(cmd, ram, regs);
  return ret;
}
 
static int Vdp1PolygonDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs) {
  Vdp2 *varVdp2Regs = &Vdp2Lines[0];
 
  if ( CONVERTCMD(&cmd->CMDXA) ||
       CONVERTCMD(&cmd->CMDYA) ||
       CONVERTCMD(&cmd->CMDXB) ||
       CONVERTCMD(&cmd->CMDYB) ||
       CONVERTCMD(&cmd->CMDXC) ||
       CONVERTCMD(&cmd->CMDYC) ||
       CONVERTCMD(&cmd->CMDXD) ||
       CONVERTCMD(&cmd->CMDYD)) {
   /* §6.7 p.105: out-of-range coordinates -- -1 per convention. */
         yabsys.vdp1cycles += 70;
         return -1;
       }
 
  cmd->CMDXA += regs->localX;
  cmd->CMDYA += regs->localY;
  cmd->CMDXB += regs->localX;
  cmd->CMDYB += regs->localY;
  cmd->CMDXC += regs->localX;
  cmd->CMDYC += regs->localY;
  cmd->CMDXD += regs->localX;
  cmd->CMDYD += regs->localY;
 
  yabsys.vdp1cycles += getPolygonCycles(cmd);
  vdp1EstQuad(cmd, 0);
  //gouraud
  memset(cmd->G, 0, sizeof(float)*12);
	// (VDP1 Manual §5.3 Table 5.3 : correction = value - 0x10, range [-16,+15]) :
	if ((cmd->CMDPMOD & 4))
	{
		u32 gouraud_base = (u32)cmd->CMDGRDA << 3;
		for (int i = 0; i < 4; i++){
			u16 color2 = Vdp1RamReadWord(NULL, ram,
				(gouraud_base + (i << 1)) & 0x7FFFF);
			/* VDP1 Manual §5.3 Table 5.3:
			 * correction = table_value - 0x10
			 * 0x00 → -16, 0x10 → 0, 0x1F → +15
			 * The correction is added to the 5-bit colour component, in
			 * the same unit. The shaders normalise that component as
			 * c / 31.0, so the correction must be divided by 31.0 too
			 * (see the note in Vdp1NormalSpriteDraw). */
			cmd->G[(i * 3) + 0] = (float)((int)((color2 & 0x001F))       - 0x10) / 31.0f;
			cmd->G[(i * 3) + 1] = (float)((int)((color2 & 0x03E0) >> 5)  - 0x10) / 31.0f;
			cmd->G[(i * 3) + 2] = (float)((int)((color2 & 0x7C00) >> 10) - 0x10) / 31.0f;
		}
	}
  cmd->w = 1;
  cmd->h = 1;
  cmd->flip = 0;
	 /* VDP1 §6.3: color calculation mode 101B is prohibited */
	 if ((cmd->CMDPMOD & 0x7) == 5) {
	   yabsys.vdp1cycles += 70;
	   return -1;
	 }
 
  VIDCore->Vdp1PolygonDraw(cmd, ram, regs);
  return 1;
}
 
static int Vdp1PolylineDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs) {
 
  Vdp2 *varVdp2Regs = &Vdp2Lines[0];
 
  cmd->w = 1;
  cmd->h = 1;
  cmd->flip = 0;
 
  if ( CONVERTCMD(&cmd->CMDXA) ||
       CONVERTCMD(&cmd->CMDYA) ||
       CONVERTCMD(&cmd->CMDXB) ||
       CONVERTCMD(&cmd->CMDYB) ||
       CONVERTCMD(&cmd->CMDXC) ||
       CONVERTCMD(&cmd->CMDYC) ||
       CONVERTCMD(&cmd->CMDXD) ||
       CONVERTCMD(&cmd->CMDYD)) {
         // damaged data
         yabsys.vdp1cycles += 70;
         return 0;
       }
 
 
  cmd->CMDXA += regs->localX;
  cmd->CMDYA += regs->localY;
  cmd->CMDXB += regs->localX;
  cmd->CMDYB += regs->localY;
  cmd->CMDXC += regs->localX;
  cmd->CMDYC += regs->localY;
  cmd->CMDXD += regs->localX;
  cmd->CMDYD += regs->localY;
  /* Polyligne : 4 segments (modele de l'emulateur, puis estimation
   * detaillee pour le debogueur). */
  yabsys.vdp1cycles += getLineCycles(cmd, 4);
  vdp1EstLines(cmd, 4);
 
  //gouraud
  memset(cmd->G, 0, sizeof(float)*12);
	// (VDP1 Manual §5.3 Table 5.3 : correction = value - 0x10, range [-16,+15]) :
	if ((cmd->CMDPMOD & 4))
	{
		u32 gouraud_base = (u32)cmd->CMDGRDA << 3;
		for (int i = 0; i < 4; i++){
			u16 color2 = Vdp1RamReadWord(NULL, ram,
				(gouraud_base + (i << 1)) & 0x7FFFF);
			/* VDP1 Manual §5.3 Table 5.3:
			 * correction = table_value - 0x10
			 * 0x00 → -16, 0x10 → 0, 0x1F → +15
			 * The correction is added to the 5-bit colour component, in
			 * the same unit. The shaders normalise that component as
			 * c / 31.0, so the correction must be divided by 31.0 too
			 * (see the note in Vdp1NormalSpriteDraw). */
			cmd->G[(i * 3) + 0] = (float)((int)((color2 & 0x001F))       - 0x10) / 31.0f;
			cmd->G[(i * 3) + 1] = (float)((int)((color2 & 0x03E0) >> 5)  - 0x10) / 31.0f;
			cmd->G[(i * 3) + 2] = (float)((int)((color2 & 0x7C00) >> 10) - 0x10) / 31.0f;
		}
	}
	
	/* VDP1 §6.3: color calculation mode 101B is prohibited */
	 if ((cmd->CMDPMOD & 0x7) == 5) {
	   yabsys.vdp1cycles += 70;
	   return -1;
	 }
 
  VIDCore->Vdp1PolylineDraw(cmd, ram, regs);
 
  return 1;
}
 
static int Vdp1LineDraw(vdp1cmd_struct *cmd, u8 * ram, Vdp1 * regs) {
  Vdp2 *varVdp2Regs = &Vdp2Lines[0];
 
 
  if ( CONVERTCMD(&cmd->CMDXA) ||
       CONVERTCMD(&cmd->CMDYA) ||
       CONVERTCMD(&cmd->CMDXB) ||
       CONVERTCMD(&cmd->CMDYB)) {
         // damaged data
         yabsys.vdp1cycles += 70;
         return 0;
       }
 
 
  cmd->CMDXA += regs->localX;
  cmd->CMDYA += regs->localY;
  cmd->CMDXB += regs->localX;
  cmd->CMDYB += regs->localY;
  cmd->CMDXC = cmd->CMDXB;
  cmd->CMDYC = cmd->CMDYB;
  cmd->CMDXD = cmd->CMDXA;
  cmd->CMDYD = cmd->CMDYA;
  /* Ligne : 1 segment (modele de l'emulateur, puis estimation). */
  yabsys.vdp1cycles += getLineCycles(cmd, 1);
  vdp1EstLines(cmd, 1);
 
  //gouraud
  memset(cmd->G, 0, sizeof(float)*12);
	// (VDP1 Manual §5.3 Table 5.3 : correction = value - 0x10, range [-16,+15]) :
	if ((cmd->CMDPMOD & 4))
	{
		u32 gouraud_base = (u32)cmd->CMDGRDA << 3;
		for (int i = 0; i < 4; i++){
			u16 color2 = Vdp1RamReadWord(NULL, ram,
				(gouraud_base + (i << 1)) & 0x7FFFF);
			/* VDP1 Manual §5.3 Table 5.3:
			 * correction = table_value - 0x10
			 * 0x00 → -16, 0x10 → 0, 0x1F → +15
			 * The correction is added to the 5-bit colour component, in
			 * the same unit. The shaders normalise that component as
			 * c / 31.0, so the correction must be divided by 31.0 too
			 * (see the note in Vdp1NormalSpriteDraw). */
			cmd->G[(i * 3) + 0] = (float)((int)((color2 & 0x001F))       - 0x10) / 31.0f;
			cmd->G[(i * 3) + 1] = (float)((int)((color2 & 0x03E0) >> 5)  - 0x10) / 31.0f;
			cmd->G[(i * 3) + 2] = (float)((int)((color2 & 0x7C00) >> 10) - 0x10) / 31.0f;
		}
	}
  cmd->w = 1;
  cmd->h = 1;
  cmd->flip = 0;
	 /* VDP1 §6.3: color calculation mode 101B is prohibited */
	 if ((cmd->CMDPMOD & 0x7) == 5) {
	   yabsys.vdp1cycles += 70;
	   return -1;
	 }
 
  VIDCore->Vdp1LineDraw(cmd, ram, regs);
 
  return 1;
}
 
/* ---------------------------------------------------------------------------
 * VDP1 per-raster drawing budget.
 *
 * Source: VDP1 User's Manual ST-013-R3, Table 4.4 p.49 + Table 4.5 p.50.
 *
 * Table 4.4 lists four "screen modes" (NTSC / PAL / 31KC / HDTV) with a
 * "number of pixels in 1 raster" column: 1708 / 1820 / 852 / 848. Read
 * naively, the NTSC row (320 px) and the PAL row (352 px) suggest the value
 * is selected by TV standard. It is NOT. The rows pair each standard with a
 * *different horizontal resolution*, and it is the horizontal resolution --
 * i.e. the dot-clock family -- that picks the value.
 *
 * Proof, from Table 4.5, which tabulates the V-blank erase capacity for a
 * 16 bpp frame buffer. The manual (p.49) gives that capacity as
 *     {(pixels in 1 raster) - 200} x {(rasters in 1 field) - (display rasters)}
 * Solving for "pixels in 1 raster" against every entry of Table 4.5:
 *
 *   NTSC 320x224 :  58812 / (263-224) = 1508  -> 1708   (320 -> 1708)
 *   NTSC 352x224 :  63180 / (263-224) = 1620  -> 1820   (352 -> 1820, NTSC!)
 *   PAL  320x224 : 134212 / (313-224) = 1508  -> 1708   (320 -> 1708, PAL!)
 *   PAL  352x224 : 144180 / (313-224) = 1620  -> 1820
 *   PAL  320x256 :  85956 / (313-256) = 1508  -> 1708
 *   31KC 320x480 :  29340 / (525-480) =  652  ->  852
 *   HDTV 352x480 :  53136 / (562-480) =  648  ->  848
 *
 * Every 320-mode entry yields 1708 and every 352-mode entry yields 1820,
 * on BOTH NTSC and PAL. The TV standard only selects the number of rasters
 * per field (263 vs 313), never the pixels per raster.
 *
 * Physically this is just the two Saturn video clocks:
 *   26.8741 MHz / 15734.26 Hz = 1708   (320 / 640 modes)
 *   28.6364 MHz / 15734.26 Hz = 1820   (352 / 704 modes)
 *
 * The previous implementation selected on yabsys.IsPal and discarded the
 * is352 argument entirely, so it was wrong in exactly the two common cases:
 * a PAL 320 game got 1820 (+6.6% draw budget) and an NTSC 352 game got 1708
 * (-6.2%), and the 31KC/HDTV values were never reachable at all.
 *
 * Selection is by HRESO2-0 (VDP2 TVMD bits 2-0, VDP2 Manual p.28):
 *   000 320 normal     001 352 normal
 *   010 640 hi-res     011 704 hi-res
 *   100 320 excl.norm  101 352 excl.norm   (31KC / Hi-Vision)
 *   110 640 excl.hires 111 704 excl.hires
 * Hi-res (640/704) is the same master clock as its normal counterpart, so it
 * inherits the same per-raster value; Table 4.4 lists no separate row for it.
 * ------------------------------------------------------------------------- */
#define VDP1_RASTER_320   1708  /* Table 4.4 "NTSC" row  */
#define VDP1_RASTER_352   1820  /* Table 4.4 "PAL"  row  */
#define VDP1_RASTER_31KC   852  /* Table 4.4 "31KC" row  */
#define VDP1_RASTER_HDTV   848  /* Table 4.4 "HDTV" row  */
 
static int rasterValue = VDP1_RASTER_320;
 
/* hreso = VDP2 TVMD bits 2-0 (HRESO2-0). */
void Vdp1SetRaster(int hreso) {
    switch (hreso & 0x7) {
        case 0x0: /* 320 normal     */
        case 0x2: /* 640 hi-res     */ rasterValue = VDP1_RASTER_320;  break;
        case 0x1: /* 352 normal     */
        case 0x3: /* 704 hi-res     */ rasterValue = VDP1_RASTER_352;  break;
        case 0x4: /* 320 excl. norm */
        case 0x6: /* 640 excl. hi-r */ rasterValue = VDP1_RASTER_31KC; break;
        case 0x5: /* 352 excl. norm */
        case 0x7: /* 704 excl. hi-r */ rasterValue = VDP1_RASTER_HDTV; break;
        default:                       rasterValue = VDP1_RASTER_320;  break;
    }
}
 
static int getVdp1CyclesPerLine(void)
{
    if (Vdp1External.blocked != 0) return 0;
    return rasterValue;
}
 
/* État persistant entre appels de Vdp1DrawCommands : le rendu VDP1
 * peut être interrompu puis repris (émulation cycle-accurate), donc
 * l'adresse de retour CALL/RETURN doit survivre entre deux appels.
 * Renommée depuis 'returnAddr' qui masquait les variables locales
 * homonymes de EvaluateCmdListHash / Vdp1FakeDrawCommands /
 * Vdp1DebugGetCommandNumberAddr (-Wshadow). */
static u32 g_vdp1DrawReturnAddr = 0xffffffff;
 
#ifdef DEBUG_CMD_LIST
void debugCmdList() {
  YuiMsg("Draw %d (%d)\n", yabsys.LineCount, _Ygl->drawframe);
  for (int i=0;;i++)
  {
     char *string;
     u32 addr = Vdp1DebugGetCommandAddr(i);
     if ((string = Vdp1DebugGetCommandNumberName(addr)) == NULL)
        break;
 
     YuiMsg("\t%s\n", string);
  }
}
#endif
static u32 Vdp1DebugGetCommandNumberAddr(u32 number);
 
int EvaluateCmdListHash(Vdp1 * regs){
  int hash = 0;
  u32 addr = 0;
  u32 returnAddr = 0xFFFFFFFF;
  u32 commandCounter = 0;
  u16 command;
 
  command = T1ReadWord(Vdp1Ram, addr);
 
  while (!(command & 0x8000) && (commandCounter < 2000))
  {
      vdp1cmd_struct cmd;
     // Make sure we're still dealing with a valid command
     if ((command & 0x000C) == 0x000C)
        // Invalid, abort
        return hash;
      Vdp1ReadCommand(&cmd, addr, Vdp1Ram);
      hash ^= (cmd.CMDCTRL << 16) | cmd.CMDLINK;
      hash ^= (cmd.CMDPMOD << 16) | cmd.CMDCOLR;
      hash ^= (cmd.CMDSRCA << 16) | cmd.CMDSIZE;
      hash ^= (cmd.CMDXA << 16) | cmd.CMDYA;
      hash ^= (cmd.CMDXB << 16) | cmd.CMDYB;
      hash ^= (cmd.CMDXC << 16) | cmd.CMDYC;
      hash ^= (cmd.CMDXD << 16) | cmd.CMDYD;
      hash ^= (cmd.CMDGRDA << 16) | _Ygl->drawframe;
 
     // Determine where to go next
     switch ((command & 0x3000) >> 12)
     {
        case 0: // NEXT, jump to following table
           addr += 0x20;
           break;
        case 1: // ASSIGN, jump to CMDLINK
           addr = T1ReadWord(Vdp1Ram, addr + 2) * 8;
           break;
        case 2: // CALL, call a subroutine
           if (returnAddr == 0xFFFFFFFF)
              returnAddr = addr + 0x20;
 
           addr = T1ReadWord(Vdp1Ram, addr + 2) * 8;
           break;
        case 3: // RETURN, return from subroutine
           if (returnAddr != 0xFFFFFFFF) {
              addr = returnAddr;
              returnAddr = 0xFFFFFFFF;
           }
           else
              addr += 0x20;
           break;
     }
 
     if (addr > 0x7FFE0)
        return hash;
     command = T1ReadWord(Vdp1Ram, addr);
     commandCounter++;
  }
  return hash;
}
 
/* sameCmd() supprime.
 *
 * Introduit en amont (Kronos e869a0bf, issue #1245, ralentissement des
 * videos de Gihren no Yabou) pour ne pas retracer une commande identique a
 * la precedente. Le refactoring dedd8fc4 l'a rendu inoperant : 'oldCmd = cmd'
 * copiait la commande locale tout juste remise a zero, puis sameCmd()
 * comparait la commande lue a une table nulle -- et renvoyait 0 pour une
 * table nulle (emptyCmd). Il ne detectait donc jamais rien.
 *
 * Il n'est pas remis en service : le VDP1 trace chaque table qu'il lit et en
 * paie le cout (VDP1 Manual ST-013-R3, chap. 6 ; idem Mednafen ss/vdp1.c et
 * Ymir, qui n'ont aucun saut de ce genre). Sauter une commande identique
 * changerait le resultat des modes cumulatifs (deux demi-transparences
 * successives donnent 3/4 + 1/4, pas 1/2 + 1/2 ; idem MSB On, Gouraud +
 * demi-transparence) et retirerait son cout du budget de cycles, puisque
 * yabsys.vdp1cycles est incremente dans les fonctions de trace. Le comportement
 * actuel (tout tracer) est donc conserve ; seul le code mort disparait. */
 
static int lastHash = -1;
void Vdp1DrawCommands(u8 * ram, Vdp1 * regs)
{
  int cylesPerLine  = getVdp1CyclesPerLine();
 
  if ((Vdp1External.status&VDP1_STATUS_MASK) == VDP1_STATUS_IDLE) {
    FRAMELOG("Start vdp1 Draw %d(%d)\n", yabsys.LineCount, yabsys.DecilineCount);
    #if 0
    int newHash = EvaluateCmdListHash(regs);
    // Breaks megamanX4
    if (newHash == lastHash) {
      #ifdef DEBUG_CMD_LIST
      YuiMsg("Abort same command %x %x (%d) (%d)\n", newHash, lastHash, _Ygl->drawframe, yabsys.LineCount);
      #endif
      return;
    }
    lastHash = newHash;
    YuiMsg("The last list is 0x%x (%d) (%d)\n", newHash, _Ygl->drawframe, yabsys.LineCount);
    #endif
    #ifdef DEBUG_CMD_LIST
    debugCmdList();
    #endif
 
    g_vdp1DrawReturnAddr = 0xffffffff;
    nbCmdToProcess = 0;
    vdp1StatsListStart();
 
     // Vdp1Regs->EDSR >>= 1;
     Vdp1Regs->addr = 0;
     // BEF <- CEF
     // CEF <- 0
     Vdp1Regs->COPR = 0;
     Vdp1Regs->lCOPR = 0;
     if (VIDCore->startVdp1Render) VIDCore->startVdp1Render();
  }
 
   Vdp1External.status &= ~VDP1_STATUS_MASK;
   Vdp1External.status |= VDP1_STATUS_RUNNING;
 
   /* VDP1 Manual ST-013-R3 p.19: "When fetching of the command table goes
    * beyond the end address (07FFFFH) of VRAM, fetching wraps to the top
    * address (000000H) of VRAM."
    *
    * Running off the end of VRAM is therefore not an error condition and
    * must not stop the list: the address simply wraps. Dropping to
    * VDP1_STATUS_IDLE here made the VDP1 report itself free while a list
    * was still live, which then let a CPU access believe it was hitting an
    * idle chip and skip the arbitration stall. Wrap instead. */
   regs->addr &= 0x7FFFF;
 
    u16 command = Vdp1RamReadWord(NULL, ram, regs->addr);
 
 
   FRAMELOG_CMD("Command is 0x%x @ 0x%x available cycles %d %d(%d)\n", command, regs->addr, vdp1_clock, yabsys.LineCount, yabsys.DecilineCount);
 
   Vdp1External.checkEDSR = 0;
 
   yabsys.vdp1cycles = 0;
   //Shall continue is used for prohibited usage of ENd bit. In case a command is valid (like polygon drawing) but with a end bit set, the command is executed then stopped.
   //Not sure it is really stopped in that case, maybe end bit is ignored for other code than 0x8000
   while (!(command & 0x8000) && (nbCmdToProcess < CMD_QUEUE_SIZE) && (CmdListInLoop == 0)) {
     vdp1cmd_struct cmd = {0};
      regs->COPR = (regs->addr & 0x7FFFF) >> 3;
      // First, process the command
      if (!(command & 0x4000)) { // if (!skip)
         int ret;
         if (vdp1_clock <= 0) {
           //No more clock cycle, wait next line
           return;
         }
         Vdp1ReadCommand(&cmd, regs->addr, ram);
         switch (command & 0x000F) {
           case 0: // normal sprite draw
           ret = Vdp1NormalSpriteDraw(&cmd, ram, regs);
           if (ret == 1) nbCmdToProcess++;
           /* ret == 0 : commande valide sans pixel a tracer (table vide,
            * taille nulle, sprite hors du clipping systeme). Le VDP1 passe
            * a la table suivante ; seule une commande invalide (-1) fait
            * attendre la ligne suivante. */
           else if (ret < 0) {
             FRAMELOG_CMD("Reset vdp1_clock %d %d\n", yabsys.LineCount, __LINE__);
             vdp1_clock = 0; //Incorrect command, wait next line to continue
           }
           break;
           case 1: // scaled sprite draw
           ret = Vdp1ScaledSpriteDraw(&cmd, ram, regs);
           if (ret == 1) nbCmdToProcess++;
           else {
             FRAMELOG_CMD("Reset vdp1_clock %d %d\n", yabsys.LineCount, __LINE__);
             vdp1_clock = 0; //Incorrect command, wait next line to continue
           }
           break;
           case 2: // distorted sprite draw
           case 3: /* this one should be invalid, but some games
           (Hardcore 4x4 for instance) use it instead of 2 */
           ret = Vdp1DistortedSpriteDraw(&cmd, ram, regs);
           if (ret == 1) nbCmdToProcess++;
           else {
             FRAMELOG_CMD("Reset vdp1_clock %d %d\n", yabsys.LineCount, __LINE__);
             vdp1_clock = 0; //Incorrect command, wait next line to continue
           }
           break;
           case 4: // polygon draw
             nbCmdToProcess += Vdp1PolygonDraw(&cmd, ram, regs);
             break;
             case 5: // polyline draw
             case 7: // undocumented mirror
             nbCmdToProcess += Vdp1PolylineDraw(&cmd, ram, regs);
             break;
             case 6: // line draw
             nbCmdToProcess += Vdp1LineDraw(&cmd, ram, regs);
             break;
             case 8: // user clipping coordinates
             /* VDP1 Manual §6.1 p.71: command table = 30 bytes, fetched as
              * 32-byte aligned block. No pixels drawn — fetch cost only.
              * At 28MHz pixel clock: 32 bytes / 2 bytes per cycle = 16 cycles.
              * VDP1 Manual §6.3 p.74 Table 6.1: set command overhead = 16. */
             yabsys.vdp1cycles += 16;
             VIDCore->Vdp1UserClipping(&cmd, ram, regs);
             break;
             case 11: // undocumented command
              //Do nothing as we are skipping it.
             break;
             case 9: // system clipping coordinates
             /* VDP1 Manual §6.1/§6.3 Table 6.1: same fetch overhead as
              * user clipping — 16 cycles, no pixels drawn. */
             yabsys.vdp1cycles += 16;
             VIDCore->Vdp1SystemClipping(&cmd, ram, regs);
             break;
             case 10: // local coordinate
             /* VDP1 Manual §6.1/§6.3 Table 6.1: local coordinate set command
              * has fetch overhead only — 16 cycles, no pixels drawn. */
             yabsys.vdp1cycles += 16;
             VIDCore->Vdp1LocalCoordinate(&cmd, ram, regs);
             break;
             default: // Abort
             FRAMELOG("vdp1\t: Bad command: %x\n", command);
             Vdp1External.status &= ~VDP1_STATUS_MASK;
             Vdp1External.status |= VDP1_STATUS_IDLE;
             if (VIDCore->endVdp1Render) VIDCore->endVdp1Render();
             regs->COPR = (regs->addr & 0x7FFFF) >> 3;
             /* Do NOT touch EDSR here.
              *
              * CEF (bit 1) is already 0: VDP1 Manual §4.4 p.52 says it "is
              * reset to 0 when the frame buffers are changed or when drawing
              * is started", and only the fetch of a draw end command sets it.
              * An invalid command is not an end-bit fetch, so CEF correctly
              * stays 0 and there is nothing to clear.
              *
              * BEF (bit 0) belongs to the PREVIOUS frame. §4.4 p.53: it "is
              * written with the value of the CEF value when the frame buffer
              * is changed or at the start of drawing, and is maintained until
              * the next frame buffer change". Nothing in the spec lets a
              * malformed command of the current frame disturb it.
              *
              * The old `regs->EDSR = 0` cleared both. Since BEF = 0 is the
              * transfer-over indication ("the end bit in previous frame has
              * not been fetched"), it did not merely lose information, it
              * fabricated a transfer-over report -- a title that polls BEF to
              * decide whether to shorten its command list would throttle
              * itself for a frame that in fact completed. */
             return;
           }
      } else {
        yabsys.vdp1cycles += 16;
      }
      vdp1StatsCmd(regs->addr, yabsys.vdp1cycles, (command & 0x4000) != 0);
      vdp1_clock -= yabsys.vdp1cycles;
      yabsys.vdp1cycles = 0;
 
      // Next, determine where to go next
      switch ((command & 0x3000) >> 12) {
      case 0: // NEXT, jump to following table
         regs->addr += 0x20;
         break;
      case 1: // ASSIGN, jump to CMDLINK
        {
          u32 oldAddr = regs->addr;
          u32 target = T1ReadWord(ram, regs->addr + 2) * 8;
          /* VDP1 Manual §6.1: LINK target must be 32-byte aligned and
           * within VRAM. Out-of-range link terminates the list. */
          if (target > 0x7FFE0) {
            FRAMELOG("VDP1 ASSIGN target out of range: 0x%x\n", target);
            Vdp1External.status &= ~VDP1_STATUS_MASK;
            Vdp1External.status |= VDP1_STATUS_IDLE;
            if (VIDCore->endVdp1Render) VIDCore->endVdp1Render();
            return;
          }
          regs->addr = target;
          if (((regs->addr == oldAddr) && (command & 0x4000)) || (regs->addr == 0))   {
            //The next adress is the same as the old adress and the command is skipped => Exit
            //The next adress is the start of the command list. It means the list has an infinte loop => Exit (used by Burning Rangers)
            //another example is Kanzen Chuukei Pro Yakyuu
            Vdp1LoopAddr = regs->addr + 2;
            regs->lCOPR = (regs->addr & 0x7FFFF) >> 3;
            FRAMELOG_CMD("Reset vdp1_clock addr = %x %d %d\n", regs->addr, yabsys.LineCount, __LINE__);
            vdp1_clock = 0;
            CmdListInLoop = 1;
            return;
          }
        }
         break;
      case 2: // CALL, call a subroutine
         if (g_vdp1DrawReturnAddr == 0xFFFFFFFF)
            g_vdp1DrawReturnAddr = regs->addr + 0x20;
         {
            u32 target = T1ReadWord(ram, regs->addr + 2) * 8;
            if (target > 0x7FFE0) {
               FRAMELOG("VDP1 CALL target out of range: 0x%x\n", target);
               Vdp1External.status &= ~VDP1_STATUS_MASK;
               Vdp1External.status |= VDP1_STATUS_IDLE;
               if (VIDCore->endVdp1Render) VIDCore->endVdp1Render();
               return;
            }
            regs->addr = target;
         }
         break;
		case 3: // RETURN, return from subroutine
		   if (g_vdp1DrawReturnAddr != 0xFFFFFFFF) {
			  regs->addr = g_vdp1DrawReturnAddr;
			  g_vdp1DrawReturnAddr = 0xFFFFFFFF;
		   }
		   else {
			  /* VDP1 Manual §6.1: RETURN without matching CALL is
			   * undefined. Fall back to NEXT behaviour and log. */
			  FRAMELOG("VDP1 RETURN without CALL at 0x%x\n", regs->addr);
			  regs->addr += 0x20;
		   }
		   break;
      }
 
      command = Vdp1RamReadWord(NULL,ram, regs->addr);
      FRAMELOG_CMD("Command is 0x%x @ 0x%x\n", command, regs->addr);
      //If we change directly CPR to last value, scorcher will not boot.
      //If we do not change it, Noon will not start
      //So store the value and update COPR with last value at VBlank In
      regs->lCOPR = (regs->addr & 0x7FFFF) >> 3;
   }
   if (command & 0x8000) {
     if (vdp1_clock >= 0) {
       FRAMELOG("VDP1: Command Finished! count = %d @ %08X\n", nbCmdToProcess, regs->addr);
       Vdp1External.status &= ~VDP1_STATUS_MASK;
       Vdp1External.status |= VDP1_STATUS_IDLE;
       if (VIDCore->endVdp1Render) VIDCore->endVdp1Render();
       regs->COPR = (regs->addr & 0x7FFFF) >> 3;
       regs->lCOPR = (regs->addr & 0x7FFFF) >> 3;
       FRAMELOG("Set EDSR\n");
       regs->EDSR |= 2;
       Vdp1LoopAddr = -1;
       vdp1StatsListEnd();
     } else {
       FRAMELOG("Wait a bit before the stop. Enough for the command to end\n");
     }
   }
}
 
//ensure that registers are set correctly
void Vdp1FakeDrawCommands(u8 * ram, Vdp1 * regs)
{
   u16 command = T1ReadWord(ram, regs->addr);
   u32 commandCounter = 0;
   u32 returnAddr = 0xffffffff;
   vdp1cmd_struct cmd;
   // Vdp1Regs->EDSR >>= 1;
 
   while (!(command & 0x8000) && commandCounter < 2000) { // fix me
      // First, process the command
      if (!(command & 0x4000)) { // if (!skip)
         switch (command & 0x000F) {
         case 0: // normal sprite draw
         case 1: // scaled sprite draw
         case 2: // distorted sprite draw
         case 3: /* this one should be invalid, but some games
                 (Hardcore 4x4 for instance) use it instead of 2 */
         case 4: // polygon draw
         case 5: // polyline draw
         case 6: // line draw
         case 7: // undocumented polyline draw mirror
         case 11: // undocumented command - do nnothing
            break;
         case 8: // user clipping coordinates
            Vdp1ReadCommand(&cmd, regs->addr, ram);
            VIDCore->Vdp1UserClipping(&cmd, ram, regs);
            break;
         case 9: // system clipping coordinates
            Vdp1ReadCommand(&cmd, regs->addr, ram);
            VIDCore->Vdp1SystemClipping(&cmd, ram, regs);
            break;
         case 10: // local coordinate
            Vdp1ReadCommand(&cmd, regs->addr, ram);
            VIDCore->Vdp1LocalCoordinate(&cmd, ram, regs);
            break;
         default: // Abort
            FRAMELOG("vdp1\t: Bad command: %x\n", command);
            // regs->EDSR |= 2;
            regs->COPR = regs->addr >> 3;
            return;
         }
      }
 
      // Next, determine where to go next
      switch ((command & 0x3000) >> 12) {
      case 0: // NEXT, jump to following table
         regs->addr += 0x20;
         break;
      case 1: // ASSIGN, jump to CMDLINK
         /* VDP1 Manual §6.1 p.71: LINK target = CMDLINK*8H, must lie
          * within VRAM (0x00000..0x7FFE0).  Out-of-range link is
          * undefined hardware behaviour — terminate the fake walk
          * to mirror the real Vdp1DrawCommands logic above. */
         {
            u32 t = T1ReadWord(ram, regs->addr + 2) * 8;
            if (t > 0x7FFE0) { regs->EDSR |= 2; return; }
            regs->addr = t;
         }
         break;
      case 2: // CALL, call a subroutine
         if (returnAddr == 0xFFFFFFFF)
            returnAddr = regs->addr + 0x20;
 
         {
            u32 t = T1ReadWord(ram, regs->addr + 2) * 8;
            if (t > 0x7FFE0) { regs->EDSR |= 2; return; }
            regs->addr = t;
         }
         break;
      case 3: // RETURN, return from subroutine
         if (returnAddr != 0xFFFFFFFF) {
            regs->addr = returnAddr;
            returnAddr = 0xFFFFFFFF;
         }
         else
            regs->addr += 0x20;
         break;
      }
 
      command = T1ReadWord(ram, regs->addr);
      commandCounter++;
   }
   if (command & 0x8000) {
     regs->EDSR |= 2;
   }
}
 
static int Vdp1Draw(void)
{
  FRAMELOG_CMD("Vdp1Draw %d\n", yabsys.LineCount);
  VIDCore->Vdp1Draw();
 
  if ((Vdp1External.status & VDP1_STATUS_MASK) != VDP1_STATUS_IDLE)
    return 1;
 
  FRAMELOG("Vdp1Draw end at line %d \n", yabsys.LineCount);
 
  /* VDP1 Manual §4.4 p.52: "When the draw end command is fetched, the VDP1
   * sets CEF to 1 and generates an interrupt signal." The interrupt and
   * CEF are two faces of the same event -- the fetch of the end bit -- and
   * the manual offers them as the two interchangeable ways of detecting the
   * end of drawing ("one confirms the fetch status of the end bit with CEF
   * (polling) and the other uses the interrupt signal"). They must agree.
   *
   * Going idle is a weaker condition than fetching the end bit.
   * Vdp1DrawCommands() also drops to VDP1_STATUS_IDLE on four failure
   * paths -- address error past 0x7FFFF, unknown command code, and an
   * out-of-range ASSIGN or CALL target -- none of which fetch an end bit
   * and none of which set CEF. Firing unconditionally sent the SCU Sprite
   * Draw End interrupt (vector 0x4D, level 2, SCU Manual ST-097-R5 §2.2)
   * on a list that never completed, telling a title its frame was ready
   * while EDSR simultaneously told it the opposite.
   *
   * Gate on CEF so the interrupt and the register cannot disagree. CEF is
   * cleared at the start of every draw, so it can only be 1 here because
   * this list reached its draw end command. */
  if (Vdp1Regs->EDSR & 0x2)
    ScuSendDrawEnd();
 
  return 0;
}
 
//////////////////////////////////////////////////////////////////////////////
 
static void Vdp1NoDraw(void) {
   Vdp1Regs->lCOPR = 0;
   Vdp1External.status &= ~VDP1_STATUS_MASK;
   Vdp1External.status |= VDP1_STATUS_IDLE;
   if (VIDCore->endVdp1Render) VIDCore->endVdp1Render();
   Vdp1FakeDrawCommands(Vdp1Ram, Vdp1Regs);
}
 
//////////////////////////////////////////////////////////////////////////////
 
static void FASTCALL Vdp1ReadCommand(vdp1cmd_struct *cmd, u32 addr, u8* ram) {
    /* VDP1 Manual §6.2 p.71: "command table is defined on a 20H-byte
     * boundary, the lower 5 bits of the address are fixed at 00000B".
     * Align to 32 bytes (not 16) so LINK chains that cross tables do
     * not read half of one command + half of the next. */
   addr &= 0x7FFE0;
   cmd->CMDCTRL = T1ReadWord(ram, addr);
   cmd->CMDLINK = T1ReadWord(ram, addr + 0x2);
   cmd->CMDPMOD = T1ReadWord(ram, addr + 0x4);
   cmd->CMDCOLR = T1ReadWord(ram, addr + 0x6);
   cmd->CMDSRCA = T1ReadWord(ram, addr + 0x8);
   cmd->CMDSIZE = T1ReadWord(ram, addr + 0xA);
   cmd->CMDXA = T1ReadWord(ram, addr + 0xC);
   cmd->CMDYA = T1ReadWord(ram, addr + 0xE);
   cmd->CMDXB = T1ReadWord(ram, addr + 0x10);
   cmd->CMDYB = T1ReadWord(ram, addr + 0x12);
   cmd->CMDXC = T1ReadWord(ram, addr + 0x14);
   cmd->CMDYC = T1ReadWord(ram, addr + 0x16);
   cmd->CMDXD = T1ReadWord(ram, addr + 0x18);
   cmd->CMDYD = T1ReadWord(ram, addr + 0x1A);
   cmd->CMDGRDA = T1ReadWord(ram, addr + 0x1C);
}
 
//////////////////////////////////////////////////////////////////////////////
 
 
int Vdp1SaveState(void ** stream)
{
   int offset;
#ifdef IMPROVED_SAVESTATES
   int i = 0;
   u8 back_framebuffer[0x40000] = { 0 };
#endif
 
   /* Version 3 : frame buffer sérialisé en 16 bits sans perte. */
   offset = MemStateWriteHeader(stream, "VDP1", 3);
 
   // Write registers
   MemStateWrite((void *)Vdp1Regs, sizeof(Vdp1), 1, stream);
 
   // Write VDP1 ram
   MemStateWrite((void *)Vdp1Ram, 0x80000, 1, stream);
 
#ifdef IMPROVED_SAVESTATES
   /* Lecture du pixel 16 bits complet via l'accesseur Word et stockage
    * octet haut puis octet bas (ordre interne au savestate ; seule la
    * cohérence save/load importe). */
   for (i = 0; i < 0x40000; i += 2) {
      u16 px = Vdp1FrameBuffer16bReadWord(NULL, NULL, i);
      back_framebuffer[i]     = (u8)((px >> 8) & 0xFF);
      back_framebuffer[i + 1] = (u8)( px       & 0xFF);
   }
   MemStateWrite((void *)back_framebuffer, 0x40000, 1, stream);
#endif
 
    // VDP1 status
   int size = sizeof(Vdp1External_struct);
   MemStateWrite((void *)(&size), sizeof(int),1,stream);
   MemStateWrite((void *)(&Vdp1External), sizeof(Vdp1External_struct),1,stream);
   return MemStateFinishHeader(stream, offset);
}
 
 
//////////////////////////////////////////////////////////////////////////////
 
int Vdp1LoadState(const void * stream, UNUSED int version, int size)
{
#ifdef IMPROVED_SAVESTATES
   int i = 0;
   u8 back_framebuffer[0x40000] = { 0 };
#endif
   // Read registers
   MemStateRead((void *)Vdp1Regs, sizeof(Vdp1), 1, stream);
   // Read VDP1 ram
   MemStateRead((void *)Vdp1Ram, 0x80000, 1, stream);
   vdp1Ram_update_start = 0x0;
   vdp1Ram_update_end = 0x80000;
#ifdef IMPROVED_SAVESTATES
   MemStateRead((void *)back_framebuffer, 0x40000, 1, stream);
   YglGenerate();
   if (version >= 3) {
      /* v3+ : restauration 16 bits sans perte. Les accesseurs Byte
       * ignorent (addr & 1) et mettent l'octet haut du pixel à zéro ;
       * on passe donc par l'accesseur Word, qui gère le pixel complet.
       * Ordre interne : octet haut puis octet bas (cf. Vdp1SaveState). */
      for (i = 0; i < 0x40000; i += 2) {
         u16 px = ((u16)back_framebuffer[i] << 8) | back_framebuffer[i + 1];
         Vdp1FrameBuffer16bWriteWord(NULL, NULL, i, px);
      }
   } else {
      /* v2 : ces sauvegardes contenaient déjà un FB 8 bits/pixel (octet
       * haut perdu au moment du save). On rejoue l'ancienne sémantique
       * pour les charger exactement comme avant — jamais pire. */
      for (i = 0; i < 0x40000; i++)
         Vdp1FrameBuffer16bWriteByte(NULL, NULL, i, back_framebuffer[i]);
   }
#endif
   {
   /* disptoggle est le choix de l'utilisateur (ToggleVDP1, affichage des
    * sprites), pas un etat de la machine : il ne doit pas venir du fichier.
    * Surtout, quand le bloc Vdp1External n'est pas relisible (etat ancien,
    * ou structure agrandie depuis : eraseOverrun/eraseOverrunLines ont ete
    * ajoutes), le memset le mettait a 0 et common_glshader.c n'affichait
    * plus du tout la couche VDP1 apres le chargement. */
   const int userDisptoggle = Vdp1External.disptoggle;
   if (version > 1) {
     int extsize = 0;
     MemStateRead((void *)(&extsize), sizeof(int), 1, stream);
     if (extsize == sizeof(Vdp1External_struct)) {
        MemStateRead((void *)(&Vdp1External), sizeof(Vdp1External_struct),1,stream);
     } else {
       YuiMsg("Too old savestate, can not restore Vdp1External\n");
       memset((void *)(&Vdp1External), 0, sizeof(Vdp1External_struct));
       /* Les champs connus en tete de la structure sont restes au meme
        * endroit : on relit ce qui est commun au lieu de tout perdre. */
       if ((extsize > 0) && (extsize < (int)sizeof(Vdp1External_struct)))
          MemStateRead((void *)(&Vdp1External), extsize, 1, stream);
     }
   } else {
     YuiMsg("Too old savestate, can not restore Vdp1External\n");
     memset((void *)(&Vdp1External), 0, sizeof(Vdp1External_struct));
   }
   Vdp1External.disptoggle = userDisptoggle;
   }
   Vdp1External.updateVdp1Ram = 1;
   /* Le verrou n'est pas serialise (format de savestate inchange) : on le
    * reprend des registres restaures, comme au dernier swap. */
   Vdp1LatchEraseParameters();
   if (Vdp1Regs->TVMR & 0x1) switchFB8bit();
   else switchFB16bit();
   return size;
}
 
//////////////////////////////////////////////////////////////////////////////
 
static u32 Vdp1DebugGetCommandNumberAddr(u32 number)
{
   u32 addr = 0;
   u32 returnAddr = 0xFFFFFFFF;
   u32 commandCounter = 0;
   u16 command;
 
   command = T1ReadWord(Vdp1DebugMem(), addr);
 
   while (!(command & 0x8000) && (commandCounter != number) && (commandCounter<2000))
   {
      // Determine where to go next
      switch ((command & 0x3000) >> 12)
      {
         case 0: // NEXT, jump to following table
            addr += 0x20;
            break;
         case 1: // ASSIGN, jump to CMDLINK
            addr = T1ReadWord(Vdp1DebugMem(), addr + 2) * 8;
            break;
         case 2: // CALL, call a subroutine
            if (returnAddr == 0xFFFFFFFF)
               returnAddr = addr + 0x20;
 
            addr = T1ReadWord(Vdp1DebugMem(), addr + 2) * 8;
            break;
         case 3: // RETURN, return from subroutine
            if (returnAddr != 0xFFFFFFFF) {
               addr = returnAddr;
               returnAddr = 0xFFFFFFFF;
            }
            else
               addr += 0x20;
            break;
      }
 
      if (addr > 0x7FFE0)
         return 0xFFFFFFFF;
      command = T1ReadWord(Vdp1DebugMem(), addr);
      commandCounter++;
   }
 
   if (commandCounter == number)
      return addr;
   else
      return 0xFFFFFFFF;
}
 
//////////////////////////////////////////////////////////////////////////////
 
Vdp1CommandType Vdp1DebugGetCommandTypeAtAddr(u32 addr)
{
   if (addr != 0xFFFFFFFF)
   {
      const u16 command = T1ReadWord(Vdp1DebugMem(), addr);
      if (command & 0x8000)
        return VDPCT_DRAW_END;
      else if ((command & 0x000F) < VDPCT_INVALID)
        return (Vdp1CommandType) (command & 0x000F);
   }
 
   return VDPCT_INVALID;
}
 
Vdp1CommandType Vdp1DebugGetCommandType(u32 number)
{
   return Vdp1DebugGetCommandTypeAtAddr(Vdp1DebugGetCommandNumberAddr(number));
}
 
u32 Vdp1DebugGetCommandAddr(u32 number) {
  return Vdp1DebugGetCommandNumberAddr(number);
}
 
/* Parcourt la table de commandes UNE seule fois et range l'adresse de chaque
 * commande (Draw End compris, en derniere position) dans addrs[].
 *
 * Remplir la liste de l'interface avec Vdp1DebugGetCommandAddr(i) pour
 * i = 0..n reparcourait la table depuis l'adresse 0 a chaque entree : O(n^2),
 * et plafonne a 2000 commandes par Vdp1DebugGetCommandNumberAddr(). La VRAM
 * VDP1 (512 Ko) contient au plus 0x80000 / 0x20 = 16384 tables : c'est la
 * borne naturelle, qui protege aussi contre une liste qui boucle sur
 * elle-meme (ASSIGN vers sa propre adresse).
 *
 * Saut CALL/RETURN : un seul niveau, comme Vdp1DebugGetCommandNumberAddr()
 * et le moteur de trace (ST-013-R3 �5 : pas d'imbrication de sous-routines).
 * Renvoie le nombre d'adresses ecrites. */
int Vdp1DebugGetCommandList(u32 *addrs, int maxCount)
{
   u8 *ram = Vdp1DebugMem();
   u32 addr = 0;
   u32 returnAddr = 0xFFFFFFFF;
   int count = 0;
 
   if ((ram == NULL) || (addrs == NULL) || (maxCount <= 0))
      return 0;
 
   while (count < maxCount)
   {
      u16 command = T1ReadWord(ram, addr);
      addrs[count++] = addr;
 
      if (command & 0x8000)
         break; // Draw End
 
      switch ((command & 0x3000) >> 12)
      {
         case 0: // NEXT
            addr += 0x20;
            break;
         case 1: // ASSIGN
            addr = T1ReadWord(ram, addr + 2) * 8;
            break;
         case 2: // CALL
            if (returnAddr == 0xFFFFFFFF)
               returnAddr = addr + 0x20;
            addr = T1ReadWord(ram, addr + 2) * 8;
            break;
         case 3: // RETURN
            if (returnAddr != 0xFFFFFFFF) {
               addr = returnAddr;
               returnAddr = 0xFFFFFFFF;
            }
            else
               addr += 0x20;
            break;
      }
 
      if (addr > 0x7FFE0)
         break;
   }
 
   return count;
}
 
char *Vdp1DebugGetCommandRaw(u32 addr)
{
   u16 command;
   char *out;
 
   if (addr == 0xFFFFFFFF)
      return NULL;
 
   out = (char*)malloc(128*sizeof(char));
   command = T1ReadWord(Vdp1DebugMem(), addr);
 
   if (command & 0x8000) {
     snprintf(out, 128, "END");
     return out;
   }
 
   // Next, determine where to go next
   switch ((command & 0x3000) >> 12) {
   case 0: // NEXT, jump to following table
      snprintf(out, 128, "NEXT 0x%x", addr+0x20);
      break;
   case 1: // ASSIGN, jump to CMDLINK
      snprintf(out, 128, "ASSIGN 0x%x", T1ReadWord(Vdp1DebugMem(), addr + 2) * 8);
      break;
   case 2: // CALL, call a subroutine
      snprintf(out, 128, "CALL 0x%x", T1ReadWord(Vdp1DebugMem(), addr + 2) * 8);
      break;
   case 3: // RETURN, return from subroutine
      snprintf(out, 128, "RETURN");
      break;
   default:
      free(out);
      out = NULL;
   }
   return out;
}
 
char *Vdp1DebugGetCommandNumberName(u32 addr)
{
   u16 command;
 
   if (addr != 0xFFFFFFFF)
   {
      command = T1ReadWord(Vdp1DebugMem(), addr);
 
      if (command & 0x8000)
         return "Draw End";
 
      // Figure out command name
      switch (command & 0x000F)
      {
         case 0:
            return "Normal Sprite";
         case 1:
            return "Scaled Sprite";
         case 2:
            return "Distorted Sprite";
         case 3:
            return "Distorted Sprite *";
         case 4:
            return "Polygon";
         case 5:
            return "Polyline";
         case 6:
            return "Line";
         case 7:
            return "Polyline *";
         case 8:
            return "User Clipping Coordinates";
         case 9:
            return "System Clipping Coordinates";
         case 10:
            return "Local Coordinates";
         case 11:
            return "Command 0xB undocumented";
         default:
             return "Bad command - Abort";
      }
   }
   else
      return NULL;
}
 
//////////////////////////////////////////////////////////////////////////////
 
/* Les trois fonctions de detail ci-dessous prennent desormais une ADRESSE
 * de commande plutot qu'un rang dans la liste. Le rang n'est pas stable :
 * il oblige a reparcourir toute la table a chaque appel (O(n^2) sur le
 * remplissage de la liste) et, surtout, un jeu qui reconstruit sa table a
 * chaque trame -- Doom par exemple -- decale les entrees entre l'instant
 * ou l'interface remplit sa liste et l'instant ou l'utilisateur clique.
 * Le nom affiche et le detail portaient alors sur deux commandes
 * differentes. Les variantes historiques a base de rang restent
 * disponibles, implementees au-dessus des nouvelles. */
void Vdp1DebugCommandAtAddr(u32 addr, char *outstring)
{
   u16 command;
   vdp1cmd_struct cmd;
 
   if (addr == 0xFFFFFFFF)
      return;
 
   command = T1ReadWord(Vdp1DebugMem(), addr);
 
   if (command & 0x8000)
   {
      // Draw End
      outstring[0] = 0x00;
      return;
   }
 
   if (command & 0x4000)
   {
      AddString(outstring, "Command is skipped\r\n");
      return;
   }
 
   Vdp1ReadCommand(&cmd, addr, Vdp1DebugMem());
 
   if ((cmd.CMDCTRL & 0x000F) < 4) {
     cmd.w = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
     cmd.h = cmd.CMDSIZE & 0xFF;
   }
 
   int invalid = 0;
   switch (cmd.CMDCTRL & 0x000F)
   {
      case 0:
         AddString(outstring, "Normal Sprite\r\n");
         AddString(outstring, "CMDXA = 0x%04x, CMDYA = 0x%04x\r\n", cmd.CMDXA&0xFFFF, cmd.CMDYA&0xFFFF);
         invalid = CONVERTCMD(&cmd.CMDXA) || CONVERTCMD(&cmd.CMDYA);
         if (invalid) {
           AddString(outstring, "Invalid coordinates - Not drawn\n");
         } else {
           AddString(outstring, "x = %d, y = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA);
           int cycles = getNormalCycles(&cmd);
           AddString(outstring, "estimated cycles=%d\n", cycles);
         }
 
         break;
      case 1:
         AddString(outstring, "Scaled Sprite\r\n");
 
         AddString(outstring, "Zoom Point: ");
 
         switch ((cmd.CMDCTRL >> 8) & 0xF)
         {
            case 0x0:
               AddString(outstring, "Only two coordinates\r\n");
               invalid = CONVERTCMD(&cmd.CMDXC) || CONVERTCMD(&cmd.CMDYC);
               break;
            case 0x5:
               AddString(outstring, "Upper-left\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            case 0x6:
               AddString(outstring, "Upper-center\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            case 0x7:
               AddString(outstring, "Upper-right\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            case 0x9:
               AddString(outstring, "Center-left\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            case 0xA:
               AddString(outstring, "Center-center\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            case 0xB:
               AddString(outstring, "Center-right\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            case 0xC:
               AddString(outstring, "Lower-left\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            case 0xE:
               AddString(outstring, "Lower-center\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            case 0xF:
               AddString(outstring, "Lower-right\r\n");
               invalid = CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
               break;
            default: break;
         }
 
         AddString(outstring, "CMDXA = 0x%04x, CMDYA = 0x%04x\r\n", cmd.CMDXA&0xFFFF, cmd.CMDYA&0xFFFF);
         AddString(outstring, "CMDXB = 0x%04x, CMDYB = 0x%04x\r\n", cmd.CMDXB&0xFFFF, cmd.CMDYB&0xFFFF);
         AddString(outstring, "CMDXC = 0x%04x, CMDYC = 0x%04x\r\n", cmd.CMDXC&0xFFFF, cmd.CMDYC&0xFFFF);
         invalid |= CONVERTCMD(&cmd.CMDXA) || CONVERTCMD(&cmd.CMDYA);
         if (invalid) {
           AddString(outstring, "Invalid coordinates - Not drawn\n");
         } else {
           s16 x = cmd.CMDXA;
           s16 y = cmd.CMDYA;
           s16 rh = 0;
           s16 rw = 0;
           // Setup Zoom Point
           switch ((cmd.CMDCTRL & 0xF00) >> 8)
           {
           case 0x0: // Only two coordinates
             rw = cmd.CMDXC - cmd.CMDXA;
             rh = cmd.CMDYC - cmd.CMDYA;
             break;
           case 0x5: // Upper-left
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             if ((rw < 0)||(rh <0)) {
               return;
             }
             break;
           case 0x6: // Upper-Center
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             x = x - rw / 2;
             if ((rw < 0)||(rh <0)) {
               return;
             }
             break;
           case 0x7: // Upper-Right
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             x = x - rw;
             if ((rw < 0)||(rh <0)) {
               return;
             }
             break;
           case 0x9: // Center-left
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             y = y - rh / 2;
             if ((rw < 0)||(rh <0)) {
               return;
             }
             break;
           case 0xA: // Center-center
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             x = x - rw / 2;
             y = y - rh / 2;
             break;
           case 0xB: // Center-right
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             x = x - rw;
             y = y - rh / 2;
             if ((rw < 0)||(rh <0)) {
               return;
             }
             break;
           case 0xD: // Lower-left
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             y = y - rh;
             if ((rw < 0)||(rh <0)) {
               return;
             }
             break;
           case 0xE: // Lower-center
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             x = x - rw / 2;
             y = y - rh;
             break;
           case 0xF: // Lower-right
             rw = cmd.CMDXB;
             rh = cmd.CMDYB;
             x = x - rw;
             y = y - rh;
             if ((rw < 0)||(rh <0)) {
               return;
             }
             break;
           default: break;
           }
           cmd.CMDXA = x;
           cmd.CMDYA = y;
           cmd.CMDXB = x + rw;
           cmd.CMDYB = y;
           cmd.CMDXC = x + rw;
           cmd.CMDYC = y + rh;
           cmd.CMDXD = x;
           cmd.CMDYD = y + rh;
           AddString(outstring, "x1 = %d, y1 = %d, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXB, (s16)cmd.CMDYB);
           AddString(outstring, "x3 = %d, y3 = %d, x4 = %d, y4 = %d\r\n", (s16)cmd.CMDXC, (s16)cmd.CMDYC, (s16)cmd.CMDXD, (s16)cmd.CMDYD);
           int cycles = getScaledCycles(&cmd);
           AddString(outstring, "estimated cycles=%d\n", cycles);
         }
         break;
      case 2:
         AddString(outstring, "Distorted Sprite\r\n");
         AddString(outstring, "CMDXA = 0x%04x, CMDYA = 0x%04x\r\n", cmd.CMDXA&0xFFFF, cmd.CMDYA&0xFFFF);
         AddString(outstring, "CMDXB = 0x%04x, CMDYB = 0x%04x\r\n", cmd.CMDXB&0xFFFF, cmd.CMDYB&0xFFFF);
         AddString(outstring, "CMDXC = 0x%04x, CMDYC = 0x%04x\r\n", cmd.CMDXC&0xFFFF, cmd.CMDYC&0xFFFF);
         AddString(outstring, "CMDXD = 0x%04x, CMDYD = 0x%04x\r\n", cmd.CMDXD&0xFFFF, cmd.CMDYD&0xFFFF);
         invalid = CONVERTCMD(&cmd.CMDXA) || CONVERTCMD(&cmd.CMDYA);
         invalid |= CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
         invalid |= CONVERTCMD(&cmd.CMDXC) || CONVERTCMD(&cmd.CMDYC);
         invalid |= CONVERTCMD(&cmd.CMDXD) || CONVERTCMD(&cmd.CMDYD);
         if (invalid) {
           AddString(outstring, "Invalid coordinates - Not drawn\n");
         } else {
           AddString(outstring, "x1 = %d, y1 = %d, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXB, (s16)cmd.CMDYB);
           AddString(outstring, "x3 = %d, y3 = %d, x4 = %d, y4 = %d\r\n", (s16)cmd.CMDXC, (s16)cmd.CMDYC, (s16)cmd.CMDXD, (s16)cmd.CMDYD);
           int cycles = getDistortedCycles(&cmd);
           AddString(outstring, "estimated cycles=%d\n", cycles);
         }
         break;
      case 3:
         AddString(outstring, "Distorted Sprite *\r\n");
         AddString(outstring, "CMDXA = 0x%04x, CMDYA = 0x%04x\r\n", cmd.CMDXA&0xFFFF, cmd.CMDYA&0xFFFF);
         AddString(outstring, "CMDXB = 0x%04x, CMDYB = 0x%04x\r\n", cmd.CMDXB&0xFFFF, cmd.CMDYB&0xFFFF);
         AddString(outstring, "CMDXC = 0x%04x, CMDYC = 0x%04x\r\n", cmd.CMDXC&0xFFFF, cmd.CMDYC&0xFFFF);
         AddString(outstring, "CMDXD = 0x%04x, CMDYD = 0x%04x\r\n", cmd.CMDXD&0xFFFF, cmd.CMDYD&0xFFFF);
         invalid = CONVERTCMD(&cmd.CMDXA) || CONVERTCMD(&cmd.CMDYA);
         invalid |= CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
         invalid |= CONVERTCMD(&cmd.CMDXC) || CONVERTCMD(&cmd.CMDYC);
         invalid |= CONVERTCMD(&cmd.CMDXD) || CONVERTCMD(&cmd.CMDYD);
         if (invalid) {
           AddString(outstring, "Invalid coordinates - Not drawn\n");
         } else {
           AddString(outstring, "x1 = %d, y1 = %d, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXB, (s16)cmd.CMDYB);
           AddString(outstring, "x3 = %d, y3 = %d, x4 = %d, y4 = %d\r\n", (s16)cmd.CMDXC, (s16)cmd.CMDYC, (s16)cmd.CMDXD, (s16)cmd.CMDYD);
           int cycles = getDistortedCycles(&cmd);
           AddString(outstring, "estimated cycles=%d\n", cycles);
         }
         break;
      case 4:
         AddString(outstring, "Polygon\r\n");
         AddString(outstring, "CMDXA = 0x%04x, CMDYA = 0x%04x\r\n", cmd.CMDXA&0xFFFF, cmd.CMDYA&0xFFFF);
         AddString(outstring, "CMDXB = 0x%04x, CMDYB = 0x%04x\r\n", cmd.CMDXB&0xFFFF, cmd.CMDYB&0xFFFF);
         AddString(outstring, "CMDXC = 0x%04x, CMDYC = 0x%04x\r\n", cmd.CMDXC&0xFFFF, cmd.CMDYC&0xFFFF);
         AddString(outstring, "CMDXD = 0x%04x, CMDYD = 0x%04x\r\n", cmd.CMDXD&0xFFFF, cmd.CMDYD&0xFFFF);
         invalid = CONVERTCMD(&cmd.CMDXA) || CONVERTCMD(&cmd.CMDYA);
         invalid |= CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
         invalid |= CONVERTCMD(&cmd.CMDXC) || CONVERTCMD(&cmd.CMDYC);
         invalid |= CONVERTCMD(&cmd.CMDXD) || CONVERTCMD(&cmd.CMDYD);
         if (invalid) {
           AddString(outstring, "Invalid coordinates - Not drawn\n");
         } else {
           AddString(outstring, "x1 = %d, y1 = %d, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXB, (s16)cmd.CMDYB);
           AddString(outstring, "x3 = %d, y3 = %d, x4 = %d, y4 = %d\r\n", (s16)cmd.CMDXC, (s16)cmd.CMDYC, (s16)cmd.CMDXD, (s16)cmd.CMDYD);
           int cycles = getPolygonCycles(&cmd);
           AddString(outstring, "estimated cycles=%d\n", cycles);
         }
         break;
      case 5:
         AddString(outstring, "Polyline\r\n");
         AddString(outstring, "CMDXA = 0x%04x, CMDYA = 0x%04x\r\n", cmd.CMDXA&0xFFFF, cmd.CMDYA&0xFFFF);
         AddString(outstring, "CMDXB = 0x%04x, CMDYB = 0x%04x\r\n", cmd.CMDXB&0xFFFF, cmd.CMDYB&0xFFFF);
         AddString(outstring, "CMDXC = 0x%04x, CMDYC = 0x%04x\r\n", cmd.CMDXC&0xFFFF, cmd.CMDYC&0xFFFF);
         AddString(outstring, "CMDXD = 0x%04x, CMDYD = 0x%04x\r\n", cmd.CMDXD&0xFFFF, cmd.CMDYD&0xFFFF);
         invalid = CONVERTCMD(&cmd.CMDXA) || CONVERTCMD(&cmd.CMDYA);
         invalid |= CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
         invalid |= CONVERTCMD(&cmd.CMDXC) || CONVERTCMD(&cmd.CMDYC);
         invalid |= CONVERTCMD(&cmd.CMDXD) || CONVERTCMD(&cmd.CMDYD);
         if (invalid) {
           AddString(outstring, "Invalid coordinates - Not drawn\n");
         } else {
           AddString(outstring, "x1 = %d, y1 = %d, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXB, (s16)cmd.CMDYB);
           AddString(outstring, "x3 = %d, y3 = %d, x4 = %d, y4 = %d\r\n", (s16)cmd.CMDXC, (s16)cmd.CMDYC, (s16)cmd.CMDXD, (s16)cmd.CMDYD);
         }
         break;
      case 6:
         AddString(outstring, "Line\r\n");
         AddString(outstring, "CMDXA = 0x%04x, CMDYA = 0x%04x\r\n", cmd.CMDXA&0xFFFF, cmd.CMDYA&0xFFFF);
         AddString(outstring, "CMDXB = 0x%04x, CMDYB = 0x%04x\r\n", cmd.CMDXB&0xFFFF, cmd.CMDYB&0xFFFF);
         invalid = CONVERTCMD(&cmd.CMDXA) || CONVERTCMD(&cmd.CMDYA);
         invalid |= CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
         if (invalid) {
           AddString(outstring, "Invalid coordinates - Not drawn\n");
         } else {
           AddString(outstring, "x1 = %d, y1 = %d, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXB, (s16)cmd.CMDYB);
         }
         break;
      case 7:
         AddString(outstring, "Polyline *\r\n");
         AddString(outstring, "CMDXA = 0x%04x, CMDYA = 0x%04x\r\n", cmd.CMDXA&0xFFFF, cmd.CMDYA&0xFFFF);
         AddString(outstring, "CMDXB = 0x%04x, CMDYB = 0x%04x\r\n", cmd.CMDXB&0xFFFF, cmd.CMDYB&0xFFFF);
         AddString(outstring, "CMDXC = 0x%04x, CMDYC = 0x%04x\r\n", cmd.CMDXC&0xFFFF, cmd.CMDYC&0xFFFF);
         AddString(outstring, "CMDXD = 0x%04x, CMDYD = 0x%04x\r\n", cmd.CMDXD&0xFFFF, cmd.CMDYD&0xFFFF);
         invalid = CONVERTCMD(&cmd.CMDXA) || CONVERTCMD(&cmd.CMDYA);
         invalid |= CONVERTCMD(&cmd.CMDXB) || CONVERTCMD(&cmd.CMDYB);
         invalid |= CONVERTCMD(&cmd.CMDXC) || CONVERTCMD(&cmd.CMDYC);
         invalid |= CONVERTCMD(&cmd.CMDXD) || CONVERTCMD(&cmd.CMDYD);
         if (invalid) {
           AddString(outstring, "Invalid coordinates - Not drawn\n");
         } else {
           AddString(outstring, "x1 = %d, y1 = %d, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXB, (s16)cmd.CMDYB);
           AddString(outstring, "x3 = %d, y3 = %d, x4 = %d, y4 = %d\r\n", (s16)cmd.CMDXC, (s16)cmd.CMDYC, (s16)cmd.CMDXD, (s16)cmd.CMDYD);
         }
         break;
      case 8:
         AddString(outstring, "User Clipping\r\n");
         AddString(outstring, "x1 = %d, y1 = %d, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXC, (s16)cmd.CMDYC);
         break;
      case 9:
         AddString(outstring, "System Clipping\r\n");
         AddString(outstring, "x1 = 0, y1 = 0, x2 = %d, y2 = %d\r\n", (s16)cmd.CMDXC, (s16)cmd.CMDYC);
         break;
      case 10:
         AddString(outstring, "Local Coordinates\r\n");
         AddString(outstring, "x = %d, y = %d\r\n", (s16)cmd.CMDXA, (s16)cmd.CMDYA);
         break;
      default:
         AddString(outstring, "Invalid command\r\n");
         return;
   }
 
   // Only Sprite commands use CMDSRCA, CMDSIZE
   if (!(cmd.CMDCTRL & 0x000C))
   {
      AddString(outstring, "Texture address = %08X\r\n", ((unsigned int)cmd.CMDSRCA) << 3);
      AddString(outstring, "Texture width = %d, height = %d\r\n", MAX(1, (cmd.CMDSIZE & 0x3F00) >> 5), MAX(1,cmd.CMDSIZE & 0xFF));
      if ((((cmd.CMDSIZE & 0x3F00) >> 5)==0) || ((cmd.CMDSIZE & 0xFF)==0)) AddString(outstring, "Texture malformed \r\n");
      AddString(outstring, "Texture read direction: ");
 
      switch ((cmd.CMDCTRL >> 4) & 0x3)
      {
         case 0:
            AddString(outstring, "Normal\r\n");
            break;
         case 1:
            AddString(outstring, "Reversed horizontal\r\n");
            break;
         case 2:
            AddString(outstring, "Reversed vertical\r\n");
            break;
         case 3:
            AddString(outstring, "Reversed horizontal and vertical\r\n");
            break;
         default: break;
      }
   }
 
   // Only draw commands use CMDPMOD
   if (!(cmd.CMDCTRL & 0x0008))
   {
      if (cmd.CMDPMOD & 0x8000)
      {
         AddString(outstring, "MSB On (MSBON)\r\n");
      }
 
      if (cmd.CMDPMOD & 0x1000)
      {
         AddString(outstring, "High Speed Shrink Enabled\r\n");
      }
 
      if (!(cmd.CMDPMOD & 0x0800))
      {
         AddString(outstring, "Pre-clipping Enabled\r\n");
      }
 
      if (cmd.CMDPMOD & 0x0400)
      {
         AddString(outstring, "User Clipping Enabled\r\n");
         AddString(outstring, "Clipping Mode = %d\r\n", (cmd.CMDPMOD >> 9) & 0x1);
      }
 
      if (cmd.CMDPMOD & 0x0100)
      {
         AddString(outstring, "Mesh Enabled\r\n");
      }
 
      if (!(cmd.CMDPMOD & 0x0080))
      {
         AddString(outstring, "End Code Enabled\r\n");
      }
 
      if (!(cmd.CMDPMOD & 0x0040))
      {
         AddString(outstring, "Transparent Pixel Enabled\r\n");
      }
 
      if (cmd.CMDCTRL & 0x0004){
          AddString(outstring, "Non-textured color: %04X\r\n", cmd.CMDCOLR);
      } else {
          AddString(outstring, "Color mode: ");
 
          switch ((cmd.CMDPMOD >> 3) & 0x7)
          {
             case 0:
                AddString(outstring, "4 BPP(16 color bank)\r\n");
                AddString(outstring, "Color bank: %08X\r\n", (cmd.CMDCOLR));
                break;
             case 1:
                AddString(outstring, "4 BPP(16 color LUT)\r\n");
                AddString(outstring, "Color lookup table: %08X\r\n", (cmd.CMDCOLR));
                break;
             case 2:
                AddString(outstring, "8 BPP(64 color bank)\r\n");
                AddString(outstring, "Color bank: %08X\r\n", (cmd.CMDCOLR));
                break;
             case 3:
                AddString(outstring, "8 BPP(128 color bank)\r\n");
                AddString(outstring, "Color bank: %08X\r\n", (cmd.CMDCOLR));
                break;
             case 4:
                AddString(outstring, "8 BPP(256 color bank)\r\n");
                AddString(outstring, "Color bank: %08X\r\n", (cmd.CMDCOLR));
                break;
             case 5:
                AddString(outstring, "15 BPP(RGB)\r\n");
                break;
             default: break;
          }
        }
 
      AddString(outstring, "Color Calc. mode: ");
 
      switch (cmd.CMDPMOD & 0x7)
      {
         case 0:
            AddString(outstring, "Replace\r\n");
            break;
         case 1:
            AddString(outstring, "Shadow\r\n");
            break;
         case 2:
            AddString(outstring, "Half-luminance\r\n");
            break;
         case 3:
            AddString(outstring, "Half-transparency\r\n");
            break;
         case 4:
            AddString(outstring, "Gouraud Shading\r\n");
            AddString(outstring, "Gouraud Shading Table = %08X\r\n", ((unsigned int)cmd.CMDGRDA) << 3);
            break;
         case 6:
            AddString(outstring, "Gouraud Shading + Half-luminance\r\n");
            AddString(outstring, "Gouraud Shading Table = %08X\r\n", ((unsigned int)cmd.CMDGRDA) << 3);
            break;
         case 5:
            /* CCB = 101b : reglage interdit. Le "default: break;" laissait
             * la ligne "Color Calc. mode: " sans valeur. */
            AddString(outstring, "Setting prohibited (5)\r\n");
            break;
         case 7:
            AddString(outstring, "Gouraud Shading + Half-transparency\r\n");
            AddString(outstring, "Gouraud Shading Table = %08X\r\n", ((unsigned int)cmd.CMDGRDA) << 3);
            break;
         default: break;
      }
   }
}
 
//////////////////////////////////////////////////////////////////////////////
 
static u32 ColorRamGetColor(u32 colorindex)
{
   switch(Vdp2Internal.ColorMode)
   {
      case 0:
      case 1:
      {
         u32 tmp;
         colorindex <<= 1;
         tmp = T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
         return SAT2YAB1(0xFF, tmp);
      }
      case 2:
      {
         u32 tmp1, tmp2;
         colorindex <<= 2;
         colorindex &= 0xFFF;
         tmp1 = T2ReadWord(Vdp2ColorRam, colorindex);
         tmp2 = T2ReadWord(Vdp2ColorRam, colorindex+2);
         return SAT2YAB2(0xFF, tmp1, tmp2);
      }
      default: break;
   }
 
   return 0;
}
 
//////////////////////////////////////////////////////////////////////////////
 
/* Couleur qu'affiche le VDP2 pour une donnee de sprite 16 bits, telle que le
 * VDP1 l'ecrit dans le frame buffer (mode banque : CMDCOLR | code ; mode LUT :
 * l'entree de la table ; mode RGB : le mot de texture ; polygone : CMDCOLR).
 *
 * Le VDP2 ne sait pas comment la donnee a ete produite. Il l'interprete selon
 * SPCTL :
 *   - bit 5 (SPCLMD) = 1 et MSB = 1 : couleur RGB 5:5:5 ;
 *   - sinon donnee palette : le code couleur (DC) est extrait selon le type de
 *     sprite (SPCTL bits 3-0), les bits au-dessus sont priorite / calcul de
 *     couleur / ombre, puis le decalage SPCAOS (CRAOFB bits 6-4, x 0x100) est
 *     ajoute pour former l'index en Color RAM.
 *
 * Largeur du DC par type : Saturn Technical Bulletin #25, tableau "Normal
 * Shadow Data According to Sprite Type" (types 0-3 et 5 : 11 bits ; 4 et 6 :
 * 10 ; 7 : 9 ; 8 : 7 ; 9-B : 6 ; C-F : 8). Bit SPCLMD : VDP2 User's Manual,
 * registre SPCTL p.207 (absent du workspace ; meme decodage dans Mednafen et
 * Ymir).
 *
 * Avant, l'apercu ajoutait le decalage a la donnee BRUTE (sans masque DC) et
 * prenait tout mot a MSB = 1 pour du RGB, quel que soit SPCLMD : les bits de
 * priorite d'une banque CMDCOLR deplacaient l'index, et un jeu en mode
 * "palette seule" voyait ses texels a MSB = 1 affiches en RGB. */
static const u16 Vdp1DebugSpriteDCMask[16] = {
   0x7FF, 0x7FF, 0x7FF, 0x7FF, 0x3FF, 0x7FF, 0x3FF, 0x1FF,
   0x07F, 0x03F, 0x03F, 0x03F, 0x0FF, 0x0FF, 0x0FF, 0x0FF
};
 
static int Vdp1DebugIsRgbData(u32 data)
{
   return ((Vdp2Regs->SPCTL & 0x20) != 0) && ((data & 0x8000) != 0);
}
 
static u32 Vdp1DebugPaletteIndex(u32 data)
{
   return (data & Vdp1DebugSpriteDCMask[Vdp2Regs->SPCTL & 0xF])
          + ((Vdp2Regs->CRAOFB & 0x70) << 4);
}
 
static u32 Vdp1DebugFbDataColor(u32 data)
{
   if (Vdp1DebugIsRgbData(data))
      return SAT2YAB1(0xFF, data);
   return ColorRamGetColor(Vdp1DebugPaletteIndex(data));
}
 
/* Versions exportees, pour l'editeur graphique (vdpedit.c) : la couleur et
 * l'index de Color RAM d'une donnee de sprite suivent exactement les regles
 * de l'apercu ci-dessus. Index -1 = donnee RGB. */
u32 Vdp1DebugSpriteDataColor(u32 data)
{
   return Vdp1DebugFbDataColor(data);
}

int Vdp1DebugSpriteDataCramIndex(u32 data)
{
   if (Vdp1DebugIsRgbData(data))
      return -1;
   return (int)(Vdp1DebugPaletteIndex(data) & 0x7FF);
}

//////////////////////////////////////////////////////////////////////////////
 
static INLINE int CheckEndcode(int dot, int endcode, int *code)
{
   if (dot == endcode)
   {
      code[0]++;
      if (code[0] == 2)
      {
         code[0] = 0;
         return 2;
      }
      return 1;
   }
 
   return 0;
}
 
//////////////////////////////////////////////////////////////////////////////
 
static INLINE int DoEndcode(int count, u32 *charAddr, u32 **textdata, int width, int xoff, int oddpixel, int pixelsize)
{
   if (count > 1)
   {
      float divisor = (float)(8 / pixelsize);
 
      if(divisor != 0)
         charAddr[0] += (int)((float)(width - xoff + oddpixel) / divisor);
      memset(textdata[0], 0, sizeof(u32) * (width - xoff));
      textdata[0] += (width - xoff);
      return 1;
   }
   else
      *textdata[0]++ = 0;
 
   return 0;
}
 
//////////////////////////////////////////////////////////////////////////////
 
u32 *Vdp1DebugTextureAtAddr(u32 addr, int *w, int *h)
{
   u16 command;
   vdp1cmd_struct cmd;
   u32 *texture;
   u32 charAddr;
   u32 dot;
   u8 SPD;
   u32 alpha;
   u32 *textdata;
   int isendcode=0;
   int code=0;
   int ret;
 
   if (addr == 0xFFFFFFFF)
      return NULL;
 
   command = T1ReadWord(Vdp1DebugMem(), addr);
 
   if (command & 0x8000)
      // Draw End
      return NULL;
 
   if (command & 0x4000)
      // Command Skipped
      return NULL;
 
   Vdp1ReadCommand(&cmd, addr, Vdp1DebugMem());
 
   switch (cmd.CMDCTRL & 0x000F)
   {
      case 0: // Normal Sprite
      case 1: // Scaled Sprite
      case 2: // Distorted Sprite
      case 3: // Distorted Sprite *
         w[0] = MAX(1, (cmd.CMDSIZE & 0x3F00) >> 5);
         h[0] = MAX(1, cmd.CMDSIZE & 0xFF);
 
         if ((texture = (u32 *)malloc(sizeof(u32) * w[0] * h[0])) == NULL)
            return NULL;
 
         if (!(cmd.CMDPMOD & 0x80))
         {
            isendcode = 1;
            code = 0;
         }
         else
            isendcode = 0;
         break;
      case 4: // Polygon
      case 5: // Polyline
      case 6: // Line
      case 7: // Polyline *
         // Do 1x1 pixel
         w[0] = 1;
         h[0] = 1;
         if ((texture = (u32 *)malloc(sizeof(u32))) == NULL)
            return NULL;
 
         texture[0] = Vdp1DebugFbDataColor(cmd.CMDCOLR);
 
         return texture;
      case 8: // User Clipping
      case 9: // System Clipping
      case 10: // Local Coordinates
      case 11: // undocumented
         return NULL;
      default: // Invalid command
         return NULL;
   }
 
   charAddr = cmd.CMDSRCA * 8;
   SPD = ((cmd.CMDPMOD & 0x40) != 0);
   alpha = 0xFF;
   textdata = texture;
 
   switch((cmd.CMDPMOD >> 3) & 0x7)
   {
      case 0:
      {
         // 4 bpp Bank mode
         /* ST-013-R3 §6.3 : en mode banque les bits bas de CMDCOLR sont
          * ignores par le materiel (4 bits de code couleur ici). Le
          * shader masquait deja correctement, pas cette fonction. */
         u32 colorBank = cmd.CMDCOLR & 0xFFF0;
         u16 i;
 
         for(i = 0;i < h[0];i++)
         {
            u16 j;
            j = 0;
               /* ST-013-R3 §6.3 p.86 : "Drawing in the horizontal direction
                * is terminated when an end code is read twice" -- le compte
                * est PAR LIGNE. 'code' etait initialise une seule fois avant
                * la boucle : une ligne contenant un nombre impair d'end codes
                * laissait le compteur a 1, et la ligne suivante se faisait
                * couper des son premier end code. */
               code = 0;
            while(j < w[0])
            {
               dot = T1ReadByte(Vdp1DebugMem(), charAddr & 0x7FFFF);
 
               // Pixel 1
               if (isendcode && (ret = CheckEndcode(dot >> 4, 0xF, &code)) > 0)
               {
                  if (DoEndcode(ret, &charAddr, &textdata, w[0], j, 0, 4))
                     break;
               }
               else
               {
                  if (((dot >> 4) == 0) && !SPD) *textdata++ = 0;
                  else *textdata++ = Vdp1DebugFbDataColor((dot >> 4) | colorBank);
               }
 
               j += 1;
 
               // Pixel 2
               if (isendcode && (ret = CheckEndcode(dot & 0xF, 0xF, &code)) > 0)
               {
                  if (DoEndcode(ret, &charAddr, &textdata, w[0], j, 1, 4))
                     break;
               }
               else
               {
                  if (((dot & 0xF) == 0) && !SPD) *textdata++ = 0;
                  else *textdata++ = Vdp1DebugFbDataColor((dot & 0xF) | colorBank);
               }
 
               j += 1;
               charAddr += 1;
            }
         }
         break;
      }
      case 1:
      {
         // 4 bpp LUT mode
         u32 temp;
         /* LUT : adresse = CMDCOLR x 8, bits 1-0 de CMDCOLR ignores,
          * comme dans le renderer (vdp1_prog_compute.h) et Mednafen
          * (ss/vdp1_sprite.c). */
         u32 colorLut = (cmd.CMDCOLR & 0xFFFC) * 8;
         /* L'entree de LUT est ecrite telle quelle dans le frame buffer et
          * interpretee par le VDP2 comme toute donnee de sprite
          * (Vdp1DebugFbDataColor) : RGB si MSB = 1 en mode mixte, sinon
          * palette avec masque DC et decalage SPCAOS. Ce dernier n'etait
          * pas ajoute ici, contrairement aux modes banque : l'apercu
          * prenait la mauvaise couleur des qu'un jeu utilise SPCAOS != 0. */
         u16 i;
 
         for(i = 0;i < h[0];i++)
         {
            u16 j;
            j = 0;
               /* ST-013-R3 §6.3 p.86 : "Drawing in the horizontal direction
                * is terminated when an end code is read twice" -- le compte
                * est PAR LIGNE. 'code' etait initialise une seule fois avant
                * la boucle : une ligne contenant un nombre impair d'end codes
                * laissait le compteur a 1, et la ligne suivante se faisait
                * couper des son premier end code. */
               code = 0;
            while(j < w[0])
            {
               dot = T1ReadByte(Vdp1DebugMem(), charAddr & 0x7FFFF);
 
               if (isendcode && (ret = CheckEndcode(dot >> 4, 0xF, &code)) > 0)
               {
                  if (DoEndcode(ret, &charAddr, &textdata, w[0], j, 0, 4))
                     break;
               }
               else
               {
                  if (((dot >> 4) == 0) && !SPD)
                     *textdata++ = 0;
                  else
                  {
                     temp = T1ReadWord(Vdp1DebugMem(), ((dot >> 4) * 2 + colorLut) & 0x7FFFF);
                     *textdata++ = Vdp1DebugFbDataColor(temp);
                  }
               }
 
               j += 1;
 
               if (isendcode && (ret = CheckEndcode(dot & 0xF, 0xF, &code)) > 0)
               {
                  if (DoEndcode(ret, &charAddr, &textdata, w[0], j, 1, 4))
                     break;
               }
               else
               {
                  if (((dot & 0xF) == 0) && !SPD)
                     *textdata++ = 0;
                  else
                  {
                     temp = T1ReadWord(Vdp1DebugMem(), ((dot & 0xF) * 2 + colorLut) & 0x7FFFF);
                     *textdata++ = Vdp1DebugFbDataColor(temp);
                  }
               }
 
               j += 1;
 
               charAddr += 1;
            }
         }
         break;
      }
      case 2:
      {
         // 8 bpp(64 color) Bank mode
         /* ST-013-R3 §6.3 : 6 bits de code couleur -> CMDCOLR & 0xFFC0. */
         u32 colorBank = cmd.CMDCOLR & 0xFFC0;
         u16 i, j;
 
         for(i = 0;i < h[0];i++)
         {
            j = 0;
               /* ST-013-R3 §6.3 p.86 : "Drawing in the horizontal direction
                * is terminated when an end code is read twice" -- le compte
                * est PAR LIGNE. 'code' etait initialise une seule fois avant
                * la boucle : une ligne contenant un nombre impair d'end codes
                * laissait le compteur a 1, et la ligne suivante se faisait
                * couper des son premier end code. */
               code = 0;
            while(j < w[0])
            {
               u32 raw = T1ReadByte(Vdp1DebugMem(), charAddr & 0x7FFFF);
               /* ST-013-R3 §6.3 p.86 : en modes couleur 2, 3 et 4 l'end
                * code est FFH sur 8 bits. Il doit etre teste sur l'octet
                * BRUT, avant tout masquage : les masques 0x3F (mode 2) et
                * 0x7F (mode 3) le detruiraient.
                * Ces trois cas ne traitaient PAS du tout les end codes,
                * contrairement aux cas 0, 1 et 5. L'apercu affichait donc
                * un caractere complet la ou le moteur de rendu tronque
                * correctement chaque ligne, ce qui rendait cet apercu
                * inutilisable pour diagnostiquer un sprite tronque. */
               if (isendcode && (ret = CheckEndcode(raw, 0xFF, &code)) > 0)
               {
                  /* count>1 : DoEndcode remplit la fin de ligne de pixels
                   * transparents, avance charAddr et renvoie 1 -> on sort.
                   * count==1 : il a deja emis le pixel transparent du
                   * premier end code, on poursuit la ligne. */
                  if (DoEndcode(ret, &charAddr, &textdata, w[0], j, 0, 8))
                     break;
                  charAddr++;
                  j += 1;
                  continue;
               }
 
               dot = raw & 0x3F;
               charAddr++;
 
               if ((dot == 0) && !SPD) *textdata++ = 0;
               else *textdata++ = Vdp1DebugFbDataColor(dot | colorBank);
 
               j += 1;
            }
         }
         break;
      }
      case 3:
      {
         // 8 bpp(128 color) Bank mode
         /* ST-013-R3 §6.3 : 7 bits de code couleur -> CMDCOLR & 0xFF80. */
         u32 colorBank = cmd.CMDCOLR & 0xFF80;
         u16 i, j;
 
         for(i = 0;i < h[0];i++)
         {
            j = 0;
               /* ST-013-R3 §6.3 p.86 : "Drawing in the horizontal direction
                * is terminated when an end code is read twice" -- le compte
                * est PAR LIGNE. 'code' etait initialise une seule fois avant
                * la boucle : une ligne contenant un nombre impair d'end codes
                * laissait le compteur a 1, et la ligne suivante se faisait
                * couper des son premier end code. */
               code = 0;
            while(j < w[0])
            {
               u32 raw = T1ReadByte(Vdp1DebugMem(), charAddr & 0x7FFFF);
               /* ST-013-R3 §6.3 p.86 : en modes couleur 2, 3 et 4 l'end
                * code est FFH sur 8 bits. Il doit etre teste sur l'octet
                * BRUT, avant tout masquage : les masques 0x3F (mode 2) et
                * 0x7F (mode 3) le detruiraient.
                * Ces trois cas ne traitaient PAS du tout les end codes,
                * contrairement aux cas 0, 1 et 5. L'apercu affichait donc
                * un caractere complet la ou le moteur de rendu tronque
                * correctement chaque ligne, ce qui rendait cet apercu
                * inutilisable pour diagnostiquer un sprite tronque. */
               if (isendcode && (ret = CheckEndcode(raw, 0xFF, &code)) > 0)
               {
                  /* count>1 : DoEndcode remplit la fin de ligne de pixels
                   * transparents, avance charAddr et renvoie 1 -> on sort.
                   * count==1 : il a deja emis le pixel transparent du
                   * premier end code, on poursuit la ligne. */
                  if (DoEndcode(ret, &charAddr, &textdata, w[0], j, 0, 8))
                     break;
                  charAddr++;
                  j += 1;
                  continue;
               }
 
               dot = raw & 0x7F;
               charAddr++;
 
               if ((dot == 0) && !SPD) *textdata++ = 0;
               else *textdata++ = Vdp1DebugFbDataColor(dot | colorBank);
 
               j += 1;
            }
         }
         break;
      }
      case 4:
      {
         // 8 bpp(256 color) Bank mode
         /* ST-013-R3 §6.3 : 8 bits de code couleur -> CMDCOLR & 0xFF00. */
         u32 colorBank = cmd.CMDCOLR & 0xFF00;
         u16 i, j;
 
         for(i = 0;i < h[0];i++)
         {
            j = 0;
               /* ST-013-R3 §6.3 p.86 : "Drawing in the horizontal direction
                * is terminated when an end code is read twice" -- le compte
                * est PAR LIGNE. 'code' etait initialise une seule fois avant
                * la boucle : une ligne contenant un nombre impair d'end codes
                * laissait le compteur a 1, et la ligne suivante se faisait
                * couper des son premier end code. */
               code = 0;
            while(j < w[0])
            {
               u32 raw = T1ReadByte(Vdp1DebugMem(), charAddr & 0x7FFFF);
               /* ST-013-R3 §6.3 p.86 : en modes couleur 2, 3 et 4 l'end
                * code est FFH sur 8 bits. Il doit etre teste sur l'octet
                * BRUT, avant tout masquage : les masques 0x3F (mode 2) et
                * 0x7F (mode 3) le detruiraient.
                * Ces trois cas ne traitaient PAS du tout les end codes,
                * contrairement aux cas 0, 1 et 5. L'apercu affichait donc
                * un caractere complet la ou le moteur de rendu tronque
                * correctement chaque ligne, ce qui rendait cet apercu
                * inutilisable pour diagnostiquer un sprite tronque. */
               if (isendcode && (ret = CheckEndcode(raw, 0xFF, &code)) > 0)
               {
                  /* count>1 : DoEndcode remplit la fin de ligne de pixels
                   * transparents, avance charAddr et renvoie 1 -> on sort.
                   * count==1 : il a deja emis le pixel transparent du
                   * premier end code, on poursuit la ligne. */
                  if (DoEndcode(ret, &charAddr, &textdata, w[0], j, 0, 8))
                     break;
                  charAddr++;
                  j += 1;
                  continue;
               }
 
               dot = raw;
               charAddr++;
 
               if ((dot == 0) && !SPD) *textdata++ = 0;
               else *textdata++ = Vdp1DebugFbDataColor(dot | colorBank);
 
               j += 1;
            }
         }
         break;
      }
      case 5:
      {
         // 16 bpp Bank mode
         u16 i, j;
 
         for(i = 0;i < h[0];i++)
         {
            /* ST-013-R3 §6.3 p.86 : le compte d'end codes est PAR LIGNE. */
            code = 0;
            for(j = 0;j < w[0];j++)
            {
               dot = T1ReadWord(Vdp1DebugMem(), charAddr & 0x7FFFF);
 
               if (isendcode && (ret = CheckEndcode(dot, 0x7FFF, &code)) > 0)
               {
                  if (DoEndcode(ret, &charAddr, &textdata, w[0], j, 0, 16))
                     break;
               }
               else
               {
                  /* Transparence : MSB = 0 avec SPD = 0, meme regle que le
                   * moteur (vdp1_prog_compute*.h). Avec SPD = 1, un mot a
                   * MSB = 0 est ecrit tel quel et le VDP2 le lit comme une
                   * donnee PALETTE : il etait affiche ici comme une couleur
                   * RGB. Vdp1DebugFbDataColor applique la lecture du VDP2
                   * (RGB seulement si MSB = 1 et SPCLMD = 1). */
                  if (!(dot & 0x8000) && !SPD) *textdata++ = 0;
                  else *textdata++ = Vdp1DebugFbDataColor(dot);
               }
 
               charAddr += 2;
            }
         }
         break;
      }
      default:
         break;
   }
 
   return texture;
}
 
u8 *Vdp1DebugRawTextureAtAddr(u32 cmdAddress, int *width, int *height, int *numBytes)
{
   u16 cmdRaw;
   vdp1cmd_struct cmd;
   u8 *texture = NULL;
 
   // Initial number of bytes written to texture
   *numBytes = 0;
 
   if (cmdAddress == 0xFFFFFFFF)
      return NULL;
 
   cmdRaw = T1ReadWord(Vdp1DebugMem(), cmdAddress);
 
   if (cmdRaw & 0x8000)
      // Draw End
      return NULL;
 
   if (cmdRaw & 0x4000)
      // Command Skipped
      return NULL;
 
   Vdp1ReadCommand(&cmd, cmdAddress, Vdp1DebugMem());
 
   const int spriteCmdType = ((cmd.CMDPMOD >> 3) & 0x7);
   switch (cmd.CMDCTRL & 0x000F)
   {
      case 0: // Normal Sprite
      case 1: // Scaled Sprite
      case 2: // Distorted Sprite
      case 3: // Distorted Sprite *
         width[0] = (cmd.CMDSIZE & 0x3F00) >> 5;
         height[0] = cmd.CMDSIZE & 0xFF;
 
         switch (spriteCmdType) {
            // 0: 4 bpp Bank mode
            // 1: 4 bpp LUT mode
            case 0:
            case 1:
               numBytes[0] = 0.5 * width[0] * height[0];
               texture = (u8*) malloc(numBytes[0]);
               break;
            // 2: 8 bpp(64 color) Bank mode
            // 3: 8 bpp(128 color) Bank mode
            // 4: 8 bpp(256 color) Bank mode
            case 2:
            case 3:
            case 4:
               numBytes[0] = width[0] * height[0];
               texture = (u8*) malloc(numBytes[0]);
               break;
            // 5: 16 bpp Bank mode
            case 5:
               numBytes[0] = 2 * width[0] * height[0];
               texture = (u8*) malloc(numBytes[0]);
               break;
            default:
               texture = NULL;
               break;
         }
 
         if (texture == NULL)
            return NULL;
 
         break;
      case 4: // Polygon
      case 5: // Polyline
      case 6: // Line
      case 7: // Polyline *
         // Do 1x1 pixel
         width[0] = 1;
         height[0] = 1;
         texture = (u8*) malloc(sizeof(u16));
 
         if (texture == NULL)
            return NULL;
 
         *numBytes = 2;
         memcpy(texture, &cmd.CMDCOLR, sizeof(u16));
         return texture;
      case 8:  // User Clipping
      case 9:  // System Clipping
      case 10: // Local Coordinates
      case 11: // Undocumented
         return NULL;
      default: // Invalid command
         return NULL;
   }
 
   // Read texture data directly from VRAM.
   for (u32 i = 0; i < *numBytes; ++i)
   {
     texture[ i ] = T1ReadByte(Vdp1DebugMem(), ((cmd.CMDSRCA * 8) + i) & 0x7FFFF);
   }
 
   return texture;
}
 
//////////////////////////////////////////////////////////////////////////////
 
//////////////////////////////////////////////////////////////////////////////
// Aide au debogage : couleur d'un texel et apercu de la trame complete.
//
// Ni l'une ni l'autre n'interviennent dans le rendu : elles ne lisent que
// Vdp1DebugMem(), la Color RAM et les registres VDP2, pour l'interface.
//////////////////////////////////////////////////////////////////////////////
 
/* Decrit le chemin complet d'un code couleur de texel jusqu'a sa couleur
 * finale : banque/LUT, decalage CRAOFB, index et contenu de la Color RAM.
 * Meme arithmetique que Vdp1DebugTextureAtAddr (et donc que l'apercu).
 * Renvoie le nombre de caracteres ecrits (0 si rien a dire). */
int Vdp1DebugTexelColorText(u32 cmdAddr, u32 code, char *out, int outSize)
{
   u8 *ram = Vdp1DebugMem();
   vdp1cmd_struct cmd;
   u32 data = 0;
   int n = 0;
 
   if ((out == NULL) || (outSize <= 0)) return 0;
   out[0] = '\0';
   if ((ram == NULL) || (cmdAddr == 0xFFFFFFFF)) return 0;
 
   Vdp1ReadCommand(&cmd, cmdAddr, ram);
   if ((cmd.CMDCTRL & 0xC000) || ((cmd.CMDCTRL & 0xF) > 3)) return 0;
 
   /* Donnee 16 bits que le VDP1 ecrit dans le frame buffer pour ce texel. */
   switch ((cmd.CMDPMOD >> 3) & 0x7)
   {
      case 0: data = (code | (cmd.CMDCOLR & 0xFFF0)); break;
      case 2: data = (code | (cmd.CMDCOLR & 0xFFC0)); break;
      case 3: data = (code | (cmd.CMDCOLR & 0xFF80)); break;
      case 4: data = (code | (cmd.CMDCOLR & 0xFF00)); break;
      case 1:
      {
         u32 lutAddr = ((cmd.CMDCOLR & 0xFFFC) * 8 + code * 2) & 0x7FFFF;
         data = T1ReadWord(ram, lutAddr);
         n += snprintf(out + n, outSize - n, "LUT[0x%05X] -> ", lutAddr);
         break;
      }
      case 5: data = code; break;
      default:
         return 0;
   }
   if (n >= outSize) return (int)strlen(out);
 
   if (Vdp1DebugIsRgbData(data)) {
      snprintf(out + n, outSize - n, "FB 0x%04X = RGB R=%d G=%d B=%d",
               data, data & 0x1F, (data >> 5) & 0x1F, (data >> 10) & 0x1F);
      return (int)strlen(out);
   }
 
   {
      const int type = Vdp2Regs->SPCTL & 0xF;
      const u32 mask = Vdp1DebugSpriteDCMask[type];
      const u32 dc = data & mask;
      const u32 full = Vdp1DebugPaletteIndex(data);
      n += snprintf(out + n, outSize - n, "FB 0x%04X  type %X: DC 0x%03X + offset 0x%03X = CRAM idx 0x%03X",
                    data, type, dc, full - dc, full & 0x7FF);
      if (n < outSize) {
         if (Vdp2Internal.ColorMode == 2) {
            u32 a = (full << 2) & 0xFFF;
            n += snprintf(out + n, outSize - n, "  [0x%03X] = 0x%04X%04X",
                          a, T2ReadWord(Vdp2ColorRam, a), T2ReadWord(Vdp2ColorRam, a + 2));
         } else {
            u32 a = (full << 1) & 0xFFF;
            u16 v = T2ReadWord(Vdp2ColorRam, a);
            n += snprintf(out + n, outSize - n, "  [0x%03X] = 0x%04X  R=%d G=%d B=%d",
                          a, v, v & 0x1F, (v >> 5) & 0x1F, (v >> 10) & 0x1F);
         }
      }
      /* TB #25 : un DC egal a "tous les bits a 1 sauf le bit 0" est une
       * donnee d'ombre normale pour le VDP2, pas une couleur. */
      if ((n < outSize) && (dc == (mask & ~1u)))
         snprintf(out + n, outSize - n, "  (normal shadow data, TB#25)");
   }
   return (int)strlen(out);
}
 
static void Vdp1DebugPlot(u32 *fb, int W, int H, int x, int y, u32 c)
{
   if ((x >= 0) && (y >= 0) && (x < W) && (y < H))
      fb[y * W + x] = c;
}
 
static void Vdp1DebugDrawLine(u32 *fb, int W, int H, int x0, int y0, int x1, int y1, u32 c)
{
   int dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
   int dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
   int err = dx + dy;
   int guard = 0;
   for (;;) {
      Vdp1DebugPlot(fb, W, H, x0, y0, c);
      if (((x0 == x1) && (y0 == y1)) || (++guard > 8192)) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
   }
}
 
/* Plaque la texture tw x th sur le quadrilatere A B C D (A = coin haut
 * gauche de la texture, B haut droit, C bas droit, D bas gauche) par
 * echantillonnage direct : chaque point (u, v) est interpole bilineairement
 * entre les coins puis trace. Ce n'est PAS le rasteriseur du VDP1 (pas de
 * pas de Bresenham, pas de pixels "trous" ni de recouvrement identiques) :
 * c'est un apercu, suffisant pour voir ou chaque commande se place. */
static void Vdp1DebugDrawQuad(u32 *fb, int W, int H, const int *qx, const int *qy,
                              const u32 *tex, int tw, int th, int flipH, int flipV)
{
   int lenU = MAX(MAX(abs(qx[1] - qx[0]), abs(qy[1] - qy[0])),
                  MAX(abs(qx[2] - qx[3]), abs(qy[2] - qy[3])));
   int lenV = MAX(MAX(abs(qx[3] - qx[0]), abs(qy[3] - qy[0])),
                  MAX(abs(qx[2] - qx[1]), abs(qy[2] - qy[1])));
   int nu = MIN(2048, MAX(tw, lenU) * 2 + 1);
   int nv = MIN(2048, MAX(th, lenV) * 2 + 1);
   int iu, iv;
 
   for (iv = 0; iv <= nv; iv++) {
      float v = (float)iv / (float)nv;
      int ty = MIN(th - 1, (int)(v * th));
      if (flipV) ty = th - 1 - ty;
      for (iu = 0; iu <= nu; iu++) {
         float u = (float)iu / (float)nu;
         int tx = MIN(tw - 1, (int)(u * tw));
         u32 c;
         float topX, topY, botX, botY;
         if (flipH) tx = tw - 1 - tx;
         c = tex[ty * tw + tx];
         if (c == 0) continue; // transparent
         topX = qx[0] + (qx[1] - qx[0]) * u;  topY = qy[0] + (qy[1] - qy[0]) * u;
         botX = qx[3] + (qx[2] - qx[3]) * u;  botY = qy[3] + (qy[2] - qy[3]) * u;
         Vdp1DebugPlot(fb, W, H, (int)floorf(topX + (botX - topX) * v + 0.5f),
                                 (int)floorf(topY + (botY - topY) * v + 0.5f), c);
      }
   }
}
 
/* Assemble toutes les commandes de addrs[] dans une image ARGB (meme format
 * que Vdp1DebugTextureAtAddr, 0 = transparent) de la taille du clipping
 * systeme (320x224 si aucune commande System Clipping ne le fixe).
 *
 * boxes[4*i .. 4*i+3] recoit x0, y0, x1, y1 (inclus, coordonnees ecran, avant
 * clipping) de la commande i, ou x1 < x0 si elle ne trace rien.
 *
 * Pris en compte : coordonnees locales, inversions H/V, zoom point des
 * scaled sprites (meme decomposition que Vdp1NormalSpriteDraw et voisins),
 * quadrilateres des distorted sprites et polygones, polylignes et lignes,
 * transparence et end codes (via Vdp1DebugTextureAtAddr).
 * Ignores : calcul de couleur (demi-transparence, ombre, Gouraud), mesh,
 * clipping utilisateur, HSS. */
u32 *Vdp1DebugRenderFrame(const u32 *addrs, int count, int *outW, int *outH, int *boxes)
{
   u8 *ram = Vdp1DebugMem();
   int W = 320, H = 224;
   s32 localX = 0, localY = 0;
   u32 *fb;
   int i;
 
   if ((ram == NULL) || (addrs == NULL) || (count <= 0)) return NULL;
 
   /* Taille de la toile : derniere commande System Clipping de la liste. */
   for (i = 0; i < count; i++) {
      u16 ctrl = T1ReadWord(ram, addrs[i]);
      if (!(ctrl & 0xC000) && ((ctrl & 0xF) == 9)) {
         W = (T1ReadWord(ram, addrs[i] + 0x14) & 0x3FF) + 1;
         H = (T1ReadWord(ram, addrs[i] + 0x16) & 0x3FF) + 1;
      }
   }
   W = MAX(8, MIN(1024, W));
   H = MAX(8, MIN(512, H));
 
   fb = (u32 *)calloc((size_t)W * H, sizeof(u32));
   if (fb == NULL) return NULL;
 
   for (i = 0; i < count; i++) {
      vdp1cmd_struct cmd;
      int qx[4], qy[4];
      int type, k;
      u32 *tex = NULL;
      int tw = 0, th = 0;
 
      if (boxes) { boxes[4*i] = 0; boxes[4*i+1] = 0; boxes[4*i+2] = -1; boxes[4*i+3] = -1; }
 
      Vdp1ReadCommand(&cmd, addrs[i], ram);
      if (cmd.CMDCTRL & 0x8000) break;       // Draw End
      if (cmd.CMDCTRL & 0x4000) continue;    // commande sautee
      type = cmd.CMDCTRL & 0xF;
 
      CONVERTCMD(&cmd.CMDXA); CONVERTCMD(&cmd.CMDYA);
      CONVERTCMD(&cmd.CMDXB); CONVERTCMD(&cmd.CMDYB);
      CONVERTCMD(&cmd.CMDXC); CONVERTCMD(&cmd.CMDYC);
      CONVERTCMD(&cmd.CMDXD); CONVERTCMD(&cmd.CMDYD);
 
      if (type == 10) {                       // Local Coordinates
         localX = cmd.CMDXA;
         localY = cmd.CMDYA;
         continue;
      }
      if (type > 7) continue;                 // clipping, invalide
 
      tex = Vdp1DebugTextureAtAddr(addrs[i], &tw, &th);
      if (tex == NULL) continue;
 
      switch (type) {
         case 0: // Normal Sprite
            qx[0] = cmd.CMDXA;          qy[0] = cmd.CMDYA;
            qx[1] = cmd.CMDXA + tw - 1; qy[1] = cmd.CMDYA;
            qx[2] = cmd.CMDXA + tw - 1; qy[2] = cmd.CMDYA + th - 1;
            qx[3] = cmd.CMDXA;          qy[3] = cmd.CMDYA + th - 1;
            break;
         case 1: // Scaled Sprite
         {
            int zp = (cmd.CMDCTRL >> 8) & 0xF;
            int x0, x1, y0, y1;
            if (zp == 0) {
               x0 = cmd.CMDXA; y0 = cmd.CMDYA; x1 = cmd.CMDXC; y1 = cmd.CMDYC;
            } else {
               int rw = cmd.CMDXB, rh = cmd.CMDYB;
               switch (zp & 3) {
                  case 2:  x0 = cmd.CMDXA - rw / 2; x1 = cmd.CMDXA + (rw + 1) / 2; break;
                  case 3:  x0 = cmd.CMDXA - rw;     x1 = cmd.CMDXA;                break;
                  default: x0 = cmd.CMDXA;          x1 = cmd.CMDXA + rw;           break;
               }
               switch ((zp >> 2) & 3) {
                  case 2:  y0 = cmd.CMDYA - rh / 2; y1 = cmd.CMDYA + (rh + 1) / 2; break;
                  case 3:  y0 = cmd.CMDYA - rh;     y1 = cmd.CMDYA;                break;
                  default: y0 = cmd.CMDYA;          y1 = cmd.CMDYA + rh;           break;
               }
            }
            qx[0] = x0; qy[0] = y0; qx[1] = x1; qy[1] = y0;
            qx[2] = x1; qy[2] = y1; qx[3] = x0; qy[3] = y1;
            break;
         }
         default: // Distorted Sprite, Polygon, Polyline, Line
            qx[0] = cmd.CMDXA; qy[0] = cmd.CMDYA;
            qx[1] = cmd.CMDXB; qy[1] = cmd.CMDYB;
            qx[2] = cmd.CMDXC; qy[2] = cmd.CMDYC;
            qx[3] = cmd.CMDXD; qy[3] = cmd.CMDYD;
            break;
      }
 
      for (k = 0; k < 4; k++) { qx[k] += localX; qy[k] += localY; }
 
      if (boxes) {
         int nk = (type == 6) ? 2 : 4;
         boxes[4*i] = boxes[4*i+2] = qx[0];
         boxes[4*i+1] = boxes[4*i+3] = qy[0];
         for (k = 1; k < nk; k++) {
            boxes[4*i]   = MIN(boxes[4*i],   qx[k]);
            boxes[4*i+1] = MIN(boxes[4*i+1], qy[k]);
            boxes[4*i+2] = MAX(boxes[4*i+2], qx[k]);
            boxes[4*i+3] = MAX(boxes[4*i+3], qy[k]);
         }
      }
 
      if (type == 0) {
         /* Copie exacte texel a texel. */
         int flipH = (cmd.CMDCTRL >> 4) & 1, flipV = (cmd.CMDCTRL >> 5) & 1;
         int x, y;
         for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++) {
               u32 c = tex[(flipV ? th - 1 - y : y) * tw + (flipH ? tw - 1 - x : x)];
               if (c != 0) Vdp1DebugPlot(fb, W, H, qx[0] + x, qy[0] + y, c);
            }
      } else if (type <= 4) {
         /* Scaled/Distorted : texture ; Polygon : texture 1x1 de CMDCOLR. */
         int flip = (type <= 3) ? cmd.CMDCTRL : 0;
         Vdp1DebugDrawQuad(fb, W, H, qx, qy, tex, tw, th, (flip >> 4) & 1, (flip >> 5) & 1);
      } else if (type == 6) {                  // Line
         Vdp1DebugDrawLine(fb, W, H, qx[0], qy[0], qx[1], qy[1], tex[0]);
      } else {                                 // Polyline
         for (k = 0; k < 4; k++)
            Vdp1DebugDrawLine(fb, W, H, qx[k], qy[k], qx[(k + 1) & 3], qy[(k + 1) & 3], tex[0]);
      }
 
      free(tex);
   }
 
   *outW = W;
   *outH = H;
   return fb;
}
 
void ToggleVDP1(void)
{
   Vdp1External.disptoggle ^= 1;
}
//////////////////////////////////////////////////////////////////////////////
 
/* Number of display lines the V-blank erase/write overruns by.
 *
 * Sources: VDP1 User's Manual ST-013-R3, EWDR p.46, EWLR/EWRR pp.47-48,
 * erase/write rules pp.48-49, Tables 4.4 and 4.5 pp.49-50.
 *
 * Register layout (p.47):
 *   EWLR bit 15 = 0, bits 14-9 = X1, bits 8-0 = Y1
 *   EWRR bits 15-9 = X3, bits 8-0 = Y3
 * The X coordinate is set in 8-pixel units at 16 bpp and 16-pixel units at
 * 8 bpp, and the lower-right X is "8 or 16 times the register setting and
 * from which 1 is subtracted" (p.48).
 *
 * Required pixels, 16 bpp (p.49): (X3 - X1) x (Y3 - Y1 + 1) x 8, where X3
 * and X1 are register values. Converting to pixel coordinates first, as we
 * do here, folds the x8 in: x3px - x1px + 1 = (X3reg x 8 - 1) - X1reg x 8
 * + 1 = (X3reg - X1reg) x 8. The two forms agree.
 *
 * Capacity (p.49): {(pixels in 1 raster) - 200} per non-display raster.
 */
static int getVdp1ErasePixelLine() {
    int is8bpp = (Vdp1Regs->TVMR & 0x1);
    int xunit  = is8bpp ? 16 : 8;
 
    /* Parametres verrouilles au dernier swap (cf. Vdp1LatchEraseParameters). */
    int x1 = ((Vdp1EraseLatchEWLR >> 9) & 0x3F) * xunit;
    int y1 =  (Vdp1EraseLatchEWLR) & 0x1FF;
    int x3 = (((Vdp1EraseLatchEWRR >> 9) & 0x7F) * xunit) - 1;
    int y3 =  (Vdp1EraseLatchEWRR) & 0x1FF;
 
    /* p.48: "Because the register setting for the Y coordinate is doubled
     * during double interlace, the actual coordinate value should be set to
     * one half. For example, when the setting is 223, the coordinate
     * becomes 447." Double interlace is FBCR bit 3, DIE = 1 (p.53). The
     * erased area -- and therefore the time it takes -- doubles vertically.
     * This scaling was missing entirely, so every double-interlace title
     * had its erase cost halved. */
    if ((Vdp1Regs->FBCR & 0x8) != 0) {
        y1 *= 2;
        y3 = y3 * 2 + 1;
    }
 
    /* p.49: "If the setting is X1 >= X3 or Y1 > Y3, then erase/write is
     * performed for 1 dot in the normal or high-resolution mode and for
     * 8 dots in the case of rotation or HDTV. In these cases, erase/write
     * is performed under the assumption that the area (X1, Y1) is set to
     * (X3 = X1 + 1, Y3 = Y1)."
     *
     * So a degenerate rectangle is not "no erase": it is a minimal one.
     * The old code returned 0 on this path, and also rejected y3 == 0
     * outright -- but Y1 = Y3 = 0 is a perfectly legal single-line erase,
     * not a degenerate setting. TVMR bits 2-1 select the TV mode: 10b is
     * rotation 16 and 11b rotation 8, and TVM bit 3 (HDTV) likewise draws
     * 8 dots (p.36). */
    int area_w, area_h;
    if ((x1 >= x3) || (y1 > y3)) {
        int isRotOrHdtv = ((Vdp1Regs->TVMR & 0x6) != 0) ||
                          ((Vdp1Regs->TVMR & 0x8) != 0);
        area_w = isRotOrHdtv ? 8 : 1;
        area_h = 1;
    } else {
        area_w = x3 - x1 + 1;
        area_h = y3 - y1 + 1;
    }
 
    /* EWDR p.46: "Erase/write is performed 2 pixels at a time when the frame
     * buffer depth is 8 bits/pixel." The frame buffer moves one 16-bit word
     * per cycle (p.20), so 8 bpp clears 2 pixels per cycle and 16 bpp one.
     * Cross-check against Table 4.5: NTSC 320x224 16 bpp gives 58812, which
     * is (1708-200) x (263-224) = 1508 x 39 -- one pixel per cycle. */
    int area_pixels     = area_w * area_h;
    int pixels_per_cycle = is8bpp ? 2 : 1;
    int total_cycles     = (area_pixels + pixels_per_cycle - 1)
                           / pixels_per_cycle;
 
    /* p.49: usable pixels per raster = (pixels in 1 raster) - 200.
     *
     * Read rasterValue directly rather than going through
     * getVdp1CyclesPerLine(), which returns 0 while Vdp1External.blocked is
     * set for DMA concurrency. That made usable_per_line come out at -200
     * and the function bail out with 0, reporting an instantaneous erase
     * precisely when the bus was most contended. The erase capacity is a
     * property of the video mode, not of who currently owns the bus. */
    int usable_per_line = rasterValue - 200;
    if (usable_per_line <= 0) return 0;
 
    return (total_cycles + usable_per_line - 1) / usable_per_line;
}
 
static void Vdp1EraseWrite(int id){
  lastHash = -1;
  _Ygl->shallVdp1Erase[id] = 1;
}
 
//////////////////////////////////////////////////////////////////////////////
 
void Vdp1HBlankIN(void)
{
  if (yabsys.LineCount == (yabsys.VBlankLineCount + 1)) {
    //First HBlankIn after VBlankIn // Evaluate erase
    updateFBCRVBE();
    int eraseId = 0;
    if (_Ygl != NULL) eraseId = _Ygl->readframe;
    if (Vdp1External.useVBlankErase != 0) {
      //Vblank time - VBE on, erase read frame
      FRAMELOG("##### VBlank on %d (%d %d)\n", eraseId, yabsys.LineCount, yabsys.DecilineCount);
      Vdp1EraseWrite(eraseId);
 
      /* VDP1 Manual p.49: the erase/write finishes only if the pixels it
       * needs fit within the V-blank capacity of Tables 4.4 and 4.5. Past
       * that the hardware stops mid-erase and the remainder of the frame
       * buffer keeps the previous field's contents -- the classic residue
       * left by a title that erases more than blanking allows.
       *
       * The result of getVdp1ErasePixelLine() used to be assigned to a
       * local, clamped to the length of the blanking period, and then
       * discarded without ever being read. Clamping first is also what hid
       * the interesting case: after the clamp the value can never exceed
       * what blanking offers, so the overrun it was meant to expose was
       * arithmetically unreachable. Compare before clamping and record the
       * shortfall instead. */
      int eraseLines     = getVdp1ErasePixelLine();
      int availableLines = (yabsys.MaxLineCount - 1)
                         - (yabsys.VBlankLineCount + 1);
 
      Vdp1External.eraseOverrun      = (eraseLines > availableLines);
      Vdp1External.eraseOverrunLines = Vdp1External.eraseOverrun
                                     ? (eraseLines - availableLines) : 0;
      if (Vdp1External.eraseOverrun) {
        FRAMELOG("V-blank erase overruns by %d lines (needs %d, has %d)\n",
                 Vdp1External.eraseOverrunLines, eraseLines, availableLines);
      }
 
      updateFBCRChange();
    }
  }
  if (yabsys.LineCount == (yabsys.MaxLineCount - 1)) {
    /* Statistiques : le champ se termine ici, avant la bascule eventuelle
     * (Vdp1SwitchFrame cloture l'image, qui compte donc ce champ). */
    vdp1StatsEndField();
 
    FRAMELOG("### Update FBCR for next field %d %d\n", yabsys.LineCount, yabsys.DecilineCount);
    updateFBCRErase();
    updateFBCRChange();
    int swap_frame_buffer = (Vdp1External.manualchange == 1);
    swap_frame_buffer |= (Vdp1External.onecyclechange == 1);
    // Frame Change
    if (swap_frame_buffer == 1)
    {
      addVdp1Framecount();
      FRAMELOG("####Swap Line %d (v=%d,m=%d)\n", yabsys.LineCount , yabsys.VBlankLineCount, yabsys.MaxLineCount);
      lastHash = -1;
      Vdp1SwitchFrame();
    }
    Vdp1External.manualchange = 0;
 
    int eraseId = 0;
    if (_Ygl != NULL) eraseId = _Ygl->readframe;
    //Vblank time - VBE of, erase read frame if erase mode or oncyclemode
    if ((Vdp1External.manualerase == 1) || (Vdp1External.onecycleerase == 1))
    {
      FRAMELOG("########frame %d was erased in this field\n", eraseId);
      Vdp1EraseWrite(eraseId);
    }
    Vdp1External.manualerase = 0;
  }
  int cyclesPerLine  = getVdp1CyclesPerLine();
  vdp1StatsLine(rasterValue, Vdp1External.blocked != 0);
  if (vdp1_clock > 0) vdp1_clock = 0;
  vdp1_clock += cyclesPerLine;
  Vdp1TryDraw();
}
//////////////////////////////////////////////////////////////////////////////
 
void Vdp1StartVisibleLine(void)
{
}
 
//////////////////////////////////////////////////////////////////////////////
void Vdp1VBlankIN(void)
{
}
void Vdp1VBlankOUT(void) {
  //at field change, frame is changing in case of VblankErase - one cyclemode or manualchange
  // if (yabsys.LineCount == (yabsys.MaxLineCount - 1)) {
    //First blankin after VBlankOut
    //Evaluate FBCR
  // }
}
 
void Vdp1VBlankIN_It(void)
{
  FRAMELOG("VBLANKIn line %d (%d)\n", yabsys.LineCount, yabsys.DecilineCount);
  checkFBSync();
}
 
void Vdp1SwitchFrame(void)
{
  /* Statistiques : liste encore en cours au moment de la bascule ? (le
   * trace n'a pas fini dans la periode de l'image). Releve AVANT un
   * eventuel abortVdp1() du mode PTMR = 2 ci-dessous. */
  const int statRunningAtSwap =
      ((Vdp1External.status & VDP1_STATUS_MASK) == VDP1_STATUS_RUNNING);
  FRAMELOG("Change frames before draw %d, read %d (%d)\n", _Ygl->drawframe, _Ygl->readframe, yabsys.LineCount);
  checkFBSync();
  FRAMELOG("Switch Frame change VDP1 %d(%d)\n", yabsys.LineCount, yabsys.DecilineCount);
  VIDCore->Vdp1FrameChange();
  /* Verrouillage APRES Vdp1FrameChange() : l'effacement differe execute
   * dans ce dernier appartient au champ precedent et doit utiliser les
   * parametres verrouilles au swap precedent (Mednafen / Ymir). */
  Vdp1LatchEraseParameters();
  FRAMELOG("Change frames now draw %d, read %d (%d)\n", _Ygl->drawframe, _Ygl->readframe, yabsys.LineCount);
  Vdp1External.current_frame = !Vdp1External.current_frame;
  // Spec VDP1 §6.2 : LOPR = adresse dernière commande traitée
  Vdp1Regs->LOPR = Vdp1Regs->lCOPR;
  Vdp1Regs->COPR = 0;
  Vdp1Regs->lCOPR = 0;
 
  if (Vdp1Regs->PTMR == 0x2) {
    FRAMELOG("[VDP1] PTMR == 0x2 start drawing immidiatly %d %d EDSR %x PTMR %x\n", yabsys.LineCount, yabsys.DecilineCount, Vdp1Regs->EDSR,  Vdp1Regs->PTMR);
    int cylesPerLine = getVdp1CyclesPerLine();
    checkFBSync();
    abortVdp1();
    vdp1_clock = (vdp1_clock + cylesPerLine)%(cylesPerLine+1);
    RequestVdp1ToDraw();
    // Vdp1TryDraw();
  } else {
    Vdp1Regs->EDSR >>= 1;
  }
  /* Cloture APRES l'abortVdp1() du mode PTMR = 2 : la liste coupee par la
   * bascule est comptee dans l'image qui se termine, pas dans la suivante.
   * Aucun cycle n'est consomme entre les deux. */
  vdp1StatsEndFrame(statRunningAtSwap);
}
 
//////////////////////////////////////////////////////////////////////////////
// Variantes historiques a base de rang dans la liste de commandes.
//
// Elles resolvent l'adresse une fois puis delèguent aux fonctions ci-dessus.
// A n'utiliser que lorsque l'appelant n'a pas deja l'adresse sous la main :
// chaque appel reparcourt la table depuis l'adresse 0, et rien ne garantit
// que la table n'a pas bouge depuis le moment ou le rang a ete calcule.
//////////////////////////////////////////////////////////////////////////////
 
void Vdp1DebugCommand(u32 number, char *outstring)
{
   Vdp1DebugCommandAtAddr(Vdp1DebugGetCommandNumberAddr(number), outstring);
}
 
u32 *Vdp1DebugTexture(u32 number, int *w, int *h)
{
   return Vdp1DebugTextureAtAddr(Vdp1DebugGetCommandNumberAddr(number), w, h);
}
 
u8 *Vdp1DebugRawTexture(u32 cmdNumber, int *width, int *height, int *numBytes)
{
   return Vdp1DebugRawTextureAtAddr(Vdp1DebugGetCommandNumberAddr(cmdNumber),
                                    width, height, numBytes);
}
