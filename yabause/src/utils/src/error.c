/*  Copyright 2005-2006 Theo Berkau

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

/*! \file error.c
    \brief Error handling functions.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "error.h"
#include "yui.h"

//////////////////////////////////////////////////////////////////////////////

static void AllocAmendPrintString(const char *string1, const char *string2)
{
   char *string;
   size_t length = 0;
   if (string1 != NULL) length += strlen(string1);
   if (string2 != NULL) length += strlen(string2);
   if ((string = (char *)malloc(length + 2)) == NULL)
      return;

   if ((string1 != NULL)&&(string2 != NULL)) sprintf(string, "%s%s", string1, string2);
   if ((string1 != NULL)&&(string2 == NULL)) sprintf(string, "%s", string1);
   if ((string1 == NULL)&&(string2 != NULL)) sprintf(string, "%s", string2);
   YuiErrorMsg(string);

   free(string);
}

//////////////////////////////////////////////////////////////////////////////

void YabSetError(int type, const void *extra)
{
   char tempstr[2048];
   char regstr[512];
   SH2_struct *sh;

   switch (type)
   {
      case YAB_ERR_FILENOTFOUND:
         AllocAmendPrintString(_("File not found: "), (const char *)extra);
         break;
      case YAB_ERR_MEMORYALLOC:
         YuiErrorMsg(_("Error allocating memory"));
         break;
      case YAB_ERR_FILEREAD:
         AllocAmendPrintString(_("Error reading file: "), (const char *)extra);
         break;
      case YAB_ERR_FILEWRITE:
         AllocAmendPrintString(_("Error writing file: "), (const char *)extra);
         break;
      case YAB_ERR_CANNOTINIT:
         AllocAmendPrintString(_("Cannot initialize "), (const char *)extra);
         break;
      case YAB_ERR_SH2INVALIDOPCODE:
         sh = (SH2_struct *)extra;
#ifdef DMPHISTORY
         SH2DumpHistory(sh);
#endif
         SH2FormatRegs(sh, regstr, sizeof(regstr));
         snprintf(tempstr, sizeof(tempstr), "%s SH2 invalid opcode\n\n%s",
                  sh->isslave ? "Slave" : "Master", regstr);
         YuiMsg(tempstr);
         break;
#ifdef SH2_HANG_WATCH
      case YAB_ERR_SH2HANG:
         sh = (SH2_struct *)extra;
         SH2HangWatchFormat(sh, tempstr, sizeof(tempstr));
         YuiMsg(tempstr);
         break;
#endif
      case YAB_ERR_SH2READ:
         YuiErrorMsg(_("SH2 read error")); // fix me
         break;
      case YAB_ERR_SH2WRITE:
         YuiErrorMsg(_("SH2 write error")); // fix me
         break;
      case YAB_ERR_SDL:
         AllocAmendPrintString(_("SDL Error: "), (const char *)extra);
         break;
      case YAB_ERR_OTHER:
         YuiErrorMsg((char *)extra);
         break;
      case YAB_ERR_UNKNOWN:
      default:
         YuiErrorMsg(_("Unknown error occurred\n"));
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

void YabErrorMsg(const char * format, ...) {
    va_list l;
    int n;
    char * buffer;

    va_start(l, format);
    n = vsnprintf(NULL, 0, format, l);
    va_end(l);

    buffer = (char *)malloc(n + 1);

    va_start(l, format);
    vsprintf(buffer, format, l);
    va_end(l);

    YuiErrorMsg(buffer);
    free(buffer);
}

//////////////////////////////////////////////////////////////////////////////
