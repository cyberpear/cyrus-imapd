/* config.h -- Configuration routines
 $Id: imapconf.h,v 1.6.2.1 2001/04/28 00:54:01 ken3 Exp $
 
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

#ifndef INCLUDED_IMAPCONF_H
#define INCLUDED_IMAPCONF_H

#include <sasl.h>
#include "auth.h"

extern int config_init(const char *alt_config, const char *ident);
extern const char *config_getstring(const char *key, const char *def);
extern int config_getint(const char *key, int def);
extern int config_getswitch(const char *key, int def);
extern const char *config_partitiondir(const char *partition);
extern int config_changeident(const char *ident);

/* sasl configuration */
extern int mysasl_config(void *context,
			 const char *plugin_name,
			 const char *option,
			 const char **result,
			 unsigned *len);
extern sasl_security_properties_t *mysasl_secprops(int flags);

/* check if `authstate' is a valid member of class */
extern int authisa(struct auth_state *authstate, 
		   const char *service, const char *class);

/* Values of mandatory options */
extern const char *config_dir;
extern const char *config_defpartition;
extern const char *config_newsspool;

extern const char *config_servername;

extern int config_hashimapspool;

void config_scanpartition( void (*proc)() );

/* signal handling (signals.c) */

typedef void shutdownfn(int);

void signals_add_handlers(void);
void signals_set_shutdown(shutdownfn *s);
void signals_poll(void);

/* base64 authentication functions (base64.c) */
struct protstream;
struct buf {
    char *s;
    int alloc;
};

void printauthready(struct protstream *out, int len, unsigned char *data);
int getbase64string(struct protstream *in, struct buf *buf);
int parsebase64string(char **ptr, const char *s);

/* imap parsing functions (imapparse.c) */
int getword(struct protstream *in, struct buf *buf);

enum string_types { IMAP_ASTRING, IMAP_NSTRING, IMAP_STRING };
int getxstring(struct protstream *pin, struct protstream *pout,
	       struct buf *buf, int type);
#define getastring(pin, pout, buf) getxstring((pin), (pout), (buf), IMAP_ASTRING)
#define getnstring(pin, pout, buf) getxstring((pin), (pout), (buf), IMAP_NSTRING)
#define getstring(pin, pout, buf) getxstring((pin), (pout), (buf), IMAP_STRING)

void eatline(struct protstream *pin, int c);

/* filenames */
#define FNAME_DBDIR "/db"
#define FNAME_USERDIR "/user/"
#define FNAME_LOGDIR "/log/"

#endif /* INCLUDED_IMAPCONF_H */
