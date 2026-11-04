/*
 * worker.c -- starting, restarting and stopping worker processes.
 */
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int                hb_fd = -1;
static msg_t              hb_msg;
static struct sockaddr_in hb_addr;

static void send_heartbeat(void)
{
    int saved = errno;
    ssize_t n = sendto(hb_fd, &hb_msg, sizeof hb_msg, 0,
                       (struct sockaddr *)&hb_addr, sizeof hb_addr);
    (void)n;
    errno = saved;
}

static void on_alarm(int sig)
{
    (void)sig;
    send_heartbeat();
}

static void start_heartbeats(const config_t *cfg, int role, int id)
{
    hb_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (hb_fd < 0)
        abort();
    memset(&hb_msg, 0, sizeof hb_msg);
    hb_msg.type = MT_HEARTBEAT;
    hb_msg.role = role;
    hb_msg.id = id;
    hb_msg.count = getpid();        /* lets the coordinator ignore replaced processes */
    memset(&hb_addr, 0, sizeof hb_addr);
    hb_addr.sin_family = AF_INET;
    hb_addr.sin_port = htons((uint16_t)coordinator_port(cfg->base_port));
    hb_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    send_heartbeat();
    struct itimerval it = {
        .it_interval = { .tv_sec = 0, .tv_usec = HEARTBEAT_INTERVAL_MS * 1000 },
        .it_value    = { .tv_sec = 0, .tv_usec = HEARTBEAT_INTERVAL_MS * 1000 },
    };
    setitimer(ITIMER_REAL, &it, NULL);
}

static int bind_retry(int (*fn)(int), int port)
{
    for (int i = 0; i < 100; i++) {
        int fd = fn(port);
        if (fd >= 0)
            return fd;
        sleep_us(20000);
    }
    return -1;
}

static void run_worker(const config_t *cfg, job_t *job, int w, int recovering, pid_t parent)
{
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent)
        _exit(0);
    close(job->fd);
    signal(SIGPIPE, SIG_IGN);

    worker_args_t a;
    memset(&a, 0, sizeof a);
    a.M = cfg->M;
    a.R = cfg->R;
    a.base_port = cfg->base_port;
    a.recovering = recovering;
    a.sleep_us = cfg->sleep_us;
    a.no_align = cfg->no_align;
    a.listen_fd = -1;
    if (w < cfg->M) {
        a.role = ROLE_MAPPER;
        a.id = w;
    } else {
        a.role = ROLE_REDUCER;
        a.id = w - cfg->M;
    }

    log_init(a.role == ROLE_MAPPER ? "mapper" : "reducer", a.id);
    chaos_worker_init(a.role, a.id);

    int port = worker_port(cfg->base_port, cfg->M, a.role, a.id);
    a.ctl_fd = bind_retry(udp_bind, port);
    if (a.ctl_fd < 0) {
        wlog("cannot bind UDP port %d: %s", port, strerror(errno));
        abort();
    }
    if (a.role == ROLE_REDUCER) {
        a.listen_fd = bind_retry(tcp_listen, port);
        if (a.listen_fd < 0) {
            wlog("cannot listen on TCP port %d: %s", port, strerror(errno));
            abort();
        }
    } else {
        /* File k belongs to mapper k mod M. */
        a.files = calloc((size_t)cfg->nfiles + 1, sizeof *a.files);
        if (!a.files)
            abort();
        for (int k = a.id; k < cfg->nfiles; k += cfg->M)
            a.files[a.nfiles++] = cfg->files[k];
    }

    start_heartbeats(cfg, a.role, a.id);
    wlog("started%s", recovering ? " (recovering)" : "");

    if (a.role == ROLE_MAPPER)
        mapper_main(&a);
    else
        reducer_main(&a);
    exit(0);
}

static int spawn_worker(job_t *job, const config_t *cfg, int w, int recovering)
{
    pid_t parent = getpid();
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0)
        run_worker(cfg, job, w, recovering, parent);
    job->pid[w] = pid;
    job->exited[w] = 0;
    job->status[w] = 0;
    wlog("spawned %s %d as pid %d%s", w < cfg->M ? "mapper" : "reducer",
         w < cfg->M ? w : w - cfg->M, (int)pid, recovering ? " (recovering)" : "");
    return 0;
}

int spawn_all(job_t *job, const config_t *cfg)
{
    job->M = cfg->M;
    job->R = cfg->R;
    job->n = cfg->M + cfg->R;
    for (int w = 0; w < job->n; w++)
        if (spawn_worker(job, cfg, w, 0) != 0)
            return -1;
    return 0;
}

int respawn_worker(job_t *job, const config_t *cfg, int w)
{
    if (!job->exited[w] && job->pid[w] > 0) {
        kill(job->pid[w], SIGKILL);
        int st;
        while (waitpid(job->pid[w], &st, 0) < 0 && errno == EINTR)
            ;
        job->exited[w] = 1;
        job->status[w] = st;
    }
    return spawn_worker(job, cfg, w, 1);
}

int reap_children(job_t *job)
{
    int n = 0;
    for (;;) {
        int st;
        pid_t pid = waitpid(-1, &st, WNOHANG);
        if (pid <= 0)
            break;
        for (int w = 0; w < job->n; w++) {
            if (job->pid[w] == pid && !job->exited[w]) {
                job->exited[w] = 1;
                job->status[w] = st;
                n++;
            }
        }
    }
    return n;
}

void kill_all(job_t *job)
{
    for (int w = 0; w < job->n; w++)
        if (!job->exited[w] && job->pid[w] > 0)
            kill(job->pid[w], SIGKILL);
    for (int w = 0; w < job->n; w++) {
        if (!job->exited[w] && job->pid[w] > 0) {
            int st;
            while (waitpid(job->pid[w], &st, 0) < 0 && errno == EINTR)
                ;
            job->exited[w] = 1;
            job->status[w] = st;
        }
    }
}
