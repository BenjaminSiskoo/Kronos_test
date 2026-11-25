/*  SH-2 / bus tracer for Kronos. See trace.h for the file format.

    Design notes
    - One line per retired instruction. An instruction that blocks the CPU
      (bus conflict, see SH2InstructionWillBeReplayed in sh2core.c) is rolled
      back and replayed by ExecSave: its pending line and bus lines are
      dropped by TraceDiscard() so that the replay is logged only once.
    - The instruction line shows the registers BEFORE the opcode executes.
      Bus lines follow their instruction. Accesses made outside an
      instruction (interrupt entry stacking) are appended to the line group
      of the previous instruction.
    - Bus accesses are logged at the SH2MappedMemory* level, i.e. what the CPU
      issues. SCU/DMA transfers (DMAMappedMemory*) are not CPU accesses and
      are not logged.
    - The cycle counter is context->cycles, a CPU-local timestamp: it is
      useful to compare two runs of the same build, tracediff.py can ignore it.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "trace/trace.h"
#include "sh2int_kronos.h"
#include "yabause.h"

volatile u8 SH2TraceActive = 0;
volatile u8 SH2TraceBusActive = 0;

typedef struct {
   char file[512];
   char opts[512];
   int  haveFile;
   int  bus, regs, cycles;
   int  cpuMask;                  /* bit0 master, bit1 slave */
   u32  pcLo, pcHi, addrLo, addrHi;
   int  usePc, useAddr;
   u32  startPc, stopPc;
   int  useStart, useStop;
   u64  skip, max;
} TraceCfg;

static TraceCfg cfg;
static FILE *fp = NULL;
static int armed = 0;             /* start trigger reached (or none) */
static int stopped = 0;
static u64 seen = 0, logged = 0, frames = 0;

#define PENDING_SIZE 8192
static char pending[PENDING_SIZE];
static int  pendingLen = 0;
static int  pendingOn = 0;        /* current instruction passes the filters */

static void cfg_defaults(void)
{
   memset(&cfg, 0, sizeof(cfg));
   cfg.cycles = 1;
   cfg.cpuMask = 3;
}

static int parse_u32(const char *s, u32 *out)
{
   char *end;
   unsigned long v = strtoul(s, &end, 16);
   if (end == s) return 0;
   *out = (u32)v;
   return 1;
}

static void parse_opts(const char *o)
{
   char buf[512], *p;
   strncpy(buf, o, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
   p = buf;
   while (*p)
   {
      char *tok, *eq, *val = NULL;
      while (*p == ',' || *p == ';' || *p == ' ') p++;     /* portable strtok_r */
      if (!*p) break;
      tok = p;
      while (*p && *p != ',' && *p != ';' && *p != ' ') p++;
      if (*p) *p++ = 0;
      eq = strchr(tok, '=');
      if (eq) { *eq = 0; val = eq + 1; }
      if (!strcmp(tok, "bus")) cfg.bus = (val == NULL) ? 1 : atoi(val);
      else if (!strcmp(tok, "regs")) cfg.regs = (val == NULL) ? 1 : atoi(val);
      else if (!strcmp(tok, "cycles")) cfg.cycles = (val == NULL) ? 1 : atoi(val);
      else if (!strcmp(tok, "cpu") && val)
      {
         int m = 0;
         for (; *val; val++) { if (tolower((unsigned char)*val) == 'm') m |= 1; if (tolower((unsigned char)*val) == 's') m |= 2; }
         if (m) cfg.cpuMask = m;
      }
      else if ((!strcmp(tok, "pc") || !strcmp(tok, "addr")) && val)
      {
         u32 lo, hi; char *dash = strchr(val, '-');
         if (parse_u32(val, &lo))
         {
            hi = (dash && parse_u32(dash + 1, &hi)) ? hi : lo;
            if (tok[0] == 'p') { cfg.pcLo = lo; cfg.pcHi = hi; cfg.usePc = 1; }
            else { cfg.addrLo = lo; cfg.addrHi = hi; cfg.useAddr = 1; }
         }
      }
      else if (!strcmp(tok, "start") && val) cfg.useStart = parse_u32(val, &cfg.startPc);
      else if (!strcmp(tok, "stop") && val) cfg.useStop = parse_u32(val, &cfg.stopPc);
      else if (!strcmp(tok, "skip") && val) cfg.skip = strtoull(val, NULL, 0);
      else if (!strcmp(tok, "max") && val) cfg.max = strtoull(val, NULL, 0);
      else fprintf(stderr, "[trace] unknown option '%s'\n", tok);
   }
}

static int cfg_inited = 0;
static void ensure_cfg(void)
{
   if (!cfg_inited) { cfg_defaults(); cfg_inited = 1; }
}

void TraceSetOutput(const char *file, const char *options)
{
   ensure_cfg();                          /* never let a later cfg_defaults() wipe these */
   if (file != NULL)
   {
      if (file[0]) { strncpy(cfg.file, file, sizeof(cfg.file) - 1); cfg.file[sizeof(cfg.file) - 1] = 0; cfg.haveFile = 1; }
      else { cfg.file[0] = 0; cfg.haveFile = 0; }      /* "" = tracing off */
   }
   if (options != NULL) { strncpy(cfg.opts, options, sizeof(cfg.opts) - 1); cfg.opts[sizeof(cfg.opts) - 1] = 0; }
}

int TraceRequested(void)
{
   const char *e = getenv("KRONOS_TRACE_FILE");
   ensure_cfg();
   return cfg.haveFile || (e != NULL && e[0]);
}

int TraceSelectSH2Core(int requested)
{
   return TraceRequested() ? SH2CORE_KRONOS_DEBUG_INTERPRETER : requested;
}

static void flush_pending(void)
{
   if (pendingLen > 0 && fp != NULL)
   {
      fwrite(pending, 1, (size_t)pendingLen, fp);
      logged++;
   }
   pendingLen = 0;
   pendingOn = 0;
}

static void finish(const char *why)
{
   if (fp == NULL) return;
   flush_pending();
   fprintf(fp, "# end: %s, %llu instructions logged of %llu seen, %llu frames\n", why,
           (unsigned long long)logged, (unsigned long long)seen, (unsigned long long)frames);
   fflush(fp);
   SH2TraceActive = 0;
   SH2TraceBusActive = 0;
   stopped = 1;
   fprintf(stderr, "[trace] stopped (%s): %llu instructions logged\n", why, (unsigned long long)logged);
}

int TraceInit(void)
{
   const char *ef, *eo;
   ensure_cfg();
   ef = getenv("KRONOS_TRACE_FILE");
   eo = getenv("KRONOS_TRACE_OPTS");
   if (!cfg.haveFile && ef != NULL && ef[0]) { strncpy(cfg.file, ef, sizeof(cfg.file) - 1); cfg.haveFile = 1; }
   if (cfg.opts[0] == 0 && eo != NULL) strncpy(cfg.opts, eo, sizeof(cfg.opts) - 1);
   if (!cfg.haveFile) return 0;
   if (fp != NULL) return 0;

   {  /* a new run starts from the defaults: options of a previous run must not stay */
      char file[sizeof(cfg.file)], opts[sizeof(cfg.opts)];
      memcpy(file, cfg.file, sizeof(file)); memcpy(opts, cfg.opts, sizeof(opts));
      cfg_defaults();
      memcpy(cfg.file, file, sizeof(file)); memcpy(cfg.opts, opts, sizeof(opts));
      cfg.haveFile = 1;
   }
   parse_opts(cfg.opts);
   fp = fopen(cfg.file, "wb");
   if (fp == NULL) { fprintf(stderr, "[trace] cannot open %s\n", cfg.file); return -1; }
   setvbuf(fp, NULL, _IOFBF, 1 << 20);

   fprintf(fp, "# kronos-sh2-trace v1\n# opts: bus=%d regs=%d cycles=%d cpu=%d\n", cfg.bus, cfg.regs, cfg.cycles, cfg.cpuMask);
   if (cfg.usePc) fprintf(fp, "# pc range %08X-%08X\n", cfg.pcLo, cfg.pcHi);
   if (cfg.useAddr) fprintf(fp, "# addr range %08X-%08X\n", cfg.addrLo, cfg.addrHi);
   if (cfg.useStart) fprintf(fp, "# start at pc %08X\n", cfg.startPc);
   if (cfg.useStop) fprintf(fp, "# stop at pc %08X\n", cfg.stopPc);

   armed = cfg.useStart ? 0 : 1;
   stopped = 0; seen = logged = frames = 0; pendingLen = 0; pendingOn = 0;
   SH2TraceActive = 1;
   SH2TraceBusActive = cfg.bus ? 1 : 0;
   fprintf(stderr, "[trace] writing %s (SH-2 debug interpreter)\n", cfg.file);
   return 0;
}

void TraceDeInit(void)
{
   if (fp == NULL) return;
   if (!stopped) finish("emulator closed");
   fclose(fp);
   fp = NULL;
}

void TraceFrame(void)
{
   if (fp == NULL || stopped) return;
   flush_pending();
   frames++;
   if (armed) fprintf(fp, "# frame %llu\n", (unsigned long long)frames);
}

static INLINE int cpu_ok(SH2_struct *c)
{
   return (cfg.cpuMask & ((c != NULL && c->isslave) ? 2 : 1)) != 0;
}

void TraceInstr(SH2_struct *c)
{
   const u32 pc = c->regs.PC;
   int n;
   if (fp == NULL || stopped) return;
   flush_pending();

   if (!armed)
   {
      if (cfg.useStart && pc == cfg.startPc && cpu_ok(c)) { armed = 1; fprintf(fp, "# triggered at %08X\n", pc); }
      else return;
   }
   if (cfg.useStop && pc == cfg.stopPc && cpu_ok(c)) { finish("stop address reached"); return; }
   if (!cpu_ok(c)) return;
   if (cfg.usePc && (pc < cfg.pcLo || pc > cfg.pcHi)) return;
   if (seen++ < cfg.skip) return;
   if (cfg.max && logged >= cfg.max) { finish("max reached"); return; }

   n = snprintf(pending, PENDING_SIZE, "%c I %08X %04X", c->isslave ? 'S' : 'M', pc, (unsigned)(c->instruction & 0xFFFF));
   if (cfg.cycles) n += snprintf(pending + n, PENDING_SIZE - n, " cyc=%u", (unsigned)c->cycles);
   if (cfg.regs)
   {
      int i;
      for (i = 0; i < 16; i++) n += snprintf(pending + n, PENDING_SIZE - n, " r%d=%08X", i, c->regs.R[i]);
      n += snprintf(pending + n, PENDING_SIZE - n, " sr=%08X pr=%08X gbr=%08X vbr=%08X mach=%08X macl=%08X",
                    c->regs.SR.all, c->regs.PR, c->regs.GBR, c->regs.VBR, c->regs.MACH, c->regs.MACL);
   }
   pending[n++] = '\n';
   pendingLen = n;
   pendingOn = 1;
}

void TraceDiscard(SH2_struct *c)
{
   (void)c;
   if (pendingOn) { pendingLen = 0; pendingOn = 0; seen--; }
}

void TraceBus(SH2_struct *c, int write, int size, u32 addr, u32 value)
{
   int n;
   if (fp == NULL || stopped || !pendingOn || !cpu_ok(c)) return;
   if (cfg.useAddr && (addr < cfg.addrLo || addr > cfg.addrHi)) return;
   if (pendingLen > PENDING_SIZE - 64) return;
   n = snprintf(pending + pendingLen, PENDING_SIZE - pendingLen, "%c %c%d %08X %0*X\n",
                (c == NULL) ? '-' : (c->isslave ? 'S' : 'M'), write ? 'w' : 'r', size, addr, size * 2,
                (size == 4) ? value : (value & ((1u << (size * 8)) - 1)));
   pendingLen += n;
}
