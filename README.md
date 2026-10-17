# coro-arena

Head-to-head concurrency shootout on GitHub Actions runners: the proven
open-source designs against the same load, instead of another from-scratch
server.

## Why

The twelve-dollar-server-challenge bench rig on GitHub runners has a wall:
a canned-response sink server with zero application logic accepts exactly
262,141 of 300,000 connections and fails the 300k level with the same
signature as any real submission (6.8% request timeouts, p95/p99 = 25s).
262,141 is suspiciously close to 262,144 = 2^18, which smells like the
runner's default `nf_conntrack_max`. The arena raises and logs conntrack so
the 300k level measures the servers instead of the rig.

## Contestants

| server | design | source |
|---|---|---|
| `sink` | raw epoll, thread per core, no framework | baseline (tdsc-bench) |
| `co_server` | coroutine per connection, shared stacks | Tencent/libco (WeChat), Apache-2.0 |
| `muduo_server` | Reactor, one loop per thread | chenshuo/muduo, BSD |
| `fio_server` | evented C micro-framework | facil.io, MIT |

All four answer byte-identical canned responses (loaded from the same
JSON bodies as the sink baseline) and face the same C load generator with
the challenge's user model. Server pinned to cores 0-1, loadgen to 2-3:
the "2-core server" shape.

## The corocount job

libco's README claims shared-stack mode supports tens of millions of
connections per machine. Measured facts so far (local, stock build):

- `stCoRoutine_t` carries `stCoSpec_t aSpec[1024]` = 8KB of
  routine-specific storage per coroutine, unused unless you call
  `co_setspecific`. 9,165 bytes/coroutine measured at 100k coroutines.
- context switch: ~39 ns when the two coroutines live on different shared
  stacks (pure register/rsp swap), ~67 ns when they share one physical
  stack (save-out + restore-back of the used region, the "copy-stack" path)

The corocount job runs 1M / 3M / 5M coroutines, stock vs slim
(`aSpec[1024]` -> `aSpec[4]`), and reports create rate, resident memory,
event-loop liveness with everyone asleep, and switch cost.

## Two libco footguns found while porting (documented, not hidden)

1. `g_rpchook_socket_fd[102400]`: the syscall-hook fd map caps at 102,400
   entries per process. Above that, `get_by_fd()` returns NULL and the
   read/write hooks silently degrade to single raw syscalls on
   blocking-mode sockets, which park the whole thread. Arena patch:
   widened to 1,048,576 (8MB BSS, no behavior change below 102,400).
2. The "write blocking-style code" hooks default to a 1s poll timeout
   (`alloc_by_fd`: `read_timeout.tv_sec = 1`; raise via `SO_RCVTIMEO`,
   which the setsockopt hook intercepts). On timeout the hook still calls
   the syscall, so an idle connection past the timeout blocks the thread.
   The arena server instead marks fds O_NONBLOCK through the fcntl hook
   (so read/write stay single raw syscalls) and waits explicitly with
   `co_poll()`, which is the same syscall count without the cliff.

## Results (first full run, GitHub runners, conntrack raised, server pinned to cores 0-1)

| server | 20k | 100k | 250k | 300k |
|---|---|---|---|---|
| sink (raw epoll) | PASS | PASS | PASS | PASS (p99 1ms, 300k conns) |
| co_server (libco) | PASS | PASS | PASS | PASS (p99 1ms, 150k conns/proc, 1.7GB RSS/proc) |
| muduo_server | PASS | PASS | PASS (avg 0.1ms) | PASS (p99 1ms) |
| fio_server (facil.io) | PASS | PASS | **FAIL 96%** | **FAIL 98%** |

- The canned-response layer is not the differentiator people expect: raw
  epoll, muduo's Reactor and libco's coroutines all hold 300k connections
  at p99 1ms on two pinned cores. What differs is per-connection memory
  and the shape of the code you maintain.
- facil.io passes 100k cleanly and stops accepting around ~94k
  connections (its internal fd map ceiling); at 250k+ the loadgen
  reconnect-storms and 96% of requests fail. Out of the box, that is.
- libco network-layer memory: ~5.5KB per connection (coroutine + task
  struct + save buffer), 1.7GB per process at 150k connections.

## corocount results (the "5M concurrent tasks" question)

| variant | 1M | 3M | 5M |
|---|---|---|---|
| stock aSpec[1024] | 8.7GB (9,170 B/co) | 15GB, **runner VM shut down** | (does not fit) |
| slim aSpec[4] | 966MB (1,010 B/co) | 2.9GB, created in 2.0s | **4.8GB, created in 3.3s** |

5M coroutines in one process: 4,811MB RSS, event loop healthy at its 1ms
tick (4,677 ticks / 5s, zero spurious wakeups with everyone asleep),
54ns per swap on the copy-stack path, 33ns when the pair lives on
different shared stacks. So the claim is real - after slimming the 8KB
per-coroutine `aSpec` array the open-source tree ships (unused unless
you call `co_setspecific`). Stock libco cannot pass ~2M coroutines on a
16GB machine.

## The runner rig, characterized

- default `nf_conntrack_max` on the runners is 262144; once that many
  connections are tracked, SYNs are silently dropped. This is the wall
  that made every 300k run fail (including a canned-response sink with
  no application logic). Raising it to 1048576 moves the wall past 350k.
- runner VMs vary: identical code, identical tuning, 0% CPU steal on
  both, and one VM serves 250k at p95 50ms while another serves the same
  run at p95 2500ms. Failures cluster in time windows, not commits.
  Single verdicts at this level are noise; the runs here log
  `RIG:` lines (steal, loadavg, PSI, CPU model) to catch it.

Job summaries carry the verdict lines; artifacts carry full
loadgen/server logs. `BENCH_RESULT: <tag> server=<name> RESULT:
PASS|FAIL` is the machine-readable line.

## Layout

```
common/        canned responses shared by all contestants
loadgen/       C load generator (same as the tdsc-bench rig) + bodies
servers/       sink (baseline), libco, facil, muduo
corocount/     libco coroutine capacity + switch-cost measurement
scripts/       build-all.sh, run-arena.sh
```

Vendored third-party code keeps its upstream license files
(`servers/libco/libco/LICENSE.txt`, `servers/facil/LICENSE`). muduo is
cloned at build time, not vendored.
