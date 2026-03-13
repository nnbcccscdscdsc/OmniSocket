/*
 * test_main.c
 * 简单测试程序：客户端/服务端双向传输 + 日志观测
 *
 * 使用方式示例（同一机器上，两个终端）：
 *   终端1：./omni_test -r server -p tcp -P 9000
 *   终端2：./omni_test -r client -p tcp -P 9000 -H 127.0.0.1
 *
 * 协议可选：tcp / udp / kcp
 */

#include "common.h"
#include "network.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s -r server -p tcp|udp|kcp -P <port>\n"
            "  %s -r client -p tcp|udp|kcp -P <port> -H <host>\n",
            prog, prog);
}

static OmniProtocol parse_proto(const char *s)
{
    if (strcmp(s, "tcp") == 0) return OMNI_PROTO_TCP;
    if (strcmp(s, "udp") == 0) return OMNI_PROTO_UDP;
    if (strcmp(s, "kcp") == 0) return OMNI_PROTO_KCP;
    return OMNI_PROTO_TCP;
}

static void run_server(OmniProtocol proto, uint16_t port)
{
    OmniContext *ctx = omni_init(OMNI_ROLE_SERVER, proto,
                                 NULL, port,
                                 NULL, 0);
    if (!ctx) {
        fprintf(stderr, "server: omni_init failed\n");
        return;
    }

    logger_log("INFO", "test", "server_started proto=%d port=%u",
               (int)proto, (unsigned)port);

    char buf[4096];
    for (;;) {
        ssize_t n = omni_recv(ctx, buf, sizeof(buf));
        if (n <= 0) {
            logger_log("INFO", "test", "server_recv_end n=%zd", n);
            break;
        }
        logger_log("INFO", "test", "server_recv bytes=%zd", n);

        /* 简单 echo 回客户端，验证双向通信 */
        ssize_t m = omni_send(ctx, buf, (size_t)n);
        logger_log("INFO", "test", "server_echo bytes=%zd", m);
    }

    omni_close(ctx);
}

static void run_client(OmniProtocol proto, const char *host, uint16_t port)
{
    if (!host) {
        fprintf(stderr, "client: host is required\n");
        return;
    }

    OmniContext *ctx = omni_init(OMNI_ROLE_CLIENT, proto,
                                 NULL, 0,
                                 host, port);
    if (!ctx) {
        fprintf(stderr, "client: omni_init failed\n");
        return;
    }

    logger_log("INFO", "test", "client_started proto=%d host=%s port=%u",
               (int)proto, host, (unsigned)port);

    char send_buf[2048];
    char recv_buf[4096];

    for (int i = 0; i < 100; ++i) {
        int len = snprintf(send_buf, sizeof(send_buf),
                           "msg=%d time_ms=%llu payload_size=%zu",
                           i,
                           (unsigned long long)omni_now_ms(),
                           sizeof(send_buf));

        ssize_t n = omni_send(ctx, send_buf, (size_t)len);
        logger_log("INFO", "test", "client_send i=%d bytes=%zd", i, n);
        if (n <= 0) break;

        ssize_t m = omni_recv(ctx, recv_buf, sizeof(recv_buf));
        if (m <= 0) {
            logger_log("INFO", "test", "client_recv_end i=%d bytes=%zd", i, m);
            break;
        }
        logger_log("INFO", "test",
                   "client_recv_echo i=%d bytes=%zd first_bytes=\"%.32s\"",
                   i, m, recv_buf);

        usleep(10 * 1000); /* 10ms 间隔，模拟稳定流量 */
    }

    omni_close(ctx);
}

int main(int argc, char **argv)
{
    const char *role_str = NULL;
    const char *proto_str = "tcp";
    const char *host = NULL;
    int port = 0;

    int opt;
    while ((opt = getopt(argc, argv, "r:p:P:H:")) != -1) {
        switch (opt) {
        case 'r':
            role_str = optarg;
            break;
        case 'p':
            proto_str = optarg;
            break;
        case 'P':
            port = atoi(optarg);
            break;
        case 'H':
            host = optarg;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (!role_str || port <= 0) {
        usage(argv[0]);
        return 1;
    }

    OmniProtocol proto = parse_proto(proto_str);

    if (strcmp(role_str, "server") == 0) {
        run_server(proto, (uint16_t)port);
    } else if (strcmp(role_str, "client") == 0) {
        run_client(proto, host, (uint16_t)port);
    } else {
        usage(argv[0]);
        return 1;
    }

    return 0;
}

