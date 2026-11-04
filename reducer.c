/*
 * reducer.c -- YOU IMPLEMENT THE TODOs IN THIS FILE.
 */
#include "common.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CONN (4 * MAX_MAPPERS)

static const worker_args_t *args;
static int      coord_port;
static wcmap_t *counts;
static int      rid;                /* current recovery id; -1 = waiting for RECOVER */

static int  conn_fd[MAX_CONN];
static int  conn_mapper[MAX_CONN];  /* -1 until known */
static int  nconn;
static int  blocked[MAX_MAPPERS];
static int  nblocked;

static void fail(const char *what)
{
    wlog("fatal: %s: %s", what, strerror(errno));
    abort();
}

static void to_coordinator(int type, long cp)
{
    msg_t m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.role = ROLE_REDUCER;
    m.id = args->id;
    m.recovery_id = rid;
    m.checkpoint_id = cp;
    wlog("-> coordinator: %s cp=%ld rid=%d", msg_type_name(type), cp, rid);
    if (udp_send(args->ctl_fd, coord_port, &m) != 0)
        fail("udp_send");
}

static void close_conn(int i)
{
    if (conn_mapper[i] >= 0)
        wlog("closed connection from mapper %d", conn_mapper[i]);
    else
        wlog("closed connection from unknown mapper");
    close(conn_fd[i]);
    conn_fd[i] = conn_fd[nconn - 1];
    conn_mapper[i] = conn_mapper[nconn - 1];
    nconn--;
}

/*
 * TODO (Task 3)
 */
static void recover(long c, int new_rid)
{
    (void)c;
    (void)new_rid;
    (void)to_coordinator;           /* TODO: remove */
    (void)nblocked;
}

static void handle_control(void)
{
    msg_t m;
    int r;
    while ((r = udp_recv(args->ctl_fd, &m)) == 1) {
        wlog("<- coordinator: %s cp=%ld rid=%d (mine: rid=%d, %d blocked)",
             msg_type_name(m.type), (long)m.checkpoint_id, m.recovery_id, rid, nblocked);
        if (m.type == MT_RECOVER)
            recover((long)m.checkpoint_id, m.recovery_id);
        else if (m.type == MT_EXIT) {
            size_t pos = 0;
            const char *w;
            int64_t c, total = 0;
            while (wcmap_next(counts, &pos, &w, &c))
                total += c;
            wlog("exit: %zu distinct words, %lld words in total",
                 wcmap_size(counts), (long long)total);
            exit(0);
        }
    }
    if (r < 0)
        fail("udp_recv");
}

/*
 * TODO (Task 3)
 */
static int is_stale(const msg_t *m)
{
    (void)m;
    return 0;
}

/*
 * TODO (Task 1, Task 2)
 */
static void handle_marker(int j, long c)
{
    (void)j;
    (void)c;
}

/* PROVIDED: one message from connection i. */
static void handle_data(int i, const msg_t *in)
{
    msg_t m = *in;
    if (m.id < 0 || m.id >= args->M)
        return;
    if (is_stale(&m)) {
        if (m.type != MT_WORD_COUNT)
            wlog("dropped stale %s from mapper %d (its rid=%d, mine=%d)",
                 msg_type_name(m.type), m.id, m.recovery_id, rid);
        return;
    }
    if (conn_mapper[i] != m.id)
        wlog("connection %d is mapper %d", i, m.id);
    conn_mapper[i] = m.id;          /* learn which mapper this connection is */

    switch (m.type) {
    case MT_WORD_COUNT:
        m.word[MAX_WORD - 1] = '\0';
        wcmap_add(counts, m.word, m.count);
        break;
    case MT_FWD_CHECKPOINT:
        wlog("<- mapper %d: FWD_CHECKPOINT cp=%ld rid=%d", m.id,
             (long)m.checkpoint_id, m.recovery_id);
        handle_marker(m.id, (long)m.checkpoint_id);
        break;
    default:
        break;
    }
}

void reducer_main(const worker_args_t *a)
{
    args = a;
    coord_port = coordinator_port(a->base_port);
    counts = wcmap_new();
    rid = a->recovering ? -1 : 0;
    if (a->recovering)
        wlog("restarted: waiting for RECOVER");

    for (;;) {
        struct pollfd pfd[2 + MAX_CONN];
        int which[2 + MAX_CONN];
        int n = 0;
        pfd[n] = (struct pollfd){ .fd = a->ctl_fd, .events = POLLIN };
        which[n++] = -1;
        pfd[n] = (struct pollfd){ .fd = a->listen_fd, .events = POLLIN };
        which[n++] = -2;
        for (int i = 0; i < nconn; i++) {
            if (conn_mapper[i] >= 0 && blocked[conn_mapper[i]])
                continue;           /* leave its messages in the socket for now */
            pfd[n] = (struct pollfd){ .fd = conn_fd[i], .events = POLLIN };
            which[n++] = i;
        }

        if (poll(pfd, (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue;
            fail("poll");
        }

        int rid_before = rid;
        if (pfd[0].revents)
            handle_control();

        for (int k = n - 1; k >= 2 && rid == rid_before; k--) {
            if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int i = which[k];
            if (conn_mapper[i] >= 0 && blocked[conn_mapper[i]])
                continue;
            msg_t m;
            int r = recv_msg(conn_fd[i], &m);
            if (r <= 0)
                close_conn(i);      /* mapper died or reconnected */
            else
                handle_data(i, &m);
        }

        if (pfd[1].revents) {
            int fd;
            while ((fd = tcp_accept(a->listen_fd)) >= 0) {
                if (nconn == MAX_CONN) {
                    close(fd);
                    continue;
                }
                conn_fd[nconn] = fd;
                conn_mapper[nconn] = -1;
                nconn++;
                wlog("accepted a connection (%d open)", nconn);
            }
        }
    }
}
