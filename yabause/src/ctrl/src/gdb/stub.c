/*  GDB remote serial protocol stub for the two SH-2 of the Saturn.

    This file is part of Kronos.

    Kronos is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/* Design
 *
 *  - All-stop protocol, ONE connection for both CPUs: thread 1 = master
 *    SH-2 (MSH2), thread 2 = slave SH-2 (SSH2).
 *  - No thread, no lock: everything runs in the emulation thread.
 *      GdbStubFrame()   once per frame, at a frame boundary: accepts a
 *                       connection (the target halts there) and looks for
 *                       Ctrl-C (0x03) while the target runs.
 *      bp_callback()    called by the SH-2 debug interpreter (sh2core.c,
 *                       SH2StandardExecDebug / SH2BlockableExecDebug) when a
 *                       code or memory breakpoint hit: the stub serves GDB
 *                       there, until a continue.
 *    While halted the stub calls the idle hook (the Qt port runs its event
 *    loop there; YabauseGL drops nested frame requests, see
 *    YabauseGL::event()).
 *  - Breakpoints are the ones of the SH-2 debug interpreter, not opcodes
 *    written in memory: Z0/Z1 = code breakpoint (set for the address and its
 *    cache-through mirror), Z2/Z3/Z4 = memory watch on the start address,
 *    whatever the access width.
 *  - Memory is accessed with DMAMappedMemory*(): no cycle, no cache fill, no
 *    memory breakpoint triggered (same as the hex editor).
 *  - Register order of GDB for SH (gdb/sh-tdep.h): r0-r15, pc, pr, gbr, vbr,
 *    mach, macl, sr, big endian. A shorter 'g' reply is accepted by GDB: the
 *    registers it does not contain are "not in the g packet".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "sh2core.h"
#include "sh2int_kronos.h"     /* SH2CORE_KRONOS_DEBUG_INTERPRETER */
#include "memory.h"
#include "scsp.h"
#include "yabause.h"
#include "sock.h"
#include "gdb/stub.h"

#define PKT_MAX      0x4000      /* "PacketSize=3fff" */
#define MEM_CHUNK    0x1800      /* bytes of memory per 'm' reply */
#define NREGS        23          /* r0-r15 pc pr gbr vbr mach macl sr */
#define MAX_GDB_BP   64

#define ACT_NONE     0
#define ACT_RESUME   1
#define ACT_DETACH   2

typedef struct
{
   int used;
   int type;          /* 0..4 : Z0 .. Z4 */
   u32 addr;          /* as sent by GDB */
} gdbbp_struct;

static struct
{
   int inited;
   int port;
   int allowRemote;
   YabSock listenSock;
   YabSock client;
   int noack;
   int (*idle)(void);

   SH2_struct *cpu[2];
   int curG;                  /* Hg : thread for registers / memory */
   int curC;                  /* Hc : thread for step */
   int halted;
   char stop[200];            /* last stop reply */

   int inStep;
   int stepHit;
   breakpoint_userdata stepUd;

   gdbbp_struct bp[MAX_GDB_BP];
} G = { 0, 0, 0, YAB_SOCK_INVALID, YAB_SOCK_INVALID, 0, NULL };

static int cfgPort = 0;
static int cfgAllowRemote = 0;

static unsigned char rxbuf[4096];
static int rxlen = 0, rxpos = 0;

//////////////////////////////////////////////////////////////////////////////
// Configuration

void GdbStubSetPort(int port) { cfgPort = port; }

int GdbStubGetPort(void)
{
   if (cfgPort > 0)
      return cfgPort;
   {
      const char *e = getenv("KRONOS_GDB_PORT");
      if (e != NULL && atoi(e) > 0 && atoi(e) < 65536)
         return atoi(e);
   }
   return 0;
}

void GdbStubSetAllowRemote(int allow) { cfgAllowRemote = allow; }

void GdbStubSetIdleHook(int (*hook)(void)) { G.idle = hook; }

int GdbStubSelectSH2Core(int requested)
{
   if (GdbStubGetPort() > 0)
      return SH2CORE_KRONOS_DEBUG_INTERPRETER;
   return requested;
}

//////////////////////////////////////////////////////////////////////////////
// Hex helpers

static int hexval(int c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

/* Hex number at *p (at least 1 digit). Returns 0 and advances *p, -1 if none. */
static int parse_hex(const char **p, u32 *val)
{
   u32 v = 0;
   int n = 0;
   while (hexval(**p) >= 0)
   {
      v = (v << 4) | (u32)hexval(**p);
      (*p)++;
      n++;
   }
   if (n == 0) return -1;
   *val = v;
   return 0;
}

static void put_hex_byte(char *out, u8 b)
{
   static const char d[] = "0123456789abcdef";
   out[0] = d[b >> 4];
   out[1] = d[b & 15];
}

static void hex_encode_text(const char *text, char *out)
{
   while (*text)
   {
      put_hex_byte(out, (u8)*text++);
      out += 2;
   }
   *out = 0;
}

//////////////////////////////////////////////////////////////////////////////
// Socket I/O

static void rx_reset(void) { rxlen = rxpos = 0; }

/* 1 = a byte is available, 0 = timeout, -1 = connection closed / error. */
static int rx_fill(int timeout_ms)
{
   int readable = 1, r, n;

   if (rxpos < rxlen)
      return 1;
   if (G.client == YAB_SOCK_INVALID)
      return -1;

   r = YabSockPoll(G.client, timeout_ms, &readable, NULL);
   if (r < 0)
      return -1;
   if (r == 0 || !readable)
      return 0;

   n = YabSockReceive(G.client, rxbuf, (int)sizeof(rxbuf), 0);
   if (n == 0)
      return -1;
   if (n < 0)
      return YabSockWouldBlock() ? 0 : -1;
   rxpos = 0;
   rxlen = n;
   return 1;
}

static int send_all(const void *buf, int len)
{
   const char *p = (const char *)buf;
   while (len > 0)
   {
      int n = YabSockSend(G.client, p, len, 0);
      if (n <= 0)
      {
         if (n < 0 && YabSockWouldBlock())
         {
            int w = 1;
            YabSockPoll(G.client, 100, NULL, &w);
            continue;
         }
         return -1;
      }
      p += n;
      len -= n;
   }
   return 0;
}

/* Packet "$data#xx". Returns 1 = packet (payload in buf), 2 = Ctrl-C,
   0 = nothing within timeout_ms, -1 = connection lost. */
static int read_packet(char *buf, int max, int timeout_ms)
{
   for (;;)
   {
      int c, n = 0, sum = 0, r, h1, h2, ok;

      for (;;)
      {
         r = rx_fill(timeout_ms);
         if (r <= 0) return r;
         c = rxbuf[rxpos++];
         if (c == 0x03) return 2;
         if (c == '$') break;
         /* '+' / '-' of an acknowledgement, noise: ignored */
      }

      for (;;)
      {
         r = rx_fill(2000);
         if (r <= 0) return -1;
         c = rxbuf[rxpos++];
         if (c == '#') break;
         if (c == '$') { n = 0; sum = 0; continue; }   /* restart */
         sum += c;
         if (c == '}')                                  /* escaped byte */
         {
            r = rx_fill(2000);
            if (r <= 0) return -1;
            c = rxbuf[rxpos++];
            sum += c;
            c ^= 0x20;
         }
         if (n < max - 1) buf[n++] = (char)c;
      }
      buf[n] = 0;

      r = rx_fill(2000);
      if (r <= 0) return -1;
      h1 = hexval(rxbuf[rxpos++]);
      r = rx_fill(2000);
      if (r <= 0) return -1;
      h2 = hexval(rxbuf[rxpos++]);

      ok = (h1 >= 0 && h2 >= 0 && (((h1 << 4) | h2) == (sum & 0xFF)));
      if (G.noack)
         return 1;
      if (send_all(ok ? "+" : "-", 1) < 0)
         return -1;
      if (ok)
         return 1;
      /* bad checksum : GDB resends */
   }
}

static int send_packet(const char *data)
{
   static char out[PKT_MAX + 16];
   int len = (int)strlen(data), i, sum = 0, tries;

   if (len > PKT_MAX) len = PKT_MAX;
   out[0] = '$';
   for (i = 0; i < len; i++)
   {
      out[1 + i] = data[i];
      sum += (unsigned char)data[i];
   }
   out[1 + len] = '#';
   put_hex_byte(out + 2 + len, (u8)sum);
   out[4 + len] = 0;

   for (tries = 0; tries < 5; tries++)
   {
      if (send_all(out, len + 4) < 0)
         return -1;
      if (G.noack)
         return 0;
      for (;;)
      {
         int r = rx_fill(2000), c;
         if (r <= 0) return -1;
         c = rxbuf[rxpos++];
         if (c == '+') return 0;
         if (c == '-') break;          /* resend */
         /* anything else (Ctrl-C ...) while halted : ignored */
      }
   }
   return -1;
}

//////////////////////////////////////////////////////////////////////////////
// CPU access

static int cpu_of_thread(u32 tid)
{
   if (tid == 1) return 0;
   if (tid == 2) return 1;
   return -1;
}

static void get_regs(SH2_struct *c, u32 v[NREGS])
{
   sh2regs_struct r;
   int i;
   memset(&r, 0, sizeof(r));
   SH2GetRegisters(c, &r);
   for (i = 0; i < 16; i++) v[i] = r.R[i];
   v[16] = r.PC;
   v[17] = r.PR;
   v[18] = r.GBR;
   v[19] = r.VBR;
   v[20] = r.MACH;
   v[21] = r.MACL;
   v[22] = r.SR.all;
}

static void set_reg(SH2_struct *c, int n, u32 val)
{
   sh2regs_struct r;
   memset(&r, 0, sizeof(r));
   SH2GetRegisters(c, &r);
   if (n < 16) r.R[n] = val;
   else switch (n)
   {
      case 16: r.PC = val; break;
      case 17: r.PR = val; break;
      case 18: r.GBR = val; break;
      case 19: r.VBR = val; break;
      case 20: r.MACH = val; break;
      case 21: r.MACL = val; break;
      case 22: r.SR.all = val; break;
      default: return;
   }
   SH2SetRegisters(c, &r);
}

static u32 get_pc(SH2_struct *c)
{
   sh2regs_struct r;
   memset(&r, 0, sizeof(r));
   SH2GetRegisters(c, &r);
   return r.PC;
}

static int cpu_runs(int cpu)
{
   return (cpu == 0) || (yabsys.IsSSH2Running != 0);
}

//////////////////////////////////////////////////////////////////////////////
// Breakpoints

static u32 code_alias(u32 addr) { return addr & 0x1FFFFFFF; }     /* cached mirror */
static u32 mem_alias(u32 addr)  { return addr & 0x0FFFFFFF; }

static int code_bp_at(SH2_struct *c, u32 pc)
{
   int i;
   for (i = 0; i < c->bp.numcodebreakpoints; i++)
      if (c->bp.codebreakpoint[i].addr == pc)
         return 1;
   return 0;
}

static int code_in_use(u32 canon)
{
   int i;
   for (i = 0; i < MAX_GDB_BP; i++)
      if (G.bp[i].used && G.bp[i].type <= 1 && code_alias(G.bp[i].addr) == canon)
         return 1;
   return 0;
}

/* Sets / removes the code breakpoint (address and cache-through mirror) on
   both CPUs. Returns 0 if, afterwards, the state is the wanted one on every
   CPU (SH2AddCodeBreakpoint() fails both when the breakpoint already exists
   and when the list is full : the list is looked at). */
static int code_apply(u32 canon, int add)
{
   int cpu, v, ret = 0;
   for (cpu = 0; cpu < 2; cpu++)
      for (v = 0; v < 2; v++)
      {
         u32 a = v ? (canon | 0x20000000) : canon;
         if (add)
            SH2AddCodeBreakpoint(G.cpu[cpu], a);
         else
            SH2DelCodeBreakpoint(G.cpu[cpu], a);
         if (code_bp_at(G.cpu[cpu], a) != add)
            ret = -1;
      }
   return ret;
}

/* Memory watch flags wanted at address m (union of all GDB entries). */
static u32 mem_flags_wanted(u32 m)
{
   u32 f = 0;
   int i;
   for (i = 0; i < MAX_GDB_BP; i++)
   {
      if (!G.bp[i].used || G.bp[i].type < 2 || mem_alias(G.bp[i].addr) != m)
         continue;
      if (G.bp[i].type == 2 || G.bp[i].type == 4)
         f |= BREAK_BYTEWRITE | BREAK_WORDWRITE | BREAK_LONGWRITE;
      if (G.bp[i].type == 3 || G.bp[i].type == 4)
         f |= BREAK_BYTEREAD | BREAK_WORDREAD | BREAK_LONGREAD;
   }
   return f;
}

/* Returns 0, -1 if the SH-2 core has no room / refuses the address. */
static int mem_sync(u32 m)
{
   u32 f = mem_flags_wanted(m);
   int cpu, ret = 0;
   for (cpu = 0; cpu < 2; cpu++)
      SH2DelMemoryBreakpoint(G.cpu[cpu], m);
   if (f != 0)
      for (cpu = 0; cpu < 2; cpu++)
         if (SH2AddMemoryBreakpoint(G.cpu[cpu], m, f) != 0)
            ret = -1;
   return ret;
}

static int bp_set(int type, u32 addr, int add)
{
   int i, slot = -1;

   for (i = 0; i < MAX_GDB_BP; i++)
   {
      if (G.bp[i].used && G.bp[i].type == type && G.bp[i].addr == addr)
      {
         if (add) return 0;          /* already set */
         slot = i;
         break;
      }
      if (!G.bp[i].used && slot < 0) slot = i;
   }

   if (add)
   {
      if (slot < 0) return -1;
      G.bp[slot].used = 1;
      G.bp[slot].type = type;
      G.bp[slot].addr = addr;
      if (type <= 1)
      {
         if (code_apply(code_alias(addr), 1) != 0)
         {
            /* list full : give the entry back (and the SH-2 slots if no
               other GDB entry needs them) */
            G.bp[slot].used = 0;
            if (!code_in_use(code_alias(addr)))
               code_apply(code_alias(addr), 0);
            return -1;
         }
         return 0;
      }
      if (mem_sync(mem_alias(addr)) != 0)
      {
         G.bp[slot].used = 0;
         mem_sync(mem_alias(addr));
         return -1;
      }
      return 0;
   }

   if (slot < 0 || !G.bp[slot].used || G.bp[slot].type != type || G.bp[slot].addr != addr)
      return 0;                      /* removing something unknown : fine */
   G.bp[slot].used = 0;
   if (type <= 1)
   {
      if (!code_in_use(code_alias(addr)))
         code_apply(code_alias(addr), 0);
   }
   else
      mem_sync(mem_alias(addr));
   return 0;
}

static void bp_clear_all(void)
{
   int i;
   if (G.cpu[0] == NULL || G.cpu[1] == NULL)
      return;
   for (i = 0; i < 2; i++)
   {
      SH2ClearCodeBreakpoints(G.cpu[i]);
      SH2ClearMemoryBreakpoints(G.cpu[i]);
   }
   memset(G.bp, 0, sizeof(G.bp));
}

/* Stop reply for a breakpoint / watch reported by the SH-2 core. */
static void make_stop_from(int cpu, const breakpoint_userdata *ud)
{
   char extra[64];
   int i;

   extra[0] = 0;
   if (ud != NULL)
   {
      if (ud->PCAddress == ud->BPAddress)
      {
         int type = 0;
         for (i = 0; i < MAX_GDB_BP; i++)
            if (G.bp[i].used && G.bp[i].type <= 1 &&
                code_alias(G.bp[i].addr) == code_alias(ud->BPAddress))
            { type = G.bp[i].type; break; }
         snprintf(extra, sizeof(extra), "%s:;", type == 1 ? "hwbreak" : "swbreak");
      }
      else
      {
         for (i = 0; i < MAX_GDB_BP; i++)
            if (G.bp[i].used && G.bp[i].type >= 2 &&
                mem_alias(G.bp[i].addr) == mem_alias(ud->BPAddress))
            {
               static const char *const kind[3] = { "watch", "rwatch", "awatch" };
               snprintf(extra, sizeof(extra), "%s:%x;", kind[G.bp[i].type - 2], G.bp[i].addr);
               break;
            }
      }
   }
   snprintf(G.stop, sizeof(G.stop), "T05thread:%x;%s", cpu + 1, extra);
}

//////////////////////////////////////////////////////////////////////////////
// Execution control

/* Executes one instruction of cpu, ignoring the code breakpoint(s) at its PC
   (the one it stands on, otherwise the same breakpoint would fire again at
   once). Returns 1 if a memory watch or another breakpoint fired during the
   step (G.stop is then filled). */
static int do_step(int cpu)
{
   SH2_struct *c = G.cpu[cpu];
   u32 pc = get_pc(c);
   u32 removed[2];
   int nrem = 0, i;

   for (i = 0; i < 2; i++)
   {
      u32 a = (i == 0) ? pc : (pc | 0x20000000);
      if (code_bp_at(c, a) && SH2DelCodeBreakpoint(c, a) == 0)
         removed[nrem++] = a;
   }

   G.inStep = 1;
   G.stepHit = 0;
   c->bp.inbreakpoint = 0;
   SH2Step(c);
   c->bp.inbreakpoint = 0;
   G.inStep = 0;

   for (i = 0; i < nrem; i++)
      SH2AddCodeBreakpoint(c, removed[i]);

   if (G.stepHit)
   {
      make_stop_from(cpu, &G.stepUd);
      return 1;
   }
   return 0;
}

/* Before letting the emulation run: a CPU standing on a code breakpoint must
   first step over it. Returns 1 if that step already produced a stop. */
static int resume_prepare(void)
{
   int cpu;
   for (cpu = 0; cpu < 2; cpu++)
   {
      if (!cpu_runs(cpu))
         continue;
      if (code_bp_at(G.cpu[cpu], get_pc(G.cpu[cpu])))
         if (do_step(cpu))
            return 1;
   }
   return 0;
}

//////////////////////////////////////////////////////////////////////////////
// Packets

static void drop_client(void)
{
   if (G.client != YAB_SOCK_INVALID)
   {
      YabSockCloseSocket(G.client);
      G.client = YAB_SOCK_INVALID;
      fprintf(stderr, "[gdbstub] debugger disconnected\n");
   }
   G.noack = 0;
   rx_reset();
   bp_clear_all();
}

static void reply_regs(char *out, int cpu)
{
   u32 v[NREGS];
   int i;
   get_regs(G.cpu[cpu], v);
   for (i = 0; i < NREGS; i++)
      sprintf(out + i * 8, "%08x", v[i]);
}

static int cmd_memory_read(const char *p, char *out)
{
   u32 addr, len, i;
   if (parse_hex(&p, &addr) != 0 || *p++ != ',' || parse_hex(&p, &len) != 0)
      return -1;
   if (len > MEM_CHUNK) len = MEM_CHUNK;
   for (i = 0; i < len; i++)
      put_hex_byte(out + 2 * i, DMAMappedMemoryReadByte(addr + i));
   out[2 * len] = 0;
   return 0;
}

static int cmd_memory_write(const char *p)
{
   u32 addr, len, i;
   if (parse_hex(&p, &addr) != 0 || *p++ != ',' || parse_hex(&p, &len) != 0 || *p++ != ':')
      return -1;
   for (i = 0; i < len; i++)
   {
      int h = hexval(p[2 * i]), l = (h >= 0) ? hexval(p[2 * i + 1]) : -1;
      if (h < 0 || l < 0) return -1;
      DMAMappedMemoryWriteByte(addr + i, (u8)((h << 4) | l));
   }
   return 0;
}

static void cmd_monitor(const char *hex, char *out)
{
   char text[128];
   char reply[512];
   int n = 0;

   while (hexval(hex[0]) >= 0 && hexval(hex[1]) >= 0 && n < (int)sizeof(text) - 1)
   {
      text[n++] = (char)((hexval(hex[0]) << 4) | hexval(hex[1]));
      hex += 2;
   }
   text[n] = 0;

   if (strcmp(text, "cpus") == 0)
   {
      u32 m[NREGS], s[NREGS];
      get_regs(G.cpu[0], m);
      get_regs(G.cpu[1], s);
      snprintf(reply, sizeof(reply),
               "MSH2 pc=%08x pr=%08x sr=%08x cycles=%u\nSSH2 pc=%08x pr=%08x sr=%08x cycles=%u%s\n",
               m[16], m[17], m[22], (unsigned)G.cpu[0]->cycles,
               s[16], s[17], s[22], (unsigned)G.cpu[1]->cycles,
               yabsys.IsSSH2Running ? "" : " (not running)");
   }
   else
      snprintf(reply, sizeof(reply),
               "Kronos GDB stub. monitor commands:\n"
               "  cpus    PC / PR / SR / cycle counter of both SH-2\n"
               "Threads: 1 = master SH-2, 2 = slave SH-2 (thread N).\n"
               "Z2/Z3/Z4 watch the start address only, for any access width.\n");
   hex_encode_text(reply, out);
}

static int handle(char *pkt)
{
   static char out[PKT_MAX + 16];
   const char *p = pkt + 1;
   u32 a, b;
   int cpu;

   out[0] = 0;

   switch (pkt[0])
   {
      case '?':
         return send_packet(G.stop) < 0 ? ACT_DETACH : ACT_NONE;

      case 'g':
         reply_regs(out, G.curG);
         break;

      case 'G':
      {
         int i;
         u32 v;
         for (i = 0; i < NREGS; i++)
         {
            int j;
            v = 0;
            for (j = 0; j < 8; j++)
            {
               int h = hexval(p[i * 8 + j]);
               if (h < 0) { strcpy(out, "E01"); goto done; }
               v = (v << 4) | (u32)h;
            }
            set_reg(G.cpu[G.curG], i, v);
         }
         strcpy(out, "OK");
         break;
      }

      case 'p':
         if (parse_hex(&p, &a) != 0) { strcpy(out, "E01"); break; }
         if (a < NREGS)
         {
            u32 v[NREGS];
            get_regs(G.cpu[G.curG], v);
            sprintf(out, "%08x", v[a]);
         }
         else if (a <= 24)
            strcpy(out, "xxxxxxxx");       /* fpul / fpscr : none on SH-2 */
         else
            strcpy(out, "E01");
         break;

      case 'P':
         if (parse_hex(&p, &a) != 0 || *p++ != '=' || parse_hex(&p, &b) != 0 || a >= NREGS)
            strcpy(out, "E01");
         else
         {
            set_reg(G.cpu[G.curG], (int)a, b);
            strcpy(out, "OK");
         }
         break;

      case 'm':
         if (cmd_memory_read(p, out) != 0) strcpy(out, "E01");
         break;

      case 'M':
         strcpy(out, cmd_memory_write(p) == 0 ? "OK" : "E01");
         break;

      case 'c':
         if (parse_hex(&p, &a) == 0)
            set_reg(G.cpu[G.curC], 16, a);
         if (resume_prepare())
         {
            /* the step over the breakpoint already stopped on something */
            return send_packet(G.stop) < 0 ? ACT_DETACH : ACT_NONE;
         }
         return ACT_RESUME;

      case 's':
         if (parse_hex(&p, &a) == 0)
            set_reg(G.cpu[G.curC], 16, a);
         if (!cpu_runs(G.curC))
         {
            strcpy(out, "E02");
            break;
         }
         if (!do_step(G.curC))
            snprintf(G.stop, sizeof(G.stop), "T05thread:%x;", G.curC + 1);
         return send_packet(G.stop) < 0 ? ACT_DETACH : ACT_NONE;

      case 'Z':
      case 'z':
      {
         int type = pkt[1] - '0';
         p = pkt + 2;
         if (type < 0 || type > 4 || *p++ != ',' || parse_hex(&p, &a) != 0)
            { strcpy(out, "E01"); break; }
         if (bp_set(type, a, pkt[0] == 'Z') != 0)
            strcpy(out, "E01");
         else
            strcpy(out, "OK");
         break;
      }

      case 'H':
      {
         int op = pkt[1];
         const char *t = pkt + 2;
         if (*t == '-') { strcpy(out, "OK"); break; }       /* all threads */
         if (parse_hex(&t, &a) != 0) { strcpy(out, "E01"); break; }
         if (a == 0) { strcpy(out, "OK"); break; }          /* any */
         cpu = cpu_of_thread(a);
         if (cpu < 0) { strcpy(out, "E01"); break; }
         if (op == 'g') G.curG = cpu;
         else if (op == 'c') G.curC = cpu;
         strcpy(out, "OK");
         break;
      }

      case 'T':
         if (parse_hex(&p, &a) == 0 && cpu_of_thread(a) >= 0) strcpy(out, "OK");
         else strcpy(out, "E01");
         break;

      case 'D':
         send_packet("OK");
         return ACT_DETACH;

      case 'k':
         return ACT_DETACH;

      case 'q':
         if (strncmp(pkt, "qSupported", 10) == 0)
            strcpy(out, "PacketSize=3fff;QStartNoAckMode+;swbreak+;hwbreak+");
         else if (strcmp(pkt, "qC") == 0)
            sprintf(out, "QC%x", G.curG + 1);
         else if (strcmp(pkt, "qfThreadInfo") == 0)
            strcpy(out, "m1,2");
         else if (strcmp(pkt, "qsThreadInfo") == 0)
            strcpy(out, "l");
         else if (strcmp(pkt, "qAttached") == 0)
            strcpy(out, "1");
         else if (strcmp(pkt, "qOffsets") == 0)
            strcpy(out, "Text=0;Data=0;Bss=0");
         else if (strncmp(pkt, "qSymbol", 7) == 0)
            strcpy(out, "OK");
         else if (strncmp(pkt, "qThreadExtraInfo,", 17) == 0)
         {
            const char *t = pkt + 17;
            if (parse_hex(&t, &a) == 0 && cpu_of_thread(a) >= 0)
               hex_encode_text(a == 1 ? "Master SH-2" : "Slave SH-2", out);
         }
         else if (strncmp(pkt, "qRcmd,", 6) == 0)
            cmd_monitor(pkt + 6, out);
         /* any other query : empty reply = not supported */
         break;

      case 'Q':
         if (strcmp(pkt, "QStartNoAckMode") == 0)
         {
            if (send_packet("OK") < 0)
               return ACT_DETACH;
            G.noack = 1;
            return ACT_NONE;
         }
         break;

      default:
         /* v..., X, R, ... : empty reply = not supported (GDB falls back on
            c / s / M) */
         break;
   }

done:
   return send_packet(out) < 0 ? ACT_DETACH : ACT_NONE;
}

/* Serves GDB while the target is halted, until a continue or a detach. */
static void serve(int cpu)
{
   char pkt[PKT_MAX];

   G.halted = 1;
   G.curG = G.curC = cpu;
   ScspMuteAudio(SCSP_MUTE_SYSTEM);

   if (send_packet(G.stop) < 0)
      drop_client();

   while (G.client != YAB_SOCK_INVALID)
   {
      int r, act;

      if (G.idle != NULL && !G.idle())
      {
         drop_client();            /* application closing */
         break;
      }

      r = read_packet(pkt, (int)sizeof(pkt), 10);
      if (r == 0 || r == 2)
         continue;
      if (r < 0)
      {
         drop_client();
         break;
      }

      act = handle(pkt);
      if (act == ACT_RESUME)
         break;
      if (act == ACT_DETACH)
      {
         drop_client();
         break;
      }
   }

   ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
   G.halted = 0;
}

//////////////////////////////////////////////////////////////////////////////
// Hooks

/* Called by sh2core.c when a breakpoint of the debug interpreter fired. */
static void bp_callback(void *ctx, u32 addr, void *userdata)
{
   SH2_struct *c = (SH2_struct *)ctx;
   int cpu = (c == G.cpu[1]) ? 1 : 0;
   (void)addr;

   if (G.inStep)
   {
      /* a step of the stub : only remember what fired */
      G.stepHit = 1;
      if (userdata != NULL)
         G.stepUd = *(breakpoint_userdata *)userdata;
      return;
   }
   if (G.client == YAB_SOCK_INVALID || G.halted)
      return;

   make_stop_from(cpu, (const breakpoint_userdata *)userdata);
   serve(cpu);
}

int GdbStubInit(SH2_struct *msh2, int port)
{
   (void)msh2;

   if (G.inited)
      GdbStubDeInit();

   if (port <= 0)
      port = GdbStubGetPort();
   if (port <= 0)
      return 0;

   if (MSH2 == NULL || SSH2 == NULL)
      return -1;

   if (YabSockInit() != 0)
   {
      fprintf(stderr, "[gdbstub] sockets not available\n");
      return -1;
   }
   G.listenSock = YAB_SOCK_INVALID;
   if (YabSockListenSocket(port, &G.listenSock) != 0 || G.listenSock == YAB_SOCK_INVALID)
   {
      fprintf(stderr, "[gdbstub] cannot listen on port %d\n", port);
      YabSockDeInit();
      return -1;
   }

   G.allowRemote = cfgAllowRemote || (getenv("KRONOS_GDB_REMOTE") != NULL);
   G.port = port;
   G.cpu[0] = MSH2;
   G.cpu[1] = SSH2;
   G.client = YAB_SOCK_INVALID;
   G.noack = 0;
   G.halted = 0;
   G.inStep = 0;
   G.curG = G.curC = 0;
   memset(G.bp, 0, sizeof(G.bp));
   strcpy(G.stop, "T05thread:1;");

   SH2SetBreakpointCallBack(MSH2, bp_callback, NULL);
   SH2SetBreakpointCallBack(SSH2, bp_callback, NULL);

   G.inited = 1;
   fprintf(stderr, "[gdbstub] waiting for GDB on port %d (%s). "
                   "gdb: set architecture sh2 / set endian big / target remote :%d\n",
           port, G.allowRemote ? "any address" : "127.0.0.1 only", port);
   return 0;
}

void GdbStubDeInit(void)
{
   if (!G.inited)
      return;
   drop_client();
   if (G.cpu[0] != NULL) SH2SetBreakpointCallBack(G.cpu[0], NULL, NULL);
   if (G.cpu[1] != NULL) SH2SetBreakpointCallBack(G.cpu[1], NULL, NULL);
   if (G.listenSock != YAB_SOCK_INVALID)
      YabSockCloseSocket(G.listenSock);
   G.listenSock = YAB_SOCK_INVALID;
   YabSockDeInit();
   G.inited = 0;
}

void GdbStubFrame(void)
{
   int readable;

   if (!G.inited || G.halted)
      return;

   if (G.client == YAB_SOCK_INVALID)
   {
      YabSock s;
      unsigned int ip = 0;

      readable = 1;
      if (YabSockPoll(G.listenSock, 0, &readable, NULL) <= 0 || !readable)
         return;
      s = YabSockAccept(G.listenSock);
      if (s == YAB_SOCK_INVALID)
         return;
      if (!G.allowRemote && (YabSockPeer4(s, &ip) != 0 || ip != 0x7F000001u))
      {
         fprintf(stderr, "[gdbstub] connection refused (not from 127.0.0.1, see --gdb-remote)\n");
         YabSockCloseSocket(s);
         return;
      }
      YabSockSetNoDelay(s);
      G.client = s;
      G.noack = 0;
      rx_reset();
      fprintf(stderr, "[gdbstub] debugger connected, target halted\n");
      strcpy(G.stop, "T05thread:1;");
      serve(0);
      return;
   }

   /* Running: only Ctrl-C (0x03) is expected from GDB. */
   readable = 1;
   if (YabSockPoll(G.client, 0, &readable, NULL) > 0 && readable)
   {
      int n = YabSockReceive(G.client, rxbuf, (int)sizeof(rxbuf), 0), i, ctrlc = 0;
      if (n == 0 || (n < 0 && !YabSockWouldBlock()))
      {
         drop_client();
         return;
      }
      if (n < 0)
         return;
      rxpos = 0;
      rxlen = n;
      for (i = 0; i < rxlen; i++)
         if (rxbuf[i] == 0x03)
         {
            memmove(rxbuf + i, rxbuf + i + 1, (size_t)(rxlen - i - 1));
            rxlen--;
            ctrlc = 1;
            break;
         }
      if (ctrlc)
      {
         strcpy(G.stop, "T02thread:1;");      /* SIGINT */
         serve(0);
      }
      else
         rx_reset();
   }
}
