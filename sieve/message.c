/* message.c -- message parsing functions
 * Larry Greenfield
 * $Id: message.c,v 1.4 2000/01/28 22:09:55 leg Exp $
 */
/***********************************************************
        Copyright 1999 by Carnegie Mellon University

                      All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of Carnegie Mellon
University not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission.

CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE FOR
ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
******************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

#include "sieve_interface.h"
#include "interp.h"
#include "message.h"
#include "parseaddr.h"
#include "xmalloc.h"

/* reject message m with message msg
 *
 * incompatible with: fileinto, forward
 */
int do_reject(action_list_t *a, char *msg)
{
    action_list_t *b = NULL;

    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_FILEINTO ||
	    a->a == ACTION_KEEP ||
	    a->a == ACTION_REDIRECT ||
	    a->a == ACTION_REJECT ||
	    a->a == ACTION_VACATION ||
	    a->a == ACTION_SETFLAG ||
	    a->a == ACTION_ADDFLAG ||
	    a->a == ACTION_REMOVEFLAG ||
	    a->a == ACTION_MARK ||
	    a->a == ACTION_UNMARK
	    )
	    return -1;
	a = a->next;
    }

    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_REJECT;
    a->u.rej.msg = msg;
    b->next = a;
    a->next =  NULL;
    return 0;
}

/* fileinto message m into mailbox 
 *
 * incompatible with: reject
 */
int do_fileinto(action_list_t *a, char *mbox)
{
    action_list_t *b = NULL;

    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT)
	    return -1;
	a = a->next;
    }

    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_FILEINTO;
    a->u.fil.mbox = mbox;
    b->next = a;
    a->next = NULL;
    return 0;
}

/* forward message m to to addr
 *
 * incompatible with: reject
 */
int do_forward(action_list_t *a, char *addr)
{
    action_list_t *b = NULL;

    /* *** we should validate addr */

    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT)
	    return -1;
	a = a->next;
    }

    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_REDIRECT;
    a->u.red.addr = addr;
    a->next = NULL;
    b->next = a;
    return 0;
}

/* keep message
 *
 * incompatible with: reject
 */
int do_keep(action_list_t *a)
{
    action_list_t *b = NULL;

    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT)
	    return -1;
	if (a->a == ACTION_KEEP) /* don't bother doing it twice */
	    return 0;
	a = a->next;
    }

    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_KEEP;
    a->next = NULL;
    b->next = a;
    return 0;
}

/* discard message m
 *
 * incompatible with: nothing---it doesn't cancel any actions
 */
int do_discard(action_list_t *a)
{
    action_list_t *b = NULL;

    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_DISCARD) /* don't bother doing twice */
	    return 0;
	a = a->next;
    }

    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_DISCARD;
    a->next = NULL;
    b->next = a;
    return 0;
}

int do_vacation(action_list_t *a, char *addr, char *fromaddr,
		char *subj, char *msg, int days,
		int mime)
{
    action_list_t *b = NULL;

    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT ||
	    a->a == ACTION_VACATION) /* vacation can't be used twice */
	    return -1;
	a = a->next;
    }

    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_VACATION;
    a->u.vac.addr = addr;
    a->u.vac.fromaddr = fromaddr;
    a->u.vac.subj = subj;	/* user specified subject */
    a->u.vac.msg = msg;
    a->u.vac.days = days;
    a->u.vac.mime = mime;
    a->next = NULL;
    b->next = a;
    return 0;
}

/* setflag f on message m
 *
 * incompatible with: reject
 */
int do_setflag(action_list_t *a, char *flag)
{
    action_list_t *b = NULL;
 
    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT)
	    return -1;
	a = a->next;
    }
 
    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_SETFLAG;
    a->u.fla.flag = flag;
    b->next = a;
    a->next = NULL;
    return 0;
}

/* addflag f on message m
 *
 * incompatible with: reject
 */
int do_addflag(action_list_t *a, char *flag)
{
    action_list_t *b = NULL;
 
    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT)
	    return -1;
	a = a->next;
    }
 
    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_ADDFLAG;
    a->u.fla.flag = flag;
    b->next = a;
    a->next = NULL;
    return 0;
}

/* removeflag f on message m
 *
 * incompatible with: reject
 */
int do_removeflag(action_list_t *a, char *flag)
{
    action_list_t *b = NULL;
 
    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT)
	    return -1;
	a = a->next;
    }
 
    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_REMOVEFLAG;
    a->u.fla.flag = flag;
    b->next = a;
    a->next = NULL;
    return 0;
}


/* mark message m
 *
 * incompatible with: reject
 */
int do_mark(action_list_t *a)
{
    action_list_t *b = NULL;
 
    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT)
	    return -1;
	a = a->next;
    }
 
    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_MARK;
    b->next = a;
    a->next = NULL;
    return 0;
}


/* unmark message m
 *
 * incompatible with: reject
 */
int do_unmark(action_list_t *a)
{
    action_list_t *b = NULL;
 
    /* see if this conflicts with any previous actions taken on this message */
    while (a != NULL) {
	b = a;
	if (a->a == ACTION_REJECT)
	    return -1;
	a = a->next;
    }
 
    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_UNMARK;
    b->next = a;
    a->next = NULL;
    return 0;
}

/* notify
 *
 * incomaptible with: none
 */
int do_notify(sieve_interp_t *i,void *m, action_list_t *a,
	      char *priority, char *method, char *message, char **headers)
{
    action_list_t *b = NULL;
    int oldnum = 0;
    char **newheaders;
    int lup;
 
    /* just go to end */
    while (a != NULL) {
	b = a;
	a = a->next;
    }

    /* add to the action list */
    a = (action_list_t *) xmalloc(sizeof(action_list_t));
    if (a == NULL)
	return -1;
    a->a = ACTION_NOTIFY;
    a->u.not.priority = priority;
    a->u.not.method   = method;
    a->u.not.message  = message;

    /* count number of headers */
    if (headers)
    {
	while (headers[oldnum]!=NULL)
	    oldnum++;
    }
    

    newheaders = xmalloc(sizeof(char *)*(oldnum*2+1));

    for (lup=0;lup<oldnum;lup++)
    {
	char **tmp;
	newheaders[lup*2] = headers[lup];
	i->getheader(m,headers[lup], &tmp);

	if (tmp) newheaders[lup*2+1] = tmp[0];
	else newheaders[lup*2+1] = NULL;
    }

    newheaders[oldnum*2]=NULL;

    a->u.not.headers  = newheaders;
    
    a->next = NULL;
    b->next = a;
    return 0;
}

/* denotify
 *
 * incomaptible with: none
 */
int do_denotify(action_list_t **list)
{
    action_list_t *a = *list;
    action_list_t *b = NULL;
    action_list_t *elim = NULL;
    action_list_t *prev = NULL;
    action_list_t *next = NULL;

    /* eliminate the last ACTION_NOTIFY we see */
    while (a != NULL) {

	if (a->a == ACTION_NOTIFY)
	{
	    prev = b;
	    elim = a;
	    next = a->next;
	}
	b = a;
	a = a->next;
    }
    
    /* if didn't find return */
    if (elim == NULL) return 0;

    if (prev != NULL)
	prev->next = next;
    else
	*list = next;

    free(elim);

    return 0;
}



/* given a header, extract an address out of it.  if marker points to NULL,
   extract the first address.  otherwise, it's an index into the header to
   say where to start extracting */
struct addr_marker {
    struct address *where;
    char *freeme;
};

int parse_address(char *header, void **data, void **marker)
{
    struct addr_marker *am = (struct addr_marker *) *marker;

    parseaddr_list(header, (struct address **) data);
    am = (void *) xmalloc(sizeof(struct addr_marker));
    am->where = *data;
    am->freeme = NULL;
    *marker = am;
    return SIEVE_OK;
}

char *get_address(address_part_t addrpart, void **data, void **marker)
{
    char *ret = NULL;
    struct address *a;
    struct addr_marker *am = *marker;

    a = am->where;
    if (am->freeme) {
	free(am->freeme);
	am->freeme = NULL;
    }

    if (a == NULL) {
	ret = NULL;
    } else {
	switch (addrpart) { 
	case ADDRESS_ALL:
#define U_DOMAIN "unspecified-domain"
#define U_USER "unknown-user"
	    if (a->mailbox && a->domain) {
		am->freeme = (char *) xmalloc(strlen(a->mailbox) + 
					     strlen(a->domain) +
					     2);
		strcpy(am->freeme, a->mailbox);
		strcat(am->freeme, "@");
		strcat(am->freeme, a->domain);
		ret = am->freeme;
	    } else if (a->mailbox) {
		am->freeme = (char *) xmalloc(strlen(a->mailbox) +
					     strlen(U_DOMAIN) + 2);
		strcpy(am->freeme, a->mailbox);
		strcat(am->freeme, "@");
		strcat(am->freeme, U_DOMAIN);
		ret = am->freeme;
	    } else if (a->domain) {
		am->freeme = (char *) xmalloc(strlen(a->domain) +
					     strlen(U_USER) + 2);
		strcpy(am->freeme, U_USER);
		strcat(am->freeme, "@");
		strcat(am->freeme, a->domain);
		ret = am->freeme;
	    } else {
		ret = NULL;
	    }
	    break;
	case ADDRESS_LOCALPART:
	    ret = a->mailbox;
	    break;
	    
	case ADDRESS_DOMAIN:
	    ret = a->domain;
	    break;
	}
	a = a->next;
	am->where = a;
    }
    *marker = am;
    return ret;
}

int free_address(void **data, void **marker)
{
    struct addr_marker *am = (struct addr_marker *) *marker;

    if (*data)
	parseaddr_free((struct address *) *data);
    *data = NULL;
    if (am->freeme) free(am->freeme);
    free(am);
    *marker = NULL;
    return SIEVE_OK;
}

action_list_t *new_action_list(void)
{
    action_list_t *ret = xmalloc(sizeof(action_list_t));

    if (ret != NULL) {
	ret->a = ACTION_NONE;
	ret->param = NULL;
	ret->next = NULL;
    }
    return ret;
}

void free_action_list(action_list_t *a)
{
    while (a) {
	action_list_t *b = a->next;
	switch (a->a) {
	case ACTION_VACATION:
	    if (a->u.vac.addr) free(a->u.vac.addr);
	    if (a->u.vac.fromaddr) free(a->u.vac.addr);
	    if (a->u.vac.subj) free(a->u.vac.subj);
	    break;

	default:
	    break;
	}
	free(a);
	a = b;
    }
}

