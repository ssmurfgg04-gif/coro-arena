/* co_server.cpp - the libco contestant for coro-arena.
 *
 * Tencent libco (WeChat's coroutine library) in its canonical server shape:
 * one process per core, single-threaded event loop, coroutine per
 * connection taken from an idle pool, shared stacks so physical stack
 * memory stays O(stacks) instead of O(connections).
 *
 * Hook usage (the part worth reading):
 *   - accepted fds are marked O_NONBLOCK through the fcntl hook, which
 *     records it in the fd's hook state, so read()/write() become single
 *     raw syscalls and we handle EAGAIN ourselves with co_poll()
 *   - the "pure blocking style" hooks are deliberately NOT used here:
 *     their default fd timeout is 1s (SO_RCVTIMEO can raise it) and their
 *     fd map caps at 102400 entries per process (we patch that array to
 *     1M for the arena). Both are real findings, see README.
 *
 * Straight-line code per connection, no state machine, no io_uring:
 *   co_poll(POLLIN) -> read -> route -> write (poll POLLOUT on EAGAIN)
 *
 * usage: co_server <port> <feed.json> <post.json> [procs=2] [stacks=128]
 *                 [stack_kb=128] [maxconn=400000]
 */
#define _GNU_SOURCE
#include "libco/co_routine.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "canned.h"
}

/* defined in co_hook_sys_call.cpp, not declared in any header (C++ linkage) */
int co_accept(int fd, struct sockaddr *addr, socklen_t *len);

struct task_t {
    stCoRoutine_t *co;
    int fd;
    char in[2048];
    int inlen;
};

/* per-process state; the event loop is single-threaded so no locks */
static int g_lfd = -1;
static std::vector<task_t *> g_idle;
static long long g_created, g_accepts, g_closes, g_reqs, g_wegain, g_regain;
static long long g_nfeed, g_npost, g_nlike, g_ncreate, g_nhealth, g_nunk;
static long long g_open;
static int g_maxconn = 400000;

static long rss_mb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size, resident;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0;
    fclose(f);
    return resident * 4096 / 1048576;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ---- the per-connection coroutine: straight-line request loop ---- */
static void *conn_routine(void *arg)
{
    co_enable_hook_sys();
    task_t *t = (task_t *)arg;

    for (;;) {
        if (t->fd == -1) {
            /* parked: waiting for the acceptor to hand us a connection */
            g_idle.push_back(t);
            co_yield_ct();
            continue;
        }

        int fd = t->fd;
        for (;;) {
            /* ---- read one full request (headers + content-length body) ---- */
            t->inlen = 0;
            for (;;) {
                char *eoh = NULL;
                if (t->inlen >= 4) {
                    char *lim = t->in + t->inlen - 3;
                    for (char *p = t->in; p <= lim; p++)
                        if (p[0]=='\r'&&p[1]=='\n'&&p[2]=='\r'&&p[3]=='\n') { eoh = p; break; }
                }
                long cl = 0;
                if (eoh) {
                    /* find content-length in the header region */
                    for (char *p = t->in; p < eoh; ) {
                        char *le = (char*)memmem((void*)p, eoh - p, "\r\n", 2);
                        if (!le) le = eoh;
                        if (le - p > 16 && !strncasecmp(p, "Content-Length:", 15))
                            cl = atol(p + 15);
                        p = le + 2;
                    }
                    long total = (eoh + 4 - t->in) + cl;
                    if (t->inlen >= total) break;   /* full request in hand */
                }
                if (t->inlen >= (int)sizeof t->in) goto drop; /* malformed */

                struct pollfd pf = { .fd = fd, .events = POLLIN | POLLERR | POLLHUP };
                co_poll(co_get_epoll_ct(), &pf, 1, -1);
                int r = read(fd, t->in + t->inlen, sizeof t->in - t->inlen);
                if (r > 0) { t->inlen += r; continue; }
                if (r == 0) goto drop;
                if (errno == EAGAIN || errno == EWOULDBLOCK) { g_regain++; continue; }
                if (errno == EINTR) continue;
                goto drop;
            }

            /* ---- route and answer with a canned response ---- */
            {
                char *eol = (char*)memmem(t->in, t->inlen, "\r\n", 2);
                if (!eol) eol = t->in + t->inlen;
                int rlen = 0;
                const char *resp = canned_route(t->in, (int)(eol - t->in), &rlen);
                if (rlen == 0) goto drop;
                if (eol - t->in >= 9 && !memcmp(t->in, "GET /feed", 9)) g_nfeed++;
                else if (eol - t->in >= 10 && !memcmp(t->in, "GET /posts", 10)) g_npost++;
                else if (eol - t->in >= 5 && !memcmp(t->in, "POST ", 5) &&
                         eol - t->in >= 12 && !memcmp(t->in + 5, "/posts/", 7) &&
                         memmem(t->in, eol - t->in, "/like", 5)) g_nlike++;
                else if (eol - t->in >= 5 && !memcmp(t->in, "POST ", 5)) g_ncreate++;
                else if (eol - t->in >= 11 && !memcmp(t->in, "GET /health", 11)) g_nhealth++;
                else g_nunk++;
                g_reqs++;

                int off = 0;
                while (off < rlen) {
                    int w = write(fd, resp + off, rlen - off);
                    if (w > 0) { off += w; continue; }
                    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        g_wegain++;
                        struct pollfd pf = { .fd = fd, .events = POLLOUT | POLLERR | POLLHUP };
                        co_poll(co_get_epoll_ct(), &pf, 1, 5000);
                        continue;
                    }
                    if (w < 0 && errno == EINTR) continue;
                    goto drop;
                }
            }
            /* keep-alive: loop for the next request on this connection */
        }
drop:
        close(fd);
        g_closes++;
        g_open--;
        t->fd = -1;
        break; /* back to the parking spot */
    }
    return NULL;
}

/* ---- the acceptor coroutine ---- */
static void *accept_routine(void *)
{
    co_enable_hook_sys();
    for (;;) {
        struct sockaddr_in addr;
        socklen_t len = sizeof addr;
        int fd = co_accept(g_lfd, (struct sockaddr *)&addr, &len);
        if (fd < 0) {
            struct pollfd pf = { .fd = g_lfd, .events = POLLIN | POLLERR | POLLHUP };
            co_poll(co_get_epoll_ct(), &pf, 1, 1000);
            continue;
        }
        if (g_accepts - g_closes >= g_maxconn || g_idle.empty()) {
            /* pool exhausted or at the cap: take the hit visibly, don't crash */
            close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        set_nonblock(fd);   /* recorded by the fcntl hook */
        g_accepts++;
        g_open++;
        task_t *t = g_idle.back();
        g_idle.pop_back();
        t->fd = fd;
        t->inlen = 0;
        co_resume(t->co);
    }
    return NULL;
}

/* ---- stats coroutine, one line every 5s, same shape as the sink ---- */
static void *stats_routine(void *)
{
    co_enable_hook_sys();
    long long t = 0, prev_reqs = 0, prev_acc = 0, prev_close = 0, prev_weg = 0;
    for (;;) {
        poll(NULL, 0, 5000);   /* hooked poll = coroutine-safe sleep */
        t += 5;
        fprintf(stderr,
            "costats: t=%llds pid=%d open=%lld acc=%lld (+%lld) closes=%lld (+%lld) "
            "reqs=%lld (+%lld) | feed=%lld post=%lld like=%lld create=%lld "
            "health=%lld unk=%lld | weagain=%lld (+%lld) reag=%lld | "
            "cos=%lld idle=%zu rss=%ldMB\n",
            t, (int)getpid(), g_open, g_accepts, g_accepts - prev_acc,
            g_closes, g_closes - prev_close,
            g_reqs, g_reqs - prev_reqs,
            g_nfeed, g_npost, g_nlike, g_ncreate, g_nhealth, g_nunk,
            g_wegain, g_wegain - prev_weg, g_regain,
            g_created, g_idle.size(), rss_mb());
        fflush(stderr);
        prev_reqs = g_reqs; prev_acc = g_accepts;
        prev_close = g_closes; prev_weg = g_wegain;
    }
    return NULL;
}

/* ---- per-process boot ---- */
static void grow_pool(int n, stShareStack_t *ss)
{
    stCoRoutineAttr_t attr;
    memset(&attr, 0, sizeof attr);
    attr.stack_size = 0;
    attr.share_stack = ss;
    for (int i = 0; i < n && (long long)g_created < g_maxconn; i++) {
        task_t *t = (task_t *)calloc(1, sizeof *t);
        if (!t) { perror("calloc"); exit(1); }
        t->fd = -1;
        co_create(&t->co, &attr, conn_routine, t);
        g_created++;
        co_resume(t->co);   /* runs until it parks itself in g_idle */
    }
}

static int child_main(int port, int stacks, int stack_kb)
{
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(g_lfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
    if (listen(g_lfd, 65536) < 0) { perror("listen"); exit(1); }
    set_nonblock(g_lfd);

    stShareStack_t *ss = co_alloc_sharestack(stacks, stack_kb * 1024);
    grow_pool(8192, ss);

    stCoRoutine_t *co = NULL;
    co_create(&co, NULL, accept_routine, NULL);
    co_resume(co);
    co_create(&co, NULL, stats_routine, NULL);
    co_resume(co);
    fprintf(stderr, "co_server: pid %d on :%d (%d shared stacks x %dKB)\n",
            (int)getpid(), port, stacks, stack_kb);
    co_eventloop(co_get_epoll_ct(), NULL, NULL);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: co_server <port> <feed.json> <post.json> "
                        "[procs=2] [stacks=128] [stack_kb=128] [maxconn=400000]\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int port = atoi(argv[1]);
    if (canned_init(argv[2], argv[3]) != 0) return 1;
    int procs = argc > 4 ? atoi(argv[4]) : 2;
    int stacks = argc > 5 ? atoi(argv[5]) : 128;
    int stack_kb = argc > 6 ? atoi(argv[6]) : 128;
    if (argc > 7) g_maxconn = atoll(argv[7]);
    if (procs < 1 || procs > 16) { fprintf(stderr, "procs 1..16\n"); return 1; }

    for (int i = 0; i < procs; i++) {
        pid_t pid = fork();
        if (pid == 0) return child_main(port, stacks, stack_kb);
        if (pid < 0) { perror("fork"); return 1; }
    }
    while (wait(NULL) > 0) {}
    return 0;
}
