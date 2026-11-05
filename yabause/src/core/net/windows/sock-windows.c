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

/*! \file sock-windows.c
    \brief Windows port's socket interface
*/

#include "core.h"
#include "sock.h"
#include "debug.h"

#ifdef __MINGW32__
// I blame mingw for this
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x501
#endif
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <stdio.h>
#include <string.h>

static fd_set read_fds;
static fd_set write_fds;

// WSAStartup/WSACleanup are reference counted: keep our own count so that
// several users (NetLink, gdb stub...) can init/deinit independently.
static volatile LONG wsa_refcount = 0;

//////////////////////////////////////////////////////////////////////////////

int YabSockInit()
{
   WSADATA wsaData;

   if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
      return -1;

   InterlockedIncrement(&wsa_refcount);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockDeInit()
{
   if (InterlockedCompareExchange(&wsa_refcount, 0, 0) > 0)
   {
      InterlockedDecrement(&wsa_refcount);
      WSACleanup();
   }
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockConnectSocket(const char *ip, int port, YabSock *sock)
{
   struct addrinfo *result = NULL, hints;
   char port_str[16];

   memset(&hints, 0, sizeof(hints));

   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;

   _snprintf(port_str, sizeof(port_str), "%d", port);
   port_str[sizeof(port_str)-1] = '\0';
   if (getaddrinfo(ip, port_str, &hints, &result) != 0)
   {
      LOG("getaddrinfo: err code %d\n", WSAGetLastError());
      return -1;
   }

   // Create a Socket
   if ((sock[0] = (YabSock)socket(result->ai_family, result->ai_socktype,
      result->ai_protocol)) == YAB_SOCK_INVALID)
   {
      freeaddrinfo(result);
      LOG("socket: err code %d\n", WSAGetLastError());
      return -1;
   }

   // Connect to the socket
   if (connect(sock[0], result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
   {
      LOG("connect: err code %d\n", WSAGetLastError());
      freeaddrinfo(result);
      closesocket(sock[0]);
      sock[0] = YAB_SOCK_INVALID;
      return -1;
   }

   freeaddrinfo(result);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockConnectTimeout(const char *host, int port, YabSock *sock,
                          int timeout_ms, volatile int *abort_flag)
{
   struct addrinfo hints;
   struct addrinfo *result = NULL, *rp;
   char port_str[16];
   int ret = -1;
   int refused = 0;

   *sock = YAB_SOCK_INVALID;

   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;

   _snprintf(port_str, sizeof(port_str), "%d", port);
   port_str[sizeof(port_str)-1] = '\0';
   if (getaddrinfo(host, port_str, &hints, &result) != 0 || result == NULL)
      return -1;

   for (rp = result; rp != NULL && ret != 0; rp = rp->ai_next)
   {
      SOCKET s;
      u_long nonblock = 1;
      int waited = 0;

      if (abort_flag && *abort_flag)
         break;

      s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (s == INVALID_SOCKET)
         continue;

      if (ioctlsocket(s, FIONBIO, &nonblock) != 0)
      {
         closesocket(s);
         continue;
      }

      if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0)
         ret = 0;
      else if (WSAGetLastError() == WSAEWOULDBLOCK)
      {
         while (waited < timeout_ms)
         {
            fd_set wfds, efds;
            struct timeval tv;
            int slice = timeout_ms - waited;
            int r;

            if (abort_flag && *abort_flag)
               break;

            if (slice > 50)
               slice = 50;

            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds); // a failed connect is signalled here on Windows
            tv.tv_sec = 0;
            tv.tv_usec = slice * 1000;

            r = select(0, NULL, &wfds, &efds, &tv);
            if (r == SOCKET_ERROR)
               break;

            if (r > 0)
            {
               int err = 0;
               int len = sizeof(err);

               getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
               if (FD_ISSET(s, &wfds) && err == 0)
                  ret = 0;
               else if (err == WSAECONNREFUSED)
                  refused = 1;
               break;
            }

            waited += slice;
         }
      }
      else if (WSAGetLastError() == WSAECONNREFUSED)
         refused = 1;

      if (ret == 0)
      {
         nonblock = 0;
         ioctlsocket(s, FIONBIO, &nonblock); // back to blocking mode
         *sock = (YabSock)s;
         YabSockSetNoDelay(*sock);
      }
      else
         closesocket(s);
   }

   freeaddrinfo(result);

   if (ret != 0 && refused)
      return -2;

   return ret;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockListenSocket(int port, YabSock *sock)
{
   struct sockaddr_in addr;
   BOOL opt = TRUE;

   sock[0] = (YabSock)socket(AF_INET, SOCK_STREAM, 0);
   if (sock[0] == YAB_SOCK_INVALID)
   {
      LOG("socket: err code %d\n", WSAGetLastError());
      return -1;
   }

   // On Windows SO_REUSEADDR lets another process steal the port; use
   // SO_EXCLUSIVEADDRUSE like recommended by Microsoft instead.
#ifdef SO_EXCLUSIVEADDRUSE
   setsockopt(sock[0], SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&opt, sizeof(opt));
#else
   setsockopt(sock[0], SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#endif

   memset(&addr, 0, sizeof(struct sockaddr_in));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = INADDR_ANY;
   addr.sin_port = htons((u_short)port);
   if (bind(sock[0], (struct sockaddr *) &addr, sizeof(addr)) == SOCKET_ERROR)
   {
      fprintf(stderr, "Can't bind to port %d: err code %d\n", port, WSAGetLastError());
      closesocket(sock[0]);
      sock[0] = YAB_SOCK_INVALID;
      return -1;
   }

   if (listen(sock[0], 3) == SOCKET_ERROR)
   {
      LOG("listen: err code %d\n", WSAGetLastError());
      closesocket(sock[0]);
      sock[0] = YAB_SOCK_INVALID;
      return -1;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockCloseSocket(YabSock sock)
{
   if (sock == YAB_SOCK_INVALID)
      return -1;
   shutdown(sock, SD_BOTH);
   return closesocket(sock);
}

//////////////////////////////////////////////////////////////////////////////

int YabSockSelect(YabSock sock, int check_read, int check_write )
{
   fd_set *read_fds_ptr;
   fd_set *write_fds_ptr;
   struct timeval tv;
   int ret;

   FD_ZERO(&read_fds);
   FD_ZERO(&write_fds);

   // Let's see if we can even connect at this point
   if (check_read)
   {
      FD_SET(sock, &read_fds);
      read_fds_ptr = &read_fds;
   }
   else
      read_fds_ptr = NULL;

   if (check_write)
   {
      FD_SET(sock, &write_fds);
      write_fds_ptr = &write_fds;
   }
   else
      write_fds_ptr = NULL;

   tv.tv_sec = 0;
   tv.tv_usec = 0;

   if ((ret=select(sock+1, read_fds_ptr, write_fds_ptr, NULL, &tv)) < 1)
   {
      if (ret == SOCKET_ERROR)
         LOG("select: err code %d\n", WSAGetLastError());
      else
         LOG("select: %d\n", ret);
      return -1;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockIsReadSet(YabSock sock)
{
   return FD_ISSET(sock, &read_fds);
}

//////////////////////////////////////////////////////////////////////////////

int YabSockIsWriteSet(YabSock sock)
{
   return FD_ISSET(sock, &write_fds);
}

//////////////////////////////////////////////////////////////////////////////

int YabSockPoll(YabSock sock, int timeout_ms, int *readable, int *writable)
{
   fd_set rfds, wfds, efds;
   struct timeval tv;
   int want_read = (readable && *readable);
   int want_write = (writable && *writable);
   int r;

   if (readable) *readable = 0;
   if (writable) *writable = 0;

   if (sock == YAB_SOCK_INVALID)
      return -1;

   if (!want_read && !want_write)
   {
      Sleep(timeout_ms > 0 ? timeout_ms : 0);
      return 0;
   }

   FD_ZERO(&rfds);
   FD_ZERO(&wfds);
   FD_ZERO(&efds);
   if (want_read)
      FD_SET(sock, &rfds);
   if (want_write)
      FD_SET(sock, &wfds);
   FD_SET(sock, &efds);

   tv.tv_sec = timeout_ms / 1000;
   tv.tv_usec = (timeout_ms % 1000) * 1000;
   if (timeout_ms < 0)
      tv.tv_sec = tv.tv_usec = 0;

   r = select(0, &rfds, &wfds, &efds, &tv);
   if (r == SOCKET_ERROR)
      return -1;
   if (r == 0)
      return 0;

   if (readable && (FD_ISSET(sock, &rfds) || FD_ISSET(sock, &efds)))
      *readable = 1;
   if (writable && FD_ISSET(sock, &wfds))
      *writable = 1;

   return 1;
}

//////////////////////////////////////////////////////////////////////////////

YabSock YabSockAccept(YabSock sock)
{
   SOCKET s = accept(sock,NULL,NULL);
   if (s == INVALID_SOCKET)
      return YAB_SOCK_INVALID;
   return (YabSock)s;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockSetNoDelay(YabSock sock)
{
   BOOL one = TRUE;
   return setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one)) == 0 ? 0 : -1;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockSend(YabSock sock, const void *buf, int len, int flags)
{
   return send(sock, (const char*)buf, len, flags);
}

//////////////////////////////////////////////////////////////////////////////

int YabSockReceive(YabSock sock, void *buf, int len, int flags)
{
   int sysflags = (flags & YAB_SOCK_PEEK) ? MSG_PEEK : 0;
   return recv(sock, (char *)buf, len, sysflags);
}

//////////////////////////////////////////////////////////////////////////////

int YabSockWouldBlock(void)
{
   int err = WSAGetLastError();
   return err == WSAEWOULDBLOCK || err == WSAEINTR;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockResolve4(const char *host, unsigned int *ip)
{
   struct addrinfo hints, *result = NULL;

   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_INET;
   hints.ai_socktype = SOCK_DGRAM;

   if (getaddrinfo(host, NULL, &hints, &result) != 0 || result == NULL)
      return -1;

   *ip = ntohl(((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr);
   freeaddrinfo(result);
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockPeer4(YabSock sock, unsigned int *ip)
{
   struct sockaddr_storage ss;
   int len = sizeof(ss);

   if (getpeername(sock, (struct sockaddr *)&ss, &len) != 0)
      return -1;

   if (ss.ss_family == AF_INET)
   {
      *ip = ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
      return 0;
   }
#ifdef AF_INET6
   if (ss.ss_family == AF_INET6)
   {
      // IPv4-mapped IPv6 address (::ffff:a.b.c.d)
      const unsigned char *a = ((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr;
      static const unsigned char prefix[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
      if (memcmp(a, prefix, 12) == 0)
      {
         *ip = ((unsigned int)a[12] << 24) | ((unsigned int)a[13] << 16) |
               ((unsigned int)a[14] << 8) | a[15];
         return 0;
      }
   }
#endif
   return -1;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockSendTo4(YabSock sock, const void *buf, int len, unsigned int ip, int port)
{
   struct sockaddr_in addr;

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(ip);
   addr.sin_port = htons((unsigned short)port);

   return (int)sendto(sock, (const char *)buf, len, 0, (struct sockaddr *)&addr, sizeof(addr));
}

//////////////////////////////////////////////////////////////////////////////

int YabSockRecvFrom4(YabSock sock, void *buf, int len, unsigned int *ip, int *port)
{
   struct sockaddr_in addr;
   int alen = sizeof(addr);
   int n;

   memset(&addr, 0, sizeof(addr));
   n = (int)recvfrom(sock, (char *)buf, len, 0, (struct sockaddr *)&addr, &alen);
   if (n < 0)
      return -1;

   if (ip)
      *ip = ntohl(addr.sin_addr.s_addr);
   if (port)
      *port = ntohs(addr.sin_port);
   return n;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockUdpOpen(int port, YabSock *sock)
{
   struct sockaddr_in addr;
   u_long nonblock = 1;
   SOCKET s;

   *sock = YAB_SOCK_INVALID;
   s = socket(AF_INET, SOCK_DGRAM, 0);
   if (s == INVALID_SOCKET)
      return -1;

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   addr.sin_port = htons((u_short)port);

   if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
       ioctlsocket(s, FIONBIO, &nonblock) != 0)
   {
      fprintf(stderr, "Can't bind UDP port %d: err code %d\n", port, WSAGetLastError());
      closesocket(s);
      return -1;
   }

#ifdef SIO_UDP_CONNRESET
   {
      // Don't report ICMP "port unreachable" as an error on the next
      // receive (the peer is not listening yet)
      BOOL off = FALSE;
      DWORD ret = 0;
      WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
   }
#endif

   *sock = (YabSock)s;
   return 0;
}

//////////////////////////////////////////////////////////////////////////////
