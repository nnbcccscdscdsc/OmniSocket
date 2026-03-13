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
    (void)role;

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

    /* conv 可简单使用端口号 */
    IUINT32 conv = (IUINT32)peer_port;
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
               "init bind_port=%u peer_ip=%s peer_port=%u",
               (unsigned)bind_port,
               peer_ip ? peer_ip : "NULL",
               (unsigned)peer_port);

    return (OmniContext *)ctx;
}

static void kcp_update_loop(struct KcpContext *ctx)
{
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
}

static ssize_t kcp_send(OmniContext *c, const void *buf, size_t len)
{
    struct KcpContext *ctx = (struct KcpContext *)c;
    if (!ctx || !ctx->kcp) return OMNI_ERR_PARAM;

    int rc = ikcp_send(ctx->kcp, (const char *)buf, (int)len);
    if (rc < 0) {
        logger_log("ERROR", "kcp", "ikcp_send_failed rc=%d", rc);
        return OMNI_ERR_IO;
    }

    /* 驱动一次 flush */
    kcp_update_loop(ctx);
    return (ssize_t)len;
}

static ssize_t kcp_recv(OmniContext *c, void *buf, size_t len)
{
    struct KcpContext *ctx = (struct KcpContext *)c;
    if (!ctx || !ctx->kcp) return OMNI_ERR_PARAM;

    kcp_update_loop(ctx);

    int n = ikcp_recv(ctx->kcp, (char *)buf, (int)len);
    if (n < 0) {
        return 0; /* 暂无数据 */
    }
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

