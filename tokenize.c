/*
 * tokenize.c -- split a file into words and count them.
 */
#include "common.h"

#include <stdio.h>

static int is_letter(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int lower(int c)     { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

wcmap_t *count_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    wcmap_t *m = wcmap_new();
    char w[MAX_WORD];
    size_t len = 0;
    int in_word = 0;
    int c;

    while ((c = getc(f)) != EOF) {
        if (is_letter(c)) {
            if (len < MAX_WORD - 1)
                w[len++] = (char)lower(c);
            in_word = 1;
        } else if (in_word) {
            w[len] = '\0';
            wcmap_add(m, w, 1);
            len = 0;
            in_word = 0;
        }
    }
    if (in_word) {
        w[len] = '\0';
        wcmap_add(m, w, 1);
    }

    if (ferror(f)) {
        fclose(f);
        wcmap_free(m);
        return NULL;
    }
    fclose(f);
    return m;
}
