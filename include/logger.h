/*
 * logger.h
 * 日志与性能统计接口
 */

#ifndef OMNISOCKET_LOGGER_H
#define OMNISOCKET_LOGGER_H

#include <stddef.h>
#include <stdint.h>

/* 通过该结构体收集全局统计信息 */
typedef struct OmniStats {
    uint64_t start_ms;           /* 起始时间（毫秒） */
    uint64_t last_report_ms;     /* 上一次打印日志时间 */

    uint64_t bytes_sent;         /* 发送总字节数 */
    uint64_t bytes_recv;         /* 接收总字节数 */

    uint64_t send_count;         /* 调用 omni_send 次数 */
    uint64_t recv_count;         /* 调用 omni_recv 次数 */

    uint64_t last_rtt_ms;        /* 最近一次 RTT */
    uint64_t max_rtt_ms;         /* 最大 RTT */

    uint64_t tcp_retrans;        /* 预留：TCP 重传统计（如可从内核获取） */
    uint64_t udp_retrans;        /* UDP 上层重传次数 */
    uint64_t kcp_retrans;        /* KCP 内部重传次数（可从 ikcp 统计） */
} OmniStats;

/* 初始化统计模块，在程序启动时调用一次 */
void logger_init(void);

/* 记录一次发送/接收 */
void logger_on_send(size_t bytes);
void logger_on_recv(size_t bytes);

/* 记录一次 RTT（由上层在合适时机调用） */
void logger_on_rtt(uint64_t rtt_ms);

/* 记录 KCP 重传次数变化（可在 KCP 更新循环中调用） */
void logger_on_kcp_retrans(uint64_t delta);

/* 计算当前吞吐量（返回：字节/秒） */
double logger_calculate_throughput(void);

/* 打印一条结构化性能日志（例如每隔若干秒调用） */
void logger_print_performance_log(const char *tag);

/* 结构化通用日志（key=value 形式） */
void logger_log(const char *level, const char *component,
                const char *fmt, ...);

/* 获取内部统计快照（线程不安全，仅调试用） */
OmniStats logger_get_snapshot(void);

#endif /* OMNISOCKET_LOGGER_H */

