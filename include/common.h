/*
 * common.h
 * 全局公共定义：消息头、错误码、通用宏
 */

#ifndef OMNISOCKET_COMMON_H
#define OMNISOCKET_COMMON_H

#include <stdint.h>
#include <time.h>

/* 统一的 16 字节消息头（解决 TCP 粘包用） */
typedef struct MsgHeader {
    uint32_t magic;      /* 固定魔数，用于快速校验 */
    uint32_t length;     /* 后续负载长度（字节数） */
    uint64_t seq;        /* 序列号或会话内消息 ID */
} MsgHeader;

#define MSG_HEADER_SIZE  (sizeof(MsgHeader)) /* 16 字节 */
#define MSG_MAGIC        0x4F4D4E49u         /* 'OMNI' */

/* 通用错误码（负数返回表示出错） */
enum {
    OMNI_OK          = 0,
    OMNI_ERR_GENERIC = -1,
    OMNI_ERR_PARAM   = -2,
    OMNI_ERR_IO      = -3,
    OMNI_ERR_TIMEOUT = -4
};

/* 获取当前单调时间（毫秒），用于延迟统计 */
static inline uint64_t omni_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

#endif /* OMNISOCKET_COMMON_H */

