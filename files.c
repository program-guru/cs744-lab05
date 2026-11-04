/*
 * files.c -- small atomic files and input listing.
 */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int save_int(const char *path, long value)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    char buf[32];
    int n = snprintf(buf, sizeof buf, "%ld\n", value);
    ssize_t w = write(fd, buf, (size_t)n);
    int e = errno;
    if (close(fd) != 0 || w != n) {
        unlink(tmp);
        errno = (w != n) ? e : errno;
        return -1;
    }
    if (rename(tmp, path) != 0) {
        e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    return 0;
}

int load_int(const char *path, long *value)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    long v;
    int ok = (fscanf(f, "%ld", &v) == 1);
    fclose(f);
    if (!ok)
        return -1;
    *value = v;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int list_input_files(const char *dir, char ***files)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;

    char **v = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        size_t len = strlen(dir) + strlen(de->d_name) + 2;
        char *p = malloc(len);
        if (!p)
            abort();
        snprintf(p, len, "%s/%s", dir, de->d_name);

        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(p);
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            v = realloc(v, (size_t)cap * sizeof *v);
            if (!v)
                abort();
        }
        v[n++] = p;
    }
    closedir(d);

    if (n > 0)
        qsort(v, (size_t)n, sizeof *v, cmp_str);
    *files = v;
    return n;
}

int ensure_dir(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}
