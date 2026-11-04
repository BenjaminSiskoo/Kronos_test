/*  Copyright 2006-2007 Theo Berkau

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

/*! \file bios.c
    \brief Emulated bios functions required for running games and saving backup ram.
*/

#include "memory.h"
#include "cs0.h"
#include "cs2.h"
#include "debug.h"
#include "sh2core.h"
#include "bios.h"
#include "smpc.h"
#include "yabause.h"
#include "error.h"
#include "yui.h"
#include <string.h>

/* Table de priorite des interruptions SCU (SYS_CHGSCUPRI, ST-162 5.1
   "SCU Interrupt Routine Priority Change"), au format du boot ROM : un mot
   long par facteur 40H-5FH, mot haut = SR pendant le gestionnaire, mot bas =
   masque SCU ajoute (etendu sur 32 bits par son bit 15). Le boot ROM la tient
   en Work RAM-H a 6000A80H (= 6000980H + vecteur x 4) et son repartiteur la
   relit a chaque interruption ; valeurs initiales relevees dans un vidage de
   la Work RAM-H avec le BIOS reel. */
#define BIOS_SCU_PRI_TABLE   0x06000980   /* + vecteur x 4 (vecteurs 40H-5FH) */
#define BIOS_SCU_CHGSCUPRI   0x06000620   /* SYS_CHGSCUPRI en SH-2, voir BiosInit() */
static const u32 scupri_default[0x20] = {
0x00F0FFFF, 0x00E0FFFE, 0x00D0FFFC, 0x00C0FFF8,
0x00B0FFF0, 0x00A0FFE0, 0x0090FFC0, 0x0080FF80,
0x0080FF80, 0x0070FE00, 0x0070FE00, 0x0070FE00,
0x0070FE00, 0x0070FE00, 0x0070FE00, 0x0070FE00,
0x0070FE00, 0x0070FE00, 0x0070FE00, 0x0070FE00,
0x0070FE00, 0x0070FE00, 0x0070FE00, 0x0070FE00,
0x0070FE00, 0x0070FE00, 0x0070FE00, 0x0070FE00,
0x0070FE00, 0x0070FE00, 0x0070FE00, 0x0070FE00
};

/* SYS_CHGSYSCK en cours sur le maitre : contexte de l'appelant (pile, retour,
   SR et registres preserves R8-R14), rendu par BiosNmi() quand le NMI de fin
   de commande arrive. */
static int hle_ckchg_pending = 0;
static u32 hle_ckchg_r15, hle_ckchg_pr, hle_ckchg_sr;
static u32 hle_ckchg_r8_14[7];

u32 interruptlist[2][0x80];

extern u32 backup_file_addr;
extern u32 backup_file_size;

void FASTCALL BiosBUPRead(SH2_struct * sh);
//////////////////////////////////////////////////////////////////////////////


/* Table des vecteurs de la ROM du BIOS (00000000H-000000FFH) du BIOS emule.

   Le BIOS emule n'a pas de ROM : BiosRom est rempli de 0, sauf le pointeur
   de pile (4H). Les jeux n'executent rien dans la ROM, mais certains la
   lisent sans le vouloir. Sol Divide (T-14423G) tourne avec GBR = 0 alors
   que sa bibliotheque SGL suppose GBR = 060FFC00H : la fonction d'etat des
   DMA (06020938) lit l'octet d'etat du niveau 0 en GBR + BBH, donc en
   000000BBH dans la ROM, et ne regarde DSTA que si son bit 2 est a 0. Avec
   la ROM vide, le maitre attendait sans fin apres le DMA du premier secteur
   du film (commande CD 6100H), sans jamais envoyer 0600H / 6200H ; avec le
   vrai BIOS, l'octet vaut 26H et la fonction rend la main aussitot.

   Valeurs : table des vecteurs du BIOS Saturn v1.01, vecteurs 4 a 63
   (exceptions et interruptions ; NMI en 2CH). Les vecteurs 0 a 3 (reset)
   ne sont pas touches : Kronos fixe lui-meme le PC et la pile du BIOS
   emule. Ces adresses ne sont jamais executees par le BIOS emule (le VBR
   est en 06000000H), elles ne servent qu'aux lectures. */
void BiosInitRomVectors(u8 *rom)
{
   int i;
   if (rom == NULL) return;
   for (i = 4; i < 64; i++)
   {
      u32 v = 0x20000226;
      if ((i == 4) || (i == 6) || (i == 9) || (i == 10)) v = 0x20000222;
      else if (i == 11) v = 0x20000534;
      T2WriteLong(rom, i << 2, v);
   }
}

//////////////////////////////////////////////////////////////////////////////

void BiosInit(SH2_struct *context)
{
   int i;

   /* Etat du BIOS emule laisse par le jeu precedent. Ces tables et l'appel
      SYS_CHGSYSCK en cours sont des variables statiques : elles survivaient
      au changement de jeu et au reset, si bien qu'un jeu demarrait avec la
      table de priorites installee par SYS_CHGSCUPRI dans le jeu d'avant
      (Ishin no Arashi et Yuukyuu Gensoukyoku Ensemble en installent une ou
      le SR vaut toujours 0), ou avec un SYS_CHGSYSCK reste en suspens. */
   hle_ckchg_pending = 0;

   // Setup vectors
   SH2MappedMemoryWriteLong(context, 0x06000600, 0x002B0009); // rte, nop
   SH2MappedMemoryWriteLong(context, 0x06000604, 0xE0F0600C); // mov #0xF0, r0; extu.b r0, r0
   SH2MappedMemoryWriteLong(context, 0x06000608, 0x400E8BFE); // ldc r0, sr; bf
   SH2MappedMemoryWriteLong(context, 0x0600060C, 0x00090009); // nop
   SH2MappedMemoryWriteLong(context, 0x06000610, 0x000B0009); // rts, nop
   SH2MappedMemoryWriteLong(context, 0x06000614, 0xAFFE0009); // bra self, nop

   for (i = 0; i < 0x200; i+=4)
   {
      SH2MappedMemoryWriteLong(context, 0x06000000+i, 0x06000600);
      SH2MappedMemoryWriteLong(context, 0x06000400+i, 0x06000600);
      interruptlist[0][i >> 2] = 0x06000600;
      interruptlist[1][i >> 2] = 0x06000600;
   }

   /* NMI (vecteur 11) du maitre : gestionnaire du BIOS emule en 4A8H, voir
      BiosNmi(). Le NMI de fin de SYS_CHGSYSCK y termine l'appel. */
   SH2MappedMemoryWriteLong(context, 0x0600002C, 0x000004A8);
   interruptlist[0][11] = 0x000004A8;

   SH2MappedMemoryWriteLong(context, 0x06000010, 0x06000604);
   SH2MappedMemoryWriteLong(context, 0x06000018, 0x06000604);
   SH2MappedMemoryWriteLong(context, 0x06000024, 0x06000604);
   SH2MappedMemoryWriteLong(context, 0x06000028, 0x06000604);
   interruptlist[0][4] = 0x06000604;
   interruptlist[0][6] = 0x06000604;
   interruptlist[0][9] = 0x06000604;
   interruptlist[0][10] = 0x06000604;

   SH2MappedMemoryWriteLong(context, 0x06000410, 0x06000604);
   SH2MappedMemoryWriteLong(context, 0x06000418, 0x06000604);
   SH2MappedMemoryWriteLong(context, 0x06000424, 0x06000604);
   SH2MappedMemoryWriteLong(context, 0x06000428, 0x06000604);
   interruptlist[1][4] = 0x06000604;
   interruptlist[1][6] = 0x06000604;
   interruptlist[1][9] = 0x06000604;
   interruptlist[1][10] = 0x06000604;

   // Scu Interrupts
   for (i = 0; i < 0x38; i+=4)
   {
      SH2MappedMemoryWriteLong(context, 0x06000100+i, 0x00000400+i);
      interruptlist[0][0x40+(i >> 2)] = 0x00000400+i;
   }

   for (i = 0; i < 0x40; i+=4)
   {
      SH2MappedMemoryWriteLong(context, 0x06000140+i, 0x00000440+i);
      interruptlist[0][0x50+(i >> 2)] = 0x00000440+i;
   }

   /* Fonctions des facteurs SCU 40H-5FH (SYS_SETUINT) : 6000A00H-6000A7FH. */
   for (i = 0; i < 0x80; i+=4)
      SH2MappedMemoryWriteLong(context, 0x06000A00+i, 0x06000610);

   /* Table de priorite : 6000A80H-6000AFFH (voir scupri_default). */
   for (i = 0; i < 0x20; i++)
      SH2MappedMemoryWriteLong(context, BIOS_SCU_PRI_TABLE + ((0x40 + i) << 2), scupri_default[i]);

   /* SYS_CHGSCUPRI (6000280H) en code SH-2 dans la Work RAM-H, comme le fait
      le boot ROM, au lieu d'une fonction du BIOS emule.

      Le BIOS emule executait les fonctions systeme en 0 cycle. Le boot ROM
      execute cette copie de 32 mots longs (environ 135 instructions, 32
      lectures et 32 ecritures) sur le CPU appelant. Yuukyuu Gensoukyoku
      Ensemble (T-27808G) l'appelle entre l'envoi de la commande
      d'initialisation 08H a l'esclave et l'ecriture des travaux de la trame
      suivante dans la file maitre->esclave (0604992A) : mesure dans Kronos,
      3035 cycles du maitre entre les deux avec le BIOS reel, 2055 avec le
      BIOS emule. L'esclave, qui ecrit ses tables par-dessus la file
      (060EF538-), arrivait alors en 060EF550 apres le maitre et ecrasait
      la fin de trame (2CH) : ecran noir au premier lancement.

         6000620H  0102  stc   sr,r1
         6000622H  E3F0  mov   #-16,r3
         6000624H  633C  extu.b r3,r3          ; r3 = F0H
         6000626H  430E  ldc   r3,sr           ; interruptions masquees
         6000628H  D004  mov.l @(600063CH),r0  ; r0 = 6000A80H
         600062AH  E320  mov   #32,r3
         600062CH  6246  mov.l @r4+,r2
         600062EH  4310  dt    r3
         6000630H  2022  mov.l r2,@r0
         6000632H  8FFB  bf/s  600062CH
         6000634H  7004  add   #4,r0
         6000636H  000B  rts
         6000638H  410E  ldc   r1,sr
         600063AH  0009  nop
         600063CH  06000A80                     */
   SH2MappedMemoryWriteLong(context, 0x06000620, 0x0102E3F0);
   SH2MappedMemoryWriteLong(context, 0x06000624, 0x633C430E);
   SH2MappedMemoryWriteLong(context, 0x06000628, 0xD004E320);
   SH2MappedMemoryWriteLong(context, 0x0600062C, 0x62464310);
   SH2MappedMemoryWriteLong(context, 0x06000630, 0x20228FFB);
   SH2MappedMemoryWriteLong(context, 0x06000634, 0x7004000B);
   SH2MappedMemoryWriteLong(context, 0x06000638, 0x410E0009);
   SH2MappedMemoryWriteLong(context, 0x0600063C, BIOS_SCU_PRI_TABLE + (0x40 << 2));

   /* Point d'entree de l'esclave.

      YabauseStartSlave() lit 6000250H une seule fois, au moment du SSHON,
      et y prend le PC de l'esclave. Rien n'initialisait cette case : elle
      valait donc 0 (HighWram vient d'etre remis a zero), et un jeu qui
      emet SSHON avant d'avoir publie son entree lancait l'esclave a
      l'adresse 0.

      Le vrai boot ROM, lui, laisse l'esclave dans une boucle (SEGA Saturn
      Dual CPU User's Guide, ST-202-R1, 4.3 : "the boot ROM rewrites the
      execution entry ... after read initialization"). On pointe donc la
      valeur par defaut sur la boucle inconditionnelle ecrite en 6000614H :
      l'esclave tourne a vide au lieu d'executer n'importe quoi, et le jeu
      ecrase cette valeur des qu'il fournit sa vraie fonction d'entree. */
   SH2MappedMemoryWriteLong(context, 0x06000250, 0x06000614);

   /* 6000260H : adresse d'une zone libre de la RAM du BIOS.

      Rien ne l'initialisait : la case valait 0. 3D Lemmings (T-11304H, Europe
      et Japon) a un AIP (IP, 6002E20H) qui s'en sert pour installer son
      propre CDINIT1 :
          r7 = *(6000260H)            zone libre
          *(6002E9CH) = *(60002DCH)   sauvegarde du vrai CDINIT1
          copie 16H mots (6002E48H-6002E9FH, code + litteraux) vers r7
          *(60002DCH) = r7            CDINIT1 pointe sur la copie
      Avec 0 la copie partait en 00000000H (ROM, ecritures perdues) et
      CDINIT1 pointait sur 0 : premier appel CD du jeu (0604A0CCH, appel
      M 00000) -> instruction invalide -> boucle d'exception en 600060AH,
      ecran noir, VDP1 a 0 image/s.

      La valeur du vrai BIOS n'est pas documentee dans les manuels du
      workspace. On prend une zone que le BIOS emule n'utilise pas :
      600061CH-60008FFH est libre (code en 6000600H-6000617H, gestionnaires
      SCU en 6000900H-6000A7FH, table 6000A00H-6000AFFH, semaphores en
      6000B00H). 6000700H laisse 200H octets. */
   SH2MappedMemoryWriteLong(context, 0x06000260, 0x06000700);

   // Setup Bios Functions
   SH2MappedMemoryWriteLong(context, 0x06000210, 0x00000210);
   SH2MappedMemoryWriteLong(context, 0x0600026C, 0x0000026C);
   SH2MappedMemoryWriteLong(context, 0x06000274, 0x00000274);
   SH2MappedMemoryWriteLong(context, 0x06000280, BIOS_SCU_CHGSCUPRI);
   SH2MappedMemoryWriteLong(context, 0x0600029C, 0x0000029C);
   SH2MappedMemoryWriteLong(context, 0x060002DC, 0x000002DC);
   SH2MappedMemoryWriteLong(context, 0x06000300, 0x00000300);
   SH2MappedMemoryWriteLong(context, 0x06000304, 0x00000304);
   SH2MappedMemoryWriteLong(context, 0x06000310, 0x00000310);
   SH2MappedMemoryWriteLong(context, 0x06000314, 0x00000314);
   SH2MappedMemoryWriteLong(context, 0x06000320, 0x00000320);
   SH2MappedMemoryWriteLong(context, 0x06000324, 0x00000000);
   SH2MappedMemoryWriteLong(context, 0x06000330, 0x00000330);
   SH2MappedMemoryWriteLong(context, 0x06000334, 0x00000334);
   SH2MappedMemoryWriteLong(context, 0x06000340, 0x00000340);
   SH2MappedMemoryWriteLong(context, 0x06000344, 0x00000344);
   SH2MappedMemoryWriteLong(context, 0x06000348, 0xFFFFFFFF);
   SH2MappedMemoryWriteLong(context, 0x06000354, 0x00000000);
   SH2MappedMemoryWriteLong(context, 0x06000358, 0x00000358);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosSetScuInterrupt(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

//   LOG("BiosSetScuInterrupt. vector = %02X, func = %08X\n", sh->regs.R[4], sh->regs.R[5]);

   if (sh->regs.R[5] == 0)
   {
      SH2MappedMemoryWriteLong(sh, 0x06000900+(sh->regs.R[4] << 2), 0x06000610);
   }
   else
   {
      SH2MappedMemoryWriteLong(sh, 0x06000900+(sh->regs.R[4] << 2), sh->regs.R[5]);
   }

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosGetScuInterrupt(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

   // check me
//   LOG("BiosGetScuInterrupt\n");

   sh->regs.R[0] = SH2MappedMemoryReadLong(sh, 0x06000900+(sh->regs.R[4] << 2));

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosSetSh2Interrupt(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

//   LOG("BiosSetSh2Interrupt\n");

   if (sh->regs.R[5] == 0)
   {
      SH2MappedMemoryWriteLong(sh, sh->regs.VBR+(sh->regs.R[4] << 2), interruptlist[sh->isslave][sh->regs.R[4]]);
   }
   else
   {
      SH2MappedMemoryWriteLong(sh, sh->regs.VBR+(sh->regs.R[4] << 2), sh->regs.R[5]);
   }

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosGetSh2Interrupt(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

   // check me
//   LOG("BiosGetSh2Interrupt\n");

   sh->regs.R[0] = SH2MappedMemoryReadLong(sh, sh->regs.VBR+(sh->regs.R[4] << 2));

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

/* SYS_SETSCUIM (6000340H) et SYS_CHGSCUIM (6000344H), d'apres le code que le
   boot ROM installe en Work RAM-H (060007B0 et 060007C0, releve dans un vidage
   de la Work RAM-H avec le BIOS reel), qui precise ST-162 (Saturn System
   Library User's Guide, 2.1 et 2.2) :

   SYS_SETSCUIM(MaskPat)              SYS_CHGSCUIM(AndMask, OrMask)
     SR = F0H                           SR = F0H
     M = MaskPat                        M = (@6000348H & AndMask) | OrMask
     @6000348H = M, IMS = M             @6000348H = M, IMS = M
     v = MaskPat (16 bits, signe)       v = AndMask | OrMask (16 bits, signe)
     IST = v                            IST = v
     si bit 15 de v = 0 : AIACK = 1     si bit 15 de v = 0 : AIACK = 1
     SR restaure                        SR restaure

   Une ecriture de IST efface les bits ecrits a 0 : SYS_CHGSCUIM efface donc
   les interruptions en attente dont le bit est a 0 dans AndMask | OrMask,
   pas seulement le mot haut. Le code ne teste pas le CPU appelant.

   Le BIOS emule n'ecrivait IST pour SYS_CHGSCUIM que si le nouveau masque
   autorisait l'A-Bus, et alors seulement 0000FFFFH. Sol Divide (T-14423G) :
   GFS_Init appelle SYS_CHGSCUIM(FFFFF7FFH, 0) pour demasquer la fin de DMA
   niveau 0 alors qu'une fin de DMA0 du transfert de la liste VDP1 est encore
   en attente (IST bit 11). Le BIOS reel l'efface (IST = FFFFF7FFH) ; le BIOS
   emule la laissait, l'interruption partait aussitot vers le gestionnaire par
   defaut, et la lecture du premier secteur du film par DMA0 (commande CD
   6100H) n'etait jamais terminee : pas de commandes 0600H / 6200H, jeu fige
   sur l'ecran du film. */
static void BiosWriteScuMask(SH2_struct * sh, u32 mask, u32 istval)
{
   const u32 v = (u32)(s32)(s16)(istval & 0xFFFF);

   /* Le boot ROM ecrit IMS puis IST, SR a F0H : aucune interruption ne peut
      etre prise entre les deux, et une demande effacee dans IST disparait.
      Le SCU de Kronos, lui, presente au CPU une demande devenue demasquee des
      l'ecriture suivante d'un de ses registres (ScuTestAllInterrupt() au
      debut de l'ecriture de IST, avant l'effacement) et ne la retire plus :
      dans cet ordre, la fin de DMA0 en attente de Sol Divide partait quand
      meme. IST est donc ecrit avant IMS ; l'etat final est le meme que sur
      la console. */
   SH2MappedMemoryWriteLong(sh, 0x06000348, mask);
   SH2MappedMemoryWriteLong(sh, 0x25FE00A4, v);    // Interrupt Status Register
   SH2MappedMemoryWriteLong(sh, 0x25FE00A0, mask); // Interrupt Mask Register
   if (!(v & 0x8000))
      SH2MappedMemoryWriteLong(sh, 0x25FE00A8, 1); // A-bus Interrupt Acknowledge
}

static void FASTCALL BiosSetScuInterruptMask(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

   BiosWriteScuMask(sh, sh->regs.R[4], sh->regs.R[4]);

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosChangeScuInterruptMask(SH2_struct * sh)
{
   u32 newmask;

   SH2GetRegisters(sh, &sh->regs);

   // Read Stored Scu Interrupt Mask, AND it by R4, OR it by R5, then put it back
   newmask = (SH2MappedMemoryReadLong(sh, 0x06000348) & sh->regs.R[4]) | sh->regs.R[5];
   BiosWriteScuMask(sh, newmask, sh->regs.R[4] | sh->regs.R[5]);

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////

static void FASTCALL BiosCDINIT2(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosCDINIT1(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosGetSemaphore(SH2_struct * sh)
{
   u8 temp;

   SH2GetRegisters(sh, &sh->regs);

   // check me
//   LOG("BiosGetSemaphore\n");

   if ((temp = SH2MappedMemoryReadByte(sh, 0x06000B00 + sh->regs.R[4])) == 0)
      sh->regs.R[0] = 1;
   else
      sh->regs.R[0] = 0;

   temp |= 0x80;
   SH2MappedMemoryWriteByte(sh, 0x06000B00 + sh->regs.R[4], temp);

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosClearSemaphore(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

   // check me
//   LOG("BiosClearSemaphore\n");

   SH2MappedMemoryWriteByte(sh, 0x06000B00 + sh->regs.R[4], 0);

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////


static void FASTCALL BiosChangeSystemClock(SH2_struct * sh)
{
   int i;
   SH2GetRegisters(sh, &sh->regs);

   hle_ckchg_pending = !sh->isslave;
   hle_ckchg_r15 = sh->regs.R[15];
   hle_ckchg_pr = sh->regs.PR;
   hle_ckchg_sr = sh->regs.SR.all;
   for (i = 0; i < 7; i++)
      hle_ckchg_r8_14[i] = sh->regs.R[8 + i];

//   LOG("BiosChangeSystemClock\n");

   // Set new system clock speed
   SH2MappedMemoryWriteLong(sh, 0x06000324, sh->regs.R[4]);

   SH2MappedMemoryWriteLong(sh, 0x25FE00A8, 0); // Clear A-bus Interrupt ACK
   SH2MappedMemoryWriteLong(sh, 0x25FE00B8, 0); // Clear A-Bus Refresh

   SH2MappedMemoryWriteByte(sh, 0xFFFFFE91, 0x80); // Transition to standby mode
   SH2MappedMemoryWriteWord(sh, 0xFFFFFE80, 0xA51D); // Set WDT counter
   /* ICR (FFFFFEE0H) : bit 15 NMIL est en lecture seule, bit 8 NMIE choisit
      le front du NMI, bit 0 VECMD choisit le mode vecteur. Seuls les bits 8
      et 0 s'ecrivent (voir OnchipWriteWord). L'ancienne ecriture 8000H
      n'avait donc aucun effet sur le front, mais remettait VECMD a 0 :
      le SH-2 repassait en auto-vecteur alors que sur Saturn c'est le SCU qui
      fournit le numero de vecteur (le boot ROM met VECMD a 1, et
      YabauseQuickLoadGame() fait de meme en BIOS emule).

      En auto-vecteur, SH2EvaluateInterrupt() prend 40H + (niveau >> 1) :
      le V-Blank IN (niveau 15) et le V-Blank OUT (niveau 14) partaient sur
      le vecteur 47H (SMPC), et ScuAcceptInterrupt() n'etant appele que dans
      la branche vecteur externe, le SCU ne relachait plus son interruption en
      cours : plus aucune interruption apres les premieres.
      Virtua Cop appelle SYS_CHGSYSCK juste apres avoir installe ses
      gestionnaires (trace : vecteur 47H recu trois fois, puis plus rien,
      maitre bloque en 60047C0H a attendre le V-Blank, ecran noir).

      NMIE garde la valeur que donnait l'ancienne ecriture (0) ; VECMD reste
      a 1. */
   SH2MappedMemoryWriteWord(sh, 0xFFFFFEE0, 0x0001);

   /* Le changement d'horloge passe par la commande SMPC, comme sur le vrai
      BIOS, et la fonction ne rend la main qu'une fois la commande terminee
      (BiosChangeSystemClockWait, en 4A0H).

      Avant, le BIOS emule appelait SmpcCKCHG320/352() directement : remise
      a zero de VDP1, VDP2 et du SCU, NMI, puis retour immediat. Avec la
      commande SMPC, Kronos remet ces circuits a zero au debut de la commande
      et n'envoie le NMI que deux trames plus tard (SmpcCKCHGStart et
      SmpcSetTiming dans smpc.c, ordre de Mednafen) ; entre les deux, le SCU
      vient d'etre remis a zero (IMS = BFFFH, tout masque) et IST memorise
      les V-Blank IN/OUT et H-Blank IN qui surviennent. Le programme retrouve
      ces bits a 1 quand SYS_CHGSYSCK revient.

      Discworld (T-20502G) en depend : juste apres SYS_CHGSYSCK(320) il
      masque le V-Blank OUT (SYS_CHGSCUIM) et lit deux fois IST (06013F20,
      06013F28) en attendant le bit 1 ; la boucle qui suit (06013F2C) ne
      relit pas IST. Avec le retour immediat, IST venait d'etre remis a 0 :
      maitre bloque en 06013F2C, ecran noir, VDP1 a 0 image/s. C'est le meme
      comportement que smpc.c corrige deja pour le vrai BIOS. */
   SH2MappedMemoryWriteByte(sh, 0x2010001F, (sh->regs.R[4] == 0) ? 0x0F : 0x0E);

   sh->regs.PC = 0x000004A0;
   SH2SetRegisters(sh, &sh->regs);
}

/* Attente de la fin de la commande CKCHG lancee par BiosChangeSystemClock().
   Le PC reste en 4A0H (l'instruction non decodee est reexecutee) tant que le
   SMPC n'a pas remis SF a 0 ; le NMI envoye a la fin de la commande passe par
   le vecteur 11 de BiosInit() (rte) et revient ici. Ensuite, la fin de
   l'ancienne fonction : registres du SCU et masque d'interruption. */
/* Fin de SYS_CHGSYSCK : registres du SCU et masque d'interruption.

   Les trois niveaux de DMA du SCU sont espaces de 20H (D0R 25FE0000H,
   D1R 25FE0020H, D2R 25FE0040H), six registres chacun (R, W, C, AD, EN,
   MD). La boucle avancait de 0CH par niveau et ecrivait sept registres :
   elle remettait a zero une partie des niveaux 0 et 1 deux fois et ne
   touchait jamais le niveau 2. */
static void BiosChangeSystemClockFinish(SH2_struct * sh)
{
   int i, j;
   u32 mask;

   // Clear SCU DMA Regs
   for (j = 0; j < 3; j++)
   {
      for (i = 0; i < 6; i++)
         SH2MappedMemoryWriteLong(sh, 0x25FE0000+(j*0x20)+(i*4), 0);
   }

   SH2MappedMemoryWriteLong(sh, 0x25FE0060, 0); // Clear DMA force stop
   SH2MappedMemoryWriteLong(sh, 0x25FE0080, 0); // Clear DSP Control Port
   SH2MappedMemoryWriteLong(sh, 0x25FE00B0, 0x1FF01FF0); // Reset A-Bus Set
   SH2MappedMemoryWriteLong(sh, 0x25FE00B4, 0x1FF01FF0);
   SH2MappedMemoryWriteLong(sh, 0x25FE00B8, 0x1F); // Reset A-Bus Refresh
   SH2MappedMemoryWriteLong(sh, 0x25FE00A8, 0x1); // Reset A-bus Interrupt ACK
   SH2MappedMemoryWriteLong(sh, 0x25FE0090, 0x3FF); // Reset Timer 0 Compare
   SH2MappedMemoryWriteLong(sh, 0x25FE0094, 0x1FF); // Reset Timer 1 Set Data
   SH2MappedMemoryWriteLong(sh, 0x25FE0098, 0); // Reset Timer 1 Mode

   mask = SH2MappedMemoryReadLong(sh, 0x06000348);
   SH2MappedMemoryWriteLong(sh, 0x25FE00A0, mask); // Interrupt Mask Register

   if (!(mask & 0x8000))
      SH2MappedMemoryWriteLong(sh, 0x25FE00A8, 1); // A-bus Interrupt Acknowledge
}

static void FASTCALL BiosChangeSystemClockWait(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

   /* Le NMI de fin de commande est leve en meme temps que SF retombe
      (SmpcCKCHG320/352). Tant qu'il n'a pas ete pris, on attend : c'est
      lui qui termine l'appel (BiosNmi). On ne finit ici que si le jeu a
      installe son propre gestionnaire NMI et que celui-ci est revenu par
      rte sans passer par le BIOS. */
   if ((SmpcRegs->SF & 0x1) || (sh->intc.nmi != 0))
   {
      /* Le SH-2 attend : on lui fait consommer le reste de sa tranche, comme
         SLEEP. L'instruction non decodee en 4A0H ne coute rien (ROM BIOS
         emulee, aucun cout d'acces) : sans cela la boucle de l'interpreteur
         (while cycles < target_cycles) ne finissait jamais, la trame non
         plus, et le SMPC qui doit terminer la commande ne s'executait plus. */
      if (sh->target_cycles > sh->cycles)
         sh->cycles = sh->target_cycles;
      else
         sh->cycles += 4;
      return;
   }

   hle_ckchg_pending = 0;
   BiosChangeSystemClockFinish(sh);

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

/* 4A8H : gestionnaire NMI du maitre installe par BiosInit().

   Pendant SYS_CHGSYSCK, le NMI envoye par le SMPC a la fin de CKCHG termine
   l'appel : on rend a l'appelant sa pile, son SR et ses registres R8-R14
   tels qu'ils etaient a l'appel, et on revient a son adresse de retour, quel
   que soit l'etat de la pile au moment ou ce gestionnaire est atteint.

   Sol Divide (T-14423G) en depend. Avant SYS_CHGSYSCK il installe son
   propre gestionnaire NMI (060034A0) et garde l'ancien. Ce gestionnaire est
   une fonction C ordinaire (empile R8 et PR, reinitialise l'affichage,
   appelle l'ancien gestionnaire par jsr, puis rts) : il compte sur le BIOS
   pour ne jamais revenir vers lui. L'ancien gestionnaire du BIOS emule etait
   un simple rte (6000600H), qui depilait le PR et le R8 sauves par le jeu a
   la place du PC et du SR : le jeu reprenait a l'adresse de retour de
   SYS_CHGSYSCK avec 8 octets de trop sur la pile, et la fonction appelante
   revenait plus tard en 4A0H (PC = PR = 4A0H, trame t250 du releve),
   bloquant le maitre dans l'attente de SYS_CHGSYSCK, ecran noir.

   Hors SYS_CHGSYSCK, ce gestionnaire se comporte comme l'ancien : rte. */
static void FASTCALL BiosNmi(SH2_struct * sh)
{
   int i;
   SH2GetRegisters(sh, &sh->regs);

   if (hle_ckchg_pending && !sh->isslave)
   {
      hle_ckchg_pending = 0;
      sh->regs.R[15] = hle_ckchg_r15;
      for (i = 0; i < 7; i++)
         sh->regs.R[8 + i] = hle_ckchg_r8_14[i];
      sh->regs.PR = hle_ckchg_pr;
      sh->regs.SR.all = hle_ckchg_sr;
      BiosChangeSystemClockFinish(sh);
      sh->regs.PC = hle_ckchg_pr;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   /* rte */
   sh->regs.PC = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.SR.all = SH2MappedMemoryReadLong(sh, sh->regs.R[15] + 4) & 0x000003F3;
   sh->regs.R[15] += 8;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosChangeScuInterruptPriority(SH2_struct * sh)
{
   int i;

   /* Appel direct de 280H (6000280H pointe sur la copie SH-2 en 6000620H,
      voir BiosInit()) : meme effet, la table est en Work RAM-H. */
   SH2GetRegisters(sh, &sh->regs);

   for (i = 0; i < 0x20; i++)
      SH2MappedMemoryWriteLong(sh, BIOS_SCU_PRI_TABLE + ((0x40 + i) << 2),
                               SH2MappedMemoryReadLong(sh, sh->regs.R[4]+(i << 2)));

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosExecuteCDPlayer(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

//   LOG("BiosExecuteCDPlayer\n");

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosPowerOnMemoryClear(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

//   LOG("BiosPowerOnMemoryClear\n");

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosCheckMPEGCard(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

   // Real return-value convention for this HLE call is undocumented, but
   // every other CheckXxx/Get* stub in this file returns its boolean in
   // R0 (see BiosGetSemaphore() above), and this is exactly what
   // Cs2GetHardwareInfo()'s CR2 bit 9 and Cs2AuthenticateDevice()'s
   // mpgauth agree on: see Cs2IsMpegCardPresent() in cs2.c.
   sh->regs.R[0] = Cs2IsMpegCardPresent() ? 1 : 0;

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

/* Codes de retour de la bibliotheque de sauvegarde : sega_bup.h de la SGL
 * (Copyright(c) 1994 SEGA, "Machine state", Ver.1.00 du 1994-07-29).
 * ST-162 (Saturn System Library User's Guide, 2.4) dit quelle fonction rend
 * quel code. Le HLE ecrivait jusqu'ici des nombres en dur. */
#define BUP_NON                 1   /* peripherique non connecte */
#define BUP_UNFORMAT            2   /* non formate */
#define BUP_WRITE_PROTECT       3   /* protege en ecriture */
#define BUP_NOT_ENOUGH_MEMORY   4   /* place insuffisante */
#define BUP_NOT_FOUND           5   /* fichier absent */
#define BUP_FOUND               6   /* un fichier du meme nom existe */
#define BUP_NO_MATCH            7   /* BUP_Verify : donnees differentes */
#define BUP_BROKEN              8   /* fichier endommage */

/* Les listes de blocs d'une sauvegarde sont en 16 bits (2 octets par bloc,
 * voir BiosBUPWrite) : un numero de bloc ne depasse pas 0xFFFF. */
#define BUP_MAX_BLOCKS          0x10000

static u32 GetDeviceStats(u32 device, u32 *size, u32 *addr, u32 *blocksize)
{
   switch(device)
   {
      case 0:
          *addr = backup_file_addr;
          *size = backup_file_size;
          *blocksize = 0x40;
          /* FIX : en mode "extended backup" (8 Mo, blocs de 64 octets) le
           * peripherique comptait 0x20000 blocs, mais les numeros de bloc
           * sont stockes sur 16 bits : GetFreeBlocks() tronquait (u16)i et
           * une sauvegarde allouee au-dela du bloc 0xFFFF pointait sur un
           * autre bloc (ecrasement de donnees). On n'expose que les 0x10000
           * premiers blocs (4 Mo) ; la memoire interne standard (32 Ko, ST-162
           * 1.2.3 "The built-in backup memory is 32 Kbyte") n'est pas
           * concernee. */
          if ((*size / *blocksize) > BUP_MAX_BLOCKS)
             *size = BUP_MAX_BLOCKS * *blocksize;
        return 0;
      case 1:
         if ((CartridgeArea->cartid & 0xF0) == 0x20)
         {
            *addr = 0x04000000;
            *size = 0x40000 << (CartridgeArea->cartid & 0x0F);
            if (CartridgeArea->cartid == 0x24)
               *blocksize = 0x400;
            else
               *blocksize = 0x200;

            return 0;
         }
         else
            return BUP_NON;
      default:
         *addr = 0;
         *size = 0;
         *blocksize = 0;
         return BUP_NON;
   }

   return BUP_NON;
}

//////////////////////////////////////////////////////////////////////////////

/* Verifie que le peripherique est formate.
 * FIX : la fonction rendait toujours 0, si bien que BUP_Stat ne renvoyait
 * jamais BUP_UNFORMAT. ST-162 1.3 (exemple de programme) : un jeu fait
 *    if (BUP_Stat(0, &sttb) == BUP_UNFORMAT) BUP_Format(0);
 * Sur une memoire non formatee il ecrivait donc dans une structure vide.
 * Le format est celui ecrit par FormatBackupRam() (memory.c) : l'en-tete
 * "BackUpRam Format" (16 octets) repete 4 fois au debut de la memoire, sur
 * les octets impairs du bus (SRAM sur D7-D0, voir BupRamMemoryReadByte).
 * Rend 0 si formate, BUP_UNFORMAT sinon (BUP_NON si non connecte). */
static int CheckHeader(u32 device)
{
   static const char header[16] = {
      'B', 'a', 'c', 'k', 'U', 'p', 'R', 'a', 'm', ' ', 'F', 'o', 'r', 'm', 'a', 't' };
   u32 size, addr, blocksize;
   int i;

   if (GetDeviceStats(device, &size, &addr, &blocksize) != 0)
      return BUP_NON;

   for (i = 0; i < 16; i++)
      if (DMAMappedMemoryReadByte(addr + (i << 1) + 1) != (u8)header[i])
         return BUP_UNFORMAT;

   return 0;
}

/* Erreur commune des fonctions BUP : non connecte (BUP_NON) ou non formate
 * (BUP_UNFORMAT), 0 si le peripherique est utilisable. */
static u32 BupCheckDevice(u32 device, u32 *size, u32 *addr, u32 *blocksize)
{
   u32 ret = GetDeviceStats(device, size, addr, blocksize);
   if (ret != 0)
      return ret;
   return (u32)CheckHeader(device);
}

//////////////////////////////////////////////////////////////////////////////

// Walks the block list of the save starting at 'block' and returns the
// number of blocks listed (the first block is not counted).
// When the list does not fit in the first block, it continues in the blocks
// it lists (in order), after their 4 bytes header: this is the same chaining
// used by BiosBUPWrite/BiosBUPImport (writing) and ReadBlockTable (reading).
// If 'usedtbl' is not NULL, every listed block is marked as used in it.
static int CalcSaveSize(u32 addr, u32 block, u32 blocksize, u32 size, u8 *usedtbl)
{
   u32 totalblocks = size / blocksize;
   u32 mask = (blocksize << 1) - 1;
   u32 tableaddr = addr + (block * blocksize * 2) + 0x45;
   u16 *list;
   u32 numblocks = 0;
   u32 blocksread = 0;

   if ((list = (u16 *)malloc(sizeof(u16) * totalblocks)) == NULL)
      return 0;

   for(;;)
   {
      u16 next = (DMAMappedMemoryReadByte(tableaddr) << 8) | DMAMappedMemoryReadByte(tableaddr + 2);

      if (next == 0)
         break;

      // Broken list: stop instead of reading/writing out of bounds
      if (next < 2 || next >= totalblocks || numblocks >= totalblocks)
      {
         LOG("CalcSaveSize: broken block list (block %d, entry %d = %04X)\n", block, numblocks, next);
         break;
      }

      list[numblocks++] = next;
      if (usedtbl)
         usedtbl[next] = 1;

      tableaddr += 4;
      if ((((tableaddr - 1) & mask) == 0) && (blocksread < numblocks))
      {
         // The list continues in the next block of the save
         tableaddr = addr + (list[blocksread] * blocksize * 2) + 9;
         blocksread++;
      }
   }

   free(list);
   return (int)numblocks;
}

//////////////////////////////////////////////////////////////////////////////

static u32 GetFreeSpace(UNUSED u32 device, u32 size, u32 addr, u32 blocksize)
{
   u32 i;
   u32 usedblocks=0;

   for (i = ((2 * blocksize) << 1); i < (size << 1); i += (blocksize << 1))
   {
      // Find a block with the start of a save
      if (((s8)DMAMappedMemoryReadByte(addr + i + 1)) < 0)
      {
         // Now figure out how many blocks this save is
         usedblocks += (CalcSaveSize(addr, i / (blocksize << 1), blocksize, size, NULL) + 1);
      }
   }

   // Avoid an unsigned underflow if the memory is corrupted
   if ((usedblocks + 2) >= (size / blocksize))
      return 0;

   return ((size / blocksize) - 2 - usedblocks);
}

//////////////////////////////////////////////////////////////////////////////

/* Comparaison du nom de fichier.
 * ST-162 (Saturn System Library User's Guide, BUP_Read / BUP_Delete /
 * BUP_Verify) : "file name (11 characters are required in ASCII)" ; seul
 * BUP_Dir fait une recherche "forward" (par prefixe). L'ancien code faisait
 * la recherche par prefixe pour TOUTES les fonctions : un BUP_Write("SAVE")
 * trouvait et ECRASAIT (DeleteSave) un fichier "SAVE_OPTION" existant, et
 * BUP_Read/BUP_Delete/BUP_Verify pouvaient agir sur le mauvais fichier.
 * prefix = 1 : recherche par prefixe (BUP_Dir) ; 0 : nom exact (un NUL dans
 * le nom demande doit correspondre a un NUL sur le support). */
static int BupNameMatch(u8 wanted, u8 stored, int i3, int prefix, int *done)
{
   *done = 0;
   if (stored != wanted)
   {
      if ((wanted == 0) && prefix) { *done = 1; return 1; }
      *done = 1;
      return 0;
   }
   if ((i3 == 10) || (wanted == 0)) { *done = 1; return 1; }
   return 0;
}

static u32 FindSave(SH2_struct *context, UNUSED u32 device, u32 stringaddr, u32 blockoffset, u32 size, u32 addr, u32 blocksize, int prefix)
{
   u32 i;

   for (i = ((blockoffset * blocksize) << 1); i < (size << 1); i += (blocksize << 1))
   {
      // Find a block with the start of a save
      if (((s8)SH2MappedMemoryReadByte(context, addr + i + 1)) < 0)
      {
         int i3;

         // See if string matches, or if there's no string to check, just copy
         // the data over
         for (i3 = 0; i3 < 11; i3++)
         {
            int done;
            u8 data = SH2MappedMemoryReadByte(context, stringaddr+i3);
            u8 stored = SH2MappedMemoryReadByte(context, addr+i+0x9+(i3*2));

            if (BupNameMatch(data, stored, i3, prefix, &done))
               return ((i / blocksize) >> 1);
            if (done)
               break; // No Match, move onto the next block
         }
      }
   }

   return 0;
}


//////////////////////////////////////////////////////////////////////////////

static u32 FindSave2(UNUSED u32 device, const char *string, u32 blockoffset, u32 size, u32 addr, u32 blocksize)
{
   u32 i;

   for (i = ((blockoffset * blocksize) << 1); i < (size << 1); i += (blocksize << 1))
   {
      // Find a block with the start of a save
      if (((s8)DMAMappedMemoryReadByte(addr + i + 1)) < 0)
      {
         int i3;

         // See if string matches, or if there's no string to check, just copy
         // the data over
         for (i3 = 0; i3 < 11; i3++)
         {
            int done;
            // Compare as unsigned bytes: Japanese file names use half-width
            // katakana (0xA1-0xDF) which are negative when char is signed
            u8 data = (u8)string[i3];
            u8 stored = DMAMappedMemoryReadByte(addr+i+0x9+(i3*2));

            // Exact name (used by the frontend: delete/export/import)
            if (BupNameMatch(data, stored, i3, 0, &done))
               return ((i / blocksize) >> 1);
            if (done)
               break; // No Match, move onto the next block
         }
      }
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void DeleteSave(u32 addr, u32 blockoffset, u32 blocksize)
{
    DMAMappedMemoryWriteByte(addr + (blockoffset * blocksize * 2) + 0x1, 0x00);
}

//////////////////////////////////////////////////////////////////////////////

static u16 *GetFreeBlocks(u32 addr, u32 blocksize, u32 numblocks, u32 size)
{
   u8 *blocktbl;
   u16 *freetbl;
   u32 i;
   u32 blockcount=0;

   // Create a table that tells us which blocks are free and used
   if ((blocktbl = (u8 *)malloc(sizeof(u8) * (size / blocksize))) == NULL)
      return NULL;

   memset(blocktbl, 0, (size / blocksize));

   for (i = ((2 * blocksize) << 1); i < (size << 1); i += (blocksize << 1))
   {
      // Find a block with the start of a save
      if (((s8)DMAMappedMemoryReadByte(addr + i + 1)) < 0)
      {
         blocktbl[i / (blocksize << 1)] = 1;

         // Now let's figure out which blocks are used
         CalcSaveSize(addr, i / (blocksize << 1), blocksize, size, blocktbl);
      }
   }

   if ((freetbl = (u16 *)malloc(sizeof(u16) * numblocks)) == NULL)
   {
      free(blocktbl);
      return NULL;
   }

   // Find some free blocks for us to use
   for (i = 2; i < (size / blocksize); i++)
   {
      if (blocktbl[i] == 0)
      {
         freetbl[blockcount] = (u16)i;
         blockcount++;

         if (blockcount >= numblocks)
            break;
      }
   }

   // Ok, we're all done
   free(blocktbl);

   // Not enough free blocks: don't return a partially filled table
   if (blockcount < numblocks)
   {
      free(freetbl);
      return NULL;
   }

   return freetbl;
}

//////////////////////////////////////////////////////////////////////////////

/* Lit la liste des blocs d'une sauvegarde (meme chainage que CalcSaveSize).
 * FIX : aucune borne n'etait verifiee. Sur un support abime (ou un fichier de
 * sauvegarde corrompu) un numero de bloc hors limites faisait lire n'importe
 * ou, et une liste sans 0 final bouclait sans fin en grossissant le tableau
 * (realloc() non verifie). numblocks n'etait jamais renseigne alors que les
 * appelants s'en servent maintenant pour borner blocktbl[]. Retourne NULL si
 * la liste est invalide (les appelants rendent alors BUP_BROKEN = 8). */
static u16 *ReadBlockTable(u32 addr, u32 *tableaddr, int block, int blocksize, u32 size, int *numblocks, int *blocksread)
{
   u16 *blocktbl = NULL;
   int allocsize = 32;
   int index = 0;
   const u32 totalblocks = size / blocksize;
   const u32 mask = (blocksize << 1) - 1;

   tableaddr[0] = addr + (block * blocksize * 2) + 0x45;
   blocksread[0]=0;
   numblocks[0]=0;

   // Allocate buffer
   if ((blocktbl = (u16 *)malloc(sizeof(u16) * allocsize)) == NULL)
      return NULL;

   // Now read in the table
   for(;;)
   {
       u16 nextblock;
       nextblock = (DMAMappedMemoryReadByte(tableaddr[0]) << 8) | DMAMappedMemoryReadByte(tableaddr[0] + 2);
       if (nextblock == 0) {
         break;
       }
       if ((nextblock < 2) || (nextblock >= totalblocks) || ((u32)index >= totalblocks)) {
         LOG("ReadBlockTable: broken block list (block %d, entry %d = %04X)\n", block, index, nextblock);
         free(blocktbl);
         return NULL;
       }
       blocktbl[index] = nextblock;
       index++;
       if (index >= allocsize ) {
         u16 *newtbl;
         allocsize *= 2;
         newtbl = (u16 *)realloc(blocktbl, sizeof(u16) * allocsize);
         if (newtbl == NULL) {
           free(blocktbl);
           return NULL;
         }
         blocktbl = newtbl;
       }
       tableaddr[0] += 4;
       if (((tableaddr[0]-1) & mask) == 0)
       {
          if (blocksread[0] >= index) {
            free(blocktbl);
            return NULL;
          }
          tableaddr[0] = addr + (blocktbl[blocksread[0]] * blocksize * 2) + 9;
          blocksread[0]++;
       }
   }

   tableaddr[0] += 4;
   if (((tableaddr[0] - 1) & mask) == 0)
   {
     if (blocksread[0] >= index) {
       free(blocktbl);
       return NULL;
     }
     tableaddr[0] = addr + (blocktbl[blocksread[0]] * blocksize * 2) + 9;
     blocksread[0]++;
   }

   numblocks[0] = index;
   return blocktbl;
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPInit(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

   LOG("BiosBUPInit. arg1 = %08X, arg2 = %08X, arg3 = %08X\n", sh->regs.R[4], sh->regs.R[5], sh->regs.R[6]);

   // Setup Function table
   SH2MappedMemoryWriteLong(sh, 0x06000354, sh->regs.R[5]);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x00, 0x00000380);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x04, 0x00000384);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x08, 0x00000388);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x0C, 0x0000038C);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x10, 0x00000390);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x14, 0x00000394);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x18, 0x00000398);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x1C, 0x0000039C);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x20, 0x000003A0);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x24, 0x000003A4);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x28, 0x000003A8);
   SH2MappedMemoryWriteLong(sh, sh->regs.R[5]+0x2C, 0x000003AC);

   // Setup Device list

   // First Device
   SH2MappedMemoryWriteWord(sh, sh->regs.R[6], 1); // ID
   SH2MappedMemoryWriteWord(sh, sh->regs.R[6]+0x2, 1); // Number of partitions

   // Second Device
   if ((CartridgeArea->cartid & 0xF0) == 0x20)
   {
      SH2MappedMemoryWriteWord(sh, sh->regs.R[6]+0x4, 2); // ID
      SH2MappedMemoryWriteWord(sh, sh->regs.R[6]+0x6, 1); // Number of partitions
   }
   else
   {
      SH2MappedMemoryWriteWord(sh, sh->regs.R[6]+0x4, 0); // ID
      SH2MappedMemoryWriteWord(sh, sh->regs.R[6]+0x6, 0); // Number of partitions
   }

   // Third Device
   SH2MappedMemoryWriteWord(sh, sh->regs.R[6]+0x08, 0); // ID
   SH2MappedMemoryWriteWord(sh, sh->regs.R[6]+0x0A, 0); // Number of partitions

   // cycles need to be incremented

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPSelectPartition(SH2_struct * sh)
{
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 ret;
   u32 freeblocks = 0;
   u32 needsize;
   int aftersize;

   SH2GetRegisters(sh, &sh->regs);

   LOG_BUP("BiosBUPSelectPartition. PR = %08X device = %d \n", sh->regs.PR, sh->regs.R[4] );

   ret = GetDeviceStats(sh->regs.R[4], &size, &addr, &blocksize);
   /* ST-162 BUP_SelPart : num = 0 .. (nombre de partitions - 1). Chaque
    * peripherique emule n'a qu'une partition (BiosBUPInit) : un autre numero
    * est un echec ("other : failure"), il etait accepte. */
   if ((ret == 0) && ((sh->regs.R[5] & 0xFFFF) != 0))
     ret = BUP_NON;
   if (ret != 0) {
     // Error
     sh->regs.R[0] = ret;
     sh->regs.PC = sh->regs.PR;
     SH2SetRegisters(sh, &sh->regs);
     return;
   }

   sh->regs.R[0] = 0; // returns 0 if there's no error
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPFormat(SH2_struct * sh)
{
   SH2GetRegisters(sh, &sh->regs);

   LOG_BUP("BiosBUPFormat. PR = %08X\n", sh->regs.PR);
   /* Evenement rare et destructeur : toujours journalise, avec l'appelant
    * (jeu ou gestionnaire de memoire du BIOS). */
   YuiMsg("BUP_Format(device %d) called from %08X\n", (int)sh->regs.R[4], (unsigned int)sh->regs.PR);

   {
      u32 size, addr, blocksize;
      /* FIX : rendait 0 meme sans peripherique (ST-162 BUP_Format :
       * "other : failure"). */
      if (GetDeviceStats(sh->regs.R[4], &size, &addr, &blocksize) != 0)
      {
         sh->regs.R[0] = BUP_NON;
         sh->regs.PC = sh->regs.PR;
         SH2SetRegisters(sh, &sh->regs);
         return;
      }
   }

   BupFormat(sh->regs.R[4]);

   sh->regs.R[0] = 0; // returns 0 if there's no error
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPStatus(SH2_struct * sh)
{
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 ret;
   u32 freeblocks=0;
   u32 needsize;
   int aftersize;

   SH2GetRegisters(sh, &sh->regs);

   LOG_BUP("BiosBUPStatus. arg1 = %d, arg2 = %d, arg3 = %08X, PR = %08X\n", sh->regs.R[4], sh->regs.R[5], sh->regs.R[6], sh->regs.PR);

   // Fill in status variables
   // Make sure there's a proper header, and return if there's any other errors
   /* FIX : un peripherique non formate passait pour formate (CheckHeader
    * rendait toujours 0) et ret valait alors 0 : BUP_Stat annoncait un
    * succes. Rend maintenant BUP_NON ou BUP_UNFORMAT (ST-162 BUP_Stat). */
   ret = BupCheckDevice(sh->regs.R[4], &size, &addr, &blocksize);
   if (ret != 0)
   {
      // Error
      sh->regs.R[0] = ret;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   freeblocks = GetFreeSpace(sh->regs.R[4], size, addr, blocksize);

   /* datanum (BupStat, ST-162 2.1) : "number of items that can be written",
    * et "The value for the size specified by BUP_Stat() for datasize is
    * stored in datanum" : nombre de fichiers de 'datasize' octets qui
    * tiennent dans les blocs libres. Le code ecrivait (octets libres -
    * datasize) / taille de bloc, qui donnait 0 alors qu'un fichier tenait
    * exactement (le jeu concluait a tort qu'il n'y avait pas la place).
    * Nombre de blocs d'un fichier : meme calcul que BiosBUPWrite. */
   needsize = 1 + ((sh->regs.R[5] + 0x1D) / (blocksize - 6));
   aftersize = (int)(freeblocks / needsize);

   SH2MappedMemoryWriteLong(sh, sh->regs.R[6], size); // Size of Backup Ram (in bytes)
   SH2MappedMemoryWriteLong(sh, sh->regs.R[6]+0x4, size / blocksize); // Size of Backup Ram (in blocks)
   SH2MappedMemoryWriteLong(sh, sh->regs.R[6]+0x8, blocksize); // Size of block
   SH2MappedMemoryWriteLong(sh, sh->regs.R[6]+0xC, (((blocksize - 6) * freeblocks) > 30) ? (((blocksize - 6) * freeblocks) - 30) : 0); // Free space(in bytes)
   SH2MappedMemoryWriteLong(sh, sh->regs.R[6]+0x10, freeblocks); // Free space(in blocks)
   SH2MappedMemoryWriteLong(sh, sh->regs.R[6]+0x14, (u32)aftersize); // datanum: files of 'datasize' bytes that can be written

   // cycles need to be incremented

   sh->regs.R[0] = ret; // returns 0 if there's no error
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void BiosBUPWriteBody(SH2_struct * sh);

/* BUP_Write ecrit la RAM de sauvegarde avec le contexte du SH-2 appelant
 * (SH2MappedMemoryWriteByte). BupHleAccess indique a memory.c que ces
 * ecritures sont legitimes, pour les distinguer des ecritures egarees d'un
 * jeu dans la zone "extended backup" (BupRamMemoryWriteByte()). */
void FASTCALL BiosBUPWrite(SH2_struct * sh)
{
   BupHleAccess = 1;
   BiosBUPWriteBody(sh);
   BupHleAccess = 0;
}

static void BiosBUPWriteBody(SH2_struct * sh)
{
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 block;
   u32 ret;
   u32 savesize;
   u16 *blocktbl;
   u32 workaddr;
   u32 blockswritten=0;
   u32 datasize;
   u32 i;
   u8 val;

   SH2GetRegisters(sh, &sh->regs);

   LOG_BUP("BiosBUPWrite. arg1 = %d, arg2 = %08X, arg3 = %08X, arg4 = %d, PR = %08X\n", sh->regs.R[4], sh->regs.R[5], sh->regs.R[6], sh->regs.R[7], sh->regs.PR);

   // Fill in status variables
   /* ST-162 BUP_Write : BUP_NON / BUP_UNFORMAT. Ecrire dans une memoire non
    * formatee y creait un fichier sans en-tete de format. */
   ret = BupCheckDevice(sh->regs.R[4], &size, &addr, &blocksize);
   if (ret != 0)
   {
      // Error
      sh->regs.R[0] = ret;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   // See if save exists already
   if ((block = FindSave(sh, sh->regs.R[4], sh->regs.R[5], 2, size, addr, blocksize, 0)) != 0)
   {
      // save exists

      // Should we be overwriting it?
      if (sh->regs.R[7] != 0)
      {
         // Nope, let's bail instead
         sh->regs.R[0] = BUP_FOUND;
         sh->regs.PC = sh->regs.PR;
         SH2SetRegisters(sh, &sh->regs);
         return;
      }
   }

   // Let's figure out how many blocks will be needed for the save
   // Consider available size of a block as "blocksize - 6",
   // because 4 bytes at the beginning of each headers are
   // reserved to save file system, and because each blocks
   // require 2 bytes in allocation table.
   // First block doesn't requires its entry in allocation
   // table, but since allocation table ends with dummy zero
   // block ID, we can consider that data available in first
   // block is the same as in the other blocks.
   //
   // Memo when used with internal memory :
   //  - Block size is 64 bytes
   //  - datasize=  1; -> savesize:  1 block
   //  - datasize= 28; -> savesize:  1 block
   //  - datasize= 29; -> savesize:  2 blocks
   //  - datasize= 86; -> savesize:  2 blocks
   //  - datasize= 87; -> savesize:  3 blocks
   datasize = SH2MappedMemoryReadLong(sh, sh->regs.R[5]+0x1C);
   savesize = 1 + ((datasize + 0x1D) / (blocksize - 6));

   // Will it blend? Err... fit
   /* FIX : l'ancienne sauvegarde du meme nom etait effacee AVANT ce test.
    * Si la nouvelle ne tenait pas, la fonction rendait "pas assez de place"
    * (4) apres avoir detruit l'ancienne : perte de la sauvegarde existante.
    * On compte maintenant ses blocs comme liberables et on n'efface qu'une
    * fois la place garantie. */
   {
      u32 freeblocks = GetFreeSpace(sh->regs.R[4], size, addr, blocksize);
      if (block != 0)
         freeblocks += (u32)CalcSaveSize(addr, block, blocksize, size, NULL) + 1;
      if (savesize > freeblocks)
      {
         // Nope, time to bail
         sh->regs.R[0] = BUP_NOT_ENOUGH_MEMORY;
         sh->regs.PC = sh->regs.PR;
         SH2SetRegisters(sh, &sh->regs);
         return;
      }
   }

   // Delete old save (only now that the new one is known to fit)
   if (block != 0)
      DeleteSave(addr, block, blocksize);

   // Find free blocks for the save
   if ((blocktbl = GetFreeBlocks(addr, blocksize, savesize, size)) == NULL)
   {
      /* Place verifiee juste avant : seule une table de blocs endommagee
       * peut faire echouer GetFreeBlocks(). */
      sh->regs.R[0] = BUP_BROKEN;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   // Create save
   workaddr = addr + (blocktbl[0] * blocksize * 2);

   SH2MappedMemoryWriteByte(sh, workaddr+0x1, 0x80);

   // Copy over filename
   for (i = workaddr+0x9; i < ((workaddr+0x9) + (11 * 2)); i+=2)
   {
      SH2MappedMemoryWriteByte(sh, i, SH2MappedMemoryReadByte(sh, sh->regs.R[5]));
      sh->regs.R[5]++;
   }

   sh->regs.R[5]++;

   // Copy over comment
   for (i = workaddr+0x21; i < ((workaddr+0x21) + (10 * 2)); i+=2)
   {
      SH2MappedMemoryWriteByte(sh, i, SH2MappedMemoryReadByte(sh, sh->regs.R[5]));
      sh->regs.R[5]++;
   }

   // Skip comment NUL terminator (BupDir.comment[10], offset 0x16)
   sh->regs.R[5]++;

   // Copy over language (BupDir.language, offset 0x17)
   SH2MappedMemoryWriteByte(sh, workaddr+0x1F, SH2MappedMemoryReadByte(sh, sh->regs.R[5]));
   sh->regs.R[5]++;

   // Copy over date
   for (i = workaddr+0x35; i < ((workaddr+0x35) + (4 * 2)); i+=2)
   {
      SH2MappedMemoryWriteByte(sh, i, SH2MappedMemoryReadByte(sh, sh->regs.R[5]));
      sh->regs.R[5]++;
   }

   // Copy over data size
   for (i = workaddr+0x3D; i < ((workaddr+0x3D) + (4 * 2)); i+=2)
   {
      SH2MappedMemoryWriteByte(sh, i, SH2MappedMemoryReadByte(sh, sh->regs.R[5]));
      sh->regs.R[5]++;
   }

   // write the block table
   workaddr += 0x45;
   for (i = 1; i < savesize; i++)
   {
      SH2MappedMemoryWriteByte(sh, workaddr, (u8)(blocktbl[i] >> 8));
      SH2MappedMemoryWriteByte(sh, workaddr + 2, (u8)blocktbl[i]);
      LOG("write block %08X,%d,%04X", workaddr, i, blocktbl[i]);
      workaddr += 4;
      if (((workaddr-1) & ((blocksize << 1) - 1)) == 0) {
         blockswritten++;
         workaddr = addr + (blocktbl[blockswritten] * blocksize * 2) + 9;
         //LOG("BiosBUPWrite %d, block move %08X, %d/%d", __LINE__, workaddr, blockswritten, savesize);
      }
   }

   // Write 2 blank bytes so we now how large the table size is next time
   SH2MappedMemoryWriteByte(sh, workaddr, 0);
   workaddr+=2;
   if (((workaddr - 1) & ((blocksize << 1) - 1)) == 0) {
     blockswritten++;
     workaddr = addr + (blocktbl[blockswritten] * blocksize * 2) + 9;
     LOG("BiosBUPWrite %d, block move %08X, %d/%d", __LINE__, workaddr, blockswritten, savesize);
   }

   SH2MappedMemoryWriteByte(sh, workaddr, 0);
   workaddr+=2;
   if (((workaddr - 1) & ((blocksize << 1) - 1)) == 0) {
     blockswritten++;
     workaddr = addr + (blocktbl[blockswritten] * blocksize * 2) + 9;
     LOG("BiosBUPWrite %d, block move %08X, %d/%d", __LINE__, workaddr, blockswritten, savesize);
   }

   LOG("BiosBUPWrite from %08X size %08X", sh->regs.R[6], datasize);


   // Lastly, write the actual save data
   //FILE * fp = fopen("writecheck.bin", "wb");
   while (datasize > 0)
   {
      if (((workaddr-1) & ((blocksize << 1) - 1)) == 0)
      {
         // Next block
         blockswritten++;
         workaddr = addr + (blocktbl[blockswritten] * blocksize * 2) + 9;
         LOG("BiosBUPWrite %d, block move %08X, %d/%d", __LINE__, workaddr, blockswritten, savesize);
      }
      val = SH2MappedMemoryReadByte(sh, sh->regs.R[6]);
      SH2MappedMemoryWriteByte(sh, workaddr, val);

      datasize--;
      sh->regs.R[6]++;
      workaddr+=2;
   }
   //fclose(fp);
   free(blocktbl);

   YabFlushBackups();

   sh->regs.R[0] = 0; // returns 0 if there's no error
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPDelete(SH2_struct * sh)
{
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 block;
   u32 ret;

   SH2GetRegisters(sh, &sh->regs);

   LOG_BUP("BiosBUPDelete. PR = %08X\n", sh->regs.PR);

   // Fill in status variables
   /* BUP_Delete, BUP_Dir, BUP_Verify, BUP_Read : BupCheckDevice() rend
    * BUP_NON ou BUP_UNFORMAT (ST-162 2.4). Seul "non connecte" (1) etait
    * detecte ; une memoire non formatee etait parcourue comme une vraie. */
   ret = BupCheckDevice(sh->regs.R[4], &size, &addr, &blocksize);
   if (ret != 0)
   {
      // Error
      sh->regs.R[0] = ret;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   // See if save exists
   if ((block = FindSave(sh, sh->regs.R[4], sh->regs.R[5], 2, size, addr, blocksize, 0)) == 0)
   {
      // Since the save doesn't exist, let's bail with an error

      sh->regs.R[0] = BUP_NOT_FOUND;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   DeleteSave(addr, block, blocksize);

   /* FIX : l'effacement n'etait pas ecrit sur disque (seul BUP_Write
    * appelait YabFlushBackups()) : apres un arret brutal, le fichier
    * efface reapparaissait. */
   YabFlushBackups();

   sh->regs.R[0] = 0; // returns 0 if there's no error
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPDirectory(SH2_struct * sh)
{
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 ret;
   u32 i;
   char filename[12];
   u32 blockoffset=2;

   SH2GetRegisters(sh, &sh->regs);

   for (i = 0; i < 12; i++)
      filename[i] = SH2MappedMemoryReadByte(sh, sh->regs.R[5]+i);

   LOG_BUP("BiosBUPDirectory. arg1 = %d, arg2 = %s, arg3 = %08X, arg4 = %08X, PR = %08X\n", sh->regs.R[4], filename, sh->regs.R[6], sh->regs.R[7], sh->regs.PR);

   ret = BupCheckDevice(sh->regs.R[4], &size, &addr, &blocksize);

   if (ret != 0)
   {
      // Error
     sh->regs.R[0] = 0; // should be return 0 when it's failed
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   // Count Max size (a save uses at least one block)
   for (i = 0; i < (size / blocksize); i++)
   {
      u32 block = FindSave(sh, sh->regs.R[4], sh->regs.R[5], blockoffset, size, addr, blocksize, 1);

      if (block == 0)
         break;

      blockoffset = block + 1;
      block = addr + (blocksize * block * 2);
   }

   if (sh->regs.R[6] < i)
   {
      sh->regs.R[0] = -(s32)i; // returns the number of successfully read dir entries
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   // reset offet
   blockoffset = 2;

   for (i = 0; i < sh->regs.R[6]; i++)
   {
      u32 i4;
      u32 datasize=0;
      u32 block = FindSave(sh, sh->regs.R[4], sh->regs.R[5], blockoffset, size, addr, blocksize, 1);

      if (block == 0)
         break;

      blockoffset = block+1;

      // Alright, we found a match :) Time to copy over some data
      block = addr + (blocksize * block * 2);

      // Copy over filename
      for (i4 = block+0x9; i4 < ((block+0x9) + (11 * 2)); i4+=2)
      {
         SH2MappedMemoryWriteByte(sh, sh->regs.R[7], SH2MappedMemoryReadByte(sh, i4));
         sh->regs.R[7]++;
      }
      SH2MappedMemoryWriteByte(sh, sh->regs.R[7], 0);
      sh->regs.R[7]++;

      // Copy over comment
      for (i4 = block+0x21; i4 < ((block+0x21) + (10 * 2)); i4+=2)
      {
         SH2MappedMemoryWriteByte(sh, sh->regs.R[7], SH2MappedMemoryReadByte(sh, i4));
         sh->regs.R[7]++;
      }

      // Comment NUL terminator (BupDir.comment[10], offset 0x16)
      SH2MappedMemoryWriteByte(sh, sh->regs.R[7], 0);
      sh->regs.R[7]++;

      // Copy over language (BupDir.language, offset 0x17)
      SH2MappedMemoryWriteByte(sh, sh->regs.R[7], SH2MappedMemoryReadByte(sh, block+0x1F));
      sh->regs.R[7]++;

      // Copy over date
      for (i4 = block+0x35; i4 < ((block+0x35) + (4 * 2)); i4+=2)
      {
         SH2MappedMemoryWriteByte(sh, sh->regs.R[7], SH2MappedMemoryReadByte(sh, i4));
         sh->regs.R[7]++;
      }

      // Copy over data size
      for (i4 = block+0x3D; i4 < ((block+0x3D) + (4 * 2)); i4+=2)
      {
         u8 data;
         datasize <<= 8;
         data = SH2MappedMemoryReadByte(sh, i4);
         SH2MappedMemoryWriteByte(sh, sh->regs.R[7], data);
         datasize |= data;
         sh->regs.R[7]++;
      }

      // Calculate block size from the data size, and then copy it over
      SH2MappedMemoryWriteWord(sh, sh->regs.R[7], (u16)(((datasize + 0x1D) / (blocksize - 6)) + 1));
      sh->regs.R[7] += 4;
   }

   sh->regs.R[0] = i; // returns the number of successfully read dir entries
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPVerify(SH2_struct * sh)
{
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 block;
   u32 ret;
   u32 tableaddr;
   u32 datasize;
   u16 *blocktbl;
   int numblocks;
   int blocksread;

   SH2GetRegisters(sh, &sh->regs);

   LOG_BUP("BiosBUPVerify. PR = %08X\n", sh->regs.PR);

   ret = BupCheckDevice(sh->regs.R[4], &size, &addr, &blocksize);

   if (ret != 0)
   {
      // Error
      sh->regs.R[0] = ret;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   // See if save exists
   if ((block = FindSave(sh, sh->regs.R[4], sh->regs.R[5], 2, size, addr, blocksize, 0)) == 0)
   {
      // Since the save doesn't exist, let's bail with an error
      sh->regs.R[0] = BUP_NOT_FOUND; // Not found
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   tableaddr = addr + (block * blocksize * 2) + 0x3D;
   datasize = (SH2MappedMemoryReadByte(sh, tableaddr) << 24) | (SH2MappedMemoryReadByte(sh, tableaddr + 2) << 16) |
              (SH2MappedMemoryReadByte(sh, tableaddr+4) << 8) | SH2MappedMemoryReadByte(sh, tableaddr + 6);

   // Read in Block Table
   if ((blocktbl = ReadBlockTable(addr, &tableaddr, block, blocksize, size, &numblocks, &blocksread)) == NULL)
   {
      // Just return an error that might make sense
      sh->regs.R[0] = BUP_BROKEN; // Broken
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   // Now let's read in the data, and check to see if it matches
   while (datasize > 0)
   {
      if (((tableaddr-1) & ((blocksize << 1) - 1)) == 0)
      {
         // Data size larger than the block list: damaged save
         if (blocksread >= numblocks)
         {
            free(blocktbl);
            sh->regs.R[0] = BUP_BROKEN; // Broken
            sh->regs.PC = sh->regs.PR;
            SH2SetRegisters(sh, &sh->regs);
            return;
         }
         // Load up the next block
         tableaddr = addr + (blocktbl[blocksread] * blocksize * 2) + 9;
         blocksread++;
      }
      if (SH2MappedMemoryReadByte(sh, sh->regs.R[6]) != SH2MappedMemoryReadByte(sh, tableaddr))
      {
         LOG("BiosBUPVerify. failed at %08X  want = %02X get = %08X\n", tableaddr, SH2MappedMemoryReadByte(sh, sh->regs.R[6]), SH2MappedMemoryReadByte(sh, tableaddr));
         free(blocktbl);
         // Ok, the data doesn't match
         sh->regs.R[0] = BUP_NO_MATCH; // No match
         sh->regs.PC = sh->regs.PR;
         SH2SetRegisters(sh, &sh->regs);
         return;
      }

      datasize--;
      sh->regs.R[6]++;
      tableaddr+=2;
   }

   free(blocktbl);

   sh->regs.R[0] = 0; // returns 0 if there's no error
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

// Date format used by the backup library: minutes since 1980/01/01 00:00.
// Years are handled by 4 years cycles (0x5B5 days), the first year of each
// cycle being a leap year.
static void ConvertMonthAndDayMem(u32 data, u8* monthaddr, u8 * dayaddr, int leap)
{
   // Days elapsed before the start of each month
   static const u16 monthtbl[2][13] = {
      { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
      { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 } };
   int i = leap ? 1 : 0;
   int month = 1;

   // data is the day of the year, starting at 0
   while (month < 12 && data >= monthtbl[i][month])
      month++;

   *monthaddr = (u8)month;
   *dayaddr = (u8)(data - monthtbl[i][month - 1] + 1);
}

static void ConvertBupDate(u32 date, u8 *year, u8 *month, u8 *day, u8 *hour, u8 *minute, u8 *week)
{
   u32 div;
   u32 yearremainder;
   u32 yearoffset;

   *hour = (u8)((date % 0x5A0) / 0x3C);
   *minute = (u8)(date % 0x3C);

   // Days since 1980/01/01 (a tuesday)
   div = date / 0x5A0;

   // Week (sunday = 0)
   if (div > 0xAB71)
      *week = (u8)((div + 1) % 7);
   else
      *week = (u8)((div + 2) % 7);

   yearremainder = div % 0x5B5;
   if (yearremainder >= 0x16E)
   {
      // 2nd, 3rd or 4th year of the cycle (365 days)
      yearoffset = (yearremainder - 1) / 0x16D;
      ConvertMonthAndDayMem((yearremainder - 1) % 0x16D, month, day, 0);
   }
   else
   {
      // 1st year of the cycle (leap year, 366 days)
      yearoffset = 0;
      ConvertMonthAndDayMem(yearremainder, month, day, 1);
   }

   *year = (u8)(((div / 0x5B5) * 4) + yearoffset);
}


//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPGetDate(SH2_struct * sh)
{
   u8 year, month, day, hour, minute, week;

   SH2GetRegisters(sh, &sh->regs);

   LOG_BUP("BiosBUPGetDate. PR = %08X\n", sh->regs.PR);

   ConvertBupDate(sh->regs.R[4], &year, &month, &day, &hour, &minute, &week);

   // BupDate structure
   SH2MappedMemoryWriteByte(sh, sh->regs.R[5], year);
   SH2MappedMemoryWriteByte(sh, sh->regs.R[5]+1, month);
   SH2MappedMemoryWriteByte(sh, sh->regs.R[5]+2, day);
   SH2MappedMemoryWriteByte(sh, sh->regs.R[5]+3, hour);
   SH2MappedMemoryWriteByte(sh, sh->regs.R[5]+4, minute);
   SH2MappedMemoryWriteByte(sh, sh->regs.R[5]+5, week);

   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL BiosBUPSetDate(SH2_struct * sh)
{
   u32 date;
   u8 data;
   u32 remainder;
   u16 monthtbl[11] = { 31, 31+28, 31+28+31, 31+28+31+30, 31+28+31+30+31,
                        31+28+31+30+31+30, 31+28+31+30+31+30+31,
                        31+28+31+30+31+30+31+31, 31+28+31+30+31+30+31+31+30,
                        31+28+31+30+31+30+31+31+30+31,
                        31+28+31+30+31+30+31+31+30+31+30 };

   SH2GetRegisters(sh, &sh->regs);

   LOG_BUP("BiosBUPSetDate. PR = %08X\n", sh->regs.PR);

   // Year
   data = SH2MappedMemoryReadByte(sh, sh->regs.R[4]);
   date = (data / 4) * 0x5B5;
   remainder = data % 4;
   if (remainder)
      date += (remainder * 0x16D) + 1;

   // Month
   data = SH2MappedMemoryReadByte(sh, sh->regs.R[4]+1);
   if (data != 1 && data < 13)
   {
      date += monthtbl[data - 2];
      // Leap year (1st year of the cycle): add February 29th after February
      if (data > 2 && remainder == 0)
         date++;
   }

   // Day
   date += SH2MappedMemoryReadByte(sh, sh->regs.R[4]+2) - 1;
   date *= 0x5A0;

   // Hour
   date += (SH2MappedMemoryReadByte(sh, sh->regs.R[4]+3) * 0x3C);

   // Minute
   date += SH2MappedMemoryReadByte(sh, sh->regs.R[4]+4);

   sh->regs.R[0] = date;
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosHandleScuInterrupt(SH2_struct * sh, int vector)
{
   SH2GetRegisters(sh, &sh->regs);

   // Save R0-R7, PR, GBR, and old Interrupt mask to stack
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.R[0]);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.R[1]);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.R[2]);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.R[3]);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], SH2MappedMemoryReadLong(sh, 0x06000348));
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.R[4]);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.R[5]);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.R[6]);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.R[7]);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.PR);
   sh->regs.R[15] -= 4;
   SH2MappedMemoryWriteLong(sh, sh->regs.R[15], sh->regs.GBR);

   /* SR et masque SCU du facteur : table de priorite en Work RAM-H
      (6000980H + vecteur x 4), mot haut = SR, mot bas = masque etendu par
      son bit 15, comme le repartiteur du boot ROM. */
   {
      const u32 pri = SH2MappedMemoryReadLong(sh, BIOS_SCU_PRI_TABLE + (vector << 2));
      const u32 addmask = (u32)(s32)(s16)(pri & 0xFFFF);
      const u32 newmask = SH2MappedMemoryReadLong(sh, 0x06000348) | addmask;

      sh->regs.SR.all = pri >> 16;
      SH2MappedMemoryWriteLong(sh, 0x06000348, newmask);
      SH2MappedMemoryWriteLong(sh, 0x25FE00A0, newmask);
   }

   // Set PR to our Interrupt Return handler
   sh->regs.PR = 0x00000480;

   // Now execute the interrupt
   sh->regs.PC = SH2MappedMemoryReadLong(sh, 0x06000900+(vector << 2));
//   LOG("Interrupt PC = %08X. Read from %08X\n", sh->regs.PC, 0x06000900+(vector << 2));

   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL BiosHandleScuInterruptReturn(SH2_struct * sh)
{
   u32 oldmask;

   SH2GetRegisters(sh, &sh->regs);

   // Restore R0-R7, PR, GBR, and old Interrupt mask from stack
   sh->regs.GBR = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.PR = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.R[7] = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.R[6] = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.R[5] = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.R[4] = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   // Return SR back to normal
   sh->regs.SR.all = 0xF0;
   oldmask = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   SH2MappedMemoryWriteLong(sh, 0x06000348, oldmask);
   SH2MappedMemoryWriteLong(sh, 0x25FE00A0, oldmask);
   sh->regs.R[15] += 4;
   sh->regs.R[3] = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.R[2] = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.R[1] = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.R[0] = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;

   sh->regs.PC = SH2MappedMemoryReadLong(sh, sh->regs.R[15]);
   sh->regs.R[15] += 4;
   sh->regs.SR.all = SH2MappedMemoryReadLong(sh, sh->regs.R[15]) & 0x000003F3;
   sh->regs.R[15] += 4;

   SH2SetRegisters(sh, &sh->regs);
}

//////////////////////////////////////////////////////////////////////////////

int FASTCALL BiosHandleFunc(SH2_struct * sh)
{
   int addr = (sh->regs.PC & 0xFFFFF);

   /* Les points d'entree du BIOS emule (fonctions systeme, BUP, repartiteur
      d'interruptions SCU) sont tous dans la ROM BIOS : 00000000H-000FFFFFH,
      ou son miroir sans cache 20000000H-200FFFFFH (les 3 bits de poids fort
      de l'adresse SH-2 ne choisissent que le mode de cache). Cette fonction
      est appelee pour TOUTE instruction non decodee ; sans ce test, une
      instruction invalide en WORK RAM dont les 20 bits faibles valent par
      exemple 00300H (060A0300H...) etait prise pour SYS_SETSINT. */
   if ((sh->regs.PC & 0x1FF00000) != 0)
      return 0;

   SH2GetRegisters(sh, &sh->regs);
   // Let's see if it's a bios function
   switch((addr - 0x200) >> 2)
   {
      case 0x04: // 0x06000210
         BiosPowerOnMemoryClear(sh);
         break;
      case 0x1B: // 0x0600026C
         BiosExecuteCDPlayer(sh);
         break;
      case 0x1D: // 0x06000274
         BiosCheckMPEGCard(sh);
         break;
      case 0x20: // 0x06000280
         BiosChangeScuInterruptPriority(sh);
         break;
      case 0x27: // 0x0600029C
         BiosCDINIT2(sh);
         break;
      case 0x37: // 0x060002DC
         BiosCDINIT1(sh);
         break;
      case 0x40: // 0x06000300
         BiosSetScuInterrupt(sh);
         break;
      case 0x41: // 0x06000304
         BiosGetScuInterrupt(sh);
         break;
      case 0x44: // 0x06000310
         BiosSetSh2Interrupt(sh);
         break;
      case 0x45: // 0x06000314
         BiosGetSh2Interrupt(sh);
         break;
      case 0x48: // 0x06000320
         BiosChangeSystemClock(sh);
         break;
      case 0x4C: // 0x06000330
         BiosGetSemaphore(sh);
         break;
      case 0x4D: // 0x06000334
         BiosClearSemaphore(sh);
         break;
      case 0x50: // 0x06000340
         BiosSetScuInterruptMask(sh);
         break;
      case 0x51: // 0x06000344
         BiosChangeScuInterruptMask(sh);
         break;
      case 0x56: // 0x06000358
         BiosBUPInit(sh);
         break;
      case 0x60: // 0x06000380
         /* Entree 0 de la table BUP (voir BiosBUPInit) : aucune fonction
            derriere. Un simple "break" laissait le PC sur 380H et le SH-2 la
            reexecutait sans fin. On rend la main a l'appelant. */
         sh->regs.PC = sh->regs.PR;
         SH2SetRegisters(sh, &sh->regs);
         break;
      case 0x61: // 0x06000384
         BiosBUPSelectPartition(sh);
         break;
      case 0x62: // 0x06000388
         BiosBUPFormat(sh);
         break;
      case 0x63: // 0x0600038C
         BiosBUPStatus(sh);
         break;
      case 0x64: // 0x06000390
         BiosBUPWrite(sh);
         break;
      case 0x65: // 0x06000394
         BiosBUPRead(sh);
         break;
      case 0x66: // 0x06000398
         BiosBUPDelete(sh);
         break;
      case 0x67: // 0x0600039C
         BiosBUPDirectory(sh);
         break;
      case 0x68: // 0x060003A0
         BiosBUPVerify(sh);
         break;
      case 0x69: // 0x060003A4
         BiosBUPGetDate(sh);
         break;
      case 0x6A: // 0x060003A8
         BiosBUPSetDate(sh);
         break;
      case 0x6B: // 0x060003AC
         /* Derniere entree de la table BUP : meme correction que 380H. */
         sh->regs.PC = sh->regs.PR;
         SH2SetRegisters(sh, &sh->regs);
         break;
      case 0x80: // Interrupt Handler
      case 0x81:
      case 0x82:
      case 0x83:
      case 0x84:
      case 0x85:
      case 0x86:
      case 0x87:
      case 0x88:
      case 0x89:
      case 0x8A:
      case 0x8B:
      case 0x8C:
      case 0x8D:
      case 0x90:
      case 0x91:
      case 0x92:
      case 0x93:
      case 0x94:
      case 0x95:
      case 0x96:
      case 0x97:
      case 0x98:
      case 0x99:
      case 0x9A:
      case 0x9B:
      case 0x9C:
      case 0x9D:
      case 0x9E:
      case 0x9F:
         BiosHandleScuInterrupt(sh, (addr - 0x300) >> 2);
         break;
      case 0xA0: // Interrupt Handler Return
         BiosHandleScuInterruptReturn(sh);
         break;
      case 0xA8: // 0x000004A0 : fin de SYS_CHGSYSCK (attente de la commande SMPC)
         BiosChangeSystemClockWait(sh);
         break;
      case 0xAA: // 0x000004A8 : NMI du maitre
         BiosNmi(sh);
         break;
      default:
         return 0;
   }

   return 1;
}

//////////////////////////////////////////////////////////////////////////////

deviceinfo_struct *BupGetDeviceList(int *numdevices)
{
   deviceinfo_struct *device;
   int devicecount=1;

   if ((CartridgeArea->cartid & 0xF0) == 0x20)
      devicecount++;

   if ((device = (deviceinfo_struct *)malloc(devicecount * sizeof(deviceinfo_struct))) == NULL)
   {
      *numdevices = 0;
      return NULL;
   }

   *numdevices = devicecount;

   device[0].id = 0;
   sprintf(device[0].name, "Internal Backup RAM");

   if ((CartridgeArea->cartid & 0xF0) == 0x20)
   {
      device[1].id = 1;
      sprintf(device[1].name, "%d Mbit Backup RAM Cartridge", 1 << ((CartridgeArea->cartid & 0xF)+1));
   }

   // For now it's only internal backup ram and cartridge, no floppy :(
//   device[2].id = 2;
//   sprintf(device[1].name, "Floppy Disk Drive");

   return device;
}

//////////////////////////////////////////////////////////////////////////////

int BupGetStats(u32 device, u32 *freespace, u32 *maxspace)
{
   u32 ret;
   u32 size;
   u32 addr;
   u32 blocksize;

   ret = GetDeviceStats(device, &size, &addr, &blocksize);

   // Make sure there's a proper header, and return if there's any other errors
   if (ret == 1 || CheckHeader(device) != 0)
      return 0;

   *maxspace = size / blocksize;
   *freespace = GetFreeSpace(device, size, addr, blocksize);

   return 1;
}

//////////////////////////////////////////////////////////////////////////////

saveinfo_struct *BupGetSaveList(u32 device, int *numsaves)
{
   u32 ret;
   u32 size;
   u32 addr;
   u32 blocksize;
   saveinfo_struct *save;
   int savecount=0;
   u32 i, j;
   u32 workaddr;
   u32 date;

   ret = GetDeviceStats(device, &size, &addr, &blocksize);

   // Make sure there's a proper header, and return if there's any other errors
   if (ret == 1 || CheckHeader(device) != 0)
   {
      *numsaves = 0;
      return NULL;
   }

   for (i = ((2 * blocksize) << 1); i < (size << 1); i += (blocksize << 1))
   {
      // Find a block with the start of a save
      if (((s8)DMAMappedMemoryReadByte(addr + i + 1)) < 0)
         savecount++;
   }

   if ((save = (saveinfo_struct *)malloc(savecount * sizeof(saveinfo_struct))) == NULL)
   {
      *numsaves = 0;
      return NULL;
   }

   *numsaves = savecount;

   savecount = 0;

   for (i = ((2 * blocksize) << 1); i < (size << 1); i += (blocksize << 1))
   {
      // Find a block with the start of a save
      if (((s8)DMAMappedMemoryReadByte(addr + i + 1)) < 0)
      {
         workaddr = addr + i;

         // Copy over filename
         for (j = 0; j < 11; j++)
            save[savecount].filename[j] = DMAMappedMemoryReadByte(workaddr+0x9+(j * 2));
         save[savecount].filename[11] = '\0';

         // Copy over comment
         for (j = 0; j < 10; j++)
            save[savecount].comment[j] = DMAMappedMemoryReadByte(workaddr+0x21+(j * 2));
         save[savecount].comment[10] = '\0';

         // Copy over language
         save[savecount].language = DMAMappedMemoryReadByte(workaddr+0x1F);

         // Copy over Date
         date = (DMAMappedMemoryReadByte(workaddr+0x35) << 24) |
                    (DMAMappedMemoryReadByte(workaddr+0x37) << 16) |
                    (DMAMappedMemoryReadByte(workaddr+0x39) << 8) |
                    DMAMappedMemoryReadByte(workaddr+0x3B);

        save[savecount].date = date;
        ConvertBupDate(date, &save[savecount].year, &save[savecount].month, &save[savecount].day,
                       &save[savecount].hour, &save[savecount].minute, &save[savecount].week);

         // Copy over data size
         save[savecount].datasize = (DMAMappedMemoryReadByte(workaddr+0x3D) << 24) |
                                    (DMAMappedMemoryReadByte(workaddr+0x3F) << 16) |
                                    (DMAMappedMemoryReadByte(workaddr+0x41) << 8) |
                                    DMAMappedMemoryReadByte(workaddr+0x43);

         // Calculate size in blocks
         save[savecount].blocksize = CalcSaveSize(addr, i / (blocksize << 1), blocksize, size, NULL) + 1;
         savecount++;
      }
   }

   return save;
}

//////////////////////////////////////////////////////////////////////////////

int BupDeleteSave(u32 device, const char *savename)
{
   u32 ret;
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 block;

   ret = GetDeviceStats(device, &size, &addr, &blocksize);

   // Make sure there's a proper header, and return if there's any other errors
   if (ret == 1 || CheckHeader(device) != 0)
      return -1;

   // Let's find and delete the save game
   if ((block = FindSave2(device, savename, 2, size, addr, blocksize)) != 0)
   {
      // Delete old save
      DeleteSave(addr, block, blocksize);
      YabFlushBackups();
      return 0;
   }

   return -2;
}

//////////////////////////////////////////////////////////////////////////////
extern FILE * pbackup;

/* Copie de securite avant formatage. Formater efface tout : si le jeu
 * formate a tort (memoire jugee non formatee apres une corruption, ou
 * mauvais peripherique), le contenu precedent reste recuperable dans
 * "<fichier>.preformat-<date>" (memory.c, BackupDumpCopy()). Uniquement si
 * le peripherique etait formate : une memoire vide n'a rien a garder. */
static void BupKeepCopyBeforeFormat(u32 device)
{
   u32 size, addr, blocksize;
   if (CheckHeader(device) != 0)
      return;
   if (GetDeviceStats(device, &size, &addr, &blocksize) != 0)
      return;
   if (device == 0)
      BackupDumpCopy(BupRam, backup_file_size, BackupGetFileName(), "preformat");
   else if ((device == 1) && (CartridgeArea != NULL) && (CartridgeArea->bupram != NULL))
      BackupDumpCopy(CartridgeArea->bupram, 0x40000 << (CartridgeArea->cartid & 0x0F),
                     CartridgeArea->filename, "preformat");
}

void BupFormat(u32 device)
{
   BupKeepCopyBeforeFormat(device);

   switch (device)
   {
      case 0:
         FormatBackupRam(BupRam, backup_file_size);
         break;
      case 1:
         if ((CartridgeArea->cartid & 0xF0) == 0x20)
         {
            switch (CartridgeArea->cartid & 0xF)
            {
               case 1:
                  FormatBackupRam(CartridgeArea->bupram, CART_BUP4MBIT_SIZE);
                  break;
               case 2:
                  FormatBackupRam(CartridgeArea->bupram, CART_BUP8MBIT_SIZE);
                  break;
               case 3:
                  FormatBackupRam(CartridgeArea->bupram, CART_BUP16MBIT_SIZE);
                  break;
               case 4:
                  FormatBackupRam(CartridgeArea->bupram, CART_BUP32MBIT_SIZE);
                  break;
               default: break;
            }
         }
         break;
      case 2:
         LOG("Formatting FDD not supported\n");
      default: break;
   }

   /* Le formatage (BUP_Format ou interface) n'etait pas ecrit sur disque. */
   YabFlushBackups();
}

//////////////////////////////////////////////////////////////////////////////

int BupCopySave(UNUSED u32 srcdevice, UNUSED u32 dstdevice, UNUSED const char *savename)
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int BupImportSave(UNUSED u32 device, const char *filename)
{
   FILE *fp;
   long filesize;
   u8 *buffer;
   size_t num_read = 0;

   if (!filename)
      return -1;

   if ((fp = fopen(filename, "rb")) == NULL)
      return -1;

   // Calculate file size
   fseek(fp, 0, SEEK_END);
   filesize = ftell(fp);

   if (filesize <= 0)
   {
      YabSetError(YAB_ERR_FILEREAD, filename);
      fclose(fp);
      return -1;
   }

   fseek(fp, 0, SEEK_SET);

   if ((buffer = (u8 *)malloc(filesize)) == NULL)
   {
      fclose(fp);
      return -2;
   }

   num_read = fread((void *)buffer, 1, filesize, fp);
   fclose(fp);

   // Write save here

   free(buffer);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int BupExportSave(UNUSED u32 device, UNUSED const char *savename, UNUSED const char *filename)
{
   return 0;
}

int BiosBUPExport(u32 device, const char *savename, char ** buf, int * bufsize )
{
  u32 ret;
  u32 size;
  u32 addr;
  u32 blocksize;
  u32 block;
  u32 tableaddr;
  u16 *blocktbl;
  int numblocks;
  int blocksread;
  u32 datasize;
  char fname[11];
  int i;

  ret = GetDeviceStats(device, &size, &addr, &blocksize);

  // Make sure there's a proper header, and return if there's any other errors
  if (ret == 1 || CheckHeader(device) != 0)
     return -1;

  // Let's find and get the save game
  if ((block = FindSave2(device, savename, 2, size, addr, blocksize)) == 0) {
     LOG("%s is not found on this device(%d)", savename, device);
     return -1;
  }

  tableaddr = addr + (block * blocksize * 2) + 0x3D;
  datasize = (DMAMappedMemoryReadByte(tableaddr) << 24) | (DMAMappedMemoryReadByte(tableaddr + 2) << 16) |
             (DMAMappedMemoryReadByte(tableaddr+4) << 8) | DMAMappedMemoryReadByte(tableaddr + 6);

  LOG("tableaddr=%08X, datasize = %d", tableaddr, datasize );

  // Read in Block Table
  if ((blocktbl = ReadBlockTable(addr, &tableaddr, block, blocksize, size, &numblocks, &blocksread)) == NULL)
  {
    LOG("ReadBlockTable failed", tableaddr, datasize );
    return -1;
  }

  LOG("BiosBUPExport from %08X size %08X", tableaddr, datasize);
  /* datasize vient du support : une valeur corrompue demandait un malloc
   * geant. Une sauvegarde ne peut pas depasser la taille du peripherique. */
  if ((datasize == 0) || (datasize > size)) {
    LOG("BiosBUPExport: bad data size %08X", datasize);
    free(blocktbl);
    return -1;
  }
  *buf = (char*)malloc(datasize);
  if( (*buf) ==NULL ){
    LOG("Failed to allocate *buf");
    free(blocktbl);
    return -1;
  }
  *bufsize = datasize;

  //FILE * fp = fopen("savecheck.bin", "wb");
  // Now let's read in the data
  i=0;
  while (datasize > 0)
  {
     // Move to the next block only when there is still data to read
     // (as BiosBUPRead does), so blocktbl is never read past its end
     if (((tableaddr-1) & ((blocksize << 1) - 1)) == 0)
     {
        // Data size larger than the block list: damaged save
        if (blocksread >= numblocks)
        {
           free(blocktbl);
           free(*buf);
           *buf = NULL;
           *bufsize = 0;
           return -1;
        }
        // Load up the next block
        tableaddr = addr + (blocktbl[blocksread] * blocksize * 2) + 9;
        blocksread++;
     }

     (*buf)[i] = DMAMappedMemoryReadByte(tableaddr);
     datasize--;
     i++;
     tableaddr+=2;
  }
  //fclose(fp);
  free(blocktbl);
  LOG("BiosBUPExport success!");
   return 0;
}

int BiosBUPImport(u32 device, saveinfo_struct * saveinfo, const char * buf, int bufsize )
{
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 block;
   u32 ret;
   u32 savesize;
   u16 *blocktbl;
   u32 workaddr;
   u32 blockswritten=0;
   u32 datasize;
   u32 i;
   u32 rindex;

   //LOG("BiosBUPWrite. arg1 = %d, arg2 = %08X, arg3 = %08X, arg4 = %d, PR = %08X\n",
   //sh->regs.R[4], sh->regs.R[5], sh->regs.R[6], sh->regs.R[7], sh->regs.PR);

   // Fill in status variables
   ret = BupCheckDevice(device, &size, &addr, &blocksize);
   if (ret != 0)
   {
      return -1;
   }

   if (bufsize <= 0)
      return -1;

   // See if save exists already
   block = FindSave2(device, saveinfo->filename, 2, size, addr, blocksize);

   // Let's figure out how many blocks will be needed for the save
   datasize = bufsize;
   // Same computation as BiosBUPWrite (30 bytes header + 2 bytes per block
   // in the block list, 4 bytes reserved at the start of each block)
   savesize = 1 + ((datasize + 0x1D) / (blocksize - 6));

   // Will it blend? Err... fit
   /* Meme correction que BiosBUPWrite : la sauvegarde existante n'est
    * effacee qu'une fois la place de la nouvelle garantie. */
   {
      u32 freeblocks = GetFreeSpace(device, size, addr, blocksize);
      if (block != 0)
         freeblocks += (u32)CalcSaveSize(addr, block, blocksize, size, NULL) + 1;
      if (savesize > freeblocks)
      {
         // Nope, time to bail
         return BUP_NOT_ENOUGH_MEMORY;
      }
   }

   if (block != 0)
   {
      // Delete old save
      DeleteSave(addr, block, blocksize);
   }

   // Find free blocks for the save
   if ((blocktbl = GetFreeBlocks(addr, blocksize, savesize, size)) == NULL)
   {
      // Just return an error that might make sense
      return BUP_BROKEN;
   }

   // Create save
   workaddr = addr + (blocktbl[0] * blocksize * 2);

   DMAMappedMemoryWriteByte(workaddr+0x1, 0x80);

   // Copy over filename
   rindex = 0;
   for (i = workaddr+0x9; i < ((workaddr+0x9) + (11 * 2)); i+=2)
   {
      DMAMappedMemoryWriteByte(i, saveinfo->filename[rindex]);
      rindex++;
   }

   // Copy over comment
   rindex = 0;
   for (i = workaddr+0x21; i < ((workaddr+0x21) + (10 * 2)); i+=2)
   {
      DMAMappedMemoryWriteByte(i, saveinfo->comment[rindex]);
      rindex++;
   }

   // Copy over language
   DMAMappedMemoryWriteByte(workaddr+0x1F, saveinfo->language);

  // Copy over date
  DMAMappedMemoryWriteByte(workaddr+0x35, (saveinfo->date>>24)&0xFF);
  DMAMappedMemoryWriteByte(workaddr+0x37, (saveinfo->date>>16)&0xFF);
  DMAMappedMemoryWriteByte(workaddr+0x39, (saveinfo->date>>8)&0xFF);
  DMAMappedMemoryWriteByte(workaddr+0x3B, saveinfo->date&0xFF);


  // Copy over data size
  /* FIX : la taille ecrite dans l'en-tete doit etre celle des donnees
   * reellement ecrites (bufsize). saveinfo->datasize pouvait differer : la
   * lecture suivante lisait alors au-dela des blocs alloues. */
  DMAMappedMemoryWriteByte(workaddr+0x3D, (datasize>>24)&0xFF);
  DMAMappedMemoryWriteByte(workaddr+0x3F, (datasize>>16)&0xFF);
  DMAMappedMemoryWriteByte(workaddr+0x41, (datasize>>8)&0xFF);
  DMAMappedMemoryWriteByte(workaddr+0x43, datasize&0xFF);

   // write the block table
   workaddr += 0x45;

   for (i = 1; i < savesize; i++)
   {
      DMAMappedMemoryWriteByte(workaddr, (u8)(blocktbl[i] >> 8));
      workaddr+=2;
      DMAMappedMemoryWriteByte(workaddr, (u8)blocktbl[i]);
      workaddr+=2;

      if (((workaddr-1) & ((blocksize << 1) - 1)) == 0)
      {
         // Next block
         blockswritten++;
         workaddr = addr + (blocktbl[blockswritten] * blocksize * 2) + 9;
      }
   }

   // Write 2 blank bytes so we now how large the table size is next time
   DMAMappedMemoryWriteByte(workaddr, 0);
   workaddr+=2;
   if (((workaddr - 1) & ((blocksize << 1) - 1)) == 0)
   {
     // Next block
     blockswritten++;
     workaddr = addr + (blocktbl[blockswritten] * blocksize * 2) + 9;
   }

   DMAMappedMemoryWriteByte(workaddr, 0);
   workaddr+=2;
   if (((workaddr - 1) & ((blocksize << 1) - 1)) == 0)
   {
     // Next block
     blockswritten++;
     workaddr = addr + (blocktbl[blockswritten] * blocksize * 2) + 9;
   }

   LOG("BiosBUPWrite from %08X size %08X", workaddr, datasize);

   //FILE * fp = fopen("writecheck.bin","wb");
   // Lastly, write the actual save data
   rindex=0;
   while (datasize > 0)
   {
      // Move to the next block only when there is still data to write
      // (as BiosBUPWrite does), so blocktbl is never read past its end
      if (((workaddr-1) & ((blocksize << 1) - 1)) == 0)
      {
         // Next block
         blockswritten++;
         workaddr = addr + (blocktbl[blockswritten] * blocksize * 2) + 9;
      }

      DMAMappedMemoryWriteByte(workaddr, buf[rindex]);

      datasize--;
      rindex++;
      workaddr+=2;
   }
   //fclose(fp);
   free(blocktbl);
   /* L'import n'etait pas ecrit sur disque. */
   YabFlushBackups();
   return 0;
}



void FASTCALL BiosBUPRead(SH2_struct * sh)
{
  u32 size;
  u32 addr;
  u32 blocksize;
  u32 block;
  u32 ret;
  u32 tableaddr;
  u16 *blocktbl;
  int numblocks;
  int blocksread;
  u32 datasize;
  char fname[11];
  int i;
  u8 val;

  SH2GetRegisters(sh, &sh->regs);

  for (i = 0; i < 10; i++) {
    fname[i] = SH2MappedMemoryReadByte(sh, sh->regs.R[5]+i);
  }
  fname[10] = 0;

   LOG_BUP("BiosBUPRead rtn=%08X device=%d, %s, \n", sh->regs.PR, sh->regs.R[4], fname);

   ret = BupCheckDevice(sh->regs.R[4], &size, &addr, &blocksize);

   if (ret != 0)
   {
      // Error
      sh->regs.R[0] = ret;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   // See if save exists
   if ((block = FindSave(sh, sh->regs.R[4], sh->regs.R[5], 2, size, addr, blocksize, 0)) == 0)
   {
      LOG("BiosBUPRead not found");
      // save doesn't exist
      sh->regs.R[0] = BUP_NOT_FOUND;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   tableaddr = addr + (block * blocksize * 2) + 0x3D;
   datasize = (SH2MappedMemoryReadByte(sh, tableaddr) << 24) | (SH2MappedMemoryReadByte(sh, tableaddr + 2) << 16) |
              (SH2MappedMemoryReadByte(sh, tableaddr+4) << 8) | SH2MappedMemoryReadByte(sh, tableaddr + 6);

   // Read in Block Table
   if ((blocktbl = ReadBlockTable(addr, &tableaddr, block, blocksize, size, &numblocks, &blocksread)) == NULL)
   {
      // Just return an error that might make sense
      sh->regs.R[0] = BUP_BROKEN;
      sh->regs.PC = sh->regs.PR;
      SH2SetRegisters(sh, &sh->regs);
      return;
   }

   LOG("BiosBUPRead from %08X size %08X to %08X", tableaddr, datasize, sh->regs.R[6] );

   //FILE * fp = fopen("savecheck.bin", "wb");
   // Now let's read in the data
   while (datasize > 0)
   {
      if (((tableaddr-1) & ((blocksize << 1) - 1)) == 0)
      {
         // Data size larger than the block list: damaged save
         if (blocksread >= numblocks)
         {
            free(blocktbl);
            sh->regs.R[0] = BUP_BROKEN; // Broken
            sh->regs.PC = sh->regs.PR;
            SH2SetRegisters(sh, &sh->regs);
            return;
         }
         // Load up the next block
         tableaddr = addr + (blocktbl[blocksread] * blocksize * 2) + 9;
         blocksread++;
      }
      val = SH2MappedMemoryReadByte(sh, tableaddr);
      SH2MappedMemoryWriteByte(sh, sh->regs.R[6], val);
      //fputc(val,fp);
      datasize--;
      sh->regs.R[6]++;
      tableaddr+=2;

   }
   //fclose(fp);
   free(blocktbl);

   sh->regs.R[0] = 0; // returns 0 if there's no error
   sh->regs.PC = sh->regs.PR;
   SH2SetRegisters(sh, &sh->regs);
}



int BiosBUPStatusMem(SH2_struct *sh, int device, devicestatus_struct * status )
{
   u32 size;
   u32 addr;
   u32 blocksize;
   u32 ret;
   u32 freeblocks=0;
   u32 needsize;
   int aftersize;


   // Fill in status variables
   ret = GetDeviceStats(device, &size, &addr, &blocksize);

   // Make sure there's a proper header, and return if there's any other errors
   if (ret == 1 )
   {
      return -1;
   }

   freeblocks = GetFreeSpace(device, size, addr, blocksize);

   needsize = 0;
   aftersize = (((blocksize - 6) * freeblocks) - 30) - needsize;
   if (aftersize < 0) aftersize = 0;

   status->totalsize = size; // Size of Backup Ram (in bytes)
   status->totalblock = size / blocksize; // Size of Backup Ram (in blocks)
   status->blocksize = blocksize; // Size of block
   status->freesize = (((blocksize - 6) * freeblocks) > 30) ? (((blocksize - 6) * freeblocks) - 30) : 0; // Free space(in bytes)
   status->freeblock = freeblocks; // Free space(in blocks)
   status->datanum = aftersize / blocksize; // writable block size
   return 0;
}

//////////////////////////////////////////////////////////////////////////////
