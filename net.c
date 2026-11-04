/*
 * net.c -- sockets, time, hashing and debug logging.
 */
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

int coordinator_port(int base_port) { return base_port; }

int worker_port(int base_port, int M, int role, int id)
{
    return base_port + 1 + (role == ROLE_MAPPER ? id : M + id);
}

int worker_index(int M, int role, int id) { return role == ROLE_MAPPER ? id : M + id; }

static struct sockaddr_in loopback(int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* UDP helpers */
int udp_bind(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = loopback(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || set_nonblock(fd) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int udp_send(int fd, int port, const msg_t *m)
{
    struct sockaddr_in a = loopback(port);
    for (;;) {
        ssize_t n = sendto(fd, m, sizeof *m, 0, (struct sockaddr *)&a, sizeof a);
        if (n == (ssize_t)sizeof *m)
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = fd, .events = POLLOUT, .revents = 0 };
            poll(&p, 1, 10);
            continue;
        }
        return -1;
    }
}

int udp_recv(int fd, msg_t *m)
{
    for (;;) {
        ssize_t n = recv(fd, m, sizeof *m, 0);
        if (n == (ssize_t)sizeof *m)
            return 1;
        if (n >= 0)
            continue;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        if (errno == ECONNREFUSED)
            continue;
        return -1;
    }
}

/* TCP helpers */
int tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = loopback(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 64) != 0 ||
        set_nonblock(fd) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int tcp_accept(int listen_fd)
{
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd >= 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            return fd;
        }
        if (errno == EINTR || errno == ECONNABORTED)
            continue;
        return -1;
    }
}

int tcp_connect(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a = loopback(port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        if (errno != EINTR && errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        /* Interrupted by a signal: the connection continues in the background. */
        struct pollfd p = { .fd = fd, .events = POLLOUT, .revents = 0 };
        while (poll(&p, 1, -1) < 0 && errno == EINTR)
            ;
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

int tcp_connect_retry(int port)
{
    for (;;) {
        int fd = tcp_connect(port);
        if (fd >= 0)
            return fd;
        sleep_us(50000);
    }
}

int send_msg(int fd, const msg_t *m)
{
    size_t done = 0;
    while (done < sizeof *m) {
        ssize_t w = send(fd, (const char *)m + done, sizeof *m - done, MSG_NOSIGNAL);
        if (w > 0)
            done += (size_t)w;
        else if (w < 0 && errno == EINTR)
            continue;
        else
            return -1;
    }
    return 0;
}

int recv_msg(int fd, msg_t *m)
{
    size_t got = 0;
    while (got < sizeof *m) {
        ssize_t r = recv(fd, (char *)m + got, sizeof *m - got, 0);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            if (got == 0)
                return 0;
            errno = EIO;
            return -1;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
            poll(&p, 1, -1);
        } else {
            return -1;
        }
    }
    return 1;
}

/* UDS helpers */
static int unix_addr(const char *path, struct sockaddr_un *a)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a->sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(a->sun_path, path);
    return 0;
}

int unix_dgram_bind(const char *path)
{
    struct sockaddr_un a;
    if (unix_addr(path, &a) != 0)
        return -1;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int unix_dgram_send(int fd, const char *path, const msg_t *m)
{
    struct sockaddr_un a;
    if (unix_addr(path, &a) != 0)
        return -1;
    for (;;) {
        ssize_t n = sendto(fd, m, sizeof *m, 0, (struct sockaddr *)&a, sizeof a);
        if (n == (ssize_t)sizeof *m)
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
}

/* Time and hashing helpers */
const char *msg_type_name(int type)
{
    static const char *names[] = {
        "HEARTBEAT", "CHECKPOINT", "FWD_CHECKPOINT", "WORD_COUNT", "CHECKPOINT_ACK",
        "EXIT", "DONE", "LAST_CHECKPOINT_ACK", "RECOVER", "RECOVERY_ACK",
    };
    if (type < 0 || type >= (int)(sizeof names / sizeof names[0]))
        return "UNKNOWN";
    return names[type];
}

long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_us(long us)
{
    struct timespec req = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };
    struct timespec rem;
    while (nanosleep(&req, &rem) != 0 && errno == EINTR)
        req = rem;
}

uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

/* Logging helpers */
static char log_role[16] = "coord";
static int  log_id = -1;
static int  log_enabled = -1;

void log_init(const char *role, int id)
{
    snprintf(log_role, sizeof log_role, "%s", role);
    log_id = id;
}

void wlog(const char *fmt, ...)
{
    if (log_enabled < 0) {
        const char *e = getenv("WC_DEBUG");
        log_enabled = (e && *e && strcmp(e, "0") != 0);
    }
    if (!log_enabled)
        return;

    char buf[512];
    long ms = now_ms() % 100000000;
    int n;
    if (log_id >= 0)
        n = snprintf(buf, sizeof buf, "%8ld [%s %d] ", ms, log_role, log_id);
    else
        n = snprintf(buf, sizeof buf, "%8ld [%s] ", ms, log_role);

    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n++] = '\n';

    int saved = errno;
    ssize_t w = write(STDERR_FILENO, buf, (size_t)n);
    (void)w;
    errno = saved;
}
