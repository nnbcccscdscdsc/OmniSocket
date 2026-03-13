/*
 * udp_impl.c
 * UDP 协议实现（无连接，基础 sendto/recvfrom）
 */

#include "common.h"
#include "network.h"
#include "logger.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct UdpContext {
    int fd;
    struct sockaddr_in peer_addr;
    socklen_t peer_len;
};

static OmniContext *udp_init(OmniRole role,
                             const char *bind_ip,
                             uint16_t bind_port,
                             const char *peer_ip,
                             uint16_t peer_port)
{
    (void)role;

    struct UdpContext *ctx = (struct UdpContext *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        logger_log("ERROR", "udp", "socket_failed errno=%d", errno);
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
            logger_log("ERROR", "udp", "bind_failed errno=%d", errno);
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

    logger_log("INFO", "udp",
               "init bind_port=%u peer_ip=%s peer_port=%u",
               (unsigned)bind_port,
               peer_ip ? peer_ip : "NULL",
               (unsigned)peer_port);

    return (OmniContext *)ctx;
}

static ssize_t udp_send(OmniContext *c, const void *buf, size_t len)
{
    struct UdpContext *ctx = (struct UdpContext *)c;
    if (!ctx || ctx->fd < 0) return OMNI_ERR_PARAM;

    ssize_t n = sendto(ctx->fd, buf, len, 0,
                       (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
    if (n < 0) {
        logger_log("ERROR", "udp", "sendto_failed errno=%d", errno);
        return OMNI_ERR_IO;
    }
    return n;
}

static ssize_t udp_recv(OmniContext *c, void *buf, size_t len)
{
    struct UdpContext *ctx = (struct UdpContext *)c;
    if (!ctx || ctx->fd < 0) return OMNI_ERR_PARAM;

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(ctx->fd, buf, len, 0,
                         (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        if (errno == EINTR) return 0;
        logger_log("ERROR", "udp", "recvfrom_failed errno=%d", errno);
        return OMNI_ERR_IO;
    }

    /* 默认更新 peer 为最近一次通信对端，便于“伪长连接” */
    ctx->peer_addr = from;
    ctx->peer_len = fromlen;

    return n;
}

static void udp_close(OmniContext *c)
{
    struct UdpContext *ctx = (struct UdpContext *)c;
    if (!ctx) return;
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    free(ctx);
}

const struct ProtoVTable UDP_PROTO_VTABLE = {
    .init = udp_init,
    .send = udp_send,
    .recv = udp_recv,
    .close = udp_close,
};

