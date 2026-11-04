/*
 * check.c -- grading checker.
 */
#include "common.h"

#include <dirent.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int quiet;

struct diff {
    long    words;
    int64_t net;
    int64_t abs;
    char    example[160];
};

static void compare(const wcmap_t *exp, const wcmap_t *got, struct diff *d)
{
    memset(d, 0, sizeof *d);
    size_t pos = 0;
    const char *w;
    int64_t c;
    while (wcmap_next(exp, &pos, &w, &c)) {
        int64_t g = wcmap_get(got, w);
        if (g != c) {
            if (!d->words)
                snprintf(d->example, sizeof d->example,
                         "'%s' expected %" PRId64 " got %" PRId64, w, c, g);
            d->words++;
            d->net += g - c;
            d->abs += g > c ? g - c : c - g;
        }
    }
    pos = 0;
    while (wcmap_next(got, &pos, &w, &c)) {
        if (wcmap_get(exp, w) == 0) {
            if (!d->words)
                snprintf(d->example, sizeof d->example,
                         "'%s' expected 0 got %" PRId64, w, c);
            d->words++;
            d->net += c;
            d->abs += c > 0 ? c : -c;
        }
    }
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

static long *list_cp_ids(const char *ckpt, int *n_out)
{
    DIR *d = opendir(ckpt);
    long *ids = NULL;
    int n = 0, cap = 0;
    if (!d) {
        *n_out = 0;
        return NULL;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char kind;
        int wid, used = 0;
        long c;
        if (sscanf(de->d_name, "%c%d_%ld%n", &kind, &wid, &c, &used) != 3)
            continue;
        if ((kind != 'm' && kind != 'r') || de->d_name[used] != '\0' || c <= LAST_CHECKPOINT_ID)
            continue;
        int seen = 0;
        for (int i = 0; i < n; i++)
            if (ids[i] == c)
                seen = 1;
        if (seen)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            ids = realloc(ids, (size_t)cap * sizeof *ids);
            if (!ids)
                abort();
        }
        ids[n++] = c;
    }
    closedir(d);
    if (n)
        qsort(ids, (size_t)n, sizeof *ids, cmp_long);
    *n_out = n;
    return ids;
}

int main(int argc, char **argv)
{
    int check_cps = 0, min_cps = 0, M = 0, R = 0;
    static const struct option opts[] = {
        { "checkpoints",     no_argument,       NULL, 'c' },
        { "min-checkpoints", required_argument, NULL, 'n' },
        { NULL, 0, NULL, 0 },
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "m:r:q", opts, NULL)) != -1) {
        switch (ch) {
        case 'c': check_cps = 1; break;
        case 'n': min_cps = atoi(optarg); check_cps = 1; break;
        case 'm': M = atoi(optarg); break;
        case 'r': R = atoi(optarg); break;
        case 'q': quiet = 1; break;
        default:  goto usage;
        }
    }
    if (argc - optind != 2)
        goto usage;
    const char *in_dir = argv[optind], *ckpt = argv[optind + 1];
    char path[4096];

    if (M <= 0 || R <= 0) {
        snprintf(path, sizeof path, "%s/META", ckpt);
        FILE *f = fopen(path, "r");
        int fm = 0, fr = 0;
        if (!f || fscanf(f, "%d %d", &fm, &fr) != 2) {
            fprintf(stderr, "check: cannot read %s; pass -m and -r\n", path);
            return 2;
        }
        fclose(f);
        if (M <= 0) M = fm;
        if (R <= 0) R = fr;
    }
    if (M < 1 || M > MAX_MAPPERS || R < 1 || R > MAX_REDUCERS) {
        fprintf(stderr, "check: bad M=%d R=%d\n", M, R);
        return 2;
    }

    char **files;
    int nfiles = list_input_files(in_dir, &files);
    if (nfiles < 0) {
        perror(in_dir);
        return 2;
    }
    wcmap_t **per_file = calloc((size_t)nfiles + 1, sizeof *per_file);
    wcmap_t *expected = wcmap_new();
    for (int k = 0; k < nfiles; k++) {
        per_file[k] = count_file(files[k]);
        if (!per_file[k]) {
            perror(files[k]);
            return 2;
        }
        size_t pos = 0;
        const char *w;
        int64_t c;
        while (wcmap_next(per_file[k], &pos, &w, &c))
            wcmap_add(expected, w, c);
    }

    int ok = 1;

    wcmap_t *got = wcmap_new();
    long misrouted = 0;
    int missing_files = 0;
    for (int r = 0; r < R; r++) {
        snprintf(path, sizeof path, "%s/r%d_%d", ckpt, r, LAST_CHECKPOINT_ID);
        wcmap_t *part = wcmap_load(path);
        if (!part) {
            printf("output: missing or malformed %s\n", path);
            missing_files++;
            continue;
        }
        size_t pos = 0;
        const char *w;
        int64_t c;
        while (wcmap_next(part, &pos, &w, &c)) {
            if (fnv1a(w) % (uint32_t)R != (uint32_t)r)
                misrouted++;
            wcmap_add(got, w, c);
        }
        wcmap_free(part);
    }
    struct diff d;
    compare(expected, got, &d);
    if (missing_files || d.words || misrouted) {
        ok = 0;
        printf("output: WRONG: %ld word(s) differ, net %+" PRId64 ", abs %" PRId64
               ", %ld misrouted%s%s\n",
               d.words, d.net, d.abs, misrouted, d.words ? "; e.g. " : "", d.example);
    } else if (!quiet) {
        printf("output: OK (%zu distinct words)\n", wcmap_size(expected));
    }

    if (check_cps) {
        int n_ids;
        long *ids = list_cp_ids(ckpt, &n_ids);
        int complete = 0, bad = 0;

        for (int t = 0; t < n_ids; t++) {
            long c = ids[t];
            int all = 1;
            for (int i = 0; i < M && all; i++) {
                snprintf(path, sizeof path, "%s/m%d_%ld", ckpt, i, c);
                all = (access(path, F_OK) == 0);
            }
            for (int r = 0; r < R && all; r++) {
                snprintf(path, sizeof path, "%s/r%d_%ld", ckpt, r, c);
                all = (access(path, F_OK) == 0);
            }
            if (!all)
                continue;
            complete++;

            wcmap_t *exp_r[MAX_REDUCERS];
            for (int r = 0; r < R; r++)
                exp_r[r] = wcmap_new();
            int cp_ok = 1;
            for (int i = 0; i < M; i++) {
                long off;
                snprintf(path, sizeof path, "%s/m%d_%ld", ckpt, i, c);
                int mine = 0;
                for (int k = i; k < nfiles; k += M)
                    mine++;
                if (load_int(path, &off) != 0 || off < 0 || off > mine) {
                    printf("checkpoint %ld: INCONSISTENT: bad mapper offset in %s\n", c, path);
                    cp_ok = 0;
                    break;
                }
                for (long j = 0; j < off; j++) {
                    size_t pos = 0;
                    const char *w;
                    int64_t cnt;
                    const wcmap_t *fm = per_file[i + j * M];
                    while (wcmap_next(fm, &pos, &w, &cnt))
                        wcmap_add(exp_r[fnv1a(w) % (uint32_t)R], w, cnt);
                }
            }
            for (int r = 0; r < R && cp_ok; r++) {
                snprintf(path, sizeof path, "%s/r%d_%ld", ckpt, r, c);
                wcmap_t *saved = wcmap_load(path);
                if (!saved) {
                    printf("checkpoint %ld: INCONSISTENT: malformed %s\n", c, path);
                    cp_ok = 0;
                    break;
                }
                struct diff cd;
                compare(exp_r[r], saved, &cd);
                wcmap_free(saved);
                if (cd.words) {
                    printf("checkpoint %ld: INCONSISTENT: reducer %d differs in %ld word(s), "
                           "net %+" PRId64 "; e.g. %s\n", c, r, cd.words, cd.net, cd.example);
                    cp_ok = 0;
                }
            }
            for (int r = 0; r < R; r++)
                wcmap_free(exp_r[r]);
            if (!cp_ok)
                bad++;
        }
        free(ids);

        if (bad)
            ok = 0;
        if (complete < min_cps) {
            printf("checkpoints: only %d complete, need at least %d\n", complete, min_cps);
            ok = 0;
        }
        printf("checkpoints: %d complete, %d inconsistent\n", complete, bad);
    }

    for (int k = 0; k < nfiles; k++)
        wcmap_free(per_file[k]);
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;

usage:
    fprintf(stderr, "usage: %s [--checkpoints] [--min-checkpoints N] [-m M] [-r R] [-q] "
                    "INPUT_DIR CKPT_DIR\n", argv[0]);
    return 2;
}
