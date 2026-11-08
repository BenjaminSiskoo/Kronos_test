/*  Copyright 2026 Kronos team

    This file is part of Kronos.

    Kronos is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kronos is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kronos; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file commfdd.c
    \brief Sega Saturn Floppy Disk Drive (HSS-0128) on the communication
    connector.

    No Sega document of the drive protocol exists in the workspace (ST-121
    "Floppy Disk Information" is about the Boot ROM V0.8 sources, not the
    drive). The protocol below was read in the code of the BIOS backup
    library (device 2 "serial interface", loaded at 060F0000 by the FDD
    Operator disc, dump of the high work RAM), with an SCI trace of its
    detection. Addresses are those of that copy.

    Link (sci.c): master SH-2 SCI, clocked synchronous, BRR = 10h (16 x 17
    = 272 cycles per bit). The Saturn sends with its own clock (SCR = 20h,
    send 060F21D8) and receives with the drive clock (SCR = 12h, CKE1 = 1,
    receive 060F22AA).

    Every frame ends with a CRC-16 (polynomial 1021h, initial value FFFFh,
    not reflected, no final XOR, i.e. CRC-16/CCITT-FALSE), high byte first.
    The receiver (060F23BA) runs the same CRC over the whole frame and wants
    a zero remainder (it doesn't check the low 4 bits of the last byte).

    Frames:
      command    S->D  80 cc p0 p1 p2 p3 + CRC              (060F2484)
      reply      D->S  20 00 mm ee 00 00 + CRC
                       ee = status (0 = OK), mm = FFh "end of exchange",
                       anything else "more to come". After a command, mm
                       must not be FFh; a final status (060F2568) must have
                       mm = FFh.
      request    S->D  40 00 00 00 n0..n10 len 00.. (38 bytes) + CRC
                       n = file name, len = name length for a directory
                       search (12 = whole name); write: comment at +16,
                       language +26, date +28, size +32; directory: table
                       size at +32.
      request reply D->S  6 bytes + CRC, byte 3 = status.
      data       both  10 ff Lh Ll <L bytes> 00 00 + CRC
                       ff = FFh on the last data frame of a transfer.

    Commands (byte cc):
      11h  drive information, p1 = partition (also selects it): reply,
           data frame of 20 bytes (060F266E), final status.
             +0 unit id (<= 15, BupConfig), +1 number of partitions,
             +2 new BRR (0 = keep 10h), +3 ?, +4 total size, +8 total
             blocks, +12 block size (16 bits), +14 maximum data frame
             length (16 bits), +16 ?
      10h  free space for a file of p0-p3 bytes: reply, data frame of 12
           bytes (free size, free blocks, number of such files), final
           status (060F25C8, BUP_Stat).
      20h  format the current partition: reply, final status (060F2732).
      30h  delete: reply, request, request reply, final status (060F279C).
      40h  directory: reply, request, request reply, then data frames of
           32-byte entries (name 11, comment 10, language 1, blocks 16-bit,
           date 32-bit, size 32-bit), each one answered by the Saturn with
           command 00h; the drive's reply to the last one has mm = FFh
           (060F2C08 / 060F2860). A frame of length 4 holding the number
           of matches tells the table is too small (BUP_Dir < 0).
      41h  read: same as 40h with the file data (060F2FD0 / 060F2D82).
      50h  write: reply, request, request reply, then the Saturn sends the
           data frames (060F30DE), the drive answers each with a final
           status (060F32A6).
      00h  "next" after a data frame (see 40h / 41h).

    Status codes (BUP_Read 060F135A, BUP_Write 060F0A78...): 21h no disk
    (BUP_NON), 23h write protected, 24h unformatted, 30h file not found,
    31h disk full.

    Assumptions (not in the library code): unit id 1, 2 partitions of 11392
    blocks of 64 bytes (one per side, 22784 blocks per floppy - SEGA Saturn
    Shiro "Secrets of the Floppy Disk Drive"), space counted per 512-byte
    sector, byte 0 / padding bytes of the drive frames, maximum data frame
    of 7FE0h bytes (so that a directory always fits in one frame: the
    library keeps only the count of the last frame).

    The floppy: <folder>/fdd_part0.bin and fdd_part1.bin (format: see
    FddSave()), loaded at each command, written after each change.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commfdd.h"

#define FDD_PARTITIONS     2
#define FDD_BLOCK_SIZE     64
#define FDD_TOTAL_BLOCKS   11392
#define FDD_SECTOR         512
#define FDD_MAX_FRAME      0x7FE0
#define FDD_MAX_FILES      512
#define FDD_BIT_CYCLES     272

#define ST_OK              0x00
#define ST_NO_DISK         0x21
#define ST_WRITE_PROTECT   0x23
#define ST_UNFORMATTED     0x24
#define ST_NOT_FOUND       0x30
#define ST_DISK_FULL       0x31

typedef struct
{
   char name[11];
   char comment[10];
   u8 language;
   u32 date;
   u32 size;
   u8 * data;
} FddFile;

typedef struct
{
   FddFile files[FDD_MAX_FILES];
   int count;
} FddPartition;

enum
{
   FS_COMMAND,     /* waiting for an 8-byte command frame */
   FS_REQUEST,     /* waiting for the 40-byte request frame */
   FS_DATA_OUT,    /* drive sends data frames, Saturn answers with 00h */
   FS_DATA_IN      /* Saturn sends data frames (write) */
};

static char fdd_folder[1024] = "";
static int fdd_write_protect = 0;
static int fdd_partition = 0;

static FddPartition fdd_part;

/* Saturn -> drive */
static u8  fdd_in[FDD_MAX_FRAME + 64];
static u32 fdd_in_len;
static int fdd_state = FS_COMMAND;
static u8  fdd_cmd;

/* drive -> Saturn */
static u8 * fdd_out = NULL;
static u32 fdd_out_size, fdd_out_len, fdd_out_pos;
#define FDD_MAX_QUEUED 8
static u32 fdd_frame_end[FDD_MAX_QUEUED]; /* end offset of each queued frame */
static int fdd_frame_count;

/* transfer in progress */
static u8 * fdd_xfer = NULL;     /* read data / write data */
static u32 fdd_xfer_len, fdd_xfer_pos;
static FddFile fdd_wfile;
static int fdd_dir_frames_left;
static u8 * fdd_dir = NULL;
static u32 fdd_dir_len;

//////////////////////////////////////////////////////////////////////////////

static u16 FddCrc(const u8 * p, u32 len)
{
   u16 crc = 0xFFFF;
   u32 i;
   int b;
   for (i = 0; i < len; i++)
   {
      crc ^= (u16)(p[i] << 8);
      for (b = 0; b < 8; b++)
         crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
   }
   return crc;
}

static void Put32(u8 * p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 Get32(const u8 * p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static void FddQueue(const u8 * p, u32 len)
{
   u16 crc;
   if (fdd_out_len + len + 2 > fdd_out_size)
   {
      u32 n = (fdd_out_len + len + 2) * 2 + 256;
      u8 * q = (u8 *)realloc(fdd_out, n);
      if (q == NULL)
         return;
      fdd_out = q;
      fdd_out_size = n;
   }
   memcpy(fdd_out + fdd_out_len, p, len);
   crc = FddCrc(p, len);
   fdd_out[fdd_out_len + len] = (u8)(crc >> 8);
   fdd_out[fdd_out_len + len + 1] = (u8)crc;
   fdd_out_len += len + 2;
   if (fdd_frame_count < FDD_MAX_QUEUED)
      fdd_frame_end[fdd_frame_count++] = fdd_out_len;
}

static void FddReply(u8 more, u8 status)
{
   u8 f[6] = { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00 };
   f[2] = more;
   f[3] = status;
   FddQueue(f, 6);
}

/* 10 ff Lh Ll <data> 00 00 */
static void FddDataFrame(u8 last, const u8 * data, u32 len)
{
   u8 * f = (u8 *)calloc(1, len + 6);
   if (f == NULL)
      return;
   f[0] = 0x10;
   f[1] = last ? 0xFF : 0x00;
   f[2] = (u8)(len >> 8);
   f[3] = (u8)len;
   if (len)
      memcpy(f + 4, data, len);
   FddQueue(f, len + 6);
   free(f);
}

//////////////////////////////////////////////////////////////////////////////
// Floppy contents
//
// fdd_partN.bin: "KRONOSFDD1" + 2 bytes 0, u32 count, then per file:
// name 11, comment 10, language 1, date u32, size u32, data (big-endian).
//////////////////////////////////////////////////////////////////////////////

static void FddClear(void)
{
   int i;
   for (i = 0; i < fdd_part.count; i++)
      free(fdd_part.files[i].data);
   fdd_part.count = 0;
}

static int FddDiskIn(void)
{
   return fdd_folder[0] != 0;
}

static void FddPath(char * out, size_t size, int part)
{
   snprintf(out, size, "%s/fdd_part%d.bin", fdd_folder, part);
}

static void FddLoad(void)
{
   char path[1100];
   u8 hdr[16];
   FILE * fp;
   u32 n, i;

   FddClear();
   if (!FddDiskIn())
      return;
   FddPath(path, sizeof(path), fdd_partition);
   fp = fopen(path, "rb");
   if (fp == NULL)
      return; /* empty, formatted floppy */
   if (fread(hdr, 1, 16, fp) != 16 || memcmp(hdr, "KRONOSFDD1", 10) != 0)
   {
      fclose(fp);
      return;
   }
   n = Get32(hdr + 12);
   for (i = 0; i < n && fdd_part.count < FDD_MAX_FILES; i++)
   {
      u8 e[30];
      FddFile * f = &fdd_part.files[fdd_part.count];
      if (fread(e, 1, 30, fp) != 30)
         break;
      memcpy(f->name, e, 11);
      memcpy(f->comment, e + 11, 10);
      f->language = e[21];
      f->date = Get32(e + 22);
      f->size = Get32(e + 26);
      f->data = (u8 *)malloc(f->size ? f->size : 1);
      if (f->data == NULL || fread(f->data, 1, f->size, fp) != f->size)
      {
         free(f->data);
         break;
      }
      fdd_part.count++;
   }
   fclose(fp);
}

static void FddSave(void)
{
   char path[1100];
   u8 hdr[16];
   FILE * fp;
   int i;

   if (!FddDiskIn())
      return;
   FddPath(path, sizeof(path), fdd_partition);
   fp = fopen(path, "wb");
   if (fp == NULL)
      return;
   memset(hdr, 0, sizeof(hdr));
   memcpy(hdr, "KRONOSFDD1", 10);
   Put32(hdr + 12, (u32)fdd_part.count);
   fwrite(hdr, 1, 16, fp);
   for (i = 0; i < fdd_part.count; i++)
   {
      const FddFile * f = &fdd_part.files[i];
      u8 e[30];
      memcpy(e, f->name, 11);
      memcpy(e + 11, f->comment, 10);
      e[21] = f->language;
      Put32(e + 22, f->date);
      Put32(e + 26, f->size);
      fwrite(e, 1, 30, fp);
      fwrite(f->data, 1, f->size, fp);
   }
   fclose(fp);
}

/* Blocks a file takes: whole 512-byte sectors. */
static u32 FddBlocks(u32 size)
{
   u32 sectors = (size + FDD_SECTOR - 1) / FDD_SECTOR;
   if (sectors == 0)
      sectors = 1;
   return sectors * (FDD_SECTOR / FDD_BLOCK_SIZE);
}

static u32 FddUsedBlocks(void)
{
   u32 used = 0;
   int i;
   for (i = 0; i < fdd_part.count; i++)
      used += FddBlocks(fdd_part.files[i].size);
   return used;
}

static int FddFind(const u8 * name)
{
   int i;
   for (i = 0; i < fdd_part.count; i++)
      if (memcmp(fdd_part.files[i].name, name, 11) == 0)
         return i;
   return -1;
}

static void FddDelete(int i)
{
   free(fdd_part.files[i].data);
   memmove(&fdd_part.files[i], &fdd_part.files[i + 1], (size_t)(fdd_part.count - i - 1) * sizeof(FddFile));
   fdd_part.count--;
}

//////////////////////////////////////////////////////////////////////////////
// Commands
//////////////////////////////////////////////////////////////////////////////

static void FddInfo(u8 partition)
{
   u8 d[20];
   if (!FddDiskIn())
   {
      FddReply(0x00, ST_NO_DISK);
      return;
   }
   if (partition < FDD_PARTITIONS)
      fdd_partition = partition;
   FddReply(0x00, ST_OK);
   memset(d, 0, sizeof(d));
   d[0] = 1;                       /* unit id */
   d[1] = FDD_PARTITIONS;
   d[2] = 0;                       /* keep BRR = 10h */
   Put32(d + 4, FDD_TOTAL_BLOCKS * FDD_BLOCK_SIZE);
   Put32(d + 8, FDD_TOTAL_BLOCKS);
   d[12] = (u8)(FDD_BLOCK_SIZE >> 8); d[13] = (u8)FDD_BLOCK_SIZE;
   d[14] = (u8)(FDD_MAX_FRAME >> 8);  d[15] = (u8)FDD_MAX_FRAME;
   FddDataFrame(1, d, sizeof(d));
   FddReply(0xFF, ST_OK);
}

static void FddStat(u32 datasize)
{
   u8 d[12];
   u32 freeblocks, need;
   if (!FddDiskIn())
   {
      FddReply(0x00, ST_NO_DISK);
      return;
   }
   FddLoad();
   FddReply(0x00, ST_OK);
   freeblocks = FDD_TOTAL_BLOCKS - FddUsedBlocks();
   need = FddBlocks(datasize);
   Put32(d, freeblocks * FDD_BLOCK_SIZE);
   Put32(d + 4, freeblocks);
   Put32(d + 8, need ? freeblocks / need : 0);
   FddDataFrame(1, d, sizeof(d));
   FddReply(0xFF, ST_OK);
}

static void FddFormat(u8 partition)
{
   if (!FddDiskIn())
   {
      FddReply(0x00, ST_NO_DISK);
      return;
   }
   if (partition < FDD_PARTITIONS)
      fdd_partition = partition;
   FddReply(0x00, ST_OK);
   if (fdd_write_protect)
   {
      FddReply(0xFF, ST_WRITE_PROTECT);
      return;
   }
   FddClear();
   FddSave();
   FddReply(0xFF, ST_OK);
}

/* Next directory / read data frame (after the request reply, then after
   each 00h command). */
static void FddSendNextFrame(void)
{
   if (fdd_cmd == 0x40)
   {
      FddDataFrame(1, fdd_dir, fdd_dir_len);
      fdd_dir_frames_left = 0;
      return;
   }
   {
      u32 len = fdd_xfer_len - fdd_xfer_pos;
      int last = 1;
      if (len > FDD_MAX_FRAME)
      {
         len = FDD_MAX_FRAME;
         last = 0;
      }
      FddDataFrame((u8)last, fdd_xfer + fdd_xfer_pos, len);
      fdd_xfer_pos += len;
      fdd_dir_frames_left = !last;
   }
}

static void FddRequest(const u8 * r)
{
   const u8 * name = r + 4;
   int i;

   FddLoad();

   switch (fdd_cmd)
   {
      case 0x30: /* delete */
         if (fdd_write_protect) { FddReply(0x00, ST_WRITE_PROTECT); break; }
         i = FddFind(name);
         if (i < 0) { FddReply(0x00, ST_NOT_FOUND); break; }
         FddDelete(i);
         FddSave();
         FddReply(0x00, ST_OK);
         FddReply(0xFF, ST_OK);
         break;

      case 0x40: /* directory */
      {
         u32 len = r[15];
         u32 tbsize = Get32(r + 32);
         u32 matches = 0, n;
         if (len > 11) len = 11;
         for (i = 0; i < fdd_part.count; i++)
            if (memcmp(fdd_part.files[i].name, name, len) == 0)
               matches++;
         FddReply(0x00, ST_OK);
         free(fdd_dir);
         fdd_dir = NULL;
         if (matches > tbsize || matches * 32 > FDD_MAX_FRAME)
         {
            /* table too small: frame of 4 bytes with the number of files */
            fdd_dir = (u8 *)malloc(4);
            Put32(fdd_dir, matches);
            fdd_dir_len = 4;
         }
         else
         {
            fdd_dir = (u8 *)calloc(1, matches * 32 + 1);
            n = 0;
            for (i = 0; i < fdd_part.count; i++)
            {
               const FddFile * f = &fdd_part.files[i];
               u8 * e;
               u32 b = FddBlocks(f->size);
               if (memcmp(f->name, name, len) != 0)
                  continue;
               e = fdd_dir + n * 32;
               memcpy(e, f->name, 11);
               memcpy(e + 11, f->comment, 10);
               e[21] = f->language;
               e[22] = (u8)(b >> 8); e[23] = (u8)b;
               Put32(e + 24, f->date);
               Put32(e + 28, f->size);
               n++;
            }
            fdd_dir_len = n * 32;
         }
         fdd_state = FS_DATA_OUT;
         FddSendNextFrame();
         break;
      }

      case 0x41: /* read */
         i = FddFind(name);
         if (i < 0) { FddReply(0x00, ST_NOT_FOUND); break; }
         FddReply(0x00, ST_OK);
         free(fdd_xfer);
         fdd_xfer_len = fdd_part.files[i].size;
         fdd_xfer = (u8 *)malloc(fdd_xfer_len ? fdd_xfer_len : 1);
         memcpy(fdd_xfer, fdd_part.files[i].data, fdd_xfer_len);
         fdd_xfer_pos = 0;
         fdd_state = FS_DATA_OUT;
         FddSendNextFrame();
         break;

      case 0x50: /* write */
      {
         u32 size = Get32(r + 32);
         u32 used;
         if (fdd_write_protect) { FddReply(0x00, ST_WRITE_PROTECT); break; }
         used = FddUsedBlocks();
         i = FddFind(name);
         if (i >= 0)
            used -= FddBlocks(fdd_part.files[i].size); /* replaced */
         if (used + FddBlocks(size) > FDD_TOTAL_BLOCKS ||
             (i < 0 && fdd_part.count >= FDD_MAX_FILES))
         {
            FddReply(0x00, ST_DISK_FULL);
            break;
         }
         memset(&fdd_wfile, 0, sizeof(fdd_wfile));
         memcpy(fdd_wfile.name, name, 11);
         memcpy(fdd_wfile.comment, r + 16, 10);
         fdd_wfile.language = r[26];
         fdd_wfile.date = Get32(r + 28);
         fdd_wfile.size = size;
         free(fdd_xfer);
         fdd_xfer = (u8 *)calloc(1, size ? size : 1);
         fdd_xfer_len = size;
         fdd_xfer_pos = 0;
         FddReply(0x00, ST_OK);
         fdd_state = FS_DATA_IN;
         break;
      }
   }
   if (fdd_state == FS_REQUEST)
      fdd_state = FS_COMMAND;
}

static void FddWriteDone(void)
{
   int i = FddFind((const u8 *)fdd_wfile.name);
   if (i >= 0)
      FddDelete(i);
   if (fdd_part.count < FDD_MAX_FILES)
   {
      fdd_wfile.data = fdd_xfer;
      fdd_xfer = NULL;
      fdd_part.files[fdd_part.count++] = fdd_wfile;
      FddSave();
   }
}

static void FddCommand(const u8 * c)
{
   fdd_cmd = c[1];
   switch (fdd_cmd)
   {
      case 0x00: /* next frame */
         if (fdd_state == FS_DATA_OUT && fdd_dir_frames_left)
         {
            FddReply(0x00, ST_OK);
            FddSendNextFrame();
         }
         else
         {
            FddReply(0xFF, ST_OK);
            fdd_state = FS_COMMAND;
         }
         return;
      case 0x10: FddStat(Get32(c + 2)); break;
      case 0x11: FddInfo(c[3]); break;
      case 0x20: FddFormat(c[3]); break;
      case 0x30:
      case 0x40:
      case 0x41:
      case 0x50:
         if (!FddDiskIn()) { FddReply(0x00, ST_NO_DISK); break; }
         FddReply(0x00, ST_OK);
         fdd_state = FS_REQUEST;
         return;
      default:
         FddReply(0x00, ST_UNFORMATTED);
         break;
   }
   fdd_state = FS_COMMAND;
}

/* A whole frame from the Saturn is in fdd_in. */
static void FddFrame(void)
{
   if (FddCrc(fdd_in, fdd_in_len - 2) != (u16)((fdd_in[fdd_in_len - 2] << 8) | fdd_in[fdd_in_len - 1]))
   {
      fdd_state = (fdd_state == FS_REQUEST || fdd_state == FS_DATA_IN) ? FS_COMMAND : fdd_state;
      return; /* bad frame: no answer, the Saturn times out */
   }

   switch (fdd_state)
   {
      case FS_REQUEST:
         FddRequest(fdd_in);
         break;
      case FS_DATA_IN:
      {
         const u32 len = ((u32)fdd_in[2] << 8) | fdd_in[3];
         const int last = (fdd_in[1] == 0xFF);
         u32 n = len;
         if (fdd_xfer_pos + n > fdd_xfer_len)
            n = (fdd_xfer_pos < fdd_xfer_len) ? fdd_xfer_len - fdd_xfer_pos : 0;
         if (n && fdd_xfer)
            memcpy(fdd_xfer + fdd_xfer_pos, fdd_in + 4, n);
         fdd_xfer_pos += n;
         FddReply(last ? 0xFF : 0x00, ST_OK);
         if (last && fdd_xfer != NULL)
            FddWriteDone();
         /* An exact multiple of the frame length gives one more empty last
            frame (060F33EE: size / frame + 1 frames): stay in FS_DATA_IN
            until a command frame comes. */
         if (last && fdd_xfer == NULL)
            fdd_state = FS_DATA_IN;
         break;
      }
      default:
         FddCommand(fdd_in);
         break;
   }
}

/* Length of the frame being received, 0 = not known yet. */
static u32 FddExpectedLength(void)
{
   if (fdd_state == FS_REQUEST)
      return 40;
   if (fdd_state == FS_DATA_IN)
   {
      if (fdd_in_len >= 1 && fdd_in[0] == 0x80)
         return 8;             /* a command after the last data frame */
      if (fdd_in_len < 4)
         return 0;
      return (((u32)fdd_in[2] << 8) | fdd_in[3]) + 8;
   }
   return 8;
}

//////////////////////////////////////////////////////////////////////////////
// CommPortDevice
//////////////////////////////////////////////////////////////////////////////

static void FddReset(void)
{
   fdd_state = FS_COMMAND;
   fdd_in_len = 0;
   fdd_out_len = fdd_out_pos = 0;
   fdd_frame_count = 0;
   fdd_partition = 0;
}

static void FddTransmit(int cpu, u8 data, int sync)
{
   u32 expect;
   (void)sync;
   if (cpu != SCI_CPU_MASTER)
      return;

   /* After the last data frame of a write, a command (80h) ends the
      transfer. During a read / directory, the Saturn's "next" commands
      (80 00 ...) are handled by FddCommand(). */
   if (fdd_in_len == 0 && data == 0x80 && fdd_state == FS_DATA_IN && fdd_xfer == NULL)
      fdd_state = FS_COMMAND;

   if (fdd_in_len < sizeof(fdd_in))
      fdd_in[fdd_in_len++] = data;

   expect = FddExpectedLength();
   if (expect != 0 && fdd_in_len >= expect)
   {
      /* The Saturn reads the answer from scratch. */
      fdd_out_len = fdd_out_pos = 0;
      fdd_frame_count = 0;
      if (fdd_state == FS_DATA_IN && fdd_in[0] == 0x80)
         fdd_state = FS_COMMAND;
      FddFrame();
      fdd_in_len = 0;
   }
}

static int FddReceive(int cpu, u8 * data, int sync)
{
   (void)sync;
   if (cpu != SCI_CPU_MASTER || fdd_out_pos >= fdd_out_len)
   {
      *data = 0xFF;
      return 0;
   }
   *data = fdd_out[fdd_out_pos++];
   if (fdd_out_pos >= fdd_out_len)
   {
      fdd_out_len = fdd_out_pos = 0;
      fdd_frame_count = 0;
      return SCI_RX_END_OF_FRAME;
   }
   /* Last byte of a frame (not of the queue): the drive waits for the
      Saturn to enable its receiver again before the next frame, which it
      does for each frame (060F22AA: SCR = 12h ... SCR &= 0Fh). */
   if (fdd_frame_count > 0 && fdd_out_pos == fdd_frame_end[0])
   {
      memmove(fdd_frame_end, fdd_frame_end + 1, (size_t)(fdd_frame_count - 1) * sizeof(fdd_frame_end[0]));
      fdd_frame_count--;
      return SCI_RX_END_OF_FRAME;
   }
   return 1;
}

static u32 FddExternalBitCycles(int cpu)
{
   return (cpu == SCI_CPU_MASTER) ? FDD_BIT_CYCLES : 0;
}

static CommPortDevice FddDevice = {
   "Sega Saturn Floppy Disk Drive (HSS-0128)",
   FddReset,
   FddTransmit,
   FddReceive,
   FddExternalBitCycles
};

CommPortDevice * FddGetDevice(void)
{
   return &FddDevice;
}

void FddSetFolder(const char * folder)
{
   if (folder == NULL)
      folder = "";
   snprintf(fdd_folder, sizeof(fdd_folder), "%s", folder);
   FddClear();
}

void FddSetWriteProtect(int on)
{
   fdd_write_protect = on ? 1 : 0;
}
