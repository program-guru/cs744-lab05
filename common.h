/*
 * common.h -- shared definitions for the word-count lab.
 */
#ifndef COMMON_H
#define COMMON_H

#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define MAX_MAPPERS   16
#define MAX_REDUCERS  16
#define MAX_WORKERS   (MAX_MAPPERS + MAX_REDUCERS)
#define MAX_WORD      48

#define HEARTBEAT_INTERVAL_MS  500
#define HEARTBEAT_TIMEOUT_MS   1000
#define DEFAULT_BASE_PORT      12900

#define CKPT_DIR      "ckpt"
#define META_PATH     CKPT_DIR "/META"

#define LAST_CHECKPOINT_ID  0
#define NO_CHECKPOINT       (-1)

enum { ROLE_MAPPER = 0, ROLE_REDUCER = 1, ROLE_COORDINATOR = 2 };

/* Message types. */
typedef enum {
    MT_HEARTBEAT           = 0,     /* worker -> coordinator (sent by provided code) */
    MT_CHECKPOINT          = 1,     /* coordinator -> mapper */
    MT_FWD_CHECKPOINT      = 2,     /* mapper -> reducer: the checkpoint marker */
    MT_WORD_COUNT          = 3,     /* mapper -> reducer */
    MT_CHECKPOINT_ACK      = 4,     /* worker -> coordinator */
    MT_EXIT                = 5,     /* coordinator -> worker */
    MT_DONE                = 6,     /* mapper -> coordinator: all files processed */
    MT_LAST_CHECKPOINT_ACK = 7,     /* worker -> coordinator: ack for checkpoint 0 */
    MT_RECOVER             = 8,     /* coordinator -> worker */
    MT_RECOVERY_ACK        = 9,     /* worker -> coordinator */
} msg_type_t;

/* One fixed-size message format for every channel. */
typedef struct {
    int32_t type;               /* msg_type_t */
    int32_t role;               /* sender: ROLE_MAPPER, ROLE_REDUCER or ROLE_COORDINATOR */
    int32_t id;                 /* sender's mapper or reducer id */
    int32_t recovery_id;        /* sender's recovery id (not used in heartbeats) */
    int64_t checkpoint_id;      /* CHECKPOINT, FWD_CHECKPOINT, *_ACK, RECOVER */
    int64_t count;              /* WORD_COUNT; HEARTBEAT: sender's pid */
    char    word[MAX_WORD];     /* WORD_COUNT: NUL-terminated word */
} msg_t;

_Static_assert(sizeof(msg_t) <= PIPE_BUF, "msg_t must stay small");

enum { CHAOS_ANY, CHAOS_MAPPER, CHAOS_REDUCER, CHAOS_BOTH, CHAOS_ALL };

typedef struct {
    int       M, R;             /* number of mappers and reducers */
    const char *input_dir;
    int       interval_ms;      /* -t: checkpoint interval; 0 = only the last checkpoint */
    int       sleep_us;         /* -s: simulated processing time per file */
    int       base_port;        /* -p: coordinator port; workers use the next M + R */
    int       kills;            /* -k: number of chaos kill events */
    int       chaos_mode;       /* --chaos */
    unsigned  seed;             /* --seed */
    int       kill_blocked;     /* --kill-blocked */
    int       no_align;         /* --no-align */
    char    **files;            /* all input files, sorted by name */
    int       nfiles;
} config_t;

typedef struct {
    int    role;                /* ROLE_MAPPER or ROLE_REDUCER */
    int    id;                  /* 0..M-1 or 0..R-1 */
    int    M, R;
    int    base_port;
    int    recovering;          /* 1: restarted after a failure; wait for MT_RECOVER */
    int    sleep_us;            /* mapper: simulated processing time per file */
    int    no_align;            /* reducer: --no-align given */

    /* Mapper only */
    char **files;
    int    nfiles;

    /* Control channel */
    int    ctl_fd;

    /* Reducer only */
    int    listen_fd;
} worker_args_t;

typedef struct {
    int    M, R, n;             /* n = M + R */
    pid_t  pid[MAX_WORKERS];    /* current process of each worker */
    int    exited[MAX_WORKERS]; /* set by reap_children() once that process is reaped */
    int    status[MAX_WORKERS]; /* its wait status */
    int    fd;                  /* coordinator's UDP socket, O_NONBLOCK */
} job_t;

/* Network utilities */
int  coordinator_port(int base_port);
int  worker_port(int base_port, int M, int role, int id);
int  worker_index(int M, int role, int id);     /* mappers first, then reducers */
int  udp_bind(int port);
int  udp_send(int fd, int port, const msg_t *m);
int  udp_recv(int fd, msg_t *m);
int  tcp_listen(int port);
int  tcp_accept(int listen_fd);
int  tcp_connect(int port);
int  tcp_connect_retry(int port);
int  send_msg(int fd, const msg_t *m);
int  recv_msg(int fd, msg_t *m);
int  unix_dgram_bind(const char *path);
int  unix_dgram_send(int fd, const char *path, const msg_t *m);

/* Other utilities */
void sleep_us(long us);
long now_ms(void);
uint32_t fnv1a(const char *s);

/* Logging */
void log_init(const char *role, int id);
const char *msg_type_name(int type);   /* "CHECKPOINT", "WORD_COUNT", ... */
void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Word and file utilities */
typedef struct wcmap wcmap_t;

wcmap_t *wcmap_new(void);
void     wcmap_free(wcmap_t *m);
void     wcmap_add(wcmap_t *m, const char *word, int64_t n);
int64_t  wcmap_get(const wcmap_t *m, const char *word);        /* 0 if absent */
size_t   wcmap_size(const wcmap_t *m);
int      wcmap_next(const wcmap_t *m, size_t *pos, const char **word, int64_t *count);
int      wcmap_save(const wcmap_t *m, const char *path);
wcmap_t *wcmap_load(const char *path);
wcmap_t *count_file(const char *path);
int save_int(const char *path, long value);     /* atomic */
int load_int(const char *path, long *value);    /* 0, or -1 if missing/malformed */
int list_input_files(const char *dir, char ***files);
int ensure_dir(const char *path);

/* worker utilities */

/* Start every worker for the first time (recovering = 0). Each worker binds
 * its sockets, sends its first heartbeat, keeps sending one every
 * HEARTBEAT_INTERVAL_MS, and then runs mapper_main() or reducer_main().
 * When that returns, the worker exits with status 0. Returns 0 or -1. */
int  spawn_all(job_t *job, const config_t *cfg);

/* Replace worker w after a failure: SIGKILL its current process if it has not
 * been reaped, reap it, and start a new one with recovering = 1. The new
 * process starts sending heartbeats once its sockets are bound. Returns 0 or -1. */
int  respawn_worker(job_t *job, const config_t *cfg, int w);

/* Reap any worker processes that have exited (non-blocking) and record it in
 * job->exited / job->status. Returns the number reaped. This is only
 * bookkeeping: detect failures with heartbeats, not with this. */
int  reap_children(job_t *job);

/* SIGKILL and reap every worker process still running. */
void kill_all(job_t *job);


/* Chaos failure injection utilities */
void chaos_init(const config_t *cfg);       /* coordinator, before spawning */
void chaos_worker_init(int role, int id);   /* called in each worker */
void chaos_tick(job_t *job);                /* call once per coordinator loop iteration */
void chaos_report_blocked(int n);           /* reducer: number of blocked channels changed */
int  chaos_kills(void);                     /* number of processes killed so far */

/* Worker main functions */
void mapper_main(const worker_args_t *a);                  /* mapper.c */
void reducer_main(const worker_args_t *a);                 /* reducer.c */
int  coordinator_loop(const config_t *cfg, job_t *job);    /* coordinator.c */

#endif /* COMMON_H */
