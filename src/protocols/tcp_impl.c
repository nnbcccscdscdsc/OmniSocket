/*
 * tcp_impl.c
 * TCP 协议实现，带 16 字节包头解决粘包
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
    int flag = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

static int tcp_set_reuseaddr(int fd)
{
    int flag = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
}

static int tcp_bind_and_listen(struct TcpContext *ctx,
                               const char *bind_ip,
                               uint16_t bind_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        logger_log("ERROR", "tcp", "socket_failed errno=%d", errno);
        return -1;
    }

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

    if (listen(fd, 1) < 0) {
        logger_log("ERROR", "tcp", "listen_failed errno=%d", errno);
        close(fd);
        return -1;
    }

    logger_log("INFO", "tcp", "listening port=%u", (unsigned)bind_port);

    /* 简化：阻塞接受一个客户端，之后用于长连接 */
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0) {
        logger_log("ERROR", "tcp", "accept_failed errno=%d", errno);
        close(fd);
        return -1;
    }

    close(fd);
    tcp_set_nodelay(cfd);

    ctx->fd = cfd;
    return 0;
}

static int tcp_connect_peer(struct TcpContext *ctx,
                            const char *peer_ip,
                            uint16_t peer_port)
{
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
    struct TcpContext *ctx = (struct TcpContext *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    int rc;
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

    uint64_t t0 = omni_now_ms();
    MsgHeader hdr;
    hdr.magic = htonl(MSG_MAGIC);
    hdr.length = htonl((uint32_t)len);
    hdr.seq = 0; /* 如有需要，上层可扩展维护序列号 */

    uint8_t header_buf[MSG_HEADER_SIZE];
    memcpy(header_buf, &hdr, MSG_HEADER_SIZE);

    ssize_t n1 = tcp_write_n(ctx->fd, header_buf, MSG_HEADER_SIZE);
    if (n1 != (ssize_t)MSG_HEADER_SIZE) {
        return OMNI_ERR_IO;
    }

    ssize_t n2 = tcp_write_n(ctx->fd, buf, len);
    if (n2 != (ssize_t)len) {
        return OMNI_ERR_IO;
    }

    uint64_t t1 = omni_now_ms();
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

    uint64_t t0 = omni_now_ms();
    uint8_t header_buf[MSG_HEADER_SIZE];
    ssize_t n1 = tcp_read_n(ctx->fd, header_buf, MSG_HEADER_SIZE);
    if (n1 <= 0) {
        return n1; /* 0 表示对端关闭，负数为错误 */
    }
    if (n1 != (ssize_t)MSG_HEADER_SIZE) {
        return OMNI_ERR_IO;
    }

    MsgHeader hdr;
    memcpy(&hdr, header_buf, MSG_HEADER_SIZE);
    if (ntohl(hdr.magic) != MSG_MAGIC) {
        logger_log("ERROR", "tcp", "invalid_magic");
        return OMNI_ERR_IO;
    }

    uint32_t payload_len = ntohl(hdr.length);
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
    logger_on_proto_recv_latency(t1 - t0);
    logger_log("DEBUG", "tcp", "recv payload_bytes=%u header_bytes=%zu proto_ms=%llu",
               payload_len, (size_t)MSG_HEADER_SIZE, (unsigned long long)(t1 - t0));
#ifdef __linux__
    tcp_log_info(ctx->fd, "after_recv");
#endif
    return (ssize_t)payload_len;
}

static void tcp_close(OmniContext *c)
{
    struct TcpContext *ctx = (struct TcpContext *)c;
    if (!ctx) return;
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

