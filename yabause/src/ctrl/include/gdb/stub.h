/*  GDB remote serial protocol stub for the two SH-2 of the Saturn.

    This file is part of Kronos.

    Kronos is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#ifndef GDBSTUB_H
#define GDBSTUB_H

#include "core.h"
#include "sh2core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Usage (all-stop mode, one TCP connection for both CPUs):
 *
 *    kronos-qt --gdb=43434 -i game.cue        (or KRONOS_GDB_PORT=43434)
 *    gdb-multiarch / sh-elf-gdb:
 *       set architecture sh2
 *       set endian big
 *       target remote 127.0.0.1:43434
 *
 *  Thread 1 = master SH-2, thread 2 = slave SH-2 ("info threads", "thread 2").
 *  The target stops when GDB connects. Everything runs in the emulation
 *  thread: there is no lock, the stub is called once per frame (to accept a
 *  connection and to see Ctrl-C) and from the breakpoint callback of the SH-2
 *  debug interpreter, where it serves GDB until a continue / step.
 *
 *  Needs the SH-2 debug interpreter (breakpoints): GdbStubSelectSH2Core()
 *  forces it when a port is configured.
 */

/* Port (0 = stub off). GdbStubGetPort() falls back on KRONOS_GDB_PORT. */
void GdbStubSetPort(int port);
int  GdbStubGetPort(void);

/* By default only connections from 127.0.0.1 are accepted: the stub can read
 * and write all the memory and registers. */
void GdbStubSetAllowRemote(int allow);

/* Core id to give to SH2Init(): the debug interpreter if the stub is on. */
int  GdbStubSelectSH2Core(int requested);

/* Idle hook, called about every 10 ms while the target is halted and GDB is
 * thinking (the Qt port runs its event loop there). It returns 0 to make the
 * stub give up (application closing): the target then resumes. */
void GdbStubSetIdleHook(int (*hook)(void));

/* After SH2Init(). msh2 only tells that the CPUs exist (both are driven).
 * port <= 0 : use GdbStubGetPort(); if that is 0 too nothing is started.
 * Returns 0 (also when off), -1 if the port cannot be opened. */
int  GdbStubInit(SH2_struct *msh2, int port);
void GdbStubDeInit(void);

/* Once per emulated frame, at a frame boundary. */
void GdbStubFrame(void);

#ifdef __cplusplus
}
#endif

#endif /* GDBSTUB_H */
