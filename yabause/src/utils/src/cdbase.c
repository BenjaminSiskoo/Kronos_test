/*  Copyright 2004-2008, 2013 Theo Berkau
    Copyright 2005 Joost Peters
    Copyright 2005-2006 Guillaume Duhamel

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

/*! \file cdbase.c
    \brief Dummy and ISO, BIN/CUE, MDS CD Interfaces
*/
#define _GNU_SOURCE
#include <string.h>
#ifndef WIN32
#include <strings.h>
#include <dirent.h>
#endif
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <wchar.h>
#include "cdbase.h"
#include "yui.h"
#include "error.h"
#include "debug.h"
#include "junzip.h"
#include "zlib.h"
#include "chd.h"
#include "threads.h"

/* Thread slot of the zip inflate worker. YAB_THREAD_CS_CMD_1 is declared in
   every version of threads.h and is not used anywhere else, so no change to
   threads.h is needed. */
#define CD_ZIP_THREAD_ID YAB_THREAD_CS_CMD_1

#ifndef HAVE_STRICMP
#ifdef HAVE_STRCASECMP
#define stricmp strcasecmp
#endif
#endif

#ifndef HAVE_WFOPEN
static char * wcsdupstr(const wchar_t * path)
{
   char * mbs;
   size_t len = wcstombs(NULL, path, 0);
   if (len == (size_t) -1) return NULL;

   mbs = (char*)calloc(1, len);
   len = wcstombs(mbs, path, len);
   if (len == (size_t) -1)
   {
      free(mbs);
      return NULL;
   }

   return mbs;
}

static FILE * _wfopen(const wchar_t *wpath, const wchar_t *wmode)
{
   FILE * fd;
   char * path;
   char * mode;

   path = wcsdupstr(wpath);
   if (path == NULL) return NULL;

   mode = wcsdupstr(wmode);
   if (mode == NULL)
   {
      free(path);
      return NULL;
   }

   fd = fopen(path, mode);

   free(path);
   free(mode);

   return fd;
}
#endif

//////////////////////////////////////////////////////////////////////////////

// Contains the Dummy and ISO CD Interfaces

static int DummyCDInit(const char *);
static void DummyCDDeInit(void);
static int DummyCDGetStatus(void);
static s32 DummyCDReadTOC(u32 *);
static int DummyCDReadSectorFAD(u32, void *);
static void DummyCDReadAheadFAD(u32);
static void DummyCDSetStatus(int status );

CDInterface DummyCD = {
CDCORE_DUMMY,
"Dummy CD Drive",
DummyCDInit,
DummyCDDeInit,
DummyCDGetStatus,
DummyCDReadTOC,
DummyCDReadSectorFAD,
DummyCDReadAheadFAD,
DummyCDSetStatus,
};

static int ISOCDInit(const char *);
static void ISOCDDeInit(void);
static int ISOCDGetStatus(void);
static s32 ISOCDReadTOC(u32 *);
static int ISOCDReadSectorFAD(u32, void *);
static void ISOCDReadAheadFAD(u32);
static void ISOCDSetStatus(int status);

static int ISOCDInitStatus = 0;

CDInterface ISOCD = {
CDCORE_ISO,
"ISO-File Virtual Drive",
ISOCDInit,
ISOCDDeInit,
ISOCDGetStatus,
ISOCDReadTOC,
ISOCDReadSectorFAD,
ISOCDReadAheadFAD,
ISOCDSetStatus,
};

static int dmy_status = 2;
//////////////////////////////////////////////////////////////////////////////
// Dummy Interface
//////////////////////////////////////////////////////////////////////////////

static int DummyCDInit(UNUSED const char *cdrom_name)
{
	// Initialization function. cdrom_name can be whatever you want it to
	// be. Obviously with some ports(e.g. the dreamcast port) you probably
	// won't even use it.
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void DummyCDDeInit(void)
{
	// Cleanup function. Enough said.
}

//////////////////////////////////////////////////////////////////////////////

static int DummyCDGetStatus(void)
{
	// This function is called periodically to see what the status of the
	// drive is.
	//
	// Should return one of the following values:
	// 0 - CD Present, disc spinning
	// 1 - CD Present, disc not spinning
	// 2 - CD not present
	// 3 - Tray open
	//
	// If you really don't want to bother too much with this function, just
	// return status 0. Though it is kind of nice when the bios's cd
	// player, etc. recognizes when you've ejected the tray and popped in
	// another disc.

	return dmy_status;
}

static void DummyCDSetStatus(int status){
  dmy_status = status;
  if (dmy_status != 3) {
    dmy_status = 2;
  }
	return;
}

//////////////////////////////////////////////////////////////////////////////

static s32 DummyCDReadTOC(UNUSED u32 *TOC)
{
	// The format of TOC is as follows:
	// TOC[0] - TOC[98] are meant for tracks 1-99. Each entry has the
	// following format:
	// bits 0 - 23: track FAD address
	// bits 24 - 27: track addr
	// bits 28 - 31: track ctrl
	//
	// Any Unused tracks should be set to 0xFFFFFFFF
	//
	// TOC[99] - Point A0 information
	// Uses the following format:
	// bits 0 - 7: PFRAME(should always be 0)
	// bits 7 - 15: PSEC(Program area format: 0x00 - CDDA or CDROM,
	//                   0x10 - CDI, 0x20 - CDROM-XA)
	// bits 16 - 23: PMIN(first track's number)
	// bits 24 - 27: first track's addr
	// bits 28 - 31: first track's ctrl
	//
	// TOC[100] - Point A1 information
	// Uses the following format:
	// bits 0 - 7: PFRAME(should always be 0)
	// bits 7 - 15: PSEC(should always be 0)
	// bits 16 - 23: PMIN(last track's number)
	// bits 24 - 27: last track's addr
	// bits 28 - 31: last track's ctrl
	//
	// TOC[101] - Point A2 information
	// Uses the following format:
	// bits 0 - 23: leadout FAD address
	// bits 24 - 27: leadout's addr
	// bits 28 - 31: leadout's ctrl
	//
	// Special Note: To convert from LBA/LSN to FAD, add 150.

	return 0;
}

//////////////////////////////////////////////////////////////////////////////

static int DummyCDReadSectorFAD(UNUSED u32 FAD, void * buffer)
{
	// This function is supposed to read exactly 1 -RAW- 2352-byte sector
	// at the specified FAD address to buffer. Should return true if
	// successful, false if there was an error.
	//
	// Special Note: To convert from FAD to LBA/LSN, minus 150.
	//
	// The whole process needed to be changed since I need more control
	// over sector detection, etc. Not to mention it means less work for
	// the porter since they only have to implement raw sector reading as
	// opposed to implementing mode 1, mode 2 form1/form2, -and- raw
	// sector reading.

	memset(buffer, 0, 2352);

	return 1;
}

//////////////////////////////////////////////////////////////////////////////

static void DummyCDReadAheadFAD(UNUSED u32 FAD)
{
	// This function is called to tell the driver which sector (FAD
	// address) is expected to be read next. If the driver supports
	// read-ahead, it should start reading the given sector in the
	// background while the emulation continues, so that when the
	// sector is actually read with ReadSectorFAD() it'll be available
	// immediately. (Note that there's no guarantee this sector will
	// actually be requested--the emulated CD might be stopped before
	// the sector is read, for example.)
	//
	// This function should NOT block. If the driver can't perform
	// asynchronous reads (or you just don't want to bother handling
	// them), make this function a no-op and just read sectors
	// normally.
}

//////////////////////////////////////////////////////////////////////////////
// ISO Interface
//////////////////////////////////////////////////////////////////////////////

typedef struct
{
   u8 ctl_addr;
   u32 fad_start;
   u32 fad_end;
   // Premier FAD lisible dans le fichier de la piste (INDEX 00 / pre-gap
   // stocke dans le fichier). 0 = identique a fad_start. Utilise seulement
   // pour localiser un secteur, jamais pour la TOC (qui reste sur INDEX 01).
   u32 fad_lookup_start;
   u32 file_offset;
   u32 sector_size;
   FILE *fp;
   int file_size;
   int file_id;
   int interleaved_sub;
   u32 frames;
   u32 extraframes;
   u32 pregap;
   u32 postgap;
   u32 physframeofs;
   u32 chdframeofs;
   u32 logframeofs;
   int isZip;
   char* filename;
   ZipEntry* tr;
   int sync_checked;      /* raw data track: start offset already validated */
   int scrambled;         /* raw sectors are stored scrambled (ECMA-130) */
} track_info_struct;

typedef struct
{
   u32 fad_start;
   u32 fad_end;
   track_info_struct *track;
   int track_num;
} session_info_struct;

typedef struct
{
   int session_num;
   JZFile *zip;
   JZEndRecord* endRecord;
   session_info_struct *session;
} disc_info_struct;

#pragma pack(push, 1)
typedef struct
{
   u8 signature[16];
   u8 version[2];
   u16 medium_type;
   u16 session_count;
   u16 unused1[2];
   u16 bca_length;
   u32 unused2[2];
   u32 bca_offset;
   u32 unused3[6];
   u32 disk_struct_offset;
   u32 unused4[3];
   u32 sessions_blocks_offset;
   u32 dpm_blocks_offset;
   u32 enc_key_offset;
} mds_header_struct;

typedef struct
{
   s32 session_start;
   s32 session_end;
   u16 session_number;
   u8 total_blocks;
   u8 leadin_blocks;
   u16 first_track;
   u16 last_track;
   u32 unused;
   u32 track_blocks_offset;
} mds_session_struct;

typedef struct
{
   u8 mode;
   u8 subchannel_mode;
   u8 addr_ctl;
   u8 unused1;
   u8 track_num;
   u32 unused2;
   u8 m;
   u8 s;
   u8 f;
   u32 extra_offset;
   u16 sector_size;
   u8 unused3[18];
   u32 start_sector;
   u64 start_offset;
   u8 session;
   u8 unused4[3];
   u32 footer_offset;
   u8 unused5[24];
} mds_track_struct;

typedef struct
{
   u32 filename_offset;
   u32 is_widechar;
   u32 unused1;
   u32 unused2;
} mds_footer_struct;

#pragma pack(pop)

#define CCD_MAX_SECTION 20
#define CCD_MAX_NAME 30
#define CCD_MAX_VALUE 20

typedef struct
{
	char section[CCD_MAX_SECTION];
	char name[CCD_MAX_NAME];
	char value[CCD_MAX_VALUE];
} ccd_dict_struct;

typedef struct
{
	ccd_dict_struct *dict;
	int num_dict;
} ccd_struct;

static const u8 syncHdr[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
enum IMG_TYPE { IMG_NONE, IMG_ISO, IMG_BINCUE, IMG_MDS, IMG_CCD, IMG_CHD, IMG_NRG };
enum IMG_TYPE imgtype = IMG_ISO;
static u32 isoTOC[102];
static disc_info_struct disc;
static int iso_cd_status = 0;

static int current_file_id = 0;

#define MSF_TO_FAD(m,s,f) ((m * 4500) + (s * 75) + f)

//////////////////////////////////////////////////////////////////////////////
static int shallBeEscaped(char c) {
  return ((c=='\\'));
}

static int charToEscape(char *buffer) {
  int i;
  int ret = 0;
  for (i=0; i<strlen(buffer); i++) {
    if(shallBeEscaped(buffer[i])) ret++;
  }
  return ret;
}

#ifndef WIN32
static FILE* fopenInPath(char* filename, char* path){
  int nbFiles,i,k;
  char* tmp;
  int l = strlen(filename) + 2;
  struct dirent **fileListTemp;
  nbFiles = scandir(path, &fileListTemp, NULL, alphasort);
  for(i = 0; i < nbFiles; i++){
    if (strncasecmp(filename, fileListTemp[i]->d_name, l) == 0) {
      int p= (l + charToEscape(filename) + charToEscape(path)+strlen(path));
      char* filepath = (char*)calloc(1, p*sizeof(char));
      tmp = filepath;
      for (k=0; k<strlen(path); k++) {
        if (shallBeEscaped(path[k])) *tmp++='\\';
           *tmp++ = path[k];
      }
      *tmp++ = '/';
      for (k=0; k<strlen(fileListTemp[i]->d_name); k++) {
        if (shallBeEscaped(fileListTemp[i]->d_name[k])) *tmp++='\\';
           *tmp++ = fileListTemp[i]->d_name[k];
      }
      *tmp++ = '\0';
      return fopen(filepath,"rb");
    }
  }
  return NULL;

}
#else
static FILE* fopenInPath(char* filename, char* path){
  size_t l = strlen(filename)+2;
  int k;
  char* filepath = (char*)calloc(1, (l + charToEscape(filename) + charToEscape(path)+strlen(path))*sizeof(char));
  char* tmp;
  tmp = filepath;
  for (k=0; k<strlen(path); k++) {
    if (shallBeEscaped(path[k])) *tmp++='\\';
    *tmp++ = path[k];
  }
  *tmp++ = '\\';
  for (k=0; k<strlen(filename); k++) {
    if (shallBeEscaped(filename[k])) *tmp++='\\';
    *tmp++ = filename[k];
  }
  *tmp++ = '\0';
  return fopen(filepath,"rb");
}
#endif

static FILE* OpenFile(char* buffer, const char* cue) {
   char *filename, *endofpath;
   char *path;
   int tmp;
   FILE *ret_file = NULL;
   // Now go and open up the image file, figure out its size, etc.
   if ((ret_file = fopen(buffer, "rb")) == NULL)
   {
      // Ok, exact path didn't work. Let's trim the path and try opening the
      // file from the same directory as the cue.

      // find the start of filename
      filename = buffer;
      for (tmp=0; tmp < strlen(buffer); tmp++)
      {
         if ((buffer[tmp] == '/') || (buffer[tmp] == '\\'))
           filename = &buffer[tmp+1];
      }

      // append directory of cue file with bin filename
      // find end of path
      endofpath = (char*)cue;
      for (tmp=0; tmp < strlen(cue); tmp++)
      {
         if ((cue[tmp] == '/') || (cue[tmp] == '\\'))
           endofpath = (char*)&cue[tmp];
      }

      if ((path = (char *)calloc((endofpath - cue +1)*sizeof(char), 1)) == NULL)
      {
         return NULL;
      }
      strncpy(path, cue, endofpath - cue);

      // Let's give it another try
      ret_file = fopenInPath(filename, path);
      free(path);
      if (ret_file == NULL)
      {
         YabSetError(YAB_ERR_FILENOTFOUND, buffer);
      }
   }
   return ret_file;
}

static int LoadCHD(const char *chd_filename, FILE *iso_file);
static int ISOCDReadSectorFADFromCHD(u32 FAD, void *buffer);

// Nombre de secteurs de la piste presents dans son fichier a partir de
// INDEX 01 (file_offset).
static u32 BinCueTrackSectors(const track_info_struct *t)
{
   if (t->sector_size == 0 || t->file_size <= (int)t->file_offset)
      return 0;
   return (u32)((t->file_size - t->file_offset) / t->sector_size);
}

// Fin de piste (inclusive) bornee au contenu reel de son fichier.
static u32 BinCueTrackFileEnd(const track_info_struct *t)
{
   u32 n = BinCueTrackSectors(t);
   return (n > 0) ? (t->fad_start + n - 1) : t->fad_start;
}

// Borne basse de recherche des secteurs : la zone INDEX 00 d'une piste qui
// ouvre un nouveau FILE est stockee au debut de ce fichier (avant
// file_offset). Elle appartient donc a cette piste et non a la precedente.
// Un PREGAP (non stocke) n'est pas couvert : on ne descend jamais sous le
// debut du fichier.
static void BinCueSetLookupStart(track_info_struct *trk, unsigned int track_num)
{
   unsigned int i;
   for (i = 0; i < track_num; i++)
   {
      track_info_struct *t = &trk[i];
      u32 lo = t->fad_start;
      if (i > 0 && t->file_id != trk[i-1].file_id && t->sector_size != 0)
      {
         u32 in_file = t->file_offset / t->sector_size;   // secteurs avant INDEX 01
         u32 file_start = (t->fad_start >= in_file) ? (t->fad_start - in_file) : 0;
         u32 after_prev = trk[i-1].fad_end + 1;
         lo = (file_start > after_prev) ? file_start : after_prev;
         if (lo > t->fad_start) lo = t->fad_start;
      }
      t->fad_lookup_start = lo;
   }
}

static int LoadBinCue(const char *cuefilename, FILE *iso_file)
{
   long size;
   char* temp_buffer;
   unsigned int track_num = 0;
   unsigned int indexnum, min, sec, frame;
   unsigned int pregap=0;
   track_info_struct trk[100];
   int i;
   int matched = 0;
   FILE *trackfp = NULL;
   int trackfp_size = 0;
   int fad = 150;
   // FAD du debut du FILE courant. Les temps INDEX sont relatifs au debut du
   // fichier : pour un single-BIN (un seul FILE, temps INDEX absolus type Redump)
   // ce base reste 150 ; pour un multi-BIN il avance a la fin de piste precedente.
   int file_base_fad = 150;

	memset(trk, 0, sizeof(trk));
   disc.session = (session_info_struct*)calloc(1, sizeof(session_info_struct));
   if (disc.session == NULL)
   {
      YabSetError(YAB_ERR_MEMORYALLOC, NULL);
      return -1;
   }
   disc.session_num = 1;

   fseek(iso_file, 0, SEEK_END);
   size = ftell(iso_file);

   if(size <= 0)
   {
      YabSetError(YAB_ERR_FILEREAD, cuefilename);
      return -1;
   }

   fseek(iso_file, 0, SEEK_SET);

   // Allocate buffer with enough space for reading cue
   if ((temp_buffer = (char *)calloc(size, 1)) == NULL)
      return -1;

   // Time to generate TOC
   for (;;)
   {
      // Retrieve a line in cue
      if (fscanf(iso_file, "%s", temp_buffer) == EOF)
         break;

      if (strncmp(temp_buffer, "FILE", 4) == 0)
      {
         matched = fscanf(iso_file, " \"%[^\"]\"", temp_buffer);
         trackfp = OpenFile(temp_buffer, cuefilename);
         if (trackfp == NULL) {
           printf("Can not open file %s\n", temp_buffer);
           free(temp_buffer);
           return -1;
         }
         fseek(trackfp, 0, SEEK_END);
         trackfp_size = ftell(trackfp);
         fseek(trackfp, 0, SEEK_SET);
         current_file_id++;
         if(track_num>0) {
           // La piste precedente s'arrete a la fin de SON fichier (inclusive) ;
           // le nouveau FILE commence au FAD suivant.
           trk[track_num-1].fad_end = BinCueTrackFileEnd(&trk[track_num-1]);
           fad = trk[track_num-1].fad_start + BinCueTrackSectors(&trk[track_num-1]);
         }
         file_base_fad = fad; // nouveau FILE -> nouvelle base pour les temps INDEX
         continue;
      }

      // Figure out what it is
      if (strncmp(temp_buffer, "TRACK", 5) == 0)
      {
         // Handle accordingly
         if (fscanf(iso_file, "%d %[^\r\n]\r\n", &track_num, temp_buffer) == EOF)
            break;

         trk[track_num-1].fp = trackfp;
         trk[track_num-1].file_size = trackfp_size;
         trk[track_num-1].file_id = current_file_id;

         if (strncmp(temp_buffer, "MODE1", 5) == 0 ||
            strncmp(temp_buffer, "MODE2", 5) == 0)
         {
            // Figure out the track sector size
            trk[track_num-1].sector_size = atoi(temp_buffer + 6);
            trk[track_num-1].ctl_addr = 0x41;
         }
         else if (strncmp(temp_buffer, "AUDIO", 5) == 0)
         {
            // Update toc entry
            trk[track_num-1].sector_size = 2352;
            trk[track_num-1].ctl_addr = 0x01;
         }
      }
      else if (strncmp(temp_buffer, "INDEX", 5) == 0)
      {
         // Handle accordingly

         if (fscanf(iso_file, "%d %d:%d:%d\r\n", &indexnum, &min, &sec, &frame) == EOF)
            break;

         if (indexnum == 1)
         {
            // Update toc entry
            // FAD = base du FILE courant + temps INDEX (relatif au fichier) + pregap.
            // Etait "fad += ..." (accumulation) -> faussait les FAD des pistes 3+ d'un
            // single-BIN (Redump). Equivalent au multi-BIN (file_base = fad_end precedent).
            fad = file_base_fad + MSF_TO_FAD(min, sec, frame) + pregap;
            trk[track_num-1].fad_start = fad;
            trk[track_num-1].file_offset = MSF_TO_FAD(min, sec, frame) * trk[track_num-1].sector_size;
            CDLOG("Start[%d] %d\n", track_num, trk[track_num-1].fad_start);
            // Meme fichier : la piste precedente s'etend jusqu'a INDEX 01 de
            // celle-ci (son pre-gap INDEX 00 est dans ce meme fichier, a la
            // suite). Fichier different : sa fin a deja ete fixee a la fin de
            // son propre fichier lors de la ligne FILE ; le pre-gap INDEX 00
            // de cette piste est au debut du nouveau fichier et lui appartient.
            if (track_num > 1 && trk[track_num-2].file_id == trk[track_num-1].file_id) {
              trk[track_num-2].fad_end = trk[track_num-1].fad_start-1;
              CDLOG("End[%d] %d\n", track_num-1, trk[track_num-2].fad_end);
            }
            // PREGAP ne s'applique qu'a la piste qui vient d'etre positionnee : une
            // fois consomme il doit repartir a 0, sinon toute piste SUIVANTE sans
            // PREGAP propre herite a tort du gap de la precedente (et les pistes
            // qui declarent chacune leur propre PREGAP -- convention Redump courante
            // pour des CDDA consecutives -- accumulent l'erreur piste apres piste :
            // +2s, +4s, +6s..., ce qui finit par placer fad_start hors de la zone
            // reelle du fichier -> pistes audio muettes/desalignees en fin de disque).
            pregap = 0;
         }
      }
      else if (strncmp(temp_buffer, "PREGAP", 6) == 0)
      {
         if (fscanf(iso_file, "%d:%d:%d\r\n", &min, &sec, &frame) == EOF)
            break;

         pregap += MSF_TO_FAD(min, sec, frame);
      }
      else if (strncmp(temp_buffer, "POSTGAP", 7) == 0)
      {
         if (fscanf(iso_file, "%d:%d:%d\r\n", &min, &sec, &frame) == EOF)
            break;
      }
   }

   trk[track_num].file_offset = 0;
   trk[track_num].fad_start = 0xFFFFFFFF;

   if (track_num == 0) {
     YabSetError(YAB_ERR_FILENOTFOUND, NULL);
     free(disc.session);
     disc.session = NULL;
     disc.session_num = 0;
     return -1;
   }

   // Derniere piste : fin inclusive ; le lead-out (session.fad_end) est le
   // FAD qui suit le dernier secteur (valeur inchangee pour la TOC).
   trk[track_num-1].fad_end = BinCueTrackFileEnd(&trk[track_num-1]);
   BinCueSetLookupStart(trk, track_num);

   //for (int i =0; i<track_num; i++) printf("Track %d [%d - %d]\n", i+1, trk[i].fad_start, trk[i].fad_end);

   disc.session[0].fad_start = 150;
   disc.session[0].fad_end = trk[track_num-1].fad_start + BinCueTrackSectors(&trk[track_num-1]);
   disc.session[0].track_num = track_num;
   disc.session[0].track = (track_info_struct*)calloc(1, sizeof(track_info_struct) * disc.session[0].track_num);
   if (disc.session[0].track == NULL)
   {
      YabSetError(YAB_ERR_MEMORYALLOC, NULL);
      free(disc.session);
      disc.session = NULL;
      disc.session_num = 0;
      return -1;
   }

   memcpy(disc.session[0].track, trk, track_num * sizeof(track_info_struct));

   // buffer is no longer needed
   free(temp_buffer);

   fclose(iso_file);
   return 0;
}


//////////////////////////////////////////////////////////////////////////////
// Zipped BIN/CUE: progressive decompression
//
// A deflate stream cannot be seeked, so a zipped track has to be inflated
// from its first byte. The loader used to inflate EVERY file of the archive
// to the end before returning (hundreds of MB, on the emulation thread, with
// one full scan of the central directory per FILE line of the cue).
//
// Now the central directory is read once, only the cue is inflated at load
// time, and the tracks are inflated by a background thread, in cue order,
// into buffers whose final size is known from the directory. A read only
// waits until the bytes it needs are there: the head of track 1 is ready
// within milliseconds, so the disc boots at once. When a read targets a
// track that is not the one being inflated (CD-DA track, far seek), the
// worker switches to that track first; every stream keeps its own inflate
// state so the interrupted one is simply resumed later.
//
// Without thread support (YabThreadStart() failing, thr-dummy.c) the same
// streams are inflated on demand by the reader itself.
//////////////////////////////////////////////////////////////////////////////

#define ZIP_IN_CHUNK   (256 * 1024)    // compressed bytes read per fread()
#define ZIP_OUT_CHUNK  (1024 * 1024)   // inflated bytes published per step

typedef struct
{
   char *name;
   u32 offset;      // local file header offset
   u32 compSize;
   u32 size;
   u16 method;
} ZipDirEntry;

typedef struct
{
   ZipDirEntry *ent;
   int count;
   int cap;
} ZipDir;

typedef struct
{
   ZipEntry e;      // MUST stay first: track_info_struct.tr points to it
   u32 localOffset;
   u32 compSize;
   u16 method;
   // Private to whoever runs ZipStreamStep() (the worker, or the reader
   // when there is no worker)
   int started;
   int strmInit;
   u32 filePos;     // next compressed byte in the archive
   u32 compLeft;
   u32 produced;
   z_stream strm;
   u8 *inbuf;
   // Shared between the worker and the reader, under zipldr.mtx
   u32 avail;       // bytes of e.zipBuffer that are valid
   int done;        // stream finished (completely, or on error)
   int error;
} ZipStream;

static struct
{
   ZipStream **stream;
   int count;
   FILE *fp;            // the archive; only ZipStreamStep() touches it
   YabMutex *mtx;
   int threaded;
   int quit;
   ZipStream *wanted;   // stream the reader is waiting for
} zipldr;

static int ZipDirCollect(JZFile *zip, int idx, JZFileHeader *header, char *filename, void *user_data)
{
   ZipDir *dir = (ZipDir *)user_data;
   ZipDirEntry *d;

   (void)zip; (void)idx;
   if (dir->count == dir->cap)
   {
      int cap = dir->cap ? dir->cap * 2 : 64;
      ZipDirEntry *n = (ZipDirEntry *)realloc(dir->ent, cap * sizeof(ZipDirEntry));
      if (n == NULL) return 0; // abort
      dir->ent = n;
      dir->cap = cap;
   }
   d = &dir->ent[dir->count];
   if ((d->name = strdup(filename)) == NULL) return 0;
   d->offset = header->offset;
   d->compSize = header->compressedSize;
   d->size = header->uncompressedSize;
   d->method = header->compressionMethod;
   dir->count++;
   return 1; // continue
}

static void ZipDirFree(ZipDir *dir)
{
   int i;
   for (i = 0; i < dir->count; i++) free(dir->ent[i].name);
   free(dir->ent);
   memset(dir, 0, sizeof(*dir));
}

static const char *ZipBaseName(const char *name)
{
   const char *s = strrchr(name, '/');
   const char *b = strrchr(name, '\\');
   if (b != NULL && (s == NULL || b > s)) s = b;
   return s ? s + 1 : name;
}

static int ZipNameEqualNoCase(const char *a, const char *b)
{
   for (; *a && *b; a++, b++)
      if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
   return *a == *b;
}

// filename == NULL: first entry whose name ends with ".cue" (any case).
// Otherwise the entry with that base name: exact match first, then
// ignoring case (cue sheets written on Windows).
static ZipDirEntry *ZipDirFind(ZipDir *dir, const char *filename)
{
   int i;
   if (filename == NULL)
   {
      for (i = 0; i < dir->count; i++)
      {
         size_t l = strlen(dir->ent[i].name);
         if (l >= 4 && ZipNameEqualNoCase(dir->ent[i].name + l - 4, ".cue"))
            return &dir->ent[i];
      }
      return NULL;
   }
   filename = ZipBaseName(filename);
   for (i = 0; i < dir->count; i++)
      if (strcmp(ZipBaseName(dir->ent[i].name), filename) == 0) return &dir->ent[i];
   for (i = 0; i < dir->count; i++)
      if (ZipNameEqualNoCase(ZipBaseName(dir->ent[i].name), filename)) return &dir->ent[i];
   return NULL;
}

// The buffer is zero-filled and one byte longer than the file: whatever is
// not inflated (yet, or ever if the archive is damaged) reads as zeroes,
// and the cue sheet is a NUL-terminated string.
static ZipStream *ZipStreamNew(const ZipDirEntry *d)
{
   ZipStream *s;
   ZipStream **list;

   if (d->method != 0 && d->method != 8)
   {
      YuiMsg("[CD] zip: \"%s\" uses unsupported compression method %u (only store/deflate)\n", d->name, d->method);
      return NULL;
   }
   list = (ZipStream **)realloc(zipldr.stream, (zipldr.count + 1) * sizeof(ZipStream *));
   if (list == NULL) return NULL;
   zipldr.stream = list;
   if ((s = (ZipStream *)calloc(1, sizeof(ZipStream))) == NULL) return NULL;
   s->e.filename = strdup(ZipBaseName(d->name));
   s->e.size = d->size;
   s->e.zipBuffer = (u8 *)calloc(1, (size_t)d->size + 1);
   if (s->e.zipBuffer == NULL)
   {
      free(s->e.filename);
      free(s);
      return NULL;
   }
   s->localOffset = d->offset;
   s->compSize = d->compSize;
   s->method = d->method;
   zipldr.stream[zipldr.count++] = s;
   return s;
}

static void ZipStreamEnd(ZipStream *s)
{
   if (s->strmInit) inflateEnd(&s->strm);
   s->strmInit = 0;
   free(s->inbuf);
   s->inbuf = NULL;
}

// Inflate (or copy, for a stored entry) up to ZIP_OUT_CHUNK more bytes.
// Returns the number of bytes added to the buffer; *fin is set when the
// stream is over, *err when it ended on an error. Never called by two
// threads at once (see zipldr.threaded).
static u32 ZipStreamStep(ZipStream *s, int *fin, int *err)
{
   FILE *fp = zipldr.fp;
   u32 want, got = 0;
   int ended = 0;

   *fin = 0;
   *err = 0;

   if (!s->started)
   {
      u8 lh[30];
      s->started = 1;
      // The sizes come from the central directory: the local header may
      // hold zeroes (data descriptor, general purpose bit 3). It is only
      // needed to find where the data starts.
      if (fseek(fp, (long)s->localOffset, SEEK_SET) != 0 || fread(lh, 1, 30, fp) != 30 ||
          lh[0] != 'P' || lh[1] != 'K' || lh[2] != 3 || lh[3] != 4)
         goto fail;
      s->filePos = s->localOffset + 30 + (lh[26] | (lh[27] << 8)) + (lh[28] | (lh[29] << 8));
      s->compLeft = s->compSize;
      if (s->method == 8)
      {
         memset(&s->strm, 0, sizeof(s->strm));
         if (inflateInit2(&s->strm, -MAX_WBITS) != Z_OK) goto fail;
         s->strmInit = 1;
         if ((s->inbuf = (u8 *)malloc(ZIP_IN_CHUNK)) == NULL) goto fail;
      }
   }

   want = s->e.size - s->produced;
   if (want > ZIP_OUT_CHUNK) want = ZIP_OUT_CHUNK;

   if (want != 0 && s->method == 0)
   {
      if (fseek(fp, (long)s->filePos, SEEK_SET) != 0) goto fail;
      got = (u32)fread(s->e.zipBuffer + s->produced, 1, want, fp);
      if (got == 0) goto fail;
      s->filePos += got;
   }
   else if (want != 0)
   {
      s->strm.next_out = s->e.zipBuffer + s->produced;
      s->strm.avail_out = want;
      while (s->strm.avail_out > 0)
      {
         int ret;
         if (s->strm.avail_in == 0)
         {
            u32 n = (s->compLeft < ZIP_IN_CHUNK) ? s->compLeft : ZIP_IN_CHUNK;
            if (n == 0 || fseek(fp, (long)s->filePos, SEEK_SET) != 0) break;
            n = (u32)fread(s->inbuf, 1, n, fp);
            if (n == 0) break;
            s->filePos += n;
            s->compLeft -= n;
            s->strm.next_in = s->inbuf;
            s->strm.avail_in = n;
         }
         ret = inflate(&s->strm, Z_NO_FLUSH);
         if (ret == Z_STREAM_END) { ended = 1; break; }
         if (ret != Z_OK) { ended = 1; break; }
      }
      got = want - s->strm.avail_out;
      if (!ended && s->strm.avail_out > 0) ended = 1; // truncated archive
   }

   s->produced += got;
   if (ended || s->produced >= s->e.size)
   {
      *fin = 1;
      if (s->produced < s->e.size) *err = 1;
      ZipStreamEnd(s);
   }
   return got;

fail:
   *fin = 1;
   *err = 1;
   ZipStreamEnd(s);
   return 0;
}

static void ZipStreamPublish(ZipStream *s, u32 got, int fin, int err)
{
   if (zipldr.threaded) YabThreadLock(zipldr.mtx);
   s->avail += got;
   if (err) s->error = 1;
   if (fin) s->done = 1;
   if (zipldr.threaded) YabThreadUnLock(zipldr.mtx);
   if (err)
      YuiMsg("[CD] zip: \"%s\" could not be fully decompressed (%u of %u bytes) - the archive may be damaged\n",
             s->e.filename ? s->e.filename : "?", s->avail, s->e.size);
}

static void *ZipWorker(void *arg)
{
   (void)arg;
   for (;;)
   {
      ZipStream *s = NULL;
      int i, fin, err;
      u32 got;

      YabThreadLock(zipldr.mtx);
      if (!zipldr.quit)
      {
         if (zipldr.wanted != NULL && !zipldr.wanted->done)
            s = zipldr.wanted;
         else
            for (i = 0; i < zipldr.count; i++)
               if (!zipldr.stream[i]->done) { s = zipldr.stream[i]; break; }
      }
      YabThreadUnLock(zipldr.mtx);
      if (s == NULL) break; // everything is inflated, or we are told to stop

      got = ZipStreamStep(s, &fin, &err);
      ZipStreamPublish(s, got, fin, err);
   }
   return NULL;
}

// Returns once bytes [0, end) of the entry are in its buffer (or the stream
// is over). Costs one uncontended lock when the data is already there.
static void ZipStreamWait(ZipEntry *entry, u32 end)
{
   ZipStream *s = (ZipStream *)entry;

   if (s == NULL) return;
   if (end > s->e.size) end = s->e.size;

   if (!zipldr.threaded)
   {
      while (!s->done && s->avail < end)
      {
         int fin, err;
         u32 got = ZipStreamStep(s, &fin, &err);
         ZipStreamPublish(s, got, fin, err);
      }
      return;
   }

   for (;;)
   {
      int ready;
      YabThreadLock(zipldr.mtx);
      ready = s->done || s->avail >= end;
      if (!ready) zipldr.wanted = s;
      YabThreadUnLock(zipldr.mtx);
      if (ready) return;
      // Polling on purpose: YabThreadCondWait() takes the mutex itself, so
      // it cannot be used to wait on a predicate without losing wake-ups.
      YabThreadUSleep(1000);
   }
}

static void ZipLoaderStart(FILE *fp)
{
   zipldr.fp = fp;
   zipldr.quit = 0;
   zipldr.wanted = NULL;
   zipldr.threaded = 0;
   if (zipldr.mtx == NULL) zipldr.mtx = YabThreadCreateMutex();
   if (zipldr.mtx != NULL)
   {
      // Set before the thread exists: it reads the flag through
      // ZipStreamPublish().
      zipldr.threaded = 1;
      if (YabThreadStart(CD_ZIP_THREAD_ID, ZipWorker, NULL) != 0)
         zipldr.threaded = 0;
   }
}

// Stops the worker and releases every stream. Must run before the archive
// is closed.
static void ZipLoaderStop(void)
{
   int i;

   if (zipldr.threaded)
   {
      YabThreadLock(zipldr.mtx);
      zipldr.quit = 1;
      YabThreadUnLock(zipldr.mtx);
      YabThreadWait(CD_ZIP_THREAD_ID);
   }
   for (i = 0; i < zipldr.count; i++)
   {
      ZipStream *s = zipldr.stream[i];
      ZipStreamEnd(s);
      free(s->e.zipBuffer);
      free(s->e.filename);
      free(s);
   }
   free(zipldr.stream);
   if (zipldr.mtx != NULL) YabThreadFreeMutex(zipldr.mtx);
   memset(&zipldr, 0, sizeof(zipldr));
}

static int LoadBinCueInZip(const char *filename, FILE *fp)
{
   long size;
   unsigned int track_num = 0;
   unsigned int indexnum, min, sec, frame;
   unsigned int pregap=0;
   track_info_struct trk[100];
   int i;
   int matched = 0;
   char *trackfp = NULL;
   int trackfp_size = 0;
   int fad = 150;
   int file_base_fad = 150; // base FAD du FILE courant (cf. LoadBinCue)
   int pos;
   char *temp_buffer = NULL;

   JZEndRecord endRecord;
   u8* data;
   ZipEntry* tracktr = NULL;
   JZFile *zip;
   ZipDir dir;
   ZipDirEntry *cuefile;
   ZipStream *cue;
   int index = 0;

   memset(&dir, 0, sizeof(dir));
   memset(trk, 0, sizeof(trk));
   ZipLoaderStop(); // nothing left from a previous image
   zipldr.fp = fp;

   // The JZFile wrapper is only needed to read the directory. On failure it
   // is freed WITHOUT closing fp: ISOCDInit() closes the file itself.
   zip = jzfile_from_stdio_file(fp);
   if (zip == NULL) return -1;

   // One pass over the central directory for the whole archive.
   if (jzReadEndRecord(zip, &endRecord) ||
       jzReadCentralDirectory(zip, &endRecord, ZipDirCollect, &dir) ||
       dir.count != endRecord.numEntries)
   {
      YuiMsg("[CD] zip: cannot read the directory of \"%s\"\n", filename);
      goto zipfail;
   }

   // The cue sheet is tiny: inflate it right here.
   cuefile = ZipDirFind(&dir, NULL);
   if (cuefile == NULL || cuefile->size == 0 || (cue = ZipStreamNew(cuefile)) == NULL)
   {
      YabSetError(YAB_ERR_FILEREAD, filename);
      goto zipfail;
   }
   ZipStreamWait(&cue->e, cue->e.size);
   if (cue->error)
   {
      YabSetError(YAB_ERR_FILEREAD, filename);
      goto zipfail;
   }

   disc.session = (session_info_struct*)calloc(1, sizeof(session_info_struct));
   if (disc.session == NULL)
   {
      disc.session_num = 0;
      YabSetError(YAB_ERR_MEMORYALLOC, NULL);
      goto zipfail;
   }
   disc.session_num = 1;

   size = cue->e.size;
   data = cue->e.zipBuffer; // NUL-terminated, see ZipStreamNew()
   // Allocate buffer with enough space for reading cue (+1: a token can be
   // as long as the whole sheet)
   if ((temp_buffer = (char *)calloc(size + 1, 1)) == NULL)
      goto zipfail;
   // Time to generate TOC
   for (;;)
   {
      // Retrieve a line in cue
      if (sscanf((const char*)&data[index], "%s%n", temp_buffer, &pos) == EOF)
         break;
      index+= pos;
      if (strncmp(temp_buffer, "FILE", 4) == 0)
      {
         ZipDirEntry *d;
         ZipStream *f;
         matched = sscanf((const char *)&data[index], " \"%[^\"]\"%n", temp_buffer, &pos);
         if (matched != 1) break; // FILE without a quoted name
         index+= pos;
         // Only the directory is consulted here: the track is inflated by
         // the worker started at the end of this function.
         d = ZipDirFind(&dir, temp_buffer);
         if (d == NULL)
         {
            YuiMsg("[CD] zip: \"%s\" (FILE of the cue sheet) is not in the archive\n", temp_buffer);
            YabSetError(YAB_ERR_FILENOTFOUND, temp_buffer);
            goto zipfail;
         }
         if ((f = ZipStreamNew(d)) == NULL)
         {
            YabSetError(YAB_ERR_MEMORYALLOC, NULL);
            goto zipfail;
         }
         trackfp_size = f->e.size;
         current_file_id++;
         trackfp = strdup(temp_buffer);
         tracktr = &f->e;
         if(track_num > 0) {
           // La piste precedente s'arrete a la fin de SON fichier (inclusive) ;
           // le nouveau FILE commence au FAD suivant.
           trk[track_num-1].fad_end = BinCueTrackFileEnd(&trk[track_num-1]);
           fad = trk[track_num-1].fad_start + BinCueTrackSectors(&trk[track_num-1]);
         }
         file_base_fad = fad; // nouveau FILE -> nouvelle base pour les temps INDEX
         continue;
      }

      // Figure out what it is
      if (strncmp(temp_buffer, "TRACK", 5) == 0)
      {
         // Handle accordingly
         if (sscanf((const char *)&data[index], "%d %[^\r\n]\r\n%n", &track_num, temp_buffer, &pos) == EOF)
            break;
         index+= pos;
         if (track_num < 1 || track_num > 99 || tracktr == NULL)
         {
            // trk[] holds 100 entries; a TRACK before any FILE has no data
            YabSetError(YAB_ERR_FILEREAD, filename);
            goto zipfail;
         }
         trk[track_num-1].isZip = 1;
         trk[track_num-1].filename = trackfp;
         trk[track_num-1].file_size = trackfp_size;
         trk[track_num-1].tr = tracktr;
         trk[track_num-1].file_id = current_file_id;

         if (strncmp(temp_buffer, "MODE1", 5) == 0 ||
            strncmp(temp_buffer, "MODE2", 5) == 0)
         {
            // Figure out the track sector size
            trk[track_num-1].sector_size = atoi(temp_buffer + 6);
            trk[track_num-1].ctl_addr = 0x41;
         }
         else if (strncmp(temp_buffer, "AUDIO", 5) == 0)
         {
            // Update toc entry
            trk[track_num-1].sector_size = 2352;
            trk[track_num-1].ctl_addr = 0x01;
         }
      }
      else if (strncmp(temp_buffer, "INDEX", 5) == 0)
      {
         // Handle accordingly

         if (sscanf((const char *)&data[index], "%d %d:%d:%d\r\n%n", &indexnum, &min, &sec, &frame,&pos) == EOF)
            break;
         index+= pos;

         if (indexnum == 1)
         {
            // Update toc entry
            // FAD = base du FILE courant + temps INDEX + pregap (cf. LoadBinCue).
            fad = file_base_fad + MSF_TO_FAD(min, sec, frame) + pregap;
            trk[track_num-1].fad_start = fad;
            trk[track_num-1].file_offset = MSF_TO_FAD(min, sec, frame) * trk[track_num-1].sector_size;
            CDLOG("Start[%d] %d\n", track_num, trk[track_num-1].fad_start);
            // Meme fichier : la piste precedente s'etend jusqu'a INDEX 01 de
            // celle-ci (son pre-gap INDEX 00 est dans ce meme fichier, a la
            // suite). Fichier different : sa fin a deja ete fixee a la fin de
            // son propre fichier lors de la ligne FILE ; le pre-gap INDEX 00
            // de cette piste est au debut du nouveau fichier et lui appartient.
            if (track_num > 1 && trk[track_num-2].file_id == trk[track_num-1].file_id) {
              trk[track_num-2].fad_end = trk[track_num-1].fad_start-1;
              CDLOG("End[%d] %d\n", track_num-1, trk[track_num-2].fad_end);
            }
            // cf. LoadBinCue : un PREGAP ne s'applique qu'a la piste qui suit, il doit
            // etre remis a 0 une fois consomme (sinon accumulation piste apres piste).
            pregap = 0;
         }
      }
      else if (strncmp(temp_buffer, "PREGAP", 6) == 0)
      {
         if (sscanf((const char *)&data[index], "%d:%d:%d\r\n%n", &min, &sec, &frame, &pos) == EOF)
            break;
         index+= pos;
         pregap += MSF_TO_FAD(min, sec, frame);
      }
      else if (strncmp(temp_buffer, "POSTGAP", 7) == 0)
      {
         if (sscanf((const char *)&data[index], "%d:%d:%d\r\n%n", &min, &sec, &frame, &pos) == EOF)
            break;
         index+= pos;
      }
   }

   trk[track_num].file_offset = 0;
   trk[track_num].fad_start = 0xFFFFFFFF;

   if (track_num == 0) {
     YabSetError(YAB_ERR_FILENOTFOUND, NULL);
     goto zipfail;
   }

   // Derniere piste : fin inclusive ; le lead-out (session.fad_end) est le
   // FAD qui suit le dernier secteur (valeur inchangee pour la TOC).
   trk[track_num-1].fad_end = BinCueTrackFileEnd(&trk[track_num-1]);
   BinCueSetLookupStart(trk, track_num);

   //for (int i =0; i<track_num; i++) printf("Track %d [%d - %d]\n", i+1, trk[i].fad_start, trk[i].fad_end);

   disc.session[0].fad_start = 150;
   disc.session[0].fad_end = trk[track_num-1].fad_start + BinCueTrackSectors(&trk[track_num-1]);
   disc.session[0].track_num = track_num;
   disc.session[0].track = (track_info_struct*)calloc(1, sizeof(track_info_struct) * disc.session[0].track_num);
   if (disc.session[0].track == NULL)
   {
      YabSetError(YAB_ERR_MEMORYALLOC, NULL);
      goto zipfail;
   }

   memcpy(disc.session[0].track, trk, track_num * sizeof(track_info_struct));

   // buffer is no longer needed
   free(temp_buffer);
   ZipDirFree(&dir);

   // From now on the archive belongs to the loader (ISOCDDeInit() closes it
   // through disc.zip once the worker is stopped). Track 1 is inflated
   // first, then the others in cue order.
   disc.zip = zip;
   disc.endRecord = NULL;
   ZipLoaderStart(fp);

   return 0;

zipfail:
   // trk[].filename strings are duplicated per FILE line
   for (i = 0; i < 100; i++)
      if (trk[i].filename != NULL && (i == 0 || trk[i].filename != trk[i-1].filename))
         free(trk[i].filename);
   free(temp_buffer);
   ZipDirFree(&dir);
   ZipLoaderStop();
   free(disc.session);
   disc.session = NULL;
   disc.session_num = 0;
   disc.zip = NULL;
   disc.endRecord = NULL;
   free(zip); // wrapper only, fp stays open for the caller
   return -1;
}

//////////////////////////////////////////////////////////////////////////////

int LoadMDSTracks(const char *mds_filename, FILE *iso_file, mds_session_struct *mds_session, session_info_struct *session)
{
   int i;
   int track_num=0;
   u32 fad_end = 0;

   session->track = (track_info_struct*)calloc(1, sizeof(track_info_struct) * mds_session->last_track);
   if (session->track == NULL)
   {
      YabSetError(YAB_ERR_MEMORYALLOC, NULL);
      return -1;
   }
	memset(session->track, 0, sizeof(track_info_struct) * mds_session->last_track);

   for (i = 0; i < mds_session->total_blocks; i++)
   {
      mds_track_struct track;
      FILE *fp=NULL;
      int file_size = 0;

      fseek(iso_file, mds_session->track_blocks_offset + i * sizeof(mds_track_struct), SEEK_SET);
      if (fread(&track, 1, sizeof(mds_track_struct), iso_file) != sizeof(mds_track_struct))
      {
         YabSetError(YAB_ERR_FILEREAD, mds_filename);
         free(session->track);
         return -1;
      }

      if (track.track_num == 0xA2)
         fad_end = MSF_TO_FAD(track.m, track.s, track.f);
      if (!track.extra_offset)
         continue;

      if (track.footer_offset)
      {
         mds_footer_struct footer;
         int found_dupe=0;
         int j;

         // Make sure we haven't already opened file already
         for (j = 0; j < track_num; j++)
         {
            if (track.footer_offset == session->track[j].file_id)
            {
               found_dupe = 1;
               break;
            }
         }

         if (found_dupe)
         {
            fp = session->track[j].fp;
            file_size = session->track[j].file_size;
         }
         else
         {
            fseek(iso_file, track.footer_offset, SEEK_SET);
            if (fread(&footer, 1, sizeof(mds_footer_struct), iso_file) != sizeof(mds_footer_struct))
            {
               YabSetError(YAB_ERR_FILEREAD, mds_filename);
               free(session->track);
               session->track = NULL;
               return -1;
            }

            fseek(iso_file, footer.filename_offset, SEEK_SET);
            if (footer.is_widechar)
            {
               wchar_t filename[512];
               wchar_t img_filename[512];
               memset(img_filename, 0, 512 * sizeof(wchar_t));

               if (fwscanf(iso_file, L"%512c", img_filename) != 1)
               {
                  YabSetError(YAB_ERR_FILEREAD, mds_filename);
                  free(session->track);
                  session->track = NULL;
                  return -1;
               }

               if (wcsncmp(img_filename, L"*.", 2) == 0)
               {
                  wchar_t *ext;
                  swprintf(filename, sizeof(filename)/sizeof(wchar_t), L"%S", mds_filename);
                  ext = wcsrchr(filename, '.');
                  wcscpy(ext, img_filename+1);
               }
               else
                  wcscpy(filename, img_filename);

               fp = _wfopen(filename, L"rb");
            }
            else
            {
               char filename[512];
               char img_filename[512];
               memset(img_filename, 0, 512);

               if (fscanf(iso_file, "%512c", img_filename) != 1)
               {
                  YabSetError(YAB_ERR_FILEREAD, mds_filename);
                  free(session->track);
                  session->track = NULL;
                  return -1;
               }

               if (strncmp(img_filename, "*.", 2) == 0)
               {
                  char *ext;
                  size_t mds_filename_len = strlen(mds_filename);
                  if (mds_filename_len >= 512)
                  {
                     YabSetError(YAB_ERR_FILEREAD, mds_filename);
                     free(session->track);
                     session->track = NULL;
                     return -1;
                  }
                  strcpy(filename, mds_filename);
                  ext = strrchr(filename, '.');
                  strcpy(ext, img_filename+1);
               }
               else
                  strcpy(filename, img_filename);

               fp = fopen(filename, "rb");
            }

            if (fp == NULL)
            {
               YabSetError(YAB_ERR_FILEREAD, mds_filename);
               free(session->track);
               session->track = NULL;
               return -1;
            }

            fseek(fp, 0, SEEK_END);
            file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
         }
      }

      session->track[track_num].ctl_addr = (((track.addr_ctl << 4) | (track.addr_ctl >> 4)) & 0xFF);
      session->track[track_num].fad_start = track.start_sector+150;
      if (track_num > 0)
         session->track[track_num-1].fad_end = session->track[track_num].fad_start;
      session->track[track_num].file_offset = track.start_offset;
      session->track[track_num].sector_size = track.sector_size;
      session->track[track_num].fp = fp;
      session->track[track_num].file_size = file_size;
      session->track[track_num].file_id = track.footer_offset;
      session->track[track_num].interleaved_sub = track.subchannel_mode != 0 ? 1 : 0;

      track_num++;
   }

   session->track[track_num-1].fad_end = fad_end;
   session->fad_start = session->track[0].fad_start;
   session->fad_end = fad_end;
   session->track_num = track_num;
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static int LoadMDS(const char *mds_filename, FILE *iso_file)
{
   s32 i;
   mds_header_struct header;

   fseek(iso_file, 0, SEEK_SET);

   if (fread((void *)&header, 1, sizeof(mds_header_struct), iso_file) != sizeof(mds_header_struct))
   {
      YabSetError(YAB_ERR_FILEREAD, mds_filename);
      return -1;
   }
   else if (memcmp(&header.signature,  "MEDIA DESCRIPTOR", sizeof(header.signature)))
   {
      YabSetError(YAB_ERR_OTHER, "Bad MDS header");
      return -1;
   }
   else if (header.version[0] > 1)
   {
      YabSetError(YAB_ERR_OTHER, "Unsupported MDS version");
      return -1;
   }

   if (header.medium_type & 0x10)
   {
      // DVD's aren't supported, not will they ever be
      YabSetError(YAB_ERR_OTHER, "DVD's aren't supported");
      return -1;
   }

   disc.session_num = header.session_count;
   disc.session = (session_info_struct*)calloc(1, sizeof(session_info_struct) * disc.session_num);
   if (disc.session == NULL)
   {
      disc.session_num = 0;
      YabSetError(YAB_ERR_MEMORYALLOC, NULL);
      return -1;
   }

   for (i = 0; i < header.session_count; i++)
   {
      mds_session_struct session;

      fseek(iso_file, header.sessions_blocks_offset + i * sizeof(mds_session_struct), SEEK_SET);
      if (fread(&session, 1, sizeof(mds_session_struct), iso_file) != sizeof(mds_session_struct))
      {
         free(disc.session);
         disc.session = NULL;
         disc.session_num = 0;
         YabSetError(YAB_ERR_FILEREAD, mds_filename);
         return -1;
      }

      if (LoadMDSTracks(mds_filename, iso_file, &session, &disc.session[i]) != 0)
         return -1;
   }

   fclose(iso_file);

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static int LoadISO(FILE *iso_file)
{
   track_info_struct *track;

   disc.session = (session_info_struct*)calloc(1, sizeof(session_info_struct));
   if (disc.session == NULL)
   {
      YabSetError(YAB_ERR_MEMORYALLOC, NULL);
      return -1;
   }
   disc.session_num = 1;

   disc.session[0].fad_start = 150;
   disc.session[0].track_num = 1;
   disc.session[0].track = (track_info_struct*)calloc(1, sizeof(track_info_struct) * disc.session[0].track_num);
   if (disc.session[0].track == NULL)
   {
      YabSetError(YAB_ERR_MEMORYALLOC, NULL);
      free(disc.session);
      disc.session_num = 0;
      disc.session = NULL;
      return -1;
   }

	memset(disc.session[0].track, 0, sizeof(track_info_struct) * disc.session[0].track_num);

   track = disc.session[0].track;
   track->ctl_addr = 0x41;
   track->fad_start = 150;
   track->file_offset = 0;
   track->fp = iso_file;
   fseek(iso_file, 0, SEEK_END);
   track->file_size = ftell(iso_file);
   track->file_id = 0;

   if (0 == (track->file_size % 2048))
      track->sector_size = 2048;
   else if (0 == (track->file_size % 2352))
      track->sector_size = 2352;
   else
   {
      YabSetError(YAB_ERR_OTHER, "Unsupported CD image!\n");
      return -1;
   }

   disc.session[0].fad_end = track->fad_end = disc.session[0].fad_start + (track->file_size / track->sector_size);

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

char* StripPreSuffixWhitespace(char* string)
{
	char* p;
	for (;;)
	{
		if (string[0] == 0 || !isspace(string[0]))
			break;
		string++;
	}

	if (strlen(string) == 0)
		return string;

	p = string+strlen(string)-1;
	for (;;)
	{
		if (p <= string || !isspace(p[0]))
		{
			p[1] = '\0';
			break;
		}
		p--;
	}

	return string;
}

//////////////////////////////////////////////////////////////////////////////

int LoadParseCCD(FILE *ccd_fp, ccd_struct *ccd)
{
	char text[60], section[CCD_MAX_SECTION], old_name[CCD_MAX_NAME] = "";
	char * start, *end, *name, *value;
	int lineno = 0, error = 0, max_size = 100;

	ccd->dict = (ccd_dict_struct *)calloc(1, sizeof(ccd_dict_struct)*max_size);
	if (ccd->dict == NULL)
		return -1;

	ccd->num_dict = 0;

	// Read CCD file
	while (fgets(text, sizeof(text), ccd_fp) != NULL)
	{
		lineno++;

		start = StripPreSuffixWhitespace(text);

		if (start[0] == '[')
		{
			// Section
			end = strchr(start+1, ']');
			if (end == NULL)
			{
				// ] missing from section
				error = lineno;
			}
			else
			{
				end[0] = '\0';
				memset(section, 0, sizeof(section));
				strncpy(section, start + 1, sizeof(section));
				old_name[0] = '\0';
			}
		}
		else if (start[0])
		{
			// Name/Value pair
			end = strchr(start, '=');
			if (end)
			{
				end[0] = '\0';
				name = StripPreSuffixWhitespace(start);
				value = StripPreSuffixWhitespace(end + 1);

				memset(old_name, 0, sizeof(old_name));
				strncpy(old_name, name, sizeof(old_name));
				if (ccd->num_dict+1 > max_size)
				{
					max_size *= 2;
					ccd->dict = (ccd_dict_struct*)realloc(ccd->dict, sizeof(ccd_dict_struct)*max_size);
					if (ccd->dict == NULL)
					{
						free(ccd->dict);
            ccd->dict = NULL;
						return -2;
					}
				}
				strcpy(ccd->dict[ccd->num_dict].section, section);
				strcpy(ccd->dict[ccd->num_dict].name, name);
				strcpy(ccd->dict[ccd->num_dict].value, value);
				ccd->num_dict++;
			}
			else
				error = lineno;
		}

		if (error)
			break;
	}

	if (error)
	{
		free(ccd->dict);
    ccd->dict = NULL;
		ccd->num_dict = 0;
	}

	return error;
}

//////////////////////////////////////////////////////////////////////////////

static int GetIntCCD(ccd_struct *ccd, char *section, char *name)
{
	int i;
	for (i = 0; i < ccd->num_dict; i++)
	{
#if (defined(IOS) || defined(ANDROID))
        if (strcasecmp(ccd->dict[i].section, section) == 0 &&
            strcasecmp(ccd->dict[i].name, name) == 0)
#else
		if (stricmp(ccd->dict[i].section, section) == 0 &&
			 stricmp(ccd->dict[i].name, name) == 0)
#endif
			return strtol(ccd->dict[i].value, NULL, 0);

	}

	return -1;
}

//////////////////////////////////////////////////////////////////////////////

static int LoadCCD(const char *ccd_filename, FILE *iso_file)
{
	int i;
	ccd_struct ccd;
	int num_toc;
	char img_filename[512];
	char *ext;
	FILE *fp;
   size_t ccd_filename_len = strlen(ccd_filename);

   if (ccd_filename_len >= 512)
   {
      YabSetError(YAB_ERR_FILEREAD, ccd_filename);
      return -1;
   }

	strcpy(img_filename, ccd_filename);
	ext = strrchr(img_filename, '.');
	strcpy(ext, ".img");
	fp = fopen(img_filename, "rb");

	if (fp == NULL)
	{
		ext = strrchr(img_filename, '.');
		strcpy(ext, ".iso");
		fp = fopen(img_filename, "rb");
		if (fp == NULL){
			YabSetError(YAB_ERR_FILEREAD, img_filename);
			return -1;
		}
	}

	fseek(iso_file, 0, SEEK_SET);

	// Load CCD file as dictionary
	if (LoadParseCCD(iso_file, &ccd))
	{
		fclose(fp);
		YabSetError(YAB_ERR_FILEREAD, ccd_filename);
		return -1;
	}

	num_toc = GetIntCCD(&ccd, "DISC", "TocEntries");
	disc.session_num = GetIntCCD(&ccd, "DISC", "Sessions");
	if (disc.session_num != 1)
	{
		fclose(fp);
		YabSetError(YAB_ERR_OTHER, "Sessions more than 1 are unsupported");
		return -1;
	}

	disc.session = (session_info_struct*)calloc(1, sizeof(session_info_struct));
	if (disc.session == NULL)
	{
		fclose(fp);
		free(ccd.dict);
    ccd.dict = NULL;
		YabSetError(YAB_ERR_MEMORYALLOC, NULL);
		return -1;
	}
    disc.session_num = 1;

	if (GetIntCCD(&ccd, "DISC", "DataTracksScrambled"))
	{
		fclose(fp);
		free(ccd.dict);
		free(disc.session);
    ccd.dict = NULL;
    disc.session = NULL;
    disc.session_num = 0;
		YabSetError(YAB_ERR_OTHER, "CCD Scrambled Tracks not supported");
		return -1;
	}

	// Find track number and allocate
	for (i = 0; i < num_toc; i++)
	{
		char sect_name[64];
		int point;

		sprintf(sect_name, "Entry %d", i);
		point = GetIntCCD(&ccd, sect_name, "Point");

		if (point == 0xA1)
		{
			int ses = GetIntCCD(&ccd, sect_name, "Session");

			disc.session[ses-1].fad_start = 150;
			disc.session[ses-1].track_num=GetIntCCD(&ccd, sect_name, "PMin");;
			disc.session[ses-1].track = (track_info_struct *)calloc(1, disc.session[ses-1].track_num * sizeof(track_info_struct));
			if (disc.session[ses-1].track == NULL)
			{
				fclose(fp);
				free(ccd.dict);
				free(disc.session);
        ccd.dict = NULL;
        disc.session = NULL;
        disc.session_num = 0;
				YabSetError(YAB_ERR_MEMORYALLOC, NULL);
				return -1;
			}
			memset(disc.session[ses-1].track, 0, disc.session[ses-1].track_num * sizeof(track_info_struct));
		}
	}

	// Load TOC
	for (i = 0; i < num_toc; i++)
	{
		char sect_name[64];
		int ses, point, adr, control, trackno, amin, asec, aframe;
		int alba, zero, pmin, psec, pframe, plba;

		sprintf(sect_name, "Entry %d", i);

		ses = GetIntCCD(&ccd, sect_name, "Session");
		point = GetIntCCD(&ccd, sect_name, "Point");
		adr = GetIntCCD(&ccd, sect_name, "ADR");
		control = GetIntCCD(&ccd, sect_name, "Control");
		trackno = GetIntCCD(&ccd, sect_name, "TrackNo");
		amin = GetIntCCD(&ccd, sect_name, "AMin");
		asec = GetIntCCD(&ccd, sect_name, "ASec");
		aframe = GetIntCCD(&ccd, sect_name, "AFrame");
		alba = GetIntCCD(&ccd, sect_name, "ALBA");
		zero = GetIntCCD(&ccd, sect_name, "Zero");
		pmin = GetIntCCD(&ccd, sect_name, "PMin");
		psec = GetIntCCD(&ccd, sect_name, "PSec");
		pframe = GetIntCCD(&ccd, sect_name, "PFrame");
		plba = GetIntCCD(&ccd, sect_name, "PLBA");

		if(point >= 1 && point <= 99)
		{
			track_info_struct *track=&disc.session[ses-1].track[point-1];
			track->ctl_addr = (control << 4) | adr;
			track->fad_start = MSF_TO_FAD(pmin, psec, pframe);
			if (point >= 2) {
			   disc.session[ses-1].track[point-2].fad_end = track->fad_start-1;
         disc.session[ses-1].track[point-2].file_size = (disc.session[ses-1].track[point-2].fad_end+1-disc.session[ses-1].track[point-2].fad_start)*2352;
      }
			track->file_offset = plba*2352;
			track->sector_size = 2352;
			track->fp = fp;
			track->file_id = 0;
			track->interleaved_sub = 0;
		}
		else if (point == 0xA2)
		{
			disc.session[ses-1].fad_end = MSF_TO_FAD(pmin, psec, pframe);
			disc.session[ses-1].track[disc.session[ses-1].track_num-1].fad_end = disc.session[ses-1].fad_end;
		}
	}

	fclose(iso_file);

	return 0;
}

//////////////////////////////////////////////////////////////////////////////

void BuildTOC()
{
   int i;
   session_info_struct *session=&disc.session[0];

   for (i = 0; i < session->track_num; i++)
   {
      track_info_struct *track=&disc.session[0].track[i];
      isoTOC[i] = (track->ctl_addr << 24) | track->fad_start;
   }

   isoTOC[99] = (isoTOC[0] & 0xFF000000) | 0x010000;
   isoTOC[100] = (isoTOC[session->track_num - 1] & 0xFF000000) | (session->track_num << 16);
   isoTOC[101] = (isoTOC[session->track_num - 1] & 0xFF000000) | session->fad_end;
}

#if (defined(IOS) || defined(ANDROID))
#define stricmp strcasecmp
#endif
//////////////////////////////////////////////////////////////////////////////

static int ISOCDInit(const char * iso) {
   char header[6];
   char *ext;
   int ret = -1;
   FILE *iso_file;
   size_t num_read = 0;
   memset(isoTOC, 0xFF, 0xCC * 2);
   memset(&disc, 0, sizeof(disc));
   iso_cd_status = 0;

   if (!iso)
      return -1;

   if (!(iso_file = fopen(iso, "rb")))
   {
      YabSetError(YAB_ERR_FILENOTFOUND, (char *)iso);
      return -1;
   }

   ext = strrchr((char *)iso, '.');

   // Figure out what kind of image format we're dealing with
   if (stricmp(ext, ".CUE") == 0)
   {
      // It's a BIN/CUE
      imgtype = IMG_BINCUE;
      ret = LoadBinCue(iso, iso_file);
   }
   else if (stricmp(ext, ".ZIP") == 0)
   {
      // It's a BIN/CUE
      imgtype = IMG_BINCUE;
      ret = LoadBinCueInZip(iso, iso_file);
   }
   else if (stricmp(ext, ".MDS") == 0) {
     num_read = fread((void *)header, 1, 6, iso_file);
     if (strncmp(header, "MEDIA ", sizeof(header)) == 0)
     {
        // It's a MDS
        imgtype = IMG_MDS;
        ret = LoadMDS(iso, iso_file);
     }
   }
   else if (stricmp(ext, ".CCD") == 0)
   {
  	// It's a CCD
  	imgtype = IMG_CCD;
  	ret = LoadCCD(iso, iso_file);
   }
  else if (stricmp(ext, ".CHD") == 0)
  {
    // It's a CCD
    imgtype = IMG_CHD;
    ret = LoadCHD(iso, iso_file);
  }
   else
   {
      // Assume it's an ISO file
      imgtype = IMG_ISO;
      ret = LoadISO(iso_file);
   }

   if (ret != 0)
   {
      imgtype = IMG_NONE;

      if (iso_file)
         fclose(iso_file);
      iso_file = NULL;
      return -1;
   }

   BuildTOC();
   ISOCDInitStatus = 1;
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void ISOCDDeInit(void) {
   int i, j, k;
   if( ISOCDInitStatus == 0) return;
   // Stop the zip worker before anything it uses goes away; this also
   // releases every decompressed track (track->tr).
   ZipLoaderStop();
   if (disc.session != NULL)
   {
      for (i = 0; i < disc.session_num; i++)
      {
         if (disc.session[i].track != NULL)
         {
            for (j = 0; j < disc.session[i].track_num; j++)
            {
               if (disc.session[i].track[j].isZip == 1)
               {
                  // Buffers already released by ZipLoaderStop()
                  disc.session[i].track[j].tr = NULL;
                  // The name is duplicated once per FILE line and shared by
                  // the tracks of that file (it was never freed).
                  if (disc.session[i].track[j].filename != NULL &&
                      (j == 0 || disc.session[i].track[j].filename != disc.session[i].track[j-1].filename))
                     free(disc.session[i].track[j].filename);
               }

               if (disc.session[i].track[j].fp)
               {
                  fclose(disc.session[i].track[j].fp);

                  // Make sure we don't close the same file twice
                  for (k = j+1; k < disc.session[i].track_num; k++)
                  {
                     if (disc.session[i].track[j].file_id == disc.session[i].track[k].file_id)
                        disc.session[i].track[k].fp = NULL;
                  }
               }
            }
            free(disc.session[i].track);
            disc.session[i].track = NULL;
         }
      }
      free(disc.session);
      disc.session = NULL;
      disc.session_num = 0;
      if (disc.zip != NULL)
        disc.zip->close(disc.zip);
      disc.zip = NULL;
      disc.endRecord = NULL; // no longer allocated (see LoadBinCueInZip)
   }
   ISOCDInitStatus = 0;
}

//////////////////////////////////////////////////////////////////////////////

static int ISOCDGetStatus(void) {
	if (iso_cd_status == 0){
		return disc.session_num > 0 ? 0 : 2;
	}

	return iso_cd_status;
}

//#define CDCORE_NORMAL 0
//#define CDCORE_NODISC 2
//#define CDCORE_OPEN   3

static void ISOCDSetStatus(int status){
	iso_cd_status = status;
	return;
}

//////////////////////////////////////////////////////////////////////////////

static s32 ISOCDReadTOC(u32 * TOC) {
   memcpy(TOC, isoTOC, 0xCC * 2);

   return (0xCC * 2);
}

//////////////////////////////////////////////////////////////////////////////

track_info_struct *currentTrack = NULL;

/* ECMA-130 11.3: before being written to disc, everything in a sector past
   the 12 sync bytes is scrambled with a 15-bit LFSR (x^15 + x + 1, preset to
   1), and the drive normally unscrambles it again on read. Some dumps -- a
   drive read in fully raw mode, or a dumper that skips the step -- keep the
   scrambled form: the sync pattern is intact (it is never scrambled), but the
   header, subheader and user data are noise. Readers that trust the sync then
   hand the CD block garbage, and MPEG/XA sectors are silently discarded for
   having no valid subheader. Detect that per track and unscramble on read. */

static const u8 *CDScramblerTable(void)
{
   static u8 tbl[2340];
   static int built = 0;
   int i, bit;
   u16 reg = 1;

   if (!built)
   {
      for (i = 0; i < 2340; i++)
      {
         u8 v = 0;
         for (bit = 0; bit < 8; bit++)
         {
            v |= (u8)((reg & 1) << bit);
            reg = (u16)((reg >> 1) | (((reg ^ (reg >> 1)) & 1) << 14));
         }
         tbl[i] = v;
      }
      built = 1;
   }
   return tbl;
}

static void CDUnscramble(u8 *sector)
{
   const u8 *tbl = CDScramblerTable();
   int i;
   for (i = 12; i < 2352; i++)
      sector[i] ^= tbl[i - 12];
}

/* A plausible CD-ROM sector head: mode 1 or 2, and for mode 2 the 4-byte
   subheader must appear twice, as the format requires. */
static int CDSectorHeadLooksValid(const u8 *head)
{
   if (head[15] == 0x01)
      return 1;
   if (head[15] == 0x02)
      return memcmp(head + 16, head + 20, 4) == 0;
   return 0;
}

/* Lecture de 'len' octets a la position 'pos' d'une piste, qu'elle soit un
   fichier ouvert (fp) ou un fichier decompresse depuis un .zip (tr). Le
   pointeur de fichier n'est pas restaure : l'appelant (ISOCDReadSectorFAD)
   repositionne toujours avant de lire le secteur. */
static long BinTrackReadAt(track_info_struct *track, long pos, u8 *buf, long len)
{
   if (pos < 0 || len <= 0) return 0;
   if (track->isZip == 1)
   {
      ZipEntry *z = track->tr;
      if (z == NULL || z->zipBuffer == NULL || pos >= (long)z->size) return 0;
      if (pos + len > (long)z->size) len = (long)z->size - pos;
      ZipStreamWait(z, (u32)(pos + len)); // inflated in the background
      memcpy(buf, z->zipBuffer + pos, (size_t)len);
      return len;
   }
   if (track->fp == NULL) return 0;
   if (fseek(track->fp, pos, SEEK_SET) != 0) return 0;
   return (long)fread(buf, 1, (size_t)len, track->fp);
}

static int ISOCDReadSectorFAD(u32 FAD, void *buffer) {
   int i,j;
   size_t num_read = 0;
   ZipEntry *tr = NULL;
   u8* zipBuffer;
   int found = 0;
   int offset = 0;

   assert(disc.session);

   if (IMG_CHD == imgtype) {
     return ISOCDReadSectorFADFromCHD(FAD,buffer);
   }

   memset(buffer, 0, 2448);

   for (i = 0; i < disc.session_num && !found; i++)
   {
      for (j = 0; j < disc.session[i].track_num; j++)
      {
         track_info_struct *track = &disc.session[i].track[j];
         // fad_lookup_start couvre le pre-gap INDEX 00 stocke dans le fichier
         // de la piste (bin/cue multi-fichiers) ; 0 = pas de pre-gap lisible.
         u32 lo = (track->fad_lookup_start != 0 && track->fad_lookup_start < track->fad_start) ?
                  track->fad_lookup_start : track->fad_start;
         if (FAD >= lo && FAD <= track->fad_end)
         {
            if (currentTrack != track) {
              currentTrack = track;
              if (currentTrack->isZip == 1) {
                if (currentTrack->tr == NULL) {
                  //This should never happen, otherwise we might suffer some delay to deflation during game
                  printf("%s was not defalted!!!\n", currentTrack->filename);
                  return 0;
                }
              }
            }
            tr = currentTrack->tr;
            found = 1;
            break;
         }
      }
   }

   if (!found)
   {
      // Secteur absent de tous les fichiers (PREGAP non stocke, au-dela du
      // dernier secteur...) : secteur vide, sans relire la piste precedente
      // avec un offset faux. Comportement identique a l'ancien cas "offset
      // borne a la taille du fichier" qui renvoyait deja un secteur nul.
      CDLOG("Warning: FAD %u not found in track list\n", FAD);
      return (currentTrack != NULL) ? 1 : 0;
   }
   if ((currentTrack->isZip == 1) && (tr == NULL)) {
     CDLOG("Warning Zip file: Sector not found in track list\n");
     return 0;
   }
   // FAD < fad_start possible (pre-gap INDEX 00 dans le fichier) : calcul signe.
   offset = (int)currentTrack->file_offset + ((int)FAD - (int)currentTrack->fad_start) * (int)currentTrack->sector_size;
   if (offset < 0) offset = 0;

   /* A raw data track (MODE1/MODE2 with 2352- or 2448-byte sectors) must
      begin with the 12-byte sync pattern 00 FF*10 00 at the offset computed
      above. When it does not, the track's start offset is wrong -- typically
      because the .bin does not actually contain the pregap that the cue's
      INDEX 00/INDEX 01 pair implies, so every sector is read from the wrong
      place. The caller then sees no sync header, takes the sector for CD-DA
      and throws it away, silently.

      Rather than fail that way, locate the real sector boundary once per
      track and correct file_offset by the difference. The search window is
      bounded (a few sectors either side, which is all a pregap
      mismatch can ever amount to) so a genuinely corrupt image is still
      reported instead of being papered over. */
   if (!currentTrack->sync_checked &&
       ((currentTrack->isZip != 1 && currentTrack->fp != NULL) ||
        (currentTrack->isZip == 1 && tr != NULL && tr->zipBuffer != NULL)) &&
       (currentTrack->ctl_addr & 0x40) &&
       (currentTrack->sector_size == 2352 || currentTrack->sector_size == 2448))
   {
      /* Meme verification pour une piste lue dans un .zip : elle etait
         reservee aux fichiers ouverts par fopen() (condition isZip != 1).
         Un .bin zippe dont l'offset de depart est faux ou dont les secteurs
         sont stockes brouilles etait donc lu de travers, alors que le meme
         .bin decompresse etait corrige. La lecture passe maintenant par
         BinTrackReadAt(), qui sert le fichier ou le tampon decompresse. */
      static const u8 sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
      const int window = 8 * 2352;            /* +/- 8 sectors */
      long base = (long)currentTrack->file_offset;
      long from = base - window; long to = base + window;
      long trackSize = (currentTrack->isZip == 1) ? (long)tr->size : (long)currentTrack->file_size;
      u8 *buf; long span; long syncpos = -1; long i;

      currentTrack->sync_checked = 1;

      if (from < 0) from = 0;
      if (to > trackSize - 12) to = trackSize - 12;
      span = to - from + 12;

      if (span > 0 && (buf = (u8 *)malloc((size_t)span)) != NULL)
      {
         span = BinTrackReadAt(currentTrack, from, buf, span);

         /* Prefer the candidate closest to the computed offset. A candidate
            only counts when the NEXT sector starts with the sync pattern
            too (when it is inside the window): the 12-byte pattern can also
            occur inside user data, and taking such a spot for a sector
            boundary would shift every sector of the track. */
         for (i = 0; i + 12 <= span; i++)
            if (memcmp(buf + i, sync, 12) == 0)
            {
               long next = i + (long)currentTrack->sector_size;
               if (next + 12 <= span && memcmp(buf + next, sync, 12) != 0)
                  continue;
               if (syncpos < 0 || labs((from + i) - base) < labs(syncpos - base))
                  syncpos = from + i;
            }
         free(buf);
      }

      if (syncpos >= 0 && syncpos != base)
      {
         YuiMsg("[CD] track FAD %d..%d: start offset %d is NOT a sector boundary; "
                "real one found at %d (%+d bytes, %+d sectors) - corrected\n",
                currentTrack->fad_start, currentTrack->fad_end, (int)base, (int)syncpos,
                (int)(syncpos - base), (int)((syncpos - base) / (long)currentTrack->sector_size));
         currentTrack->file_offset = (u32)syncpos;
         offset = (int)currentTrack->file_offset + ((int)FAD - (int)currentTrack->fad_start) * (int)currentTrack->sector_size;
         if (offset < 0) offset = 0;
      }

      /* Scrambled or not? Test the sector head as stored, then unscrambled. */
      if (syncpos >= 0)
      {
         u8 head[24];

         if (BinTrackReadAt(currentTrack, syncpos, head, 24) == 24 && !CDSectorHeadLooksValid(head))
         {
            const u8 *tbl = CDScramblerTable();
            u8 test[24];
            int i;
            memcpy(test, head, 24);
            for (i = 12; i < 24; i++)
               test[i] ^= tbl[i - 12];

            if (CDSectorHeadLooksValid(test))
            {
               currentTrack->scrambled = 1;
               YuiMsg("[CD] track FAD %d..%d: sectors are stored SCRAMBLED (ECMA-130) - "
                      "unscrambling on read\n", currentTrack->fad_start, currentTrack->fad_end);
            }
            else
            {
               YuiMsg("[CD] track FAD %d..%d: sector head is not valid either way "
                      "(mode byte %02X) - the image may be damaged\n",
                      currentTrack->fad_start, currentTrack->fad_end, head[15]);
            }
         }
      }
      else
      {
         YuiMsg("[CD] track FAD %d..%d: no sync pattern within +/-8 sectors of offset %d - "
                "the image may not be a raw %d-byte-per-sector dump\n",
                currentTrack->fad_start, currentTrack->fad_end, (int)base,
                currentTrack->sector_size);
      }
   }

   if (currentTrack->isZip != 1) {
	 if (offset > currentTrack->file_size) offset = currentTrack->file_size;
     fseek(currentTrack->fp, offset, SEEK_SET);
   } else {
	 if (offset > tr->size) offset = tr->size;
     // The track is inflated in the background: wait until this sector
     // (2352 + 3 * 2448 covers the widest read, interleaved subcode) is
     // there. Immediate once the worker has gone past it.
     ZipStreamWait(tr, (u32)offset + 2352 + 3 * 2448);
     zipBuffer = &tr->zipBuffer[offset];
   }

   if (currentTrack->sector_size == 2448)
   {
      if (!currentTrack->interleaved_sub)
      {
         if (currentTrack->isZip != 1) {
           num_read = fread(buffer, 2448, 1, currentTrack->fp);
         } else {
           int delta = ((tr->size - offset) < 2448)?(tr->size - offset):2448;
           memcpy(buffer, zipBuffer, delta);
           zipBuffer+=delta;
         }
      }
      else
      {
         const u16 deint_offsets[] = {
            0, 66, 125, 191, 100, 50, 150, 175, 8, 33, 58, 83,
            108, 133, 158, 183, 16, 41, 25, 91, 116, 141, 166, 75,
            24, 90, 149, 215, 124, 74, 174, 199, 32, 57, 82, 107,
            132, 157, 182, 207, 40, 65, 49, 115, 140, 165, 190, 99,
            48, 114, 173, 239, 148, 98, 198, 223, 56, 81, 106, 131,
            156, 181, 206, 231, 64, 89, 73, 139, 164, 189, 214, 123,
            72, 138, 197, 263, 172, 122, 222, 247, 80, 105, 130, 155,
            180, 205, 230, 255, 88, 113, 97, 163, 188, 213, 238, 147
         };
         u8 subcode_buffer[96 * 3];
         if (currentTrack->isZip != 1) {
           num_read = fread(buffer, 2352, 1, currentTrack->fp);

           num_read = fread(subcode_buffer, 96, 1, currentTrack->fp);
           fseek(currentTrack->fp, 2352, SEEK_CUR);
           num_read = fread(subcode_buffer + 96, 96, 1, currentTrack->fp);
           fseek(currentTrack->fp, 2352, SEEK_CUR);
           num_read = fread(subcode_buffer + 192, 96, 1, currentTrack->fp);
         } else {
           int delta = ((tr->size - offset) < 2352)?(tr->size - offset):2352;
           //if ((tr->size - offset) < 2352) return 0;
           int k;
           memcpy(buffer, zipBuffer, delta);
           // Same layout as the fread() path above: the 96 subcode bytes of
           // this sector and of the next two (2448 bytes apart). This path
           // used to copy the same 96 bytes three times.
           memset(subcode_buffer, 0, sizeof(subcode_buffer));
           for (k = 0; k < 3; k++) {
             u32 subpos = (u32)offset + 2352 + (u32)k * 2448;
             if (subpos + 96 <= tr->size)
               memcpy(&subcode_buffer[96 * k], &tr->zipBuffer[subpos], 96);
           }
         }
           for (i = 0; i < 96; i++)
              ((u8 *)buffer)[2352+i] = subcode_buffer[deint_offsets[i]];
      }
   }
   else if (currentTrack->sector_size == 2352)
   {
      if (currentTrack->isZip != 1) {
        // Generate subcodes here
        num_read = fread(buffer, 2352, 1, currentTrack->fp);
      } else {
        int delta = ((tr->size - offset) < 2352)?(tr->size - offset):2352;
        memcpy(buffer, zipBuffer, delta);
        zipBuffer+=delta;
      }
   }
   else if (currentTrack->sector_size == 2048)
   {
      memcpy(buffer, syncHdr, 12);
      if (currentTrack->isZip != 1) {
        num_read = fread((char *)buffer + 0x10, 2048, 1, currentTrack->fp);
      } else {
        int delta = ((tr->size - offset) < 2048)?(tr->size - offset):2048;
        memcpy((char *)buffer + 0x10, zipBuffer, delta);
        zipBuffer+=delta;
      }
   }

   /* Raw sectors only: the 2048 branch above builds its own sync header and
      its payload was never scrambled to begin with. */
   if (currentTrack->scrambled &&
       (currentTrack->sector_size == 2352 || currentTrack->sector_size == 2448))
      CDUnscramble((u8 *)buffer);

	return 1;
}

//////////////////////////////////////////////////////////////////////////////

static void ISOCDReadAheadFAD(UNUSED u32 FAD)
{
	// No-op
}

//////////////////////////////////////////////////////////////////////////////

#define CD_MAX_SECTOR_DATA      (2352)
#define CD_MAX_SUBCODE_DATA     (96)
#define CD_FRAME_SIZE           (CD_MAX_SECTOR_DATA + CD_MAX_SUBCODE_DATA)
#define CD_MAX_TRACKS           (99)    /* AFAIK the theoretical limit */
#define CD_TRACK_PADDING 4

typedef struct ChdInfo_ {
  chd_file *chd;
  core_file * image_file;
  const chd_header * header;
  char * hunk_buffer;
  int current_hunk_id;
} ChdInfo;

ChdInfo * pChdInfo = NULL;

static int LoadCHD(const char *chd_filename, FILE *iso_file)
{
  int trak_number;
  char track_type[64];
  char track_subtype[64];
  int frame = 0;
  int pregap = 0;
  char pg_type[64];
  char pg_sub_type[64];
  int postgap = 0;

  int meta_outlen = 512 * 1024;
  u8 * buf = (u8*)calloc(1, meta_outlen);
  u32 resultlen;
  u32 resulttag;
  u8 resultflags;

  if (pChdInfo != NULL) {
    free(pChdInfo);
    pChdInfo = NULL;
  }

  pChdInfo = (ChdInfo*)calloc(1, sizeof(ChdInfo));
  memset(pChdInfo, 0, sizeof(ChdInfo));

  track_info_struct trk[100];
  memset(trk, 0, sizeof(trk));

  int num_tracks = 0;

  chd_error error = chd_open(chd_filename, CHD_OPEN_READ, NULL, &pChdInfo->chd);
  if (error != CHDERR_NONE) {
    free(buf);
    return -1;
  }

  pChdInfo->header = chd_get_header(pChdInfo->chd);

  trk[num_tracks].fad_start = frame + pregap + 150;

  while ( chd_get_metadata(pChdInfo->chd, 0, num_tracks, buf, meta_outlen, &resultlen, &resulttag, &resultflags) == CHDERR_NONE )  {

    LOG("track info %s", buf);
    switch (resulttag) {
    case CDROM_TRACK_METADATA_TAG:
      sscanf((const char*)buf, CDROM_TRACK_METADATA_FORMAT, &trak_number, track_type, track_subtype, &frame);
      pregap = 0;
      postgap = 0;
      sprintf(pg_type, "NONE");
      break;
    case CDROM_TRACK_METADATA2_TAG:
      sscanf((const char*)buf, CDROM_TRACK_METADATA2_FORMAT, &trak_number, track_type, track_subtype, &frame, &pregap, pg_type, pg_sub_type, &postgap);
      break;
    default:
      return -1;
    }

    trk[num_tracks].pregap = pregap;
    trk[num_tracks].postgap = postgap;

    trk[num_tracks].frames = frame;
    int padded = (frame + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING;
    trk[num_tracks].extraframes = padded * CD_TRACK_PADDING - frame;


    if (!strcmp(track_type, "MODE1"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2048;
    }
    else if (!strcmp(track_type, "MODE1/2048"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2048;
    }
    else if (!strcmp(track_type, "MODE1_RAW"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2352;
    }
    else if (!strcmp(track_type, "MODE1/2352"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2352;
    }
    else if (!strcmp(track_type, "MODE2"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2336;
    }
    else if (!strcmp(track_type, "MODE2/2336"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2336;
    }
    else if (!strcmp(track_type, "MODE2_FORM1"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2048;
    }
    else if (!strcmp(track_type, "MODE2/2048"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2048;
    }
    else if (!strcmp(track_type, "MODE2_FORM2"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2324;
    }
    else if (!strcmp(track_type, "MODE2/2324"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2324;
    }
    else if (!strcmp(track_type, "MODE2_FORM_MIX"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2336;
    }
    else if (!strcmp(track_type, "MODE2/2336"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2336;
    }
    else if (!strcmp(track_type, "MODE2_RAW"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2352;
    }
    else if (!strcmp(track_type, "MODE2/2352"))
    {
      trk[num_tracks].ctl_addr = 0x41;
      trk[num_tracks].sector_size = 2352;
    }
    else if (!strcmp(track_type, "AUDIO"))
    {
      trk[num_tracks].ctl_addr = 0x01;
      trk[num_tracks].sector_size = 2352;
      //trk[num_tracks].pregap = 0;
    }

    //trk[num_tracks].fad_start = trk[num_tracks].fad_start + pregap;
    //trk[num_tracks].fad_end = trk[num_tracks].fad_start + (frame - 1) + postgap;
    //frame = trk[num_tracks].fad_end+1;
    num_tracks++;
    //trk[num_tracks].fad_start = frame;
  }
  free(buf);

  trk[num_tracks].file_offset = 0;
  trk[num_tracks].fad_start = 0xFFFFFFFF;

  u32 chdofs = 0;
  u32 physofs = 0;
  u32 logofs = 150;
  int i;
  for (i = 0; i < num_tracks; i++)
  {
    trk[i].fad_start = logofs + trk[i].pregap;

    trk[i].physframeofs = physofs;
    trk[i].chdframeofs = chdofs;
    trk[i].logframeofs = logofs;

    //logofs += trk[i].pregap;
    //logofs += trk[i].postgap;
    logofs += trk[i].frames;
    trk[i].fad_end = logofs;

    physofs += trk[i].frames;

    chdofs += trk[i].frames;
    chdofs += trk[i].extraframes;
  }
  trk[i].logframeofs = logofs;
  trk[i].physframeofs = physofs;
  trk[i].chdframeofs = chdofs;

  //trk[num_tracks - 1].fad_end = (pChdInfo->header->logicalbytes - trk[num_tracks - 1].file_offset) / trk[num_tracks - 1].sector_size;

  disc.session_num = 1;
  disc.session = (session_info_struct*)calloc(1, sizeof(session_info_struct) * disc.session_num);
  if (disc.session == NULL)
  {
    YabSetError(YAB_ERR_MEMORYALLOC, NULL);
    return -1;
  }
  disc.session[0].fad_start = 150;
  disc.session[0].fad_end = trk[num_tracks - 1].fad_end;
  disc.session[0].track_num = num_tracks;
  disc.session[0].track = (track_info_struct*)calloc(1, sizeof(track_info_struct) * disc.session[0].track_num);
  if (disc.session[0].track == NULL)
  {
    YabSetError(YAB_ERR_MEMORYALLOC, NULL);
    free(disc.session);
    disc.session = NULL;
    disc.session_num = 0;
    return -1;
  }

  memcpy(disc.session[0].track, trk, num_tracks * sizeof(track_info_struct));

  pChdInfo->hunk_buffer = (char*)calloc(1, pChdInfo->header->hunkbytes);
  chd_read(pChdInfo->chd, 0, pChdInfo->hunk_buffer);
  pChdInfo->current_hunk_id = 0;

  return 0;
}


static int ISOCDReadSectorFADFromCHD(u32 FAD, void *buffer) {
  int i, j;
  size_t num_read = 0;
  track_info_struct *track = NULL;
  u32 chdlba;
  u32 physlba;
  u32 loglba = FAD;

  chdlba = loglba;
  for (i = 0; i < disc.session_num; i++)
  {
    for (j = 0; j < disc.session[i].track_num-1 ; j++)
    {
      //if (j == 1) {
      //  int a = 0;
      //}
      if (loglba < disc.session[i].track[j+1].logframeofs) {
        //if ((loglba > disc.session[i].track[j].pregap)) {
       //   loglba -= disc.session[i].track[j].pregap;
       // }
        physlba = disc.session[i].track[j].physframeofs + (loglba - disc.session[i].track[j].logframeofs);
        //if (disc.session[i].track[j].ctl_addr == 0x01) {
        //  physlba += disc.session[i].track[j].pregap;
        //}
        chdlba = physlba - disc.session[i].track[j].physframeofs + disc.session[i].track[j].chdframeofs;
        track = &disc.session[i].track[j];
        break;
      }
    }
  }

  if (track == NULL)
  {
    CDLOG("Warning: Sector not found in track list");
    return 0;
  }

  int hunkid = (chdlba*CD_FRAME_SIZE) / pChdInfo->header->hunkbytes ;
  int hunk_offset =  (chdlba*CD_FRAME_SIZE) % pChdInfo->header->hunkbytes;

  if (pChdInfo->current_hunk_id != hunkid) {
    chd_read(pChdInfo->chd, hunkid, pChdInfo->hunk_buffer);
    pChdInfo->current_hunk_id = hunkid;
  }

  if (track->ctl_addr == 0x01) {
    for (int i = 0; i < track->sector_size; i += 2) {
      ((char*)buffer)[i] = pChdInfo->hunk_buffer[hunk_offset + i + 1];
      ((char*)buffer)[i+1] = pChdInfo->hunk_buffer[hunk_offset + i];
    }
  }
  else {
    if (track->sector_size == 2048)
    {
      memcpy(buffer, syncHdr, 12);
      memcpy((char *)buffer + 0x10, pChdInfo->hunk_buffer + hunk_offset, track->sector_size);
    }
    else {
      memcpy(buffer, pChdInfo->hunk_buffer + hunk_offset, track->sector_size);
    }
  }

  return 1;
}
