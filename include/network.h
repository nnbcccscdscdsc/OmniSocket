/*
 * network.h
 * 统一的协议抽象层，对上层暴露 omni_* 接口
 *
 * 支持 TCP / UDP / KCP 三种协议，通过命令行参数切换：
 *   -t 使用 TCP
 *   -u 使用 UDP
 *   -k 使用 KCP
 */

#ifndef OMNISOCKET_NETWORK_H
#define OMNISOCKET_NETWORK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* for ssize_t */

/* 协议类型 */
typedef enum {
    OMNI_PROTO_TCP = 0,
    OMNI_PROTO_UDP = 1,
    OMNI_PROTO_KCP = 2
} OmniProtocol;

/* 角色：客户端 / 服务端 */
typedef enum {
    OMNI_ROLE_CLIENT = 0,
    OMNI_ROLE_SERVER = 1
} OmniRole;

/* 统一上下文句柄（对上层不透明） */
typedef struct OmniContext OmniContext;

/* 协议实现函数表（由底层协议模块提供） */
struct ProtoVTable {
    OmniContext *(*init)(OmniRole role,
                         const char *bind_ip,
                         uint16_t bind_port,
                         const char *peer_ip,
                         uint16_t peer_port);
    ssize_t (*send)(OmniContext *ctx, const void *buf, size_t len);
    ssize_t (*recv)(OmniContext *ctx, void *buf, size_t len);
    void (*close)(OmniContext *ctx);
};

/*
 * 创建并初始化一个网络上下文。
 *
 * 参数：
 *   role      - 客户端或服务端
 *   proto     - 协议类型（TCP/UDP/KCP）
 *   bind_ip   - 服务器监听或客户端本地绑定 IP（可为 NULL 表示 INADDR_ANY）
 *   bind_port - 监听端口或本地端口（0 表示让系统分配）
 *   peer_ip   - 对端 IP（客户端连接或服务端应答的默认地址，可为 NULL）
 *   peer_port - 对端端口
 *
 * 返回：
 *   成功：上下文指针
 *   失败：NULL
 */
OmniContext *omni_init(OmniRole role,
                       OmniProtocol proto,
                       const char *bind_ip,
                       uint16_t bind_port,
                       const char *peer_ip,
                       uint16_t peer_port);

/* 发送数据（阻塞直到全部发送或出错） */
ssize_t omni_send(OmniContext *ctx, const void *buf, size_t len);

/* 接收数据（阻塞直到读到至少 1 字节或出错/关闭） */
ssize_t omni_recv(OmniContext *ctx, void *buf, size_t len);

/* 关闭并释放上下文 */
void omni_close(OmniContext *ctx);

#endif /* OMNISOCKET_NETWORK_H */

