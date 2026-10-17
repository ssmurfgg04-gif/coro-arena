#!/usr/bin/env bash
# build-all.sh - builds every arena contestant from scratch.
# Each server binary lands in build/<name>.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build"
mkdir -p "$OUT"
cc -O2 -c -o "$OUT/canned.o" "$ROOT/common/canned.c" -I"$ROOT/common"

echo "== sink (raw epoll baseline)"
cc -O2 -Wall -Wextra -o "$OUT/sink" "$ROOT/servers/sink-epoll/sink.c" -lpthread

echo "== libco"
# one real patch, applied visibly: the hook fd map in stock libco caps at
# 102400 entries per process (g_rpchook_socket_fd[102400]); beyond that the
# hooks silently treat sockets as unregistered. Widened to 1M for the arena.
# 8MB of BSS, no behavior change below 102400.
LIB="$ROOT/servers/libco/libco"
sed -i.bak 's/g_rpchook_socket_fd\[ 102400 \]/g_rpchook_socket_fd[ 1048576 ]/' \
    "$LIB/co_hook_sys_call.cpp"
g++ -O2 -std=gnu++11 -Wall -Wno-deprecated \
    -o "$OUT/co_server" \
    "$ROOT/servers/libco/co_server.cpp" \
    "$LIB/co_routine.cpp" "$LIB/co_epoll.cpp" "$LIB/co_hook_sys_call.cpp" \
    "$LIB/co_comm.cpp" "$LIB/coctx.cpp" \
    "$LIB/coctx_swap.S" \
    -I"$LIB" -I"$ROOT/common" "$OUT/canned.o" -lpthread -ldl

echo "== libco corocount (stock aSpec + slim aSpec variants)"
g++ -O2 -std=gnu++11 -o "$OUT/corocount" \
    "$ROOT/corocount/corocount.cpp" \
    "$LIB/co_routine.cpp" "$LIB/co_epoll.cpp" "$LIB/co_hook_sys_call.cpp" \
    "$LIB/co_comm.cpp" "$LIB/coctx.cpp" \
    "$LIB/coctx_swap.S" \
    -I"$ROOT/servers/libco" -I"$LIB" -lpthread -ldl
# slim variant: aSpec[1024] (8KB/coroutine, unused by corocount) -> aSpec[4]
SLIM="$ROOT/corocount/libco_slim"
rm -rf "$SLIM"; mkdir -p "$SLIM"
cp "$LIB"/*.cpp "$LIB"/*.h "$LIB"/*.S "$SLIM/"
sed -i 's/stCoSpec_t aSpec\[1024\]/stCoSpec_t aSpec[4]/' "$SLIM/co_routine_inner.h"
g++ -O2 -std=gnu++11 -o "$OUT/corocount_slim" \
    "$ROOT/corocount/corocount.cpp" \
    "$SLIM/co_routine.cpp" "$SLIM/co_epoll.cpp" "$SLIM/co_hook_sys_call.cpp" \
    "$SLIM/co_comm.cpp" "$SLIM/coctx.cpp" \
    "$SLIM/coctx_swap.S" \
    -I"$ROOT/servers/libco" -I"$SLIM" -lpthread -ldl

echo "== facil.io"
cc -O2 -Wall -o "$OUT/fio_server" \
    "$ROOT/servers/facil/fio_server.c" "$ROOT/servers/facil/fio.c" \
    -I"$ROOT/servers/facil" -I"$ROOT/common" "$OUT/canned.o" -pthread

echo "== muduo"
if [ ! -d "$ROOT/servers/muduo/muduo" ]; then
    git clone -q --depth 1 https://github.com/chenshuo/muduo "$ROOT/servers/muduo/muduo"
fi
MU="$ROOT/servers/muduo/muduo"
# explicit file list (muduo's own CMakeLists selection minus examples/tests;
# boilerplate.cc and friends are not part of the library)
MU_SRC=$(echo \
  Acceptor Buffer Channel EventLoop EventLoopThread EventLoopThreadPool \
  InetAddress Poller Socket SocketsOps TcpConnection TcpServer Timer TimerQueue \
  | sed "s|[^ ]*|$MU/muduo/net/\&.cc|g")" \
  $MU/muduo/net/poller/DefaultPoller.cc $MU/muduo/net/poller/EPollPoller.cc $MU/muduo/net/poller/PollPoller.cc \
  $(ls $MU/muduo/base/*.cc | grep -v /tests/)
g++ -O2 -std=c++11 -w -o "$OUT/muduo_server" \
    "$ROOT/servers/muduo/muduo_server.cpp" $MU_SRC \
    -I"$MU" -I"$ROOT/common" "$OUT/canned.o" -lpthread -lrt

ls -la "$OUT"
echo "== build done"
