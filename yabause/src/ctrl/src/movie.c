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

    Speedruns assistes par outil (TAS) :
      - toutes les trames sont en memoire (table mv_buf), le fichier suit ;
      - l'octet de drapeaux de chaque trame dit si c'etait une trame de lag
        (le jeu n'a pas lu les manettes) : bit 0 = lag, bit 1 = lag connu ;
      - en lecture seule, charger un etat reprend la lecture a sa trame ;
        en lecture/ecriture il reprend l'enregistrement (re-record), et la
        fin du film enchaine sur un enregistrement ;
      - le bloc du film dans les etats contient ce qui manque au reste de
        l'etat pour retrouver exactement la meme suite (voir
        SaveMovieInState()) ;
      - l'editeur d'entrees modifie la table (MovieEdit*()), revient en
        arriere avec MovieSeek() a partir d'instantanes gardes en memoire ;
      - une modification rend caduques les empreintes des trames suivantes :
        l'en-tete garde la premiere trame dont l'empreinte n'est plus
        valable (hash_invalid_from, drapeau 2), une relecture en
        lecture/ecriture les refait.
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

/* Offsets des champs reecrits pendant la vie du film. */
#define HDR_OFS_FRAMES     20
#define HDR_OFS_RERECORDS  24
#define HDR_OFS_FLAGS      28
#define HDR_OFS_HASHMARK   302

/* Drapeaux de l'en-tete. */
#define HDR_FLAG_HASH      1   /* chaque trame a une empreinte */
#define HDR_FLAG_HASHMARK  2   /* hash_invalid_from est renseigne */

/* Drapeaux d'une trame. */
#define REC_LAG            0x01
#define REC_LAG_KNOWN      0x02

#define MOVIE_NOT_STALE    0x7FFFFFFF

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
   /* premiere trame dont l'empreinte n'est pas valable (les films d'avant
      n'ont pas ce champ : toutes le sont) */
   u32 hash_invalid_from;
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
   h->flags = HDR_FLAG_HASH;
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
   put32(p, h->hash_invalid_from);         p += 4;   /* HDR_OFS_HASHMARK */

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
   h->hash_invalid_from = (h->flags & HDR_FLAG_HASHMARK) ? get32(p) : 0xFFFFFFFFu;
   p += 4;

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
// Table des trames
//////////////////////////////////////////////////////////////////////////////

/* Toutes les trames du film sont en memoire (mv_buf, Movie.Frames entrees de
   hdr.frame_len octets) ; le fichier est tenu a jour au fil de l'eau quand
   il est ouvert en ecriture. L'editeur d'entrees lit et modifie cette
   table. */
static u8 *mv_buf = NULL;
static u32 mv_cap = 0;         /* capacite, en trames */
static int mv_writable = 0;    /* fichier ouvert en lecture/ecriture */

static u8 *MovieRec(u32 index)
{
   return mv_buf + (size_t)index * hdr.frame_len;
}

static int MovieReserve(u32 frames)
{
   u32 cap;
   u8 *nb;
   if (frames <= mv_cap && mv_buf != NULL)
      return 0;
   if (frames > 0x7FFFFFFFu / (hdr.frame_len ? hdr.frame_len : 1))
      return -1;
   cap = mv_cap ? mv_cap : 4096;
   while (cap < frames) {
      u32 more = cap / 2 + 4096;
      if (cap > 0x7FFFFFFFu / (hdr.frame_len ? hdr.frame_len : 1) - more) {
         cap = frames;
         break;
      }
      cap += more;
   }
   nb = (u8 *)realloc(mv_buf, (size_t)cap * hdr.frame_len);
   if (nb == NULL)
      return -1;
   mv_buf = nb;
   mv_cap = cap;
   return 0;
}

static void MovieFreeFrames(void)
{
   free(mv_buf);
   mv_buf = NULL;
   mv_cap = 0;
}

/* Ecrit des trames de la table dans le fichier. */
static int MovieFlushFrames(u32 from, u32 count)
{
   if (!mv_writable || Movie.fp == NULL || count == 0)
      return 0;
   fseek(Movie.fp, (long)(hdr.header_size + (u64)from * hdr.frame_len), SEEK_SET);
   if (fwrite(MovieRec(from), hdr.frame_len, count, Movie.fp) != count)
      return -1;
   fflush(Movie.fp);
   return 0;
}

/* Champs de l'en-tete qui changent pendant la vie du film. Pendant un
   enregistrement le nombre de trames reste a 0 dans le fichier (final = 0) :
   apres un plantage, la longueur du film est celle du fichier. */
static void MovieWriteCounts(int final)
{
   u8 b[4];
   if (!mv_writable || Movie.fp == NULL)
      return;
   put32(b, final ? (u32)Movie.Frames : 0);
   fseek(Movie.fp, HDR_OFS_FRAMES, SEEK_SET);
   fwrite(b, 1, 4, Movie.fp);
   put32(b, (u32)Movie.Rerecords);
   fseek(Movie.fp, HDR_OFS_RERECORDS, SEEK_SET);
   fwrite(b, 1, 4, Movie.fp);
   hdr.flags |= HDR_FLAG_HASHMARK;
   put32(b, hdr.flags);
   fseek(Movie.fp, HDR_OFS_FLAGS, SEEK_SET);
   fwrite(b, 1, 4, Movie.fp);
   put32(b, hdr.hash_invalid_from);
   fseek(Movie.fp, HDR_OFS_HASHMARK, SEEK_SET);
   fwrite(b, 1, 4, Movie.fp);
   fflush(Movie.fp);
}

/* Reecrit tout le fichier a partir de la table (le film a raccourci, ou des
   trames ont ete inserees). Le nouveau contenu est ecrit dans un fichier
   temporaire qui remplace ensuite l'ancien : une erreur d'ecriture (disque
   plein) ne detruit pas le film. Renvoie 0 ou -1 ; apres -1, Movie.fp est
   toujours le fichier d'origine s'il a pu etre rouvert, NULL sinon. */
static int MovieRewriteFile(int final)
{
   u8 *head;
   char *tmpname;
   FILE *tmp;
   size_t len;
   int ok;

   if (!mv_writable || Movie.fp == NULL || movie_filename == NULL)
      return -1;

   head = (u8 *)malloc(hdr.header_size);
   if (head == NULL)
      return -1;
   fflush(Movie.fp);
   fseek(Movie.fp, 0, SEEK_SET);
   if (fread(head, 1, hdr.header_size, Movie.fp) != hdr.header_size) {
      free(head);
      return -1;
   }
   hdr.flags |= HDR_FLAG_HASHMARK;
   put32(head + HDR_OFS_FRAMES, final ? (u32)Movie.Frames : 0);
   put32(head + HDR_OFS_RERECORDS, (u32)Movie.Rerecords);
   put32(head + HDR_OFS_FLAGS, hdr.flags);
   put32(head + HDR_OFS_HASHMARK, hdr.hash_invalid_from);

   len = strlen(movie_filename);
   tmpname = (char *)malloc(len + 5);
   if (tmpname == NULL) {
      free(head);
      return -1;
   }
   memcpy(tmpname, movie_filename, len);
   memcpy(tmpname + len, ".tmp", 5);

   tmp = fopen(tmpname, "wb");
   if (tmp == NULL) {
      free(head);
      free(tmpname);
      return -1;
   }
   ok = (fwrite(head, 1, hdr.header_size, tmp) == hdr.header_size);
   if (ok && Movie.Frames > 0)
      ok = (fwrite(mv_buf, hdr.frame_len, (size_t)Movie.Frames, tmp) == (size_t)Movie.Frames);
   if (fclose(tmp) != 0)
      ok = 0;
   free(head);
   if (!ok) {
      remove(tmpname);
      free(tmpname);
      return -1;
   }

   /* rename() ne remplace pas un fichier existant sous Windows */
   fclose(Movie.fp);
   Movie.fp = NULL;
   remove(movie_filename);
   if (rename(tmpname, movie_filename) != 0) {
      /* le film complet est dans le fichier temporaire */
      MovieReport(_("Couldn't replace the movie file, its content is in %s"), tmpname);
      free(tmpname);
      return -1;
   }
   free(tmpname);
   Movie.fp = fopen(movie_filename, "r+b");
   if (Movie.fp == NULL)
      return -1;
   return 0;
}

/* Les entrees de deux trames (tailles et donnees des deux ports) sont-elles
   les memes ? Les drapeaux et l'empreinte ne comptent pas. */
static int MovieSameInput(const u8 *a, const u8 *b)
{
   return memcmp(a + 1, b + 1, hdr.frame_len - 5) == 0;
}

static int MoviePortIsPad(const u8 *data, int size)
{
   return (size >= 4 && data[0] == 0xF1 && data[1] == PERPAD);
}

/* Relache les boutons d'une manette numerique branchee directement. */
static void MovieReleasePad(PortData_struct *port)
{
   if (MoviePortIsPad(port->data, port->size)) {
      port->data[2] = 0xFF;
      port->data[3] = 0xFF;
   }
}

//////////////////////////////////////////////////////////////////////////////
// Instantanes (retour en arriere de l'editeur d'entrees)
//////////////////////////////////////////////////////////////////////////////

/* Etats de sauvegarde gardes en memoire a intervalle regulier pendant un
   film, sans capture d'ecran et compresses (PackBits). MovieSeek() repart
   du plus proche au lieu de rejouer le film depuis la mise sous tension. */
typedef struct
{
   int frame;      /* trames executees quand l'instantane a ete pris */
   u8 *data;
   u32 packed;
   u32 size;
} MovieSnap;

static MovieSnap *snaps = NULL;
static int nsnaps = 0;
static int snaps_cap = 0;
static int snap_interval = 0;                    /* 0 : pas d'instantanes */
static u64 snap_budget = (u64)256 * 1024 * 1024; /* octets */
static u64 snap_bytes = 0;
static int movie_state_light = 0;

int MovieStateIsLight(void)
{
   return movie_state_light;
}

static void MovieSnapRemove(int i)
{
   snap_bytes -= snaps[i].packed;
   free(snaps[i].data);
   memmove(snaps + i, snaps + i + 1, (size_t)(nsnaps - i - 1) * sizeof(MovieSnap));
   nsnaps--;
}

static void MovieSnapClear(void)
{
   while (nsnaps > 0)
      MovieSnapRemove(nsnaps - 1);
   free(snaps);
   snaps = NULL;
   snaps_cap = 0;
   snap_bytes = 0;
}

/* Les instantanes pris apres 'frame' trames ne sont plus valables (une
   entree de la trame 'frame' ou d'apres a change). */
static void MovieSnapInvalidateAfter(int frame)
{
   while (nsnaps > 0 && snaps[nsnaps - 1].frame > frame)
      MovieSnapRemove(nsnaps - 1);
}

/* Dernier instantane pris a 'frame' trames ou avant, ou -1. */
static int MovieSnapFind(int frame)
{
   int i, best = -1;
   for (i = 0; i < nsnaps; i++) {
      if (snaps[i].frame > frame)
         break;
      best = i;
   }
   return best;
}

static void MovieSnapTake(void)
{
   void *state = NULL;
   size_t size = 0;
   u8 *packed;
   u32 plen;
   int i, pos, status;

   for (pos = 0; pos < nsnaps && snaps[pos].frame < framecounter; pos++)
      ;
   if (pos < nsnaps && snaps[pos].frame == framecounter)
      return; /* deja pris */

   movie_state_light = 1;
   status = YabSaveStateBuffer(&state, &size);
   movie_state_light = 0;
   if (status != 0 || state == NULL || size == 0 || size > 0x7FFFFFFFu) {
      free(state);
      return;
   }
   packed = (u8 *)malloc(size + size / 128 + 16);
   if (packed == NULL) {
      free(state);
      return;
   }
   plen = MoviePack((const u8 *)state, (u32)size, packed);
   free(state);
   {
      u8 *shrunk = (u8 *)realloc(packed, plen ? plen : 1);
      if (shrunk != NULL)
         packed = shrunk;
   }

   if (nsnaps >= snaps_cap) {
      int cap = snaps_cap ? snaps_cap * 2 : 64;
      MovieSnap *ns = (MovieSnap *)realloc(snaps, (size_t)cap * sizeof(MovieSnap));
      if (ns == NULL) {
         free(packed);
         return;
      }
      snaps = ns;
      snaps_cap = cap;
   }
   memmove(snaps + pos + 1, snaps + pos, (size_t)(nsnaps - pos) * sizeof(MovieSnap));
   snaps[pos].frame = framecounter;
   snaps[pos].data = packed;
   snaps[pos].packed = plen;
   snaps[pos].size = (u32)size;
   nsnaps++;
   snap_bytes += plen;

   /* budget memoire depasse : les plus anciens partent d'abord (on revient
      surtout sur la fin du film) ; celui qui vient d'etre pris reste */
   for (i = 0; snap_bytes > snap_budget && nsnaps > 1 && i < nsnaps; ) {
      if (snaps[i].frame == framecounter) {
         i++;
         continue;
      }
      MovieSnapRemove(i);
   }
}

void MovieSetSnapshots(int interval_frames, int budget_mb)
{
   if (interval_frames < 0)
      interval_frames = 0;
   if (budget_mb < 16)
      budget_mb = 16;
   snap_interval = interval_frames;
   snap_budget = (u64)budget_mb * 1024 * 1024;
   if (snap_interval == 0)
      MovieSnapClear();
}

int MovieGetSnapshotCount(void)
{
   return nsnaps;
}

//////////////////////////////////////////////////////////////////////////////
// Affichage
//////////////////////////////////////////////////////////////////////////////

/* Boutons d'une manette numerique, dans l'ordre d'affichage. PORTDATA :
   data[2] = Droite Gauche Bas Haut Start A C B (bits 7 a 0), data[3] =
   R X Y Z L (bits 7 a 3), 0 = appuye. */
static const struct { u8 byte; u8 mask; char letter; } pad_buttons[MOVIE_PAD_BUTTONS] = {
   { 2, 0x10, 'U' }, { 2, 0x20, 'D' }, { 2, 0x40, 'L' }, { 2, 0x80, 'R' },
   { 2, 0x08, 'S' }, { 2, 0x04, 'A' }, { 2, 0x01, 'B' }, { 2, 0x02, 'C' },
   { 3, 0x40, 'X' }, { 3, 0x20, 'Y' }, { 3, 0x10, 'Z' }, { 3, 0x08, 'l' },
   { 3, 0x80, 'r' }
};

static void MovieInputString(const u8 *data, int size, char *out, size_t outlen)
{
   size_t n = 0;
   int i;

   out[0] = 0;
   if (size <= 0 || outlen < 2)
      return;
   if (MoviePortIsPad(data, size)) {
      for (i = 0; i < MOVIE_PAD_BUTTONS && n + 1 < outlen; i++)
         out[n++] = (data[pad_buttons[i].byte] & pad_buttons[i].mask) ? '.' : pad_buttons[i].letter;
      out[n] = 0;
      return;
   }
   /* autre peripherique : les premiers octets, en hexadecimal */
   for (i = 0; i < size && i < 10 && n + 3 < outlen; i++) {
      snprintf(out + n, outlen - n, "%02X", data[i]);
      n += 2;
   }
}

/* L'ancienne version intervertissait Gauche et Droite (bit 6 = Gauche,
   bit 7 = Droite, voir PerPadLeftPressed()/PerPadRightPressed()). */
static void SetInputDisplayCharacters(void)
{
   MovieInputString(PORTDATA1.data, PORTDATA1.size, InputDisplayString, sizeof(InputDisplayString));
}

static int movie_osd = 0;
static int movie_stale_from = MOVIE_NOT_STALE;

void MovieSetOSD(int on)
{
   movie_osd = on ? 1 : 0;
}

int MovieGetOSD(void)
{
   return movie_osd;
}

static void MovieOSD(void)
{
   char in1[40], in2[40], line[256];
   size_t n;

   SetInputDisplayCharacters();
   if (!movie_osd)
      return;

   MovieInputString(PORTDATA1.data, PORTDATA1.size, in1, sizeof(in1));
   MovieInputString(PORTDATA2.data, PORTDATA2.size, in2, sizeof(in2));

   if (Movie.Status == Recording)
      snprintf(line, sizeof(line), "REC %d", framecounter);
   else if (Movie.Status == Playback)
      snprintf(line, sizeof(line), "PLAY %d/%d%s", framecounter, Movie.Frames, Movie.ReadOnly ? "" : " RW");
   else
      snprintf(line, sizeof(line), "FRAME %d", framecounter);
   n = strlen(line);
   snprintf(line + n, sizeof(line) - n, "   LAG %d", lagframecounter);
   n = strlen(line);
   if (IsMovieLoaded()) {
      snprintf(line + n, sizeof(line) - n, "   RR %d", Movie.Rerecords);
      n = strlen(line);
   }
   if (in1[0]) {
      snprintf(line + n, sizeof(line) - n, "   1:%s", in1);
      n = strlen(line);
   }
   if (in2[0]) {
      snprintf(line + n, sizeof(line) - n, "   2:%s", in2);
      n = strlen(line);
   }
   if (movie_stale_from != MOVIE_NOT_STALE)
      snprintf(line + n, sizeof(line) - n, "   EDITED: SEEK NEEDED");
   else if (movie_desync >= 0)
      snprintf(line + n, sizeof(line) - n, "   DESYNC@%d", movie_desync);
   OSDPushMessage(OSDMSG_TAS, 1, "%s", line);
}

//////////////////////////////////////////////////////////////////////////////
// Trames
//////////////////////////////////////////////////////////////////////////////

static int movie_cur_index = -1;     /* trame du film en cours d'execution */
static int movie_rec_dirty = 0;      /* son entree doit etre reecrite */
static int movie_last_lag = 0;
static int movie_pause_at = -1;
static int movie_pause_req = 0;
static int movie_keep_on_load = 0;

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

/* Remet les ports de l'utilisateur (mis de cote au debut de la lecture),
   boutons relaches : les donnees du film ne doivent pas rester "appuyees". */
static void MovieRestoreUserPorts(void)
{
   if (!movie_ports_saved)
      return;
   PORTDATA1 = movie_saved_port1;
   PORTDATA2 = movie_saved_port2;
   MovieReleasePad(&PORTDATA1);
   MovieReleasePad(&PORTDATA2);
   movie_ports_saved = 0;
}

static void MovieFinishPlayback(void)
{
   if (Movie.fp) {
      if (mv_writable && !Movie.ReadOnly)
         MovieWriteCounts(1);
      fclose(Movie.fp);
   }
   Movie.fp = NULL;
   Movie.Status = Stopped;
   MovieLeaveDeterministic();
   MovieRestoreUserPorts();
   MovieSnapClear();
   MovieFreeFrames();
   movie_stale_from = MOVIE_NOT_STALE;
   movie_pause_at = -1;
   strcpy(MovieStatus, "Playback Stopped");
}

static void MovieAnnounceEnd(void)
{
   movie_ended = 1;
   if (movie_desync >= 0)
      MovieMessage(_("End of movie (%d frames) - DESYNC at frame %d"), Movie.Frames, movie_desync);
   else
      MovieMessage(_("End of movie (%d frames), in sync"), Movie.Frames);
}

/* Enregistrement -> lecture (chargement d'un etat en lecture seule, ou
   modification du film par l'editeur). */
static void MovieSwitchToPlayback(void)
{
   if (Movie.Status != Recording)
      return;
   Movie.Status = Playback;
   MovieWriteCounts(1);
   movie_saved_port1 = PORTDATA1;
   movie_saved_port2 = PORTDATA2;
   movie_ports_saved = 1;
   /* A partir d'ici la RAM de sauvegarde suit le film (etats, retours en
      arriere) : celle de l'utilisateur est mise de cote telle qu'elle est
      maintenant, avec ce que le jeu y a ecrit depuis le debut. */
   if (!bup_swapped)
      MovieSaveUserBackup();
   strcpy(MovieStatus, "Playback Started");
}

/* Lecture -> enregistrement (fin du film en lecture/ecriture, ou reprise a
   partir d'un etat). */
static void MovieSwitchToRecording(void)
{
   if (Movie.Status != Playback)
      return;
   Movie.Status = Recording;
   Movie.ReadOnly = 0;
   MovieRestoreUserPorts();
   MovieWriteCounts(0);
   strcpy(MovieStatus, "Recording Resumed");
}

void DoMovie(void)
{
   u32 index, hash;
   u8 *rec;

   framecounter++;
   /* Leve par le SMPC quand le jeu lit les manettes (INTBACK ou acces
      direct aux ports) ; regarde a la fin de la trame, MovieFrameEnd(). */
   LagFrameFlag = 1;
   movie_cur_index = -1;
   movie_rec_dirty = 0;

   if (Movie.Status != Recording && Movie.Status != Playback) {
      MovieOSD();
      return;
   }

   /* Trame dont on traite l'entree (0 = premiere trame apres le reset). */
   index = (u32)(framecounter - 1);

   /* PerInit() a pu remettre une autre interface pendant le film. */
   MovieWrapPerCore();

   /* Une trame deja executee a ete modifiee dans l'editeur : la machine ne
      correspond plus au film tant qu'on n'est pas revenu en arriere
      (MovieSeek()). On n'enregistre et on ne compare rien. */
   if (movie_stale_from != MOVIE_NOT_STALE) {
      MovieOSD();
      return;
   }

   if (Movie.Status == Playback && (int)index >= Movie.Frames) {
      if (!Movie.ReadOnly && mv_writable && (int)index == Movie.Frames) {
         /* fin du film en lecture/ecriture : la suite s'enregistre */
         MovieSwitchToRecording();
         MovieMessage(_("End of movie: recording from frame %u"), index);
      } else {
         MovieFinishPlayback();
         MovieAnnounceEnd();
         MovieOSD();
         return;
      }
   }

   hash = MovieStateHash();

   if (Movie.Status == Recording) {
      if ((int)index != Movie.Frames || MovieReserve(index + 1) != 0) {
         MovieMessage(_("Movie out of step or out of memory, recording stopped"));
         StopMovie();
         MovieOSD();
         return;
      }
      /* Seul point de la trame ou l'hote peut changer les entrees. */
      if (movie_real_percore != NULL && movie_real_percore->HandleEvents != NULL)
         movie_real_percore->HandleEvents();
      /* Computed peripheral outputs (SBom Joycard, keyboard code stream):
         produced here, once per frame, so that the recorded port data are
         what the game reads. PerKeyboardUpdate() leaves the port data
         alone during a movie (peripheral.c). */
      PerMovieRecordUpdate();

      rec = MovieRec(index);
      rec[0] = 0;
      MovieWritePort(rec + 1, &PORTDATA1);
      MovieWritePort(rec + 2 + hdr.port_len, &PORTDATA2);
      put32(rec + hdr.frame_len - 4, hash);
      Movie.Frames = (int)index + 1;
      if (hdr.hash_invalid_from >= index)
         hdr.hash_invalid_from = index + 1;
      /* Le film doit survivre a un plantage de l'emulateur. */
      if (MovieFlushFrames(index, 1) != 0) {
         MovieMessage(_("Movie write error, recording stopped"));
         StopMovie();
         MovieOSD();
         return;
      }
      movie_cur_index = (int)index;
      MovieOSD();
      return;
   }

   /* Lecture */
   rec = MovieRec(index);
   MovieReadPort(rec + 1, &PORTDATA1);
   MovieReadPort(rec + 2 + hdr.port_len, &PORTDATA2);
   movie_cur_index = (int)index;

   if (hdr.flags & HDR_FLAG_HASH) {
      if (index < hdr.hash_invalid_from) {
         if (movie_desync < 0 && get32(rec + hdr.frame_len - 4) != hash) {
            movie_desync = (int)index;
            MovieMessage(_("DESYNC at frame %u (diverged during frame %d)"),
                         index, (int)index - 1);
         }
      } else if (index == hdr.hash_invalid_from && !Movie.ReadOnly && mv_writable) {
         /* Empreintes perimees par une modification du film : cette
            relecture les refait, trame apres trame. */
         put32(rec + hdr.frame_len - 4, hash);
         hdr.hash_invalid_from = index + 1;
         movie_rec_dirty = 1;
      }
   }
   MovieOSD();
}

/* Appele par YabauseEmulate() a la fin de chaque trame, quand le thread son
   a fini la sienne : la machine est dans l'etat qu'un etat de sauvegarde
   enregistrerait. */
void MovieFrameEnd(void)
{
   const int lag = (LagFrameFlag == 1);

   movie_last_lag = lag;
   if (lag)
      lagframecounter++;

   if (movie_cur_index >= 0 && movie_cur_index < Movie.Frames && mv_buf != NULL) {
      u8 *rec = MovieRec((u32)movie_cur_index);
      const u8 flags = (u8)((rec[0] & ~(REC_LAG | REC_LAG_KNOWN)) | REC_LAG_KNOWN | (lag ? REC_LAG : 0));
      if (flags != rec[0] || movie_rec_dirty) {
         rec[0] = flags;
         if (Movie.Status == Recording || !Movie.ReadOnly)
            MovieFlushFrames((u32)movie_cur_index, 1);
      }
   }
   movie_cur_index = -1;
   movie_rec_dirty = 0;

   if (Movie.Status != Recording && Movie.Status != Playback)
      return;
   if (movie_stale_from != MOVIE_NOT_STALE)
      return;

   /* Fin d'une lecture seule : arret exact apres la derniere trame du film
      (avant, la lecture n'etait arretee qu'au debut de la trame suivante,
      et la pause de l'interface venait quelques trames plus tard). */
   if (Movie.Status == Playback && framecounter >= Movie.Frames &&
       (Movie.ReadOnly || !mv_writable)) {
      MovieFinishPlayback();
      MovieAnnounceEnd();
      movie_pause_req = 1;
      return;
   }

   if (movie_pause_at >= 0 && framecounter >= movie_pause_at) {
      movie_pause_at = -1;
      movie_pause_req = 1;
   }

   if (snap_interval > 0 && framecounter > 0 && (framecounter % snap_interval) == 0)
      MovieSnapTake();
}

void MovieSetPauseFrame(int frame)
{
   movie_pause_at = frame;
}

int MovieConsumePauseRequest(void)
{
   if (movie_pause_req) {
      movie_pause_req = 0;
      return 1;
   }
   return 0;
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

static void MovieResetSession(void)
{
   MovieSnapClear();
   MovieFreeFrames();
   movie_desync = -1;
   movie_ended = 0;
   movie_port_overflow = 0;
   movie_stale_from = MOVIE_NOT_STALE;
   movie_pause_at = -1;
   movie_pause_req = 0;
   movie_cur_index = -1;
   movie_rec_dirty = 0;
}

void StopMovie(void)
{
   if (Movie.Status == Recording && Movie.fp != NULL) {
      MovieWriteCounts(1);
      fclose(Movie.fp);
      Movie.fp = NULL;
      Movie.Status = Stopped;
      MovieLeaveDeterministic();
      MovieRestoreUserPorts();
      MovieSnapClear();
      MovieFreeFrames();
      movie_stale_from = MOVIE_NOT_STALE;
      movie_pause_at = -1;
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

   MovieResetSession();
   MovieSetFilename(filename);
   Movie.fp = fp;
   mv_writable = 1;
   Movie.ReadOnly = 0;
   Movie.Rerecords = 0;
   Movie.Frames = 0;
   Movie.Size = 0;

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
   hdr.hash_invalid_from = 0;
   /* la table existe des la trame 0 : un etat pris avant la premiere trame
      doit porter le film (SaveMovieInState()) */
   if (MovieReserve(1) != 0 || MovieWriteHeader(fp, &hdr) != 0) {
      fclose(fp);
      Movie.fp = NULL;
      Movie.Status = Stopped;
      MovieReport(_("Couldn't write the movie header"));
      return -1;
   }

   /* Les entrees d'une borne ST-V passent par l'IOGA (IOPortReadByte(),
      peripheral.c), pas par les ports du SMPC : elles ne sont pas dans le
      film. */
   if (yabsys.isSTV)
      MovieReport(_("ST-V: the cabinet inputs are not recorded (only the SMPC ports are)"));

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
   int writable = 1;
   u32 frames;

   movie_report[0] = 0;
   if (filename == NULL)
      return -1;
   if (Movie.Status == Recording || Movie.Status == Playback || bup_swapped)
      StopMovie();

   /* En lecture/ecriture si possible : la lecture peut devenir un
      enregistrement (etat charge hors lecture seule, editeur d'entrees). */
   if ((fp = fopen(filename, "r+b")) == NULL) {
      writable = 0;
      if ((fp = fopen(filename, "rb")) == NULL)
         return -1;
   }

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

   MovieResetSession();
   hdr = h;
   frames = (u32)((size - (long)h.header_size) / (long)h.frame_len);
   if (h.frames != 0 && h.frames < frames)
      frames = h.frames;
   if (MovieReserve(frames ? frames : 1) != 0) {
      fclose(fp);
      MovieReport(_("Not enough memory to load the movie"));
      return -1;
   }
   fseek(fp, (long)h.header_size, SEEK_SET);
   if (frames != 0 && fread(mv_buf, h.frame_len, frames, fp) != frames) {
      fclose(fp);
      MovieFreeFrames();
      MovieReport(_("Truncated movie"));
      return -2;
   }
   if (hdr.hash_invalid_from > frames)
      hdr.hash_invalid_from = frames;

   MovieSetFilename(filename);
   Movie.fp = fp;
   mv_writable = writable;
   Movie.Size = (int)size;
   Movie.Frames = (int)frames;
   Movie.Rerecords = (int)h.rerecords;
   Movie.ReadOnly = 1;

   if (MovieSaveUserBackup() != 0) {
      fclose(fp);
      Movie.fp = NULL;
      MovieFreeFrames();
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

/* Remet la machine au debut du film en cours (retour en arriere sans
   instantane) : meme remise a zero que PlayMovie(). */
static int MovieRestart(void)
{
   if (Movie.Status != Playback || Movie.fp == NULL)
      return -1;
   if (!bup_swapped && MovieSaveUserBackup() != 0)
      return -1;
   MovieApplyBackupImages(Movie.fp, &hdr);

   MovieClearMachine();
   movie_starting = 1;
   framecounter = 0;
   lagframecounter = 0;
   YabauseReset();
   movie_starting = 0;

   MovieEnterDeterministic();
   movie_desync = -1;
   movie_stale_from = MOVIE_NOT_STALE;
   return 0;
}

int MovieSeek(int target)
{
   int limit, i;

   if (Movie.Status != Recording && Movie.Status != Playback)
      return -1;
   if (target < 0)
      target = 0;
   if (target > Movie.Frames)
      target = Movie.Frames;

   /* Ou reprendre : pas apres la premiere trame modifiee derriere nous. */
   limit = target;
   if (movie_stale_from != MOVIE_NOT_STALE && movie_stale_from < limit)
      limit = movie_stale_from;

   if (movie_stale_from == MOVIE_NOT_STALE && target >= framecounter) {
      /* vers l'avant : un instantane plus loin que la trame courante fait
         gagner du temps, sinon il n'y a qu'a laisser tourner */
      i = MovieSnapFind(target);
      if (i < 0 || snaps[i].frame <= framecounter)
         return 0;
   }

   if (Movie.Status == Recording)
      MovieSwitchToPlayback();

   for (i = MovieSnapFind(limit); i >= 0; i = MovieSnapFind(limit)) {
      u8 *state = (u8 *)malloc(snaps[i].size);
      int status = -1;
      if (state != NULL && MovieUnpack(snaps[i].data, snaps[i].packed, state, snaps[i].size) == 0) {
         movie_state_light = 1;
         status = YabLoadStateBuffer(state, snaps[i].size);
         movie_state_light = 0;
      }
      free(state);
      if (status == 0 && Movie.Status == Playback)
         return 0;
      if (Movie.Status != Playback)
         return -1; /* le chargement a arrete le film */
      MovieSnapRemove(i); /* inutilisable */
   }
   return MovieRestart();
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

/* Lecture seule : charger un etat ne touche pas au film, la lecture reprend
   a la trame de l'etat (y compris pendant un enregistrement, qui devient une
   lecture). Lecture/ecriture : charger un etat reprend l'enregistrement a
   la trame de l'etat (re-record). */
void MovieSetReadOnly(int readonly)
{
   if (Movie.Status != Recording && Movie.Status != Playback)
      return;
   readonly = readonly ? 1 : 0;
   if (!readonly && !mv_writable) {
      MovieMessage(_("The movie file can't be written: it stays read-only"));
      return;
   }
   if (Movie.ReadOnly == readonly)
      return;
   Movie.ReadOnly = readonly;
   MovieMessage(Movie.ReadOnly ? _("Movie is read-only") : _("Movie is read+write"));
}

void MovieToggleReadOnly(void)
{
   MovieSetReadOnly(!Movie.ReadOnly);
}

void MovieSetKeepOnLoad(int keep)
{
   movie_keep_on_load = keep ? 1 : 0;
}

//////////////////////////////////////////////////////////////////////////////
// Etats de sauvegarde
//////////////////////////////////////////////////////////////////////////////

/* Bloc ajoute a la fin de chaque etat (petit-boutiste) :
      0 "YMVT"                 4 version (1)        8 longueur du bloc
     12 drapeaux (1 = film, 2 = instantane sans les trames)
     16 trame (framecounter)  20 trames de lag     24 re-records
     28 header_size du film   32 frame_len         36 limite des empreintes
     40 nombre de trames jointes
     44 taille / 48 taille compressee de la RAM de sauvegarde interne
     52 taille / 56 taille compressee de la cartouche de sauvegarde
     60 etat de synchronisation (YAB_SYNC_STATE_WORDS x u64)
    124 trames [0, trame) du film, puis les deux images PackBits

   Par rapport a l'ancien bloc "YMVS" (toujours lu) :
     - l'etat de synchronisation (fractions de cycles reportees d'une trame
       a l'autre, compteur de trames) et la RAM de sauvegarde ne sont pas
       dans le reste de l'etat : sans eux, la machine restauree ne donne pas
       la meme suite que le film rejoue depuis le debut, et un film repris a
       partir d'un etat se desynchronise a la relecture ;
     - seules les trames deja jouees sont jointes, pas tout le fichier ;
     - le bloc est ecrit meme sans film (synchronisation seule). */
#define STATE_MAGIC_NEW   0x54564D59u /* "YMVT" */
#define STATE_VERSION     1
#define STATE_FLAG_MOVIE  1
#define STATE_FLAG_LIGHT  2
#define STATE_HEAD        (60 + 8 * YAB_SYNC_STATE_WORDS)

static void put64(u8 *p, u64 v)
{
   put32(p, (u32)v);
   put32(p + 4, (u32)(v >> 32));
}

static u64 get64(const u8 *p)
{
   return (u64)get32(p) | ((u64)get32(p + 4) << 32);
}

void SaveMovieInState(void ** stream)
{
   const int active = (Movie.Status == Recording || Movie.Status == Playback) && mv_buf != NULL;
   u8 head[STATE_HEAD];
   u8 *bup_packed = NULL, *cbup_packed = NULL;
   u32 bup_size = 0, bup_plen = 0, cbup_size = 0, cbup_plen = 0;
   u32 nframes = 0, flags = 0, total;
   u64 sync[YAB_SYNC_STATE_WORDS];
   int i;

   if (active) {
      flags |= STATE_FLAG_MOVIE;
      if (movie_state_light)
         flags |= STATE_FLAG_LIGHT;
      else
         nframes = (u32)((framecounter < Movie.Frames) ? framecounter : Movie.Frames);

      if (BupRam != NULL && backup_file_size != 0) {
         bup_packed = (u8 *)malloc(backup_file_size + backup_file_size / 128 + 16);
         if (bup_packed != NULL) {
            bup_size = backup_file_size;
            bup_plen = MoviePack(BupRam, bup_size, bup_packed);
         }
      }
      if (CartBupSize() != 0) {
         const u32 csize = CartBupSize();
         cbup_packed = (u8 *)malloc(csize + csize / 128 + 16);
         if (cbup_packed != NULL) {
            cbup_size = csize;
            cbup_plen = MoviePack(CartridgeArea->bupram, cbup_size, cbup_packed);
         }
      }
   }

   total = STATE_HEAD + nframes * hdr.frame_len + bup_plen + cbup_plen;
   YabauseSyncStateGet(sync);

   memset(head, 0, sizeof(head));
   put32(head + 0, STATE_MAGIC_NEW);
   put32(head + 4, STATE_VERSION);
   put32(head + 8, total);
   put32(head + 12, flags);
   put32(head + 16, (u32)framecounter);
   put32(head + 20, (u32)lagframecounter);
   put32(head + 24, (u32)Movie.Rerecords);
   put32(head + 28, active ? hdr.header_size : 0);
   put32(head + 32, active ? hdr.frame_len : 0);
   put32(head + 36, active ? ((hdr.hash_invalid_from < nframes) ? hdr.hash_invalid_from : nframes) : 0);
   put32(head + 40, nframes);
   put32(head + 44, bup_size);
   put32(head + 48, bup_plen);
   put32(head + 52, cbup_size);
   put32(head + 56, cbup_plen);
   for (i = 0; i < YAB_SYNC_STATE_WORDS; i++)
      put64(head + 60 + 8 * i, sync[i]);

   /* stream == NULL : passe de dimensionnement, seule la taille compte */
   MemStateWrite(head, STATE_HEAD, 1, stream);
   if (nframes)
      MemStateWrite(mv_buf, hdr.frame_len, nframes, stream);
   if (bup_plen)
      MemStateWrite(bup_packed, 1, bup_plen, stream);
   if (cbup_plen)
      MemStateWrite(cbup_packed, 1, cbup_plen, stream);
   free(bup_packed);
   free(cbup_packed);
}

/* Remet une image PackBits de la RAM de sauvegarde venant d'un etat. */
static void MovieStateApplyBup(const u8 *packed, u32 plen, u32 size, u8 *dst, u32 dstsize)
{
   u8 *img;
   if (plen == 0 || size == 0 || dst == NULL || size != dstsize)
      return;
   img = (u8 *)malloc(size);
   if (img == NULL)
      return;
   if (MovieUnpack(packed, plen, img, size) == 0)
      memcpy(dst, img, size);
   free(img);
}

void MovieReadState(const void * stream, int available)
{
   const u8 *p = (const u8 *)stream + MemStateGetOffset();
   const int active = (Movie.Status == Recording || Movie.Status == Playback);
   const u8 *recs = NULL, *bup = NULL, *cbup = NULL;
   u32 flags = 0, frame = 0, nframes = 0, hashmark = 0;
   u32 s_header_size = 0, s_frame_len = 0;
   u32 bup_size = 0, bup_plen = 0, cbup_size = 0, cbup_plen = 0;
   int found = 0, has_extra = 0;
   u32 d, common;

   if (available >= STATE_HEAD && get32(p) == STATE_MAGIC_NEW && get32(p + 4) == STATE_VERSION) {
      const u32 total = get32(p + 8);
      u64 need;
      u64 sync[YAB_SYNC_STATE_WORDS];
      int i;

      flags = get32(p + 12);
      frame = get32(p + 16);
      s_header_size = get32(p + 28);
      s_frame_len = get32(p + 32);
      hashmark = get32(p + 36);
      nframes = get32(p + 40);
      bup_size = get32(p + 44);
      bup_plen = get32(p + 48);
      cbup_size = get32(p + 52);
      cbup_plen = get32(p + 56);
      need = (u64)STATE_HEAD + (u64)nframes * s_frame_len + (u64)bup_plen + (u64)cbup_plen;
      if (total <= (u32)available && need <= total) {
         found = 1;
         has_extra = 1;
         recs = p + STATE_HEAD;
         bup = recs + (size_t)nframes * s_frame_len;
         cbup = bup + bup_plen;
         /* synchronisation : toujours, film ou pas */
         for (i = 0; i < YAB_SYNC_STATE_WORDS; i++)
            sync[i] = get64(p + 60 + 8 * i);
         YabauseSyncStateSet(sync);
         framecounter = (int)frame;
         lagframecounter = (int)get32(p + 20);
      }
   } else if (available >= 12) {
      /* ancien bloc : magic, trame, taille, puis le fichier du film entier
         (ecrits dans l'ordre des octets de l'hote) */
      u32 magic, size;
      memcpy(&magic, p, 4);
      if (magic == MOVIE_STATE_MAGIC) {
         memcpy(&frame, p + 4, 4);
         memcpy(&size, p + 8, 4);
         if ((u64)size <= (u64)(available - 12) && size >= MOVIE_FIXED_SIZE &&
             memcmp(p + 12, MOVIE_MAGIC, 4) == 0) {
            const u8 *file = p + 12;
            found = 1;
            flags = STATE_FLAG_MOVIE;
            s_header_size = get32(file + 8);
            s_frame_len = get32(file + 12);
            if (s_frame_len != 0 && s_header_size <= size) {
               nframes = (size - s_header_size) / s_frame_len;
               if (nframes > frame)
                  nframes = frame;
               recs = file + s_header_size;
            }
            hashmark = nframes;
         }
      }
   }

   if (!active)
      return;

   if (!found || !(flags & STATE_FLAG_MOVIE)) {
      /* etat pris hors du film : la suite n'a plus de rapport avec lui */
      MovieMessage(_("Save state without movie data: movie stopped"));
      StopMovie();
      return;
   }
   if (s_header_size != hdr.header_size || s_frame_len != hdr.frame_len) {
      MovieMessage(_("Save state from another movie: movie stopped"));
      StopMovie();
      return;
   }

   /* RAM de sauvegarde de l'etat ; celle de l'utilisateur est mise de cote
      la premiere fois (elle revient a l'arret du film). */
   if (has_extra && (bup_plen != 0 || cbup_plen != 0)) {
      if (!bup_swapped)
         MovieSaveUserBackup();
      MovieStateApplyBup(bup, bup_plen, bup_size, BupRam, backup_file_size);
      if (CartBupSize() != 0)
         MovieStateApplyBup(cbup, cbup_plen, cbup_size, CartridgeArea->bupram, CartBupSize());
   }

   if (flags & STATE_FLAG_LIGHT) {
      /* instantane de MovieSeek() : le film ne change pas */
      framecounter = (int)frame;
      if (movie_desync >= (int)frame)
         movie_desync = -1;
      if (movie_stale_from != MOVIE_NOT_STALE && (int)frame <= movie_stale_from)
         movie_stale_from = MOVIE_NOT_STALE;
      return;
   }

   /* premiere trame ou les entrees de l'etat different du film actuel */
   common = nframes;
   if (common > (u32)Movie.Frames)
      common = (u32)Movie.Frames;
   for (d = 0; d < common; d++)
      if (!MovieSameInput(recs + (size_t)d * hdr.frame_len, MovieRec(d)))
         break;

   if (Movie.ReadOnly || movie_keep_on_load || !mv_writable) {
      /* le film reste tel quel : la lecture reprend a la trame de l'etat */
      if ((int)frame > Movie.Frames || nframes < frame) {
         MovieMessage(_("Save state is past the end of the movie: playback stopped"));
         StopMovie();
         return;
      }
      if (d < frame)
         MovieMessage(_("Save state made with other inputs (from frame %u): expect a desync"), d);
      if (Movie.Status == Recording)
         MovieSwitchToPlayback();
      framecounter = (int)frame;
      movie_desync = -1;
      if (d >= frame)
         movie_stale_from = MOVIE_NOT_STALE;
      return;
   }

   /* Reprise d'enregistrement (re-record) : les trames de l'etat, coupees a
      sa trame, remplacent le film. */
   if (nframes < frame || MovieReserve(nframes ? nframes : 1) != 0) {
      MovieMessage(_("Save state unusable for this movie: movie stopped"));
      StopMovie();
      return;
   }
   memcpy(mv_buf, recs, (size_t)nframes * hdr.frame_len);
   Movie.Frames = (int)nframes;
   if (hashmark > nframes)
      hashmark = nframes;
   hdr.hash_invalid_from = hashmark;
   MovieSnapInvalidateAfter((int)d);
   if (Movie.Status == Playback) {
      MovieSwitchToRecording();
      MovieMessage(_("Recording resumed at frame %u"), frame);
   }
   Movie.ReadOnly = 0;
   Movie.Rerecords++;
   framecounter = (int)frame;
   movie_desync = -1;
   movie_stale_from = MOVIE_NOT_STALE;
   strcpy(MovieStatus, "Recording Resumed");
   if (MovieRewriteFile(0) != 0) {
      MovieMessage(_("Couldn't rewrite the movie"));
      if (Movie.fp == NULL) {
         Movie.Status = Stopped;
         MovieLeaveDeterministic();
         MovieSnapClear();
         MovieFreeFrames();
         MovieRestoreUserBackup();
      }
   }
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
// Editeur d'entrees
//////////////////////////////////////////////////////////////////////////////

int MovieCanEdit(void)
{
   return ((Movie.Status == Recording || Movie.Status == Playback) && mv_writable && mv_buf != NULL);
}

int MovieEditFrameCount(void)
{
   return (Movie.Status == Recording || Movie.Status == Playback) ? Movie.Frames : 0;
}

int MovieEditPortLen(void)
{
   return (int)hdr.port_len;
}

int MovieEditGetPort(int frame, int port, u8 *data, int maxlen)
{
   const u8 *rec;
   int n;
   if (mv_buf == NULL || frame < 0 || frame >= MovieEditFrameCount() || port < 0 || port > 1)
      return -1;
   rec = MovieRec((u32)frame) + 1 + port * (1 + hdr.port_len);
   n = (int)hdr.port_len;
   if (n > maxlen)
      n = maxlen;
   if (data != NULL && n > 0)
      memcpy(data, rec + 1, (size_t)n);
   return rec[0];
}

int MovieEditGetLag(int frame)
{
   u8 flags;
   if (mv_buf == NULL || frame < 0 || frame >= MovieEditFrameCount())
      return -1;
   flags = MovieRec((u32)frame)[0];
   if (!(flags & REC_LAG_KNOWN))
      return -1;
   return (flags & REC_LAG) ? 1 : 0;
}

/* Les entrees des trames 'first' et suivantes ont change ; les empreintes
   restent valables jusqu'a 'hash_ok' exclu. */
static void MovieEdited(int first, u32 hash_ok)
{
   int i;
   if (hdr.hash_invalid_from > hash_ok)
      hdr.hash_invalid_from = hash_ok;
   MovieSnapInvalidateAfter(first);
   /* le lag des trames suivantes n'est plus connu */
   for (i = first + 1; i < Movie.Frames; i++)
      MovieRec((u32)i)[0] &= (u8)~(REC_LAG | REC_LAG_KNOWN);
   if (first < framecounter && first < movie_stale_from)
      movie_stale_from = first;
   if (movie_desync > first)
      movie_desync = -1;
}

/* Toute modification passe le film en lecture (lecture/ecriture) : a la fin
   du film, l'enregistrement reprend. */
static int MovieEditBegin(void)
{
   if (!MovieCanEdit())
      return -1;
   if (Movie.Status == Recording)
      MovieSwitchToPlayback();
   Movie.ReadOnly = 0;
   return 0;
}

int MovieEditSetPort(int frame, int port, int size, const u8 *data, int len)
{
   u8 *rec;
   if (MovieEditBegin() != 0 || frame < 0 || frame >= Movie.Frames || port < 0 || port > 1 || data == NULL)
      return -1;
   if (size < 0 || (u32)size > hdr.port_len || len < 0)
      return -1;
   if ((u32)len > hdr.port_len)
      len = (int)hdr.port_len;
   rec = MovieRec((u32)frame) + 1 + port * (1 + hdr.port_len);
   if (rec[0] == (u8)size && memcmp(rec + 1, data, (size_t)len) == 0)
      return 0;
   rec[0] = (u8)size;
   memcpy(rec + 1, data, (size_t)len);
   MovieEdited(frame, (u32)frame + 1);
   if (MovieFlushFrames((u32)frame, 1) != 0)
      return -1;
   MovieWriteCounts(1);
   return 0;
}

int MovieEditGetButton(int frame, int port, int button)
{
   u8 data[4];
   const int size = MovieEditGetPort(frame, port, data, 4);
   if (size < 0 || button < 0 || button >= MOVIE_PAD_BUTTONS || !MoviePortIsPad(data, size))
      return -1;
   return (data[pad_buttons[button].byte] & pad_buttons[button].mask) ? 0 : 1;
}

int MovieEditSetButton(int frame, int port, int button, int pressed)
{
   u8 data[MOVIE_MAX_PORT_LEN];
   int size;
   if (button < 0 || button >= MOVIE_PAD_BUTTONS)
      return -1;
   size = MovieEditGetPort(frame, port, data, (int)sizeof(data));
   if (size < 0 || !MoviePortIsPad(data, size))
      return -1;
   if (pressed)
      data[pad_buttons[button].byte] &= (u8)~pad_buttons[button].mask;
   else
      data[pad_buttons[button].byte] |= pad_buttons[button].mask;
   return MovieEditSetPort(frame, port, size, data, (int)hdr.port_len);
}

char MovieEditButtonLetter(int button)
{
   if (button < 0 || button >= MOVIE_PAD_BUTTONS)
      return '?';
   return pad_buttons[button].letter;
}

/* Insere 'count' trames sans bouton appuye avant la trame 'at' (at ==
   nombre de trames : a la fin). Le peripherique est celui de la trame
   voisine. */
int MovieEditInsert(int at, int count)
{
   u8 *tmpl;
   int i, port;

   if (MovieEditBegin() != 0 || at < 0 || at > Movie.Frames || count <= 0 || count > 1000000)
      return -1;
   if (MovieReserve((u32)Movie.Frames + (u32)count) != 0)
      return -1;

   tmpl = (u8 *)calloc(1, hdr.frame_len);
   if (tmpl == NULL)
      return -1;
   if (Movie.Frames > 0) {
      memcpy(tmpl, MovieRec((u32)(at > 0 ? at - 1 : 0)), hdr.frame_len);
   } else {
      MovieWritePort(tmpl + 1, movie_ports_saved ? &movie_saved_port1 : &PORTDATA1);
      MovieWritePort(tmpl + 2 + hdr.port_len, movie_ports_saved ? &movie_saved_port2 : &PORTDATA2);
   }
   tmpl[0] = 0;
   put32(tmpl + hdr.frame_len - 4, 0);
   for (port = 0; port < 2; port++) {
      u8 *pd = tmpl + 1 + port * (1 + hdr.port_len);
      if (MoviePortIsPad(pd + 1, pd[0])) {
         pd[3] = 0xFF;
         pd[4] = 0xFF;
      }
   }

   memmove(MovieRec((u32)(at + count)), MovieRec((u32)at), (size_t)(Movie.Frames - at) * hdr.frame_len);
   for (i = 0; i < count; i++)
      memcpy(MovieRec((u32)(at + i)), tmpl, hdr.frame_len);
   free(tmpl);
   Movie.Frames += count;
   MovieEdited(at, (u32)at);
   return MovieRewriteFile(1);
}

int MovieEditDelete(int at, int count)
{
   if (MovieEditBegin() != 0 || at < 0 || count <= 0 || at >= Movie.Frames)
      return -1;
   if (count > Movie.Frames - at)
      count = Movie.Frames - at;
   memmove(MovieRec((u32)at), MovieRec((u32)(at + count)), (size_t)(Movie.Frames - at - count) * hdr.frame_len);
   Movie.Frames -= count;
   MovieEdited(at, (u32)at);
   return MovieRewriteFile(1);
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

int MovieGetFrameCounter(void)
{
   return framecounter;
}

int MovieGetLagCount(void)
{
   return lagframecounter;
}

int MovieLastFrameWasLag(void)
{
   return movie_last_lag;
}

int MovieGetRerecords(void)
{
   return Movie.Rerecords;
}

int MovieIsStale(void)
{
   return IsMovieLoaded() && movie_stale_from != MOVIE_NOT_STALE;
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
