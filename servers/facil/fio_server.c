/* fio_server.c - the facil.io contestant for coro-arena.
 *
 * facil.io (Boaz Segev, MIT) is a compact C evented framework: epoll on
 * Linux, thread pool, protocol-object callbacks per connection, buffered
 * deferred writes. This is the "evented C micro-framework" entry against
 * the coroutine crowd: same canned responses, same load generator.
 *
 * facil.io's write path buffers internally and flushes when the socket is
 * writable, so backpressure is the library's job (memory tells us how
 * that goes at 250k+ connections).
 *
 * build: cc -O2 -o fio_server fio_server.c fio.c ../common/canned.c -pthread
 * usage: fio_server <port> <feed.json> <post.json> [threads=2]
 */
#define _GNU_SOURCE
#include "fio.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "canned.h"

/* protocol + per-connection input buffer in one allocation
 * (facil requires a unique protocol object per connection) */
typedef struct {
    fio_protocol_s p;
    char in[2048];
    int inlen;
    int closing;
} cproto_t;

static fio_protocol_s PROTO_TEMPLATE;

/* counters: facil runs the callbacks on its IO threads; atomics are honest */
static long long g_reqs, g_nfeed, g_npost, g_nlike, g_ncreate, g_nhealth, g_nunk;
static long long g_open, g_alloc_fail;

static void stat_bump(const char *line, int len)
{
    if (len >= 9  && !memcmp(line, "GET /feed", 9))      __sync_fetch_and_add(&g_nfeed, 1);
    else if (len >= 10 && !memcmp(line, "GET /posts", 10)) __sync_fetch_and_add(&g_npost, 1);
    else if (len >= 5 && !memcmp(line, "POST ", 5)) {
        if (len >= 12 && !memcmp(line + 5, "/posts/", 7) && memmem(line, len, "/like", 5))
            __sync_fetch_and_add(&g_nlike, 1);
        else
            __sync_fetch_and_add(&g_ncreate, 1);
    }
    else if (len >= 11 && !memcmp(line, "GET /health", 11)) __sync_fetch_and_add(&g_nhealth, 1);
    else __sync_fetch_and_add(&g_nunk, 1);
    __sync_fetch_and_add(&g_reqs, 1);
}

static void on_data(intptr_t uuid, fio_protocol_s *prt)
{
    cproto_t *c = (cproto_t *)prt;
    char tmp[1024];
    ssize_t r;

    while ((r = fio_read(uuid, tmp, sizeof tmp)) > 0) {
        if (c->inlen + r > (int)sizeof c->in) { c->closing = 1; fio_close(uuid); return; }
        memcpy(c->in + c->inlen, tmp, r);
        c->inlen += r;

        /* headers complete? */
        char *eoh = (char *)memmem(c->in, c->inlen, "\r\n\r\n", 4);
        if (!eoh) continue;

        long cl = 0;
        for (char *p = c->in; p < eoh; ) {
            char *le = (char *)memmem(p, eoh - p, "\r\n", 2);
            if (!le) le = eoh;
            if (le - p > 16 && !strncasecmp(p, "Content-Length:", 15))
                cl = atol(p + 15);
            p = le + 2;
        }
        long total = (long)(eoh + 4 - c->in) + cl;
        if (c->inlen < total) continue;      /* body incomplete */

        int linelen = (int)((char *)memmem(c->in, c->inlen, "\r\n", 2) - c->in);
        int rlen = 0;
        const char *resp = canned_route(c->in, linelen, &rlen);
        fio_write(uuid, resp, rlen);
        stat_bump(c->in, linelen);

        /* consume the whole request; keep any pipelined remainder */
        int rest = c->inlen - total;
        if (rest > 0) memmove(c->in, c->in + total, rest);
        c->inlen = rest;
    }
}

static void on_close(intptr_t uuid, fio_protocol_s *prt)
{
    (void)uuid;
    __sync_fetch_and_sub(&g_open, 1);
    free(prt);
}

static void on_open(intptr_t uuid, void *udata)
{
    (void)udata;
    cproto_t *c = malloc(sizeof *c);
    if (!c) { __sync_fetch_and_add(&g_alloc_fail, 1); fio_close(uuid); return; }
    c->p = PROTO_TEMPLATE;
    c->inlen = 0;
    c->closing = 0;
    __sync_fetch_and_add(&g_open, 1);
    fio_attach(uuid, &c->p);
}

static long rss_mb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size, resident;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0;
    fclose(f);
    return resident * 4096 / 1048576;
}

static void *stats_thread(void *arg)
{
    (void)arg;
    long long t = 0, prev = 0;
    for (;;) {
        sleep(5);
        t += 5;
        fprintf(stderr,
            "fiostats: t=%llds open=%lld alloc_fail=%lld reqs=%lld (+%lld) | "
            "feed=%lld post=%lld like=%lld create=%lld health=%lld unk=%lld | "
            "rss=%ldMB\n",
            t, g_open, g_alloc_fail, g_reqs, g_reqs - prev,
            g_nfeed, g_npost, g_nlike, g_ncreate, g_nhealth, g_nunk, rss_mb());
        fflush(stderr);
        prev = g_reqs;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: fio_server <port> <feed.json> <post.json> [threads=2]\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    if (canned_init(argv[2], argv[3]) != 0) return 1;
    int port = atoi(argv[1]);
    int threads = argc > 4 ? atoi(argv[4]) : 2;

    PROTO_TEMPLATE = (fio_protocol_s){
        .on_data = on_data,
        .on_close = on_close,
    };

    pthread_t st;
    pthread_create(&st, NULL, stats_thread, NULL);
    pthread_detach(st);

    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    if (fio_listen(.port = portstr, .on_open = on_open) == -1) {
        perror("fio_listen");
        return 1;
    }
    fprintf(stderr, "fio_server: %d threads on :%d\n", threads, port);
    fio_start(.threads = threads, .workers = 1);
    return 0;
}
