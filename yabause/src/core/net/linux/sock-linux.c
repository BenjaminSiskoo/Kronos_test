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

/*! \file sock-linux.c
    \brief POSIX socket interface (Linux, Android, macOS).
*/

#include "core.h"
#include "sock.h"
#include "debug.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

// Writing to a socket closed by the peer raises SIGPIPE, whose default
// action terminates the whole emulator. Linux has a per-call flag, macOS a
// per-socket option.
#ifdef MSG_NOSIGNAL
#define YAB_SEND_FLAGS MSG_NOSIGNAL
#else
#define YAB_SEND_FLAGS 0
#endif

static fd_set read_fds;
static fd_set write_fds;

//////////////////////////////////////////////////////////////////////////////

static void YabSockNoSigPipe(int s)
{
#ifdef SO_NOSIGPIPE
   int one = 1;
   setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
   (void)s;
#endif
}

//////////////////////////////////////////////////////////////////////////////

int YabSockInit()
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockDeInit()
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockConnectSocket(const char *ip, int port, YabSock *sock)
{
   struct addrinfo *result = NULL;
   struct addrinfo hints;
   char port_str[16];

   memset(&hints, 0, sizeof(hints));

   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;

   snprintf(port_str, sizeof(port_str), "%d", port);
   if (getaddrinfo(ip, port_str, &hints, &result) != 0)
   {
      perror("getaddrinfo");
      return -1;
   }

   // Create a Socket
   if ((sock[0] = socket(result->ai_family, result->ai_socktype,
      result->ai_protocol)) == -1)
   {
      freeaddrinfo(result);
      perror("socket");
      return -1;
   }

   YabSockNoSigPipe(sock[0]);

   // Connect to the socket
   if (connect(sock[0], result->ai_addr, result->ai_addrlen) == -1)
   {
      perror("connect");
      freeaddrinfo(result);
      close(sock[0]);
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

   snprintf(port_str, sizeof(port_str), "%d", port);
   if (getaddrinfo(host, port_str, &hints, &result) != 0 || result == NULL)
      return -1;

   for (rp = result; rp != NULL && ret != 0; rp = rp->ai_next)
   {
      int s, flags, waited = 0;

      if (abort_flag && *abort_flag)
         break;

      if ((s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) == -1)
         continue;

      YabSockNoSigPipe(s);

      flags = fcntl(s, F_GETFL, 0);
      if (flags == -1 || fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1)
      {
         close(s);
         continue;
      }

      if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0)
         ret = 0;
      else if (errno == EINPROGRESS || errno == EINTR)
      {
         // Wait by small slices so that the caller can cancel the attempt
         while (waited < timeout_ms)
         {
            struct pollfd pfd;
            int slice = timeout_ms - waited;
            int r;

            if (abort_flag && *abort_flag)
               break;

            if (slice > 50)
               slice = 50;

            pfd.fd = s;
            pfd.events = POLLOUT;
            pfd.revents = 0;

            r = poll(&pfd, 1, slice);
            if (r < 0 && errno != EINTR)
               break;

            if (r > 0)
            {
               int err = 0;
               socklen_t len = sizeof(err);

               if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                  ret = 0;
               else if (err == ECONNREFUSED)
                  refused = 1;
               break;
            }

            waited += slice;
         }
      }
      else if (errno == ECONNREFUSED)
         refused = 1;

      if (ret == 0)
      {
         fcntl(s, F_SETFL, flags); // back to blocking mode
         YabSockSetNoDelay(s);
         *sock = s;
      }
      else
         close(s);
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
   int opt = 1; // setsockopt reads an int: a char here was undefined behaviour

   sock[0] = socket(AF_INET, SOCK_STREAM, 0);
   if (sock[0] == -1)
   {
      perror("socket");
      return -1;
   }

   if (setsockopt(sock[0], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
   {
      perror("setsockopt");
      close(sock[0]);
      sock[0] = YAB_SOCK_INVALID;
      return -1;
   }

   memset(&addr, 0, sizeof(struct sockaddr_in));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = INADDR_ANY;
   addr.sin_port = htons(port);
   if (bind(sock[0], (struct sockaddr *) &addr, sizeof(addr)) == -1)
   {
      fprintf(stderr, "Can't bind to port %d: %s\n", port, strerror(errno));
      close(sock[0]);
      sock[0] = YAB_SOCK_INVALID;
      return -1;
   }

   if (listen(sock[0], 3) == -1)
   {
      perror("listen");
      close(sock[0]);
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
   shutdown(sock, SHUT_RDWR); // wakes up a thread blocked on this socket
   return close(sock);
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
   struct pollfd pfd;
   int want_read = (readable && *readable);
   int want_write = (writable && *writable);
   int r;

   if (readable) *readable = 0;
   if (writable) *writable = 0;

   if (sock == YAB_SOCK_INVALID)
      return -1;

   pfd.fd = sock;
   pfd.events = (want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0);
   pfd.revents = 0;

   r = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
   if (r < 0)
      return (errno == EINTR) ? 0 : -1;
   if (r == 0)
      return 0;

   // Errors and hang-ups are reported as "readable": the following
   // receive returns 0/-1 and the caller sees the disconnection.
   if (readable && (pfd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
      *readable = 1;
   if (writable && (pfd.revents & POLLOUT))
      *writable = 1;

   return 1;
}

//////////////////////////////////////////////////////////////////////////////

YabSock YabSockAccept(YabSock sock)
{
   YabSock s = accept(sock,NULL,NULL);
   if (s != -1)
      YabSockNoSigPipe(s);
   return s;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockSetNoDelay(YabSock sock)
{
   int one = 1;
   return setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0 ? 0 : -1;
}

//////////////////////////////////////////////////////////////////////////////

int YabSockSend(YabSock sock, const void *buf, int len, int flags)
{
   return (int)send(sock, buf, len, flags | YAB_SEND_FLAGS);
}

//////////////////////////////////////////////////////////////////////////////

int YabSockReceive(YabSock sock, void *buf, int len, int flags)
{
   int sysflags = (flags & YAB_SOCK_PEEK) ? MSG_PEEK : 0;
   return (int)recv(sock, buf, len, sysflags);
}

//////////////////////////////////////////////////////////////////////////////

int YabSockWouldBlock(void)
{
   return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
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
   socklen_t len = sizeof(ss);

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
   socklen_t alen = sizeof(addr);
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
   int flags;

   *sock = socket(AF_INET, SOCK_DGRAM, 0);
   if (*sock == -1)
      return -1;

   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_ANY);
   addr.sin_port = htons((unsigned short)port);

   flags = fcntl(*sock, F_GETFL, 0);
   if (bind(*sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
       flags == -1 || fcntl(*sock, F_SETFL, flags | O_NONBLOCK) == -1)
   {
      fprintf(stderr, "Can't bind UDP port %d: %s\n", port, strerror(errno));
      close(*sock);
      *sock = YAB_SOCK_INVALID;
      return -1;
   }

   return 0;
}

//////////////////////////////////////////////////////////////////////////////
