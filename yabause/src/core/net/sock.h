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

#ifndef SOCK_H
#define SOCK_H

typedef int YabSock;

// Value of an invalid / closed socket.
#define YAB_SOCK_INVALID   (-1)

// Flag for YabSockReceive: look at the data without removing it from the
// queue (translated to MSG_PEEK by each backend).
#define YAB_SOCK_PEEK      0x2

// YabSockInit: Initializes socket code.
// Returns 0 on success. -1 on error.
int YabSockInit();

// YabSockDeInit: DeInitialize/frees socket code.
// Returns 0 on success. -1 on error.
int YabSockDeInit();

// YabSockConnectSocket: Attempts to connect to specified ip and port
// (blocking connect, kept for compatibility).
// Returns 0 on success. -1 on error.
int YabSockConnectSocket(const char *ip, int port, YabSock *sock);

// YabSockConnectTimeout: non-blocking connect to host:port (IPv4, IPv6 or
// host name), trying every address returned by the resolver. Gives up after
// timeout_ms (per address) or as soon as *abort_flag becomes non zero
// (abort_flag may be NULL). On success the socket is left in blocking mode
// with TCP_NODELAY set.
// Returns 0 on success, -1 on error, timeout or abort, -2 if the remote host
// actively refused the connection.
int YabSockConnectTimeout(const char *host, int port, YabSock *sock,
                          int timeout_ms, volatile int *abort_flag);

// YabSockListenSocket: Listen for connection attempts on specified port.
// Returns 0 on success. -1 on error (*sock is then YAB_SOCK_INVALID).
int YabSockListenSocket(int port, YabSock *sock);

// YabSockCloseSocket: Closes previously opened socket.
// Returns 0 on success. -1 on error.
int YabSockCloseSocket(YabSock sock);

// YabSockSelect: Determines the status of one or more sockets.
// Not thread-safe (shared fd sets): prefer YabSockPoll.
// Returns 0 on success. -1 on error.
int YabSockSelect(YabSock sock, int check_read, int check_write);

// YabSockIsReadSet: Is socket's read flag set (after YabSockSelect)
// Returns 1 on true. 0 on false.
int YabSockIsReadSet(YabSock sock);

// YabSockIsWriteSet: Is socket's write flag set (after YabSockSelect)
// Returns 1 on true. 0 on false.
int YabSockIsWriteSet(YabSock sock);

// YabSockPoll: thread-safe wait (at most timeout_ms, 0 = no wait) for the
// socket to become readable and/or writable. On input *readable / *writable
// say which conditions to check (pointer may be NULL = don't check); on
// output they hold the result. A socket in error or closed by the peer is
// reported readable (the following receive returns 0 or -1).
// Returns 1 if a condition is set, 0 on timeout, -1 on error.
int YabSockPoll(YabSock sock, int timeout_ms, int *readable, int *writable);

// YabSockAccept: Accept connection from socket.
// Returns opened connected socket, YAB_SOCK_INVALID on error.
YabSock YabSockAccept(YabSock sock);

// YabSockSetNoDelay: disable Nagle's algorithm (low latency link play).
// Returns 0 on success. -1 on error.
int YabSockSetNoDelay(YabSock sock);

// YabSockSend: Sends data via specified socket. Never raises SIGPIPE.
// Returns the number of bytes sent, -1 on error.
int YabSockSend(YabSock sock, const void *buf, int len, int flags);

// YabSockReceive: Receive data via specified socket
// Returns the number of bytes received, 0 if the peer closed the
// connection, -1 on error.
int YabSockReceive(YabSock sock, void *buf, int len, int flags);

// YabSockWouldBlock: non zero if the last send/receive error of the calling
// thread only means "try again later" (EAGAIN/EWOULDBLOCK/EINTR).
int YabSockWouldBlock(void);

// IPv4 helpers (addresses in host byte order), used for UDP links.

// YabSockResolve4: IPv4 address of host (dotted address or host name).
// Returns 0 on success. -1 on error.
int YabSockResolve4(const char *host, unsigned int *ip);

// YabSockPeer4: IPv4 address of the remote end of a connected TCP socket.
// Returns 0 on success. -1 on error.
int YabSockPeer4(YabSock sock, unsigned int *ip);

// YabSockUdpOpen: non-blocking UDP socket bound to port on every IPv4
// interface (port 0 = any free port).
// Returns 0 on success. -1 on error (*sock is then YAB_SOCK_INVALID).
int YabSockUdpOpen(int port, YabSock *sock);

// YabSockSendTo4: send one datagram to ip:port.
// Returns the number of bytes sent, -1 on error.
int YabSockSendTo4(YabSock sock, const void *buf, int len, unsigned int ip, int port);

// YabSockRecvFrom4: receive one datagram (non-blocking). ip/port may be NULL.
// Returns its size, -1 when nothing is waiting or on error (an ICMP "port
// unreachable" reported by Windows is also returned as -1).
int YabSockRecvFrom4(YabSock sock, void *buf, int len, unsigned int *ip, int *port);

#endif  // SOCK_H
