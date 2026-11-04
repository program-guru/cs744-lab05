/*
 * chaos.c -- failure injection.
 */
#include "common.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEFAULT_MIN_MS        2000
#define DEFAULT_MAX_MS        4000
#define BLOCKED_KILL_GRACE_MS 1500

struct chaos_shared {
    int pending_blocked_kill;
};

static struct chaos_shared *sh;
static int      my_role = -1;

/* Coordinator-side state. */
static int      M_, R_, mode;
static int      events_left, kills_done;
static int      event_no, blocked_event_no = -1;
static long     next_event_at = -1;
static long     pending_since = -1;
static long     min_ms = DEFAULT_MIN_MS, max_ms = DEFAULT_MAX_MS;
static uint64_t rng;

static uint32_t rnd(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (uint32_t)((rng * 2685821657736338717ULL) >> 32);
}

static long env_long(const char *name, long def)
{
    const char *s = getenv(name);
    return (s && *s) ? atol(s) : def;
}

static long random_delay(void)
{
    long span = max_ms > min_ms ? max_ms - min_ms : 0;
    return min_ms + (span ? (long)(rnd() % (uint32_t)(span + 1)) : 0);
}

void chaos_init(const config_t *cfg)
{
    M_ = cfg->M;
    R_ = cfg->R;
    mode = cfg->chaos_mode;
    events_left = cfg->kills;
    if (cfg->kill_blocked && events_left < 1)
        events_left = 1;
    rng = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)cfg->seed * 0xBF58476D1CE4E5B9ULL);
    if (rng == 0)
        rng = 1;
    for (int i = 0; i < 4; i++)
        rnd();
    min_ms = env_long("CHAOS_MIN_MS", DEFAULT_MIN_MS);
    max_ms = env_long("CHAOS_MAX_MS", DEFAULT_MAX_MS);
    if (cfg->kill_blocked)
        blocked_event_no = (int)(rnd() % (uint32_t)events_left);

    sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) {
        sh = NULL;
        events_left = 0;
    }
    if (events_left > 0)
        next_event_at = now_ms() + random_delay();
}

void chaos_worker_init(int role, int id)
{
    (void)id;
    my_role = role;
}

void chaos_report_blocked(int n)
{
    if (!sh || my_role != ROLE_REDUCER || n <= 0)
        return;
    if (__atomic_exchange_n(&sh->pending_blocked_kill, 0, __ATOMIC_SEQ_CST) == 1) {
        wlog("chaos: killing myself with %d blocked channel(s)", n);
        kill(getpid(), SIGKILL);
    }
}

static int pick_live(const job_t *job, int first, int count)
{
    int live[MAX_WORKERS], n = 0;
    for (int w = first; w < first + count; w++)
        if (!job->exited[w] && job->pid[w] > 0)
            live[n++] = w;
    return n ? live[rnd() % (uint32_t)n] : -1;
}

static void do_kill(job_t *job, int w)
{
    if (w < 0)
        return;
    wlog("chaos: SIGKILL %s %d (pid %d)",
         w < M_ ? "mapper" : "reducer", w < M_ ? w : w - M_, (int)job->pid[w]);
    kill(job->pid[w], SIGKILL);
    kills_done++;
}

void chaos_tick(job_t *job)
{
    if (!sh)
        return;
    long now = now_ms();

    if (pending_since >= 0) {
        if (!__atomic_load_n(&sh->pending_blocked_kill, __ATOMIC_SEQ_CST)) {
            kills_done++;
            pending_since = -1;
        } else if (now - pending_since > BLOCKED_KILL_GRACE_MS) {
            pending_since = -1;
            if (__atomic_exchange_n(&sh->pending_blocked_kill, 0, __ATOMIC_SEQ_CST) == 1)
                do_kill(job, pick_live(job, M_, R_));
            else
                kills_done++;
        }
        return;
    }

    if (events_left <= 0 || next_event_at < 0 || now < next_event_at)
        return;
    events_left--;
    next_event_at = now + random_delay();

    if (event_no++ == blocked_event_no) {
        wlog("chaos: next reducer to block will be killed");
        pending_since = now;
        __atomic_store_n(&sh->pending_blocked_kill, 1, __ATOMIC_SEQ_CST);
        return;
    }
    switch (mode) {
    case CHAOS_MAPPER:  do_kill(job, pick_live(job, 0, M_)); break;
    case CHAOS_REDUCER: do_kill(job, pick_live(job, M_, R_)); break;
    case CHAOS_BOTH:
        do_kill(job, pick_live(job, 0, M_));
        do_kill(job, pick_live(job, M_, R_));
        break;
    case CHAOS_ALL:
        for (int w = 0; w < M_ + R_; w++)
            if (!job->exited[w] && job->pid[w] > 0)
                do_kill(job, w);
        break;
    default:            do_kill(job, pick_live(job, 0, M_ + R_)); break;
    }
}

int chaos_kills(void) { return kills_done; }
