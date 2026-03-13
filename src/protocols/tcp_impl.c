/*
 * tcp_impl.c
 * TCP 协议实现，带 16 字节包头解决粘包
 *
 * 设计说明：
 * 1) TCP 是字节流，天然没有消息边界，因此这里通过“固定 16 字节头 + payload 长度”
 *    显式划分消息边界，避免粘包/拆包带来的上层读取混乱。
 * 2) 本层的头只用于“流边界管理”，上层业务仍可在 payload 中定义自己的消息头。
 * 3) send/recv 均采用阻塞全量读写语义：要么完整收发一帧，要么返回错误/关闭状态。
 */

#include "common.h"
#include "network.h"
#include "logger.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Linux 下 TCP_INFO 定义通常已在 <netinet/tcp.h> 提供，避免引入 <linux/tcp.h> 重定义 */

struct TcpContext {
    /* 已建立连接的 socket fd（服务端 accept 后或客户端 connect 后）。 */
    int fd;
};

#ifdef __linux__
static void tcp_log_info(int fd, const char *tag)
{
    struct tcp_info ti;
    socklen_t len = sizeof(ti);
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &len) != 0) {
        return;
    }

    /* 注意：tcpi_rtt 单位通常为微秒（Linux），这里转 ms 仅用于日志观察 */
    unsigned long long rtt_ms = (unsigned long long)(ti.tcpi_rtt / 1000u);
    unsigned long long rttvar_ms = (unsigned long long)(ti.tcpi_rttvar / 1000u);

    logger_log("INFO", "tcpinfo",
               "tag=%s state=%u retransmits=%u probes=%u backoff=%u "
               "rto=%u ato=%u rtt_ms=%llu rttvar_ms=%llu "
               "snd_cwnd=%u snd_ssthresh=%u snd_mss=%u rcv_mss=%u "
               "lost=%u retrans=%u fackets=%u "
               "last_data_sent_ms=%u last_data_recv_ms=%u",
               tag ? tag : "sample",
               (unsigned)ti.tcpi_state,
               (unsigned)ti.tcpi_retransmits,
               (unsigned)ti.tcpi_probes,
               (unsigned)ti.tcpi_backoff,
               (unsigned)ti.tcpi_rto,
               (unsigned)ti.tcpi_ato,
               rtt_ms,
               rttvar_ms,
               (unsigned)ti.tcpi_snd_cwnd,
               (unsigned)ti.tcpi_snd_ssthresh,
               (unsigned)ti.tcpi_snd_mss,
               (unsigned)ti.tcpi_rcv_mss,
               (unsigned)ti.tcpi_lost,
               (unsigned)ti.tcpi_retrans,
               (unsigned)ti.tcpi_fackets,
               (unsigned)ti.tcpi_last_data_sent,
               (unsigned)ti.tcpi_last_data_recv);
}
#endif

static int tcp_set_nodelay(int fd)
{
    /* 关闭 Nagle，降低小包时延（更利于交互指令场景）。 */
    int flag = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

static int tcp_set_reuseaddr(int fd)
{
    /* 允许端口快速复用，减少开发/测试时 TIME_WAIT 影响。 */
    int flag = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
}

static int tcp_bind_and_listen(struct TcpContext *ctx,
                               const char *bind_ip,
                               uint16_t bind_port)
{
    /* 创建监听 socket。 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        logger_log("ERROR", "tcp", "socket_failed errno=%d", errno);
        return -1;
    }

    /* 监听 socket 打开地址复用。 */
    tcp_set_reuseaddr(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bind_port);
    addr.sin_addr.s_addr = bind_ip ? inet_addr(bind_ip) : INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        logger_log("ERROR", "tcp", "bind_failed errno=%d", errno);
        close(fd);
        return -1;
    }

    /*
     * 这里 backlog 取 1，符合当前“单连接演示/测试”场景。
     * 若后续要支持多客户端，可提升 backlog 并改为事件循环/线程池模型。
     */
    if (listen(fd, 1) < 0) {
        logger_log("ERROR", "tcp", "listen_failed errno=%d", errno);
        close(fd);
        return -1;
    }

    logger_log("INFO", "tcp", "listening port=%u", (unsigned)bind_port);

    /* 简化：阻塞接受一个客户端，连接建立后作为长连接使用。 */
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0) {
        logger_log("ERROR", "tcp", "accept_failed errno=%d", errno);
        close(fd);
        return -1;
    }

    /* 监听 fd 仅用于 accept，一旦接入成功即可关闭监听 fd。 */
    close(fd);
    tcp_set_nodelay(cfd);

    ctx->fd = cfd;
    return 0;
}

static int tcp_connect_peer(struct TcpContext *ctx,
                            const char *peer_ip,
                            uint16_t peer_port)
{
    /* 创建主动连接 socket。 */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        logger_log("ERROR", "tcp", "socket_failed errno=%d", errno);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer_port);
    addr.sin_addr.s_addr = inet_addr(peer_ip);

    /* 阻塞 connect 到对端。 */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        logger_log("ERROR", "tcp", "connect_failed errno=%d", errno);
        close(fd);
        return -1;
    }

    tcp_set_nodelay(fd);
    ctx->fd = fd;
    logger_log("INFO", "tcp", "connected peer_ip=%s peer_port=%u",
               peer_ip, (unsigned)peer_port);
    return 0;
}

static ssize_t tcp_read_n(int fd, void *buf, size_t n)
{
    /*
     * 从 TCP 流中“恰好读取 n 字节”：
     * - 正常返回 n
     * - 返回 0 表示对端关闭（如果发生在中途，返回已读字节数）
     * - 返回 -1 表示系统调用错误
     */
    size_t off = 0;
    char *p = (char *)buf;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            return off; /* 对端关闭 */
        }
        off += (size_t)r;
    }
    return (ssize_t)off;
}

static ssize_t tcp_write_n(int fd, const void *buf, size_t n)
{
    /*
     * 向 TCP 流中“恰好写入 n 字节”：
     * - EINTR 自动重试
     * - 其余错误返回 -1
     */
    size_t off = 0;
    const char *p = (const char *)buf;
    while (off < n) {
        ssize_t r = write(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return (ssize_t)off;
}

static OmniContext *tcp_init(OmniRole role,
                             const char *bind_ip,
                             uint16_t bind_port,
                             const char *peer_ip,
                             uint16_t peer_port)
{
    /* 协议私有上下文（通过 OmniContext* 向上层做不透明传递）。 */
    struct TcpContext *ctx = (struct TcpContext *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    int rc;
    /* 按角色决定是被动监听还是主动连接。 */
    if (role == OMNI_ROLE_SERVER) {
        rc = tcp_bind_and_listen(ctx, bind_ip, bind_port);
    } else {
        if (!peer_ip || peer_port == 0) {
            logger_log("ERROR", "tcp", "client_requires_peer_ip_port");
            free(ctx);
            return NULL;
        }
        rc = tcp_connect_peer(ctx, peer_ip, peer_port);
    }

    if (rc != 0) {
        free(ctx);
        return NULL;
    }

    return (OmniContext *)ctx;
}

static ssize_t tcp_send(OmniContext *c, const void *buf, size_t len)
{
    struct TcpContext *ctx = (struct TcpContext *)c;
    if (!ctx || ctx->fd < 0) return OMNI_ERR_PARAM;

    /*
     * 外层 TCP 帧头（16B）仅用于切分消息边界。
     * 当前 type 统一标记为 MSG_TYPE_RAW，表示“payload 是上层透传内容”。
     */
    uint64_t t0 = omni_now_ms();
    MsgHeader hdr;
    omni_msg_header_encode(&hdr, MSG_TYPE_RAW, (uint32_t)len, t0);

    uint8_t header_buf[MSG_HEADER_SIZE];
    memcpy(header_buf, &hdr, MSG_HEADER_SIZE);

    /* 先写固定头，再写 payload，接收侧可据此恢复完整帧。 */
    ssize_t n1 = tcp_write_n(ctx->fd, header_buf, MSG_HEADER_SIZE);
    if (n1 != (ssize_t)MSG_HEADER_SIZE) {
        return OMNI_ERR_IO;
    }

    ssize_t n2 = tcp_write_n(ctx->fd, buf, len);
    if (n2 != (ssize_t)len) {
        return OMNI_ERR_IO;
    }

    uint64_t t1 = omni_now_ms();
    /* 记录协议层发送耗时，便于后续性能分析。 */
    logger_on_proto_send_latency(t1 - t0);
    logger_log("DEBUG", "tcp", "send payload_bytes=%zu header_bytes=%zu proto_ms=%llu",
               len, (size_t)MSG_HEADER_SIZE, (unsigned long long)(t1 - t0));
#ifdef __linux__
    tcp_log_info(ctx->fd, "after_send");
#endif
    return (ssize_t)len;
}

static ssize_t tcp_recv(OmniContext *c, void *buf, size_t len)
{
    struct TcpContext *ctx = (struct TcpContext *)c;
    if (!ctx || ctx->fd < 0) return OMNI_ERR_PARAM;

    /*
     * 收包流程：
     * 1) 固定先读 16 字节头
     * 2) 解析 payload_len
     * 3) 再读 payload_len 字节
     */
    uint64_t t0 = omni_now_ms();
    uint8_t header_buf[MSG_HEADER_SIZE];
    ssize_t n1 = tcp_read_n(ctx->fd, header_buf, MSG_HEADER_SIZE);
    if (n1 <= 0) {
        return n1; /* 0 表示对端关闭，负数为错误 */
    }
    if (n1 != (ssize_t)MSG_HEADER_SIZE) {
        return OMNI_ERR_IO;
    }

    /* 解码网络字节序头字段。 */
    MsgHeader hdr;
    MsgHeader host_hdr;
    memcpy(&hdr, header_buf, MSG_HEADER_SIZE);
    omni_msg_header_decode(&hdr, &host_hdr);

    uint32_t payload_len = host_hdr.len;
    /*
     * 调用方缓冲区不足时直接报错。
     * 当前实现不做“读取并丢弃剩余字节”，因此调用方应保证 recv 缓冲足够大。
     */
    if (payload_len > len) {
        logger_log("ERROR", "tcp", "buffer_too_small payload=%u buf_len=%zu",
                   payload_len, len);
        /* 简化：这里返回错误，实际可考虑丢弃或扩展缓冲区 */
        return OMNI_ERR_PARAM;
    }

    ssize_t n2 = tcp_read_n(ctx->fd, buf, payload_len);
    if (n2 != (ssize_t)payload_len) {
        return OMNI_ERR_IO;
    }

    uint64_t t1 = omni_now_ms();
    /* 记录协议层接收耗时。 */
    logger_on_proto_recv_latency(t1 - t0);
    logger_log("DEBUG", "tcp",
               "recv payload_bytes=%u header_bytes=%zu msg_type=%u ts_ms=%llu proto_ms=%llu",
               payload_len, (size_t)MSG_HEADER_SIZE,
               (unsigned)host_hdr.type,
               (unsigned long long)host_hdr.timestamp,
               (unsigned long long)(t1 - t0));
#ifdef __linux__
    tcp_log_info(ctx->fd, "after_recv");
#endif
    return (ssize_t)payload_len;
}

static void tcp_close(OmniContext *c)
{
    struct TcpContext *ctx = (struct TcpContext *)c;
    if (!ctx) return;
    /* 关闭连接并释放私有上下文。 */
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    free(ctx);
}

const struct ProtoVTable TCP_PROTO_VTABLE = {
    .init = tcp_init,
    .send = tcp_send,
    .recv = tcp_recv,
    .close = tcp_close,
};

