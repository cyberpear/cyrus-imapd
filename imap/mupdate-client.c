/* mupdate-client.c -- cyrus murder database clients
 *
 * $Id: mupdate-client.c,v 1.9 2002/01/22 22:31:52 rjs3 Exp $
 * Copyright (c) 2001 Carnegie Mellon University.  All rights reserved.
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

#include <config.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <sasl/sasl.h>
#include <sasl/saslutil.h>
#include <syslog.h>
#ifdef __STDC__
#include <stdarg.h>
#else
#include <varargs.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include "prot.h"
#include "xmalloc.h"
#include "imapconf.h"
#include "assert.h"
#include "imparse.h"
#include "iptostring.h"
#include "mupdate.h"
#include "mupdate_err.h"
#include "exitcodes.h"

const char service_name[] = "mupdate";

/* We're only going to supply SASL_CB_USER, other people can supply
 * more if they feel like it */
/* FIXME: this basically means we only get kerberos.  should be fixed */
static int get_user(void *context __attribute__((unused)), int id,
		    const char **result, unsigned *len) 
{
    if(id != SASL_CB_USER) return SASL_FAIL;
    if(!result) return SASL_BADPARAM;

    *result = "";
    if(len) *len = 0;
    
    return SASL_OK;
}

static const sasl_callback_t callbacks[] = {
  { SASL_CB_USER, get_user, NULL }, 
  { SASL_CB_LIST_END, NULL, NULL }
};

static sasl_security_properties_t *make_secprops(int min, int max)
{
  sasl_security_properties_t *ret =
      (sasl_security_properties_t *)xzmalloc(sizeof(sasl_security_properties_t));

  ret->maxbufsize=4096;
  ret->min_ssf = config_getint("sasl_minimum_layer", min);	
  ret->max_ssf = config_getint("sasl_maximum_layer", max);

  return ret;
}

static int mupdate_authenticate(mupdate_handle *h,
				const char *mechlist)
{
    int saslresult;
    sasl_security_properties_t *secprops=NULL;
    socklen_t addrsize;
    struct sockaddr_in saddr_l;
    struct sockaddr_in saddr_r;
    char localip[60], remoteip[60];
    const char *out;
    unsigned int outlen;
    const char *mechusing;
    struct buf tag;
    int ch;
    char buf[4096];

    /* Why do this again? */
    if(h->saslcompleted) return 1;

    secprops = make_secprops(0,256);
    if(!secprops) return 1;
    
    saslresult=sasl_setprop(h->saslconn, SASL_SEC_PROPS, secprops);
    if(saslresult != SASL_OK) return 1;
    free(secprops);
    
    addrsize=sizeof(struct sockaddr_in);
    if (getpeername(h->sock,(struct sockaddr *)&saddr_r,&addrsize)!=0)
	return 1;

    addrsize=sizeof(struct sockaddr_in);
    if (getsockname(h->sock,(struct sockaddr *)&saddr_l,&addrsize)!=0)
	return 1;

    if(iptostring((const struct sockaddr *)&saddr_l, sizeof(struct sockaddr_in),
		  localip, 60) != 0)
	return 1;
    
    if(iptostring((const struct sockaddr *)&saddr_r, sizeof(struct sockaddr_in),
		  remoteip, 60) != 0)
	return 1;

    saslresult=sasl_setprop(h->saslconn, SASL_IPREMOTEPORT, remoteip);
    if (saslresult!=SASL_OK) return 1;

    saslresult=sasl_setprop(h->saslconn, SASL_IPLOCALPORT, localip);
    if (saslresult!=SASL_OK) return 1;

    /* We shouldn't get sasl_interact's,
     * because we provide explicit callbacks */
    saslresult = sasl_client_start(h->saslconn, mechlist,
				   NULL, &out, &outlen, &mechusing);

    if(saslresult != SASL_OK && saslresult != SASL_CONTINUE) return 1;

    if(out) {
	int r = sasl_encode64(out, outlen,
			      buf, sizeof(buf), NULL);
	if(r != SASL_OK) return 1;
	
	prot_printf(h->pout, "A01 AUTHENTICATE \"%s\" \"%s\"\r\n",
		    mechusing, buf);
    } else {
        prot_printf(h->pout, "A01 AUTHENTICATE \"%s\"\r\n", mechusing);
    }

    while(saslresult == SASL_CONTINUE) {
	char *p, *in;
	unsigned int len, inlen;
	
	if(!prot_fgets(buf, sizeof(buf)-1, h->pin)) {
	    /* Connection Dropped */
	    return 1;
	}

	p = buf + strlen(buf) - 1;
	if(p >= buf && *p == '\n') *p-- = '\0';
	if(p >= buf && *p == '\r') *p-- = '\0';

	len = strlen(buf);
	in = xmalloc(len);
	saslresult = sasl_decode64(buf, len, in, len, &inlen);
	if(saslresult != SASL_OK) {
	    free(in);

	    /* CANCEL */
	    syslog(LOG_ERR, "couldn't base64 decode: aborted authentication");

	    /* If we haven't already canceled due to bad authentication,
	     * then we should */
	    if(strncmp(buf, "A01 NO ", 7)) prot_printf(h->pout, "*");
	    else {
		syslog(LOG_ERR,
		       "Authentication to master failed (%s)", buf+7);
	    }
	    return 1;
	}

	saslresult = sasl_client_step(h->saslconn, in, inlen, NULL,
				      &out, &outlen);
	free(in);

	if((saslresult == SASL_OK || saslresult == SASL_CONTINUE) && out) {
	    int r = sasl_encode64(out, outlen,
				  buf, sizeof(buf), NULL);
	    if(r != SASL_OK) return 1;
	    
	    prot_printf(h->pout, "%s\r\n", buf);
	}
    }

    if(saslresult != SASL_OK) {
	syslog(LOG_ERR, "bad authentication: %s",
	       sasl_errdetail(h->saslconn));
	
	prot_printf(h->pout, "*");
	return 1;
    }

    /* Check Result */
    memset(&tag, 0, sizeof(struct buf)) ;
    
    ch = getword(h->pin, &tag);
    if(ch != ' ') return 1; /* need an OK or NO */

    ch = getword(h->pin, &tag);
    if(!strncmp(tag.s, "NO", 2)) {
	if(ch != ' ') return 1; /* no reason really necessary, but we failed */
	ch = getstring(h->pin, h->pout, &tag);
	syslog(LOG_ERR, "authentication failed: %s", tag.s);
	return 1;
    }

    prot_setsasl(h->pin, h->saslconn);
    prot_setsasl(h->pout, h->saslconn);

    h->saslcompleted = 1;

    return 0; /* SUCCESS */
}

int mupdate_connect(const char *server, const char *port,
		    mupdate_handle **handle,
		    sasl_callback_t *cbs)
{
    mupdate_handle *h;
    struct hostent *hp;
    struct servent *sp;
    struct sockaddr_in addr;
    int s, saslresult;
    char buf[4096];
    char *mechlist;
    
    if(!server || !handle)
	return MUPDATE_BADPARAM;

    /* open connection to 'server' */
    hp = gethostbyname(server);
    if(!hp) return -2;
    
    s = socket(AF_INET, SOCK_STREAM, 0);
    if(s == -1) return errno;
    
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr, hp->h_addr, sizeof(addr.sin_addr));

    if (port && imparse_isnumber(port)) {
	addr.sin_port = htons(atoi(port));
    } else if (port) {
	sp = getservbyname(port, "tcp");
	if (!sp) return -2;
	addr.sin_port = sp->s_port;
    } else if((sp = getservbyname("mupdate", "tcp")) != NULL) {
	addr.sin_port = sp->s_port;
    } else {
	addr.sin_port = htons(2004);
    }

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
	return errno;
    }

    h = xzmalloc(sizeof(mupdate_handle));
    h->sock = s;

    saslresult = sasl_client_new(service_name,
				 server,
				 NULL, NULL,
				 cbs ? cbs : callbacks,
				 0,
				 &(h->saslconn));

    /* create protstream */
    h->pin=prot_new(h->sock, 0);
    h->pout=prot_new(h->sock, 1);

    prot_setflushonread(h->pin, h->pout);
    prot_settimeout(h->pin, 30*60);

    *handle = h;

    /* Read the banner */
    if(!prot_fgets(buf, sizeof(buf)-1, h->pin)) {
	syslog(LOG_ERR,"connection to master dropped");
	return -3;
    }

    if(strncmp(buf, "* OK MUPDATE", 12)) {
	syslog(LOG_ERR,"invalid banner from remote mupdate server");
	return -4;
    }

    /* Read the mechlist */
    if(!prot_fgets(buf, sizeof(buf)-1, h->pin)) {
	syslog(LOG_ERR,"connection to master dropped");
	return -5;
    }

    if(strncmp(buf, "* AUTH", 6)) {
	syslog(LOG_ERR,"remote server did not send AUTH banner");
	return -6;
    }

    mechlist = buf + 6;
    
    if(mupdate_authenticate(h, mechlist)) {
	syslog(LOG_ERR, "authentication to remote mupdate failed");
	return -7;
    }

    return 0; /* SUCCESS */
}

void mupdate_disconnect(mupdate_handle **h) 
{
    if(!h || !(*h)) return;
    
    prot_printf((*h)->pout, "L01 LOGOUT\r\n");
    prot_flush((*h)->pout);

    prot_free((*h)->pin);
    prot_free((*h)->pout);
    sasl_dispose(&((*h)->saslconn));
    close((*h)->sock);

    free(*h); *h=NULL;
}

int mupdate_activate(mupdate_handle *handle, 
		     const char *mailbox, const char *server,
		     const char *acl)
{
    int ret;
    
    if (!handle) return MUPDATE_BADPARAM;
    if (!mailbox || !server || !acl) return MUPDATE_BADPARAM;
    if (!handle->saslcompleted) return MUPDATE_NOAUTH;

    prot_printf(handle->pout,
		"X%u ACTIVATE %s %s %s\r\n", handle->tag++,
		mailbox, server, acl);

    /* FIXME: NULL is invalid! */
    ret = mupdate_scarf(handle, NULL, NULL, 1);
    if(ret > 0)
	return MUPDATE_NOCONN;
    else if(ret < 0)
	return MUPDATE_FAIL;
    else
	return 0;
}

int mupdate_reserve(mupdate_handle *handle,
		    const char *mailbox, const char *server)
{
    int ret;
    
    if (!handle) return MUPDATE_BADPARAM;
    if (!mailbox || !server) return MUPDATE_BADPARAM;
    if (!handle->saslcompleted) return MUPDATE_NOAUTH;

    prot_printf(handle->pout,
		"X%u RESERVE %s %s\r\n", handle->tag++, mailbox, server);

    /* FIXME: NULL is invalid! */
    ret = mupdate_scarf(handle, NULL, NULL, 1);
    if(ret > 0)
	return MUPDATE_NOCONN;
    else if(ret < 0)
	return MUPDATE_FAIL;
    else
	return 0;
}

int mupdate_delete(mupdate_handle *handle,
		   const char *mailbox)
{
    int ret;
    
    if (!handle) return MUPDATE_BADPARAM;
    if (!mailbox) return MUPDATE_BADPARAM;
    if (!handle->saslcompleted) return MUPDATE_NOAUTH;

    prot_printf(handle->pout,
		"X%u DELETE %s\r\n", handle->tag++, mailbox);

    /* FIXME: NULL is invalid! */
    ret = mupdate_scarf(handle, NULL, NULL, 1);
    if(ret > 0)
	return MUPDATE_NOCONN;
    else if(ret < 0)
	return MUPDATE_FAIL;
    else
	return 0;
}

#define CHECKNEWLINE(c, ch) do { if ((ch) == '\r') (ch)=prot_getc((c)->pin); \
                                 if ((ch) != '\n') { syslog(LOG_ERR, \
                             "extra arguments recieved, aborting connection");\
                                 return 1; }} while(0);

/* Scarf up the incoming data and perform the requested operations */
int mupdate_scarf(mupdate_handle *handle, mupdate_callback callback,
		  void *context, int wait_for_ok)
{
    fd_set read_set;
    int highest_fd, select_result=0, ret;
    struct timeval tv;
    struct mupdate_mailboxdata box;

    if(!handle || !callback) return 1;

    memset(&box, 0, sizeof(box));

    highest_fd = handle->sock + 1;

    do {
	int ch;
	struct buf tag, cmd, arg1, arg2, arg3;
	unsigned char *p;
    
	memset(&tag, 0, sizeof(tag));
	memset(&cmd, 0, sizeof(cmd));
	memset(&arg1,0, sizeof(arg1));
    
	ch = getword(handle->pin, &tag);
	if(ch != ' ') {
	    /* We always have a command */
	    syslog(LOG_ERR, "Protocol error from master: no command",
		   tag.s, ch);
	    return 1;
	}
	ch = getword(handle->pin, &cmd);
	if(ch != ' ') {
	    /* We always have an arguement */
	    syslog(LOG_ERR, "Protocol error from master: no argument");
	    return 1;
	}
	
	if (islower((unsigned char) cmd.s[0])) {
	    cmd.s[0] = toupper((unsigned char) cmd.s[0]);
	}
	for (p = &cmd.s[1]; *p; p++) {
	    if (islower((unsigned char) *p))
		*p = toupper((unsigned char) *p);
	}
	
	switch(cmd.s[0]) {
	case 'B':
	    if(!strncmp(cmd.s, "BAD", 6)) {
		ch = getstring(handle->pin, handle->pout, &arg1);
		
		CHECKNEWLINE(handle, ch);

		syslog(LOG_DEBUG, "mupdate BAD response: %s", arg1.s);
		if(wait_for_ok) return -1;
		break;
	    }
	    syslog(LOG_ERR, "bad command from master: %s", cmd.s);
	    return 1;
	case 'D':
	    if(!strncmp(cmd.s, "DELETE", 6)) {
		ch = getstring(handle->pin, handle->pout, &arg1);
		
		CHECKNEWLINE(handle, ch);
		
		box.mailbox = arg1.s;

		/* Handle delete command */
		ret = callback(&box, cmd.s, context);
		if(ret) {
		    syslog(LOG_ERR, "Error deleting mailbox");
		    return ret;
		}
		break;
	    }
	    syslog(LOG_ERR, "bad command from master: %s", cmd.s);
	    return 1;
	case 'M':
	    if(!strncmp(cmd.s, "MAILBOX", 7)) {
		memset(&arg2, 0, sizeof(arg2));
		memset(&arg3, 0, sizeof(arg3));
		
		/* Mailbox Name */
		ch = getstring(handle->pin, handle->pout, &arg1);
		if(ch != ' ') return 1;
		
		/* Server */
		ch = getstring(handle->pin, handle->pout, &arg2);
		if(ch != ' ') return 1;
		
		/* ACL */
		ch = getstring(handle->pin, handle->pout, &arg3);
		
		CHECKNEWLINE(handle, ch);
		
		/* Handle mailbox command */
		box.mailbox = arg1.s;
		box.server = arg2.s;
		box.acl = arg3.s;
		ret = callback(&box, cmd.s, context);
		if(ret) {
		    /* Was there an error? */
		    syslog(LOG_ERR, "Error activating mailbox");
		    return ret;
		}
		break;
	    }
	    syslog(LOG_ERR, "bad command from master: %s", cmd.s);
	    return 1;
	case 'N':
	    if(!strncmp(cmd.s, "NO", 6)) {
		ch = getstring(handle->pin, handle->pout, &arg1);
		
		CHECKNEWLINE(handle, ch);

		syslog(LOG_DEBUG, "mupdate NO response: %s", arg1.s);
		if(wait_for_ok) return -1;
		break;
	    }
	    syslog(LOG_ERR, "bad command from master: %s", cmd.s);
	    return 1;
	case 'O':
	    if(!strncmp(cmd.s, "OK", 2)) {
		/* It's all good, grab the attached string and move on */
		ch = getstring(handle->pin, handle->pout, &arg1);
		
		CHECKNEWLINE(handle, ch);
		
		if(wait_for_ok) return 0;
		break;
	    }
	    syslog(LOG_ERR, "bad command from master: %s", cmd.s);
	    return 1;
	case 'R':
	    if(!strncmp(cmd.s, "RESERVE", 7)) {
		memset(&arg2, 0, sizeof(arg2));
		
		/* Mailbox Name */
		ch = getstring(handle->pin, handle->pout, &arg1);
		if(ch != ' ') return 1;
		
		/* Server */
		ch = getstring(handle->pin, handle->pout, &arg2);
		
		CHECKNEWLINE(handle, ch);
		
		/* Handle reserve command */
		box.mailbox=arg1.s;
		box.server=arg2.s;
		ret = callback(&box, cmd.s, context);
		if(ret) {
		    /* Was there an error? */
		    syslog(LOG_ERR, "Error reserveing mailbox");
		    return ret;
		}
		
		break;
	    }
	    syslog(LOG_ERR, "bad command from master: %s", cmd.s);
	    return 1;
	default:
	    /* Bad Data */
	    syslog(LOG_ERR, "bad/unexpected command from master: %s", cmd.s);
	    return 1;
	}

	/* If we are waiting for an OK message, block, otherwise, drop
	   through immediately */
	ch = prot_getc(handle->pin);
	if(ch != EOF) {
	    prot_ungetc(ch, handle->pin);
	    select_result = 1;
	    continue;
	}

	FD_ZERO(&read_set);
	FD_SET(handle->sock, &read_set);

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	select_result = select(highest_fd, &read_set, NULL, NULL, 
			       (wait_for_ok ? NULL : &tv));
    } while((!wait_for_ok && select_result > 0) || (select_result > 0));

    if(select_result != 0) return 1;
    else return 0;
}
