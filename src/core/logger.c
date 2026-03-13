/*
 * logger.c
 * 性能统计与结构化日志实现
 */

#include "logger.h"
#include "common.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

static OmniStats g_stats;
static FILE *g_json_fp = NULL;
static int g_min_level = 1; /* default INFO */

static uint64_t now_ms(void)
{
    return omni_now_ms();
}

static int level_to_int(const char *level)
{
    if (!level) return 1;
    if (strcmp(level, "DEBUG") == 0) return 0;
    if (strcmp(level, "INFO") == 0) return 1;
    if (strcmp(level, "WARN") == 0) return 2;
    if (strcmp(level, "ERROR") == 0) return 3;
    return 1;
}

static void ewma_update(double *avg, double sample, double alpha)
{
    if (*avg <= 0.0) {
        *avg = sample;
        return;
    }
    *avg = (*avg) * (1.0 - alpha) + sample * alpha;
}

void logger_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_ms = now_ms();
    g_stats.last_report_ms = g_stats.start_ms;
    g_stats.send_call_min_ms = UINT64_MAX;
    g_stats.recv_call_min_ms = UINT64_MAX;

    const char *lvl_env = getenv("OMNI_LOG_LEVEL");
    g_min_level = level_to_int(lvl_env);

    if (!g_json_fp) {
        g_json_fp = fopen("omni_logs.jsonl", "a");
    }
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

void logger_on_send_call_latency(uint64_t ms)
{
    g_stats.last_send_call_ms = ms;
    if (ms < g_stats.send_call_min_ms) g_stats.send_call_min_ms = ms;
    if (ms > g_stats.send_call_max_ms) g_stats.send_call_max_ms = ms;
    ewma_update(&g_stats.send_call_avg_ms, (double)ms, 0.2);
}

void logger_on_recv_call_latency(uint64_t ms)
{
    g_stats.last_recv_call_ms = ms;
    if (ms < g_stats.recv_call_min_ms) g_stats.recv_call_min_ms = ms;
    if (ms > g_stats.recv_call_max_ms) g_stats.recv_call_max_ms = ms;
    ewma_update(&g_stats.recv_call_avg_ms, (double)ms, 0.2);
}

void logger_on_proto_send_latency(uint64_t ms)
{
    ewma_update(&g_stats.proto_send_avg_ms, (double)ms, 0.2);
}

void logger_on_proto_recv_latency(uint64_t ms)
{
    ewma_update(&g_stats.proto_recv_avg_ms, (double)ms, 0.2);
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

static void json_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_sz; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\\' || c == '\"') {
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (c == '\t') {
            dst[j++] = '\\';
            dst[j++] = 't';
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
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
            "send_call_last_ms=%llu send_call_min_ms=%llu send_call_max_ms=%llu send_call_avg_ms=%.3f "
            "recv_call_last_ms=%llu recv_call_min_ms=%llu recv_call_max_ms=%llu recv_call_avg_ms=%.3f "
            "proto_send_avg_ms=%.3f proto_recv_avg_ms=%.3f "
            "last_rtt_ms=%llu max_rtt_ms=%llu "
            "kcp_retrans=%llu\n",
            tag ? tag : "periodic",
            (unsigned long long)elapsed_ms,
            (unsigned long long)g_stats.bytes_sent,
            (unsigned long long)g_stats.bytes_recv,
            (unsigned long long)g_stats.send_count,
            (unsigned long long)g_stats.recv_count,
            thr,
            (unsigned long long)g_stats.last_send_call_ms,
            (unsigned long long)((g_stats.send_call_min_ms == UINT64_MAX) ? 0 : g_stats.send_call_min_ms),
            (unsigned long long)g_stats.send_call_max_ms,
            g_stats.send_call_avg_ms,
            (unsigned long long)g_stats.last_recv_call_ms,
            (unsigned long long)((g_stats.recv_call_min_ms == UINT64_MAX) ? 0 : g_stats.recv_call_min_ms),
            (unsigned long long)g_stats.recv_call_max_ms,
            g_stats.recv_call_avg_ms,
            g_stats.proto_send_avg_ms,
            g_stats.proto_recv_avg_ms,
            (unsigned long long)g_stats.last_rtt_ms,
            (unsigned long long)g_stats.max_rtt_ms,
            (unsigned long long)g_stats.kcp_retrans);

    g_stats.last_report_ms = now;

    if (g_json_fp) {
        fprintf(g_json_fp,
                "{\"ts_ms\":%llu,"
                "\"level\":\"INFO\","
                "\"component\":\"perf\","
                "\"tag\":\"%s\","
                "\"elapsed_ms\":%llu,"
                "\"bytes_sent\":%llu,"
                "\"bytes_recv\":%llu,"
                "\"send_count\":%llu,"
                "\"recv_count\":%llu,"
                "\"throughput_bytes_per_sec\":%.6f,"
                "\"send_call_last_ms\":%llu,"
                "\"send_call_min_ms\":%llu,"
                "\"send_call_max_ms\":%llu,"
                "\"send_call_avg_ms\":%.6f,"
                "\"recv_call_last_ms\":%llu,"
                "\"recv_call_min_ms\":%llu,"
                "\"recv_call_max_ms\":%llu,"
                "\"recv_call_avg_ms\":%.6f,"
                "\"proto_send_avg_ms\":%.6f,"
                "\"proto_recv_avg_ms\":%.6f,"
                "\"last_rtt_ms\":%llu,"
                "\"max_rtt_ms\":%llu,"
                "\"kcp_retrans\":%llu}\n",
                (unsigned long long)now,
                tag ? tag : "periodic",
                (unsigned long long)elapsed_ms,
                (unsigned long long)g_stats.bytes_sent,
                (unsigned long long)g_stats.bytes_recv,
                (unsigned long long)g_stats.send_count,
                (unsigned long long)g_stats.recv_count,
                thr,
                (unsigned long long)g_stats.last_send_call_ms,
                (unsigned long long)((g_stats.send_call_min_ms == UINT64_MAX) ? 0 : g_stats.send_call_min_ms),
                (unsigned long long)g_stats.send_call_max_ms,
                g_stats.send_call_avg_ms,
                (unsigned long long)g_stats.last_recv_call_ms,
                (unsigned long long)((g_stats.recv_call_min_ms == UINT64_MAX) ? 0 : g_stats.recv_call_min_ms),
                (unsigned long long)g_stats.recv_call_max_ms,
                g_stats.recv_call_avg_ms,
                g_stats.proto_send_avg_ms,
                g_stats.proto_recv_avg_ms,
                (unsigned long long)g_stats.last_rtt_ms,
                (unsigned long long)g_stats.max_rtt_ms,
                (unsigned long long)g_stats.kcp_retrans);
        fflush(g_json_fp);
    }
}

void logger_log(const char *level, const char *component,
                const char *fmt, ...)
{
    const char *lvl = level ? level : "INFO";
    const char *comp = component ? component : "general";

    if (level_to_int(lvl) < g_min_level) {
        return;
    }

    FILE *fp = stderr;
    print_timestamp(fp);
    fprintf(fp, "level=%s component=%s ", lvl, comp);

    char msg_buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, ap);
    va_end(ap);

    fputs(msg_buf, fp);
    fputc('\n', fp);

    if (g_json_fp) {
        char esc_buf[2048];
        json_escape(msg_buf, esc_buf, sizeof(esc_buf));
        uint64_t ts = now_ms();
        fprintf(g_json_fp,
                "{\"ts_ms\":%llu,"
                "\"level\":\"%s\","
                "\"component\":\"%s\","
                "\"message\":\"%s\"}\n",
                (unsigned long long)ts,
                lvl,
                comp,
                esc_buf);
        fflush(g_json_fp);
    }
}

OmniStats logger_get_snapshot(void)
{
    return g_stats;
}

