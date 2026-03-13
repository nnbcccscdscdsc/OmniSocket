/*
 * relay_main.c
 * 中转站：从 A 接收数据后立即转发到 B，支持运行时动态修改转发目标
 *
 * 并发模型：
 * - 主线程：阻塞接收上游流量并转发到当前目标
 * - 控制线程：读取 stdin 命令，动态切换目标地址
 *
 * 线程安全策略：
 * - tx_ctx / target_ip / target_port 受 tx_mu 互斥锁保护
 * - 主线程转发发送与控制线程切换目标不会并发踩内存
 *
 * 控制命令（stdin）：
 *   set <ip> <port>   修改目标地址
 *   show              打印当前目标
 *   quit              退出
 */

#include "common.h"
#include "network.h"
#include "logger.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RELAY_BUF_SIZE (MSG_HEADER_SIZE + 65536u)

typedef struct RelayState {
    /* 当前 relay 工作协议。 */
    OmniProtocol proto;
    /* 上游接收上下文（通常是服务端角色）。 */
    OmniContext *rx_ctx;
    /* 下游发送上下文（通常是客户端角色，可动态替换）。 */
    OmniContext *tx_ctx;
    /* 保护 tx_ctx 与目标地址信息。 */
    pthread_mutex_t tx_mu;
    /* 运行标志。 */
    atomic_int running;
    /* 当前目标地址快照（用于 show 命令与日志）。 */
    char target_ip[64];
    uint16_t target_port;
} RelayState;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s -p tcp|udp|kcp -L <listen_port> -H <target_ip> -P <target_port>\n",
            prog);
}

static OmniProtocol parse_proto(const char *s)
{
    /* 非法输入回退 TCP。 */
    if (!s) return OMNI_PROTO_TCP;
    if (strcmp(s, "tcp") == 0) return OMNI_PROTO_TCP;
    if (strcmp(s, "udp") == 0) return OMNI_PROTO_UDP;
    if (strcmp(s, "kcp") == 0) return OMNI_PROTO_KCP;
    return OMNI_PROTO_TCP;
}

static int relay_set_target(RelayState *st, const char *ip, uint16_t port)
{
    /*
     * 动态切换目标步骤：
     * 1) 先建立新 tx_ctx（失败时保持旧目标不变）
     * 2) 加锁替换指针与目标参数
     * 3) 解锁后关闭旧 tx_ctx（避免持锁做慢操作）
     */
    OmniContext *new_tx = omni_init(OMNI_ROLE_CLIENT, st->proto,
                                    NULL, 0,
                                    ip, port);
    if (!new_tx) {
        logger_log("ERROR", "relay", "connect_target_failed ip=%s port=%u",
                   ip, (unsigned)port);
        return OMNI_ERR_IO;
    }

    pthread_mutex_lock(&st->tx_mu);
    OmniContext *old_tx = st->tx_ctx;
    st->tx_ctx = new_tx;
    snprintf(st->target_ip, sizeof(st->target_ip), "%s", ip);
    st->target_port = port;
    pthread_mutex_unlock(&st->tx_mu);

    if (old_tx) {
        omni_close(old_tx);
    }

    logger_log("INFO", "relay", "target_updated ip=%s port=%u", ip, (unsigned)port);
    return OMNI_OK;
}

static void *control_thread_main(void *arg)
{
    /* 控制线程负责解析 stdin 命令。 */
    RelayState *st = (RelayState *)arg;
    char line[256];

    while (atomic_load(&st->running)) {
        if (!fgets(line, sizeof(line), stdin)) {
            /*
             * 管道/重定向 EOF 时不要立刻退出 relay：
             * - 清理 EOF 状态
             * - 短暂休眠后继续循环
             * 这样 relay 仍可继续处理主数据面转发。
             */
            clearerr(stdin);
            usleep(100 * 1000);
            continue;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        if (strcmp(line, "quit") == 0) {
            /* 通知主线程退出。 */
            atomic_store(&st->running, 0);
            break;
        }

        if (strcmp(line, "show") == 0) {
            /* 在锁保护下读取目标快照，避免与 set 并发冲突。 */
            pthread_mutex_lock(&st->tx_mu);
            fprintf(stderr, "relay target: %s:%u\n",
                    st->target_ip[0] ? st->target_ip : "N/A",
                    (unsigned)st->target_port);
            pthread_mutex_unlock(&st->tx_mu);
            continue;
        }

        char ip[64];
        unsigned port = 0;
        if (sscanf(line, "set %63s %u", ip, &port) == 2 && port > 0 && port <= 65535u) {
            /* 动态切目标。 */
            relay_set_target(st, ip, (uint16_t)port);
            continue;
        }

        fprintf(stderr, "unknown command: %s\n", line);
        fprintf(stderr, "commands: set <ip> <port> | show | quit\n");
    }

    return NULL;
}

int main(int argc, char **argv)
{
    /* 命令行参数默认值。 */
    const char *proto_str = "tcp";
    const char *target_ip = NULL;
    int listen_port = 0;
    int target_port = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:L:H:P:")) != -1) {
        switch (opt) {
        case 'p':
            proto_str = optarg;
            break;
        case 'L':
            listen_port = atoi(optarg);
            break;
        case 'H':
            target_ip = optarg;
            break;
        case 'P':
            target_port = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (!target_ip || listen_port <= 0 || target_port <= 0) {
        usage(argv[0]);
        return 1;
    }

    RelayState st;
    memset(&st, 0, sizeof(st));
    st.proto = parse_proto(proto_str);
    atomic_init(&st.running, 1);
    pthread_mutex_init(&st.tx_mu, NULL);

    /*
     * rx_ctx 作为上游入口（server 角色）：
     * - TCP: 等待上游 connect
     * - UDP/KCP: 绑定监听端口接收上游包
     */
    st.rx_ctx = omni_init(OMNI_ROLE_SERVER, st.proto, NULL, (uint16_t)listen_port, NULL, 0);
    if (!st.rx_ctx) {
        fprintf(stderr, "relay: omni_init rx failed\n");
        pthread_mutex_destroy(&st.tx_mu);
        return 1;
    }

    /*
     * 初始目标连接失败不直接退出：
     * - relay 可先启动数据入口
     * - 后续通过 set 命令修复目标地址
     */
    (void)relay_set_target(&st, target_ip, (uint16_t)target_port);

    /* 启动控制面线程。 */
    pthread_t ctrl_tid;
    if (pthread_create(&ctrl_tid, NULL, control_thread_main, &st) != 0) {
        perror("pthread_create");
        omni_close(st.rx_ctx);
        if (st.tx_ctx) omni_close(st.tx_ctx);
        pthread_mutex_destroy(&st.tx_mu);
        return 1;
    }

    uint8_t buf[RELAY_BUF_SIZE];
    while (atomic_load(&st.running)) {
        /* 数据面：收上游 -> 转发下游。 */
        ssize_t n = omni_recv(st.rx_ctx, buf, sizeof(buf));
        if (n < 0) {
            logger_log("ERROR", "relay", "recv_failed n=%zd", n);
            break;
        }
        if (n == 0) {
            /* 暂时无数据时短暂退避。 */
            usleep(2 * 1000);
            continue;
        }

        ssize_t m = OMNI_ERR_PARAM;
        /* 发送上下文受锁保护，防止与 set 命令并发替换。 */
        pthread_mutex_lock(&st.tx_mu);
        if (st.tx_ctx) {
            m = omni_send(st.tx_ctx, buf, (size_t)n);
        }
        pthread_mutex_unlock(&st.tx_mu);

        if (m != n) {
            logger_log("ERROR", "relay", "forward_failed in=%zd out=%zd", n, m);
        } else {
            logger_log("INFO", "relay", "forward_ok bytes=%zd", n);
        }
    }

    /* 收尾：先停主循环，再依次释放 rx / 控制线程 / tx。 */
    atomic_store(&st.running, 0);
    omni_close(st.rx_ctx);
    pthread_join(ctrl_tid, NULL);

    pthread_mutex_lock(&st.tx_mu);
    OmniContext *tx = st.tx_ctx;
    st.tx_ctx = NULL;
    pthread_mutex_unlock(&st.tx_mu);
    if (tx) {
        omni_close(tx);
    }

    pthread_mutex_destroy(&st.tx_mu);
    return 0;
}
