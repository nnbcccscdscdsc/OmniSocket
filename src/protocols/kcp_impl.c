/*
 * kcp_impl.c
 * 基于 UDP 的 KCP 可靠传输实现
 */

#include "common.h"
#include "network.h"
#include "logger.h"
#include "kcp/ikcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct KcpContext {
    int fd;
    struct sockaddr_in peer_addr;
    socklen_t peer_len;
    ikcpcb *kcp;
    uint32_t last_xmit; /* 用于推算重传/发送次数变化 */
};

static int kcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
    (void)kcp;
    struct KcpContext *ctx = (struct KcpContext *)user;
    ssize_t n = sendto(ctx->fd, buf, (size_t)len, 0,
                       (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
    if (n < 0) {
        logger_log("ERROR", "kcp", "sendto_failed errno=%d", errno);
        return OMNI_ERR_IO;
    }
    return 0;
}

static OmniContext *kcp_init(OmniRole role,
                             const char *bind_ip,
                             uint16_t bind_port,
                             const char *peer_ip,
                             uint16_t peer_port)
{
    struct KcpContext *ctx = (struct KcpContext *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        logger_log("ERROR", "kcp", "socket_failed errno=%d", errno);
        free(ctx);
        return NULL;
    }

    if (bind_port != 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(bind_port);
        addr.sin_addr.s_addr = bind_ip ? inet_addr(bind_ip) : INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            logger_log("ERROR", "kcp", "bind_failed errno=%d", errno);
            close(fd);
            free(ctx);
            return NULL;
        }
    }

    memset(&ctx->peer_addr, 0, sizeof(ctx->peer_addr));
    ctx->peer_addr.sin_family = AF_INET;
    ctx->peer_addr.sin_port = htons(peer_port);
    ctx->peer_addr.sin_addr.s_addr = peer_ip ? inet_addr(peer_ip) : INADDR_ANY;
    ctx->peer_len = sizeof(ctx->peer_addr);

    ctx->fd = fd;

    /* conv 必须两端一致：server 用 bind_port，client 用 peer_port */
    IUINT32 conv = (role == OMNI_ROLE_SERVER) ? (IUINT32)bind_port
                                              : (IUINT32)peer_port;
    ikcpcb *kcp = ikcp_create(conv, ctx);
    if (!kcp) {
        logger_log("ERROR", "kcp", "ikcp_create_failed");
        close(fd);
        free(ctx);
        return NULL;
    }

    ctx->kcp = kcp;

    ikcp_setoutput(kcp, kcp_output);
    ikcp_nodelay(kcp, 1, 10, 2, 1);
    ikcp_wndsize(kcp, 128, 128);

    logger_log("INFO", "kcp",
               "init bind_port=%u peer_ip=%s peer_port=%u conv=%u",
               (unsigned)bind_port,
               peer_ip ? peer_ip : "NULL",
               (unsigned)peer_port,
               (unsigned)conv);

    return (OmniContext *)ctx;
}

static void kcp_update_loop(struct KcpContext *ctx)
{
    uint64_t t0 = omni_now_ms();
    IUINT32 current = (IUINT32)omni_now_ms();
    ikcp_update(ctx->kcp, current);

    char buf[1500];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(ctx->fd, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&from, &fromlen);
    if (n > 0) {
        ctx->peer_addr = from;
        ctx->peer_len = fromlen;
        ikcp_input(ctx->kcp, buf, (long)n);
    }

    /* KCP 内部状态监控 */
    uint32_t xmit = ctx->kcp->xmit;
    if (xmit >= ctx->last_xmit) {
        logger_on_kcp_retrans((uint64_t)(xmit - ctx->last_xmit));
    }
    ctx->last_xmit = xmit;

    uint64_t t1 = omni_now_ms();
    logger_log("DEBUG", "kcp",
               "update ms=%llu cwnd=%u ssthresh=%u rmt_wnd=%u snd_wnd=%u rcv_wnd=%u "
               "rx_srtt=%u rx_rto=%u nsnd_buf=%u nsnd_que=%u nrcv_buf=%u nrcv_que=%u xmit=%u state=%u",
               (unsigned long long)(t1 - t0),
               (unsigned)ctx->kcp->cwnd,
               (unsigned)ctx->kcp->ssthresh,
               (unsigned)ctx->kcp->rmt_wnd,
               (unsigned)ctx->kcp->snd_wnd,
               (unsigned)ctx->kcp->rcv_wnd,
               (unsigned)ctx->kcp->rx_srtt,
               (unsigned)ctx->kcp->rx_rto,
               (unsigned)ctx->kcp->nsnd_buf,
               (unsigned)ctx->kcp->nsnd_que,
               (unsigned)ctx->kcp->nrcv_buf,
               (unsigned)ctx->kcp->nrcv_que,
               (unsigned)ctx->kcp->xmit,
               (unsigned)ctx->kcp->state);
}

static ssize_t kcp_send(OmniContext *c, const void *buf, size_t len)
{
    struct KcpContext *ctx = (struct KcpContext *)c;
    if (!ctx || !ctx->kcp) return OMNI_ERR_PARAM;

    uint64_t t0 = omni_now_ms();
    int rc = ikcp_send(ctx->kcp, (const char *)buf, (int)len);
    if (rc < 0) {
        logger_log("ERROR", "kcp", "ikcp_send_failed rc=%d", rc);
        return OMNI_ERR_IO;
    }

    /* 驱动一次 flush */
    kcp_update_loop(ctx);
    uint64_t t1 = omni_now_ms();
    logger_on_proto_send_latency(t1 - t0);
    logger_log("DEBUG", "kcp", "send payload_bytes=%zu proto_ms=%llu waitsnd=%d",
               len, (unsigned long long)(t1 - t0), ikcp_waitsnd(ctx->kcp));
    return (ssize_t)len;
}

static ssize_t kcp_recv(OmniContext *c, void *buf, size_t len)
{
    struct KcpContext *ctx = (struct KcpContext *)c;
    if (!ctx || !ctx->kcp) return OMNI_ERR_PARAM;

    uint64_t t0 = omni_now_ms();
    kcp_update_loop(ctx);

    int n = ikcp_recv(ctx->kcp, (char *)buf, (int)len);
    if (n < 0) {
        return 0; /* 暂无数据 */
    }
    uint64_t t1 = omni_now_ms();
    logger_on_proto_recv_latency(t1 - t0);
    logger_log("DEBUG", "kcp", "recv payload_bytes=%d proto_ms=%llu",
               n, (unsigned long long)(t1 - t0));
    return (ssize_t)n;
}

static void kcp_close(OmniContext *c)
{
    struct KcpContext *ctx = (struct KcpContext *)c;
    if (!ctx) return;
    if (ctx->kcp) {
        ikcp_release(ctx->kcp);
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    free(ctx);
}

const struct ProtoVTable KCP_PROTO_VTABLE = {
    .init = kcp_init,
    .send = kcp_send,
    .recv = kcp_recv,
    .close = kcp_close,
};

