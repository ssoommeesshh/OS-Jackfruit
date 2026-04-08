/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Tasks implemented:
 *   Task 1 - Multi-container runtime with parent supervisor
 *   Task 2 - Supervisor CLI + IPC (UNIX domain socket control channel)
 *   Task 3 - Bounded-buffer logging pipeline (producer/consumer threads)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 512
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 32          /* bounded buffer slots        */
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MAX_CONTAINERS      16

/* ------------------------------------------------------------------ */
/* Enums                                                               */
/* ------------------------------------------------------------------ */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

/*
 * Per-container metadata kept by the supervisor.
 * Protected by supervisor_ctx_t.metadata_lock.
 */
typedef struct container_record {
    char                    id[CONTAINER_ID_LEN];
    pid_t                   host_pid;
    time_t                  started_at;
    container_state_t       state;
    unsigned long           soft_limit_bytes;
    unsigned long           hard_limit_bytes;
    int                     exit_code;
    int                     exit_signal;
    int                     stop_requested;   /* set before sending SIGTERM */
    char                    log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

/*
 * One slot in the bounded log buffer.
 */
typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

/*
 * Bounded buffer shared between producer threads (one per container pipe)
 * and the single consumer (logger) thread.
 *
 * Synchronisation:
 *   mutex  – protects head/tail/count and shutting_down
 *   not_empty – producers signal; consumer waits on this
 *   not_full  – consumer signals; producers wait on this
 */
typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

/*
 * Message sent from CLI client → supervisor over the UNIX socket.
 */
typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

/*
 * Response sent from supervisor → CLI client.
 */
typedef struct {
    int  status;                        /* 0 = ok, non-zero = error   */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

/*
 * Configuration passed to each container child via clone().
 * log_write_fd is the write end of the pipe that feeds the logger.
 */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

/*
 * Arguments for the producer thread (one per live container).
 */
typedef struct {
    int              read_fd;           /* read end of the container pipe */
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_args_t;

/*
 * Global supervisor state.
 */
typedef struct {
    int               server_fd;
    int               monitor_fd;
    volatile int      should_stop;

    /* logging pipeline */
    pthread_t         logger_thread;
    bounded_buffer_t  log_buffer;

    /* container metadata list */
    pthread_mutex_t   metadata_lock;
    container_record_t *containers;

    /* producer threads – one per running container */
    pthread_t         producers[MAX_CONTAINERS];
    int               producer_count;
} supervisor_ctx_t;

/* One stack per container.  For a demo, MAX_CONTAINERS is plenty. */
static char stacks[MAX_CONTAINERS][STACK_SIZE];

/* Used by signal handlers to request graceful shutdown */
static volatile sig_atomic_t g_stop = 0;

/* Global pointer so signal handlers can reach supervisor state */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value,
                          unsigned long *out)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s too large\n", flag);
        return -1;
    }
    *out = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                char *argv[], int start)
{
    for (int i = start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1],
                               &req->soft_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1],
                               &req->hard_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end;
            errno = 0;
            long v = strtol(argv[i+1], &end, 10);
            if (errno || end == argv[i+1] || *end || v < -20 || v > 19) {
                fprintf(stderr, "Invalid --nice value\n");
                return -1;
            }
            req->nice_value = (int)v;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bounded buffer (Task 3)                                             */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    int rc;
    if ((rc = pthread_mutex_init(&b->mutex, NULL)) != 0) return rc;
    if ((rc = pthread_cond_init(&b->not_empty, NULL)) != 0) {
        pthread_mutex_destroy(&b->mutex); return rc;
    }
    if ((rc = pthread_cond_init(&b->not_full, NULL)) != 0) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex); return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

/*
 * Signal shutdown: wake all blocked threads so they can drain and exit.
 */
static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/*
 * Producer: insert one log item.
 * Blocks if the buffer is full (back-pressure).
 * Returns 0 on success, -1 if shutdown requested.
 */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    /* Wait while full, unless we are shutting down */
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);   /* wake the consumer */
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * Consumer: remove one log item.
 * Returns  1 if an item was retrieved,
 *          0 if shutting down AND the buffer is now empty.
 */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    /* Wait while empty, unless shutdown */
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0) {
        /* Shutdown and empty – nothing left to drain */
        pthread_mutex_unlock(&b->mutex);
        return 0;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);    /* wake blocked producers */
    pthread_mutex_unlock(&b->mutex);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Logging threads (Task 3)                                            */
/* ------------------------------------------------------------------ */

/*
 * Consumer thread: reads from the bounded buffer, opens the appropriate
 * per-container log file, and appends the data.
 *
 * Uses the container_id embedded in each log_item_t to determine the
 * file path (logs/<id>.log).  Files are opened/closed per-chunk so
 * that a crash still leaves partial logs on disk.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    while (1) {
        int got = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (!got)
            break;   /* shutdown + drained */

        /* Build log file path */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open log file");
            continue;
        }
        write(fd, item.data, item.length);
        close(fd);
    }

    printf("[supervisor] Logger thread exiting, all log data flushed.\n");
    return NULL;
}

/*
 * Producer thread: reads from the read end of one container's pipe,
 * packs data into log_item_t chunks, and pushes them into the bounded
 * buffer.  One producer thread per live container.
 */
static void *producer_thread(void *arg)
{
    producer_args_t *pa = (producer_args_t *)arg;
    log_item_t item;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pa->container_id,
                sizeof(item.container_id) - 1);

        ssize_t n = read(pa->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0)
            break;   /* pipe closed (container exited) or error */

        item.length = (size_t)n;
        if (bounded_buffer_push(pa->buffer, &item) != 0)
            break;   /* shutdown in progress */
    }

    close(pa->read_fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child entry point (Task 1)                               */
/* ------------------------------------------------------------------ */

/*
 * Runs inside the cloned child process.
 * Sets up namespaces, chroot, /proc, then execs the requested command.
 */
static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr into the logging pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* UTS namespace: give this container its own hostname */
    sethostname(cfg->id, strlen(cfg->id));

    /* Mount namespace: chroot into the container's rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("child_fn: chdir");
        return 1;
    }

    /* Mount /proc so tools like ps work inside the container */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Non-fatal: /proc may already be present */
        perror("child_fn: mount /proc (non-fatal)");
    }

    /* Apply nice value if requested */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the requested command.
     * For a shell, pass -i so it doesn't exit immediately. */
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);

    /* If we reach here, exec failed */
    perror("child_fn: exec");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Kernel monitor helpers (Task 4 – stubs used by start path)         */
/* ------------------------------------------------------------------ */

static int register_with_monitor(int monitor_fd, const char *id,
                                 pid_t pid, unsigned long soft,
                                 unsigned long hard)
{
    if (monitor_fd < 0) return 0;   /* module not loaded – skip */
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0 ? -1 : 0;
}

static int unregister_from_monitor(int monitor_fd, const char *id,
                                   pid_t pid)
{
    if (monitor_fd < 0) return 0;
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Container metadata helpers                                          */
/* ------------------------------------------------------------------ */

/* Caller must hold metadata_lock */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    for (container_record_t *c = ctx->containers; c; c = c->next)
        if (strcmp(c->id, id) == 0) return c;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Signal handlers                                                     */
/* ------------------------------------------------------------------ */

static void handle_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/*
 * SIGCHLD handler: reap all exited children without blocking.
 * Updates container metadata for any PID we recognise.
 */
static void handle_sigchld(int sig)
{
    (void)sig;
    if (!g_ctx) return;

    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        for (container_record_t *c = g_ctx->containers; c; c = c->next) {
            if (c->host_pid != pid) continue;

            if (WIFEXITED(status)) {
                c->exit_code = WEXITSTATUS(status);
                c->state = CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                /* Distinguish hard-limit kill from manual stop */
                if (c->stop_requested)
                    c->state = CONTAINER_STOPPED;
                else
                    c->state = CONTAINER_KILLED;
            }

            unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
            break;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Supervisor: handle one control request (Task 2)                    */
/* ------------------------------------------------------------------ */

static void handle_start(supervisor_ctx_t *ctx,
                         const control_request_t *req,
                         control_response_t *resp)
{
    /* Check for duplicate container ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "Container '%s' already exists", req->container_id);
        return;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Check we have a free stack slot */
    if (ctx->producer_count >= MAX_CONTAINERS) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "Max containers (%d) reached", MAX_CONTAINERS);
        return;
    }

    /* Create pipe: container writes → supervisor reads → log buffer */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "pipe() failed: %s", strerror(errno));
        return;
    }

    /* Build child config (allocated on heap; child reads before exec) */
    child_config_t *cfg = malloc(sizeof(child_config_t));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "malloc failed");
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id,      req->container_id, sizeof(cfg->id)      - 1);
    strncpy(cfg->rootfs,  req->rootfs,       sizeof(cfg->rootfs)  - 1);
    strncpy(cfg->command, req->command,      sizeof(cfg->command) - 1);
    cfg->nice_value    = req->nice_value;
    cfg->log_write_fd  = pipefd[1];   /* child writes here */

    /* Clone new container child */
    int slot = ctx->producer_count;
    char *stack_top = stacks[slot] + STACK_SIZE;

    pid_t pid = clone(child_fn, stack_top,
                      CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                      cfg);

    /* Close the write end in the supervisor – only child holds it now */
    close(pipefd[1]);

    if (pid < 0) {
        close(pipefd[0]);
        free(cfg);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "clone() failed: %s", strerror(errno));
        return;
    }

    /* Register with kernel memory monitor */
    register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                          req->soft_limit_bytes, req->hard_limit_bytes);

    /* Add metadata record */
    container_record_t *rec = calloc(1, sizeof(container_record_t));
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Spawn a producer thread to forward pipe data into the log buffer */
    producer_args_t *pa = malloc(sizeof(producer_args_t));
    pa->read_fd = pipefd[0];
    pa->buffer  = &ctx->log_buffer;
    strncpy(pa->container_id, req->container_id,
            sizeof(pa->container_id) - 1);

    pthread_create(&ctx->producers[slot], NULL, producer_thread, pa);
    ctx->producer_count++;

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "Container '%s' started (pid %d)", req->container_id, pid);
    free(cfg);   /* child has already exec'd or died; safe to free */
}

static void handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    char buf[CONTROL_MESSAGE_LEN];
    buf[0] = '\0';

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    if (!c) {
        strncat(buf, "(no containers)\n", sizeof(buf) - strlen(buf) - 1);
    } else {
        char line[128];
        snprintf(line, sizeof(line),
                 "%-12s %-6s %-10s %-20s\n",
                 "ID", "PID", "STATE", "STARTED");
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        for (; c; c = c->next) {
            char ts[20];
            struct tm *tm = localtime(&c->started_at);
            strftime(ts, sizeof(ts), "%H:%M:%S", tm);
            snprintf(line, sizeof(line),
                     "%-12s %-6d %-10s %-20s\n",
                     c->id, c->host_pid,
                     state_to_string(c->state), ts);
            strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    strncpy(resp->message, buf, sizeof(resp->message) - 1);
}

static void handle_logs(supervisor_ctx_t *ctx,
                        const control_request_t *req,
                        control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    char log_path[PATH_MAX] = {0};
    if (c) strncpy(log_path, c->log_path, sizeof(log_path) - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!c) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "No such container: %s", req->container_id);
        return;
    }

    /* Read and return up to CONTROL_MESSAGE_LEN bytes from the log */
    int fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "Log file not found: %s", log_path);
        return;
    }
    ssize_t n = read(fd, resp->message, sizeof(resp->message) - 1);
    close(fd);
    if (n < 0) n = 0;
    resp->message[n] = '\0';
    resp->status = 0;
}

static void handle_stop(supervisor_ctx_t *ctx,
                        const control_request_t *req,
                        control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = find_container(ctx, req->container_id);
    if (!c || c->state != CONTAINER_RUNNING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 c ? "Container not running" : "No such container: %s",
                 req->container_id);
        return;
    }
    /* Mark stop_requested BEFORE sending any signal */
    c->stop_requested = 1;
    pid_t pid = c->host_pid;
    c->state = CONTAINER_STOPPED;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Graceful: SIGTERM, then SIGKILL after a short wait */
    kill(pid, SIGTERM);
    usleep(500000);
    /* If still alive after 0.5 s, force-kill */
    if (waitpid(pid, NULL, WNOHANG) == 0)
        kill(pid, SIGKILL);

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "Container '%s' stopped", req->container_id);
}

/* ------------------------------------------------------------------ */
/* Supervisor event loop (Tasks 1 + 2)                                */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Initialise metadata mutex */
    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init"); return 1;
    }

    /* Initialise bounded buffer */
    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock); return 1;
    }

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* Try to open the kernel monitor device (optional) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] /dev/container_monitor not found – "
                "memory monitoring disabled\n");

    /* Start the consumer (logger) thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger"); return 1;
    }

    /* ---------------------------------------------------------------
     * Create UNIX domain socket for CLI → supervisor control plane
     * (Task 2 – IPC Path B)
     * --------------------------------------------------------------- */
    unlink(CONTROL_PATH);   /* remove stale socket if present */

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); return 1;
    }

    /* Install signal handlers */
    struct sigaction sa_int = {0};
    sa_int.sa_handler = handle_sigint;
    sigaction(SIGINT,  &sa_int, NULL);
    sigaction(SIGTERM, &sa_int, NULL);

    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = handle_sigchld;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    printf("[supervisor] Started.  base-rootfs=%s  ctrl=%s\n",
           rootfs, CONTROL_PATH);
    printf("[supervisor] Waiting for CLI commands...\n");

    /* ---------------------------------------------------------------
     * Main event loop: accept CLI connections, dispatch commands
     * --------------------------------------------------------------- */
    while (!g_stop) {
        /*
         * Use select() with a timeout so SIGINT/SIGTERM are not
         * permanently stuck in accept().
         */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = {1, 0};   /* 1 s timeout */

        int rc = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;   /* signal – check g_stop */
            perror("select"); break;
        }
        if (rc == 0) continue;   /* timeout – loop and check g_stop */

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }

        /* Read one control_request_t */
        control_request_t req;
        if (recv(client_fd, &req, sizeof(req), MSG_WAITALL) !=
            (ssize_t)sizeof(req)) {
            close(client_fd); continue;
        }

        control_response_t resp;
        memset(&resp, 0, sizeof(resp));

        switch (req.kind) {
        case CMD_START:
        case CMD_RUN:
            handle_start(&ctx, &req, &resp);
            break;
        case CMD_PS:
            handle_ps(&ctx, &resp);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, &req, &resp);
            break;
        case CMD_STOP:
            handle_stop(&ctx, &req, &resp);
            break;
        default:
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Unknown command kind %d", req.kind);
        }

        send(client_fd, &resp, sizeof(resp), 0);
        close(client_fd);

        /* For CMD_RUN: block until that container exits */
        if (req.kind == CMD_RUN && resp.status == 0) {
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = find_container(&ctx, req.container_id);
            pid_t pid = c ? c->host_pid : -1;
            pthread_mutex_unlock(&ctx.metadata_lock);

            if (pid > 0) {
                int wstatus;
                waitpid(pid, &wstatus, 0);
                /* Update metadata */
                pthread_mutex_lock(&ctx.metadata_lock);
                c = find_container(&ctx, req.container_id);
                if (c) {
                    if (WIFEXITED(wstatus)) {
                        c->exit_code = WEXITSTATUS(wstatus);
                        c->state = CONTAINER_EXITED;
                    } else if (WIFSIGNALED(wstatus)) {
                        c->exit_signal = WTERMSIG(wstatus);
                        c->state = c->stop_requested
                                   ? CONTAINER_STOPPED : CONTAINER_KILLED;
                    }
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            }
        }
    }

    /* ---------------------------------------------------------------
     * Orderly shutdown
     * --------------------------------------------------------------- */
    printf("\n[supervisor] Shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    for (container_record_t *c = ctx.containers; c; c = c->next) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for all container children */
    while (waitpid(-1, NULL, 0) > 0)
        ;

    /* Shut down the logging pipeline */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);

    /* Join producer threads */
    for (int i = 0; i < ctx.producer_count; i++)
        pthread_join(ctx.producers[i], NULL);

    /* Join consumer thread */
    pthread_join(ctx.logger_thread, NULL);

    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free metadata list */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    /* Close sockets and device */
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);

    printf("[supervisor] Clean shutdown complete.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client-side: send a request to the supervisor (Task 2)             */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s\n"
                        "Is it running?  Try: sudo ./engine supervisor <rootfs>\n",
                CONTROL_PATH);
        close(fd); return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    control_response_t resp;
    if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) !=
        (ssize_t)sizeof(resp)) {
        perror("recv"); close(fd); return 1;
    }
    close(fd);

    printf("%s\n", resp.message);
    return resp.status == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* CLI command entry points                                            */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s start <id> <rootfs> <command> "
            "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,        argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,       argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s run <id> <rootfs> <command> "
            "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,        argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,       argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)  return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0)  return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0)  return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0)  return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0)  return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
