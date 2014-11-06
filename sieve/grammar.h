/*
 * grammar.h
 *
 *  Created on: Nov 6, 2014
 *      Author: James Cassell
 */

#ifndef SIEVE_GRAMMAR_H_
#define SIEVE_GRAMMAR_H_

#include "strarray.h"
#include "varlist.h"

static int is_identifier(char *s)
{
/* identifier         = (ALPHA / "_") *(ALPHA / DIGIT / "_") */

    int i = 0;
    while (s && s[i]) {
	if (s[i] == '_' || (s[i] >= 'a' && s[i] <= 'z')
		|| (s[i] >= 'A' && s[i] <= 'A')
		|| (i && (s[i] >= '0' && s[i] <= '9'))) {
	    i++;
	} else {
	    return 0;
	}
    }
    return 1;
}

char *parse_string(const char *s, variable_list_t *vars)
{
/* identifier         = (ALPHA / "_") *(ALPHA / DIGIT / "_") */
    strarray_t stringparts = STRARRAY_INITIALIZER;
    variable_list_t *variable = NULL;
    char *t;
    strarray_append(&stringparts, s);
    t = strarray_nth(&stringparts, -1);
    while (t && *t) {
	char *u, *v;
	while (*t && *(t+1) && !('$' == *t && '{' == *(t+1))) {
	    t++;
	}
	if (!(*t && *(t+1))) {
	    break;
	}
	u = v = t;
	v+=2;
	while (*v && !('}' == *v)) {
	    v++;
	}
	if (!*v) {
	    break;
	}
	*v = '\0';
	if (is_identifier(u+2)) {
	    t = '\0';
	    t = v + 1;
	    if ((variable = varlist_select(vars, u+2))) {
		char *temp;
		strarray_append(&stringparts, temp = strarray_join(variable->var, ""));
		free (temp);
	    }
	    strarray_append(&stringparts, t);
	} else {
	    *v = '}';
	    t += 2;
	}
    }
    t = strarray_join(&stringparts, "");
    strarray_fini(&stringparts);

    return t;
}

#endif /* SIEVE_GRAMMAR_H_ */
