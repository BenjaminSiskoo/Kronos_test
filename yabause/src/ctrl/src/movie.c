/*
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

/*! \file movie.c
    \brief Enregistreur / lecteur d'entrees (films .ymv).

    But : un testeur enregistre sa partie, envoie le fichier, et la partie
    est rejouee a l'identique (meme trame, memes entrees, meme etat) pour
    reproduire un bug.

    Format YMV2 (petit-boutiste) :
      - en-tete fixe de MOVIE_FIXED_SIZE octets (voir MovieWriteHeader()) :
        identite du jeu (IP.BIN), CRC du BIOS, reglages qui changent le
        deroulement (BIOS HLE, cache SH-2, modele de cycles VDP1, coeur SH-2,
        cartouche, PAL), memoire SMPC (langue), images compressees (PackBits)
        de la RAM de sauvegarde interne et de la cartouche de sauvegarde ;
      - puis une entree par trame, de longueur fixe frame_len :
          u8  drapeaux (reserve, 0)
          u8  taille du port 1, port_len octets de PORTDATA1.data
          u8  taille du port 2, port_len octets de PORTDATA2.data
          u32 empreinte de l'etat de la machine au debut de la trame (Work RAM
              haute et basse, registres des deux SH-2)
    L'empreinte permet de dire, a la lecture, a quelle trame exacte la
    partie diverge de celle du testeur.

    Corrections par rapport a l'ancien movie.c de Yabause :
      - ReadMovieInState() ecrivait &(tempbuffer.data), l'adresse du
        pointeur et la pile qui suit, au lieu des donnees du film (meme
        erreur dans SaveMovieInState() et TestWrite()) : charger un etat
        pendant un enregistrement corrompait le fichier ;
      - ReadHeader() lisait le nombre de "rerecords" a l'offset fixe 172 alors
        que WriteHeader() ne l'y ecrivait pas (les champs avant lui ont des
        tailles variables selon VERSION) ;
      - le resultat de fread() n'etait jamais verifie : un film tronque
        donnait des entrees au hasard ;
      - SaveMovie()/PlayMovie() copiaient le nom du fichier dans un tampon de
        1024 octets jamais libere (et debordaient avec un chemin plus long) ;
      - TruncateMovie() n'etait pas ecrit (reprise d'enregistrement apres un
        chargement d'etat : la fin de l'ancien film restait dans le fichier) ;
      - MovieReadState() lisait le bloc du film meme quand l'etat n'en
        contenait pas (lecture hors du tampon).
*/

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "peripheral.h"
#include "scsp.h"
#include "movie.h"
#include "cs0.h"
#include "cs2.h"
#include "vdp1.h"
#include "vdp2.h"
#include "smpc.h"
#include "sh2core.h"
#include "memory.h"
#include "yabause.h"
#include "yui.h"
#include "osdcore.h"
#include "error.h"

extern u32 backup_file_size;   /* memory.c */

struct MovieStruct Movie;

char MovieStatus[40];
char InputDisplayString[40];

/* Compteurs (framecounter est aussi utilise par smpc.c pour l'horloge du
   film et range dans les etats de sauvegarde par memory.c). */
int framecounter;
int lagframecounter;
int LagFrameFlag;
int FrameAdvanceVariable = 0;

//////////////////////////////////////////////////////////////////////////////
// Format
//////////////////////////////////////////////////////////////////////////////

#define MOVIE_MAGIC        "YMV2"
#define MOVIE_FORMAT       2
#define MOVIE_FIXED_SIZE   512
#define MOVIE_STATE_MAGIC  0x53564D59u /* "YMVS" */
#define MOVIE_MIN_PORT_LEN 16
#define MOVIE_MAX_PORT_LEN 255

/* Offsets des champs reecrits a l'arret de l'enregistrement. */
#define HDR_OFS_FRAMES     20
#define HDR_OFS_RERECORDS  24

typedef struct
{
   u32 header_size;
   u32 frame_len;
   u32 port_len;
   u32 frames;
   u32 rerecords;
   u32 flags;
   char kronos_version[32];
   char itemnum[11];
   char version[7];
   char date[11];
   char cdinfo[9];
   char region[11];
   char gamename[113];
   u32 bios_crc;
   u32 emulatebios;
   u32 usequickload;
   u32 ispal;
   u32 regionid;
   u32 languageid;
   u32 usecache;
   u32 vdp1cycles;
   u32 sh2core;
   u32 carttype;
   u32 sh2steprun;
   u8  smem[4];
   u32 record_time;
   u32 bup_size, bup_crc, bup_packed;
   u32 cbup_size, cbup_crc, cbup_packed;
} MovieHeader;

static MovieHeader hdr;
static char *movie_filename = NULL;
static int movie_starting = 0;       /* SaveMovie()/PlayMovie() en cours */
static int movie_desync = -1;
static int movie_ended = 0;
static int movie_port_overflow = 0;
static char movie_report[2048];

/* Entrees de l'utilisateur, remises en place a la fin d'une lecture. */
static PortData_struct movie_saved_port1, movie_saved_port2;
static int movie_ports_saved = 0;

/* RAM de sauvegarde de l'utilisateur, mise de cote pendant la lecture. */
static u8 *bup_user = NULL;
static u32 bup_user_size = 0;
static u8 *cbup_user = NULL;
static u32 cbup_user_size = 0;
static int bup_swapped = 0;

//////////////////////////////////////////////////////////////////////////////
// Outils
//////////////////////////////////////////////////////////////////////////////

static void MovieReport(const char *fmt, ...)
{
   va_list ap;
   size_t len = strlen(movie_report);
   if (len >= sizeof(movie_report) - 2)
      return;
   va_start(ap, fmt);
   vsnprintf(movie_report + len, sizeof(movie_report) - len - 1, fmt, ap);
   va_end(ap);
   len = strlen(movie_report);
   movie_report[len] = '\n';
   movie_report[len + 1] = 0;
}

static void MovieMessage(const char *fmt, ...)
{
   char msg[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   YuiMsg("Movie: %s\n", msg);
   OSDPushMessage(OSDMSG_STATUS, 180, "%s", msg);
}

static u32 crc_table[256];
static int crc_table_ready = 0;

static u32 MovieCrc32(const u8 *p, u32 len)
{
   u32 c = 0xFFFFFFFFu;
   u32 i;
   if (!crc_table_ready) {
      u32 n, k;
      for (n = 0; n < 256; n++) {
         c = n;
         for (k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
         crc_table[n] = c;
      }
      crc_table_ready = 1;
      c = 0xFFFFFFFFu;
   }
   if (p == NULL)
      return 0;
   for (i = 0; i < len; i++)
      c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
   return c ^ 0xFFFFFFFFu;
}

/* Empreinte 64 bits rapide (mots de 64 bits, ~0,3 ms pour 2 Mo). */
static void MovieHashBlock(u64 *h, const u8 *p, u32 len)
{
   u64 x = *h;
   u32 i = 0;
   if (p == NULL)
      return;
   for (; i + 8 <= len; i += 8) {
      u64 w;
      memcpy(&w, p + i, 8);
      x ^= w;
      x *= 0x9E3779B97F4A7C15ULL;
      x ^= x >> 29;
   }
   for (; i < len; i++) {
      x ^= p[i];
      x *= 0x100000001B3ULL;
   }
   *h = x;
}

static u32 MovieStateHash(void)
{
   u64 h = 0xCBF29CE484222325ULL;
   sh2regs_struct regs;

   MovieHashBlock(&h, HighWram, 0x100000);
   MovieHashBlock(&h, LowWram, 0x100000);
   if (MSH2 != NULL && SH2Core != NULL) {
      memset(&regs, 0, sizeof(regs));
      SH2GetRegisters(MSH2, &regs);
      MovieHashBlock(&h, (const u8 *)&regs, sizeof(regs));
   }
   if (SSH2 != NULL && SH2Core != NULL && yabsys.IsSSH2Running) {
      memset(&regs, 0, sizeof(regs));
      SH2GetRegisters(SSH2, &regs);
      MovieHashBlock(&h, (const u8 *)&regs, sizeof(regs));
   }
   return (u32)(h ^ (h >> 32));
}

/* PackBits : n < 128 -> n+1 octets litteraux ; n >= 128 -> octet suivant
   repete 257-n fois (2..129). */
static u32 MoviePack(const u8 *src, u32 len, u8 *dst)
{
   u32 i = 0, o = 0;
   while (i < len) {
      u32 run = 1;
      while (i + run < len && run < 129 && src[i + run] == src[i])
         run++;
      if (run >= 2) {
         dst[o++] = (u8)(257 - run);
         dst[o++] = src[i];
         i += run;
      } else {
         u32 start = i, lit = 0;
         while (i < len && lit < 128) {
            if (i + 1 < len && src[i + 1] == src[i])
               break;
            i++;
            lit++;
         }
         if (lit == 0) { i++; lit = 1; }
         dst[o++] = (u8)(lit - 1);
         memcpy(dst + o, src + start, lit);
         o += lit;
      }
   }
   return o;
}

static int MovieUnpack(const u8 *src, u32 len, u8 *dst, u32 dstlen)
{
   u32 i = 0, o = 0;
   while (i < len) {
      u8 n = src[i++];
      if (n < 128) {
         u32 lit = (u32)n + 1;
         if (i + lit > len || o + lit > dstlen)
            return -1;
         memcpy(dst + o, src + i, lit);
         i += lit;
         o += lit;
      } else {
         u32 run = 257 - (u32)n;
         if (i >= len || o + run > dstlen)
            return -1;
         memset(dst + o, src[i++], run);
         o += run;
      }
   }
   return (o == dstlen) ? 0 : -1;
}

static void put32(u8 *p, u32 v)
{
   p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 get32(const u8 *p)
{
   return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 CartBupSize(void)
{
   if (CartridgeArea == NULL || CartridgeArea->bupram == NULL)
      return 0;
   switch (CartridgeArea->carttype) {
      case CART_BACKUPRAM4MBIT:  return CART_BUP4MBIT_SIZE;
      case CART_BACKUPRAM8MBIT:  return CART_BUP8MBIT_SIZE;
      case CART_BACKUPRAM16MBIT: return CART_BUP16MBIT_SIZE;
      case CART_BACKUPRAM32MBIT: return CART_BUP32MBIT_SIZE;
      default: return 0;
   }
}

static u32 CartDramSize(void)
{
   if (CartridgeArea == NULL || CartridgeArea->dram == NULL)
      return 0;
   switch (CartridgeArea->carttype) {
      case CART_DRAM8MBIT:   return 0x100000;
      case CART_DRAM32MBIT:  return 0x400000;
      case CART_PAR:         return 0x400000;
      case CART_DRAM128MBIT: return CART_DRAM128MBIT_SIZE;
      default: return 0;
   }
}

static void CopyField(char *dst, size_t dstlen, const char *src)
{
   memset(dst, 0, dstlen);
   if (src != NULL)
      strncpy(dst, src, dstlen - 1);
}

static void TrimCopy(char *dst, size_t dstlen, const char *src)
{
   size_t n;
   CopyField(dst, dstlen, src);
   n = strlen(dst);
   while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\r' || dst[n - 1] == '\n'))
      dst[--n] = 0;
}

//////////////////////////////////////////////////////////////////////////////
// Mode deterministe
//////////////////////////////////////////////////////////////////////////////

/* Les entrees de l'hote ne doivent changer qu'en un seul point de la
   trame (DoMovie()). PERCore->HandleEvents() est aussi appele en pleine
   trame par l'INTBACK du SMPC, la lecture directe des ports et le pistolet
   (smpc.c, vdp2.c) : pendant un film, ces appels passent par une copie de
   l'interface dont HandleEvents ne fait rien, et DoMovie() appelle le vrai
   une fois par trame. */
static PerInterface_struct movie_percore;
static PerInterface_struct *movie_real_percore = NULL;

static int MovieNoHandleEvents(void)
{
   return 0;
}

static void MovieWrapPerCore(void)
{
   if (PERCore == NULL || PERCore == &movie_percore)
      return;
   movie_real_percore = PERCore;
   movie_percore = *PERCore;
   movie_percore.HandleEvents = MovieNoHandleEvents;
   PERCore = &movie_percore;
}

static void MovieUnwrapPerCore(void)
{
   if (PERCore == &movie_percore)
      PERCore = movie_real_percore;
   movie_real_percore = NULL;
}

/* Ce que YabauseResetNoLoad() laisse en l'etat (il remet deja a zero les
   Work RAM, la VRAM VDP1/VDP2, la CRAM et la Sound RAM) : la DRAM d'une
   cartouche d'extension, que le materiel ne vide pas non plus a un reset,
   et le compteur de trames (phase du saut d'images). On part d'une machine
   identique a la mise sous tension (T1MemoryInit = calloc). */
static void MovieClearMachine(void)
{
   u32 dsize = CartDramSize();
   if (dsize)
      memset(CartridgeArea->dram, 0, dsize);
   yabsys.frame_count = 0;
}

static void MovieEnterDeterministic(void)
{
   MovieWrapPerCore();
   ScspSetStrictSync(1);
}

static void MovieLeaveDeterministic(void)
{
   ScspSetStrictSync(0);
   MovieUnwrapPerCore();
}

//////////////////////////////////////////////////////////////////////////////
// RAM de sauvegarde
//////////////////////////////////////////////////////////////////////////////

static void MovieRestoreUserBackup(void)
{
   if (!bup_swapped)
      return;
   if (bup_user != NULL && BupRam != NULL && bup_user_size == backup_file_size)
      memcpy(BupRam, bup_user, bup_user_size);
   if (cbup_user != NULL && cbup_user_size != 0 && cbup_user_size == CartBupSize())
      memcpy(CartridgeArea->bupram, cbup_user, cbup_user_size);
   free(bup_user);
   free(cbup_user);
   bup_user = cbup_user = NULL;
   bup_user_size = cbup_user_size = 0;
   bup_swapped = 0;
   YuiMsg("Movie: backup RAM of the user restored\n");
}

/* Met de cote la RAM de sauvegarde de l'utilisateur. Elle sera remise en
   place par StopMovie() ou par un reset ; d'ici la, rien n'est ecrit sur le
   disque (MovieBackupLocked()). */
static int MovieSaveUserBackup(void)
{
   u32 csize = CartBupSize();
   MovieRestoreUserBackup();
   if (BupRam != NULL && backup_file_size != 0) {
      bup_user = (u8 *)malloc(backup_file_size);
      if (bup_user == NULL)
         return -1;
      memcpy(bup_user, BupRam, backup_file_size);
      bup_user_size = backup_file_size;
   }
   if (csize != 0) {
      cbup_user = (u8 *)malloc(csize);
      if (cbup_user == NULL) {
         free(bup_user);
         bup_user = NULL;
         return -1;
      }
      memcpy(cbup_user, CartridgeArea->bupram, csize);
      cbup_user_size = csize;
   }
   bup_swapped = 1;
   return 0;
}

int MovieBackupLocked(void)
{
   return bup_swapped || (Movie.Status == Playback);
}

//////////////////////////////////////////////////////////////////////////////
// En-tete
//////////////////////////////////////////////////////////////////////////////

static void MovieFillHeaderFromMachine(MovieHeader *h)
{
   memset(h, 0, sizeof(*h));
   CopyField(h->kronos_version, sizeof(h->kronos_version), VERSION);
   if (cdip != NULL) {
      CopyField(h->itemnum, sizeof(h->itemnum), cdip->itemnum);
      CopyField(h->version, sizeof(h->version), cdip->version);
      CopyField(h->date, sizeof(h->date), cdip->date);
      CopyField(h->cdinfo, sizeof(h->cdinfo), cdip->cdinfo);
      CopyField(h->region, sizeof(h->region), cdip->region);
      CopyField(h->gamename, sizeof(h->gamename), cdip->gamename);
   }
   h->bios_crc = (!yabsys.emulatebios && BiosRom != NULL) ? MovieCrc32(BiosRom, 0x80000) : 0;
   h->emulatebios = (u32)yabsys.emulatebios;
   h->usequickload = (u32)yabsys.usequickload;
   h->ispal = (u32)yabsys.IsPal;
   h->regionid = SmpcInternalVars ? SmpcInternalVars->regionid : 0;
   h->languageid = SmpcInternalVars ? SmpcInternalVars->languageid : 0;
   if (SmpcInternalVars)
      memcpy(h->smem, SmpcInternalVars->SMEM, 4);
   h->usecache = (u32)yabsys.usecache;
   h->vdp1cycles = (u32)yabsys.vdp1cycles;
   h->sh2core = SH2Core ? (u32)SH2Core->id : 0;
   h->carttype = CartridgeArea ? (u32)CartridgeArea->carttype : 0;
   h->sh2steprun = (u32)yabsys.sh2StepRun;
   h->record_time = (u32)time(NULL);
   h->flags = 1;
}

/* Ecrit l'en-tete complet (avec les images de sauvegarde) en debut de
   fichier ; renvoie 0 ou -1. */
static int MovieWriteHeader(FILE *fp, MovieHeader *h)
{
   u8 fixed[MOVIE_FIXED_SIZE];
   u8 *p = fixed;
   u8 *bup_packed = NULL, *cbup_packed = NULL;
   u32 csize = CartBupSize();
   u32 total;

   h->bup_size = h->bup_crc = h->bup_packed = 0;
   h->cbup_size = h->cbup_crc = h->cbup_packed = 0;

   if (BupRam != NULL && backup_file_size != 0) {
      bup_packed = (u8 *)malloc(backup_file_size + backup_file_size / 128 + 16);
      if (bup_packed == NULL)
         return -1;
      h->bup_size = backup_file_size;
      h->bup_crc = MovieCrc32(BupRam, backup_file_size);
      h->bup_packed = MoviePack(BupRam, backup_file_size, bup_packed);
   }
   if (csize != 0) {
      cbup_packed = (u8 *)malloc(csize + csize / 128 + 16);
      if (cbup_packed == NULL) {
         free(bup_packed);
         return -1;
      }
      h->cbup_size = csize;
      h->cbup_crc = MovieCrc32(CartridgeArea->bupram, csize);
      h->cbup_packed = MoviePack(CartridgeArea->bupram, csize, cbup_packed);
   }

   total = MOVIE_FIXED_SIZE + h->bup_packed + h->cbup_packed;
   h->header_size = (total + 15) & ~15u;

   memset(fixed, 0, sizeof(fixed));
   memcpy(p, MOVIE_MAGIC, 4);              p += 4;
   put32(p, MOVIE_FORMAT);                 p += 4;
   put32(p, h->header_size);               p += 4;
   put32(p, h->frame_len);                 p += 4;
   put32(p, h->port_len);                  p += 4;
   put32(p, h->frames);                    p += 4;   /* HDR_OFS_FRAMES */
   put32(p, h->rerecords);                 p += 4;   /* HDR_OFS_RERECORDS */
   put32(p, h->flags);                     p += 4;
   memcpy(p, h->kronos_version, 32);       p += 32;
   memcpy(p, h->itemnum, 11);              p += 11;
   memcpy(p, h->version, 7);               p += 7;
   memcpy(p, h->date, 11);                 p += 11;
   memcpy(p, h->cdinfo, 9);                p += 9;
   memcpy(p, h->region, 11);               p += 11;
   memcpy(p, h->gamename, 113);            p += 113;
   put32(p, h->bios_crc);                  p += 4;
   put32(p, h->emulatebios);               p += 4;
   put32(p, h->usequickload);              p += 4;
   put32(p, h->ispal);                     p += 4;
   put32(p, h->regionid);                  p += 4;
   put32(p, h->languageid);                p += 4;
   put32(p, h->usecache);                  p += 4;
   put32(p, h->vdp1cycles);                p += 4;
   put32(p, h->sh2core);                   p += 4;
   put32(p, h->carttype);                  p += 4;
   put32(p, h->sh2steprun);                p += 4;
   memcpy(p, h->smem, 4);                  p += 4;
   put32(p, h->record_time);               p += 4;
   put32(p, h->bup_size);                  p += 4;
   put32(p, h->bup_crc);                   p += 4;
   put32(p, h->bup_packed);                p += 4;
   put32(p, h->cbup_size);                 p += 4;
   put32(p, h->cbup_crc);                  p += 4;
   put32(p, h->cbup_packed);               p += 4;

   fseek(fp, 0, SEEK_SET);
   if (fwrite(fixed, 1, MOVIE_FIXED_SIZE, fp) != MOVIE_FIXED_SIZE)
      goto fail;
   if (h->bup_packed && fwrite(bup_packed, 1, h->bup_packed, fp) != h->bup_packed)
      goto fail;
   if (h->cbup_packed && fwrite(cbup_packed, 1, h->cbup_packed, fp) != h->cbup_packed)
      goto fail;
   while (total < h->header_size) {
      fputc(0, fp);
      total++;
   }
   free(bup_packed);
   free(cbup_packed);
   fflush(fp);
   return 0;

fail:
   free(bup_packed);
   free(cbup_packed);
   return -1;
}

/* Lit l'en-tete fixe ; renvoie 0 ou -1. */
static int MovieReadHeader(FILE *fp, MovieHeader *h)
{
   u8 fixed[MOVIE_FIXED_SIZE];
   const u8 *p = fixed;

   memset(h, 0, sizeof(*h));
   fseek(fp, 0, SEEK_SET);
   if (fread(fixed, 1, MOVIE_FIXED_SIZE, fp) != MOVIE_FIXED_SIZE)
      return -1;
   if (memcmp(p, MOVIE_MAGIC, 4) != 0)
      return -1;
   p += 4;
   if (get32(p) != MOVIE_FORMAT)
      return -1;
   p += 4;
   h->header_size = get32(p);  p += 4;
   h->frame_len = get32(p);    p += 4;
   h->port_len = get32(p);     p += 4;
   h->frames = get32(p);       p += 4;
   h->rerecords = get32(p);    p += 4;
   h->flags = get32(p);        p += 4;
   memcpy(h->kronos_version, p, 32); h->kronos_version[31] = 0; p += 32;
   memcpy(h->itemnum, p, 11);  h->itemnum[10] = 0;  p += 11;
   memcpy(h->version, p, 7);   h->version[6] = 0;   p += 7;
   memcpy(h->date, p, 11);     h->date[10] = 0;     p += 11;
   memcpy(h->cdinfo, p, 9);    h->cdinfo[8] = 0;    p += 9;
   memcpy(h->region, p, 11);   h->region[10] = 0;   p += 11;
   memcpy(h->gamename, p, 113); h->gamename[112] = 0; p += 113;
   h->bios_crc = get32(p);     p += 4;
   h->emulatebios = get32(p);  p += 4;
   h->usequickload = get32(p); p += 4;
   h->ispal = get32(p);        p += 4;
   h->regionid = get32(p);     p += 4;
   h->languageid = get32(p);   p += 4;
   h->usecache = get32(p);     p += 4;
   h->vdp1cycles = get32(p);   p += 4;
   h->sh2core = get32(p);      p += 4;
   h->carttype = get32(p);     p += 4;
   h->sh2steprun = get32(p);   p += 4;
   memcpy(h->smem, p, 4);      p += 4;
   h->record_time = get32(p);  p += 4;
   h->bup_size = get32(p);     p += 4;
   h->bup_crc = get32(p);      p += 4;
   h->bup_packed = get32(p);   p += 4;
   h->cbup_size = get32(p);    p += 4;
   h->cbup_crc = get32(p);     p += 4;
   h->cbup_packed = get32(p);  p += 4;

   if (h->port_len < 4 || h->port_len > MOVIE_MAX_PORT_LEN)
      return -1;
   if (h->frame_len != 1 + 2 * (1 + h->port_len) + 4)
      return -1;
   if (h->header_size < MOVIE_FIXED_SIZE + h->bup_packed + h->cbup_packed)
      return -1;
   return 0;
}

/* Charge une image compressee de l'en-tete ; renvoie un tampon a liberer
   ou NULL. */
static u8 *MovieLoadImage(FILE *fp, u32 offset, u32 packed, u32 size, u32 crc)
{
   u8 *src, *dst;
   if (packed == 0 || size == 0)
      return NULL;
   src = (u8 *)malloc(packed);
   dst = (u8 *)malloc(size);
   if (src == NULL || dst == NULL)
      goto fail;
   fseek(fp, (long)offset, SEEK_SET);
   if (fread(src, 1, packed, fp) != packed)
      goto fail;
   if (MovieUnpack(src, packed, dst, size) != 0)
      goto fail;
   if (MovieCrc32(dst, size) != crc)
      goto fail;
   free(src);
   return dst;
fail:
   free(src);
   free(dst);
   return NULL;
}

/* Compare le film a la machine. Renvoie -1 si le jeu n'est pas le meme. */
static int MovieCheckHeader(const MovieHeader *h)
{
   MovieHeader cur;
   char a[128], b[128];
   int fatal = 0;

   MovieFillHeaderFromMachine(&cur);

   TrimCopy(a, sizeof(a), h->itemnum);
   TrimCopy(b, sizeof(b), cur.itemnum);
   if (strcmp(a, b) != 0) {
      MovieReport(_("Different game: movie = %s, loaded = %s"), a, b);
      fatal = 1;
   }
   TrimCopy(a, sizeof(a), h->version);
   TrimCopy(b, sizeof(b), cur.version);
   if (strcmp(a, b) != 0)
      MovieReport(_("Different game version: movie = %s, loaded = %s"), a, b);
   TrimCopy(a, sizeof(a), h->date);
   TrimCopy(b, sizeof(b), cur.date);
   if (strcmp(a, b) != 0)
      MovieReport(_("Different disc date: movie = %s, loaded = %s"), a, b);

   if (h->ispal != cur.ispal)
      MovieReport(_("Different PAL/NTSC setting (movie: %s)"), h->ispal ? "PAL" : "NTSC");
   if (h->regionid != cur.regionid)
      MovieReport(_("Different console region: movie = %u, current = %u"), h->regionid, cur.regionid);
   if (h->emulatebios != cur.emulatebios)
      MovieReport(_("BIOS: movie = %s, current = %s"), h->emulatebios ? _("emulated") : _("real"), cur.emulatebios ? _("emulated") : _("real"));
   else if (!h->emulatebios && h->bios_crc != cur.bios_crc)
      MovieReport(_("Different BIOS image (CRC %08X, current %08X)"), h->bios_crc, cur.bios_crc);
   if (h->usequickload != cur.usequickload)
      MovieReport(_("Different 'quick load' setting (movie: %u)"), h->usequickload);
   if (h->usecache != cur.usecache)
      MovieReport(_("Different SH-2 cache emulation (movie: %u)"), h->usecache);
   if (h->vdp1cycles != cur.vdp1cycles)
      MovieReport(_("Different VDP1 cycle model (movie: %u)"), h->vdp1cycles);
   if (h->sh2core != cur.sh2core)
      MovieReport(_("Different SH-2 core (movie: %u, current: %u)"), h->sh2core, cur.sh2core);
   if (h->carttype != cur.carttype)
      MovieReport(_("Different cartridge (movie: type %u, current: type %u)"), h->carttype, cur.carttype);
   if (h->sh2steprun != cur.sh2steprun)
      MovieReport(_("Different SH-2 step setting (movie: %u)"), h->sh2steprun);
   TrimCopy(a, sizeof(a), h->kronos_version);
   if (strcmp(a, VERSION) != 0)
      MovieReport(_("Movie recorded with Kronos %s (current: %s)"), a, VERSION);

   return fatal ? -1 : 0;
}

/* Remplace la RAM de sauvegarde par celle du film (apres
   MovieSaveUserBackup()). */
static void MovieApplyBackupImages(FILE *fp, const MovieHeader *h)
{
   u8 *img;
   u32 csize = CartBupSize();

   if (h->bup_size != 0) {
      if (BupRam == NULL || h->bup_size != backup_file_size) {
         MovieReport(_("Internal backup RAM: different size (movie %u, current %u), not loaded"),
                     h->bup_size, backup_file_size);
      } else {
         img = MovieLoadImage(fp, MOVIE_FIXED_SIZE, h->bup_packed, h->bup_size, h->bup_crc);
         if (img != NULL) {
            memcpy(BupRam, img, h->bup_size);
            free(img);
         } else
            MovieReport(_("The movie's internal backup RAM is unreadable"));
      }
   }
   if (h->cbup_size != 0) {
      if (csize != h->cbup_size) {
         MovieReport(_("Backup cartridge: different size (movie %u, current %u), not loaded"),
                     h->cbup_size, csize);
      } else {
         img = MovieLoadImage(fp, MOVIE_FIXED_SIZE + h->bup_packed, h->cbup_packed, h->cbup_size, h->cbup_crc);
         if (img != NULL) {
            memcpy(CartridgeArea->bupram, img, h->cbup_size);
            free(img);
         } else
            MovieReport(_("The movie's backup cartridge is unreadable"));
      }
   } else if (csize != 0) {
      MovieReport(_("The movie has no backup cartridge: its current content is used"));
   }
}

//////////////////////////////////////////////////////////////////////////////
// Trames
//////////////////////////////////////////////////////////////////////////////

static void SetInputDisplayCharacters(void)
{
   static const char *Buttons[8]  = {"B", "C", "A", "S", "U", "D", "R", "L"};
   static const char *Buttons2[8] = {"", "", "", "L", "Z", "Y", "X", "R"};
   char str[40];
   int x;

   str[0] = 0;
   for (x = 0; x < 8; x++)
      strcat(str, (PORTDATA1.data[2] & (1 << x)) ? " " : Buttons[x]);
   for (x = 3; x < 8; x++)
      strcat(str, (PORTDATA1.data[3] & (1 << x)) ? " " : Buttons2[x]);
   strcpy(InputDisplayString, str);
}

static void MovieWritePort(u8 *p, const PortData_struct *port)
{
   int size = port->size;
   if (size < 0)
      size = 0;
   if ((u32)size > hdr.port_len) {
      if (!movie_port_overflow) {
         MovieMessage(_("Peripheral too large for the movie (%d > %u bytes)"), size, hdr.port_len);
         movie_port_overflow = 1;
      }
      size = (int)hdr.port_len;
   }
   p[0] = (u8)size;
   memcpy(p + 1, port->data, hdr.port_len);
}

static void MovieReadPort(const u8 *p, PortData_struct *port)
{
   port->size = p[0];
   memcpy(port->data, p + 1, hdr.port_len);
}

static void MovieFinishPlayback(void)
{
   if (Movie.fp)
      fclose(Movie.fp);
   Movie.fp = NULL;
   Movie.Status = Stopped;
   MovieLeaveDeterministic();
   if (movie_ports_saved) {
      PORTDATA1 = movie_saved_port1;
      PORTDATA2 = movie_saved_port2;
      movie_ports_saved = 0;
   }
   strcpy(MovieStatus, "Playback Stopped");
}

void DoMovie(void)
{
   u8 rec[1 + 2 * (1 + MOVIE_MAX_PORT_LEN) + 4];
   u32 index, hash;

   if (LagFrameFlag == 1)
      lagframecounter++;
   framecounter++;

   if (Movie.Status != Recording && Movie.Status != Playback)
      return;

   /* Trame dont on traite l'entree (0 = premiere trame apres le reset). */
   index = (u32)(framecounter - 1);
   LagFrameFlag = 1;

   /* PerInit() a pu remettre une autre interface pendant le film. */
   MovieWrapPerCore();

   hash = MovieStateHash();

   if (Movie.Status == Recording) {
      /* Seul point de la trame ou l'hote peut changer les entrees. */
      if (movie_real_percore != NULL && movie_real_percore->HandleEvents != NULL)
         movie_real_percore->HandleEvents();
      /* Computed peripheral outputs (SBom Joycard, keyboard code stream):
         produced here, once per frame, so that the recorded port data are
         what the game reads. PerKeyboardUpdate() leaves the port data
         alone during a movie (peripheral.c). */
      PerMovieRecordUpdate();

      rec[0] = 0;
      MovieWritePort(rec + 1, &PORTDATA1);
      MovieWritePort(rec + 2 + hdr.port_len, &PORTDATA2);
      put32(rec + hdr.frame_len - 4, hash);
      fseek(Movie.fp, (long)(hdr.header_size + (u64)index * hdr.frame_len), SEEK_SET);
      if (fwrite(rec, 1, hdr.frame_len, Movie.fp) != hdr.frame_len) {
         MovieMessage(_("Movie write error, recording stopped"));
         StopMovie();
         return;
      }
      /* Le film doit survivre a un plantage de l'emulateur. */
      fflush(Movie.fp);
      Movie.Frames = (int)index + 1;
      SetInputDisplayCharacters();
      return;
   }

   /* Lecture */
   if ((int)index >= Movie.Frames) {
      MovieFinishPlayback();
      movie_ended = 1;
      if (movie_desync >= 0)
         MovieMessage(_("End of movie (%d frames) - DESYNC at frame %d"), Movie.Frames, movie_desync);
      else
         MovieMessage(_("End of movie (%d frames), in sync"), Movie.Frames);
      return;
   }
   fseek(Movie.fp, (long)(hdr.header_size + (u64)index * hdr.frame_len), SEEK_SET);
   if (fread(rec, 1, hdr.frame_len, Movie.fp) != hdr.frame_len) {
      MovieFinishPlayback();
      movie_ended = 1;
      MovieMessage(_("Movie truncated at frame %u, playback stopped"), index);
      return;
   }
   MovieReadPort(rec + 1, &PORTDATA1);
   MovieReadPort(rec + 2 + hdr.port_len, &PORTDATA2);
   SetInputDisplayCharacters();

   if ((hdr.flags & 1) && movie_desync < 0 && get32(rec + hdr.frame_len - 4) != hash) {
      movie_desync = (int)index;
      MovieMessage(_("DESYNC at frame %u (diverged during frame %d)"),
                   index, (int)index - 1);
   }
   /* La lecture s'arrete au debut de la trame qui suit la derniere entree
      (test en tete de la fonction). */
}

//////////////////////////////////////////////////////////////////////////////
// Demarrage / arret
//////////////////////////////////////////////////////////////////////////////

static void MovieSetFilename(const char *filename)
{
   free(movie_filename);
   movie_filename = filename ? strdup(filename) : NULL;
   Movie.filename = movie_filename;
}

void StopMovie(void)
{
   if (Movie.Status == Recording && Movie.fp != NULL) {
      u8 b[4];
      put32(b, (u32)Movie.Frames);
      fseek(Movie.fp, HDR_OFS_FRAMES, SEEK_SET);
      fwrite(b, 1, 4, Movie.fp);
      put32(b, (u32)Movie.Rerecords);
      fseek(Movie.fp, HDR_OFS_RERECORDS, SEEK_SET);
      fwrite(b, 1, 4, Movie.fp);
      fclose(Movie.fp);
      Movie.fp = NULL;
      Movie.Status = Stopped;
      MovieLeaveDeterministic();
      strcpy(MovieStatus, "Recording Stopped");
      MovieMessage(_("Recording stopped (%d frames)"), Movie.Frames);
   } else if (Movie.Status == Playback) {
      MovieFinishPlayback();
      MovieMessage(_("Playback stopped"));
   }
   MovieRestoreUserBackup();
}

int SaveMovie(const char *filename)
{
   FILE *fp;
   u32 plen;

   movie_report[0] = 0;
   if (filename == NULL)
      return -1;
   if (Movie.Status == Recording || Movie.Status == Playback || bup_swapped)
      StopMovie();

   if ((fp = fopen(filename, "w+b")) == NULL)
      return -1;

   MovieSetFilename(filename);
   Movie.fp = fp;
   Movie.ReadOnly = 0;
   Movie.Rerecords = 0;
   Movie.Frames = 0;
   Movie.Size = 0;
   movie_desync = -1;
   movie_ended = 0;
   movie_port_overflow = 0;

   /* Taille des ports avec la configuration actuelle des manettes. */
   plen = (u32)PORTDATA1.size;
   if ((u32)PORTDATA2.size > plen)
      plen = (u32)PORTDATA2.size;
   if (plen < MOVIE_MIN_PORT_LEN)
      plen = MOVIE_MIN_PORT_LEN;
   if (plen > MOVIE_MAX_PORT_LEN)
      plen = MOVIE_MAX_PORT_LEN;

   /* Image de la RAM de sauvegarde avant le demarrage : celle que le jeu
      verra au boot. */
   MovieClearMachine();
   movie_starting = 1;
   framecounter = 0;
   lagframecounter = 0;
   Movie.Status = Recording;
   YabauseReset();
   movie_starting = 0;

   MovieFillHeaderFromMachine(&hdr);
   hdr.port_len = plen;
   hdr.frame_len = 1 + 2 * (1 + plen) + 4;
   hdr.frames = 0;
   hdr.rerecords = 0;
   if (MovieWriteHeader(fp, &hdr) != 0) {
      fclose(fp);
      Movie.fp = NULL;
      Movie.Status = Stopped;
      MovieReport(_("Couldn't write the movie header"));
      return -1;
   }

   MovieEnterDeterministic();
   strcpy(MovieStatus, "Recording Started");
   MovieMessage(_("Recording started"));
   return 0;
}

int PlayMovie(const char *filename)
{
   FILE *fp;
   MovieHeader h;
   long size;

   movie_report[0] = 0;
   if (filename == NULL)
      return -1;
   if (Movie.Status == Recording || Movie.Status == Playback || bup_swapped)
      StopMovie();

   if ((fp = fopen(filename, "rb")) == NULL)
      return -1;

   if (MovieReadHeader(fp, &h) != 0) {
      fclose(fp);
      MovieReport(_("This file is not a Kronos input movie (YMV2 format) or it is damaged"));
      return -2;
   }
   if (MovieCheckHeader(&h) != 0) {
      fclose(fp);
      return -2;
   }

   fseek(fp, 0, SEEK_END);
   size = ftell(fp);
   if (size < (long)h.header_size) {
      fclose(fp);
      MovieReport(_("Truncated movie"));
      return -2;
   }

   hdr = h;
   MovieSetFilename(filename);
   Movie.fp = fp;
   Movie.Size = (int)size;
   Movie.Frames = (int)((size - (long)h.header_size) / (long)h.frame_len);
   if (h.frames != 0 && (int)h.frames < Movie.Frames)
      Movie.Frames = (int)h.frames;
   Movie.Rerecords = (int)h.rerecords;
   Movie.ReadOnly = 1;
   movie_desync = -1;
   movie_ended = 0;
   movie_port_overflow = 0;

   if (MovieSaveUserBackup() != 0) {
      fclose(fp);
      Movie.fp = NULL;
      MovieReport(_("Not enough memory to set the backup RAM aside"));
      return -1;
   }
   MovieApplyBackupImages(fp, &h);

   movie_saved_port1 = PORTDATA1;
   movie_saved_port2 = PORTDATA2;
   movie_ports_saved = 1;

   MovieClearMachine();
   movie_starting = 1;
   framecounter = 0;
   lagframecounter = 0;
   Movie.Status = Playback;
   YabauseReset();
   movie_starting = 0;

   MovieEnterDeterministic();
   strcpy(MovieStatus, "Playback Started");
   MovieMessage(_("Playing movie: %d frames"), Movie.Frames);
   if (Movie.Frames == 0) {
      MovieFinishPlayback();
      movie_ended = 1;
   }
   return 0;
}

void MovieResetHook(void)
{
   if (movie_starting) {
      /* Memoire SMPC du film (langue) a la place de celle de l'hote. */
      if (Movie.Status == Playback && SmpcInternalVars != NULL) {
         memcpy(SmpcInternalVars->SMEM, hdr.smem, 4);
         SmpcInternalVars->languageid = hdr.smem[3] & 0xF;
      }
      return;
   }
   /* Reset demande par l'utilisateur : il n'est pas dans le film. */
   if (Movie.Status == Recording || Movie.Status == Playback)
      MovieMessage(_("Reset: movie stopped"));
   StopMovie();
}

void MovieToggleReadOnly(void)
{
   if (Movie.Status == Playback) {
      Movie.ReadOnly = !Movie.ReadOnly;
      MovieMessage(Movie.ReadOnly ? _("Movie is read-only") : _("Movie is read+write"));
   }
}

//////////////////////////////////////////////////////////////////////////////
// Etats de sauvegarde
//////////////////////////////////////////////////////////////////////////////

void SaveMovieInState(void ** stream)
{
   u32 magic = MOVIE_STATE_MAGIC;
   u32 frame = (u32)framecounter;
   u32 size;
   long fpos;

   if ((Movie.Status != Recording && Movie.Status != Playback) || Movie.fp == NULL)
      return;

   fflush(Movie.fp);
   fpos = ftell(Movie.fp);
   fseek(Movie.fp, 0, SEEK_END);
   size = (u32)ftell(Movie.fp);

   MemStateWrite(&magic, 4, 1, stream);
   MemStateWrite(&frame, 4, 1, stream);
   MemStateWrite(&size, 4, 1, stream);

   if (stream == NULL) {
      /* passe de dimensionnement */
      MemStateWrite(NULL, size, 1, NULL);
   } else {
      u8 *data = (u8 *)malloc(size ? size : 1);
      if (data != NULL) {
         fseek(Movie.fp, 0, SEEK_SET);
         if (fread(data, 1, size, Movie.fp) != size)
            memset(data, 0, size);
         MemStateWrite(data, size, 1, stream);
         free(data);
      } else {
         /* garder la taille annoncee */
         u8 zero[256];
         u32 left = size;
         memset(zero, 0, sizeof(zero));
         while (left) {
            u32 n = left > sizeof(zero) ? (u32)sizeof(zero) : left;
            MemStateWrite(zero, n, 1, stream);
            left -= n;
         }
      }
   }
   if (fpos >= 0)
      fseek(Movie.fp, fpos, SEEK_SET);
}

void MovieReadState(const void * stream, int available)
{
   u32 magic, frame, size;
   int fpos;

   if (Movie.Status != Recording && Movie.Status != Playback)
      return;
   if (available < 12)
      return;

   fpos = MemStateGetOffset();
   MemStateRead(&magic, 4, 1, stream);
   if (magic != MOVIE_STATE_MAGIC) {
      MemStateSetOffset(fpos);
      return;
   }
   MemStateRead(&frame, 4, 1, stream);
   MemStateRead(&size, 4, 1, stream);
   if ((u64)size > (u64)(available - 12)) {
      MemStateSetOffset(fpos);
      return;
   }

   if (Movie.Status == Recording || (Movie.Status == Playback && !Movie.ReadOnly)) {
      /* Reprise d'enregistrement : le film de l'etat, coupe a la trame de
         l'etat, remplace le fichier. */
      u8 *data = (u8 *)malloc(size ? size : 1);
      u64 keep = (u64)hdr.header_size + (u64)frame * hdr.frame_len;
      FILE *fp;

      if (data == NULL)
         return;
      MemStateRead(data, 1, size, stream);
      if (size < MOVIE_FIXED_SIZE || memcmp(data, MOVIE_MAGIC, 4) != 0 ||
          get32(data + 8) != hdr.header_size || get32(data + 12) != hdr.frame_len) {
         free(data);
         MovieMessage(_("Save state from another movie: movie stopped"));
         StopMovie();
         return;
      }
      if (keep > size)
         keep = size;
      if (Movie.fp)
         fclose(Movie.fp);
      fp = fopen(movie_filename, "w+b");
      if (fp == NULL) {
         free(data);
         Movie.fp = NULL;
         Movie.Status = Stopped;
         MovieLeaveDeterministic();
         MovieMessage(_("Couldn't rewrite the movie"));
         return;
      }
      fwrite(data, 1, (size_t)keep, fp);
      fflush(fp);
      free(data);
      Movie.fp = fp;
      if (Movie.Status == Playback) {
         /* la lecture devient un enregistrement : la RAM de sauvegarde du
            testeur reste isolee jusqu'a l'arret */
         MovieMessage(_("Recording resumed at frame %u"), frame);
      }
      Movie.Status = Recording;
      Movie.ReadOnly = 0;
      Movie.Rerecords++;
      Movie.Frames = (int)frame;
      strcpy(MovieStatus, "Recording Resumed");
   } else {
      /* lecture seule : on continue la lecture a la trame de l'etat */
      if ((int)frame > Movie.Frames) {
         MovieMessage(_("Save state is past the end of the movie: playback stopped"));
         StopMovie();
         return;
      }
   }
   framecounter = (int)frame;
   movie_desync = -1;
}

const char *MakeMovieStateName(const char *filename)
{
   static char *retbuf = NULL;
   if (Movie.Status == Recording || Movie.Status == Playback) {
      const size_t newsize = strlen(filename) + 5 + 1;
      free(retbuf);
      retbuf = (char*)malloc(newsize);
      if (!retbuf)
         return NULL;
      sprintf(retbuf, "%smovie", filename);
      return retbuf;
   }
   return filename;
}

//////////////////////////////////////////////////////////////////////////////
// Divers
//////////////////////////////////////////////////////////////////////////////

void PauseOrUnpause(void)
{
   if (FrameAdvanceVariable == RunNormal) {
      FrameAdvanceVariable = Paused;
      ScspMuteAudio(SCSP_MUTE_SYSTEM);
   } else {
      FrameAdvanceVariable = RunNormal;
      ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
   }
}

int IsMovieLoaded(void)
{
   return (Movie.Status == Recording || Movie.Status == Playback);
}

int MovieIsDeterministic(void)
{
   return (Movie.Status == Recording || Movie.Status == Playback);
}

int MovieIsActive(void)
{
   return IsMovieLoaded() || bup_swapped;
}

int MovieGetFrame(void)
{
   return IsMovieLoaded() ? framecounter : 0;
}

int MovieGetLength(void)
{
   return Movie.Frames;
}

int MovieGetDesyncFrame(void)
{
   return movie_desync;
}

int MovieConsumeEndedFlag(void)
{
   if (movie_ended) {
      movie_ended = 0;
      return 1;
   }
   return 0;
}

const char *MovieGetReport(void)
{
   return movie_report;
}
