# 默认本机编译器（可由环境变量覆盖，例如 CC=clang）。
CC ?= gcc
# ARM 交叉编译工具链前缀（可按本地环境替换）。
CROSS_COMPILE ?= arm-linux-gnueabihf-
ARM_CC ?= $(CROSS_COMPILE)gcc

# 产物目录（native: build/，arm: build/arm/）。
BUILD_DIR ?= build

# 编译参数：
# - CFLAGS: 优化级别、调试符号、告警、C 标准
# - CPPFLAGS: POSIX 特性宏，确保 clock_gettime 等接口可见
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11
CPPFLAGS ?= -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
# 链接 pthread（apps 中有多线程）。
LDLIBS ?= -lpthread
INCLUDES := -Iinclude

# 所有二进制共享的核心源码（网络核心 + 协议实现 + ikcp）。
COMMON_SRCS := \
	src/core/network.c \
	src/core/logger.c \
	src/protocols/tcp_impl.c \
	src/protocols/udp_impl.c \
	src/protocols/kcp_impl.c \
	src/protocols/ikcp.c

# 各应用入口源文件。
APP_TEST_SRC := src/apps/test_main.c
APP_CLIENT_SRC := src/apps/client_main.c
APP_SERVER_SRC := src/apps/server_main.c
APP_RELAY_SRC := src/apps/relay_main.c

# 将源文件映射到 BUILD_DIR 下的对象文件路径。
COMMON_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))
TEST_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_TEST_SRC))
CLIENT_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_CLIENT_SRC))
SERVER_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_SERVER_SRC))
RELAY_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_RELAY_SRC))

# 默认构建目标：4 个可执行程序。
TARGETS := \
	$(BUILD_DIR)/omni_test \
	$(BUILD_DIR)/omni_client \
	$(BUILD_DIR)/omni_server \
	$(BUILD_DIR)/omni_relay

.PHONY: all arm clean help

# 本机构建入口。
all: $(TARGETS)

# 各可执行程序链接规则。
$(BUILD_DIR)/omni_test: $(COMMON_OBJS) $(TEST_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/omni_client: $(COMMON_OBJS) $(CLIENT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/omni_server: $(COMMON_OBJS) $(SERVER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/omni_relay: $(COMMON_OBJS) $(RELAY_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# 通用编译规则：
# - 自动创建对象文件所在目录
# - 编译单个 .c 为 .o
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ARM 构建入口：通过子 make 覆盖 BUILD_DIR 与 CC。
arm:
	$(MAKE) BUILD_DIR=build/arm CC=$(ARM_CC) all

# 清理构建目录。
clean:
	rm -rf build

# 便捷帮助信息。
help:
	@echo "make        -> build native binaries in build/"
	@echo "make arm    -> build ARM binaries in build/arm (arm-linux-gnueabihf-gcc)"
	@echo "make clean  -> remove build artifacts"
