/*  Copyright 2006, 2013 Theo Berkau

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

/*! \file netlink.h
    \brief NetLink modem cartridge emulation (16550 UART + Hayes modem,
           the telephone line being emulated by a TCP connection).
*/

#ifndef NETLINK_H
#define NETLINK_H

#include "core.h"
#include "sock.h"
#include "sh2core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Size of the transmit/receive rings (must be a power of two)
#define NETLINK_RING_SIZE       4096
#define NETLINK_CMDLINE_SIZE    256
#define NETLINK_HOST_SIZE       256

// Kept for source compatibility with code using the old name
#define NETLINK_BUFFER_SIZE     NETLINK_RING_SIZE

// Default TCP port (listening and dialing)
#define NETLINK_DEFAULT_PORT    1337

// DreamPi compatible link ("Shiro" protocol of DreamPi's netlink.py,
// https://github.com/Kazade/dreampi): TCP handshake port, UDP port of the
// side waiting for the call and of the calling side.
#define NETLINK_SHIRO_TCP_PORT  65432
#define NETLINK_SHIRO_UDP_WAIT  20001
#define NETLINK_SHIRO_UDP_CALL  20002
#define NETLINK_SHIRO_HISTORY   5     // packets resent in every datagram
#define NETLINK_SHIRO_PAYLOAD   128   // max payload per packet (DreamPi reads 1024 bytes per datagram)

enum NL_RESULTCODE
{
   NL_RESULTCODE_OK=0,
   NL_RESULTCODE_CONNECT,
   NL_RESULTCODE_RING,
   NL_RESULTCODE_NOCARRIER,
   NL_RESULTCODE_ERROR,
   NL_RESULTCODE_CONNECT1200,
   NL_RESULTCODE_NODIALTONE,
   NL_RESULTCODE_BUSY,
   NL_RESULTCODE_NOANSWER,
   NL_RESULTCODE_NONE,          // no result (deferred: dial/answer pending)
};

// Who interprets the bytes written in the THR
enum NL_MODEMSTATE
{
   NL_MODEMSTATE_COMMAND,       // AT commands (off line, or on line after +++)
   NL_MODEMSTATE_DATA,          // bytes go to the remote modem
};

// Emulated telephone line, seen from the Saturn side
enum NL_CALLSTATE
{
   NL_CALL_ONHOOK,              // idle
   NL_CALL_DIALING,             // ATD in progress
   NL_CALL_ANSWERWAIT,          // ATA given, waiting for a caller
   NL_CALL_RINGING,             // incoming call, RING reported
   NL_CALL_ONLINE,              // carrier present
   NL_CALL_SEGANET,             // legacy SegaNet stub (fake login shell)
};

// State of the network thread (TCP side)
enum NL_NETSTATE
{
   NL_NET_IDLE,
   NL_NET_CONNECTING,
   NL_NET_INCOMING,             // a caller is connected, not answered yet
   NL_NET_CONNECTED,
   NL_NET_FAILED,               // dial failed (see net_fail)
   NL_NET_CLOSED,               // connection lost/closed by the peer
};

enum NL_NETREQ
{
   NL_REQ_NONE,
   NL_REQ_DIAL,
   NL_REQ_ANSWER,
   NL_REQ_HANGUP,
   NL_REQ_SHIRO_WAIT,           // DreamPi link, waiting side (dialed 0 / #0xx#)
   NL_REQ_SHIRO_CALL,           // DreamPi link, calling side (dialed #a*b*c*d#)
};

// Kind of link of the current call
enum NL_NETMODE
{
   NL_MODE_TCP,                 // Kronos: raw TCP stream
   NL_MODE_SHIRO,               // DreamPi: UDP packets with sequence numbers
};

enum NL_NETFAIL
{
   NL_FAIL_NONE,
   NL_FAIL_NOANSWER,            // timeout / unreachable / resolve error
   NL_FAIL_BUSY,                // connection refused
};

typedef struct
{
   u8 RBR;
   u8 THR;
   u8 IER;
   u8 DLL;
   u8 DLM;
   u8 IIR;
   u8 FCR;
   u8 LCR;
   u8 MCR;
   u8 LSR;
   u8 MSR;
   u8 SCR;
   u8 SREG[256];
} netlinkregs_struct;

// Single producer / single consumer ring
typedef struct
{
   volatile u32 head;           // written by the producer only
   volatile u32 tail;           // written by the consumer only
   u8 data[NETLINK_RING_SIZE];
} NetlinkRing;

typedef struct {
   netlinkregs_struct reg;

   // Saturn side -------------------------------------------------------
   NetlinkRing rxlocal;         // modem responses/echo (emulation thread only)
   NetlinkRing rxnet;           // network -> Saturn (network thread produces)
   NetlinkRing tx;              // Saturn -> network (network thread consumes)

   char cmdline[NETLINK_CMDLINE_SIZE];
   u32 cmdlen;
   char lastcmd[NETLINK_CMDLINE_SIZE];

   enum NL_MODEMSTATE modemstate;
   enum NL_CALLSTATE callstate;
   int seganet_step;
   int dial_waiting;            // ATD0: waiting for a DreamPi caller

   int isechoenab;              // E
   int quiet;                   // Q
   int verbose;                 // V
   int xlevel;                  // X
   int dtrmode;                 // &D
   int fifo_enabled;
   int thre_pending;
   int irq_asserted;
   u32 irq_raise_time;          // us since the SCU was last requested
   u8 msr_lines;                // current DCD/RI/DSR/CTS (MSR bits 7..4)

   u32 cycles;                  // us since last network poll
   u32 thr_idle_time;           // us since the last THR write
   u32 call_timer;              // us in the current call state
   u32 ring_timer;
   int ring_count;
   int escape_count;
   u32 rxnet_seen;              // rxnet head already signalled

   char defaulthost[NETLINK_HOST_SIZE];
   int defaultport;
   int listenport;

   // Shared with the network thread -------------------------------------
   char dialhost[NETLINK_HOST_SIZE];
   volatile int dialport;
   volatile int dialtimeout_ms;
   volatile int net_req;
   volatile u32 net_req_id;
   volatile u32 net_ack_id;
   volatile int net_state;
   volatile int net_fail;
   volatile int dial_abort;
   volatile int quit;
   volatile int thread_started;

   // Network thread only
   YabSock listensocket;
   YabSock datasocket;
   int net_mode;                // NL_NETMODE of the current call

   // DreamPi link (network thread only)
   YabSock udpsocket;
   YabSock shirolisten;         // TCP 65432 while idle: DreamPi caller -> RING
   int incoming_shiro;          // the incoming call is a DreamPi caller
   int shirolisten_retry_ms;
   int shirolisten_warned;
   unsigned int shiro_peer_ip;
   int shiro_peer_port;
   u32 shiro_txseq;             // sequence number of the next packet sent
   u32 shiro_rxseq;             // sequence number expected next
   int shiro_hist_count;
   int shiro_hist_len[NETLINK_SHIRO_HISTORY];
   u32 shiro_hist_seq[NETLINK_SHIRO_HISTORY];
   u8 shiro_hist[NETLINK_SHIRO_HISTORY][NETLINK_SHIRO_PAYLOAD]; // [0] = newest
   u32 shiro_ping_ms;           // time of the last PING sent (ms)
   u32 shiro_silence_ms;        // time of the last datagram received (ms)
} Netlink;

extern Netlink *NetlinkArea;

// Address/port used to dial and to listen. Must be called before
// NetlinkInit() (CartInit()). NULL or "" keeps the defaults
// (127.0.0.1, 1337).
void NetlinkSetConfig(const char *ip, const char *port);

u8 FASTCALL NetlinkReadByte(SH2_struct *context, u8* memory, u32 addr);
void FASTCALL NetlinkWriteByte(SH2_struct *context, u8* memory, u32 addr, u8 val);

// ip/port: NULL to use the values given to NetlinkSetConfig().
// Returns 0 on success. A network failure (port in use, no socket support)
// is not fatal: the modem still answers AT commands.
int NetlinkInit(const char *ip, const char *port);
void NetlinkDeInit(void);

// timing in microseconds. Does nothing when no NetLink is plugged in.
void NetlinkExec(u32 timing);

#ifdef __cplusplus
}
#endif

#endif
