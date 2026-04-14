# Implementation Documentation
## Changes from Original Boilerplate — `engine.c` and `monitor.c`

This document explains every change made from the original boilerplate, organised by task. For each task it covers what was added, what each important function does line by line, and what helper functions support it.

---

## Quick Reference — What Was a Stub vs What We Wrote

| Function | Boilerplate state | What we did |
|---|---|---|
| `bounded_buffer_push` | returned `-1` (stub) | Fully implemented |
| `bounded_buffer_pop` | returned `-1` (stub) | Fully implemented |
| `logging_thread` | returned `NULL` (stub) | Fully implemented |
| `child_fn` | partial — no pipe redirect, no exec | Completed |
| `run_supervisor` | launched one hardcoded container, broken loop | Rewritten completely |
| `send_control_request` | printed error and returned 1 (stub) | Fully implemented |
| `handle_start/ps/logs/stop` | did not exist | Written from scratch |
| `handle_sigchld` | did not exist | Written from scratch |
| `producer_thread` | did not exist | Written from scratch |
| `register_with_monitor` | existed, complete | Kept, added ENOENT handling to unregister |
| `monitor.c` — all TODOs | empty | All 6 TODOs filled |

---

## New Data Structures Added (not in boilerplate)

The boilerplate defined the structs but left important fields missing. Here is what was added and why.

### `termination_reason_t` enum — NEW, did not exist

```c
typedef enum {
    REASON_NONE = 0,          // container is still running
    REASON_NORMAL_EXIT,       // exited with a status code, no signal
    REASON_STOPPED,           // supervisor called stop (stop_requested was set first)
    REASON_HARD_LIMIT_KILLED, // received SIGKILL without stop_requested → kernel monitor
    REASON_UNKNOWN_SIGNAL     // died from some other signal
} termination_reason_t;
```

This is required by Task 4's attribution rule: `ps` must distinguish a manual stop from a hard-limit kill. The key insight is that both result in the process dying from SIGKILL — the only way to tell them apart is the `stop_requested` flag that the supervisor sets *before* sending any signal.

### Fields added to `container_record_t`

```c
int                  nice_value;          // scheduler priority, shown in ps
int                  stop_requested;      // set BEFORE sending SIGTERM/SIGKILL
termination_reason_t termination_reason;  // the Task 4 attribution field
```

`stop_requested` is the critical one. It must be written to the metadata record before any `kill()` call, because the SIGCHLD handler runs asynchronously and reads this flag to decide the termination reason.

### `producer_args_t` struct — NEW

```c
typedef struct {
    int              read_fd;        // read end of the container's stdout pipe
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;        // shared log buffer to push into
} producer_args_t;
```

One of these is allocated per container. It is passed to `producer_thread` and freed inside that thread when it exits.

---

## Task 1 — Multi-Container Runtime with Parent Supervisor

**Goal:** The supervisor stays alive, multiple containers can be started, each has isolated namespaces and its own rootfs, `/proc` works inside, no zombies.

### `child_fn(void *arg)` — the container entry point

This function runs inside the new process created by `clone()`. It receives a `child_config_t` pointer as its argument.

```c
static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
```
Cast the void pointer back to the config struct. This is how `clone()` passes data to the child.

```c
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }
```
`dup2` replaces file descriptors 1 (stdout) and 2 (stderr) with the write end of the logging pipe. Now anything the container prints goes into the pipe, not the terminal. We close the original fd after duplicating because we no longer need it as a separate fd — the two `dup2` calls already point fds 1 and 2 at it.

```c
    sethostname(cfg->id, strlen(cfg->id));
```
Sets the hostname inside the container's UTS namespace. Because we cloned with `CLONE_NEWUTS`, this only affects this container — the host's hostname is unchanged.

```c
    if (chroot(cfg->rootfs) != 0) { ... }
    if (chdir("/") != 0) { ... }
```
`chroot` changes the root directory for this process and all its children. The process can no longer see anything above `cfg->rootfs`. `chdir("/")` is required after `chroot` — without it the current working directory is still pointing outside the new root.

```c
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) { ... }
```
Mounts a fresh `procfs` at `/proc` inside the container. This is what makes `ps`, `top`, and `/proc/self/...` work correctly inside the container. Because we cloned with `CLONE_NEWPID`, the container has its own PID namespace — its `init` process sees itself as PID 1 in `/proc`.

```c
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);
```
Adjusts the scheduler priority. Higher nice = lower priority. This is what `--nice 15` does when starting a container. It runs inside the child so only this container's process is affected.

```c
    execl("/bin/sh", "/bin/sh", "-c", cfg->command, (char *)NULL);
```
Replaces the current process image with the shell running the requested command. If this succeeds, `child_fn` never returns — the process is now the command. If it fails (command not found, bad rootfs), we fall through to the error print and return 1.

**Clone flags used:**
- `CLONE_NEWPID` — new PID namespace (container sees itself as PID 1)
- `CLONE_NEWNS` — new mount namespace (container's `/proc` mount doesn't affect host)
- `CLONE_NEWUTS` — new UTS namespace (container has its own hostname)
- `SIGCHLD` — tell the kernel to send SIGCHLD to the parent when this child exits

### `handle_sigchld(int sig)` — child reaper

This signal handler fires whenever any child process exits. It runs asynchronously.

```c
while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
```
Loop reaping all children that have already exited. `-1` means any child. `WNOHANG` means don't block — if no child has exited yet, return immediately. This loop is needed because multiple children can exit between two deliveries of SIGCHLD.

```c
    pthread_mutex_lock(&g_ctx->metadata_lock);
    for (container_record_t *c = g_ctx->containers; c; c = c->next) {
        if (c->host_pid != pid) continue;
```
Walk the linked list to find the record matching this PID. The lock is needed because the main event loop also reads and writes the list.

```c
        if (WIFEXITED(status)) {
            c->exit_code          = WEXITSTATUS(status);
            c->state              = CONTAINER_EXITED;
            c->termination_reason = REASON_NORMAL_EXIT;
        }
```
`WIFEXITED` is true when the process called `exit()` or returned from `main`. `WEXITSTATUS` extracts the exit code (0–255).

```c
        } else if (WIFSIGNALED(status)) {
            c->exit_signal = WTERMSIG(status);
            if (c->stop_requested) {
                c->state              = CONTAINER_STOPPED;
                c->termination_reason = REASON_STOPPED;
            } else if (c->exit_signal == SIGKILL) {
                c->state              = CONTAINER_KILLED;
                c->termination_reason = REASON_HARD_LIMIT_KILLED;
            } else {
                c->state              = CONTAINER_KILLED;
                c->termination_reason = REASON_UNKNOWN_SIGNAL;
            }
        }
```
`WIFSIGNALED` is true when the process was killed by a signal. The three-way branch is the Task 4 attribution rule:
- If `stop_requested` is set → the supervisor sent the signal intentionally → `REASON_STOPPED`
- If SIGKILL and not stop_requested → the only thing that sends unsolicited SIGKILL is the kernel module → `REASON_HARD_LIMIT_KILLED`
- Anything else → some external signal → `REASON_UNKNOWN_SIGNAL`

```c
        unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
```
Tell the kernel module to remove this PID from its watch list. If the kernel timer already cleaned it up (which is common), this returns `-ENOENT` which we silently ignore.

---

## Task 2 — Supervisor CLI and IPC

**Goal:** CLI commands (`start`, `ps`, `logs`, `stop`) sent from a short-lived client process reach the long-running supervisor and get a response back. IPC mechanism chosen: **UNIX domain socket**.

### Why a UNIX domain socket?

A UNIX domain socket is a bidirectional IPC channel between processes on the same machine. It's created as a file on disk (`/tmp/mini_runtime.sock`). The supervisor binds and listens on it; each CLI invocation connects, sends one struct, reads one struct, and exits. No network stack is involved. It was chosen over a FIFO because FIFOs are unidirectional (you need two for request + response), and over shared memory because shared memory needs separate signalling to wake the supervisor.

### Supervisor side — inside `run_supervisor()`

```c
unlink(CONTROL_PATH);
ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
```
Remove any stale socket file from a previous crashed run, then create a stream socket. `SOCK_STREAM` gives us ordered, reliable, connection-oriented delivery — important so struct bytes don't get fragmented.

```c
addr.sun_family = AF_UNIX;
strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
listen(ctx.server_fd, 8);
```
`bind` attaches the socket to the filesystem path. `listen(8)` allows up to 8 pending connections queued before the supervisor accepts them.

```c
while (!g_stop) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(ctx.server_fd, &rfds);
    struct timeval tv = {1, 0};
    int rc = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
    if (rc < 0) {
        if (errno == EINTR) continue;
        break;
    }
    if (rc == 0) continue;
```
`select` with a 1-second timeout replaces a bare `accept()`. If we used `accept()` directly, a `SIGINT` signal would interrupt it but then the loop would call `accept()` again before checking `g_stop`. With `select`, every second the loop falls through, checks `g_stop`, and exits cleanly if set.

```c
    int client_fd = accept(ctx.server_fd, NULL, NULL);
    control_request_t req;
    recv(client_fd, &req, sizeof(req), MSG_WAITALL);
```
`accept` blocks until a CLI client connects. `MSG_WAITALL` tells `recv` to not return until all `sizeof(req)` bytes have arrived — without this, the kernel might deliver a partial struct.

```c
    switch (req.kind) {
    case CMD_START: case CMD_RUN: handle_start(&ctx, &req, &resp); break;
    case CMD_PS:                  handle_ps(&ctx, &resp);          break;
    case CMD_LOGS:                handle_logs(&ctx, &req, &resp);  break;
    case CMD_STOP:                handle_stop(&ctx, &req, &resp);  break;
    }
    send(client_fd, &resp, sizeof(resp), 0);
    close(client_fd);
```
Dispatch to the appropriate handler, send the response struct back, close the connection. Each CLI invocation is one connection → one request → one response.

### Client side — `send_control_request()`

```c
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
connect(fd, (struct sockaddr *)&addr, sizeof(addr));
send(fd, req, sizeof(*req), 0);
recv(fd, &resp, sizeof(resp), MSG_WAITALL);
printf("%s\n", resp.message);
return resp.status == 0 ? 0 : 1;
```
Mirror image of the server. Creates a socket, connects to the same path, sends the request struct, waits for the full response struct, prints the message, and exits. The process exits with 0 on success or 1 on error — this lets shell scripts check `$?`.

### `handle_start()` — what happens when you type `engine start`

```c
pthread_mutex_lock(&ctx->metadata_lock);
if (find_container(ctx, req->container_id)) { ... return "already exists" ... }
pthread_mutex_unlock(&ctx->metadata_lock);
```
Check for duplicate IDs first, under the lock. We release the lock before doing anything expensive.

```c
int pipefd[2];
pipe(pipefd);
```
Create the logging pipe. `pipefd[0]` is the read end (supervisor), `pipefd[1]` is the write end (container). This is IPC Path A — distinct from the UNIX socket control channel.

```c
cfg->log_write_fd = pipefd[1];
pid_t pid = clone(child_fn, stack_top,
                  CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, cfg);
close(pipefd[1]);
```
Pass the write end to the child via `cfg`. After `clone()`, close the write end in the supervisor — the supervisor only reads from the pipe. If we don't close it, the pipe never reaches EOF even after the container exits, and the producer thread would block forever.

```c
register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                      req->soft_limit_bytes, req->hard_limit_bytes);
```
Tell the kernel module to start watching this PID. The host PID (not the container-namespace PID) is used because the kernel module operates in the host PID namespace.

```c
container_record_t *rec = calloc(1, sizeof(container_record_t));
// ... fill all fields ...
pthread_mutex_lock(&ctx->metadata_lock);
rec->next = ctx->containers;
ctx->containers = rec;
pthread_mutex_unlock(&ctx->metadata_lock);
```
Allocate and prepend the metadata record to the linked list. Prepend (not append) is O(1). The lock ensures the SIGCHLD handler can't race us while we're inserting.

```c
producer_args_t *pa = malloc(sizeof(producer_args_t));
pa->read_fd = pipefd[0];
pa->buffer  = &ctx->log_buffer;
strncpy(pa->container_id, ...);
pthread_create(&ctx->producers[slot], NULL, producer_thread, pa);
ctx->producer_count++;
```
Spawn a producer thread for this container. The thread owns `pipefd[0]` and will free `pa` when it exits.

### `handle_stop()` — what happens when you type `engine stop`

```c
c->stop_requested     = 1;
c->termination_reason = REASON_STOPPED;
c->state              = CONTAINER_STOPPED;
pid_t pid             = c->host_pid;
pthread_mutex_unlock(&ctx->metadata_lock);
```
Set `stop_requested` **before** releasing the lock and **before** calling `kill()`. This ordering is critical — if SIGCHLD fires between the `kill()` and the flag being set, the handler would wrongly classify the kill as `REASON_HARD_LIMIT_KILLED`.

```c
kill(pid, SIGTERM);
usleep(500000);
pid_t reaped = waitpid(pid, &wstatus, WNOHANG);
if (reaped == 0) {
    kill(pid, SIGKILL);
    reaped = waitpid(pid, &wstatus, 0);
}
```
Graceful shutdown: SIGTERM first (allows the process to clean up), wait 500ms, then SIGKILL if still alive. `WNOHANG` on the first `waitpid` means "check but don't block." The second `waitpid` (after SIGKILL) blocks until the process is definitely dead.

```c
if (reaped > 0 && WIFSIGNALED(wstatus)) {
    pthread_mutex_lock(&ctx->metadata_lock);
    c = find_container(ctx, req->container_id);
    if (c) c->exit_signal = WTERMSIG(wstatus);
    pthread_mutex_unlock(&ctx->metadata_lock);
}
```
Write `exit_signal` so that `ps` can show `stopped(sig=15)` or `stopped(sig=9)`. This is separate from the SIGCHLD path — `handle_stop` reaps the child directly here, so SIGCHLD may or may not also fire. We only write `exit_signal` (not state or reason) because those are already correct.

### `handle_ps()` — the ps table

Builds a fixed-width ASCII table in a local buffer using a `PS_APPEND` macro that tracks position and remaining space to avoid overflows. Each container row shows:

- **ID** — container name
- **PID** — host PID (what the kernel sees)
- **STATE** — running / stopped / killed / exited
- **NICE** — scheduler priority applied at start
- **SOFT(MB) / HARD(MB)** — memory limits in MiB
- **REASON / EXIT** — `exit=0`, `stopped(sig=15)`, `hard_limit_killed`, or `-`

The `exit_info` string is built by checking `termination_reason` first, then falling back to the raw signal number. This is the human-readable output of the Task 4 attribution rule.

### Signal handlers

```c
static void handle_sigint(int sig)
{
    (void)sig;
    g_stop = 1;   // volatile sig_atomic_t — safe to write from signal handler
}
```
Sets the global flag that the `select` loop checks every second. `volatile sig_atomic_t` is the only type guaranteed safe to read/write from a signal handler.

Signal handlers are installed with `sigaction` rather than `signal()`:
```c
struct sigaction sa_chld = {0};
sa_chld.sa_handler = handle_sigchld;
sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
sigaction(SIGCHLD, &sa_chld, NULL);
```
- `SA_RESTART` — system calls interrupted by SIGCHLD automatically restart rather than returning `EINTR`. This prevents `select` and `recv` from failing spuriously.
- `SA_NOCLDSTOP` — don't deliver SIGCHLD when a child is merely stopped (e.g. by SIGSTOP), only when it exits. Reduces noise.

---

## Task 3 — Bounded-Buffer Logging and IPC Design

**Goal:** Container stdout/stderr is captured through a producer/consumer pipeline into per-container log files. No log lines dropped, no deadlock, clean shutdown.

### The pipeline

```
Container stdout/stderr
        │
        │  (pipe — one per container)
        ▼
  producer_thread         ← one thread per container
        │
        │  bounded_buffer_push()
        ▼
  bounded_buffer_t        ← 32 slots, protected by mutex + 2 cond vars
        │
        │  bounded_buffer_pop()
        ▼
  logging_thread          ← one consumer thread total
        │
        ▼
  logs/<id>.log           ← one file per container
```

### `bounded_buffer_t` struct

```c
typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];  // 32 fixed slots (circular array)
    size_t          head;         // consumer reads from here
    size_t          tail;         // producer writes here
    size_t          count;        // how many slots are currently filled
    int             shutting_down;
    pthread_mutex_t mutex;        // protects all fields above
    pthread_cond_t  not_empty;    // consumer waits here when count == 0
    pthread_cond_t  not_full;     // producer waits here when count == 32
} bounded_buffer_t;
```

**Why mutex + condition variables instead of semaphores?**

A semaphore could signal "slot available" but can't atomically check `shutting_down` at the same time. With a mutex + condvar, the producer locks, checks both `count` and `shutting_down` in one critical section, and blocks atomically if needed. This eliminates the race where shutdown begins between the check and the wait.

### `bounded_buffer_push()` — producer inserts one chunk

```c
pthread_mutex_lock(&b->mutex);
```
Lock before reading or writing any field. Without this, two producers could read the same `tail` value and write to the same slot.

```c
while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
    pthread_cond_wait(&b->not_full, &b->mutex);
```
`while` not `if` — this is the standard condvar pattern. `pthread_cond_wait` can return spuriously (the OS wakes the thread for no reason), so we re-check the condition. The loop also rechecks `shutting_down` so that a producer woken by `bounded_buffer_begin_shutdown` doesn't go back to sleep.

`pthread_cond_wait` atomically releases the mutex and puts the thread to sleep. When signalled, it reacquires the mutex before returning.

```c
if (b->shutting_down) {
    pthread_mutex_unlock(&b->mutex);
    return -1;
}
```
If we woke because of shutdown, don't insert — return -1 so the producer thread exits.

```c
b->items[b->tail] = *item;
b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
b->count++;
pthread_cond_signal(&b->not_empty);
pthread_mutex_unlock(&b->mutex);
```
Insert at `tail`, advance `tail` with wraparound (circular buffer), increment count, wake the consumer. `pthread_cond_signal` wakes one waiting thread — the consumer in this case.

### `bounded_buffer_pop()` — consumer removes one chunk

```c
while (b->count == 0 && !b->shutting_down)
    pthread_cond_wait(&b->not_empty, &b->mutex);

if (b->count == 0) {
    pthread_mutex_unlock(&b->mutex);
    return 0;   // shutdown + empty → signal consumer to exit
}
```
Same `while` pattern. If we wake because of shutdown AND the buffer is empty, return 0 to tell the consumer thread to exit. This guarantees all log data written before shutdown is drained before the consumer exits.

```c
*item = b->items[b->head];
b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
b->count--;
pthread_cond_signal(&b->not_full);
```
Read from `head`, advance with wraparound, wake a blocked producer.

### `bounded_buffer_begin_shutdown()`

```c
pthread_mutex_lock(&b->mutex);
b->shutting_down = 1;
pthread_cond_broadcast(&b->not_empty);
pthread_cond_broadcast(&b->not_full);
pthread_mutex_unlock(&b->mutex);
```
`broadcast` (not `signal`) wakes **all** waiting threads. We need broadcast because there may be multiple producers blocked on `not_full` and the consumer blocked on `not_empty`. All of them need to wake up, check `shutting_down`, and exit.

### `producer_thread()` — one per container

```c
while (1) {
    ssize_t n = read(pa->read_fd, item.data, LOG_CHUNK_SIZE);
    if (n <= 0)
        break;   // pipe closed = container has exited
    item.length = (size_t)n;
    if (bounded_buffer_push(pa->buffer, &item) != 0)
        break;   // shutdown
}
close(pa->read_fd);
free(pa);
```
`read()` blocks until the container writes something to stdout. When the container exits, all its file descriptors close, the write end of the pipe closes, and `read()` returns 0 (EOF). The thread then exits cleanly, closes the read end, and frees its argument struct.

### `logging_thread()` — the single consumer

```c
mkdir(LOG_DIR, 0755);
while (1) {
    int got = bounded_buffer_pop(&ctx->log_buffer, &item);
    if (!got) break;   // shutdown + drained

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    write(fd, item.data, item.length);
    close(fd);
}
```
Each `log_item_t` carries the `container_id` that the producer stamped into it. The consumer uses this to route the chunk to the correct log file. Files are opened in `O_APPEND` mode — the OS guarantees that concurrent appends from multiple writes don't interleave at the byte level. Open/write/close per chunk means a crash mid-run still leaves all previously written chunks safely on disk.

---

## Task 4 — Kernel Memory Monitor Integration

**Goal:** The supervisor registers each container's host PID with the kernel module after `clone()`. The kernel module tracks RSS, fires soft-limit warnings and hard-limit SIGKILLs. The supervisor metadata correctly attributes the cause of death.

### `register_with_monitor()` — called in `handle_start` after clone

```c
static int register_with_monitor(int monitor_fd, const char *id,
                                 pid_t pid, unsigned long soft,
                                 unsigned long hard)
{
    if (monitor_fd < 0) return 0;   // module not loaded — silently skip
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0 ? -1 : 0;
}
```
`ioctl` sends the `monitor_request` struct to the kernel module through the `/dev/container_monitor` device file. `MONITOR_REGISTER` is the command code defined in `monitor_ioctl.h`. The **host PID** is passed — not the PID-namespace PID — because the kernel module operates in the host's PID namespace and uses `find_vpid` to locate the `task_struct`.

### `unregister_from_monitor()` — called in `handle_sigchld` after reaping

```c
if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0 && errno != ENOENT)
    return -1;
```
The `ENOENT` check is important. The kernel timer callback removes entries as soon as it detects a PID has exited. By the time SIGCHLD fires in user space and `handle_sigchld` tries to unregister, the kernel may have already removed the entry. `ENOENT` means "not found" — this is a normal race, not an error. Without ignoring it, dmesg fills with spurious "Unregister: not found" warnings.

### Attribution rule — how `ps` knows why a container died

The rule is implemented across three places:

**1. `handle_stop` — writes the flag before signalling:**
```c
c->stop_requested     = 1;       // MUST be set before kill()
c->termination_reason = REASON_STOPPED;
c->state              = CONTAINER_STOPPED;
// ... then kill(pid, SIGTERM) ...
```

**2. `handle_sigchld` — reads the flag when the death arrives:**
```c
if (c->stop_requested) {
    c->termination_reason = REASON_STOPPED;          // supervisor did it
} else if (c->exit_signal == SIGKILL) {
    c->termination_reason = REASON_HARD_LIMIT_KILLED; // kernel module did it
} else {
    c->termination_reason = REASON_UNKNOWN_SIGNAL;
}
```

**3. `handle_ps` — displays the reason:**
```c
} else if (c->termination_reason == REASON_HARD_LIMIT_KILLED) {
    snprintf(exit_info, sizeof(exit_info), "hard_limit_killed");
} else if (c->termination_reason == REASON_STOPPED) {
    snprintf(exit_info, sizeof(exit_info), "stopped(sig=%d)", c->exit_signal);
}
```

---

## Task 5 — Scheduler Experiments

No new code in `engine.c` or `monitor.c` for this task. The `--nice` flag was wired through:

1. `parse_optional_flags()` reads `--nice N` from the CLI into `control_request_t.nice_value`
2. `handle_start()` copies it into `child_config_t.nice_value` and `container_record_t.nice_value`
3. `child_fn()` calls `nice(cfg->nice_value)` inside the container before `execl`
4. `handle_ps()` displays it in the NICE column

The workloads used are the boilerplate's `cpu_hog` (CPU-bound) and `io_pulse` (I/O-bound). `cpu_hog` prints elapsed time and an accumulator every second — two containers started simultaneously with different `--nice` values allow comparison of CPU share. `memory_hog` is used for Task 4 memory limit experiments.

---

## Task 6 — Cleanup Verification

No new code needed — cleanup was designed into Tasks 1–4. The shutdown sequence in `run_supervisor()`:

```c
// 1. Signal all running containers
for (container_record_t *c = ctx.containers; c; c = c->next) {
    if (c->state == CONTAINER_RUNNING) {
        c->stop_requested = 1;
        kill(c->host_pid, SIGTERM);
    }
}

// 2. Reap all children (blocks until all containers are gone)
while (waitpid(-1, NULL, 0) > 0)
    ;

// 3. Shut down the log pipeline — wakes all blocked producer/consumer threads
bounded_buffer_begin_shutdown(&ctx.log_buffer);

// 4. Join producer threads (they exit when their pipe closes or buffer shuts down)
for (int i = 0; i < ctx.producer_count; i++)
    pthread_join(ctx.producers[i], NULL);

// 5. Join consumer thread (it drains remaining buffer entries first, then exits)
pthread_join(ctx.logger_thread, NULL);

// 6. Free the bounded buffer's mutex and condvars
bounded_buffer_destroy(&ctx.log_buffer);

// 7. Free all container metadata records
container_record_t *c = ctx.containers;
while (c) {
    container_record_t *next = c->next;
    free(c);
    c = next;
}

// 8. Close the control socket and remove the socket file
close(ctx.server_fd);
unlink(CONTROL_PATH);

// 9. Close the kernel monitor device fd
if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
```

The ordering matters: containers must be reaped before the logging pipeline shuts down, because producer threads are still reading from pipes until the container closes them. Calling `bounded_buffer_begin_shutdown` before joining producers would cause producers to drop their last log chunks.

---

## `monitor.c` — All Changes from Boilerplate

The boilerplate provided the device registration, timer skeleton, RSS helper, and soft/hard-limit helpers. All six TODOs were filled in.

### TODO 1 — `struct monitored_entry`

```c
struct monitored_entry {
    pid_t            pid;
    char             container_id[32];
    unsigned long    soft_limit_bytes;
    unsigned long    hard_limit_bytes;
    int              soft_warned;      // 1 after the first soft-limit warning is emitted
    struct list_head list;             // kernel doubly-linked list linkage
};
```

`soft_warned` prevents the soft-limit message from spamming dmesg every second once the container is over the soft limit. The warning fires once; the hard limit enforcement fires once and removes the entry.

`struct list_head` is the kernel's intrusive linked list type. It embeds the list pointers inside the data struct rather than allocating separate nodes.

### TODO 2 — Global list and lock

```c
static LIST_HEAD(monitored_list);    // initialises an empty doubly-linked list
static DEFINE_MUTEX(monitor_lock);   // kernel mutex
```

**Why a mutex and not a spinlock?** The `ioctl` path runs in process context and can sleep (it calls `copy_from_user` and `kzalloc`). Spinlocks cannot be held while sleeping. The timer callback runs in softirq context and cannot sleep either, so it uses `mutex_trylock` — if the lock is held by an ioctl, it skips the current tick and tries again next second. This is acceptable because a one-second delay in limit enforcement is fine.

### TODO 3 — `timer_callback()`

```c
if (!mutex_trylock(&monitor_lock))
    goto reschedule;
```
Non-blocking lock attempt. If an ioctl is in progress, just skip this tick.

```c
list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
    long rss = get_rss_bytes(entry->pid);
    if (rss < 0) {
        // PID no longer exists — remove stale entry
        list_del(&entry->list);
        kfree(entry);
        continue;
    }
```
`list_for_each_entry_safe` uses a `tmp` cursor so that deleting `entry` during iteration doesn't corrupt the list walk. Regular `list_for_each_entry` would crash if you deleted the current node inside the loop.

`get_rss_bytes` returns -1 if the task no longer exists. This handles the case where a container exited but `unregister_from_monitor` was called before the kernel timer ran — the timer cleans it up automatically.

```c
    if ((unsigned long)rss > entry->hard_limit_bytes) {
        kill_process(entry->container_id, entry->pid,
                     entry->hard_limit_bytes, rss);
        list_del(&entry->list);
        kfree(entry);
        continue;
    }
    if ((unsigned long)rss > entry->soft_limit_bytes && !entry->soft_warned) {
        log_soft_limit_event(...);
        entry->soft_warned = 1;
    }
```
Hard limit is checked first — if a process is over both limits, kill it immediately rather than also logging a soft warning. After killing, remove the entry so we don't try to kill an already-dead process next tick.

### TODOs 4 and 5 — IOCTL register and unregister

**Register:**
```c
entry = kzalloc(sizeof(*entry), GFP_KERNEL);
// kzalloc = kmalloc + zeroing. GFP_KERNEL = normal sleeping allocation.
// ... fill fields ...
mutex_lock(&monitor_lock);
list_add_tail(&entry->list, &monitored_list);
mutex_unlock(&monitor_lock);
```
`list_add_tail` appends to the end of the list. Order doesn't matter for correctness but tail insertion keeps the list in registration order which makes dmesg easier to read.

**Unregister:**
```c
list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
    if (entry->pid == req.pid &&
        strcmp(entry->container_id, req.container_id) == 0) {
        list_del(&entry->list);
        kfree(entry);
        found = 1;
        break;
    }
}
```
Match by both PID and container_id for safety. `list_del` unlinks the node from the list. `kfree` frees the memory. After `list_del`, the node's `list` pointers are poisoned (set to a debug value) so any accidental use-after-free is detectable.

### TODO 6 — `monitor_exit()` cleanup

```c
timer_delete_sync(&monitor_timer);
// Synchronously cancels the timer and waits for any in-progress callback to finish.
// Without this, the callback could fire after the list is freed → use-after-free.

mutex_lock(&monitor_lock);
list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
    list_del(&entry->list);
    kfree(entry);
}
mutex_unlock(&monitor_lock);
```
`timer_delete_sync` is the kernel equivalent of joining a timer thread — it guarantees the callback is not running before we proceed to free the list. Then we drain every remaining entry. If containers are still running when `rmmod` is called, their entries are cleaned up here so no memory leaks on unload.
