OmniSocket/
├── include/
│   ├── common.h          # 全局定义：MsgHeader 结构体、错误码、宏定义
│   ├── network.h         # 定义统一的协议接口 (omni_init, omni_send等)
│   ├── kcp/              # 存放外部 KCP 源码 (ikcp.h, ikcp.c)
│   └── logger.h          # 日志统计函数声明
├── src/
│   ├── protocols/
│   │   ├── tcp_impl.c    # TCP 专用实现
│   │   ├── udp_impl.c    # UDP 专用实现
│   │   └── kcp_impl.c    # KCP 专用实现（调用 ikcp.c）
│   ├── core/
│   │   ├── network.c     # 协议分发逻辑（根据参数选 TCP/UDP/KCP）
│   │   └── logger.c      # 延迟计算、吞吐量统计逻辑实现
│   ├── apps/
│   │   ├── client_main.c # 客户端入口（文件读取、指令接收）
│   │   ├── server_main.c # 服务端入口（指令输入、数据接收）
│   │   └── relay_main.c  # 转发器入口（中转逻辑）
├── build/                # 编译产物目录
├── Makefile              # 关键：支持 make server 和 make client_arm
└── README.md             # 运行指南与参数说明