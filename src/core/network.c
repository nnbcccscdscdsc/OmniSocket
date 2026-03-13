/*
 * network.c
 * 协议分发与工厂模式实现
 */

#include "network.h"
#include "common.h"
#include "logger.h"

#include <stdlib.h>

/* TCP / UDP / KCP 在各自的实现文件中提供以下符号 */
extern const struct ProtoVTable TCP_PROTO_VTABLE;
extern const struct ProtoVTable UDP_PROTO_VTABLE;
extern const struct ProtoVTable KCP_PROTO_VTABLE;

struct OmniContext {
    OmniProtocol proto;
    OmniRole role;
    const struct ProtoVTable *vt;
    void *impl; /* 由具体协议实现使用的内部指针 */
};

/* 工厂：根据协议类型选择对应实现 */
static const struct ProtoVTable *select_vtable(OmniProtocol proto)
{
    switch (proto) {
    case OMNI_PROTO_TCP:
        return &TCP_PROTO_VTABLE;
    case OMNI_PROTO_UDP:
        return &UDP_PROTO_VTABLE;
    case OMNI_PROTO_KCP:
        return &KCP_PROTO_VTABLE;
    default:
        return NULL;
    }
}

OmniContext *omni_init(OmniRole role,
                       OmniProtocol proto,
                       const char *bind_ip,
                       uint16_t bind_port,
                       const char *peer_ip,
                       uint16_t peer_port)
{
    logger_init();

    const struct ProtoVTable *vt = select_vtable(proto);
    if (!vt || !vt->init) {
        logger_log("ERROR", "network", "select_vtable_failed proto=%d",
                   (int)proto);
        return NULL;
    }

    OmniContext *ctx = (OmniContext *)calloc(1, sizeof(OmniContext));
    if (!ctx) {
        logger_log("ERROR", "network", "calloc_OmniContext_failed");
        return NULL;
    }

    ctx->proto = proto;
    ctx->role = role;
    ctx->vt = vt;

    OmniContext *impl_ctx = vt->init(role, bind_ip, bind_port, peer_ip, peer_port);
    if (!impl_ctx) {
        logger_log("ERROR", "network", "proto_init_failed proto=%d", (int)proto);
        free(ctx);
        return NULL;
    }

    /* 简化：直接让 impl 指针等于协议实现返回的上下文 */
    ctx->impl = impl_ctx;

    logger_log("INFO", "network",
               "omni_init_success proto=%d role=%d bind_port=%u peer_port=%u",
               (int)proto, (int)role,
               (unsigned)bind_port,
               (unsigned)peer_port);

    return ctx;
}

ssize_t omni_send(OmniContext *ctx, const void *buf, size_t len)
{
    if (!ctx || !ctx->vt || !ctx->vt->send) {
        return OMNI_ERR_PARAM;
    }

    uint64_t t0 = omni_now_ms();
    ssize_t n = ctx->vt->send((OmniContext *)ctx->impl, buf, len);
    uint64_t t1 = omni_now_ms();
    logger_on_send_call_latency(t1 - t0);
    if (n > 0) {
        logger_on_send((size_t)n);
    }
    logger_log("DEBUG", "network", "omni_send proto=%d bytes=%zd call_ms=%llu",
               (int)ctx->proto, n, (unsigned long long)(t1 - t0));
    return n;
}

ssize_t omni_recv(OmniContext *ctx, void *buf, size_t len)
{
    if (!ctx || !ctx->vt || !ctx->vt->recv) {
        return OMNI_ERR_PARAM;
    }

    uint64_t t0 = omni_now_ms();
    ssize_t n = ctx->vt->recv((OmniContext *)ctx->impl, buf, len);
    uint64_t t1 = omni_now_ms();
    logger_on_recv_call_latency(t1 - t0);
    if (n > 0) {
        logger_on_recv((size_t)n);
    }
    logger_log("DEBUG", "network", "omni_recv proto=%d bytes=%zd call_ms=%llu",
               (int)ctx->proto, n, (unsigned long long)(t1 - t0));
    return n;
}

void omni_close(OmniContext *ctx)
{
    if (!ctx) return;
    if (ctx->vt && ctx->vt->close && ctx->impl) {
        ctx->vt->close((OmniContext *)ctx->impl);
    }
    logger_print_performance_log("final");
    free(ctx);
}

