/* fud.c -- long-lived finger information provider
 * Copyright (c) 1998-2000 Carnegie Mellon University.  All rights reserved.
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
 *

 */

#include <config.h>

/* $Id: fud.c,v 1.25 2002/02/14 01:15:03 rjs3 Exp $ */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <com_err.h>
#include <pwd.h>

#include "assert.h"
#include "mboxlist.h"
#include "imapconf.h"
#include "exitcodes.h"
#include "imap_err.h"
#include "mailbox.h"
#include "xmalloc.h"
#include "acl.h"
#include "seen.h"
#include "mboxname.h"
#include "map.h"

#define REQ_OK		0
#define REQ_DENY	1
#define REQ_UNK		2

extern int errno;
extern int optind;
extern char *optarg;

/* current namespace */
static struct namespace fud_namespace;

/* forward decls */
int handle_request(const char *who, const char *name, 
		   struct sockaddr_in sfrom);

void send_reply(struct sockaddr_in sfrom, int status,
		const char *user, const char *mbox, 
		int numrecent, time_t lastread, time_t lastarrived);

int soc;

char who[16];

int init_network(int port)
{
    soc = 0;	/* inetd has handed us the port as stdin */
    return(0);
}

#define MAXLOGNAME 16		/* should find out for real */

int begin_handling(void)
{
        struct sockaddr_in  sfrom;
        socklen_t sfromsiz = sizeof(sfrom);
        int r;
        char    buf[MAXLOGNAME + MAX_MAILBOX_NAME + 1];
        char    username[MAXLOGNAME];
        char    mbox[MAX_MAILBOX_NAME+1];
        char    *q;
        int     off;

	openlog("fud", LOG_PID, LOG_LOCAL6);

        while(1) {
            /* For safety */
            memset(username,'\0',MAXLOGNAME);	
            memset(mbox,'\0',MAX_MAILBOX_NAME+1);
            memset(buf, '\0', MAXLOGNAME + MAX_MAILBOX_NAME + 1);

	    signals_poll();
            r = recvfrom(soc, buf, 511, 0, 
			 (struct sockaddr *) &sfrom, &sfromsiz);
            if (r == -1) {
		return(errno);
	    }
            for(off = 0; buf[off] != '|' && off < MAXLOGNAME; off++);
            if(off < MAXLOGNAME) {
		strlcpy(username,buf,off);
            } else {
		continue;
            }
            q = buf + off + 1;
            strlcpy(mbox,q,(r - (off + 1)  < MAX_MAILBOX_NAME) ? 
		    r - (off + 1) : MAX_MAILBOX_NAME);

            handle_request(username,mbox,sfrom);
        }

	/* never reached */
}

void shut_down(int code) __attribute__((noreturn));
void shut_down(int code)
{
    seen_done();
    mboxlist_close();
    mboxlist_done();
    closelog();
    exit(code);
}


int main(int argc, char **argv)
{
    int port = 0;
    int r;
    int opt;
    char *alt_config = NULL;
   
    r = 0; /* to shut up lint/gcc */

    if(geteuid() == 0)
        fatal("must run as the Cyrus user", EC_USAGE);

    while ((opt = getopt(argc, argv, "C:")) != EOF) {
	switch (opt) {
	case 'C': /* alt config file */
	    alt_config = optarg;
	    break;
	default:
	    break;
	}
    }

    config_init(alt_config, "fud");

    signals_set_shutdown(&shut_down);
    signals_add_handlers();

    mboxlist_init(0);
    mboxlist_open(NULL);
    mailbox_initialize();

    /* Set namespace */
    if ((r = mboxname_init_namespace(&fud_namespace, 1)) != 0) {
	syslog(LOG_ERR, error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }

    r = init_network(port);
    if (r) {
        fatal("unable to configure network port", EC_OSERR);
    }
    
    begin_handling();

    shut_down(0);
}

static void cyrus_timeout(int signo)
{
  signo = 0;
  return;
}

/* Send a proxy request to the backend, send their reply to sfrom */
int do_proxy_request(const char *who, const char *name,
		     const char *backend_host,
		     struct sockaddr_in sfrom) 
{
    char tmpbuf[1024];
    int x, rc;
    int csoc = -1;
    struct sockaddr_in cin, cout;
    struct hostent *hp;
    int backend_port = 4201; /* default fud udp port */
    static struct servent *sp = NULL;

    /* Open a UDP socket to the Cyrus mail server */
    if(!sp) {
	sp = getservbyname("fud", "udp");
	if(sp) backend_port = sp->s_port;
    }

    hp = gethostbyname (backend_host);
    if (!hp) {
	send_reply(sfrom, REQ_UNK, who, name, 0, 0, 0);
	rc = IMAP_SERVER_UNAVAILABLE;
	goto done;
    }

    csoc = socket (PF_INET, SOCK_DGRAM, 0);
    memcpy (&cin.sin_addr.s_addr, hp->h_addr, hp->h_length);
    cin.sin_family = AF_INET;
    cin.sin_port = backend_port;

    /* Write a Cyrus query into *tmpbuf */
    memset (tmpbuf, '\0', sizeof(tmpbuf));
    sprintf (tmpbuf, "%s|%s", who, name);
    x = sizeof (cin);

    /* Send the query and wait for a reply */
    sendto (csoc, tmpbuf, strlen (tmpbuf), 0, (struct sockaddr *) &cin, x);
    memset (tmpbuf, '\0', strlen (tmpbuf));
    signal (SIGALRM, cyrus_timeout);
    rc = 0;
    alarm (1);
    rc = recvfrom (csoc, tmpbuf, sizeof(tmpbuf), 0,
		   (struct sockaddr *) &cout, &x);
    alarm (0);
    if (rc < 1) {
	rc = IMAP_SERVER_UNAVAILABLE;
	send_reply(sfrom, REQ_UNK, who, name, 0, 0, 0);
	goto done;
    }

    /* Send reply back */
    /* rc is size */
    sendto(soc,tmpbuf,rc,0,(struct sockaddr *) &sfrom, sizeof(sfrom));
    rc = 0;

 done:
    if(csoc != -1) close(csoc);
    return rc;
}

int handle_request(const char *who, const char *name,
		   struct sockaddr_in sfrom)
{
    int r;
    struct mailbox mailbox;
    struct seen *seendb;
    time_t lastread;
    time_t lastarrived;
    unsigned recentuid;
    char *seenuids;
    unsigned numrecent;
    char mboxname[MAX_MAILBOX_NAME+1];
    char *location;
    int mbflag;

    numrecent = 0;
    lastread = 0;
    lastarrived = 0;

    r = (*fud_namespace.mboxname_tointernal)(&fud_namespace,name,who,mboxname);
    if (r) return r; 

    r = mboxlist_detail(mboxname, &mbflag, &location, NULL, NULL, NULL);
    if(r) {
	send_reply(sfrom, REQ_UNK, who, name, 0, 0, 0);
	return r;
    }

    syslog(LOG_ERR, "foo");

    if(mbflag & MBTYPE_REMOTE) {
	char *p = NULL;

	/* xxx hide that we are storing partitions */
	p = strchr(location, '!');
	if(p) *p = '\0';
	
	/* We want to proxy this one */
	syslog(LOG_ERR, "doing proxy");
	return do_proxy_request(who, name, location, sfrom);
    }

    /*
     * Open/lock header 
     */
    r = mailbox_open_header(mboxname, NULL, &mailbox);
    if (r) {
        send_reply(sfrom, REQ_UNK, who, name, 0, 0, 0);
	return r; 
    }

    r = mailbox_open_index(&mailbox);
    if (r) {
	mailbox_close(&mailbox);
        send_reply(sfrom, REQ_UNK, who, name, 0, 0, 0);
	return r;
    }

    if(!(strncmp(mboxname,"user.",5)) && !(mailbox.myrights & ACL_USER0)) {
	mailbox_close(&mailbox);
        send_reply(sfrom, REQ_DENY, who, name, 0, 0, 0);
	return 0;
    }
   
    r = seen_open(&mailbox, who, &seendb);
    if (r) {
	mailbox_close(&mailbox);
        send_reply(sfrom, REQ_UNK, who, name, 0, 0, 0);
	return r;
    }

    seenuids = NULL;
    r = seen_read(seendb, &lastread, &recentuid, &lastarrived, &seenuids);
    if (seenuids) free(seenuids);
    seen_close(seendb);
    if (r) {
	mailbox_close(&mailbox);
        send_reply(sfrom, REQ_UNK, who, name, 0, 0, 0);
	return r;
    }
    
    lastarrived = mailbox.last_appenddate;
    {
	const char *base;
	unsigned long len = 0;
	int msg;
	unsigned uid;
	
	map_refresh(mailbox.index_fd, 0, &base, &len,
		    mailbox.start_offset +
		    mailbox.exists * mailbox.record_size,
		    "index", mailbox.name);
	for (msg = 0; msg < mailbox.exists; msg++) {
	    uid = ntohl(*((bit32 *)(base + mailbox.start_offset +
				    msg * mailbox.record_size +
				    OFFSET_UID)));
	    if (uid > recentuid) numrecent++;
	}
	map_free(&base, &len);
    }

    mailbox_close(&mailbox);
    
    send_reply(sfrom, REQ_OK, who, name, numrecent, lastread, lastarrived);
    
    return(0);
}

void
send_reply(struct sockaddr_in sfrom, int status, 
	   const char *user, const char *mbox,
	   int numrecent, time_t lastread, time_t lastarrived)
{
    char buf[MAX_MAILBOX_PATH + 16 + 9];
    int siz;

    switch(status) {
        case REQ_DENY:
            sendto(soc,"PERMDENY",9,0,(struct sockaddr *) &sfrom, sizeof(sfrom));       
            break;
        case REQ_OK:
            siz = sprintf(buf,"%s|%s|%d|%d|%d",user,mbox,numrecent,(int) lastread,(int) lastarrived);
            sendto(soc,buf,siz,0,(struct sockaddr *) &sfrom, sizeof(sfrom));       
            break;
        case REQ_UNK:
            sendto(soc,"UNKNOWN",8,0,(struct sockaddr *) &sfrom, sizeof(sfrom));       
            break;
    } 
}

void fatal(const char* s, int code)
{
    fprintf(stderr, "fud: %s\n", s);
    exit(code);
}
