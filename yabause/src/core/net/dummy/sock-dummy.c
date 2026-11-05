/*  Copyright 2013 Theo Berkau

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

/*! \file sock-dummy.c
    \brief Socket interface for platforms without network support: every
           call fails cleanly.
*/

#include "core.h"
#include "sock.h"

//////////////////////////////////////////////////////////////////////////////

int YabSockInit() { return -1; }

int YabSockDeInit() { return -1; }

int YabSockConnectSocket(const char *ip, int port, YabSock *sock) { *sock = YAB_SOCK_INVALID; return -1; }

int YabSockConnectTimeout(const char *host, int port, YabSock *sock, int timeout_ms, volatile int *abort_flag) { *sock = YAB_SOCK_INVALID; return -1; }

int YabSockListenSocket(int port, YabSock *sock) { *sock = YAB_SOCK_INVALID; return -1; }

int YabSockCloseSocket(YabSock sock) { return -1; }

int YabSockSelect(YabSock sock, int check_read, int check_write ) { return -1; }

int YabSockIsReadSet(YabSock sock) { return 0; }

int YabSockIsWriteSet(YabSock sock) { return 0; }

int YabSockPoll(YabSock sock, int timeout_ms, int *readable, int *writable)
{
   if (readable) *readable = 0;
   if (writable) *writable = 0;
   return -1;
}

// 0 is a valid descriptor: an invalid socket must be returned here
YabSock YabSockAccept(YabSock sock) { return YAB_SOCK_INVALID; }

int YabSockSetNoDelay(YabSock sock) { return -1; }

int YabSockSend(YabSock sock, const void *buf, int len, int flags)  { return -1; }

int YabSockReceive(YabSock sock, void *buf, int len, int flags) { return -1; }

int YabSockWouldBlock(void) { return 0; }

int YabSockResolve4(const char *host, unsigned int *ip) { return -1; }

int YabSockPeer4(YabSock sock, unsigned int *ip) { return -1; }

int YabSockUdpOpen(int port, YabSock *sock) { *sock = YAB_SOCK_INVALID; return -1; }

int YabSockSendTo4(YabSock sock, const void *buf, int len, unsigned int ip, int port) { return -1; }

int YabSockRecvFrom4(YabSock sock, void *buf, int len, unsigned int *ip, int *port) { return -1; }

//////////////////////////////////////////////////////////////////////////////
