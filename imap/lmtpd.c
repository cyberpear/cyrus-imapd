/* lmtpd.c -- Program to deliver mail to a mailbox
 *
 * $Id: lmtpd.c,v 1.62.2.6 2001/11/24 19:20:21 ken3 Exp $
 * Copyright (c) 1999-2000 Carnegie Mellon University.  All rights reserved.
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
 *
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <com_err.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef USE_SIEVE
#include <sieve_interface.h>

#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif /* USE_SIEVE */

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sasl/sasl.h>
#include <sasl/saslutil.h>

#include "acl.h"
#include "assert.h"
#include "util.h"
#include "auth.h"
#include "prot.h"
#include "imparse.h"
#include "lock.h"
#include "imapconf.h"
#include "exitcodes.h"
#include "imap_err.h"
#include "mailbox.h"
#include "xmalloc.h"
#include "version.h"
#include "duplicate.h"
#include "append.h"
#include "mboxlist.h"
#include "notify.h"
#include "idle.h"
#include "rfc822date.h"
#include "tls.h"

#include "lmtpengine.h"
#include "lmtpstats.h"

typedef struct mydata {
    message_data_t *m;
    int cur_rcpt;

    struct stagemsg *stage;	/* staging location for single instance
				   store */
    char *notifyheader;
    const char *temp[2];	/* used to avoid extra indirection in
				   getenvelope() */

    char *authuser;		/* user who submitted message */
    struct auth_state *authstate;
} mydata_t;

/* data per script */
typedef struct script_data {
    char *username;
    char *mailboxname;
    struct auth_state *authstate;
} script_data_t;

/* forward declarations */
int deliver_mailbox(struct protstream *msg,
		    struct stagemsg **stage,
		    unsigned size,
		    char **flag,
		    int nflags,
		    char *authuser,
		    struct auth_state *authstate,
		    char *id,
		    char *user,
		    char *notifyheader,
		    char *mailboxname,
		    int quotaoverride,
		    int acloverride);
static int deliver(message_data_t *msgdata, char *authuser,
		   struct auth_state *authstate);
static int verify_user(const char *user, long quotacheck,
		       struct auth_state *authstate);
static char *generate_notify(message_data_t *m);

void shut_down(int code);

struct lmtp_func mylmtp = { &deliver, &verify_user, 0, 0 };

static void logdupelem();
static void usage();
static void setup_sieve();

/* global state */
extern int optind;
extern char *optarg;
extern int errno;
static int dupelim = 0;		/* eliminate duplicate messages with
				   same message-id */
static int singleinstance = 1;	/* attempt single instance store */
const char *BB = "";
#ifdef USE_SIEVE
static sieve_interp_t *sieve_interp;
static int sieve_usehomedir = 0;
static const char *sieve_dir = NULL;
#endif

/* per-user/session state */
static struct protstream *deliver_out, *deliver_in;

/* current namespace */
static struct namespace lmtpd_namespace;

/* should we allow users to proxy?  return SASL_OK if yes,
   SASL_BADAUTH otherwise */
static int mysasl_authproc(sasl_conn_t *conn,
			   void *context,
			   const char *requested_user, unsigned rlen,
			   const char *auth_identity, unsigned alen,
			   const char *def_realm, unsigned urlen,
			   struct propctx *propctx)
{
    const char *val;
    char *realm;
    int allowed=0;
    struct auth_state *authstate;

    /* check if remote realm */
    if ((realm = strchr(auth_identity, '@'))!=NULL) {
	realm++;
	val = config_getstring("loginrealms", "");
	while (*val) {
	    if (!strncasecmp(val, realm, strlen(realm)) &&
		(!val[strlen(realm)] || isspace((int) val[strlen(realm)]))) {
		break;
	    }
	    /* not this realm, try next one */
	    while (*val && !isspace((int) *val)) val++;
	    while (*val && isspace((int) *val)) val++;
	}
	if (!*val) {
	    sasl_seterror(conn, 0, "cross-realm login %s denied",
			  auth_identity);
	    return SASL_BADAUTH;
	}
    }

    /* ok, is auth_identity an admin? 
     * for now only admins can do lmtp from another machine
     */
    authstate = auth_newstate(auth_identity, NULL);
    allowed = authisa(authstate, "lmtp", "admins");
    auth_freestate(authstate);
    
    if (!allowed) {
	sasl_seterror(conn, 0, "only admins may authenticate");
	return SASL_BADAUTH;
    }

    return SASL_OK;
}

int mysasl_canon_user(sasl_conn_t *conn,
		      void *context,
		      const char *user, unsigned ulen,
		      const char *authid, unsigned alen,
		      unsigned flags,
		      const char *user_realm,
		      char *out_user,
		      unsigned out_max, unsigned *out_ulen,
		      char *out_authid,
		      unsigned out_amax, unsigned *out_alen) 
{
    char *canon_authuser = NULL, *canon_requser = NULL;

    canon_authuser = auth_canonifyid(authid, alen);
    if (!canon_authuser) {
	sasl_seterror(conn, 0, "bad userid authenticated");
	return SASL_BADAUTH;
    }
    *out_alen = strlen(canon_authuser);
    if(*out_alen > strlen(canon_authuser) > out_amax+1) {
	sasl_seterror(conn, 0, "buffer overflow while canonicalizing");
	return SASL_BUFOVER;
    }
    
    strncpy(out_authid, canon_authuser, out_amax);
    
    if (!user) user = authid;
    canon_requser = auth_canonifyid(user, ulen);
    if (!canon_requser) {
	sasl_seterror(conn, 0, "bad userid requested");
	return SASL_BADAUTH;
    }
    *out_ulen = strlen(canon_requser);
    if(*out_ulen > out_max+1) {
	sasl_seterror(conn, 0, "buffer overflow while canonicalizing");
	return SASL_BUFOVER;
    }
    
    strncpy(out_user, canon_requser, out_max);

    return SASL_OK;
}

static struct sasl_callback mysasl_cb[] = {
    { SASL_CB_GETOPT, &mysasl_config, NULL },
    { SASL_CB_PROXY_POLICY, &mysasl_authproc, NULL },
    { SASL_CB_CANON_USER, &mysasl_canon_user, NULL },
    { SASL_CB_LIST_END, NULL, NULL }
};


int service_init(int argc, char **argv, char **envp)
{
    int r;

    config_changeident("lmtpd");
    if (geteuid() == 0) return 1;
    
    signals_set_shutdown(&shut_down);
    signals_add_handlers();
    signal(SIGPIPE, SIG_IGN);

#ifdef USE_SIEVE
    sieve_usehomedir = config_getswitch("sieveusehomedir", 0);
    if (!sieve_usehomedir) {
	sieve_dir = config_getstring("sievedir", "/usr/sieve");
    } else {
	sieve_dir = NULL;
    }

    mylmtp.addheaders = xmalloc(80);
    snprintf(mylmtp.addheaders, 80, "X-Sieve: %s\r\n", SIEVE_VERSION);

    /* setup sieve support */
    setup_sieve();
#endif /* USE_SIEVE */

    singleinstance = config_getswitch("singleinstancestore", 1);
    BB = config_getstring("postuser", BB);

    if ((r = sasl_server_init(mysasl_cb, "Cyrus")) != SASL_OK) {
	syslog(LOG_ERR, "SASL failed initializing: sasl_server_init(): %s", 
	       sasl_errstring(r, NULL, NULL));
	return EC_SOFTWARE;
    }

    dupelim = config_getswitch("duplicatesuppression", 1);
    if (dupelim) {
	/* initialize duplicate delivery database */
	if (duplicate_init(NULL, 0) != 0) {
	    syslog(LOG_ERR, 
		   "lmtpd: unable to init duplicate delivery database\n");
	    dupelim = 0;
	}
    }

    /* so we can do mboxlist operations */
    mboxlist_init(0);
    mboxlist_open(NULL);

    /* setup for sending IMAP IDLE notifications */
    idle_enabled();

    /* Set namespace */
    if ((r = mboxname_init_namespace(&lmtpd_namespace, 0)) != 0) {
	syslog(LOG_ERR, error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }

    /* create connection to the SNMP listener, if available. */
    snmp_connect(); /* ignore return code */
    snmp_set_str(SERVER_NAME_VERSION, CYRUS_VERSION);

    return 0;
}

/*
 * run for each accepted connection
 */
int service_main(int argc, char **argv, char **envp)
{
    int opt;

    deliver_in = prot_new(0, 0);
    deliver_out = prot_new(1, 1);
    prot_setflushonread(deliver_in, deliver_out);
    prot_settimeout(deliver_in, 360);

    while ((opt = getopt(argc, argv, "C:a")) != EOF) {
	switch(opt) {
	case 'C': /* alt config file - handled by service::main() */
	    break;

	case 'a':
	    mylmtp.preauth = 1;
	    break;

	default:
	    usage();
	}
    }

    snmp_increment(TOTAL_CONNECTIONS, 1);
    snmp_increment(ACTIVE_CONNECTIONS, 1);

    lmtpmode(&mylmtp, deliver_in, deliver_out, 0);

    /* free session state */
    if (deliver_in) prot_free(deliver_in);
    if (deliver_out) prot_free(deliver_out);
    close(0);
    close(1);
    close(2);

    return 0;
}

/* called if 'service_init()' was called but not 'service_main()' */
void service_abort(void)
{
    duplicate_done();

    mboxlist_close();
    mboxlist_done();
}

#ifdef USE_SIEVE
static char *make_sieve_db(const char *user)
{
    static char buf[MAX_MAILBOX_PATH];

    buf[0] = '.';
    buf[1] = '\0';
    strcat(buf, user);
    strcat(buf, ".sieve.");

    return buf;
}

/* gets the header "head" from msg. */
static int getheader(void *v, const char *phead, const char ***body)
{
    message_data_t *m = ((mydata_t *) v)->m;

    if (phead==NULL) return SIEVE_FAIL;
    *body = msg_getheader(m, phead);

    if (*body) {
	return SIEVE_OK;
    } else {
	return SIEVE_FAIL;
    }
}

static int getsize(void *mc, int *size)
{
    message_data_t *m = ((mydata_t *) mc)->m;

    *size = msg_getsize(m);
    return SIEVE_OK;
}

/* we use the temp field in message_data to avoid having to malloc memory
   to return, and we also can't expose our the receipients to the message */
int getenvelope(void *mc, const char *field, const char ***contents)
{
    mydata_t *mydata = (mydata_t *) mc;
    message_data_t *m = mydata->m;

    if (!strcasecmp(field, "from")) {
	*contents = mydata->temp;
	mydata->temp[0] = m->return_path;
	mydata->temp[1] = NULL;
	return SIEVE_OK;
    } else if (!strcasecmp(field, "to")) {
	*contents = mydata->temp;
	mydata->temp[0] = msg_getrcptall(m, mydata->cur_rcpt);
	mydata->temp[1] = NULL;
	return SIEVE_OK;
    } else if (!strcasecmp(field, "auth") && mydata->authuser) {
	*contents = mydata->temp;
	mydata->temp[0] = mydata->authuser;
	mydata->temp[1] = NULL;
	return SIEVE_OK;
    } else {
	*contents = NULL;
	return SIEVE_FAIL;
    }
}

#define DEFAULT_SENDMAIL ("/usr/lib/sendmail")
#define DEFAULT_POSTMASTER ("postmaster")

#define SENDMAIL (config_getstring("sendmail", DEFAULT_SENDMAIL))
#define POSTMASTER (config_getstring("postmaster", DEFAULT_POSTMASTER))

static int global_outgoing_count = 0;

pid_t open_sendmail(const char *argv[], FILE **sm)
{
    int fds[2];
    FILE *ret;
    pid_t p;

    pipe(fds);
    if ((p = fork()) == 0) {
	/* i'm the child! run sendmail! */
	close(fds[1]);
	/* make the pipe be stdin */
	dup2(fds[0], 0);
	execv(SENDMAIL, (char **) argv);

	/* if we're here we suck */
	printf("451 lmtpd: didn't exec?!?\r\n");
	fatal("couldn't exec", EC_OSERR);
    }
    /* i'm the parent */
    close(fds[0]);
    ret = fdopen(fds[1], "w");
    *sm = ret;

    return p;
}

/* sendmail_errstr.  create a descriptive message given 'sm_stat': 
   the exit code from wait() from sendmail.

   not thread safe, but probably ok */
static char *sendmail_errstr(int sm_stat)
{
    static char errstr[200];

    if (WIFEXITED(sm_stat)) {
	snprintf(errstr, sizeof errstr,
		 "Sendmail process terminated normally, exit status %d\n",
		 WEXITSTATUS(sm_stat));
    } else if (WIFSIGNALED(sm_stat)) {
	snprintf(errstr, sizeof errstr,
		"Sendmail process terminated abnormally, signal = %d %s\n",
		WTERMSIG(sm_stat),
#ifdef WCOREDUMP
		WCOREDUMP(sm_stat) ? " -- core file generated" :
#endif
		"");
    } else if (WIFSTOPPED(sm_stat)) {
	snprintf(errstr, sizeof errstr,
		 "Sendmail process stopped, signal = %d\n",
		WTERMSIG(sm_stat));
    } else {
	return NULL;
    }
    
    return errstr;
}

int send_rejection(const char *origid,
		   const char *rejto,
		   const char *origreceip, 
		   const char *mailreceip, 
		   const char *reason, 
		   struct protstream *file)
{
    FILE *sm;
    const char *smbuf[10];
    char buf[8192], *namebuf;
    int i, sm_stat;
    time_t t;
    char datestr[80];
    pid_t sm_pid, p;

    smbuf[0] = "sendmail";
    smbuf[1] = "-i";		/* ignore dots */
    smbuf[2] = "-f";
    smbuf[3] = "<>";
    smbuf[4] = "--";
    smbuf[5] = rejto;
    smbuf[6] = NULL;
    sm_pid = open_sendmail(smbuf, &sm);
    if (sm == NULL) {
	return -1;
    }

    t = time(NULL);
    p = getpid();
    snprintf(buf, sizeof(buf), "<cmu-sieve-%d-%d-%d@%s>", p, (int) t, 
	     global_outgoing_count++, config_servername);
    
    namebuf = make_sieve_db(mailreceip);
    duplicate_mark(buf, strlen(buf), namebuf, strlen(namebuf), t);
    fprintf(sm, "Message-ID: %s\r\n", buf);

    rfc822date_gen(datestr, sizeof(datestr), t);
    fprintf(sm, "Date: %s\r\n", datestr);

    fprintf(sm, "X-Sieve: %s\r\n", SIEVE_VERSION);
    fprintf(sm, "From: Mail Sieve Subsystem <%s>\r\n", POSTMASTER);
    fprintf(sm, "To: <%s>\r\n", rejto);
    fprintf(sm, "MIME-Version: 1.0\r\n");
    fprintf(sm, "Content-Type: "
	    "multipart/report; report-type=disposition-notification;"
	    "\r\n\tboundary=\"%d/%s\"\r\n", (int) p, config_servername);
    fprintf(sm, "Subject: Automatically rejected mail\r\n");
    fprintf(sm, "Auto-Submitted: auto-replied (rejected)\r\n");
    fprintf(sm, "\r\nThis is a MIME-encapsulated message\r\n\r\n");

    /* this is the human readable status report */
    fprintf(sm, "--%d/%s\r\n\r\n", (int) p, config_servername);
    fprintf(sm, "Your message was automatically rejected by Sieve, a mail\r\n"
	    "filtering language.\r\n\r\n");
    fprintf(sm, "The following reason was given:\r\n%s\r\n\r\n", reason);

    /* this is the MDN status report */
    fprintf(sm, "--%d/%s\r\n"
	    "Content-Type: message/disposition-notification\r\n\r\n",
	    (int) p, config_servername);
    fprintf(sm, "Reporting-UA: %s; Cyrus %s/%s\r\n",
	    config_servername, CYRUS_VERSION, SIEVE_VERSION);
    if (origreceip)
	fprintf(sm, "Original-Recipient: rfc822; %s\r\n", origreceip);
    fprintf(sm, "Final-Recipient: rfc822; %s\r\n", mailreceip);
    fprintf(sm, "Original-Message-ID: %s\r\n", origid);
    fprintf(sm, "Disposition: "
	    "automatic-action/MDN-sent-automatically; deleted\r\n");
    fprintf(sm, "\r\n");

    /* this is the original message */
    fprintf(sm, "--%d/%s\r\nContent-Type: message/rfc822\r\n\r\n",
	    (int) p, config_servername);
    prot_rewind(file);
    while ((i = prot_read(file, buf, sizeof(buf))) > 0) {
	fwrite(buf, i, 1, sm);
    }
    fprintf(sm, "\r\n\r\n");
    fprintf(sm, "--%d/%s--\r\n", (int) p, config_servername);

    fclose(sm);
    while (waitpid(sm_pid, &sm_stat, 0) < 0);

    return sm_stat;	/* sendmail exit value */
}

int send_forward(char *forwardto, char *return_path, struct protstream *file)
{
    FILE *sm;
    const char *smbuf[10];
    int i, sm_stat;
    char buf[1024];
    pid_t sm_pid;

    smbuf[0] = "sendmail";
    smbuf[1] = "-i";		/* ignore dots */
    if (return_path != NULL) {
	smbuf[2] = "-f";
	smbuf[3] = return_path;
    } else {
	smbuf[2] = "-f";
	smbuf[3] = "<>";
    }
    smbuf[4] = "--";
    smbuf[5] = forwardto;
    smbuf[6] = NULL;
    sm_pid = open_sendmail(smbuf, &sm);
	
    if (sm == NULL) {
	return -1;
    }

    prot_rewind(file);

    while ((i = prot_read(file, buf, sizeof(buf))) > 0) {
	fwrite(buf, i, 1, sm);
    }

    fclose(sm);
    while (waitpid(sm_pid, &sm_stat, 0) < 0);

    return sm_stat;	/* sendmail exit value */
}


static
int sieve_redirect(void *ac, void *ic, void *sc, void *mc, const char **errmsg)
{
    sieve_redirect_context_t *rc = (sieve_redirect_context_t *) ac;
    script_data_t *sd = (script_data_t *) sc;
    message_data_t *m = ((mydata_t *) mc)->m;
    char buf[8192], *sievedb = NULL;
    int res;

    /* if we have a msgid, we can track our redirects */
    if (m->id) {
	snprintf(buf, sizeof(buf), "%s-%s", m->id, rc->addr);
	sievedb = make_sieve_db(sd->username);

	/* ok, let's see if we've redirected this message before */
	if (duplicate_check(buf, strlen(buf), sievedb, strlen(sievedb)))
	    return SIEVE_OK;
    }

    if ((res = send_forward(rc->addr, m->return_path, m->data)) == 0) {
	/* mark this message as redirected */
	if (sievedb) duplicate_mark(buf, strlen(buf), 
				    sievedb, strlen(sievedb), time(NULL));

	snmp_increment(SIEVE_REDIRECT, 1);
	return SIEVE_OK;
    } else {
	if (res == -1) {
	    *errmsg = "Could not spawn sendmail process";
	} else {
	    *errmsg = sendmail_errstr(res);
	}
	return SIEVE_FAIL;
    }
}

static
int sieve_discard(void *ac, void *ic, void *sc, void *mc, const char **errmsg)
{
    snmp_increment(SIEVE_DISCARD, 1);

    /* ok, we won't file it */
    return SIEVE_OK;
}

static
int sieve_reject(void *ac, void *ic, void *sc, void *mc, const char **errmsg)
{
    sieve_reject_context_t *rc = (sieve_reject_context_t *) ac;
    script_data_t *sd = (script_data_t *) sc;
    message_data_t *md = ((mydata_t *) mc)->m;
    const char **body;
    const char *origreceip;
    int res;

    if (md->return_path == NULL) {
	/* return message to who?!? */
	*errmsg = "No return-path for reply";
	return SIEVE_FAIL;
    }
    
    body = msg_getheader(md, "original-recipient");
    origreceip = body ? body[0] : NULL;
    if ((res = send_rejection(md->id, md->return_path, 
			      origreceip, sd->username,
			      rc->msg, md->data)) == 0) {
	snmp_increment(SIEVE_REJECT, 1);
	return SIEVE_OK;
    } else {
	if (res == -1) {
	    *errmsg = "Could not spawn sendmail process";
	} else {
	    *errmsg = sendmail_errstr(res);
	}
	return SIEVE_FAIL;
    }
}

static
int sieve_fileinto(void *ac, void *ic, void *sc, void *mc, const char **errmsg)
{
    sieve_fileinto_context_t *fc = (sieve_fileinto_context_t *) ac;
    script_data_t *sd = (script_data_t *) sc;
    mydata_t *mdata = (mydata_t *) mc;
    message_data_t *md = mdata->m;
    int quotaoverride = msg_getrcpt_ignorequota(md, mdata->cur_rcpt);
    int ret;

    /* we're now the user who owns the script */
    if (!sd->authstate)
	return SIEVE_FAIL;

    ret = deliver_mailbox(md->data, &mdata->stage, md->size,
			  fc->imapflags->flag, fc->imapflags->nflags,
                          sd->username, sd->authstate, md->id,
                          sd->username, mdata->notifyheader,
                          fc->mailbox, quotaoverride, 0);

    if (ret == 0) {

	snmp_increment(SIEVE_FILEINTO, 1);

	return SIEVE_OK;
    } else {
	*errmsg = error_message(ret);
	return SIEVE_FAIL;
    }
}

static
int sieve_keep(void *ac, void *ic, void *sc, void *mc, const char **errmsg)
{
    sieve_keep_context_t *kc = (sieve_keep_context_t *) ac;
    script_data_t *sd = (script_data_t *) sc;
    mydata_t *mydata = (mydata_t *) mc;
    message_data_t *md = mydata->m;
    int quotaoverride = msg_getrcpt_ignorequota(md, mydata->cur_rcpt);
    char namebuf[MAX_MAILBOX_PATH];
    int ret = 1;

    if (sd->mailboxname) {
	strcpy(namebuf, lmtpd_namespace.prefix[NAMESPACE_INBOX]);
	strcat(namebuf, sd->mailboxname);

	ret = deliver_mailbox(md->data, &mydata->stage, md->size,
			      kc->imapflags->flag, kc->imapflags->nflags,
			      mydata->authuser, mydata->authstate, md->id,
			      sd->username, mydata->notifyheader,
			      namebuf, quotaoverride, 0);
    }
    if (ret) {
	/* we're now the user who owns the script */
	if (!sd->authstate)
	    return SIEVE_FAIL;

	strcpy(namebuf, "INBOX");

	ret = deliver_mailbox(md->data, &mydata->stage, md->size,
			      kc->imapflags->flag, kc->imapflags->nflags,
			      sd->username, sd->authstate, md->id,
			      sd->username, mydata->notifyheader,
			      namebuf, quotaoverride, 1);
    }

    if (ret == 0) {	
	snmp_increment(SIEVE_KEEP, 1);
	return SIEVE_OK;
    } else {
	*errmsg = error_message(ret);
	return SIEVE_FAIL;
    }
}

static int sieve_notify(void *ac,
			void *interp_context, 
			void *script_context,
			void *mc,
			const char **errmsg)
{
    sieve_notify_context_t *nc = (sieve_notify_context_t *) ac;
    script_data_t *sd = (script_data_t *) script_context;

    snmp_increment(SIEVE_NOTIFY, 1);

    notify("SIEVE",
	   nc->priority,
	   sd->username,
	   NULL,
	   nc->message);
    
    return SIEVE_OK;
}

int autorespond(void *ac, void *ic, void *sc, void *mc, const char **errmsg)
{
    sieve_autorespond_context_t *arc = (sieve_autorespond_context_t *) ac;
    script_data_t *sd = (script_data_t *) sc;
    time_t t, now;
    int ret;

    snmp_increment(SIEVE_VACATION_TOTAL, 1);

    now = time(NULL);

    /* ok, let's see if we've responded before */
    t = duplicate_check(arc->hash, arc->len, 
			sd->username, strlen(sd->username));
    if (t) {
	if (now >= t) {
	    /* yay, we can respond again! */
	    ret = SIEVE_OK;
	} else {
	    ret = SIEVE_DONE;
	}
    } else {
	/* never responded before */
	ret = SIEVE_OK;
    }

    if (ret == SIEVE_OK) {
	duplicate_mark((char *) arc->hash, arc->len, 
		       sd->username, strlen(sd->username), 
		       now + arc->days * (24 * 60 * 60));
    }

    return ret;
}

int send_response(void *ac, void *ic, void *sc, void *mc, const char **errmsg)
{
    FILE *sm;
    const char *smbuf[10];
    char outmsgid[8192], *sievedb;
    int i, sl, sm_stat;
    time_t t;
    char datestr[80];
    pid_t sm_pid, p;
    sieve_send_response_context_t *src = (sieve_send_response_context_t *) ac;
    message_data_t *md = ((mydata_t *) mc)->m;
    script_data_t *sdata = (script_data_t *) sc;

    smbuf[0] = "sendmail";
    smbuf[1] = "-i";		/* ignore dots */
    smbuf[2] = "-f";
    smbuf[3] = "<>";
    smbuf[4] = "--";
    smbuf[5] = src->addr;
    smbuf[6] = NULL;
    sm_pid = open_sendmail(smbuf, &sm);
    if (sm == NULL) {
	*errmsg = "Could not spawn sendmail process";
	return -1;
    }

    t = time(NULL);
    p = getpid();
    snprintf(outmsgid, sizeof(outmsgid), "<cmu-sieve-%d-%d-%d@%s>", 
	     (int) p, (int) t, global_outgoing_count++, config_servername);
    
    fprintf(sm, "Message-ID: %s\r\n", outmsgid);

    rfc822date_gen(datestr, sizeof(datestr), t);
    fprintf(sm, "Date: %s\r\n", datestr);
    
    fprintf(sm, "X-Sieve: %s\r\n", SIEVE_VERSION);
    fprintf(sm, "From: <%s>\r\n", src->fromaddr);
    fprintf(sm, "To: <%s>\r\n", src->addr);
    /* check that subject is sane */
    sl = strlen(src->subj);
    for (i = 0; i < sl; i++)
	if (iscntrl((int) src->subj[i])) {
	    src->subj[i] = '\0';
	    break;
	}
    fprintf(sm, "Subject: %s\r\n", src->subj);
    if (md->id) fprintf(sm, "In-Reply-To: %s\r\n", md->id);
    fprintf(sm, "Auto-Submitted: auto-replied (vacation)\r\n");
    if (src->mime) {
	fprintf(sm, "MIME-Version: 1.0\r\n");
	fprintf(sm, "Content-Type: multipart/mixed;"
		"\r\n\tboundary=\"%d/%s\"\r\n", (int) p, config_servername);
	fprintf(sm, "\r\nThis is a MIME-encapsulated message\r\n\r\n");
	fprintf(sm, "--%d/%s\r\n", (int) p, config_servername);
    } else {
	fprintf(sm, "\r\n");
    }

    fprintf(sm, "%s\r\n", src->msg);

    if (src->mime) {
	fprintf(sm, "\r\n--%d/%s--\r\n", (int) p, config_servername);
    }
    fclose(sm);
    while (waitpid(sm_pid, &sm_stat, 0) < 0);

    if (sm_stat == 0) { /* sendmail exit value */
	sievedb = make_sieve_db(sdata->username);

	duplicate_mark(outmsgid, strlen(outmsgid), 
		       sievedb, strlen(sievedb), t);

	snmp_increment(SIEVE_VACATION_REPLIED, 1);

	return SIEVE_OK;
    } else {
	*errmsg = sendmail_errstr(sm_stat);
	return SIEVE_FAIL;
    }
}

/* vacation support */
sieve_vacation_t vacation = {
    1,				/* min response */
    31,				/* max response */
    &autorespond,		/* autorespond() */
    &send_response,		/* send_response() */
};

/* imapflags support */
static char *markflags[] = { "\\flagged" };
static sieve_imapflags_t mark = { markflags, 1 };

int sieve_parse_error_handler(int lineno, const char *msg, void *ic, void *sc)
{
    script_data_t *sd = (script_data_t *) sc;
    
    syslog(LOG_INFO, "sieve parse error for %s: line %d: %s",
	   sd->username, lineno, msg);
    
    return SIEVE_OK;
}

int sieve_execute_error_handler(const char *msg, void *ic, void *sc, void *mc)
{
    script_data_t *sd = (script_data_t *) sc;
    message_data_t *md = ((mydata_t *) mc)->m;
    
    syslog(LOG_INFO, "sieve runtime error for %s id %s: %s",
	   sd->username, md->id ? md->id : "(null)", msg);
    
    return SIEVE_OK;
}
 
static void setup_sieve(void)
{
    int res;

    res = sieve_interp_alloc(&sieve_interp, NULL);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_interp_alloc() returns %d\n", res);
	fatal("sieve_interp_alloc()", EC_SOFTWARE);
    }

    res = sieve_register_redirect(sieve_interp, &sieve_redirect);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_redirect() returns %d\n", res);
	fatal("sieve_register_redirect()", EC_SOFTWARE);
    }
    res = sieve_register_discard(sieve_interp, &sieve_discard);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_discard() returns %d\n", res);
	fatal("sieve_register_discard()", EC_SOFTWARE);
    }
    res = sieve_register_reject(sieve_interp, &sieve_reject);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_reject() returns %d\n", res);
	fatal("sieve_register_reject()", EC_SOFTWARE);
    }
    res = sieve_register_fileinto(sieve_interp, &sieve_fileinto);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_fileinto() returns %d\n", res);
	fatal("sieve_register_fileinto()", EC_SOFTWARE);
    }
    res = sieve_register_keep(sieve_interp, &sieve_keep);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_keep() returns %d\n", res);
	fatal("sieve_register_keep()", EC_SOFTWARE);
    }
    res = sieve_register_imapflags(sieve_interp, &mark);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_imapflags() returns %d\n", res);
	fatal("sieve_register_imapflags()", EC_SOFTWARE);
    }
    res = sieve_register_notify(sieve_interp, &sieve_notify);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_notify() returns %d\n", res);
	fatal("sieve_register_notify()", EC_SOFTWARE);
    }
    res = sieve_register_size(sieve_interp, &getsize);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_size() returns %d\n", res);
	fatal("sieve_register_size()", EC_SOFTWARE);
    }
    res = sieve_register_header(sieve_interp, &getheader);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_header() returns %d\n", res);
	fatal("sieve_register_header()", EC_SOFTWARE);
    }

    res = sieve_register_envelope(sieve_interp, &getenvelope);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR,"sieve_register_envelope() returns %d\n", res);
	fatal("sieve_register_envelope()", EC_SOFTWARE);
    }
    
    res = sieve_register_vacation(sieve_interp, &vacation);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_vacation() returns %d\n", res);
	fatal("sieve_register_vacation()", EC_SOFTWARE);
    }

    res = sieve_register_parse_error(sieve_interp, &sieve_parse_error_handler);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_parse_error() returns %d\n", res);
	fatal("sieve_register_parse_error()", EC_SOFTWARE);
    }
 
    res = sieve_register_execute_error(sieve_interp, 
				       &sieve_execute_error_handler);
    if (res != SIEVE_OK) {
	syslog(LOG_ERR, "sieve_register_execute_error() returns %d\n", res);
	fatal("sieve_register_execute_error()", EC_SOFTWARE);
    }
}

/* returns true if user has a sieve file */
static FILE *sieve_find_script(const char *user)
{
    char buf[1024];

    if (strlen(user) > 900) {
	return NULL;
    }
    
    if (!dupelim) {
	/* duplicate delivery suppression is needed for sieve */
	return NULL;
    }

    if (sieve_usehomedir) { /* look in homedir */
	struct passwd *pent = getpwnam(user);

	if (pent == NULL) {
	    return NULL;
	}

	/* check ~USERNAME/.sieve */
	snprintf(buf, sizeof(buf), "%s/%s", pent->pw_dir, ".sieve");
    } else { /* look in sieve_dir */
	char hash;

	hash = (char) dir_hash_c(user);

	snprintf(buf, sizeof(buf), "%s/%c/%s/default", sieve_dir, hash, user);
    }
	
    return (fopen(buf, "r"));
}
#else /* USE_SIEVE */
static FILE *sieve_find_script(const char *user)
{
    return NULL;
}
#endif /* USE_SIEVE */

static void
usage()
{
    fprintf(stderr, "421-4.3.0 usage: lmtpd [-C <alt_config>] [-a]\r\n");
    fprintf(stderr, "421 4.3.0 %s\n", CYRUS_VERSION);
    exit(EC_USAGE);
}

/* places msg in mailbox mailboxname.  
 * if you wish to use single instance store, pass stage as non-NULL
 * if you want to deliver message regardless of duplicates, pass id as NULL
 * if you want to notify, pass user
 * if you want to force delivery (to force delivery to INBOX, for instance)
 * pass acloverride
 */
int deliver_mailbox(struct protstream *msg,
		    struct stagemsg **stage,
		    unsigned size,
		    char **flag,
		    int nflags,
		    char *authuser,
		    struct auth_state *authstate,
		    char *id,
		    char *user,
		    char *notifyheader,
		    char *mailboxname,
		    int quotaoverride,
		    int acloverride)
{
    int r;
    struct appendstate as;
    char namebuf[MAX_MAILBOX_PATH];
    time_t now = time(NULL);

    /* Translate any separators in user */
    if (user) mboxname_hiersep_tointernal(&lmtpd_namespace, user);

    r = (*lmtpd_namespace.mboxname_tointernal)(&lmtpd_namespace, mailboxname,
					       user, namebuf);

    if (dupelim && id && 
	duplicate_check(id, strlen(id), namebuf, strlen(namebuf))) {
	/* duplicate message */
	logdupelem(id, namebuf);
	return 0;
    }
    if (!r) {
	r = append_setup(&as, namebuf, MAILBOX_FORMAT_NORMAL,
			 NULL, authstate, acloverride ? 0 : ACL_POST, 
			 quotaoverride ? -1 : 0);
    }

    if (!r) {
	prot_rewind(msg);
	if (singleinstance && stage) {
	    r = append_fromstage(&as, msg, size, now, 
				 (const char **) flag, nflags, stage);
	} else {
	    r = append_fromstream(&as, msg, size, now, 
				  (const char **) flag, nflags);
	}
	if (!r) append_commit(&as, NULL, NULL, NULL);
	else append_abort(&as);
    }

    if (!r && user) {
	/* do we want to replace user.XXX with INBOX? */
	notify("MAIL", mailboxname, user, mailboxname, 
	       notifyheader ? notifyheader : "");
    }

    if (!r && dupelim && id) duplicate_mark(id, strlen(id), 
					    namebuf, strlen(namebuf),
					    now);
    return r;
}

int deliver(message_data_t *msgdata, char *authuser,
	    struct auth_state *authstate)
{
    int n, nrcpts;
    mydata_t mydata;
    char namebuf[MAX_MAILBOX_PATH];
    
    assert(msgdata);
    nrcpts = msg_getnumrcpt(msgdata);
    assert(nrcpts);

    /* create 'mydata', our per-delivery data */
    mydata.m = msgdata;
    mydata.stage = NULL;
    mydata.notifyheader = generate_notify(msgdata);
    mydata.authuser = authuser;
    mydata.authstate = authstate;
    
    /* loop through each recipient, attempting delivery for each */
    for (n = 0; n < nrcpts; n++) {
	char *rcpt = xstrdup(msg_getrcpt(msgdata, n));
	char *plus;
	int quotaoverride = msg_getrcpt_ignorequota(msgdata, n);
	int r = 0;

	mydata.cur_rcpt = n;
	plus = strchr(rcpt, '+');
	if (plus) *plus++ = '\0';
	/* case 1: shared mailbox request */
	if (plus && !strcmp(rcpt, BB)) {
	    strcpy(namebuf, lmtpd_namespace.prefix[NAMESPACE_SHARED]);
	    strcat(namebuf, plus);
	    r = deliver_mailbox(msgdata->data, 
				&mydata.stage,
				msgdata->size, 
				NULL, 0,
				mydata.authuser, mydata.authstate,
				msgdata->id, NULL, mydata.notifyheader,
				namebuf, quotaoverride, 0);
	}

	/* case 2: ordinary user, might have Sieve script */
	else if (!strchr(rcpt, lmtpd_namespace.hier_sep) &&
	         strlen(rcpt) + 30 <= MAX_MAILBOX_PATH) {
	    FILE *f = sieve_find_script(rcpt);

#ifdef USE_SIEVE
	    if (f != NULL) {
		script_data_t *sdata = NULL;
		sieve_script_t *s = NULL;

		sdata = (script_data_t *) xmalloc(sizeof(script_data_t));

		sdata->username = rcpt;
		sdata->mailboxname = plus;
		sdata->authstate = auth_newstate(rcpt, (char *)0);

		/* slap the mailboxname back on so we hash the envelope & id
		   when we figure out whether or not to keep the message */
		snprintf(namebuf, sizeof(namebuf), "%s+%s", rcpt,
			 plus ? plus : "");
		
		/* is this the first time we've sieved the message? */
		if (msgdata->id) {
		    char *sdb = make_sieve_db(namebuf);
		    
		    if (duplicate_check(msgdata->id, strlen(msgdata->id),
					sdb, strlen(sdb))) {
			logdupelem(msgdata->id, sdb);
			/* done it before ! */
			r = 0;
			goto donercpt;
		    }
		}
		
		r = sieve_script_parse(sieve_interp, f, (void *) sdata, &s);
		fclose(f);
		if (r == SIEVE_OK) {
		    r = sieve_execute_script(s, (void *) &mydata);
		}
		if ((r == SIEVE_OK) && (msgdata->id)) {
		    /* ok, we've run the script */
		    char *sdb = make_sieve_db(namebuf);
		    
		    duplicate_mark(msgdata->id, strlen(msgdata->id), 
				   sdb, strlen(sdb), time(NULL));
		}
		
		/* free everything */
		if (sdata->authstate) auth_freestate(sdata->authstate);
		if (sdata) free(sdata);
		sieve_script_free(&s);
		
		/* if there was an error, r is non-zero and 
		   we'll do normal delivery */
	    } else {
		/* no sieve script */
		r = 1; /* do normal delivery actions */
	    }
#else /* USE_SIEVE */
	    r = 1;		/* normal delivery */
#endif /* USE_SIEVE */

	    if (r && plus &&
		strlen(rcpt) + strlen(plus) + 30 <= MAX_MAILBOX_PATH) {
		/* normal delivery to + mailbox */
		strcpy(namebuf, lmtpd_namespace.prefix[NAMESPACE_INBOX]);
		strcat(namebuf, plus);
		
		r = deliver_mailbox(msgdata->data, 
				    &mydata.stage, 
				    msgdata->size, 
				    NULL, 0, 
				    mydata.authuser, mydata.authstate,
				    msgdata->id, rcpt, mydata.notifyheader,
				    namebuf, quotaoverride, 0);
	    }

	    if (r) {
		/* normal delivery to INBOX */
		strcpy(namebuf, "INBOX");
		
		/* ignore ACL's trying to deliver to INBOX */
		r = deliver_mailbox(msgdata->data, 
				    &mydata.stage,
				    msgdata->size, 
				    NULL, 0, 
				    mydata.authuser, mydata.authstate,
				    msgdata->id, rcpt, mydata.notifyheader,
				    namebuf, quotaoverride, 1);
	    }
	}

    donercpt:
	free(rcpt);
	msg_setrcpt_status(msgdata, n, r);
    }

    append_removestage(mydata.stage);
    if (mydata.notifyheader) free(mydata.notifyheader);

    return 0;
}

/*
 */
static void
logdupelem(msgid, name)
char *msgid;
char *name;
{
    if (strlen(msgid) < 80) {
	char pretty[160];

	beautify_copy(pretty, msgid);
	syslog(LOG_INFO, "dupelim: eliminated duplicate message to %s id %s",
	       name, msgid);
    }
    else {
	syslog(LOG_INFO, "dupelim: eliminated duplicate message to %s",
	       name);
    }	
}

void fatal(const char* s, int code)
{
    prot_printf(deliver_out,"421 4.3.0 lmtpd: %s\r\n", s);
    prot_flush(deliver_out);
    exit(code);
}

/*
 * Cleanly shut down and exit
 */
void shut_down(int code) __attribute__((noreturn));
void shut_down(int code)
{
    duplicate_done();

    mboxlist_close();
    mboxlist_done();
#ifdef HAVE_SSL
    tls_shutdown_serverengine();
#endif
    prot_flush(deliver_out);

    snmp_increment(ACTIVE_CONNECTIONS, -1);

    exit(code);
}

static int verify_user(const char *user, long quotacheck,
		       struct auth_state *authstate)
{
    char buf[MAX_MAILBOX_NAME];
    char *plus;
    int r;
    int sl = strlen(BB);

    /* check to see if mailbox exists and we can append to it */
    if (!strncmp(user, BB, sl) && user[sl] == '+') {
	/* special shared folder address */
	strcpy(buf, user + sl + 1);
	/* Translate any separators in user */
	mboxname_hiersep_tointernal(&lmtpd_namespace, buf);
	/* - must have posting privileges on shared folders
	   - don't care about message size (1 msg over quota allowed) */
	r = append_check(buf, MAILBOX_FORMAT_NORMAL, authstate,
			 ACL_POST, quotacheck > 0 ? 0 : quotacheck);
    } else {
	/* ordinary user */
	if (strlen(user) > sizeof(buf)-10) {
	    r = IMAP_MAILBOX_NONEXISTENT;
	} else {
	    strcpy(buf, "user.");
	    strcat(buf, user);
	    plus = strchr(buf, '+');
	    if (plus) *plus = '\0';
	    /* Translate any separators in user */
	    mboxname_hiersep_tointernal(&lmtpd_namespace, buf+5);
	    /* - don't care about ACL on INBOX (always allow post)
	       - don't care about message size (1 msg over quota allowed) */
	    r = append_check(buf, MAILBOX_FORMAT_NORMAL, authstate,
			     0, quotacheck > 0 ? 0 : quotacheck);
	}
    }

    return r;
}

const char *notifyheaders[] = { "From", "Subject", "To", 0 };
/* returns a malloc'd string that should be sent to users for successful
   delivery of 'm'. */
char *generate_notify(message_data_t *m)
{
    const char **body;
    char *ret = NULL;
    int len = 0;
    int pos = 0;
    int i;

    for (i = 0; notifyheaders[i]; i++) {
	const char *h = notifyheaders[i];
	body = msg_getheader(m, h);
	if (body) {
	    int j;

	    for (j = 0; body[j] != NULL; j++) {
		/* put the header */
		while (pos + strlen(h) + 5 > len) {
		    ret = xrealloc(ret, len += 1024);
		}
		pos += sprintf(ret + pos, "%s: ", h);
		
		/* put the header body.
		   xxx it would be nice to linewrap.*/
		while (pos + strlen(body[j]) + 3 > len) {
		    ret = xrealloc(ret, len += 1024);
		}
		pos += sprintf(ret + pos, "%s\n", body[j]);
	    }
	}
    }

    return ret;
}
