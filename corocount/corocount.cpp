/* corocount.cpp - how many coroutines can libco actually hold?
 *
 * The claim on the box: shared-stack mode supports "tens of millions of
 * connections on one machine". What the open-source code actually costs:
 *   - stCoRoutine_t contains stCoSpec_t aSpec[1024] = 8KB per coroutine
 *     (routine-specific storage, unused unless you call co_setspecific)
 *   - a save_buffer per swapped-out coroutine sized by real stack depth
 *   - physical stacks: O(stacks), not O(coroutines) - that part is real
 *
 * Measures four things:
 *   1. create rate + resident memory while creating N sleeping coroutines
 *   2. event-loop liveness (1ms ticks) with all of them parked
 *   3. raw switch cost when two coroutines live on DIFFERENT shared
 *      stacks (no stack copy) - pure register/rsp swap
 *   4. switch cost when two coroutines share ONE physical stack (the
 *      copy-stack path: save used region out, restore partner's back)
 *
 * build: g++ -O2 -o corocount corocount.cpp libco/*.cpp libco/*.S -lpthread -ldl
 * usage: corocount <n_coroutines> [stack_kb=128]
 */
#define _GNU_SOURCE
#include "libco/co_routine.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static long long g_created, g_awake, g_ticks;
static double g_loop_until;
static stCoRoutine_t *g_pair[2];

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
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

/* 1. the payload: sleep 60s at a time, forever */
static void *sleeper(void *)
{
    co_enable_hook_sys();
    for (;;) {
        g_awake++;
        poll(NULL, 0, 60000);   /* hooked poll: parks the coroutine */
        g_awake++;
    }
    return NULL;
}

/* 3+4. the ping-pong payload: run one step, yield back to the driver */
static void *stepper(void *)
{
    co_enable_hook_sys();
    for (;;) {
        g_ticks++;
        co_yield_ct();
    }
    return NULL;
}

static int loop_pfn(void *)
{
    g_ticks++;
    return now_s() >= g_loop_until ? -1 : 0;
}

static void switch_bench(int stacks, long long iters)
{
    stShareStack_t *ss = co_alloc_sharestack(stacks, 128 * 1024);
    stCoRoutineAttr_t attr;
    memset(&attr, 0, sizeof attr);
    attr.stack_size = 0;
    attr.share_stack = ss;

    for (int i = 0; i < 2; i++) {
        co_create(&g_pair[i], &attr, stepper, NULL);
        co_resume(g_pair[i]);   /* runs one step, yields back */
    }
    long long base_ticks = g_ticks;
    (void)base_ticks;
    double t0 = now_s();
    for (long long i = 0; i < iters; i++)
        co_resume(g_pair[i & 1]);   /* alternates -> with stacks=1 this is
                                       always the copy-stack path */
    double dt = now_s() - t0;
    printf("corocount: %s: %lld swaps (%.0f ns/swap) total %.2fs\n",
           stacks == 1 ? "shared-1-stack(copy-stack path)" : "2-stacks(no copy)",
           iters, dt > 0 ? dt * 1e9 / iters : 0.0, dt);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: corocount <n_coroutines> [stack_kb=128]\n");
        return 1;
    }
    long long n = atoll(argv[1]);
    int stack_kb = argc > 2 ? atoi(argv[2]) : 128;
    const int NSTACKS = 128;

    /* ---- 1. capacity ---- */
    stShareStack_t *ss = co_alloc_sharestack(NSTACKS, stack_kb * 1024);
    stCoRoutineAttr_t attr;
    memset(&attr, 0, sizeof attr);
    attr.stack_size = 0;
    attr.share_stack = ss;

    long long rss0 = rss_mb();
    double t0 = now_s();

    for (long long i = 0; i < n; i++) {
        stCoRoutine_t *co = NULL;
        co_create(&co, &attr, sleeper, NULL);
        co_resume(co);
        g_created++;
        if ((i + 1) % 1000000 == 0) {
            printf("corocount: created %lldM/%lldM in %.1fs rss=%ldMB "
                   "(%.0f bytes/coroutine so far)\n",
                   (i + 1) / 1000000, n / 1000000, now_s() - t0, rss_mb(),
                   (double)(rss_mb() - rss0) * 1048576.0 / (i + 1));
            fflush(stdout);
        }
    }
    printf("corocount: created all %lld in %.1fs rss=%ldMB base=%ldMB "
           "bytes/coroutine=%.0f\n",
           n, now_s() - t0, rss_mb(), rss0,
           n > 0 ? (double)(rss_mb() - rss0) * 1048576.0 / n : 0.0);
    fflush(stdout);

    /* ---- 2. liveness: drive the real event loop for 5s ---- */
    {
        g_ticks = 0;
        long long awake0 = g_awake;
        g_loop_until = now_s() + 5.0;
        double t1 = now_s();
        co_eventloop(co_get_epoll_ct(), loop_pfn, NULL);
        printf("corocount: eventloop 5.0s: %lld 1ms ticks, %lld wakeups, "
               "rss=%ldMB\n",
               g_ticks, g_awake - awake0, rss_mb());
        fflush(stdout);
    }

    /* ---- 3+4. switch cost ---- */
    switch_bench(1, 10000000);
    switch_bench(2, 10000000);

    printf("corocount: DONE n=%lld rss=%ldMB\n", n, rss_mb());
    return 0;
}
