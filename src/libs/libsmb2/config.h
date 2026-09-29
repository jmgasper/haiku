/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * What libsmb2's configure would have found on Haiku.
 */
#ifndef _LIBSMB2_CONFIG_H
#define _LIBSMB2_CONFIG_H

#define _U_ __attribute__((unused))

#define HAVE_ARPA_INET_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_POLL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1

#define HAVE_ADDRINFO 1
#define HAVE_LINGER 1
#define HAVE_SOCKADDR_LEN 1
#define HAVE_SOCKADDR_SA_LEN 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_SOCK_SIN_LEN 1

/* IPPROTO_TCP. Otherwise socket.c asks getprotobyname() for it, and does
   without TCP_NODELAY when that has no answer. */
#define SOL_TCP 6

#endif	/* _LIBSMB2_CONFIG_H */
