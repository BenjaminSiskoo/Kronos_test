/*  Copyright 2015 Theo Berkau

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

#include <stdio.h>
#include <stdarg.h>
#include "scsp.h"
#include "scspdsp.h"

s32 float_to_int(u16 f_val);
u16 int_to_float(u32 i_val);

//saturate 24 bit signed integer
static INLINE s32 saturate_24(s32 value)
{
   if (value > 8388607)
      value = 8388607;

   if (value < (-8388608))
      value = (-8388608);

   return value;
}


#define sign_x_to_s32(_bits, _value) (((int)((u32)(_value) << (32 - _bits))) >> (32 - _bits))
#define min(a,b) (a<b)?a:b

static INLINE unsigned clz(u32 v)
{
#if defined(__GNUC__) || defined(__clang__) || defined(__ICC) || defined(__INTEL_COMPILER)
  return __builtin_clz(v);
#elif defined(_MSC_VER)
  unsigned long idx;

  _BitScanReverse(&idx, v);

  return 31 ^ idx;
#else
  unsigned ret = 0;
  unsigned tmp;

  tmp = !(v & 0xFFFF0000) << 4; v <<= tmp; ret += tmp;
  tmp = !(v & 0xFF000000) << 3; v <<= tmp; ret += tmp;
  tmp = !(v & 0xF0000000) << 2; v <<= tmp; ret += tmp;
  tmp = !(v & 0xC0000000) << 1; v <<= tmp; ret += tmp;
  tmp = !(v & 0x80000000) << 0;            ret += tmp;

  return(ret);
#endif
}

void ScspDspExec(ScspDsp* dsp, int addr, u8 * sound_ram)
{
  u16* sound_ram_16 = (u16*)sound_ram;
  u64 mul_temp = 0;
  int nofl = 0;
  u32 x_temp = 0;
  s32 y_extended = 0;
  union ScspDspInstruction inst;
  u32 address = 0;
  s32 shift_temp = 0;

  inst.all = scsp_dsp.mpro[addr];

  const unsigned TEMPWriteAddr = (inst.part.twa + dsp->mdec_ct) & 0x7F;
  const unsigned TEMPReadAddr = (inst.part.tra + dsp->mdec_ct) & 0x7F;

  if (inst.part.ira & 0x20) {
    if (inst.part.ira & 0x10) {
      if (!(inst.part.ira & 0xE))
        dsp->inputs = dsp->exts[inst.part.ira & 0x1] * 256;
    }else{
      dsp->inputs = dsp->mixs[inst.part.ira & 0xF] * 16;
    }
  }else{
    dsp->inputs = dsp->mems[inst.part.ira & 0x1F];
  }

  const int INPUTS = sign_x_to_s32(24, dsp->inputs);
  const int TEMP = sign_x_to_s32(24, dsp->temp[TEMPReadAddr]);
  const int X_SEL_Inputs[2] = { TEMP, INPUTS };
  const u16 Y_SEL_Inputs[4] = {
    dsp->frc_reg, dsp->coef[inst.part.coef],
    (u16)((dsp->y_reg >> 11) & 0x1FFF),
    (u16)((dsp->y_reg >> 4) & 0x0FFF)
  };
  const u32 SGA_Inputs[2] = { (u32)TEMP, dsp->shift_reg }; // ToDO:?

  if (inst.part.yrl) {
    dsp->y_reg = INPUTS & 0xFFFFFF;
  }

  int ShifterOutput;

  ShifterOutput = (u32)sign_x_to_s32(26, dsp->shift_reg) << (inst.part.shift0 ^ inst.part.shift1);

  if (!inst.part.shift1)
  {
    if(ShifterOutput > 0x7FFFFF)
      ShifterOutput = 0x7FFFFF;
    else if(ShifterOutput < -0x800000)
      ShifterOutput = 0x800000;
  }
  ShifterOutput &= 0xFFFFFF;

  if (inst.part.ewt)
    dsp->efreg[inst.part.ewa] = (ShifterOutput >> 8);

  if (inst.part.twt)
    dsp->temp[TEMPWriteAddr] = ShifterOutput;

  if (inst.part.frcl)
  {
    const unsigned F_SEL_Inputs[2] = { (unsigned)(ShifterOutput >> 11), (unsigned)(ShifterOutput & 0xFFF) };

    dsp->frc_reg = F_SEL_Inputs[inst.part.shift0 & inst.part.shift1];
    //printf("FRCL: 0x%08x\n", DSP.FRC_REG);
  }

  dsp->product = ((s64)sign_x_to_s32(13, Y_SEL_Inputs[inst.part.ysel]) * X_SEL_Inputs[inst.part.xsel]) >> 12;

  s32 SGAOutput;

  SGAOutput = SGA_Inputs[inst.part.bsel];

  if (inst.part.negb)
    SGAOutput = -SGAOutput;

  if (inst.part.zero)
    SGAOutput = 0;

  dsp->shift_reg = (dsp->product + SGAOutput) & 0x3FFFFFF;
  //
  //
  if (inst.part.iwt)
  {
    dsp->mems[inst.part.iwa] = dsp->read_value;
  }

  if (dsp->read_pending)
  {
    u16 tmp = sound_ram_16[dsp->io_addr];
    dsp->read_value = (dsp->read_pending == 2) ? (tmp << 8) : float_to_int(tmp);
    dsp->read_pending = 0;
  }
  else if (dsp->write_pending)
  {
    if (!(dsp->io_addr & 0x40000))
      sound_ram_16[dsp->io_addr] = dsp->write_value;
    dsp->write_pending = 0;
  }
  {
    u16 addr;

    addr = dsp->madrs[inst.part.masa];
    addr += inst.part.nxadr;

    if (inst.part.adreb)
    {
      addr += sign_x_to_s32(12, dsp->adrs_reg);
    }

    if (!inst.part.table)
    {
      addr += dsp->mdec_ct;
      addr &= (0x2000 << dsp->rbl) - 1;
    }

    dsp->io_addr = (addr + (dsp->rbp << 12)) & 0x3FFFF;

    if (inst.part.mrd)
    {
      dsp->read_pending = 1 + inst.part.nofl;
    }
    if (inst.part.mwt)
    {
      dsp->write_pending = 1;
      dsp->write_value = inst.part.nofl ? (ShifterOutput >> 8) : int_to_float(ShifterOutput);
    }
    if (inst.part.adrl)
    {
      const u16 A_SEL_Inputs[2] = { (u16)((INPUTS >> 16) & 0xFFF), (u16)(ShifterOutput >> 12) };

      dsp->adrs_reg = A_SEL_Inputs[inst.part.shift0 & inst.part.shift1];
    }
  }
}


//sign extended to 32 bits instead of 24
s32 float_to_int(u16 f_val)
{
   u32 sign = (f_val >> 15) & 1;
   u32 sign_inverse = (!sign) & 1;
   u32 exponent = (f_val >> 11) & 0xf;
   u32 mantissa = f_val & 0x7FF;

   s32 ret_val = sign << 31;

   if (exponent > 11)
   {
      exponent = 11;
      ret_val |= (sign << 30);
   }
   else
      ret_val |= (sign_inverse << 30);

   ret_val |= mantissa << 19;

   ret_val = ret_val >> (exponent + (1 << 3));

   return ret_val;
}

u16 int_to_float(u32 i_val)
{
   u32 sign = (i_val >> 23) & 1;
   u32 exponent = 0;

   if (sign != 0)
      i_val = (~i_val) & 0x7FFFFF;

   if (i_val <= 0x1FFFF)
   {
      i_val *= 64;
      exponent += 0x3000;
   }

   if (i_val <= 0xFFFFF)
   {
      i_val *= 8;
      exponent += 0x1800;
   }

   if (i_val <= 0x3FFFFF)
   {
      i_val *= 2;
      exponent += 0x800;
   }

   if (i_val <= 0x3FFFFF)
   {
      i_val *= 2;
      exponent += 0x800;
   }

   if (i_val <= 0x3FFFFF)
      exponent += 0x800;

   i_val >>= 11;
   i_val &= 0x7ff;
   i_val |= exponent;

   if (sign != 0)
      i_val ^= (0x7ff | (1 << 15));

   return i_val;
}

int ScspDspAssembleGetValue(char* instruction)
{
   char temp[512] = { 0 };
   int value = 0;
   sscanf(instruction, "%s %d", temp, &value);
   return value;
}

u64 ScspDspAssembleLine(char* line)
{
   union ScspDspInstruction instruction = { 0 };

   char* temp = NULL;

   if ((temp = strstr(line, "tra")))
   {
      instruction.part.tra = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "twt"))
   {
      instruction.part.twt = 1;
   }

   if ((temp = strstr(line, "twa")))
   {
      instruction.part.twa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "xsel"))
   {
      instruction.part.xsel = 1;
   }

   if ((temp = strstr(line, "ysel")))
   {
      instruction.part.ysel = ScspDspAssembleGetValue(temp);
   }

   if ((temp = strstr(line, "ira")))
   {
      instruction.part.ira = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "iwt"))
   {
      instruction.part.iwt = 1;
   }

   if ((temp = strstr(line, "iwa")))
   {
      instruction.part.iwa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "table"))
   {
      instruction.part.table = 1;
   }

   if (strstr(line, "mwt"))
   {
      instruction.part.mwt = 1;
   }

   if (strstr(line, "mrd"))
   {
      instruction.part.mrd = 1;
   }

   if (strstr(line, "ewt"))
   {
      instruction.part.ewt = 1;
   }

   if ((temp = strstr(line, "ewa")))
   {
      instruction.part.ewa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "adrl"))
   {
      instruction.part.adrl = 1;
   }

   if (strstr(line, "frcl"))
   {
      instruction.part.frcl = 1;
   }

   if ((temp = strstr(line, "shift")))
   {
      instruction.part.shift1 = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "yrl"))
   {
      instruction.part.yrl = 1;
   }

   if (strstr(line, "negb"))
   {
      instruction.part.negb = 1;
   }

   if (strstr(line, "zero"))
   {
      instruction.part.zero = 1;
   }

   if (strstr(line, "bsel"))
   {
      instruction.part.bsel = 1;
   }

   if (strstr(line, "nofl"))
   {
      instruction.part.nofl = 1;
   }

   if ((temp = strstr(line, "coef")))
   {
      instruction.part.coef = ScspDspAssembleGetValue(temp);
   }

   if ((temp = strstr(line, "masa")))
   {
      instruction.part.masa = ScspDspAssembleGetValue(temp);
   }

   if (strstr(line, "adreb"))
   {
      instruction.part.adreb = 1;
   }

   if (strstr(line, "nxadr"))
   {
      instruction.part.adreb = 1;
   }

   if (strstr(line, "nop"))
   {
      instruction.all = 0;
   }

   return instruction.all;
}

void ScspDspAssembleFromFile(char * filename, u64* output)
{
   int i;
   char line[1024] = { 0 };

   FILE * fp = fopen(filename, "r");

   if (!fp)
   {
      return;
   }

   for (i = 0; i < 128; i++)
   {
      char * result = fgets(line, sizeof(line), fp);
      output[i] = ScspDspAssembleLine(line);
   }
   fclose(fp);
}

void ScspDspDisasm(u8 addr, char *outstring)
{
   union ScspDspInstruction instruction;

   instruction.all = scsp_dsp.mpro[addr];

   sprintf(outstring, "%02X: ", addr);
   outstring += strlen(outstring);

   if (instruction.all == 0)
   {
      sprintf(outstring, "nop ");
      outstring += strlen(outstring);
      return;
   }

   if (instruction.part.nofl)
   {
      sprintf(outstring, "nofl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.coef)
   {
      sprintf(outstring, "coef %02X ", (unsigned int)(instruction.part.coef & 0x3F));
      outstring += strlen(outstring);
   }

   if (instruction.part.masa)
   {
      sprintf(outstring, "masa %02X ", (unsigned int)(instruction.part.masa & 0x1F));
      outstring += strlen(outstring);
   }

   if (instruction.part.adreb)
   {
      sprintf(outstring, "adreb ");
      outstring += strlen(outstring);
   }

   if (instruction.part.nxadr)
   {
      sprintf(outstring, "nxadr ");
      outstring += strlen(outstring);
   }

   if (instruction.part.table)
   {
      sprintf(outstring, "table ");
      outstring += strlen(outstring);
   }

   if (instruction.part.mwt)
   {
      sprintf(outstring, "mwt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.mrd)
   {
      sprintf(outstring, "mrd ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ewt)
   {
      sprintf(outstring, "ewt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ewa)
   {
      sprintf(outstring, "ewa %01X ", (unsigned int)(instruction.part.ewa & 0xf));
      outstring += strlen(outstring);
   }

   if (instruction.part.adrl)
   {
      sprintf(outstring, "adrl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.frcl)
   {
      sprintf(outstring, "frcl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.shift1)
   {
      sprintf(outstring, "shift %d ", (int)(instruction.part.shift1 & 3));
      outstring += strlen(outstring);
   }

   if (instruction.part.yrl)
   {
      sprintf(outstring, "yrl ");
      outstring += strlen(outstring);
   }

   if (instruction.part.negb)
   {
      sprintf(outstring, "negb ");
      outstring += strlen(outstring);
   }

   if (instruction.part.zero)
   {
      sprintf(outstring, "zero ");
      outstring += strlen(outstring);
   }

   if (instruction.part.bsel)
   {
      sprintf(outstring, "bsel ");
      outstring += strlen(outstring);
   }

   if (instruction.part.xsel)
   {
      sprintf(outstring, "xsel ");
      outstring += strlen(outstring);
   }

   if (instruction.part.ysel)
   {
      sprintf(outstring, "ysel %d ", (int)(instruction.part.ysel & 3));
      outstring += strlen(outstring);
   }

   if (instruction.part.ira)
   {
      sprintf(outstring, "ira %02X ", (int)(instruction.part.ira & 0x3F));
      outstring += strlen(outstring);
   }

   if (instruction.part.iwt)
   {
      sprintf(outstring, "iwt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.iwa)
   {
      sprintf(outstring, "iwa %02X ", (unsigned int)(instruction.part.iwa & 0x1F));
      outstring += strlen(outstring);
   }

   if (instruction.part.tra)
   {
      sprintf(outstring, "tra %02X ", (unsigned int)(instruction.part.tra & 0x7F));
      outstring += strlen(outstring);
   }

   if (instruction.part.twt)
   {
      sprintf(outstring, "twt ");
      outstring += strlen(outstring);
   }

   if (instruction.part.twa)
   {
      sprintf(outstring, "twa %02X ", (unsigned int)(instruction.part.twa & 0x7F));
      outstring += strlen(outstring);
   }

   if (instruction.part.unknown)
   {
      sprintf(outstring, "unknown ");
      outstring += strlen(outstring);
   }

   if (instruction.part.unknown2)
   {
      sprintf(outstring, "unknown2 ");
      outstring += strlen(outstring);
   }

//   if (instruction.part.unknown3)
 //  {
 //     sprintf(outstring, "unknown3 %d", (int)(instruction.part.unknown3 & 3));
 //     outstring += strlen(outstring);
 //  }
}

void ScspDspDisassembleToFile(char * filename)
{
   int i;
   FILE * fp = fopen(filename, "w");

   if (!fp)
   {
      return;
   }

   for (i = 0; i < 128; i++)
   {
      char output[1024] = { 0 };
      ScspDspDisasm(i, output);
      fprintf(fp, "%s\n", output);
   }

   fclose(fp);
}

//////////////////////////////////////////////////////////////////////////////
// Debug: breakpoints, single-step, memory dumps
//
// Same idioms as M68KAddCodeBreakpoint/M68KSortCodeBreakpoints/etc. in
// scsp.c and ScuDspAddCodeBreakpoint/ScuDspStep in scu.c, so the Qt
// debugger code can treat all three DSP/CPU debuggers uniformly.
//////////////////////////////////////////////////////////////////////////////

static scspdspcodebreakpoint_struct scspdsp_codebreakpoint[SCSPDSP_MAX_BREAKPOINTS];
static int scspdsp_numcodebreakpoints = 0;
static void (*ScspDspBreakpointCallBack)(u32) = NULL;
static int scspdsp_inbreakpoint = 0;

// Debug-only cursor used by ScspDspStep(); independent from the addr
// argument the normal per-sample loop in scsp.c passes to ScspDspExec(),
// so single-stepping in the debugger never disturbs real playback timing.
static u32 scspdsp_debug_pc = 0;

void ScspDspSetBreakpointCallBack(void (*func)(u32))
{
   ScspDspBreakpointCallBack = func;
}

int ScspDspAddCodeBreakpoint(u32 addr)
{
   int i;

   if (addr > 0x7F)
      return -1;

   if (scspdsp_numcodebreakpoints < SCSPDSP_MAX_BREAKPOINTS)
   {
      // Make sure it isn't already on the list
      for (i = 0; i < scspdsp_numcodebreakpoints; i++)
      {
         if (addr == scspdsp_codebreakpoint[i].addr)
            return -1;
      }

      scspdsp_codebreakpoint[scspdsp_numcodebreakpoints].addr = addr;
      scspdsp_numcodebreakpoints++;

      return 0;
   }

   return -1;
}

void ScspDspSortCodeBreakpoints(void)
{
   int i, i2;
   u32 tmp;

   for (i = 0; i < (SCSPDSP_MAX_BREAKPOINTS - 1); i++)
   {
      for (i2 = i + 1; i2 < SCSPDSP_MAX_BREAKPOINTS; i2++)
      {
         if (scspdsp_codebreakpoint[i].addr == 0xFFFFFFFF &&
             scspdsp_codebreakpoint[i2].addr != 0xFFFFFFFF)
         {
            tmp = scspdsp_codebreakpoint[i].addr;
            scspdsp_codebreakpoint[i].addr = scspdsp_codebreakpoint[i2].addr;
            scspdsp_codebreakpoint[i2].addr = tmp;
         }
      }
   }
}

int ScspDspDelCodeBreakpoint(u32 addr)
{
   int i;

   if (scspdsp_numcodebreakpoints > 0)
   {
      for (i = 0; i < scspdsp_numcodebreakpoints; i++)
      {
         if (scspdsp_codebreakpoint[i].addr == addr)
         {
            scspdsp_codebreakpoint[i].addr = 0xFFFFFFFF;
            ScspDspSortCodeBreakpoints();
            scspdsp_numcodebreakpoints--;
            return 0;
         }
      }
   }

   return -1;
}

void ScspDspClearCodeBreakpoints(void)
{
   int i;

   for (i = 0; i < SCSPDSP_MAX_BREAKPOINTS; i++)
      scspdsp_codebreakpoint[i].addr = 0xFFFFFFFF;

   scspdsp_numcodebreakpoints = 0;
}

scspdspcodebreakpoint_struct *ScspDspGetBreakpointList(void)
{
   return scspdsp_codebreakpoint;
}

int ScspDspGetNumCodeBreakpoints(void)
{
   return scspdsp_numcodebreakpoints;
}

void ScspDspCheckBreakpoints(u32 addr)
{
   int i;

   // Cheap early-out: no breakpoints set, nothing to do. Keeps this call
   // effectively free in the hot per-sample loop when no debugger is
   // attached (same reasoning as m68kexecptr only switching to the
   // breakpoint-checking M68KExecBP when a breakpoint actually exists).
   if (scspdsp_numcodebreakpoints == 0)
      return;

   for (i = 0; i < scspdsp_numcodebreakpoints; i++)
   {
      if (scspdsp_codebreakpoint[i].addr == addr && scspdsp_inbreakpoint == 0)
      {
         scspdsp_inbreakpoint = 1;
         if (ScspDspBreakpointCallBack)
            ScspDspBreakpointCallBack(addr);
         scspdsp_inbreakpoint = 0;
      }
   }
}

u32 ScspDspGetPC(void)
{
   return scspdsp_debug_pc;
}

void ScspDspSetPC(u32 addr)
{
   scspdsp_debug_pc = addr & 0x7F;
}

void ScspDspStep(void)
{
   u32 last_step = scsp_dsp.last_step ? (u32)scsp_dsp.last_step : 1;

   if (scspdsp_debug_pc >= last_step)
      scspdsp_debug_pc = 0;

   ScspDspExec(&scsp_dsp, (int)scspdsp_debug_pc, SoundRam);

   scspdsp_debug_pc++;
   if (scspdsp_debug_pc >= last_step)
      scspdsp_debug_pc = 0;
}

//////////////////////////////////////////////////////////////////////////////

static int ScspDspSaveBuffer(const char *filename, const void *data, size_t elemsize, size_t count)
{
   FILE *fp;

   if (!filename)
      return -1;

   if ((fp = fopen(filename, "wb")) == NULL)
      return -1;

   fwrite(data, elemsize, count, fp);
   fclose(fp);
   return 0;
}

int ScspDspSaveProgram(const char *filename)
{
   return ScspDspSaveBuffer(filename, scsp_dsp.mpro, sizeof(u64), 128);
}

int ScspDspSaveCoef(const char *filename)
{
   return ScspDspSaveBuffer(filename, scsp_dsp.coef, sizeof(u16), 64);
}

int ScspDspSaveMadrs(const char *filename)
{
   return ScspDspSaveBuffer(filename, scsp_dsp.madrs, sizeof(u16), 32);
}

int ScspDspSaveTemp(const char *filename)
{
   return ScspDspSaveBuffer(filename, scsp_dsp.temp, sizeof(s32), 128);
}

int ScspDspSaveMems(const char *filename)
{
   return ScspDspSaveBuffer(filename, scsp_dsp.mems, sizeof(s32), 32);
}

int ScspDspSaveMixs(const char *filename)
{
   return ScspDspSaveBuffer(filename, scsp_dsp.mixs, sizeof(s32), 16);
}

//////////////////////////////////////////////////////////////////////////////

// Small helper: appends via snprintf into *pos, tracking remaining space in
// *remaining so we never write past the caller's buffer no matter how the
// buffer was sized. Silently stops appending (rather than truncating mid
// escape sequence or overflowing) once space runs out.
static void appendf(char **pos, size_t *remaining, const char *fmt, ...)
{
   va_list args;
   int written;

   if (*remaining <= 1)
      return; // only room left for (or already used up by) the NUL terminator

   va_start(args, fmt);
   written = vsnprintf(*pos, *remaining, fmt, args);
   va_end(args);

   if (written < 0)
      return;

   if ((size_t)written >= *remaining)
   {
      // vsnprintf truncated: consume the rest of the buffer and stop.
      *pos += (*remaining - 1);
      *remaining = 1;
      return;
   }

   *pos += written;
   *remaining -= (size_t)written;
}

void ScspDspFullDebugStats(char *outstring, size_t maxlen)
{
   char *pos = outstring;
   size_t remaining = maxlen;
   int i;

   if (!outstring || maxlen == 0)
      return;

   outstring[0] = '\0';

   appendf(&pos, &remaining, "--- Program ---\r\n");
   appendf(&pos, &remaining, "Debug PC   = %02X (next step run by manual Step)\r\n", ScspDspGetPC());
   appendf(&pos, &remaining, "last_step  = %d (active steps run per sample, HW max 128)\r\n", scsp_dsp.last_step);
   appendf(&pos, &remaining, "mdec_ct    = %08X\r\n\r\n", scsp_dsp.mdec_ct);

   appendf(&pos, &remaining, "--- Multiply / ALU pipeline ---\r\n");
   appendf(&pos, &remaining, "INPUTS  = %08X (%d)\r\n", (unsigned int)scsp_dsp.inputs, scsp_dsp.inputs);
   appendf(&pos, &remaining, "B       = %08X (%d)\r\n", (unsigned int)scsp_dsp.b, scsp_dsp.b);
   appendf(&pos, &remaining, "X       = %08X (%d)\r\n", (unsigned int)scsp_dsp.x, scsp_dsp.x);
   appendf(&pos, &remaining, "Y       = %04X (%d)\r\n", (unsigned short)scsp_dsp.y, scsp_dsp.y);
   appendf(&pos, &remaining, "Y_REG   = %08X (%d)\r\n", (unsigned int)scsp_dsp.y_reg, scsp_dsp.y_reg);
   appendf(&pos, &remaining, "MUL_OUT = %08X (%d)\r\n", (unsigned int)scsp_dsp.mul_out, scsp_dsp.mul_out);
   appendf(&pos, &remaining, "ACC     = %08X (%d)\r\n", (unsigned int)scsp_dsp.acc, scsp_dsp.acc);
   appendf(&pos, &remaining, "SHIFTED = %08X (%d)\r\n\r\n", (unsigned int)scsp_dsp.shifted, scsp_dsp.shifted);

   appendf(&pos, &remaining, "--- Ring buffer memory access (MRD/MWT) ---\r\n");
   appendf(&pos, &remaining, "FRC_REG   = %04X\r\n", scsp_dsp.frc_reg);
   appendf(&pos, &remaining, "ADRS_REG  = %04X\r\n", scsp_dsp.adrs_reg);
   appendf(&pos, &remaining, "RBP       = %08X\r\n", (unsigned int)scsp_dsp.rbp);
   appendf(&pos, &remaining, "RBL       = %08X\r\n", (unsigned int)scsp_dsp.rbl);
   appendf(&pos, &remaining, "MRD_VALUE = %08X\r\n", scsp_dsp.mrd_value);
   appendf(&pos, &remaining, "SHIFT_REG = %08X\r\n\r\n", scsp_dsp.shift_reg);

   appendf(&pos, &remaining, "--- Pending I/O (EWA/EWT) ---\r\n");
   appendf(&pos, &remaining, "io_addr       = %08X\r\n", scsp_dsp.io_addr);
   appendf(&pos, &remaining, "need_read     = %d\r\n", scsp_dsp.need_read);
   appendf(&pos, &remaining, "need_write    = %d\r\n", scsp_dsp.need_write);
   appendf(&pos, &remaining, "write_data    = %04X\r\n", scsp_dsp.write_data);
   appendf(&pos, &remaining, "need_nofl     = %d\r\n", scsp_dsp.need_nofl);
   appendf(&pos, &remaining, "read_pending  = %d\r\n", scsp_dsp.read_pending);
   appendf(&pos, &remaining, "write_pending = %d\r\n", scsp_dsp.write_pending);
   appendf(&pos, &remaining, "read_value    = %08X\r\n", scsp_dsp.read_value);
   appendf(&pos, &remaining, "write_value   = %08X\r\n", scsp_dsp.write_value);
   appendf(&pos, &remaining, "updated       = %d\r\n\r\n", scsp_dsp.updated);

   appendf(&pos, &remaining, "--- COEF (64 x 16-bit) ---\r\n");
   for (i = 0; i < 64; i++)
      appendf(&pos, &remaining, "%3d: %04X%s", i, scsp_dsp.coef[i], ((i % 8) == 7) ? "\r\n" : "  ");
   appendf(&pos, &remaining, "\r\n\r\n--- MADRS (32 x 16-bit) ---\r\n");
   for (i = 0; i < 32; i++)
      appendf(&pos, &remaining, "%3d: %04X%s", i, scsp_dsp.madrs[i], ((i % 8) == 7) ? "\r\n" : "  ");

   appendf(&pos, &remaining, "\r\n\r\n--- TEMP (128 x 32-bit) ---\r\n");
   for (i = 0; i < 128; i++)
      appendf(&pos, &remaining, "%3d: %08X%s", i, (unsigned int)scsp_dsp.temp[i], ((i % 4) == 3) ? "\r\n" : "   ");

   appendf(&pos, &remaining, "\r\n\r\n--- MEMS (32 x 32-bit) ---\r\n");
   for (i = 0; i < 32; i++)
      appendf(&pos, &remaining, "%2d: %08X%s", i, (unsigned int)scsp_dsp.mems[i], ((i % 4) == 3) ? "\r\n" : "   ");

   appendf(&pos, &remaining, "\r\n\r\n--- MIXS (16 x 32-bit) ---\r\n");
   for (i = 0; i < 16; i++)
      appendf(&pos, &remaining, "%2d: %08X\r\n", i, (unsigned int)scsp_dsp.mixs[i]);

   appendf(&pos, &remaining, "\r\n--- EFREG (16 x 16-bit) ---\r\n");
   for (i = 0; i < 16; i++)
      appendf(&pos, &remaining, "%2d: %04X (%d)\r\n", i, (unsigned short)scsp_dsp.efreg[i], scsp_dsp.efreg[i]);

   appendf(&pos, &remaining, "\r\n--- EXTS (2 x 16-bit) ---\r\n");
   appendf(&pos, &remaining, "0: %04X (%d)\r\n", (unsigned short)scsp_dsp.exts[0], scsp_dsp.exts[0]);
   appendf(&pos, &remaining, "1: %04X (%d)\r\n", (unsigned short)scsp_dsp.exts[1], scsp_dsp.exts[1]);
}
