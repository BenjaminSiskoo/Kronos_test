/*  SH-2 / bus tracer for Kronos.

    Writes one text line per retired SH-2 instruction (both CPUs) and,
    optionally, one line per CPU bus access, to a file that can be compared
    with tools/tracediff.py (two builds, before/after a fix, two games states).

    Start:   kronos --trace=trace.txt [--trace-opts=bus,regs,cpu=m,...]
    or env:  KRONOS_TRACE_FILE / KRONOS_TRACE_OPTS
    Needs the SH-2 debug interpreter, which is forced when a trace is requested
    (slower than the normal core). Build option: YAB_SH2TRACE (HAVE_SH2TRACE).

    File format (see doc in trace.c):
      # ...                      comment / header
      # frame N                  emitted once per emulated frame
      M I 06004000 6013 cyc=123456 [r0=... sr=... pr=...]
      M r4 0603F120 0000A5A5     bus read  (r1 r2 r4) : size, address, value
      S w2 25FE0004 0001         bus write (w1 w2 w4)
    CPU is M (master), S (slave) or - (access without a CPU context).
*/
#ifndef KRONOS_TRACE_H
#define KRONOS_TRACE_H

#include "core.h"
#include "sh2core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set the output file and the option string ("bus,regs,cycles=0,cpu=m|s|ms,
   pc=LO-HI,addr=LO-HI,start=PC,stop=PC,skip=N,max=N"). NULL keeps the
   environment / previous values. Must be called before YabauseInit. */
void TraceSetOutput(const char *file, const char *options);
int  TraceRequested(void);

/* Maps the requested SH-2 core to the debug interpreter when tracing. */
int  TraceSelectSH2Core(int requested);

/* Called by YabauseInit / YabauseDeInit / once per frame. */
int  TraceInit(void);
void TraceDeInit(void);
void TraceFrame(void);

/* Hooks. The two flags are tested by the callers before the call. */
extern volatile u8 SH2TraceActive;       /* instruction hook armed */
extern volatile u8 SH2TraceBusActive;    /* bus hook armed (option "bus") */

void TraceInstr(SH2_struct *context);                    /* before the opcode runs */
void TraceDiscard(SH2_struct *context);                  /* instruction rolled back, will be replayed */
void TraceBus(SH2_struct *context, int write, int size, u32 addr, u32 value);

#ifdef __cplusplus
}
#endif

#endif
