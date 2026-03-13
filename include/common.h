/*
 * common.h

. * 全局公共定义：消息头、错误码、通用宏
 */

#ifndef OMNISOCKET_COMMON_H
#define OMNISOCKET_COMMON_H

#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>

/* 统一的 16 字节消息头（应用层消息头） */
typedef struct MsgHeader {
    uint32_t type;       /* 消息类型：文件块/控制指令等 */
    uint32_t len;        /* 后续负载长度（字节数） */
    uint64_t timestamp;  /* 发送时间戳（毫秒） */
} MsgHeader;

#define MSG_HEADER_SIZE  (sizeof(MsgHeader)) /* 16 字节 */

enum {
    MSG_TYPE_FILE_CHUNK = 1,
    MSG_TYPE_FILE_END   = 2,
    MSG_TYPE_COMMAND    = 3,
    MSG_TYPE_RAW        = 100
};

#define OMNI_DEFAULT_MTU 1400u

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
// 64 位整数主机序与网络序转换工具
static inline uint64_t omni_bswap64(uint64_t x)
{
    return ((x & 0x00000000000000FFull) << 56) |
           ((x & 0x000000000000FF00ull) << 40) |
           ((x & 0x0000000000FF0000ull) << 24) |
           ((x & 0x00000000FF000000ull) << 8) |
           ((x & 0x000000FF00000000ull) >> 8) |
           ((x & 0x0000FF0000000000ull) >> 24) |
           ((x & 0x00FF000000000000ull) >> 40) |
           ((x & 0xFF00000000000000ull) >> 56);
}

static inline uint64_t omni_htonll(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return omni_bswap64(x);
#else
    return x;
#endif
}

static inline uint64_t omni_ntohll(uint64_t x)
{
    return omni_htonll(x);
}

static inline void omni_msg_header_encode(MsgHeader *out_hdr,
                                          uint32_t type,
                                          uint32_t len,
                                          uint64_t timestamp_ms)
{
    out_hdr->type = htonl(type);
    out_hdr->len = htonl(len);
    out_hdr->timestamp = omni_htonll(timestamp_ms);
}

static inline void omni_msg_header_decode(const MsgHeader *net_hdr,
                                          MsgHeader *host_hdr)
{
    host_hdr->type = ntohl(net_hdr->type);
    host_hdr->len = ntohl(net_hdr->len);
    host_hdr->timestamp = omni_ntohll(net_hdr->timestamp);
}

#endif /* OMNISOCKET_COMMON_H */

