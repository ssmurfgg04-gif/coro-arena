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

## Results

See the Actions runs. Job summaries carry the verdict lines; artifacts
carry full loadgen/server logs. `BENCH_RESULT: <tag> server=<name>
RESULT: PASS|FAIL` is the machine-readable line.

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
