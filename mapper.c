/*
 * mapper.c -- YOU IMPLEMENT THE TODOs IN THIS FILE.
 */
#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const worker_args_t *args;
static int  coord_port;
static long offset;                 /* files fully processed and sent */
static int  rid;                    /* current recovery id; -1 = waiting for RECOVER */
static int  conn[MAX_REDUCERS];     /* TCP connection to each reducer, -1 if none */
static int  broken;                 /* a send failed: wait for RECOVER */
static int  done_sent;

/* Unexpected errors: die by signal; the coordinator will notice. */
static void fail(const char *what)
{
    wlog("fatal: %s: %s", what, strerror(errno));
    abort();
}

/* A message of the given type from this mapper, tagged with our recovery id. */
static msg_t make(int type)
{
    msg_t m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.role = ROLE_MAPPER;
    m.id = args->id;
    m.recovery_id = rid;
    return m;
}

static void to_coordinator(int type, long cp)
{
    msg_t m = make(type);
    m.checkpoint_id = cp;
    wlog("-> coordinator: %s cp=%ld rid=%d", msg_type_name(type), cp, rid);
    if (udp_send(args->ctl_fd, coord_port, &m) != 0)
        fail("udp_send");
}

static void disconnect_all(void)
{
    for (int r = 0; r < args->R; r++) {
        if (conn[r] >= 0)
            close(conn[r]);
        conn[r] = -1;
    }
}

static void connect_all(void)
{
    for (int r = 0; r < args->R; r++)
        conn[r] = tcp_connect_retry(worker_port(args->base_port, args->M, ROLE_REDUCER, r));
    wlog("connected to %d reducer(s)", args->R);
}

/* Send to reducer r. A failure means that reducer died or dropped our
 * connection while recovering; stop working until the coordinator tells us
 * to recover. */
static void to_reducer(int r, const msg_t *m)
{
    if (broken)
        return;
    if (m->type != MT_WORD_COUNT)   /* word counts are logged per file */
        wlog("-> reducer %d: %s cp=%ld rid=%d", r, msg_type_name(m->type),
             (long)m->checkpoint_id, m->recovery_id);
    if (send_msg(conn[r], m) != 0) {
        wlog("lost reducer %d (%s); waiting for RECOVER", r, strerror(errno));
        broken = 1;
    }
}

/*
 * TODO (Task 1, Task 2)
 */
static void take_checkpoint(long c)
{
    (void)c;
}

/*
 * TODO (Task 3)
 */
static void recover(long c, int new_rid)
{
    (void)c;
    (void)new_rid;
}

/* Handle every message waiting on the control socket. */
static void poll_control(void)
{
    msg_t m;
    int r;
    while ((r = udp_recv(args->ctl_fd, &m)) == 1) {
        wlog("<- coordinator: %s cp=%ld rid=%d (mine: rid=%d offset=%ld%s)",
             msg_type_name(m.type), (long)m.checkpoint_id, m.recovery_id,
             rid, offset, broken ? " broken" : "");
        switch (m.type) {
        case MT_CHECKPOINT:
            if (m.recovery_id == rid && !broken)
                take_checkpoint((long)m.checkpoint_id);
            else
                wlog("   ignored: stale recovery id or broken");
            break;
        case MT_RECOVER:
            recover((long)m.checkpoint_id, m.recovery_id);
            break;
        case MT_EXIT:
            wlog("exit");
            disconnect_all();
            exit(0);
        default:
            break;
        }
    }
    if (r < 0)
        fail("udp_recv");
}

static void wait_for_control(void)
{
    struct pollfd p = { .fd = args->ctl_fd, .events = POLLIN, .revents = 0 };
    poll(&p, 1, -1);                /* EINTR from heartbeats is fine */
}

static void process_file(void)
{
    wcmap_t *wc = count_file(args->files[offset]);
    if (!wc)
        fail(args->files[offset]);
    sleep_us(args->sleep_us);       /* simulated processing time */

    msg_t m = make(MT_WORD_COUNT);
    size_t pos = 0;
    const char *w;
    int64_t cnt;
    while (!broken && wcmap_next(wc, &pos, &w, &cnt)) {
        snprintf(m.word, sizeof m.word, "%s", w);
        m.count = cnt;
        to_reducer((int)(fnv1a(w) % (uint32_t)args->R), &m);
    }
    wcmap_free(wc);
    if (!broken) {
        offset++;
        wlog("sent file %ld/%d (%s)", offset, args->nfiles, args->files[offset - 1]);
    }
}

void mapper_main(const worker_args_t *a)
{
    args = a;
    coord_port = coordinator_port(a->base_port);
    for (int r = 0; r < MAX_REDUCERS; r++)
        conn[r] = -1;
    offset = 0;
    broken = 0;
    done_sent = 0;

    wlog("%d file(s) assigned", a->nfiles);
    if (a->recovering) {
        rid = -1;                   /* do nothing until RECOVER */
        wlog("restarted: waiting for RECOVER");
    } else {
        rid = 0;
        connect_all();
    }

    for (;;) {
        poll_control();
        if (rid < 0 || broken) {
            wait_for_control();
        } else if (offset < a->nfiles) {
            process_file();
        } else if (!done_sent) {
            to_coordinator(MT_DONE, 0);
            done_sent = 1;
        } else {
            wait_for_control();
        }
    }
}
