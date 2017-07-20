#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include "util.h"

void *
xmalloc(size_t n)
{
	void *p;

	p = malloc(n);
	if (!p)
		err(1, "malloc");

	return p;
}

void *
xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (!p)
		err(1, "realloc");

	return p;
}

void *
xcalloc(size_t n, size_t sz)
{
	void *p;

	p = calloc(n, sz);
	if (!p)
		err(1, "calloc");

	return p;
}

char *
xstrdup(const char *s, size_t n)
{
	char *r;

	r = xmalloc(n + 1);
	memcpy(r, s, n);
	r[n] = '\0';

	return r;
}

struct string *
mkstr(size_t n)
{
	struct string *str;

	str = xmalloc(sizeof(*str) + n + 1);
	str->n = n;

	return str;
}
