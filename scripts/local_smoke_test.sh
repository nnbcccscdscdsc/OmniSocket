#!/usr/bin/env bash
# 本机一键 smoke 测试：
# - test1: TCP 直连 client -> server 文件一致性
# - test2: UDP client -> relay -> server，包含动态目标切换
set -euo pipefail

# 根目录与构建产物目录。
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
# 每次测试创建独立临时目录，避免互相污染。
TMP_DIR="$(mktemp -d /tmp/omnisocket-smoke.XXXXXX)"

# 随机选择一组端口，降低被系统中已有进程占用的概率。
BASE_PORT=$((20000 + (RANDOM % 20000)))
DIRECT_PORT="$BASE_PORT"
RELAY_PORT=$((BASE_PORT + 1))
SINK1_PORT=$((BASE_PORT + 2))
SINK2_PORT=$((BASE_PORT + 3))

# 记录后台进程 PID，统一在 cleanup 中回收。
PIDS=()

cleanup() {
    # 无论脚本成功/失败，都尽量回收子进程，避免残留占端口。
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    # 删除临时目录与中间文件。
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

log() {
    printf '[smoke] %s\n' "$1"
}

wait_with_timeout() {
    # 轮询等待某个 PID 退出，超时返回非 0。
    # 参数：
    #   $1 pid
    #   $2 timeout_s
    local pid="$1"
    local timeout_s="$2"
    local i
    for ((i = 0; i < timeout_s * 10; ++i)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}

log "ports direct=$DIRECT_PORT relay=$RELAY_PORT sink1=$SINK1_PORT sink2=$SINK2_PORT"
log "building native binaries"
# 统一从干净状态构建。
make -C "$ROOT_DIR" clean all >/dev/null

# 测试输入与输出文件路径。
INPUT_FILE="$TMP_DIR/input.bin"
DIRECT_OUT="$TMP_DIR/direct_out.bin"
RELAY1_OUT="$TMP_DIR/relay_sink1.bin"
RELAY2_OUT="$TMP_DIR/relay_sink2.bin"

# 准备随机输入文件（32 * 1400 = 44800 bytes）。
dd if=/dev/urandom of="$INPUT_FILE" bs=1400 count=32 status=none

log "test1: direct tcp client -> server"
# 启动 TCP 服务端接收文件。
"$BUILD_DIR/omni_server" -p tcp -P "$DIRECT_PORT" -o "$DIRECT_OUT" >"$TMP_DIR/direct_server.log" 2>&1 &
DIRECT_SERVER_PID=$!
PIDS+=("$DIRECT_SERVER_PID")
sleep 1

# 启动客户端发送文件。
"$BUILD_DIR/omni_client" -p tcp -H 127.0.0.1 -P "$DIRECT_PORT" -f "$INPUT_FILE" -w 1 >"$TMP_DIR/direct_client.log" 2>&1
wait_with_timeout "$DIRECT_SERVER_PID" 10
# 校验接收文件与输入文件一致。
cmp -s "$INPUT_FILE" "$DIRECT_OUT"
log "test1 passed"

log "test2: udp relay forwarding with dynamic port switch"
# sink1：relay 初始目标（预期可能不再接收最终数据）。
"$BUILD_DIR/omni_server" -p udp -P "$SINK1_PORT" -o "$RELAY1_OUT" >"$TMP_DIR/relay_sink1.log" 2>&1 &
SINK1_PID=$!
PIDS+=("$SINK1_PID")

# sink2：relay 切换后的目标（最终校验对象）。
"$BUILD_DIR/omni_server" -p udp -P "$SINK2_PORT" -o "$RELAY2_OUT" >"$TMP_DIR/relay_sink2.log" 2>&1 &
SINK2_PID=$!
PIDS+=("$SINK2_PID")

# 预置 relay 控制命令：启动后立即切到 sink2。
CTRL_FILE="$TMP_DIR/relay_ctrl.txt"
printf 'set 127.0.0.1 %s\n' "$SINK2_PORT" >"$CTRL_FILE"

# 启动 relay（UDP 监听 RELAY_PORT）。
"$BUILD_DIR/omni_relay" -p udp -L "$RELAY_PORT" -H 127.0.0.1 -P "$SINK1_PORT" <"$CTRL_FILE" >"$TMP_DIR/relay.log" 2>&1 &
RELAY_PID=$!
PIDS+=("$RELAY_PID")
sleep 1

# 客户端发送到 relay，由 relay 中转到目标 sink。
"$BUILD_DIR/omni_client" -p udp -H 127.0.0.1 -P "$RELAY_PORT" -f "$INPUT_FILE" -w 1 >"$TMP_DIR/relay_client.log" 2>&1
wait_with_timeout "$SINK2_PID" 10
# 校验 relay 最终接收端文件一致。
cmp -s "$INPUT_FILE" "$RELAY2_OUT"

if [[ -s "$RELAY1_OUT" ]]; then
    # 如果 sink1 收到数据，通常是切换命令生效前的短暂窗口内到达。
    log "warning: sink1 received data before switch (relay reconfiguration happened mid-flight)"
fi

# relay/sink1 不一定会自然退出，这里主动结束避免脚本挂住。
kill "$RELAY_PID" 2>/dev/null || true
wait "$RELAY_PID" 2>/dev/null || true
kill "$SINK1_PID" 2>/dev/null || true

log "test2 passed"
log "all smoke tests passed"
