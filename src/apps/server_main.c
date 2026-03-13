/*
 * server_main.c
 * 服务端：接收文件写盘；主线程监听键盘输入并发送 ASCII 指令到客户端
 *
 * 线程模型：
 * - 接收线程：持续收业务帧，写入文件，直到 FILE_END
 * - 主线程：在交互终端下读取 stdin，发送 COMMAND 给客户端
 *
 * 说明：
 * - 当 stdin 不是 TTY（例如被脚本后台拉起）时，主线程不做交互输入，
 *   仅等待接收线程完成传输，便于自动化测试稳定运行。
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

#define SERVER_FRAME_BUF_SIZE (MSG_HEADER_SIZE + 65536u)

typedef struct ServerRuntime {
    /* 协议抽象层句柄。 */
    OmniContext *ctx;
    /* 当前运行协议，用于区分 recv 返回 0 的语义（TCP=对端关闭）。 */
    OmniProtocol proto;
    /* 接收文件写入目标。 */
    FILE *out_fp;
    /* 全局运行标记。 */
    atomic_int running;
    /* 收到 FILE_END 后置 1。 */
    atomic_int transfer_done;
    /* 已成功写入的文件字节数。 */
    uint64_t bytes_written;
} ServerRuntime;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s -p tcp|udp|kcp -P <listen_port> -o <output_file> [-b <bind_ip>]\n",
            prog);
}

static OmniProtocol parse_proto(const char *s)
{
    /* 输入不合法时回退到 TCP。 */
    if (!s) return OMNI_PROTO_TCP;
    if (strcmp(s, "tcp") == 0) return OMNI_PROTO_TCP;
    if (strcmp(s, "udp") == 0) return OMNI_PROTO_UDP;
    if (strcmp(s, "kcp") == 0) return OMNI_PROTO_KCP;
    return OMNI_PROTO_TCP;
}

static int send_app_message(OmniContext *ctx,
                            uint32_t type,
                            const void *payload,
                            uint32_t payload_len)
{
    /* 与客户端保持一致的统一发包函数。 */
    size_t total_len = MSG_HEADER_SIZE + (size_t)payload_len;
    uint8_t *frame = (uint8_t *)malloc(total_len);
    if (!frame) {
        logger_log("ERROR", "server", "malloc_frame_failed len=%zu", total_len);
        return OMNI_ERR_GENERIC;
    }

    MsgHeader hdr;
    omni_msg_header_encode(&hdr, type, payload_len, omni_now_ms());
    memcpy(frame, &hdr, MSG_HEADER_SIZE);
    if (payload_len > 0 && payload) {
        memcpy(frame + MSG_HEADER_SIZE, payload, payload_len);
    }

    ssize_t n = omni_send(ctx, frame, total_len);
    free(frame);

    if (n != (ssize_t)total_len) {
        logger_log("ERROR", "server",
                   "omni_send_failed expect=%zu got=%zd type=%u",
                   total_len, n, (unsigned)type);
        return OMNI_ERR_IO;
    }
    return OMNI_OK;
}

static int decode_app_message(const uint8_t *frame,
                              size_t frame_len,
                              MsgHeader *out_hdr,
                              const uint8_t **out_payload)
{
    /* 与客户端一致的统一解包校验。 */
    if (!frame || frame_len < MSG_HEADER_SIZE || !out_hdr || !out_payload) {
        return OMNI_ERR_PARAM;
    }

    MsgHeader net_hdr;
    memcpy(&net_hdr, frame, MSG_HEADER_SIZE);
    omni_msg_header_decode(&net_hdr, out_hdr);

    if ((size_t)out_hdr->len + MSG_HEADER_SIZE != frame_len) {
        return OMNI_ERR_IO;
    }

    *out_payload = frame + MSG_HEADER_SIZE;
    return OMNI_OK;
}

static void *recv_thread_main(void *arg)
{
    ServerRuntime *rt = (ServerRuntime *)arg;
    uint8_t frame[SERVER_FRAME_BUF_SIZE];

    /* 允许主线程在退出时取消本线程，避免阻塞 recv 导致无法收尾。 */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    while (atomic_load(&rt->running)) {
        ssize_t n = omni_recv(rt->ctx, frame, sizeof(frame));
        if (n < 0) {
            logger_log("ERROR", "server", "recv_failed n=%zd", n);
            break;
        }
        if (n == 0) {
            /*
             * recv 返回 0 的语义依赖协议：
             * - TCP: 对端连接关闭，接收线程可退出
             * - UDP/KCP: 可能仅表示当前无可读数据，继续等待
             */
            if (rt->proto == OMNI_PROTO_TCP) {
                logger_log("INFO", "server", "tcp_peer_closed");
                break;
            }
            usleep(2 * 1000);
            continue;
        }

        MsgHeader hdr;
        const uint8_t *payload = NULL;
        int rc = decode_app_message(frame, (size_t)n, &hdr, &payload);
        if (rc != OMNI_OK) {
            logger_log("ERROR", "server", "invalid_app_frame bytes=%zd rc=%d", n, rc);
            continue;
        }

        if (hdr.type == MSG_TYPE_FILE_CHUNK) {
            /* 文件分片：直接按顺序落盘。 */
            size_t nw = fwrite(payload, 1, hdr.len, rt->out_fp);
            if (nw != hdr.len) {
                logger_log("ERROR", "server", "fwrite_failed expect=%u got=%zu",
                           (unsigned)hdr.len, nw);
                break;
            }
            rt->bytes_written += nw;
        } else if (hdr.type == MSG_TYPE_FILE_END) {
            /*
             * 文件接收结束：
             * - 仅置位 transfer_done
             * - 不退出线程，让服务端在交互模式下继续保持长连接并可下发指令
             */
            fflush(rt->out_fp);
            atomic_store(&rt->transfer_done, 1);
            logger_log("INFO", "server", "file_transfer_end bytes=%llu",
                       (unsigned long long)rt->bytes_written);
            continue;
        } else if (hdr.type == MSG_TYPE_COMMAND) {
            /* 当前服务端不处理“来自客户端”的 COMMAND，仅记录日志。 */
            logger_log("INFO", "server",
                       "recv_command_from_peer len=%u (ignored)",
                       (unsigned)hdr.len);
        } else {
            logger_log("INFO", "server",
                       "recv_unknown_type type=%u len=%u",
                       (unsigned)hdr.type, (unsigned)hdr.len);
        }
    }

    atomic_store(&rt->running, 0);
    return NULL;
}

int main(int argc, char **argv)
{
    /* 命令行参数默认值。 */
    const char *proto_str = "tcp";
    const char *bind_ip = NULL;
    const char *output_path = NULL;
    int listen_port = 0;

    int opt;
    while ((opt = getopt(argc, argv, "p:b:P:o:")) != -1) {
        switch (opt) {
        case 'p':
            proto_str = optarg;
            break;
        case 'b':
            bind_ip = optarg;
            break;
        case 'P':
            listen_port = atoi(optarg);
            break;
        case 'o':
            output_path = optarg;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (listen_port <= 0 || !output_path) {
        usage(argv[0]);
        return 1;
    }

    FILE *out_fp = fopen(output_path, "wb");
    if (!out_fp) {
        perror("fopen");
        return 1;
    }

    OmniProtocol proto = parse_proto(proto_str);
    /* 服务端角色：仅监听本地端口。 */
    OmniContext *ctx = omni_init(OMNI_ROLE_SERVER, proto,
                                 bind_ip, (uint16_t)listen_port,
                                 NULL, 0);
    if (!ctx) {
        fclose(out_fp);
        fprintf(stderr, "omni_init failed\n");
        return 1;
    }

    ServerRuntime rt;
    rt.ctx = ctx;
    rt.proto = proto;
    rt.out_fp = out_fp;
    rt.bytes_written = 0;
    atomic_init(&rt.running, 1);
    atomic_init(&rt.transfer_done, 0);

    /* 启动接收线程处理文件写入主流程。 */
    pthread_t recv_tid;
    if (pthread_create(&recv_tid, NULL, recv_thread_main, &rt) != 0) {
        perror("pthread_create");
        omni_close(ctx);
        fclose(out_fp);
        return 1;
    }

    if (isatty(STDIN_FILENO)) {
        /*
         * 交互模式：
         * - 每次回车读取一行
         * - 非空行封装为 COMMAND 发送给客户端
         */
        char line[2048];
        while (atomic_load(&rt.running)) {
            if (!fgets(line, sizeof(line), stdin)) {
                break;
            }

            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }

            if (len == 0) {
                continue;
            }
            if (strcmp(line, "quit") == 0) {
                /* 主动退出交互循环。 */
                break;
            }

            int rc = send_app_message(ctx, MSG_TYPE_COMMAND, line, (uint32_t)len);
            if (rc != OMNI_OK) {
                logger_log("ERROR", "server", "send_command_failed");
                break;
            }
        }
    } else {
        /*
         * 非交互模式（如脚本后台）：
         * 只等待接收线程将 transfer_done 置位，避免阻塞在 stdin。
         */
        while (atomic_load(&rt.running) && !atomic_load(&rt.transfer_done)) {
            usleep(100 * 1000);
        }
    }

    /* 收尾：取消接收线程 -> join -> 关闭网络 -> 关闭文件。 */
    atomic_store(&rt.running, 0);
    pthread_cancel(recv_tid);
    pthread_join(recv_tid, NULL);
    omni_close(ctx);
    fclose(out_fp);
    return 0;
}
