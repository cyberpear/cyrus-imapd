/* $Id: acconfig.h,v 1.17.6.5 2001/11/24 19:20:07 ken3 Exp $ */
/* 
 * Copyright (c) 2000 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


@TOP@

/* where are we going to be installed? */
#define CYRUS_PATH "/usr/cyrus"

/* what user are we going to run as? */
#define CYRUS_USER "cyrus"

/* do we have strerror()? */
#undef HAS_STRERROR

/* do we have kerberos? */
#undef HAVE_KRB

/* do we already have sys_errlist? */
#undef NEED_SYS_ERRLIST

/* how should we setproctitle? */
#undef SPT_TYPE

/* do we have the AFS symbol pr_End? */
#undef HAVE_PR_END

/* do we have an acceptable regex library? */
#undef ENABLE_REGEX

/* do we support XNETSCAPE */
#undef ENABLE_X_NETSCAPE_HACK

/* are we using the old sieve service name (imap) */
#undef OLD_SIEVE_SERVICE_NAME

/* we better have berkeley db 3.x */
#undef HAVE_LIBDB

/* the AFS RX (RPC) package */
#undef HAVE_RX

/* the TCP control package */
#undef HAVE_LIBWRAP

/* do we have SASL support for APOP? */
#undef HAVE_APOP

/* do we have OpenSSL? */
#undef HAVE_SSL

/* alternative to /dev/urandom? */
#undef EGD_SOCKET

/* where should we put state information? */
#undef STATEDIR

/* is Sieve enabled? */
#undef USE_SIEVE

/* use full directory hashing? */
#undef USE_DIR_FULL

/* do we have the UCD SNMP libraries? */
#undef HAVE_UCDSNMP

/* _POSIX_PTHREAD_SEMANTICS needed? */
#undef _POSIX_PTHREAD_SEMANTICS

/* _REENTRANT needed? */
#undef _REENTRANT

/* _SGI_REENTRANT_FUNCTIONS needed? */
#undef _SGI_REENTRANT_FUNCTIONS

/* This seems to be required to make Solaris happy. */
#undef __EXTENSIONS__

/* do we have socklen_t? */
#undef HAVE_SOCKLEN_T

/* do we have rlim_t? */
#undef HAVE_RLIM_T

@BOTTOM@

#ifndef __GNUC__
#define __attribute__(foo)
#endif

#ifndef HAVE_SOCKLEN_T
typedef int socklen_t;
#endif

#ifndef HAVE_RLIM_T
typedef int rlim_t;
#endif

/* some potentially memory saving tradeoffs, 
   preconfigured in memory-saving mode */

/* call sasl_client_init() at the start of imapd */
#define DELAY_SASL_CLIENT_INIT

/* save the cmdlines for the ID command */
#undef ID_SAVE_CMDLINE

/* getaddrinfo things */
#include <netdb.h>
#include <sys/socket.h>

#ifndef HAVE_GETADDRINFO
#define	getaddrinfo	sasl_getaddrinfo
#define	freeaddrinfo	sasl_freeaddrinfo
#define	getnameinfo	sasl_getnameinfo
#define	gai_strerror	sasl_gai_strerror
#include "gai.h"
#endif

#ifndef	NI_WITHSCOPEID
#define	NI_WITHSCOPEID	0
#endif

#ifdef OLD_SIEVE_SERVICE_NAME
#define SIEVE_SERVICE_NAME "imap"
#else
#define SIEVE_SERVICE_NAME "sieve"
#endif

/* compile time options; think carefully before modifying */
enum {
    /* should a hierarchical rename stop on error? */
    RENAME_STOP_ON_ERROR = 1,

    /* should we call fsync() to maybe help with softupdates? (it should) */
    APPEND_ULTRA_PARANOID = 1,

    /* should we log extra information at the DEBUG level for DB stuff? 
     * 0 -> nothing; 1 -> some; higher -> even more */
    CONFIG_DB_VERBOSE = 1,

    /* log timing information to LOG_DEBUG */
    CONFIG_TIMING_VERBOSE = 0,

    /* should we be pedantic about namespace or sleezy? */
    SLEEZY_NAMESPACE = 1,

    /* should we do a fast TLS session shutdown? */
    TLS_FAST_SHUTDOWN = 1,

    /* should we use the SQUAT engine to accelerate SEARCH? */
    SQUAT_ENGINE = 1,

    /* should we have long LMTP error messages? */
    LMTP_LONG_ERROR_MSGS = 1
};
