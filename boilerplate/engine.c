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

#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT  (64UL << 20)   /* 64 MiB */

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
typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    char               log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;
typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;
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
typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;
typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;
typedef struct {
    int                server_fd;
    int                monitor_fd;
    int                should_stop;
    pthread_t          logger_thread;
    bounded_buffer_t   log_buffer;
    pthread_mutex_t    metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;
static supervisor_ctx_t *g_ctx = NULL;
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
static int parse_mib_flag(const char *flag,const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}
static int parse_optional_flags(control_request_t *req,int argc,char *argv[],int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,"Invalid value for --nice (expected -20..19): %s\n",argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}
static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING: return "running";
    case CONTAINER_STOPPED: return "stopped";
    case CONTAINER_KILLED: return "killed";
    case CONTAINER_EXITED: return "exited";
    default: return "unknown";
    }
}
static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}
static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}
static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);
    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    if (buf->count == 0 && buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }
    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;
    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}
typedef struct {
    int               read_fd;
    char              container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_arg_t;
static void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *parg = (pipe_reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;
    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, parg->container_id, CONTAINER_ID_LEN - 1);
    while ((n = read(parg->read_fd, item.data, LOG_CHUNK_SIZE - 1)) > 0) {
        item.data[n] = '\0';
        item.length  = (size_t)n;
        bounded_buffer_push(parg->log_buffer, &item);
        memset(item.data, 0, sizeof(item.data));
    }
    close(parg->read_fd);
    free(parg);
    return NULL;
}
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char path[PATH_MAX];
        int  fd;
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            if (write(fd, item.data, item.length) < 0)
                perror("[supervisor] logging_thread write");
            close(fd);
        } else {
            perror("[supervisor] logging_thread open");
        }
    }

    fprintf(stderr, "[supervisor] logging thread exiting.\n");
    return NULL;
}
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    if (sethostname(cfg->id, strlen(cfg->id)) != 0)
        perror("[container] sethostname");
    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("[container] nice");
    }
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("[container] dup2 stdout");
        return 1;
    }
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("[container] dup2 stderr");
        return 1;
    }
    close(cfg->log_write_fd);
    if (chroot(cfg->rootfs) != 0) {
        perror("[container] chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("[container] chdir /");
        return 1;
    }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("[container] mount /proc");
    }
    char *argv_exec[] = { cfg->command, NULL };
    execvp(cfg->command, argv_exec);
    perror("[container] execvp");
    return 1;
}
int register_with_monitor(int monitor_fd,const char *container_id,pid_t host_pid,unsigned long soft_limit_bytes,unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid               = host_pid;
    req.soft_limit_bytes  = soft_limit_bytes;
    req.hard_limit_bytes  = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,const char *container_id,pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}
static container_record_t *find_container(supervisor_ctx_t *ctx,const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}
static void add_container(supervisor_ctx_t *ctx, container_record_t *rec)
{
    rec->next = ctx->containers;
    ctx->containers = rec;
}
static void sigchld_handler(int sig)
{
    (void)sig;
    int   status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx)
            continue;
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFSIGNALED(status)) {
                    c->state      = CONTAINER_KILLED;
                    c->exit_signal = WTERMSIG(status);
                    c->exit_code  = 0;
                    fprintf(stderr,"[supervisor] container %s (pid %d) killed by signal %d\n",c->id, pid, c->exit_signal);
                } else {
                    c->state     = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                    fprintf(stderr,
                            "[supervisor] container %s (pid %d) exited with code %d\n",c->id, pid, c->exit_code);
                }
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd,c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}
static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t  req;
    control_response_t res;
    ssize_t            n;
    memset(&req, 0, sizeof(req));
    memset(&res, 0, sizeof(res));
    n = read(client_fd, &req, sizeof(req));
    if (n != (ssize_t)sizeof(req)) {
        res.status = -1;
        snprintf(res.message, sizeof(res.message), "bad request size");
        write(client_fd, &res, sizeof(res));
        return;
    }
    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            res.status = -1;
            snprintf(res.message, sizeof(res.message),
                     "pipe: %s", strerror(errno));
            write(client_fd, &res, sizeof(res));
            return;
        }
        child_config_t *cfg = calloc(1, sizeof(*cfg));
        if (!cfg) {
            close(pipefd[0]); close(pipefd[1]);
            res.status = -1;
            snprintf(res.message, sizeof(res.message), "calloc failed");
            write(client_fd, &res, sizeof(res));
            return;
        }
        strncpy(cfg->id,      req.container_id, CONTAINER_ID_LEN - 1);
        strncpy(cfg->rootfs,  req.rootfs,        PATH_MAX - 1);
        strncpy(cfg->command, req.command,        CHILD_COMMAND_LEN - 1);
        cfg->nice_value   = req.nice_value;
        cfg->log_write_fd = pipefd[1];   
        char *stack = malloc(STACK_SIZE);
        if (!stack) {
            free(cfg);
            close(pipefd[0]); close(pipefd[1]);
            res.status = -1;
            snprintf(res.message, sizeof(res.message), "malloc stack failed");
            write(client_fd, &res, sizeof(res));
            return;
        }
        int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
        pid_t pid = clone(child_fn, stack + STACK_SIZE, flags, cfg);
        close(pipefd[1]);
        if (pid < 0) {
            free(cfg);
            free(stack);
            close(pipefd[0]);
            res.status = -1;
            snprintf(res.message, sizeof(res.message),
                     "clone: %s", strerror(errno));
            write(client_fd, &res, sizeof(res));
            return;
        }
        container_record_t *rec = calloc(1, sizeof(*rec));
        if (!rec) {
            kill(pid, SIGKILL);
            free(cfg);
            close(pipefd[0]);
            res.status = -1;
            snprintf(res.message, sizeof(res.message), "calloc record failed");
            write(client_fd, &res, sizeof(res));
            return;
        }
        strncpy(rec->id, req.container_id, CONTAINER_ID_LEN - 1);
        rec->host_pid         = pid;
        rec->started_at       = time(NULL);
        rec->state            = CONTAINER_RUNNING;
        rec->soft_limit_bytes = req.soft_limit_bytes;
        rec->hard_limit_bytes = req.hard_limit_bytes;
        snprintf(rec->log_path, PATH_MAX, "%s/%s.log",
                 LOG_DIR, req.container_id);
        pthread_mutex_lock(&ctx->metadata_lock);
        add_container(ctx, rec);
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (ctx->monitor_fd >= 0) {
            if (register_with_monitor(ctx->monitor_fd,
                                      req.container_id, pid,
                                      req.soft_limit_bytes,
                                      req.hard_limit_bytes) < 0)
                fprintf(stderr,
                        "[supervisor] warning: register_with_monitor failed: %s\n",
                        strerror(errno));
        }
        pipe_reader_arg_t *parg = calloc(1, sizeof(*parg));
        if (parg) {
            parg->read_fd    = pipefd[0];
            parg->log_buffer = &ctx->log_buffer;
            strncpy(parg->container_id, req.container_id,
                    CONTAINER_ID_LEN - 1);
            pthread_t reader;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&reader, &attr,
                               pipe_reader_thread, parg) != 0) {
                perror("[supervisor] pthread_create pipe_reader");
                close(pipefd[0]);
                free(parg);
            }
            pthread_attr_destroy(&attr);
        } else {
            close(pipefd[0]);
        }
        free(cfg);  
        fprintf(stderr,
                "[supervisor] started container %s pid=%d\n",
                req.container_id, pid);
        res.status = 0;
        snprintf(res.message, sizeof(res.message),
                 "started container %s pid=%d", req.container_id, pid);
        write(client_fd, &res, sizeof(res));
        if (req.kind == CMD_RUN) {
            int   wstatus = 0;
            pid_t wpid;
            do {
                wpid = waitpid(pid, &wstatus, 0);
            } while (wpid < 0 && errno == EINTR);
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *c2 = find_container(ctx, req.container_id);
            if (c2) {
                if (WIFSIGNALED(wstatus)) {
                    c2->state      = CONTAINER_KILLED;
                    c2->exit_signal = WTERMSIG(wstatus);
                } else {
                    c2->state     = CONTAINER_EXITED;
                    c2->exit_code = WEXITSTATUS(wstatus);
                }
            }
            pthread_mutex_unlock(&ctx->metadata_lock);
            if (ctx->monitor_fd >= 0)
                unregister_from_monitor(ctx->monitor_fd,
                                        req.container_id, pid);
            control_response_t res2;
            memset(&res2, 0, sizeof(res2));
            if (WIFSIGNALED(wstatus)) {
                res2.status = 128 + WTERMSIG(wstatus);
                snprintf(res2.message, sizeof(res2.message),
                         "container %s killed by signal %d",
                         req.container_id, WTERMSIG(wstatus));
            } else {
                res2.status = WEXITSTATUS(wstatus);
                snprintf(res2.message, sizeof(res2.message),
                         "container %s exited with code %d",
                         req.container_id, WEXITSTATUS(wstatus));
            }
            write(client_fd, &res2, sizeof(res2));
        }
        return;
    }
    if (req.kind == CMD_PS) {
        char buf[4096];
        int  off = 0;
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        if (!c) {
            off += snprintf(buf + off, sizeof(buf) - off,
                            "No containers tracked.\n");
        } else {
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8s %-10s %-24s %s\n",
                            "ID", "PID", "STATE", "STARTED", "LOG");
            while (c && off < (int)sizeof(buf) - 1) {
                char tstr[32];
                struct tm *tm_info = localtime(&c->started_at);
                strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", tm_info);
                off += snprintf(buf + off, sizeof(buf) - off,
                                "%-16s %-8d %-10s %-24s %s\n",
                                c->id, c->host_pid,
                                state_to_string(c->state),
                                tstr, c->log_path);
                c = c->next;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        res.status = 0;
        strncpy(res.message, buf, sizeof(res.message) - 1);
        write(client_fd, &res, sizeof(res));
        return;
    }
    if (req.kind == CMD_LOGS) {
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path),
                 "%s/%s.log", LOG_DIR, req.container_id);
        int lfd = open(log_path, O_RDONLY);
        if (lfd < 0) {
            res.status = -1;
            snprintf(res.message, sizeof(res.message),
                     "cannot open log for %s: %s",
                     req.container_id, strerror(errno));
            write(client_fd, &res, sizeof(res));
            return;
        }
        res.status = 0;
        snprintf(res.message, sizeof(res.message),
                 "log for %s:", req.container_id);
        write(client_fd, &res, sizeof(res));
        char chunk[4096];
        ssize_t nr;
        while ((nr = read(lfd, chunk, sizeof(chunk))) > 0)
            write(client_fd, chunk, (size_t)nr);
        close(lfd);
        return;
    }
    if (req.kind == CMD_STOP) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            res.status = -1;
            snprintf(res.message, sizeof(res.message),
                     "container %s not found", req.container_id);
            write(client_fd, &res, sizeof(res));
            return;
        }
        pid_t target_pid = c->host_pid;
        c->state         = CONTAINER_STOPPED;
        pthread_mutex_unlock(&ctx->metadata_lock);
        kill(target_pid, SIGTERM);
        if (ctx->monitor_fd >= 0)
            unregister_from_monitor(ctx->monitor_fd,
                                    req.container_id, target_pid);
        fprintf(stderr,
                "[supervisor] stopped container %s pid=%d\n",
                req.container_id, target_pid);
        res.status = 0;
        snprintf(res.message, sizeof(res.message),
                 "stopped container %s", req.container_id);
        write(client_fd, &res, sizeof(res));
        return;
    }
    res.status = -1;
    snprintf(res.message, sizeof(res.message), "unknown command %d", req.kind);
    write(client_fd, &res, sizeof(res));
}
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    (void)rootfs; 
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx          = &ctx;
    mkdir(LOG_DIR, 0755);
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc; perror("pthread_mutex_init"); return 1;
    }
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] warning: cannot open /dev/container_monitor: %s\n"
                "[supervisor] memory monitoring disabled.\n",
                strerror(errno));
    else
        fprintf(stderr, "[supervisor] kernel monitor opened.\n");
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket"); goto cleanup;
    }
    unlink(CONTROL_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); goto cleanup;
    }
    {
        struct sigaction sa_chld, sa_term;
        memset(&sa_chld, 0, sizeof(sa_chld));
        sa_chld.sa_handler = sigchld_handler;
        sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa_chld, NULL);
        memset(&sa_term, 0, sizeof(sa_term));
        sa_term.sa_handler = sigterm_handler;
        sa_term.sa_flags   = SA_RESTART;
        sigaction(SIGINT,  &sa_term, NULL);
        sigaction(SIGTERM, &sa_term, NULL);
    }
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc; perror("pthread_create logger"); goto cleanup;
    }
    fprintf(stderr,
            "[supervisor] ready. Listening on %s\n", CONTROL_PATH);
    while (!ctx.should_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;   
            perror("select");
            break;
        }
        if (sel == 0) continue;             
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        handle_client(&ctx, client_fd);
        close(client_fd);
    }
    fprintf(stderr, "[supervisor] shutting down...\n");
    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c = ctx.containers;
        while (c) {
            if (c->state == CONTAINER_RUNNING ||
                c->state == CONTAINER_STARTING) {
                kill(c->host_pid, SIGTERM);
                c->state = CONTAINER_STOPPED;
            }
            c = c->next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c = ctx.containers;
        while (c) {
            container_record_t *next = c->next;
            free(c);
            c = next;
        }
        ctx.containers = NULL;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
cleanup:
    if (ctx.server_fd >= 0)  { close(ctx.server_fd);  unlink(CONTROL_PATH); }
    if (ctx.monitor_fd >= 0)   close(ctx.monitor_fd);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    fprintf(stderr, "[supervisor] exited cleanly.\n");
    return 0;
}
static int send_control_request(const control_request_t *req)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n" "Is the supervisor running? Try: sudo ./engine supervisor <rootfs>\n", CONTROL_PATH, strerror(errno));
        close(sock);
        return 1;
    }
    if (write(sock, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(sock);
        return 1;
    }
    control_response_t res;
    ssize_t n = read(sock, &res, sizeof(res));
    if (n <= 0) {
        fprintf(stderr, "No response from supervisor.\n");
        close(sock);
        return 1;
    }
    printf("%s\n", res.message);
    if (req->kind == CMD_LOGS && res.status == 0) {
        char chunk[4096];
        ssize_t nr;
        while ((nr = read(sock, chunk, sizeof(chunk))) > 0)
            fwrite(chunk, 1, (size_t)nr, stdout);
    }
    if (req->kind == CMD_RUN) {
        control_response_t res2;
        memset(&res2, 0, sizeof(res2));
        n = read(sock, &res2, sizeof(res2));
        if (n > 0) {
            printf("%s\n", res2.message);
            close(sock);
            return res2.status;
        }
    }
    close(sock);
    return res.status < 0 ? 1 : 0;
}
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,"Usage: %s start <id> <container-rootfs> <command>" " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind              = CMD_START;
    req.soft_limit_bytes  = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes  = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}
static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,"Usage: %s run <id> <container-rootfs> <command>" " [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind              = CMD_RUN;
    req.soft_limit_bytes  = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes  = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
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
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}
static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}
int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
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
