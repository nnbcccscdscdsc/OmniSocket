# OmniSocket

统一的 TCP / UDP / KCP 传输框架，包含：
- 协议抽象层（`omni_init / omni_send / omni_recv`）
- 客户端：文件分片发送 + 异步接收服务端 ASCII 指令
- 服务端：接收并写文件 + 交互输入指令下发客户端
- 转发器：A->B 中转，支持运行时动态修改目标端口

## 目录结构

```text
OmniSocket/
├── include/
│   ├── common.h          # MsgHeader(type,len,timestamp)、消息类型、通用宏
│   ├── network.h         # 统一协议接口定义
│   ├── kcp/ikcp.h        # KCP 头文件
│   └── logger.h          # 日志与统计接口
├── src/
│   ├── protocols/
│   │   ├── tcp_impl.c    # TCP 实现（16字节头 + 粘包拆包）
│   │   ├── udp_impl.c    # UDP 实现（sendto/recvfrom）
│   │   ├── kcp_impl.c    # KCP 实现（基于 UDP + ikcp）
│   │   └── ikcp.c        # KCP 源码
│   ├── core/
│   │   ├── network.c     # 协议工厂分发
│   │   └── logger.c      # 性能统计日志
│   └── apps/
│       ├── client_main.c # 客户端入口
│       ├── server_main.c # 服务端入口
│       ├── relay_main.c  # 转发器入口
│       └── test_main.c   # 简易协议连通性测试
├── scripts/
│   └── local_smoke_test.sh # 本机一键 smoke 测试
├── build/                # 编译产物目录
├── Makefile
└── README.md
```

## 构建

### 本机构建

```bash
make
```

生成：
- `build/omni_client`
- `build/omni_server`
- `build/omni_relay`
- `build/omni_test`

### ARM 交叉编译

默认使用 `arm-linux-gnueabihf-gcc`：

```bash
make arm
```

生成到 `build/arm/` 目录。

## 程序参数

### `omni_server`

```bash
build/omni_server -p tcp|udp|kcp -P <listen_port> -o <output_file> [-b <bind_ip>]
```

说明：
- 接收客户端发送的文件分片并写入 `output_file`
- 若在交互终端运行，可在标准输入输入 ASCII 文本并回发给客户端
- 输入 `quit` 可退出服务端交互循环

### `omni_client`

```bash
build/omni_client -p tcp|udp|kcp -H <server_ip> -P <server_port> -f <file> [-b <bind_port>] [-m <chunk_mtu>] [-w <wait_seconds|-1>]
```

说明：
- 读取 `file`，按 `chunk_mtu`（默认 1400）分片发送
- 发送结束后额外发送 `FILE_END` 控制包
- 后台线程持续接收并打印服务端 ASCII 指令
- `-w -1` 表示常驻模式，直到手动 `Ctrl+C`

### `omni_relay`

```bash
build/omni_relay -p tcp|udp|kcp -L <listen_port> -H <target_ip> -P <target_port>
```

标准输入支持命令：
- `set <ip> <port>`：动态修改转发目标
- `show`：显示当前目标
- `quit`：退出 relay

## 快速启动（本机）

先准备一个测试文件：

```bash
dd if=/dev/urandom of=/tmp/input.bin bs=1400 count=64
```

### TCP 直连（2 个终端）

终端 1：

```bash
build/omni_server -p tcp -P 9000 -o /tmp/out_tcp.bin
```

终端 2：

```bash
build/omni_client -p tcp -H 127.0.0.1 -P 9000 -f /tmp/input.bin
```

校验：

```bash
cmp -s /tmp/input.bin /tmp/out_tcp.bin && echo OK || echo FAIL
```

### UDP 直连（2 个终端）

终端 1：

```bash
build/omni_server -p udp -P 9001 -o /tmp/out_udp.bin
```

终端 2：

```bash
build/omni_client -p udp -H 127.0.0.1 -P 9001 -f /tmp/input.bin
```

校验：

```bash
cmp -s /tmp/input.bin /tmp/out_udp.bin && echo OK || echo FAIL
```

### KCP 直连（2 个终端）

终端 1：

```bash
build/omni_server -p kcp -P 9002 -o /tmp/out_kcp.bin
```

终端 2：

```bash
build/omni_client -p kcp -H 127.0.0.1 -P 9002 -f /tmp/input.bin
```

校验：

```bash
cmp -s /tmp/input.bin /tmp/out_kcp.bin && echo OK || echo FAIL
```

## Relay 场景示例（3 个终端）

终端 1（最终接收端 B）：

```bash
build/omni_server -p udp -P 9102 -o /tmp/out_relay.bin
```

终端 2（relay）：

```bash
build/omni_relay -p udp -L 9101 -H 127.0.0.1 -P 9102
```

终端 3（发送端 A）：

```bash
build/omni_client -p udp -H 127.0.0.1 -P 9101 -f /tmp/input.bin
```

relay 终端可输入：

```text
show
set 127.0.0.1 9103
```
