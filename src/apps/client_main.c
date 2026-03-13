/*
 * client_main.c
 * 客户端：读取大文件分片发送，同时后台接收服务端 ASCII 指令并打印
 *
 * 线程模型：
 * - 主线程：读取文件并发送 FILE_CHUNK / FILE_END
 * - 子线程：持续接收服务端 COMMAND 并打印
 *
 * 消息格式：
 * - 每条业务消息为 [MsgHeader(16B) + payload]
 * - MsgHeader 字段由 common.h 中的 encode/decode 统一处理
 */

#include "common.h"
#include "network.h"
#include "logger.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLIENT_FRAME_BUF_SIZE (MSG_HEADER_SIZE + 65536u)

typedef struct ClientRuntime {
    /* 协议抽象层句柄。 */
    OmniContext *ctx;
    /* 线程共享运行标记：1=运行中，0=退出。 */
    atomic_int running;
} ClientRuntime;

/*
 * 进程级停止标记：
 * - 收到 SIGINT/SIGTERM（例如 Ctrl+C）时置 1
 * - 主线程据此触发收尾逻辑，保证线程/连接能优雅退出
 */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s -p tcp|udp|kcp -H <server_ip> -P <server_port> -f <file>\n"
            "     [-b <bind_port>] [-m <chunk_mtu>] [-w <wait_seconds|-1>]\n",
            prog);
}

static OmniProtocol parse_proto(const char *s)
{
    /* 输入非法时回退到 TCP，方便本地默认测试。 */
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
    /*
     * 统一应用层发包：
     * 1) 组装业务头（网络字节序）
     * 2) 拼接 payload
     * 3) 通过 omni_send 一次发送整帧
     */
    size_t total_len = MSG_HEADER_SIZE + (size_t)payload_len;
    uint8_t *frame = (uint8_t *)malloc(total_len);
    if (!frame) {
        logger_log("ERROR", "client", "malloc_frame_failed len=%zu", total_len);
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
        logger_log("ERROR", "client",
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
    /*
     * 统一应用层解包：
     * - 至少要有 16B 头
     * - 头中 len 与总帧长度必须一致，避免越界/脏数据
     */
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
    ClientRuntime *rt = (ClientRuntime *)arg;
    uint8_t frame[CLIENT_FRAME_BUF_SIZE];

    /*
     * 显式启用可取消：主线程收尾时通过 pthread_cancel 打断阻塞 recv，
     * 避免 UDP/KCP 场景下因长时间无回包导致 join 卡住。
     */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    while (atomic_load(&rt->running)) {
        ssize_t n = omni_recv(rt->ctx, frame, sizeof(frame));
        if (n < 0) {
            logger_log("ERROR", "client", "recv_failed n=%zd", n);
            break;
        }
        if (n == 0) {
            /* 0 在不同协议下可能代表“暂时无数据”或“对端关闭”，做短暂退避避免空转。 */
            usleep(2 * 1000);
            continue;
        }

        MsgHeader hdr;
        const uint8_t *payload = NULL;
        int rc = decode_app_message(frame, (size_t)n, &hdr, &payload);
        if (rc != OMNI_OK) {
            logger_log("ERROR", "client", "invalid_app_frame bytes=%zd rc=%d", n, rc);
            continue;
        }

        if (hdr.type == MSG_TYPE_COMMAND) {
            /* COMMAND 约定为 ASCII 文本，做安全截断后打印。 */
            char cmd[2048];
            size_t cpy = hdr.len < (uint32_t)(sizeof(cmd) - 1) ? hdr.len : (sizeof(cmd) - 1);
            memcpy(cmd, payload, cpy);
            cmd[cpy] = '\0';
            printf("[server-cmd] %s\n", cmd);
            fflush(stdout);
        } else {
            /* 客户端当前只消费 COMMAND，其它类型保留日志便于调试。 */
            logger_log("INFO", "client",
                       "recv_non_command type=%u len=%u",
                       (unsigned)hdr.type, (unsigned)hdr.len);
        }
    }

    atomic_store(&rt->running, 0);
    return NULL;
}

int main(int argc, char **argv)
{
    install_signal_handlers();

    /* 命令行参数默认值。 */
    const char *proto_str = "tcp";
    const char *server_ip = NULL;
    const char *file_path = NULL;
    int server_port = 0;
    int bind_port = 0;
    unsigned chunk_size = OMNI_DEFAULT_MTU;
    int wait_seconds = 2;

    int opt;
    while ((opt = getopt(argc, argv, "p:H:P:f:b:m:w:")) != -1) {
        switch (opt) {
        case 'p':
            proto_str = optarg;
            break;
        case 'H':
            server_ip = optarg;
            break;
        case 'P':
            server_port = atoi(optarg);
            break;
        case 'f':
            file_path = optarg;
            break;
        case 'b':
            bind_port = atoi(optarg);
            break;
        case 'm':
            chunk_size = (unsigned)strtoul(optarg, NULL, 10);
            break;
        case 'w':
            wait_seconds = atoi(optarg);
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (!server_ip || server_port <= 0 || !file_path) {
        usage(argv[0]);
        return 1;
    }
    if (chunk_size == 0 || chunk_size > 65536u) {
        /* 约束 chunk 上限，避免一次申请/发送过大缓冲。 */
        fprintf(stderr, "invalid chunk size: %u\n", chunk_size);
        return 1;
    }
    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    OmniProtocol proto = parse_proto(proto_str);
    /* 客户端角色：对端地址由 -H/-P 指定。 */
    OmniContext *ctx = omni_init(OMNI_ROLE_CLIENT, proto,
                                 NULL, (uint16_t)bind_port,
                                 server_ip, (uint16_t)server_port);
    if (!ctx) {
        fclose(fp);
        fprintf(stderr, "omni_init failed\n");
        return 1;
    }

    ClientRuntime rt;
    rt.ctx = ctx;
    atomic_init(&rt.running, 1);

    /* 启动异步接收线程（打印服务端指令）。 */
    pthread_t recv_tid;
    if (pthread_create(&recv_tid, NULL, recv_thread_main, &rt) != 0) {
        perror("pthread_create");
        atomic_store(&rt.running, 0);
        fclose(fp);
        omni_close(ctx);
        return 1;
    }

    uint8_t *chunk = (uint8_t *)malloc(chunk_size);
    if (!chunk) {
        logger_log("ERROR", "client", "malloc_chunk_failed size=%u", chunk_size);
        atomic_store(&rt.running, 0);
        pthread_cancel(recv_tid);
        pthread_join(recv_tid, NULL);
        omni_close(ctx);
        fclose(fp);
        return 1;
    }

    uint64_t total_sent = 0;
    /*
     * 主发送循环：
     * - 每次读取 chunk_size 字节
     * - 发送 FILE_CHUNK
     * - EOF 后发送 FILE_END
     */
    while (atomic_load(&rt.running)) {
        if (g_stop) {
            logger_log("INFO", "client", "signal_received_stop_sending");
            atomic_store(&rt.running, 0);
            break;
        }
        size_t nread = fread(chunk, 1, chunk_size, fp);
        if (nread == 0) {
            if (feof(fp)) {
                break;
            }
            if (ferror(fp)) {
                logger_log("ERROR", "client", "fread_failed");
                atomic_store(&rt.running, 0);
                break;
            }
        }

        if (nread > 0) {
            int rc = send_app_message(ctx, MSG_TYPE_FILE_CHUNK, chunk, (uint32_t)nread);
            if (rc != OMNI_OK) {
                atomic_store(&rt.running, 0);
                break;
            }
            total_sent += nread;
        }
    }

    if (atomic_load(&rt.running)) {
        /* 正常结束时发送 FILE_END，通知服务端落盘完成。 */
        int rc = send_app_message(ctx, MSG_TYPE_FILE_END, NULL, 0);
        if (rc != OMNI_OK) {
            atomic_store(&rt.running, 0);
        }
    }

    logger_log("INFO", "client", "file_transfer_done bytes=%llu",
               (unsigned long long)total_sent);
    free(chunk);
    fclose(fp);

    /*
     * 等待模式：
     * - wait_seconds >= 0: 发送完成后最多等待 N 秒
     * - wait_seconds < 0 : 常驻模式，直到 Ctrl+C（SIGINT）或连接异常
     */
    if (wait_seconds < 0) {
        logger_log("INFO", "client", "keepalive_mode=on press_ctrl_c_to_exit");
        while (atomic_load(&rt.running) && !g_stop) {
            sleep(1);
        }
    } else {
        for (int i = 0; i < wait_seconds && atomic_load(&rt.running) && !g_stop; ++i) {
            sleep(1);
        }
    }

    /* 收尾顺序：先停接收线程，再关闭网络上下文。 */
    atomic_store(&rt.running, 0);
    pthread_cancel(recv_tid);
    pthread_join(recv_tid, NULL);
    omni_close(ctx);
    return 0;
}
