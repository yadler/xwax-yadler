/*
 * Copyright (C) 2009 Mark Hills <mark@pogo.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#define _GNU_SOURCE /* strcasestr() */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library.h"
#include "listing.h"

#define BLOCK 256


int listing_init(struct listing_t *ls)
{
    ls->record = malloc(sizeof(struct record_t*) * BLOCK);
    if(ls->record == NULL) {
        perror("malloc");
        return -1;
    }

    ls->size = BLOCK;
    ls->entries = 0;

    return 0;
}


void listing_clear(struct listing_t *ls)
{
    free(ls->record);
}


void listing_blank(struct listing_t *ls)
{
    ls->entries = 0;
}


int listing_add(struct listing_t *ls, struct record_t *lr)
{
    struct record_t **ln;

    if(ls->entries == ls->size) {
        ln = realloc(ls->record, sizeof(struct record_t*) * ls->size * 2);

        if(!ln) {
            perror("realloc");
            return -1;
        }

        ls->record = ln;
        ls->size *= 2;
    }

    ls->record[ls->entries++] = lr;

    return 0;
}


int listing_add_library(struct listing_t *ls, struct library_t *lb)
{
    int n;

    for(n = 0; n < lb->entries; n++) {
        if(listing_add(ls, &lb->record[n]) == -1)
            return -1;
    }

    return 0;
}


static int record_cmp(const struct record_t *a, const struct record_t *b)
{
    int r;

    r = strcmp(a->artist, b->artist);
    if (r < 0)
        return -1;
    else if (r > 0)
        return 1;

    r = strcmp(a->title, b->title);
    if (r < 0)
        return -1;
    else if (r > 0)
        return 1;

    return 0;
}


void listing_sort(struct listing_t *ls)
{
    int i, changed;
    struct record_t *re;

    do {
        changed = 0;

        for(i = 0; i < ls->entries - 1; i++) {
            if(record_cmp(ls->record[i], ls->record[i + 1]) > 0) {
                re = ls->record[i];
                ls->record[i] = ls->record[i + 1];
                ls->record[i + 1] = re;
                changed++;
            }
        }
    } while(changed);
}


/* Return true if the given record matches the given string. This
 * function defines what constitutes a 'match' */

static bool record_match(struct record_t *re, const char *match)
{
    if(strcasestr(re->artist, match) != NULL)
        return true;
    if(strcasestr(re->title, match) != NULL)
        return true;
    return false;
}


int listing_match(struct listing_t *src, struct listing_t *dest, char *match)
{
    int n;
    struct record_t *re;

    fprintf(stderr, "Matching '%s'\n", match);

    for(n = 0; n < src->entries; n++) {
        re = src->record[n];

        if(record_match(re, match)) {
            if(listing_add(dest, re) == -1)
                return -1;
        }
    }

    return 0;
}


void listing_debug(struct listing_t *ls)
{
    int n;

    for(n = 0; n < ls->entries; n++)
        fprintf(stderr, "%d: %s\n", n, ls->record[n]->pathname);
}
