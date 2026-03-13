/*
 * logger.c
 * 性能统计与结构化日志实现
 */

#include "logger.h"
#include "common.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static OmniStats g_stats;

static uint64_t now_ms(void)
{
    return omni_now_ms();
}

void logger_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_ms = now_ms();
    g_stats.last_report_ms = g_stats.start_ms;
}

void logger_on_send(size_t bytes)
{
    g_stats.bytes_sent += bytes;
    g_stats.send_count++;
}

void logger_on_recv(size_t bytes)
{
    g_stats.bytes_recv += bytes;
    g_stats.recv_count++;
}

void logger_on_rtt(uint64_t rtt_ms)
{
    g_stats.last_rtt_ms = rtt_ms;
    if (rtt_ms > g_stats.max_rtt_ms) {
        g_stats.max_rtt_ms = rtt_ms;
    }
}

void logger_on_kcp_retrans(uint64_t delta)
{
    g_stats.kcp_retrans += delta;
}

double logger_calculate_throughput(void)
{
    uint64_t now = now_ms();
    uint64_t elapsed_ms = now - g_stats.start_ms;
    if (elapsed_ms == 0) {
        return 0.0;
    }
    double seconds = (double)elapsed_ms / 1000.0;
    return (double)(g_stats.bytes_sent + g_stats.bytes_recv) / seconds;
}

static void print_timestamp(FILE *fp)
{
    uint64_t ms = now_ms();
    fprintf(fp, "ts=%llu ", (unsigned long long)ms);
}

void logger_print_performance_log(const char *tag)
{
    uint64_t now = now_ms();
    uint64_t elapsed_ms = now - g_stats.start_ms;
    double thr = logger_calculate_throughput();

    FILE *fp = stderr;
    print_timestamp(fp);
    fprintf(fp,
            "level=INFO component=perf tag=%s "
            "elapsed_ms=%llu bytes_sent=%llu bytes_recv=%llu "
            "send_count=%llu recv_count=%llu "
            "throughput_bytes_per_sec=%.2f "
            "last_rtt_ms=%llu max_rtt_ms=%llu "
            "kcp_retrans=%llu\n",
            tag ? tag : "periodic",
            (unsigned long long)elapsed_ms,
            (unsigned long long)g_stats.bytes_sent,
            (unsigned long long)g_stats.bytes_recv,
            (unsigned long long)g_stats.send_count,
            (unsigned long long)g_stats.recv_count,
            thr,
            (unsigned long long)g_stats.last_rtt_ms,
            (unsigned long long)g_stats.max_rtt_ms,
            (unsigned long long)g_stats.kcp_retrans);

    g_stats.last_report_ms = now;
}

void logger_log(const char *level, const char *component,
                const char *fmt, ...)
{
    FILE *fp = stderr;
    print_timestamp(fp);
    fprintf(fp, "level=%s component=%s ", level ? level : "INFO",
            component ? component : "general");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
}

OmniStats logger_get_snapshot(void)
{
    return g_stats;
}

