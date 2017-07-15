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

char *
xstrndup(const char *s, size_t n)
{
	char *r;

	r = strndup(s, n);
	if (!r)
		err(1, "strndup");

	return r;
}
