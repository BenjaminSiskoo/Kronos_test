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

/*! \file netlink.c
    \brief NetLink modem cartridge emulation.

    The NetLink is seen by the SH-2 as a 16550 compatible UART mapped in the
    A-Bus CS2 area (one register every 4 bytes, odd addresses, from
    0x25895001), the modem behind it understanding Hayes AT commands. The
    telephone line is emulated with a TCP connection:

      - "dialing" opens a TCP connection to the peer,
      - an incoming TCP connection on the listening port is an incoming call
        (RING, answered with ATA or automatically with ATS0=n),
      - closing the connection is hanging up (NO CARRIER on the other side).
    Like on a telephone line, the caller only gets CONNECT once the called
    side has answered: the answering Kronos sends NL_ANSWER_BANNER first,
    which the caller removes from the stream. Before, the caller got
    CONNECT as soon as TCP connected, started its handshake while the other
    side was still ringing and gave up (Daytona USA CCE NetLink Edition:
    "Opponent not answering. Dialing again...").

    Dial strings understood by ATD:
      - legacy Kronos/Yabause form:  *192,168,0,2*1337
      - dotted address or host name: 192.168.0.2   example.org:1337
      - 12 digits = IPv4 address, 3 digits per byte, optionally followed by
        the port:                    192168000002   1921680000021337
      - any other number dials the address/port set in the settings
        (Cartridge/ModemIP and Cartridge/ModemPort), so that games whose
        dial screen only accepts a phone number still work.

    DreamPi compatible link (to play against a real Saturn plugged into a
    DreamPi, or another Kronos using it), same dial strings as DreamPi's
    netlink.py (https://github.com/Kazade/dreampi, "Shiro" protocol):
      - #a*b*c*d# or #aaabbbcccddd#: call the opponent at that IPv4 address.
      - 0 or 00 (or #0xx#): wait for the opponent by dialing, like with a
        DreamPi.
      - the game's own "Wait" mode also works: while idle, Kronos listens on
        TCP 65432 too, a DreamPi caller makes the game ring, and answering
        (ATA / ATS0) completes the handshake.
    Unlike a DreamPi, which answers the console at once, Kronos only gives
    CONNECT to the game once the opponent is really linked (handshake done),
    like a telephone line: games such as Daytona USA CCE start their own
    handshake on CONNECT and give up if nobody answers.
    The caller opens TCP port 65432 of the waiting side, sends "readyip" and
    gets "g2gip"; then the data goes by UDP (waiting side port 20001, calling
    side 20002), each datagram holding the last 5 packets
    "payload<dataSplit>seq" joined by "<packetSplit>", sent twice, plus
    PING_SHIRO/PONG_SHIRO every second. DreamPi's matchmaking server
    (#xx# numbers) and XBAND server numbers are not supported.

    Threads: the UART/modem side runs in the emulation thread
    (NetlinkReadByte/NetlinkWriteByte/NetlinkExec); a single network thread
    owns every socket. They only share single producer/single consumer rings
    and a few request/state words, so no lock is needed.

    Interrupt: the UART INTR output is a level, the SCU takes it as an A-Bus
    external interrupt (SCU User's Manual ST-097-R5, Table 2.1: external
    interrupt 12 = vector 5Ch, level 1, masked with all A-Bus interrupts by
    IMS bit 15). Each A-Bus interrupt must be re-enabled by the program
    writing 1 in AIACK (25FE00A8h, section 3.6), so while INTR stays high
    the request is presented again once AIACK is set (see
    NetlinkRefreshAbusIrq). Which external interrupt line the NetLink wires
    its UART to is not documented in the workspace: line 12 is the value
    inherited from Yabause.

    References: SCU User's Manual ST-097-R5 (A-Bus CS2 space 25800000h-
    258FFFFFh, external interrupts, AIACK), PC16550D UART datasheet
    (National Semiconductor / TI, register set, IIR priorities, MSR delta
    bits, loopback) and the Hayes / Rockwell AT command set. No Sega
    document on the NetLink cartridge itself is available in the workspace.
*/

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cs2.h"
#include "error.h"
#include "netlink.h"
#include "debug.h"
#include "scu.h"
#include "yui.h"
#ifdef USESOCKET
#include "sock.h"
#include "threads.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#endif

Netlink *NetlinkArea = NULL;

//////////////////////////////////////////////////////////////////////////////
// Configuration

static char netlink_cfg_host[NETLINK_HOST_SIZE] = "127.0.0.1";
static int netlink_cfg_port = NETLINK_DEFAULT_PORT;

//////////////////////////////////////////////////////////////////////////////
// Timings (microseconds)

#define NL_POLL_US          500      // call state / network polling period
#define NL_THRE_DELAY_US    100      // THR "transmitted" after a write
#define NL_RING_PERIOD_US   3000000  // one RING every 3 s
#define NL_RI_ON_US         1000000  // RI line high 1 s per ring
#define NL_DRAIN_US         500000   // keep delivering data after a remote hang-up

#define NL_RING_MASK        (NETLINK_RING_SIZE - 1)

// Messages for the user: Kronos log window (YuiMsg) and stderr.
// NL_INFO from the emulation thread, NL_TINFO from the network thread: the
// log window of the Qt port must not be touched by any thread, so the
// network thread queues its messages and NetlinkExec() shows them.
static void NetlinkInfo(int fromthread, const char *fmt, ...);
static void NetlinkFlushMessages(void);
#define NL_INFO(...)  NetlinkInfo(0, __VA_ARGS__)
#define NL_TINFO(...) NetlinkInfo(1, __VA_ARGS__)

//////////////////////////////////////////////////////////////////////////////
// Memory ordering between the emulation thread and the network thread

#if defined(__GNUC__) || defined(__clang__)
#define NL_LOAD(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define NL_STORE(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#else
// MSVC: volatile accesses have acquire/release semantics (/volatile:ms)
#define NL_LOAD(p)      (*(p))
#define NL_STORE(p, v)  (*(p) = (v))
#endif

//////////////////////////////////////////////////////////////////////////////
// Rings

// Messages queued by the network thread (single producer/single consumer)
#define NL_MSG_SLOTS 32
#define NL_MSG_LEN   192
static char netlink_msg[NL_MSG_SLOTS][NL_MSG_LEN];
static volatile u32 netlink_msg_head = 0, netlink_msg_tail = 0;

static void NetlinkInfo(int fromthread, const char *fmt, ...)
{
   char text[NL_MSG_LEN];
   va_list ap;

   va_start(ap, fmt);
   vsnprintf(text, sizeof(text), fmt, ap);
   va_end(ap);

   fprintf(stderr, "[NetLink] %s\n", text);

   if (!fromthread)
   {
      YuiMsg("[NetLink] %s\n", text);
      return;
   }

   {
      u32 head = netlink_msg_head;
      if (head - NL_LOAD(&netlink_msg_tail) >= NL_MSG_SLOTS)
         return; // queue full: stderr only
      snprintf(netlink_msg[head % NL_MSG_SLOTS], NL_MSG_LEN, "%s", text);
      NL_STORE(&netlink_msg_head, head + 1);
   }
}

// Emulation thread: shows the messages of the network thread
static void NetlinkFlushMessages(void)
{
   u32 tail = netlink_msg_tail;

   while (NL_LOAD(&netlink_msg_head) != tail)
   {
      YuiMsg("[NetLink] %s\n", netlink_msg[tail % NL_MSG_SLOTS]);
      tail++;
      NL_STORE(&netlink_msg_tail, tail);
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 RingCount(NetlinkRing *r)
{
   return NL_LOAD(&r->head) - NL_LOAD(&r->tail);
}

static INLINE int RingPush(NetlinkRing *r, u8 val)
{
   u32 head = r->head;

   if (head - NL_LOAD(&r->tail) >= NETLINK_RING_SIZE)
      return 0;

   r->data[head & NL_RING_MASK] = val;
   NL_STORE(&r->head, head + 1);
   return 1;
}

static INLINE int RingPop(NetlinkRing *r, u8 *val)
{
   u32 tail = r->tail;

   if (NL_LOAD(&r->head) == tail)
      return 0;

   *val = r->data[tail & NL_RING_MASK];
   NL_STORE(&r->tail, tail + 1);
   return 1;
}

// May only be called by the consumer of the ring
static INLINE void RingFlush(NetlinkRing *r)
{
   NL_STORE(&r->tail, NL_LOAD(&r->head));
}

static void RingPushString(NetlinkRing *r, const char *str)
{
   while (*str)
      RingPush(r, (u8)*str++);
}

//////////////////////////////////////////////////////////////////////////////
// UART side

static void NetlinkHangup(void);
static void NetlinkRequest(int req);

static int NetlinkLoopback(void)
{
   return (NetlinkArea->reg.MCR & 0x10) != 0;
}

//////////////////////////////////////////////////////////////////////////////

static int NetlinkRxAvailable(void)
{
   if (RingCount(&NetlinkArea->rxlocal))
      return 1;

   return NetlinkArea->modemstate == NL_MODEMSTATE_DATA &&
          NetlinkArea->callstate == NL_CALL_ONLINE &&
          RingCount(&NetlinkArea->rxnet) != 0;
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkUpdateLSR(void)
{
   // Transmission is instantaneous: THRE and TEMT are always set
   NetlinkArea->reg.LSR |= 0x60;

   if (NetlinkRxAvailable())
      NetlinkArea->reg.LSR |= 0x01;
   else
      NetlinkArea->reg.LSR &= ~0x01;
}

//////////////////////////////////////////////////////////////////////////////

// Interrupt identification, by decreasing priority (PC16550D):
// line status (6), received data (4), THR empty (2), modem status (0).
// "event" forces a new interrupt request while the line is already asserted
// (new data arrived, new modem status change...), the SCU interrupt being
// raised on an edge here while the UART INTR output is a level.
static void NetlinkUpdateIrq(int event)
{
   Netlink *nl = NetlinkArea;
   u8 ier = nl->reg.IER;
   u8 id;
   int asserted;

   if ((ier & 0x04) && (nl->reg.LSR & 0x1E))
      id = 0x06;
   else if ((ier & 0x01) && (nl->reg.LSR & 0x01))
      id = 0x04;
   else if ((ier & 0x02) && nl->thre_pending)
      id = 0x02;
   else if ((ier & 0x08) && (nl->reg.MSR & 0x0F))
      id = 0x00;
   else
      id = 0x01;

   nl->reg.IIR = (nl->fifo_enabled ? 0xC0 : 0x00) | id;

   asserted = !(id & 0x01);
   if (asserted && (!nl->irq_asserted || event))
   {
      ScuSendExternalInterrupt12();
      nl->irq_raise_time = 0;
   }
   nl->irq_asserted = asserted;
}

//////////////////////////////////////////////////////////////////////////////

// SCU status bit of external interrupt 12 (ST-097-R5 Table 2.1, bit 28)
#define NL_SCU_EXT12_STATUS 0x10000000

// INTR is a level: if it is still high once the program has served the
// previous request and re-enabled A-Bus interrupts (AIACK = 1, ST-097-R5
// section 3.6), present it again. Without this, an interrupt routine that
// leaves bytes in the receiver (or returns before reading IIR) waited for
// the next network packet to be interrupted again. The request must have
// been pending for at least 1 ms, so that a routine still running is not
// interrupted a second time for nothing.
static void NetlinkRefreshAbusIrq(void)
{
   Netlink *nl = NetlinkArea;

   if (!nl->irq_asserted || ScuRegs == NULL || nl->irq_raise_time < 1000)
      return;

   if (!(ScuRegs->AIACK & 0x1) || (ScuRegs->ITEdge & NL_SCU_EXT12_STATUS))
      return;

   ScuSendExternalInterrupt12();
   nl->irq_raise_time = 0;
}

//////////////////////////////////////////////////////////////////////////////

// New bytes are readable
static void NetlinkRxEvent(void)
{
   NetlinkUpdateLSR();
   NetlinkUpdateIrq(1);
}

//////////////////////////////////////////////////////////////////////////////

// DCD/RI/DSR/CTS as seen in MSR bits 7..4
static u8 NetlinkLines(void)
{
   Netlink *nl = NetlinkArea;
   u8 lines;

   if (NetlinkLoopback())
   {
      // Loopback: CTS=RTS, DSR=DTR, RI=OUT1, DCD=OUT2
      u8 mcr = nl->reg.MCR;
      return ((mcr & 0x02) ? 0x10 : 0) | ((mcr & 0x01) ? 0x20 : 0) |
             ((mcr & 0x04) ? 0x40 : 0) | ((mcr & 0x08) ? 0x80 : 0);
   }

   lines = 0x30; // CTS + DSR: the modem is always ready

   if (nl->callstate == NL_CALL_ONLINE || nl->callstate == NL_CALL_SEGANET)
      lines |= 0x80;

   if (nl->callstate == NL_CALL_RINGING && nl->ring_count > 0 &&
       nl->ring_timer < NL_RI_ON_US)
      lines |= 0x40;

   return lines;
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkRefreshMSR(void)
{
   Netlink *nl = NetlinkArea;
   u8 newlines = NetlinkLines();
   u8 oldlines = nl->msr_lines;
   u8 delta = 0;

   if ((newlines ^ oldlines) & 0x10) delta |= 0x01; // DCTS
   if ((newlines ^ oldlines) & 0x20) delta |= 0x02; // DDSR
   if ((oldlines & 0x40) && !(newlines & 0x40)) delta |= 0x04; // TERI
   if ((newlines ^ oldlines) & 0x80) delta |= 0x08; // DDCD

   nl->msr_lines = newlines;
   nl->reg.MSR = newlines | (nl->reg.MSR & 0x0F) | delta;

   if (delta)
      NetlinkUpdateIrq(1);
}

//////////////////////////////////////////////////////////////////////////////
// Modem responses

static void NetlinkRespond(enum NL_RESULTCODE code)
{
   static const char *text[] = {
      "OK", "CONNECT", "RING", "NO CARRIER", "ERROR", "CONNECT 1200",
      "NO DIALTONE", "BUSY", "NO ANSWER"
   };
   Netlink *nl = NetlinkArea;
   char buf[32];

   if (code == NL_RESULTCODE_NONE || nl->quiet)
      return;

   if (nl->verbose)
   {
      const char *str = text[code];

      if (code == NL_RESULTCODE_CONNECT && nl->xlevel > 0)
         str = "CONNECT 28800";

      snprintf(buf, sizeof(buf), "\r\n%s\r\n", str);
   }
   else
      snprintf(buf, sizeof(buf), "%d\r", (int)code);

   RingPushString(&nl->rxlocal, buf);
   NetlinkRxEvent();
}

//////////////////////////////////////////////////////////////////////////////

// Information text (ATI, ATSn?, AT&V)
static void NetlinkInfoText(const char *str)
{
   Netlink *nl = NetlinkArea;

   RingPushString(&nl->rxlocal, "\r\n");
   RingPushString(&nl->rxlocal, str);
   RingPushString(&nl->rxlocal, "\r\n");
   NetlinkRxEvent();
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkDefaults(void)
{
   Netlink *nl = NetlinkArea;

   memset(nl->reg.SREG, 0, sizeof(nl->reg.SREG));
   nl->reg.SREG[0] = 0;    // rings before auto-answer (0 = no auto-answer)
   nl->reg.SREG[2] = 43;   // escape character '+'
   nl->reg.SREG[3] = 13;   // carriage return
   nl->reg.SREG[4] = 10;   // line feed
   nl->reg.SREG[5] = 8;    // backspace
   nl->reg.SREG[6] = 2;    // wait for dial tone (s)
   nl->reg.SREG[7] = 50;   // wait for carrier (s)
   nl->reg.SREG[8] = 2;    // comma pause (s)
   nl->reg.SREG[10] = 14;  // carrier loss delay (1/10 s)
   nl->reg.SREG[12] = 50;  // escape guard time (1/50 s)

   nl->isechoenab = 1;
   nl->quiet = 0;
   nl->verbose = 1;
   nl->xlevel = 4;
   nl->dtrmode = 2;
}

//////////////////////////////////////////////////////////////////////////////
// Dial string

static int NetlinkIsSegaNetNumber(const char *digits)
{
   return strcmp(digits, "18007798852") == 0 || strcmp(digits, "8007798852") == 0;
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkSplitHostPort(char *str, char *host, size_t hostsz, int *port)
{
   char *colon;

   if (str[0] == '[' && (colon = strchr(str, ']')) != NULL)
   {
      // [ipv6]:port
      *colon = '\0';
      if (colon[1] == ':' && atoi(colon + 2) > 0)
         *port = atoi(colon + 2);
      snprintf(host, hostsz, "%s", str + 1);
      return;
   }

   colon = strchr(str, ':');
   if (colon != NULL && strchr(colon + 1, ':') == NULL)
   {
      // host:port (a single ':', not an IPv6 address)
      *colon = '\0';
      if (atoi(colon + 1) > 0 && atoi(colon + 1) < 65536)
         *port = atoi(colon + 1);
   }

   snprintf(host, hostsz, "%s", str);
}

//////////////////////////////////////////////////////////////////////////////

enum NL_DIALKIND
{
   NL_DIAL_TCP,                 // Kronos TCP link to host:port
   NL_DIAL_SEGANET,             // SegaNet stub
   NL_DIAL_SHIRO_CALL,          // DreamPi link, call host
   NL_DIAL_SHIRO_WAIT,          // DreamPi link, wait for the opponent
   NL_DIAL_UNSUPPORTED,         // DreamPi matchmaking: not available
};

//////////////////////////////////////////////////////////////////////////////

// DreamPi dial strings (digit_parser() of DreamPi's netlink.py). tel only
// holds the characters 0-9 * #, like what DreamPi decodes from the line.
static int NetlinkParseDreamPi(const char *tel, char *host, size_t hostsz)
{
   char inner[NETLINK_CMDLINE_SIZE];
   size_t len = strlen(tel);
   int parts[4], nparts = 0, i;
   const char *p;

   if (strcmp(tel, "0") == 0 || strcmp(tel, "00") == 0)
      return NL_DIAL_SHIRO_WAIT;

   if (len < 3 || tel[0] != '#' || tel[len - 1] != '#')
      return -1;

   snprintf(inner, sizeof(inner), "%.*s", (int)(len - 2), tel + 1);
   len = strlen(inner);

   // #0xx#: wait for a call (xx = DreamPi matchmaking game id, ignored)
   if (len == 3 && inner[0] == '0' && strchr(inner, '*') == NULL)
      return NL_DIAL_SHIRO_WAIT;

   // #a*b*c*d#
   for (p = inner; nparts < 5; nparts++)
   {
      if (!isdigit((unsigned char)*p))
         break;
      if (nparts < 4)
         parts[nparts] = atoi(p);
      while (isdigit((unsigned char)*p))
         p++;
      if (*p != '*')
      {
         nparts++;
         break;
      }
      p++;
   }
   if (nparts == 4 && *p == '\0')
   {
      for (i = 0; i < 4; i++)
         if (parts[i] > 255)
            return NL_DIAL_UNSUPPORTED;
      snprintf(host, hostsz, "%d.%d.%d.%d", parts[0], parts[1], parts[2], parts[3]);
      return NL_DIAL_SHIRO_CALL;
   }

   // #aaabbbcccddd#
   if (len == 12 && strspn(inner, "0123456789") == 12)
   {
      for (i = 0; i < 4; i++)
      {
         parts[i] = (inner[i*3] - '0') * 100 + (inner[i*3+1] - '0') * 10 + (inner[i*3+2] - '0');
         if (parts[i] > 255)
            return NL_DIAL_UNSUPPORTED;
      }
      snprintf(host, hostsz, "%d.%d.%d.%d", parts[0], parts[1], parts[2], parts[3]);
      return NL_DIAL_SHIRO_CALL;
   }

   // XBAND format (#a*b*c*d*1#) and matchmaking calls (#xx#)
   return NL_DIAL_UNSUPPORTED;
}

//////////////////////////////////////////////////////////////////////////////

// Returns a NL_DIALKIND
static int NetlinkParseDial(const char *dial, char *host, size_t hostsz, int *port)
{
   Netlink *nl = NetlinkArea;
   char buf[NETLINK_CMDLINE_SIZE];
   char digits[NETLINK_CMDLINE_SIZE];
   char tel[NETLINK_CMDLINE_SIZE];
   char *p, *q;
   int i, n, hostlike = 0, kind;

   snprintf(host, hostsz, "%s", nl->defaulthost);
   *port = nl->defaultport;

   snprintf(buf, sizeof(buf), "%s", dial);

   // A trailing ';' means "back to command mode": not relevant here
   if ((p = strchr(buf, ';')) != NULL)
      *p = '\0';

   // DreamPi numbers first: #a*b*c*d# contains '*' like the legacy form
   for (n = 0, q = buf; *q && n < (int)sizeof(tel) - 1; q++)
      if (isdigit((unsigned char)*q) || *q == '*' || *q == '#')
         tel[n++] = *q;
   tel[n] = '\0';

   if ((kind = NetlinkParseDreamPi(tel, host, hostsz)) >= 0)
      return kind;

   // Legacy form: *a,b,c,d*port
   if ((p = strchr(buf, '*')) != NULL)
   {
      p++;
      for (q = p; *q; q++)
         if (*q == ',')
            *q = '.';

      if ((q = strchr(p, '*')) != NULL)
      {
         *q = '\0';
         if (atoi(q + 1) > 0 && atoi(q + 1) < 65536)
            *port = atoi(q + 1);
      }

      while (*p == ' ')
         p++;
      for (q = p + strlen(p); q > p && q[-1] == ' '; q--)
         q[-1] = '\0';

      if (*p)
         snprintf(host, hostsz, "%s", p);
      return NL_DIAL_TCP;
   }

   // Skip the dial modifiers in front of the number: one tone/pulse
   // selection (ATDT/ATDP), then pauses and waits
   p = buf;
   while (*p == ' ')
      p++;
   if (*p == 'T' || *p == 't' || *p == 'P' || *p == 'p')
      p++;
   while (*p && strchr("Ww,!@ ", *p))
      p++;

   // Trim trailing spaces
   for (q = p + strlen(p); q > p && q[-1] == ' '; q--)
      q[-1] = '\0';

   for (q = p; *q; q++)
      if (*q == '.' || *q == ':' || *q == '[' ||
          (isalpha((unsigned char)*q) && !strchr("WwTtPp", *q)))
         hostlike = 1;

   if (hostlike)
   {
      NetlinkSplitHostPort(p, host, hostsz, port);
      return NL_DIAL_TCP;
   }

   // Phone number: keep only the digits
   for (n = 0, q = p; *q && n < (int)sizeof(digits) - 1; q++)
      if (isdigit((unsigned char)*q))
         digits[n++] = *q;
   digits[n] = '\0';

   if (NetlinkIsSegaNetNumber(digits))
      return NL_DIAL_SEGANET;

   // 12 digits = IPv4 address (3 digits per byte), then an optional port
   if (n >= 12 && n <= 17)
   {
      int octet[4], valid = 1, dport = 0;

      for (i = 0; i < 4; i++)
      {
         octet[i] = (digits[i*3] - '0') * 100 + (digits[i*3+1] - '0') * 10 + (digits[i*3+2] - '0');
         if (octet[i] > 255)
            valid = 0;
      }

      if (n > 12)
      {
         dport = atoi(digits + 12);
         if (dport <= 0 || dport > 65535)
            valid = 0;
      }

      if (valid)
      {
         snprintf(host, hostsz, "%d.%d.%d.%d", octet[0], octet[1], octet[2], octet[3]);
         if (dport)
            *port = dport;
         return NL_DIAL_TCP;
      }
   }

   // Any other number: the peer configured in the settings
   return NL_DIAL_TCP;
}

//////////////////////////////////////////////////////////////////////////////
// Call control (emulation thread)

static void NetlinkGoOnline(void)
{
   Netlink *nl = NetlinkArea;

   nl->callstate = NL_CALL_ONLINE;
   nl->modemstate = NL_MODEMSTATE_DATA;
   nl->escape_count = 0;
   nl->call_timer = 0;
   nl->rxnet_seen = NL_LOAD(&nl->rxnet.tail);

   NetlinkRespond(NL_RESULTCODE_CONNECT);
   NetlinkRefreshMSR();
   NetlinkRxEvent();
}

//////////////////////////////////////////////////////////////////////////////

static enum NL_RESULTCODE NetlinkDial(const char *dial)
{
   Netlink *nl = NetlinkArea;
   char host[NETLINK_HOST_SIZE];
   int port, kind;

   if (nl->callstate != NL_CALL_ONHOOK)
      return NL_RESULTCODE_ERROR;

   kind = NetlinkParseDial(dial, host, sizeof(host), &port);

   if (kind == NL_DIAL_UNSUPPORTED)
   {
      NL_INFO("DreamPi matchmaking/XBAND number \"%s\" not supported: dial #a*b*c*d# or wait with 0", dial);
      return NL_RESULTCODE_NOANSWER;
   }

   if (kind == NL_DIAL_SEGANET)
   {
      // Sega's old network: pretend to be a login shell
      NL_INFO("SegaNet number dialed (stub)");
      nl->callstate = NL_CALL_SEGANET;
      nl->modemstate = NL_MODEMSTATE_DATA;
      nl->seganet_step = 0;
      NetlinkRespond(NL_RESULTCODE_CONNECT);
      RingPushString(&nl->rxlocal, "\r\nlogin:");
      NetlinkRefreshMSR();
      NetlinkRxEvent();
      return NL_RESULTCODE_NONE;
   }

#ifdef USESOCKET
   if (!nl->thread_started)
      return NL_RESULTCODE_NODIALTONE;

   if (kind == NL_DIAL_SHIRO_CALL || kind == NL_DIAL_SHIRO_WAIT)
   {
      // CONNECT once the opponent is linked (see the top of the file)
      if (kind == NL_DIAL_SHIRO_CALL)
      {
         NL_INFO("DreamPi link: calling %s", host);
         snprintf(nl->dialhost, sizeof(nl->dialhost), "%s", host);
         NetlinkRequest(NL_REQ_SHIRO_CALL);
      }
      else
      {
         NL_INFO("DreamPi link: waiting for the opponent (TCP %d, UDP %d)",
                 NETLINK_SHIRO_TCP_PORT, NETLINK_SHIRO_UDP_WAIT);
         NetlinkRequest(NL_REQ_SHIRO_WAIT);
      }
      nl->dial_waiting = (kind == NL_DIAL_SHIRO_WAIT);
      nl->callstate = NL_CALL_DIALING;
      nl->call_timer = 0;
      return NL_RESULTCODE_NONE;
   }

   NL_INFO("dialing %s port %d", host, port);
   nl->dial_waiting = 0;

   snprintf(nl->dialhost, sizeof(nl->dialhost), "%s", host);
   nl->dialport = port;
   nl->dialtimeout_ms = (nl->reg.SREG[7] ? nl->reg.SREG[7] : 1) * 1000;
   NetlinkRequest(NL_REQ_DIAL);

   nl->callstate = NL_CALL_DIALING;
   nl->call_timer = 0;
   return NL_RESULTCODE_NONE;
#else
   NL_INFO("built without network support (USESOCKET)");
   return NL_RESULTCODE_NODIALTONE;
#endif
}

//////////////////////////////////////////////////////////////////////////////

static enum NL_RESULTCODE NetlinkAnswer(void)
{
   Netlink *nl = NetlinkArea;

   switch (nl->callstate)
   {
      case NL_CALL_RINGING:
         NetlinkRequest(NL_REQ_ANSWER);
         nl->callstate = NL_CALL_ANSWERWAIT;
         nl->call_timer = 0;
         NetlinkRefreshMSR();
         return NL_RESULTCODE_NONE;
      case NL_CALL_ONHOOK:
         // Off hook in answer mode: wait S7 seconds for a caller
         nl->callstate = NL_CALL_ANSWERWAIT;
         nl->call_timer = 0;
         return NL_RESULTCODE_NONE;
      default:
         return NL_RESULTCODE_ERROR;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkHangup(void)
{
   Netlink *nl = NetlinkArea;

   if (nl->callstate != NL_CALL_ONHOOK || NL_LOAD(&nl->net_state) != NL_NET_IDLE)
      NetlinkRequest(NL_REQ_HANGUP);

   if (nl->callstate != NL_CALL_ONHOOK)
      NL_INFO("hang up");

   nl->callstate = NL_CALL_ONHOOK;
   nl->modemstate = NL_MODEMSTATE_COMMAND;
   nl->escape_count = 0;
   nl->ring_count = 0;
   nl->reg.SREG[1] = 0;

   RingFlush(&nl->rxnet);
   nl->rxnet_seen = NL_LOAD(&nl->rxnet.tail);

   NetlinkRefreshMSR();
   NetlinkUpdateLSR();
   NetlinkUpdateIrq(0);
}

//////////////////////////////////////////////////////////////////////////////
// AT command interpreter

static int NetlinkATNumber(const char **p)
{
   int val = 0;

   while (isdigit((unsigned char)**p))
   {
      val = val * 10 + (**p - '0');
      if (val > 0xFFFF)
         val = 0xFFFF;
      (*p)++;
   }

   return val;
}

//////////////////////////////////////////////////////////////////////////////

static enum NL_RESULTCODE NetlinkExecuteAT(const char *p)
{
   Netlink *nl = NetlinkArea;
   char buf[128];
   int n;

   while (*p)
   {
      char c = (char)toupper((unsigned char)*p++);

      switch (c)
      {
         case ' ':
         case ';':
            break;
         case 'A': // Answer (nothing may follow)
            return NetlinkAnswer();
         case 'D': // Dial (the rest of the line is the number)
            return NetlinkDial(p);
         case 'E': // Command echo
            n = NetlinkATNumber(&p);
            if (n > 1)
               return NL_RESULTCODE_ERROR;
            nl->isechoenab = n;
            break;
         case 'H': // Hook control
            n = NetlinkATNumber(&p);
            if (n > 1)
               return NL_RESULTCODE_ERROR;
            if (n == 0)
               NetlinkHangup();
            break;
         case 'I': // Identification
            n = NetlinkATNumber(&p);
            switch (n)
            {
               case 0: NetlinkInfoText("28800"); break;
               case 1: NetlinkInfoText("255"); break;
               case 3: NetlinkInfoText("Kronos NetLink modem emulation (TCP)"); break;
               default: break;
            }
            break;
         case 'O': // Return on line
            NetlinkATNumber(&p);
            if (nl->callstate == NL_CALL_ONLINE)
            {
               nl->modemstate = NL_MODEMSTATE_DATA;
               nl->escape_count = 0;
               nl->rxnet_seen = NL_LOAD(&nl->rxnet.tail);
               NetlinkRespond(NL_RESULTCODE_CONNECT);
               NetlinkRxEvent();
               return NL_RESULTCODE_NONE;
            }
            return NL_RESULTCODE_NOCARRIER;
         case 'Q': // Quiet
            n = NetlinkATNumber(&p);
            if (n > 1)
               return NL_RESULTCODE_ERROR;
            nl->quiet = n;
            break;
         case 'V': // Verbose result codes
            n = NetlinkATNumber(&p);
            if (n > 1)
               return NL_RESULTCODE_ERROR;
            nl->verbose = n;
            break;
         case 'X': // Extended result codes
            n = NetlinkATNumber(&p);
            if (n > 4)
               return NL_RESULTCODE_ERROR;
            nl->xlevel = n;
            break;
         case 'Z': // Reset (hangs up, nothing may follow)
            NetlinkATNumber(&p);
            NetlinkHangup();
            NetlinkDefaults();
            return NL_RESULTCODE_OK;
         case 'S': // S registers: Sn=v, Sn?
         {
            int reg = NetlinkATNumber(&p) & 0xFF;

            if (*p == '=')
            {
               p++;
               n = NetlinkATNumber(&p);
               if (n > 255)
                  return NL_RESULTCODE_ERROR;
               if (reg != 1) // S1 (ring count) is read-only
                  nl->reg.SREG[reg] = (u8)n;
            }
            else if (*p == '?')
            {
               p++;
               snprintf(buf, sizeof(buf), "%03u", nl->reg.SREG[reg]);
               NetlinkInfoText(buf);
            }
            break;
         }
         case '&':
         {
            char c2 = (char)toupper((unsigned char)*p);

            if (c2 == '\0')
               return NL_RESULTCODE_ERROR;
            p++;
            n = NetlinkATNumber(&p);

            switch (c2)
            {
               case 'F': // Factory defaults
                  NetlinkDefaults();
                  break;
               case 'D': // DTR behaviour
                  if (n > 3)
                     return NL_RESULTCODE_ERROR;
                  nl->dtrmode = n;
                  break;
               case 'V': // View configuration
                  snprintf(buf, sizeof(buf), "E%d Q%d V%d X%d &D%d S00:%03u S07:%03u S12:%03u",
                           nl->isechoenab, nl->quiet, nl->verbose, nl->xlevel, nl->dtrmode,
                           nl->reg.SREG[0], nl->reg.SREG[7], nl->reg.SREG[12]);
                  NetlinkInfoText(buf);
                  break;
               default: // &C, &K, &Q, &S, &W, &Y...: accepted, no effect
                  break;
            }
            break;
         }
         case '\\':
         case '%':
         case '*':
            // Vendor specific: letter + number, accepted
            if (*p)
               p++;
            NetlinkATNumber(&p);
            break;
         case '+':
         case '#':
         case '-':
            // Extended syntax commands (AT+MS=..., AT#CID=...): accepted,
            // the rest of the line is ignored
            return NL_RESULTCODE_OK;
         case 'B': case 'C': case 'F': case 'K': case 'L': case 'M':
         case 'N': case 'P': case 'T': case 'W': case 'Y':
            // Accepted, no effect on the emulation
            NetlinkATNumber(&p);
            break;
         default:
            NETLINK_LOG("Unsupported AT command %c\n", c);
            return NL_RESULTCODE_ERROR;
      }
   }

   return NL_RESULTCODE_OK;
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkProcessCommandLine(const char *line)
{
   Netlink *nl = NetlinkArea;
   const char *p;

   // Everything before "AT" is ignored, lines without "AT" too
   for (p = line; *p; p++)
      if (toupper((unsigned char)p[0]) == 'A' && toupper((unsigned char)p[1]) == 'T')
         break;

   if (*p == '\0')
      return;

   NETLINK_LOG("Program issued %s\n", p);

   if (p != nl->lastcmd)
      snprintf(nl->lastcmd, sizeof(nl->lastcmd), "%s", p);

   NetlinkRespond(NetlinkExecuteAT(p + 2));
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkCommandChar(u8 val)
{
   Netlink *nl = NetlinkArea;

   // Any character aborts a dial or an answer in progress
   if (nl->callstate == NL_CALL_DIALING || nl->callstate == NL_CALL_ANSWERWAIT)
   {
      NetlinkHangup();
      NetlinkRespond(NL_RESULTCODE_NOCARRIER);
      return;
   }

   if (nl->isechoenab)
   {
      RingPush(&nl->rxlocal, val);
      NetlinkRxEvent();
   }

   if (val == nl->reg.SREG[5]) // backspace
   {
      if (nl->cmdlen > 0)
         nl->cmdlen--;
      return;
   }

   if (val == nl->reg.SREG[3]) // end of command line
   {
      nl->cmdline[nl->cmdlen] = '\0';
      nl->cmdlen = 0;
      NetlinkProcessCommandLine(nl->cmdline);
      return;
   }

   if (val == '\n' || val == '\0')
      return;

   if (nl->cmdlen < NETLINK_CMDLINE_SIZE - 1)
      nl->cmdline[nl->cmdlen++] = (char)val;

   // A/ repeats the previous command line (no carriage return needed)
   if (nl->cmdlen == 2 && toupper((unsigned char)nl->cmdline[0]) == 'A' && nl->cmdline[1] == '/')
   {
      nl->cmdlen = 0;
      if (nl->lastcmd[0])
         NetlinkProcessCommandLine(nl->lastcmd);
   }
}

//////////////////////////////////////////////////////////////////////////////

// Minimal login shell answering to Sega's old network number
static void NetlinkSegaNetChar(u8 val)
{
   Netlink *nl = NetlinkArea;

   if (val != 0x0D)
      return;

   switch (nl->seganet_step)
   {
      case 0:
         RingPushString(&nl->rxlocal, "\r\npassword:");
         NetlinkRxEvent();
         nl->seganet_step = 1;
         break;
      case 1:
         RingPushString(&nl->rxlocal, "\r\n$");
         NetlinkRxEvent();
         nl->seganet_step = 2;
         break;
      default:
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkDataChar(u8 val)
{
   Netlink *nl = NetlinkArea;
   u32 guard = nl->reg.SREG[12] * 20000;

   // Escape sequence: guard time, 3 x S2, guard time (end checked in
   // NetlinkExec). The characters are transmitted, like on a real modem.
   if (nl->reg.SREG[2] < 128 && val == nl->reg.SREG[2])
   {
      if (nl->escape_count == 0)
      {
         if (nl->thr_idle_time >= guard)
            nl->escape_count = 1;
      }
      else if (nl->escape_count < 3)
         nl->escape_count++;
      else
         nl->escape_count = 0;
   }
   else
      nl->escape_count = 0;

   if (nl->callstate == NL_CALL_SEGANET)
      NetlinkSegaNetChar(val);
   else if (nl->callstate == NL_CALL_ONLINE)
   {
      if (!RingPush(&nl->tx, val))
      {
         NETLINK_LOG("transmit ring full, byte dropped\n");
      }
   }
}

//////////////////////////////////////////////////////////////////////////////

u8 FASTCALL NetlinkReadByte(SH2_struct *context, u8* memory, u32 addr)
{
   Netlink *nl = NetlinkArea;
   u8 ret;

   if (nl == NULL)
      return 0xFF;

   switch (addr & 0xFFFFF)
   {
      case 0x95001: // Receiver Buffer/Divisor Latch Low Byte
      {
         if (nl->reg.LCR & 0x80) // Divisor Latch Low Byte
            return nl->reg.DLL;

         // Modem responses first (they always precede the data that
         // follows a CONNECT), then the bytes from the remote modem
         if (RingPop(&nl->rxlocal, &ret))
            nl->reg.RBR = ret;
         else if (nl->modemstate == NL_MODEMSTATE_DATA &&
                  nl->callstate == NL_CALL_ONLINE &&
                  RingPop(&nl->rxnet, &ret))
            nl->reg.RBR = ret;

         NetlinkUpdateLSR();
         NetlinkUpdateIrq(0);
         return nl->reg.RBR;
      }
      case 0x95005: // Interrupt Enable Register/Divisor Latch High Byte
      {
         if (nl->reg.LCR & 0x80) // Divisor Latch High Byte
            return nl->reg.DLM;
         else // Interrupt Enable Register
            return nl->reg.IER;
      }
      case 0x95009: // Interrupt Identification Register
      {
         NetlinkUpdateIrq(0);
         ret = nl->reg.IIR;

         // Reading the IIR clears a THR empty interrupt when it is the
         // source being reported
         if ((ret & 0x0F) == 0x02)
         {
            nl->thre_pending = 0;
            NetlinkUpdateIrq(0);
         }
         return ret;
      }
      case 0x9500D: // Line Control Register
      {
         return nl->reg.LCR;
      }
      case 0x95011: // Modem Control Register
      {
         return nl->reg.MCR;
      }
      case 0x95015: // Line Status Register
      {
         NetlinkUpdateLSR();
         ret = nl->reg.LSR;
         nl->reg.LSR &= ~0x1E; // OE/PE/FE/BI are cleared by the read
         NetlinkUpdateIrq(0);
         return ret;
      }
      case 0x95019: // Modem Status Register
      {
         ret = nl->reg.MSR;
         nl->reg.MSR &= 0xF0; // delta bits are cleared by the read
         NetlinkUpdateIrq(0);
         return ret;
      }
      case 0x9501D: // Scratch
      {
         return nl->reg.SCR;
      }
      default:
         break;
   }

   NETLINK_LOG("Unimplemented Netlink byte read: %08X\n", addr);
   return 0xFF;
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL NetlinkWriteByte(SH2_struct *context, u8* memory, u32 addr, u8 val)
{
   Netlink *nl = NetlinkArea;

   if (nl == NULL)
      return;

   switch (addr & 0xFFFFF)
   {
      case 0x2503D: // ???
      {
         return;
      }
      case 0x95001: // Transmitter Holding Buffer/Divisor Latch Low Byte
      {
         if (nl->reg.LCR & 0x80) // Divisor Latch Low Byte
         {
            nl->reg.DLL = val;
            return;
         }

         nl->reg.THR = val;
         nl->thre_pending = 0; // writing the THR clears its interrupt

         if (NetlinkLoopback())
         {
            RingPush(&nl->rxlocal, val);
            NetlinkRxEvent();
         }
         else if (nl->modemstate == NL_MODEMSTATE_DATA)
            NetlinkDataChar(val);
         else
            NetlinkCommandChar(val);

         nl->thr_idle_time = 0;

         NetlinkUpdateLSR();
         NetlinkUpdateIrq(0);
         return;
      }
      case 0x95005: // Interrupt Enable Register/Divisor Latch High Byte
      {
         if (nl->reg.LCR & 0x80) // Divisor Latch High Byte
         {
            nl->reg.DLM = val;
            return;
         }

         // Enabling the THR empty interrupt while the THR is empty
         // requests an interrupt immediately
         if ((val & 0x02) && !(nl->reg.IER & 0x02) && nl->thr_idle_time >= NL_THRE_DELAY_US)
            nl->thre_pending = 1;

         nl->reg.IER = val & 0x0F;
         NetlinkUpdateIrq(0);
         return;
      }
      case 0x95009: // FIFO Control Register
      {
         nl->reg.FCR = val;
         nl->fifo_enabled = val & 0x01;

         if (val & 0x02) // receiver FIFO reset
         {
            RingFlush(&nl->rxlocal);
            if (nl->modemstate == NL_MODEMSTATE_DATA)
            {
               RingFlush(&nl->rxnet);
               nl->rxnet_seen = NL_LOAD(&nl->rxnet.tail);
            }
         }

         NetlinkUpdateLSR();
         NetlinkUpdateIrq(0);
         return;
      }
      case 0x9500D: // Line Control Register
      {
         nl->reg.LCR = val;
         return;
      }
      case 0x95011: // Modem Control Register
      {
         u8 old = nl->reg.MCR;

         nl->reg.MCR = val & 0x1F;

         // DTR dropped: hang up (&D2) or reset (&D3)
         if ((old & 0x01) && !(val & 0x01) && !(val & 0x10) && nl->dtrmode >= 2 &&
             nl->callstate != NL_CALL_ONHOOK)
         {
            NetlinkHangup();
            if (nl->dtrmode == 3)
               NetlinkDefaults();
            NetlinkRespond(NL_RESULTCODE_NOCARRIER);
         }

         NetlinkRefreshMSR();
         return;
      }
      case 0x95015: // Line Status Register (factory test only)
         return;
      case 0x95019: // Modem Status Register(read-only)
         return;
      case 0x9501D: // Scratch
      {
         nl->reg.SCR = val;
         return;
      }
      default:
         break;
   }

   NETLINK_LOG("Unimplemented Netlink byte write: %08X\n", addr);
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkPollCall(void)
{
   Netlink *nl = NetlinkArea;
   int ns = -1;
   u32 timeout = (nl->reg.SREG[7] ? nl->reg.SREG[7] : 1) * 1000000;

   // The network state is only meaningful once the network thread has
   // processed every request sent to it
   if (NL_LOAD(&nl->net_ack_id) == nl->net_req_id)
      ns = NL_LOAD(&nl->net_state);

   switch (nl->callstate)
   {
      case NL_CALL_ONHOOK:
         if (ns == NL_NET_INCOMING)
         {
            NL_INFO("incoming call");
            nl->callstate = NL_CALL_RINGING;
            nl->ring_count = 0;
            nl->ring_timer = NL_RING_PERIOD_US; // first RING now
         }
         else if (ns == NL_NET_FAILED || ns == NL_NET_CLOSED)
            NetlinkRequest(NL_REQ_HANGUP);
         break;
      case NL_CALL_RINGING:
         if (ns != -1 && ns != NL_NET_INCOMING)
         {
            // The caller gave up
            NetlinkHangup();
            break;
         }

         if (nl->ring_timer >= NL_RING_PERIOD_US)
         {
            nl->ring_timer = 0;
            nl->ring_count++;
            nl->reg.SREG[1] = (u8)(nl->ring_count > 255 ? 255 : nl->ring_count);
            NetlinkRespond(NL_RESULTCODE_RING);

            if (nl->reg.SREG[0] != 0 && nl->ring_count >= nl->reg.SREG[0])
            {
               // Auto-answer
               NetlinkRequest(NL_REQ_ANSWER);
               nl->callstate = NL_CALL_ANSWERWAIT;
               nl->call_timer = 0;
            }
         }
         NetlinkRefreshMSR();
         break;
      case NL_CALL_ANSWERWAIT:
         if (ns == NL_NET_INCOMING)
            NetlinkRequest(NL_REQ_ANSWER);
         else if (ns == NL_NET_CONNECTED)
         {
            NL_INFO("connected");
            NetlinkGoOnline();
         }
         else if (ns == NL_NET_FAILED || ns == NL_NET_CLOSED || nl->call_timer >= timeout)
         {
            NetlinkHangup();
            NetlinkRespond(NL_RESULTCODE_NOCARRIER);
         }
         break;
      case NL_CALL_DIALING:
         if (ns == NL_NET_CONNECTED)
         {
            NL_INFO("connected");
            NetlinkGoOnline();
         }
         else if (ns == NL_NET_FAILED)
         {
            int busy = (NL_LOAD(&nl->net_fail) == NL_FAIL_BUSY);
            NL_INFO("%s", busy ? "busy (connection refused)" : "no answer");
            NetlinkHangup();
            NetlinkRespond(busy ? NL_RESULTCODE_BUSY : NL_RESULTCODE_NOANSWER);
         }
         else if (ns == NL_NET_CLOSED ||
                  nl->call_timer >= (nl->dial_waiting ? 121000000u : timeout + 1000000))
         {
            NetlinkHangup();
            NetlinkRespond(NL_RESULTCODE_NOANSWER);
         }
         break;
      case NL_CALL_ONLINE:
      {
         u32 head = NL_LOAD(&nl->rxnet.head);

         if (head != nl->rxnet_seen)
         {
            nl->rxnet_seen = head;
            if (nl->modemstate == NL_MODEMSTATE_DATA)
               NetlinkRxEvent();
         }

         if (ns == NL_NET_CLOSED || ns == NL_NET_IDLE || ns == NL_NET_FAILED)
         {
            // Carrier lost: let the program read what was received just
            // before the remote hang-up
            if (nl->modemstate == NL_MODEMSTATE_DATA && RingCount(&nl->rxnet) &&
                nl->call_timer < NL_DRAIN_US)
               break;

            NL_INFO("remote hung up");
            NetlinkHangup();
            NetlinkRespond(NL_RESULTCODE_NOCARRIER);
         }
         else
            nl->call_timer = 0;
         break;
      }
      default:
         break;
   }
}

//////////////////////////////////////////////////////////////////////////////

static INLINE u32 NetlinkAddTime(u32 t, u32 timing)
{
   return (t > 0xFFFFFFFF - timing) ? 0xFFFFFFFF : t + timing;
}

//////////////////////////////////////////////////////////////////////////////

void NetlinkExec(u32 timing)
{
   Netlink *nl = NetlinkArea;
   u32 previdle;

   if (nl == NULL)
      return;

   if (NL_LOAD(&netlink_msg_head) != netlink_msg_tail)
      NetlinkFlushMessages();

   previdle = nl->thr_idle_time;
   nl->thr_idle_time = NetlinkAddTime(nl->thr_idle_time, timing);
   nl->call_timer = NetlinkAddTime(nl->call_timer, timing);
   nl->ring_timer = NetlinkAddTime(nl->ring_timer, timing);
   nl->irq_raise_time = NetlinkAddTime(nl->irq_raise_time, timing);

   // The byte written in the THR has been "sent": THR empty again
   if (previdle < NL_THRE_DELAY_US && nl->thr_idle_time >= NL_THRE_DELAY_US)
   {
      nl->thre_pending = 1;
      // New request only if the THR empty interrupt is enabled
      NetlinkUpdateIrq((nl->reg.IER & 0x02) != 0);
   }

   // End of an escape sequence (+++ followed by the guard time)
   if (nl->escape_count == 3 && nl->thr_idle_time >= nl->reg.SREG[12] * 20000u)
   {
      NETLINK_LOG("Detected escape sequence, switching back to command mode\n");
      nl->escape_count = 0;
      nl->modemstate = NL_MODEMSTATE_COMMAND;
      nl->cmdlen = 0;
      NetlinkRespond(NL_RESULTCODE_OK);
   }

   nl->cycles += timing;
   if (nl->cycles < NL_POLL_US)
      return;
   nl->cycles = 0;

   NetlinkRefreshAbusIrq();
   NetlinkPollCall();
}

//////////////////////////////////////////////////////////////////////////////
// Network thread

static void NetlinkRequest(int req)
{
   Netlink *nl = NetlinkArea;

   if (!nl->thread_started)
      return;

   // A new request always cancels a dial in progress
   NL_STORE(&nl->dial_abort, 1);
   NL_STORE(&nl->net_req, req);
   NL_STORE(&nl->net_req_id, nl->net_req_id + 1);
}

//////////////////////////////////////////////////////////////////////////////

#ifdef USESOCKET

static void NetlinkNetClose(Netlink *nl)
{
   if (nl->datasocket != YAB_SOCK_INVALID)
      YabSockCloseSocket(nl->datasocket);
   nl->datasocket = YAB_SOCK_INVALID;

   if (nl->udpsocket != YAB_SOCK_INVALID)
      YabSockCloseSocket(nl->udpsocket);
   nl->udpsocket = YAB_SOCK_INVALID;
   nl->net_mode = NL_MODE_TCP;
   nl->incoming_shiro = 0;

   // Pending transmit data belongs to the closed call
   RingFlush(&nl->tx);
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkNetStopListening(Netlink *nl)
{
   if (nl->listensocket != YAB_SOCK_INVALID)
      YabSockCloseSocket(nl->listensocket);
   nl->listensocket = YAB_SOCK_INVALID;

   if (nl->shirolisten != YAB_SOCK_INVALID)
      YabSockCloseSocket(nl->shirolisten);
   nl->shirolisten = YAB_SOCK_INVALID;
}

//////////////////////////////////////////////////////////////////////////////

// Returns 0 if the connection is still up
static int NetlinkNetTransfer(Netlink *nl)
{
   int readable, writable;
   u32 head, tail, count, off, chunk;
   int n;

   head = NL_LOAD(&nl->rxnet.head);
   tail = NL_LOAD(&nl->rxnet.tail);
   readable = (head - tail) < NETLINK_RING_SIZE;   // room to receive
   writable = RingCount(&nl->tx) != 0;            // something to send

   if (!readable && !writable)
   {
      YabThreadUSleep(1000);
      return 0;
   }

   if (YabSockPoll(nl->datasocket, 1, &readable, &writable) < 0)
      return -1;

   if (writable)
   {
      tail = nl->tx.tail;
      count = NL_LOAD(&nl->tx.head) - tail;
      off = tail & NL_RING_MASK;
      chunk = (count < NETLINK_RING_SIZE - off) ? count : NETLINK_RING_SIZE - off;

      if (chunk)
      {
         n = YabSockSend(nl->datasocket, &nl->tx.data[off], (int)chunk, 0);
         if (n > 0)
            NL_STORE(&nl->tx.tail, tail + (u32)n);
         else if (n < 0 && !YabSockWouldBlock())
            return -1;
      }
   }

   if (readable)
   {
      head = nl->rxnet.head;
      count = NETLINK_RING_SIZE - (head - NL_LOAD(&nl->rxnet.tail));
      off = head & NL_RING_MASK;
      chunk = (count < NETLINK_RING_SIZE - off) ? count : NETLINK_RING_SIZE - off;

      if (chunk)
      {
         n = YabSockReceive(nl->datasocket, &nl->rxnet.data[off], (int)chunk, 0);
         if (n > 0)
            NL_STORE(&nl->rxnet.head, head + (u32)n);
         else if (n == 0 || !YabSockWouldBlock())
            return -1; // closed by the peer or error
      }
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////
// DreamPi compatible link ("Shiro" protocol of DreamPi's netlink.py)

static const char shiro_packet_split[] = "<packetSplit>";
static const char shiro_data_split[] = "<dataSplit>";

#define NL_SHIRO_SILENCE_MS  15000   // no datagram (DreamPi pings every 1 s): line lost
#define NL_SHIRO_WAIT_MS     120000  // DreamPi waits 2 minutes for the opponent
#define NL_SHIRO_TCP_MS      20000   // DreamPi socket timeout of the handshake

// Monotonic clock in milliseconds (network thread timers)
static u32 NetlinkNowMs(void)
{
#ifdef _WIN32
   return (u32)GetTickCount();
#else
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (u32)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
#endif
}

//////////////////////////////////////////////////////////////////////////////

static const u8 *NetlinkMemFind(const u8 *buf, int len, const char *pat)
{
   int plen = (int)strlen(pat), i;

   for (i = 0; i + plen <= len; i++)
      if (memcmp(buf + i, pat, plen) == 0)
         return buf + i;

   return NULL;
}

//////////////////////////////////////////////////////////////////////////////

// Receives until 'len' bytes are read, the peer closes or timeout_ms elapses
static int NetlinkShiroRecvExact(Netlink *nl, YabSock s, char *buf, int len, int timeout_ms)
{
   int got = 0, waited = 0;

   while (got < len && waited < timeout_ms && !NL_LOAD(&nl->dial_abort))
   {
      int readable = 1, n;

      if (YabSockPoll(s, 50, &readable, NULL) < 0)
         return -1;
      waited += 50;
      if (!readable)
         continue;

      n = YabSockReceive(s, buf + got, len - got, 0);
      if (n <= 0)
      {
         if (n < 0 && YabSockWouldBlock())
            continue;
         return -1;
      }
      got += n;
   }

   return got;
}

//////////////////////////////////////////////////////////////////////////////

// Waiting side: TCP 65432, caller sends "readyip", we answer "g2gip".
// Returns 0 with nl->shiro_peer_ip set, -1 on timeout/abort/error.
static int NetlinkShiroHandshakeWait(Netlink *nl)
{
   YabSock listener, conn;
   int waited = 0, ret = -1;

   if (YabSockListenSocket(NETLINK_SHIRO_TCP_PORT, &listener) != 0)
   {
      NL_TINFO("DreamPi link: cannot listen on TCP port %d (in use?)", NETLINK_SHIRO_TCP_PORT);
      return -1;
   }

   while (ret != 0 && waited < NL_SHIRO_WAIT_MS && !NL_LOAD(&nl->dial_abort))
   {
      int readable = 1;
      char buf[8];

      if (YabSockPoll(listener, 50, &readable, NULL) <= 0 || !readable)
      {
         waited += 50;
         continue;
      }

      conn = YabSockAccept(listener);
      if (conn == YAB_SOCK_INVALID)
         continue;

      if (NetlinkShiroRecvExact(nl, conn, buf, 7, NL_SHIRO_TCP_MS) == 7 &&
          memcmp(buf, "readyip", 7) == 0 &&
          YabSockPeer4(conn, &nl->shiro_peer_ip) == 0 &&
          YabSockSend(conn, "g2gip", 5, 0) == 5)
      {
         ret = 0;
      }
      else
         NL_TINFO("DreamPi link: bad handshake, still waiting");

      YabSockCloseSocket(conn);
   }

   YabSockCloseSocket(listener);
   return ret;
}

//////////////////////////////////////////////////////////////////////////////

// Calling side: "readyip" to TCP 65432 of the opponent, expects "g2gip".
// Returns 0 on success, -2 if refused, -1 otherwise.
static int NetlinkShiroHandshakeCall(Netlink *nl)
{
   YabSock s;
   char buf[8];
   int ret;

   if (YabSockResolve4(nl->dialhost, &nl->shiro_peer_ip) != 0)
      return -1;

   ret = YabSockConnectTimeout(nl->dialhost, NETLINK_SHIRO_TCP_PORT, &s,
                               NL_SHIRO_TCP_MS, &nl->dial_abort);
   if (ret != 0)
      return ret;

   ret = -1;
   if (YabSockSend(s, "readyip", 7, 0) == 7 &&
       NetlinkShiroRecvExact(nl, s, buf, 5, NL_SHIRO_TCP_MS) == 5 &&
       memcmp(buf, "g2gip", 5) == 0)
      ret = 0;

   YabSockCloseSocket(s);
   return ret;
}

//////////////////////////////////////////////////////////////////////////////

// After the handshake: UDP socket and protocol state
static int NetlinkShiroOpen(Netlink *nl, int waiting)
{
   int localport = waiting ? NETLINK_SHIRO_UDP_WAIT : NETLINK_SHIRO_UDP_CALL;

   nl->shiro_peer_port = waiting ? NETLINK_SHIRO_UDP_CALL : NETLINK_SHIRO_UDP_WAIT;

   if (YabSockUdpOpen(localport, &nl->udpsocket) != 0)
   {
      NL_TINFO("DreamPi link: cannot open UDP port %d", localport);
      return -1;
   }

   nl->net_mode = NL_MODE_SHIRO;
   nl->shiro_txseq = 0;
   nl->shiro_rxseq = 0;
   nl->shiro_hist_count = 0;
   nl->shiro_ping_ms = NetlinkNowMs() - 1000; // first PING at once
   nl->shiro_silence_ms = NetlinkNowMs();

   // Helps NAT hole punching (DreamPi's sender does the same)
   YabSockSendTo4(nl->udpsocket, "OPEN_SHIRO", 10, nl->shiro_peer_ip, nl->shiro_peer_port);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkShiroDeliver(Netlink *nl, const u8 *data, int len)
{
   int i;

   for (i = 0; i < len; i++)
      if (!RingPush(&nl->rxnet, data[i]))
      {
         NETLINK_LOG("receive ring full, %d byte(s) dropped\n", len - i);
         break;
      }
}

//////////////////////////////////////////////////////////////////////////////

// Splits "payload<dataSplit>seq". Returns 0 if valid.
static int NetlinkShiroParsePacket(const u8 *p, int len, const u8 **payload, int *plen, u32 *seq)
{
   const u8 *sep = NetlinkMemFind(p, len, shiro_data_split);
   const u8 *s, *end = p + len;
   u32 val = 0;
   int digits = 0;

   if (sep == NULL)
      return -1;

   *payload = p;
   *plen = (int)(sep - p);

   // Python's split() ends the number at a second <dataSplit>
   s = sep + strlen(shiro_data_split);
   while (s < end && (*s == ' ' || *s == '\t'))
      s++;
   for (; s < end && *s >= '0' && *s <= '9'; s++, digits++)
      val = val * 10 + (u32)(*s - '0');
   while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
      s++;

   if (digits == 0 || (s < end && NetlinkMemFind(s, (int)(end - s), shiro_data_split) != s))
      return -1;

   *seq = val;
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

// Same logic as listener() of DreamPi's netlink.py: deliver the expected
// packet if it is in the datagram (recovering lost ones from the history),
// otherwise the newest one; drop data already delivered.
static void NetlinkShiroReceive(Netlink *nl, const u8 *buf, int len)
{
   const u8 *pkt[64];
   int pktlen[64];
   int count = 0, i;
   const u8 *p = buf, *end = buf + len;
   const int splitlen = (int)strlen(shiro_packet_split);

   while (count < 64)
   {
      const u8 *sep = NetlinkMemFind(p, (int)(end - p), shiro_packet_split);
      pkt[count] = p;
      pktlen[count] = sep ? (int)(sep - p) : (int)(end - p);
      count++;
      if (sep == NULL)
         break;
      p = sep + splitlen;
   }

   for (;;)
   {
      const u8 *payload;
      int plen, idx = count;
      u32 seq;

      for (i = 0; i < count; i++)
         if (NetlinkShiroParsePacket(pkt[i], pktlen[i], &payload, &plen, &seq) == 0 &&
             seq == nl->shiro_rxseq)
         {
            idx = i;
            break;
         }

      if (idx == count)
         idx = 0;

      if (NetlinkShiroParsePacket(pkt[idx], pktlen[idx], &payload, &plen, &seq) != 0)
         return;
      if (seq < nl->shiro_rxseq)
         return; // old data only

      nl->shiro_rxseq = seq + 1;
      NetlinkShiroDeliver(nl, payload, plen);

      if (idx == 0)
         return;
   }
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkShiroSend(Netlink *nl)
{
   char seqtxt[16];
   u8 dgram[NETLINK_SHIRO_HISTORY * (NETLINK_SHIRO_PAYLOAD + 32)];
   u32 tail = nl->tx.tail;
   u32 count = NL_LOAD(&nl->tx.head) - tail;
   int len = 0, i, n;

   if (count == 0)
      return;
   if (count > NETLINK_SHIRO_PAYLOAD)
      count = NETLINK_SHIRO_PAYLOAD;

   // New packet at the head of the history (DreamPi keeps 5)
   for (i = NETLINK_SHIRO_HISTORY - 1; i > 0; i--)
   {
      memcpy(nl->shiro_hist[i], nl->shiro_hist[i - 1], nl->shiro_hist_len[i - 1]);
      nl->shiro_hist_len[i] = nl->shiro_hist_len[i - 1];
      nl->shiro_hist_seq[i] = nl->shiro_hist_seq[i - 1];
   }
   for (i = 0; i < (int)count; i++)
      nl->shiro_hist[0][i] = nl->tx.data[(tail + i) & NL_RING_MASK];
   nl->shiro_hist_len[0] = (int)count;
   nl->shiro_hist_seq[0] = nl->shiro_txseq++;
   if (nl->shiro_hist_count < NETLINK_SHIRO_HISTORY)
      nl->shiro_hist_count++;
   NL_STORE(&nl->tx.tail, tail + count);

   for (i = 0; i < nl->shiro_hist_count; i++)
   {
      if (i > 0)
      {
         memcpy(dgram + len, shiro_packet_split, strlen(shiro_packet_split));
         len += (int)strlen(shiro_packet_split);
      }
      memcpy(dgram + len, nl->shiro_hist[i], nl->shiro_hist_len[i]);
      len += nl->shiro_hist_len[i];
      memcpy(dgram + len, shiro_data_split, strlen(shiro_data_split));
      len += (int)strlen(shiro_data_split);
      n = snprintf(seqtxt, sizeof(seqtxt), "%u", (unsigned)nl->shiro_hist_seq[i]);
      memcpy(dgram + len, seqtxt, n);
      len += n;
   }

   // DreamPi sends every datagram twice
   YabSockSendTo4(nl->udpsocket, dgram, len, nl->shiro_peer_ip, nl->shiro_peer_port);
   YabSockSendTo4(nl->udpsocket, dgram, len, nl->shiro_peer_ip, nl->shiro_peer_port);
}

//////////////////////////////////////////////////////////////////////////////

// Returns 0 while the link is up
static int NetlinkShiroTransfer(Netlink *nl)
{
   u8 buf[2048];
   int readable = 1, n;
   unsigned int ip;
   int port;

   YabSockPoll(nl->udpsocket, 1, &readable, NULL);

   while ((n = YabSockRecvFrom4(nl->udpsocket, buf, sizeof(buf), &ip, &port)) >= 0)
   {
      // Only the opponent (any port: NAT may change it)
      if (ip != nl->shiro_peer_ip)
         continue;

      nl->shiro_silence_ms = NetlinkNowMs();

      if (n == 10 && memcmp(buf, "PING_SHIRO", 10) == 0)
         YabSockSendTo4(nl->udpsocket, "PONG_SHIRO", 10, nl->shiro_peer_ip, nl->shiro_peer_port);
      else if (n == 10 && (memcmp(buf, "PONG_SHIRO", 10) == 0 || memcmp(buf, "OPEN_SHIRO", 10) == 0))
         ;
      else
         NetlinkShiroReceive(nl, buf, n);
   }

   if (NetlinkNowMs() - nl->shiro_ping_ms >= 1000)
   {
      nl->shiro_ping_ms = NetlinkNowMs();
      YabSockSendTo4(nl->udpsocket, "PING_SHIRO", 10, nl->shiro_peer_ip, nl->shiro_peer_port);
   }

   NetlinkShiroSend(nl);

   if (NetlinkNowMs() - nl->shiro_silence_ms >= NL_SHIRO_SILENCE_MS)
   {
      NL_TINFO("DreamPi link: nothing received for %d s, line lost", NL_SHIRO_SILENCE_MS / 1000);
      return -1;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void NetlinkShiroStart(Netlink *nl, u32 id, int waiting)
{
   int ret;

   NetlinkNetClose(nl);
   NetlinkNetStopListening(nl); // busy during the call
   NL_STORE(&nl->dial_abort, 0);
   NL_STORE(&nl->net_fail, NL_FAIL_NONE);
   NL_STORE(&nl->net_state, NL_NET_CONNECTING);
   NL_STORE(&nl->net_ack_id, id);

   ret = waiting ? NetlinkShiroHandshakeWait(nl) : NetlinkShiroHandshakeCall(nl);

   if (ret == 0 && !NL_LOAD(&nl->dial_abort) && NetlinkShiroOpen(nl, waiting) == 0)
   {
      NL_TINFO("DreamPi link: connected to %u.%u.%u.%u",
              (nl->shiro_peer_ip >> 24) & 0xFF, (nl->shiro_peer_ip >> 16) & 0xFF,
              (nl->shiro_peer_ip >> 8) & 0xFF, nl->shiro_peer_ip & 0xFF);

      NL_STORE(&nl->net_state, NL_NET_CONNECTED);
      return;
   }

   if (!NL_LOAD(&nl->dial_abort))
      NL_TINFO("DreamPi link: %s", waiting ? "nobody called" : "opponent not reachable (TCP 65432)");

   NetlinkNetClose(nl);
   NL_STORE(&nl->net_fail, ret == -2 ? NL_FAIL_BUSY : NL_FAIL_NOANSWER);
   NL_STORE(&nl->net_state, NL_NET_FAILED);
}

//////////////////////////////////////////////////////////////////////////////

// Kronos TCP link: sent by the side that answers the call
#define NL_ANSWER_BANNER     "KRONOS-NETLINK-ANSWER\n"
#define NL_ANSWER_BANNER_LEN (sizeof(NL_ANSWER_BANNER) - 1)

// Caller: waits (at most timeout_ms) until the called side answers.
// Returns 0 when answered, -1 on hang-up/timeout/abort.
static int NetlinkWaitAnswer(Netlink *nl, YabSock s, int timeout_ms)
{
   char buf[NL_ANSWER_BANNER_LEN];
   int got = NetlinkShiroRecvExact(nl, s, buf, (int)NL_ANSWER_BANNER_LEN,
                                   timeout_ms > 0 ? timeout_ms : 1);

   if (got == (int)NL_ANSWER_BANNER_LEN && memcmp(buf, NL_ANSWER_BANNER, NL_ANSWER_BANNER_LEN) == 0)
      return 0;

   if (got > 0)
   {
      // Peer without the answer banner (older Kronos build): what it sent
      // is data, the call is up
      int i;
      for (i = 0; i < got; i++)
         RingPush(&nl->rxnet, (u8)buf[i]);
      return 0;
   }

   return -1;
}

//////////////////////////////////////////////////////////////////////////////

static void *netlink_thread(void *data)
{
   Netlink *nl = (Netlink *)data;
   u32 handled = 0;
   int listen_retry_ms = 0;
   int listen_warned = 0;

   nl->listensocket = nl->datasocket = nl->udpsocket = nl->shirolisten = YAB_SOCK_INVALID;
   nl->net_mode = NL_MODE_TCP;

   while (!NL_LOAD(&nl->quit))
   {
      u32 id = NL_LOAD(&nl->net_req_id);
      int state;

      // Requests from the emulation thread
      if (id != handled)
      {
         int req = NL_LOAD(&nl->net_req);
         handled = id;

         switch (req)
         {
            case NL_REQ_DIAL:
            {
               YabSock s;
               int ret;

               NetlinkNetClose(nl);
               NetlinkNetStopListening(nl); // busy while dialing
               NL_STORE(&nl->dial_abort, 0);
               NL_STORE(&nl->net_fail, NL_FAIL_NONE);
               NL_STORE(&nl->net_state, NL_NET_CONNECTING);
               NL_STORE(&nl->net_ack_id, id);

               {
                  u32 start = NetlinkNowMs();

                  ret = YabSockConnectTimeout(nl->dialhost, nl->dialport, &s,
                                              nl->dialtimeout_ms, &nl->dial_abort);
                  if (ret == 0)
                  {
                     // The line is ringing at the other end: CONNECT only
                     // once it answers (S7 covers the whole call setup)
                     int left = nl->dialtimeout_ms - (int)(NetlinkNowMs() - start);

                     NL_TINFO("ringing at the other end");
                     if (NetlinkWaitAnswer(nl, s, left) != 0)
                     {
                        YabSockCloseSocket(s);
                        ret = -1;
                     }
                  }
               }

               if (ret == 0)
               {
                  nl->datasocket = s;
                  RingFlush(&nl->tx);
                  NL_STORE(&nl->net_state, NL_NET_CONNECTED);
               }
               else
               {
                  NL_STORE(&nl->net_fail, ret == -2 ? NL_FAIL_BUSY : NL_FAIL_NOANSWER);
                  NL_STORE(&nl->net_state, NL_NET_FAILED);
               }
               break;
            }
            case NL_REQ_ANSWER:
               if (NL_LOAD(&nl->net_state) == NL_NET_INCOMING && nl->incoming_shiro)
               {
                  // DreamPi caller: answer its handshake, data goes by UDP
                  int ok = YabSockSend(nl->datasocket, "g2gip", 5, 0) == 5;

                  YabSockCloseSocket(nl->datasocket);
                  nl->datasocket = YAB_SOCK_INVALID;
                  nl->incoming_shiro = 0;
                  RingFlush(&nl->tx);

                  if (ok && NetlinkShiroOpen(nl, 1) == 0)
                  {
                     NL_TINFO("DreamPi link: connected to %u.%u.%u.%u",
                              (nl->shiro_peer_ip >> 24) & 0xFF, (nl->shiro_peer_ip >> 16) & 0xFF,
                              (nl->shiro_peer_ip >> 8) & 0xFF, nl->shiro_peer_ip & 0xFF);
                     NL_STORE(&nl->net_state, NL_NET_CONNECTED);
                  }
                  else
                  {
                     NetlinkNetClose(nl);
                     NL_STORE(&nl->net_state, NL_NET_CLOSED);
                  }
               }
               else if (NL_LOAD(&nl->net_state) == NL_NET_INCOMING)
               {
                  RingFlush(&nl->tx);
                  // Tells the caller that the call is answered
                  if (YabSockSend(nl->datasocket, NL_ANSWER_BANNER, (int)NL_ANSWER_BANNER_LEN, 0) != (int)NL_ANSWER_BANNER_LEN)
                  {
                     NetlinkNetClose(nl);
                     NL_STORE(&nl->net_state, NL_NET_CLOSED);
                  }
                  else
                     NL_STORE(&nl->net_state, NL_NET_CONNECTED);
               }
               break;
            case NL_REQ_HANGUP:
               NetlinkNetClose(nl);
               NL_STORE(&nl->net_state, NL_NET_IDLE);
               break;
            case NL_REQ_SHIRO_WAIT:
            case NL_REQ_SHIRO_CALL:
               NetlinkShiroStart(nl, id, req == NL_REQ_SHIRO_WAIT);
               break;
            default:
               break;
         }

         NL_STORE(&nl->net_ack_id, id);
         continue;
      }

      state = NL_LOAD(&nl->net_state);

      switch (state)
      {
         case NL_NET_IDLE:
         {
            int readable = 1;

            // DreamPi callers (TCP 65432) ring the game too
            if (nl->shirolisten == YAB_SOCK_INVALID)
            {
               if (nl->shirolisten_retry_ms <= 0)
               {
                  if (YabSockListenSocket(NETLINK_SHIRO_TCP_PORT, &nl->shirolisten) == 0)
                  {
                     if (nl->shirolisten_warned != 1)
                        NL_TINFO("waiting for DreamPi calls on TCP port %d", NETLINK_SHIRO_TCP_PORT);
                     nl->shirolisten_warned = 1;
                  }
                  else
                  {
                     if (nl->shirolisten_warned != 2)
                        NL_TINFO("cannot listen on TCP port %d, DreamPi calls disabled", NETLINK_SHIRO_TCP_PORT);
                     nl->shirolisten_warned = 2;
                     nl->shirolisten_retry_ms = 5000;
                  }
               }
               else
                  nl->shirolisten_retry_ms -= 10;
            }

            if (nl->shirolisten != YAB_SOCK_INVALID)
            {
               int r = 1;

               if (YabSockPoll(nl->shirolisten, 0, &r, NULL) > 0 && r)
               {
                  YabSock s = YabSockAccept(nl->shirolisten);
                  char buf[8];

                  if (s != YAB_SOCK_INVALID)
                  {
                     if (NetlinkShiroRecvExact(nl, s, buf, 7, 2000) == 7 &&
                         memcmp(buf, "readyip", 7) == 0 &&
                         YabSockPeer4(s, &nl->shiro_peer_ip) == 0)
                     {
                        nl->datasocket = s;
                        nl->incoming_shiro = 1;
                        NetlinkNetStopListening(nl);
                        NL_STORE(&nl->net_state, NL_NET_INCOMING);
                        break;
                     }
                     YabSockCloseSocket(s);
                  }
               }
            }

            // (Re)open the listening socket. It is closed during a call so
            // that a second caller gets "connection refused" (= BUSY).
            if (nl->listensocket == YAB_SOCK_INVALID)
            {
               if (listen_retry_ms <= 0)
               {
                  if (YabSockListenSocket(nl->listenport, &nl->listensocket) == 0)
                  {
                     if (listen_warned != 1)
                        NL_TINFO("waiting for calls on TCP port %d", nl->listenport);
                     listen_warned = 1;
                  }
                  else
                  {
                     if (listen_warned != 2)
                        NL_TINFO("cannot listen on TCP port %d (in use?), incoming calls disabled, dialing still works",
                                nl->listenport);
                     listen_warned = 2;
                     listen_retry_ms = 5000;
                  }
               }
               else
               {
                  YabThreadUSleep(10000);
                  listen_retry_ms -= 10;
                  break;
               }
            }

            if (nl->listensocket == YAB_SOCK_INVALID)
               break;

            if (YabSockPoll(nl->listensocket, 10, &readable, NULL) > 0 && readable)
            {
               YabSock s = YabSockAccept(nl->listensocket);

               if (s != YAB_SOCK_INVALID)
               {
                  YabSockSetNoDelay(s);
                  nl->datasocket = s;
                  NetlinkNetStopListening(nl);
                  NL_STORE(&nl->net_state, NL_NET_INCOMING);
               }
            }
            break;
         }
         case NL_NET_INCOMING:
         {
            int readable = 1;

            // Has the caller hung up before we answered?
            if (YabSockPoll(nl->datasocket, 10, &readable, NULL) > 0 && readable)
            {
               u8 peek;
               int n = YabSockReceive(nl->datasocket, &peek, 1, YAB_SOCK_PEEK);

               if (n == 0 || (n < 0 && !YabSockWouldBlock()))
               {
                  NetlinkNetClose(nl);
                  NL_STORE(&nl->net_state, NL_NET_CLOSED);
               }
               else
                  YabThreadUSleep(10000); // data already waiting: keep it
            }
            break;
         }
         case NL_NET_CONNECTED:
            if ((nl->net_mode == NL_MODE_SHIRO ? NetlinkShiroTransfer(nl) : NetlinkNetTransfer(nl)) != 0)
            {
               NetlinkNetClose(nl);
               NL_STORE(&nl->net_state, NL_NET_CLOSED);
            }
            break;
         default:
            // FAILED/CLOSED: wait for the emulation thread to hang up
            YabThreadUSleep(5000);
            break;
      }
   }

   NetlinkNetClose(nl);
   NetlinkNetStopListening(nl);
   return NULL;
}

#endif

//////////////////////////////////////////////////////////////////////////////
// Init

void NetlinkSetConfig(const char *ip, const char *port)
{
   if (ip != NULL)
   {
      while (*ip == ' ')
         ip++;
   }

   if (ip == NULL || *ip == '\0')
      snprintf(netlink_cfg_host, sizeof(netlink_cfg_host), "127.0.0.1");
   else
   {
      char *end;

      snprintf(netlink_cfg_host, sizeof(netlink_cfg_host), "%s", ip);
      for (end = netlink_cfg_host + strlen(netlink_cfg_host);
           end > netlink_cfg_host && end[-1] == ' '; end--)
         end[-1] = '\0';
   }

   netlink_cfg_port = NETLINK_DEFAULT_PORT;
   if (port != NULL && *port != '\0')
   {
      long p = strtol(port, NULL, 10);

      if (p > 0 && p < 65536)
         netlink_cfg_port = (int)p;
      else
         NL_INFO("invalid port \"%s\", using %d", port, NETLINK_DEFAULT_PORT);
   }
}

//////////////////////////////////////////////////////////////////////////////

int NetlinkInit(const char *ip, const char *port)
{
   Netlink *nl;

   if (ip != NULL || port != NULL)
      NetlinkSetConfig(ip, port);

   if (NetlinkArea != NULL)
      NetlinkDeInit();

   if ((nl = (Netlink *)calloc(1, sizeof(Netlink))) == NULL)
   {
      YabSetError(YAB_ERR_CANNOTINIT, (void *)"Netlink");
      return -1;
   }
   NetlinkArea = nl;

   nl->reg.RBR = 0x00;
   nl->reg.IER = 0x00;
   nl->reg.DLL = 0x00;
   nl->reg.DLM = 0x00;
   nl->reg.IIR = 0x01;
   nl->reg.FCR = 0x00;
   nl->reg.LCR = 0x00;
   nl->reg.MCR = 0x00;
   nl->reg.LSR = 0x60;
   nl->reg.MSR = 0x30;
   nl->reg.SCR = 0x01;

   NetlinkDefaults();

   nl->modemstate = NL_MODEMSTATE_COMMAND;
   nl->callstate = NL_CALL_ONHOOK;
   nl->thre_pending = 1;
   nl->thr_idle_time = 0xFFFFFFFF;
   nl->msr_lines = 0x30;

   snprintf(nl->defaulthost, sizeof(nl->defaulthost), "%s", netlink_cfg_host);
   nl->defaultport = netlink_cfg_port;
   nl->listenport = netlink_cfg_port;

   nl->listensocket = nl->datasocket = nl->udpsocket = nl->shirolisten = YAB_SOCK_INVALID;
   nl->net_state = NL_NET_IDLE;

#ifdef USESOCKET
   if (YabSockInit() != 0)
      NL_INFO("socket initialisation failed, network disabled");
   else if (YabThreadStart(YAB_THREAD_NETLINKCLIENT, netlink_thread, nl) != 0)
   {
      NL_INFO("cannot start the network thread, network disabled");
      YabSockDeInit();
   }
   else
   {
      nl->thread_started = 1;
      NL_INFO("modem ready, default peer %s port %d", nl->defaulthost, nl->defaultport);
   }
#else
   NL_INFO("built without network support (YAB_NETWORK/USESOCKET): AT commands only");
#endif

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

void NetlinkDeInit(void)
{
   Netlink *nl = NetlinkArea;

   if (nl == NULL)
      return;

#ifdef USESOCKET
   if (nl->thread_started)
   {
      NL_STORE(&nl->dial_abort, 1);
      NL_STORE(&nl->quit, 1);
      YabThreadWait(YAB_THREAD_NETLINKCLIENT);
      YabSockDeInit();
      nl->thread_started = 0;
   }
#endif

   NetlinkFlushMessages();

   NetlinkArea = NULL;
   free(nl);
}

//////////////////////////////////////////////////////////////////////////////
