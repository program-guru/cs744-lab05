/*
 * coordinator.c -- YOU IMPLEMENT THE TODOs IN THIS FILE.
 */
#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define POLL_MS 50

enum phase { PH_CP, PH_RECOVER_REDUCER, PH_RECOVER_MAPPER, PH_LAST_CP, PH_EXITING };

struct wstate {
    long last_hb;       /* time of the last heartbeat from the current process */
    int  heard;         /* current process has sent a heartbeat (its sockets are up) */
    long last_cp;       /* newest checkpoint this worker acknowledged */
    /* TODO: add whatever else you need per worker */
};

static const config_t *cfg;
static job_t         *job;
static struct wstate  ws[MAX_WORKERS];
static enum phase     phase;
static int            rid;              /* current recovery id */
static long           next_cp_id = 1;   /* never reused */

static const char *phase_name[] = {
    "CP", "RECOVER_REDUCER", "RECOVER_MAPPER", "LAST_CP", "EXITING",
};

static int is_mapper(int w) { return w < cfg->M; }

static const char *wname(int w)
{
    static char buf[4][32];
    static int k;
    char *b = buf[k++ % 4];
    snprintf(b, sizeof buf[0], "%s %d", is_mapper(w) ? "mapper" : "reducer",
             is_mapper(w) ? w : w - cfg->M);
    return b;
}

/* PROVIDED: change phase (and log it). Use this instead of assigning phase. */
static void set_phase(enum phase p)
{
    wlog("phase %s -> %s (rid %d)", phase_name[phase], phase_name[p], rid);
    phase = p;
}

/* Send a message of the given type to worker w, tagged with our recovery id. */
static void send_to(int w, int type, long cp)
{
    msg_t m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.role = ROLE_COORDINATOR;
    m.recovery_id = rid;
    m.checkpoint_id = cp;
    int role = is_mapper(w) ? ROLE_MAPPER : ROLE_REDUCER;
    int id = is_mapper(w) ? w : w - cfg->M;
    wlog("-> %s: %s cp=%ld rid=%d", wname(w), msg_type_name(type), cp, rid);
    udp_send(job->fd, worker_port(cfg->base_port, cfg->M, role, id), &m);
}

/*
 * TODO (Task 3)
 */
static void start_recovery(void)
{
}

/* One message from a worker. */
static void handle(const msg_t *m, long now)
{
    if ((m->role != ROLE_MAPPER && m->role != ROLE_REDUCER) || m->id < 0 ||
        m->id >= (m->role == ROLE_MAPPER ? cfg->M : cfg->R))
        return;
    int w = worker_index(cfg->M, m->role, m->id);

    if (m->type == MT_HEARTBEAT) {
        /* Ignore heartbeats from a process we already replaced. */
        if (m->count == (int64_t)job->pid[w]) {
            if (!ws[w].heard)
                wlog("%s is up (pid %ld)", wname(w), (long)m->count);
            ws[w].last_hb = now;
            ws[w].heard = 1;
        }
        return;
    }

    wlog("<- %s: %s cp=%ld rid=%d (mine: rid=%d, phase %s)", wname(w),
         msg_type_name(m->type), (long)m->checkpoint_id, m->recovery_id,
         rid, phase_name[phase]);

    /* TODO (Task 3): ignore messages from before the last rollback. */

    switch (m->type) {
    case MT_CHECKPOINT_ACK:
        /* TODO (Task 2) */
        break;
    case MT_LAST_CHECKPOINT_ACK:
        /* TODO (Task 1) */
        break;
    case MT_DONE:
        /* TODO (Task 1) */
        break;
    case MT_RECOVERY_ACK:
        /* TODO (Task 3) */
        break;
    default:
        break;
    }
}

/* Returns 0 when the job is complete; any other value is a fatal error. */
int coordinator_loop(const config_t *c, job_t *j)
{
    cfg = c;
    job = j;
    long now = now_ms();
    for (int w = 0; w < job->n; w++) {
        memset(&ws[w], 0, sizeof ws[w]);
        ws[w].last_hb = now;
        ws[w].last_cp = NO_CHECKPOINT;
    }
    phase = PH_CP;
    rid = 0;

    for (;;) {
        chaos_tick(job);        /* PROVIDED: do not remove */

        struct pollfd p = { .fd = job->fd, .events = POLLIN, .revents = 0 };
        if (poll(&p, 1, POLL_MS) < 0 && errno != EINTR) {
            perror("poll");
            return 1;
        }
        now = now_ms();

        msg_t m;
        int r;
        while ((r = udp_recv(job->fd, &m)) == 1)
            handle(&m, now);
        if (r < 0) {
            perror("udp_recv");
            return 1;
        }

        reap_children(job);     /* bookkeeping only */

        if (phase == PH_EXITING) {
            /* TODO (Task 1). */
            continue;
        }

        /* TODO (Task 3) */
        (void)start_recovery;

        /* TODO: phase actions. */
        (void)send_to;
        (void)set_phase;
        (void)next_cp_id;
    }
}
