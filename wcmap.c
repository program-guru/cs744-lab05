/*
 * wcmap.c -- open-addressing hash map from words to counts.
 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct entry {
    char    word[MAX_WORD];
    int64_t count;
    int     used;
};

struct wcmap {
    struct entry *e;
    size_t        cap;      /* power of two */
    size_t        size;
};

static void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);
    if (!p) {
        perror("calloc");
        abort();
    }
    return p;
}

wcmap_t *wcmap_new(void)
{
    wcmap_t *m = xcalloc(1, sizeof *m);
    m->cap = 64;
    m->e = xcalloc(m->cap, sizeof *m->e);
    return m;
}

void wcmap_free(wcmap_t *m)
{
    if (!m)
        return;
    free(m->e);
    free(m);
}

static size_t slot_for(const struct entry *e, size_t cap, const char *w)
{
    size_t i = fnv1a(w) & (cap - 1);
    while (e[i].used && strcmp(e[i].word, w) != 0)
        i = (i + 1) & (cap - 1);
    return i;
}

static void grow(wcmap_t *m)
{
    size_t ncap = m->cap * 2;
    struct entry *ne = xcalloc(ncap, sizeof *ne);
    for (size_t i = 0; i < m->cap; i++)
        if (m->e[i].used)
            ne[slot_for(ne, ncap, m->e[i].word)] = m->e[i];
    free(m->e);
    m->e = ne;
    m->cap = ncap;
}

void wcmap_add(wcmap_t *m, const char *word, int64_t n)
{
    char w[MAX_WORD];
    snprintf(w, sizeof w, "%s", word);
    if ((m->size + 1) * 2 > m->cap)
        grow(m);
    size_t i = slot_for(m->e, m->cap, w);
    if (!m->e[i].used) {
        memcpy(m->e[i].word, w, sizeof w);
        m->e[i].used = 1;
        m->e[i].count = 0;
        m->size++;
    }
    m->e[i].count += n;
}

int64_t wcmap_get(const wcmap_t *m, const char *word)
{
    char w[MAX_WORD];
    snprintf(w, sizeof w, "%s", word);
    size_t i = slot_for(m->e, m->cap, w);
    return m->e[i].used ? m->e[i].count : 0;
}

size_t wcmap_size(const wcmap_t *m) { return m->size; }

int wcmap_next(const wcmap_t *m, size_t *pos, const char **word, int64_t *count)
{
    while (*pos < m->cap) {
        const struct entry *e = &m->e[(*pos)++];
        if (e->used) {
            *word = e->word;
            *count = e->count;
            return 1;
        }
    }
    return 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const struct entry *x = *(const struct entry *const *)a;
    const struct entry *y = *(const struct entry *const *)b;
    return strcmp(x->word, y->word);
}

int wcmap_save(const wcmap_t *m, const char *path)
{
    const struct entry **v = xcalloc(m->size ? m->size : 1, sizeof *v);
    size_t n = 0;
    for (size_t i = 0; i < m->cap; i++)
        if (m->e[i].used)
            v[n++] = &m->e[i];
    qsort(v, n, sizeof *v, cmp_entry);

    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(v);
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%s %" PRId64 "\n", v[i]->word, v[i]->count);
    free(v);
    if (fclose(f) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    return 0;
}

wcmap_t *wcmap_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    wcmap_t *m = wcmap_new();
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char w[MAX_WORD];
        int64_t c;
        if (sscanf(line, "%47s %" SCNd64, w, &c) != 2) {
            fclose(f);
            wcmap_free(m);
            return NULL;
        }
        wcmap_add(m, w, c);
    }
    fclose(f);
    return m;
}
