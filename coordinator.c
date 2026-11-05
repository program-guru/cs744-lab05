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

enum phase { PH_CP,
             PH_RECOVER_REDUCER,
             PH_RECOVER_MAPPER,
             PH_LAST_CP,
             PH_EXITING };

struct wstate {
    long last_hb;       /* time of the last heartbeat from the current process */
    int heard;          /* current process has sent a heartbeat (its sockets are up) */
    long last_cp;       /* newest checkpoint this worker acknowledged */
    int done;           /* worker has sent MT_DONE */
    int is_last_cp_ack; /* worker has sent MT_LAST_CHECKPOINT_ACK */
};

static const config_t *cfg;
static job_t *job;
static struct wstate ws[MAX_WORKERS];
static enum phase phase;
static int rid;                   /* current recovery id */
static long next_cp_id = 1;       /* next checkpoint id to send to mappers */
static int is_cp_in_progress = 0; /* 1 if a checkpoint is in progress, 0 otherwise */
static long last_cp_time = 0;     /* time of the last checkpoint */

static const char *phase_name[] = {
    "CP",
    "RECOVER_REDUCER",
    "RECOVER_MAPPER",
    "LAST_CP",
    "EXITING",
};

static int is_mapper(int w) { return w < cfg->M; }

static const char *wname(int w) {
    static char buf[4][32];
    static int k;
    char *b = buf[k++ % 4];
    snprintf(b, sizeof buf[0], "%s %d", is_mapper(w) ? "mapper" : "reducer",
             is_mapper(w) ? w : w - cfg->M);
    return b;
}

/* PROVIDED: change phase (and log it). Use this instead of assigning phase. */
static void set_phase(enum phase p) {
    wlog("phase %s -> %s (rid %d)", phase_name[phase], phase_name[p], rid);
    phase = p;
}

/* Send a message of the given type to worker w, tagged with our recovery id. */
static void send_to(int w, int type, long cp) {
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
// static void start_recovery(void)
// {
//     for (int w = 0; w < job->n; w++) {
//         ws[w].done = 0;
//         ws[w].is_last_cp_ack = 0;
//     }

//     // TODO
// }

/* One message from a worker. */
static void handle(const msg_t *m, long now) {
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
        ws[w].last_cp = m->checkpoint_id;
        break;
    case MT_LAST_CHECKPOINT_ACK:
        ws[w].is_last_cp_ack = 1;
        break;
    case MT_DONE:
        ws[w].done = 1;
        break;
    case MT_RECOVERY_ACK:
        /* TODO (Task 3) */
        break;
    default:
        break;
    }
}

static void cp_phase_actions(long now) {
    if (is_cp_in_progress) {
        int is_cp_completed = 1;
        for (int w = 0; w < job->n; w++) {
            if (ws[w].last_cp != next_cp_id) {
                is_cp_completed = 0;
                break;
            }
        }

        if (is_cp_completed) {
            next_cp_id++;
            is_cp_in_progress = 0;
        }
        return;
    }

    int are_mappers_done = 1;
    for (int w = 0; w < cfg->M; w++) {
        if (!ws[w].done) {
            are_mappers_done = 0;
            break;
        }
    }

    if (are_mappers_done) {
        for (int w = 0; w < cfg->M; w++) {
            send_to(w, MT_CHECKPOINT, LAST_CHECKPOINT_ID);
        }

        set_phase(PH_LAST_CP);
        return;
    }

    if (cfg->interval_ms && now - last_cp_time >= cfg->interval_ms) {
        is_cp_in_progress = 1;
        last_cp_time = now;
    
        for (int w = 0; w < cfg->M; w++) {
            send_to(w, MT_CHECKPOINT, next_cp_id);
        }
    }
}

static void last_cp_phase_actions(void) {
    // Check if all workers have acknowledged the last checkpoint
    for (int w = 0; w < job->n; w++) {
        if (!ws[w].is_last_cp_ack) {
            return;
        }
    }

    // All workers have acknowledged the last checkpoint
    for (int w = 0; w < job->n; w++) {
        send_to(w, MT_EXIT, LAST_CHECKPOINT_ID);
    }

    wlog("all workers acked last checkpoint, exiting");
    set_phase(PH_EXITING);
}

static int exiting_phase_actions(void) {
    // Check if all workers have exited
    for (int w = 0; w < job->n; w++) {
        if (job->exited[w] == 0) {
            return 0;
        }
    }

    // All workers have exited
    wlog("all workers exited, coordinator exiting");
    return 1;
}

/* Returns 0 when the job is complete; any other value is a fatal error. */
int coordinator_loop(const config_t *c, job_t *j) {
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
    last_cp_time = now;

    for (;;) {
        chaos_tick(job); /* PROVIDED: do not remove */

        struct pollfd p = {.fd = job->fd, .events = POLLIN, .revents = 0};
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

        reap_children(job); /* bookkeeping only */

        switch (phase) {
        case PH_CP:
            cp_phase_actions(now);
            break;
        case PH_RECOVER_REDUCER:
            /* TODO (Task 3) */
            break;
        case PH_RECOVER_MAPPER:
            /* TODO (Task 3) */
            break;
        case PH_LAST_CP:
            last_cp_phase_actions();
            break;
        case PH_EXITING:
            if (exiting_phase_actions()) {
                return 0;
            }
            break;
        default:
            break;
        }
    }
}
