/* muduo_server.cpp - the muduo contestant for coro-arena.
 *
 * muduo is Chen Shuo's teaching network library (the book "Linux
 * Multi-threaded Server Programming"): Reactor pattern, one loop per
 * thread, non-blocking IO with level-triggered epoll, classic callbacks.
 * This is what "clean framework C++" looks like at the same load the
 * coroutine crowd faces: same canned responses, same load generator.
 *
 * Concurrency shape: 2 IO event loops (setThreadNum(2)) + the acceptor
 * loop, all pinned to 2 cores by the runner script.
 *
 * build: g++ -O2 -std=c++11 -o muduo_server muduo_server.cpp <muduo srcs>
 * usage: muduo_server <port> <feed.json> <post.json>
 */
#include "muduo/net/TcpServer.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/TcpConnection.h"
#include "muduo/net/Buffer.h"
#include "muduo/base/Atomic.h"
#include "muduo/base/Logging.h"

#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <thread>

extern "C" {
#include "canned.h"
}

using namespace muduo;
using namespace muduo::net;

static AtomicInt64 g_conns, g_accepts, g_reqs, g_nfeed, g_npost, g_nlike,
                    g_ncreate, g_nhealth, g_nunk;

/* one request at a time per connection, same contract as every other
 * contestant: read headers, find Content-Length, wait for the full body,
 * answer with the canned response, discard the request */
static void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp)
{
    while (true) {
        const char *peek = buf->peek();
        size_t readable = buf->readableBytes();

        const char *eoh = NULL;
        for (size_t i = 0; i + 4 <= readable; i++) {
            if (peek[i] == '\r' && peek[i+1] == '\n' && peek[i+2] == '\r' && peek[i+3] == '\n') {
                eoh = peek + i;
                break;
            }
        }
        if (!eoh) {
            if (readable > 4096) conn->shutdown(); /* malformed */
            return;                                /* headers incomplete */
        }

        long cl = 0;
        const char *p = peek;
        while (p < eoh) {
            const char *le = (const char *)memmem(p, eoh - p, "\r\n", 2);
            if (!le) le = eoh;
            if (le - p > 16 && !strncasecmp(p, "Content-Length:", 15))
                cl = atol(p + 15);
            p = le + 2;
        }
        size_t total = (size_t)(eoh + 4 - peek) + (size_t)cl;
        if (readable < total) return;              /* body incomplete */

        int linelen = (int)(eoh - peek);
        int rlen = 0;
        const char *resp = canned_route(peek, linelen, &rlen);
        conn->send(resp, rlen);

        if (linelen >= 9  && !memcmp(peek, "GET /feed", 9))     g_nfeed.increment();
        else if (linelen >= 10 && !memcmp(peek, "GET /posts", 10)) g_npost.increment();
        else if (linelen >= 12 && !memcmp(peek + 5, "/posts/", 7) &&
                 linelen >= 5 && !memcmp(peek, "POST ", 5) &&
                 memmem(peek, linelen, "/like", 5))             g_nlike.increment();
        else if (linelen >= 5 && !memcmp(peek, "POST ", 5))     g_ncreate.increment();
        else if (linelen >= 11 && !memcmp(peek, "GET /health", 11)) g_nhealth.increment();
        else                                                    g_nunk.increment();
        g_reqs.increment();

        buf->retrieve(total);   /* whole request consumed */
    }
}

static void onConnection(const TcpConnectionPtr &conn)
{
    if (conn->connected()) {
        conn->setTcpNoDelay(true);
        g_accepts.increment();
        g_conns.increment();
    } else {
        g_conns.decrement();
    }
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

static void noop_out(const char *, int) {}
static void noop_flush() {}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: muduo_server <port> <feed.json> <post.json>\n");
        return 1;
    }
    Logger::setOutput(noop_out);
    Logger::setFlush(noop_flush);

    if (canned_init(argv[2], argv[3]) != 0) return 1;
    int port = atoi(argv[1]);
    int threads = argc > 4 ? atoi(argv[4]) : 2;

    EventLoop loop;
    InetAddress addr((uint16_t)port);
    TcpServer server(&loop, addr, "coro-arena");
    server.setConnectionCallback(onConnection);
    server.setMessageCallback(onMessage);
    server.setThreadNum(threads);
    server.start();

    std::thread stats([] {
        long long t = 0, prev = 0;
        long long pf = 0, pp = 0, pl = 0, pc = 0;
        for (;;) {
            sleep(5);
            t += 5;
            long long f = g_nfeed.get(), po = g_npost.get(), l = g_nlike.get(),
                      c = g_ncreate.get(), h = g_nhealth.get(), u = g_nunk.get();
            long long r = g_reqs.get();
            fprintf(stderr,
                "mudostats: t=%llds open=%lld acc=%lld reqs=%lld (+%lld) | "
                "feed=%lld (+%lld) post=%lld (+%lld) like=%lld (+%lld) "
                "create=%lld (+%lld) health=%lld unk=%lld | rss=%ldMB\n",
                t, g_conns.get(), g_accepts.get(), r, r - prev,
                f, f - pf, po, po - pp, l, l - pl, c, c - pc, h, u, rss_mb());
            fflush(stderr);
            prev = r; pf = f; pp = po; pl = l; pc = c;
        }
    });
    stats.detach();

    fprintf(stderr, "muduo_server: %d IO threads on :%d\n", threads, port);
    loop.loop();
    return 0;
}
