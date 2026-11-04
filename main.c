/*
 * main.c -- argument parsing and startup for wcjob.
 */
#include "common.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-m M] [-r R] -i INPUT_DIR [-t INTERVAL_MS] [-s SLEEP_US] [-p PORT]\n"
            "          [-k KILLS] [--chaos MODE] [--seed N] [--kill-blocked] [--no-align]\n"
            "\n"
            "  -m M            number of mappers (default 3)\n"
            "  -r R            number of reducers (default 2)\n"
            "  -i DIR          input directory\n"
            "  -t MS           checkpoint interval in ms; 0 = only the last checkpoint (default 500)\n"
            "  -s US           simulated processing time per file, in microseconds (default 0)\n"
            "  -p PORT         coordinator UDP port; workers use the next M+R ports (default %d)\n"
            "  -k N            number of chaos kill events (default 0)\n"
            "  --chaos MODE    what a kill event kills: any, mapper, reducer, both, all (default any)\n"
            "  --seed N        chaos seed (default 1)\n"
            "  --kill-blocked  one kill event targets a reducer with a blocked channel\n"
            "  --no-align      reducers checkpoint on the first marker (Part 4)\n"
            "\n"
            "Set WC_DEBUG=1 for debug logging on stderr.\n",
            prog, DEFAULT_BASE_PORT);
    exit(2);
}

static int parse_mode(const char *s)
{
    static const char *names[] = { "any", "mapper", "reducer", "both", "all" };
    for (int i = 0; i < 5; i++)
        if (strcmp(s, names[i]) == 0)
            return i;
    return -1;
}

/* Print the job's output: the final checkpoint of every reducer (ckpt/r<i>_0). */
static void print_final_counts(int R)
{
    struct row { const char *word; int64_t count; int reducer; };
    struct row rows[4096];
    wcmap_t *maps[MAX_REDUCERS] = { 0 };
    int n = 0;
    int64_t total = 0;

    for (int r = 0; r < R; r++) {
        char path[64];
        snprintf(path, sizeof path, CKPT_DIR "/r%d_%d", r, LAST_CHECKPOINT_ID);
        maps[r] = wcmap_load(path);
        if (!maps[r]) {
            printf("wcjob: cannot read %s\n", path);
            continue;
        }
        size_t pos = 0;
        const char *w;
        int64_t c;
        while (n < (int)(sizeof rows / sizeof rows[0]) && wcmap_next(maps[r], &pos, &w, &c)) {
            rows[n++] = (struct row){ w, c, r };
            total += c;
        }
    }

    /* Sort by word (insertion sort; the vocabulary is small). */
    for (int i = 1; i < n; i++) {
        struct row x = rows[i];
        int j = i - 1;
        while (j >= 0 && strcmp(rows[j].word, x.word) > 0) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = x;
    }

    printf("\nFinal word counts (" CKPT_DIR "/r*_%d):\n\n", LAST_CHECKPOINT_ID);
    printf("  %-16s %10s  %s\n", "word", "count", "reducer");
    for (int i = 0; i < n; i++)
        printf("  %-16s %10lld  %d\n", rows[i].word, (long long)rows[i].count, rows[i].reducer);
    printf("\n  %d distinct words, %lld words in total\n", n, (long long)total);

    for (int r = 0; r < R; r++)
        if (maps[r])
            wcmap_free(maps[r]);
}

int main(int argc, char **argv)
{
    config_t cfg = {
        .M = 3, .R = 2, .input_dir = NULL, .interval_ms = 500, .sleep_us = 0,
        .base_port = DEFAULT_BASE_PORT, .kills = 0, .chaos_mode = CHAOS_ANY,
        .seed = 1, .kill_blocked = 0, .no_align = 0,
    };

    static const struct option opts[] = {
        { "chaos",        required_argument, NULL, 'C' },
        { "seed",         required_argument, NULL, 'S' },
        { "kill-blocked", no_argument,       NULL, 'B' },
        { "no-align",     no_argument,       NULL, 'N' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "m:r:i:t:s:p:k:h", opts, NULL)) != -1) {
        switch (c) {
        case 'm': cfg.M = atoi(optarg); break;
        case 'r': cfg.R = atoi(optarg); break;
        case 'i': cfg.input_dir = optarg; break;
        case 't': cfg.interval_ms = atoi(optarg); break;
        case 's': cfg.sleep_us = atoi(optarg); break;
        case 'p': cfg.base_port = atoi(optarg); break;
        case 'k': cfg.kills = atoi(optarg); break;
        case 'C':
            cfg.chaos_mode = parse_mode(optarg);
            if (cfg.chaos_mode < 0)
                usage(argv[0]);
            break;
        case 'S': cfg.seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'B': cfg.kill_blocked = 1; break;
        case 'N': cfg.no_align = 1; break;
        default:  usage(argv[0]);
        }
    }
    if (optind != argc || !cfg.input_dir)
        usage(argv[0]);
    if (cfg.M < 1 || cfg.M > MAX_MAPPERS || cfg.R < 1 || cfg.R > MAX_REDUCERS) {
        fprintf(stderr, "wcjob: need 1 <= M <= %d and 1 <= R <= %d\n", MAX_MAPPERS, MAX_REDUCERS);
        return 2;
    }
    if (cfg.interval_ms < 0 || cfg.sleep_us < 0 || cfg.kills < 0 ||
        cfg.base_port < 1024 || cfg.base_port + cfg.M + cfg.R > 65535)
        usage(argv[0]);

    cfg.nfiles = list_input_files(cfg.input_dir, &cfg.files);
    if (cfg.nfiles < 0) {
        perror(cfg.input_dir);
        return 1;
    }

    if (ensure_dir(CKPT_DIR) != 0) {
        perror("mkdir " CKPT_DIR);
        return 1;
    }
    char path[64];
    snprintf(path, sizeof path, CKPT_DIR "/r0_%d", LAST_CHECKPOINT_ID);
    if (access(path, F_OK) == 0)
        fprintf(stderr, "wcjob: warning: %s exists from an earlier run; run 'make clean' first\n",
                path);

    /* Job shape, for ./check. */
    FILE *meta = fopen(META_PATH, "w");
    if (!meta) {
        perror(META_PATH);
        return 1;
    }
    fprintf(meta, "%d %d\n", cfg.M, cfg.R);
    fclose(meta);

    signal(SIGPIPE, SIG_IGN);

    job_t job;
    memset(&job, 0, sizeof job);
    job.fd = udp_bind(coordinator_port(cfg.base_port));
    if (job.fd < 0) {
        fprintf(stderr, "wcjob: cannot bind UDP port %d: %s\n",
                coordinator_port(cfg.base_port), strerror(errno));
        return 1;
    }

    chaos_init(&cfg);
    if (spawn_all(&job, &cfg) != 0) {
        kill_all(&job);
        return 1;
    }

    int rc = coordinator_loop(&cfg, &job);
    kill_all(&job);                 /* nothing should be left; just in case */
    fprintf(stderr, "wcjob: %s (chaos kills: %d)\n",
            rc == 0 ? "job complete" : "coordinator failed", chaos_kills());
    if (rc == 0)
        print_final_counts(cfg.R);
    return rc;
}
