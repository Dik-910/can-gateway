CC      ?= gcc
CFLAGS  += -std=c99 -Wall -Wextra -I. -Isrc/util -Isrc/channel -Isrc/route -Isrc/diag -Ithirdparty

.PHONY: all test clean

all: gateway test

# PC 侧单元测试，不依赖 CAN 接口
test: test/frame_queue_test test/route_table_test test/diag_router_test test/config_test test/rate_limiter_test
	./test/frame_queue_test
	./test/route_table_test
	./test/diag_router_test
	./test/config_test
	./test/rate_limiter_test

test/frame_queue_test: test/frame_queue_test.c src/util/frame_queue.c
	$(CC) $(CFLAGS) $^ -o $@

test/route_table_test: test/route_table_test.c src/route/route_table.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

# 网关可执行文件
gateway: src/app/gateway_main.c src/route/route_engine.c src/route/route_table.c src/route/rate_limiter.c src/diag/diag_router.c \
         src/channel/can_channel.c src/util/frame_queue.c src/util/config.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

test/diag_router_test: test/diag_router_test.c src/diag/diag_router.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

test/config_test: test/config_test.c src/util/config.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

test/rate_limiter_test: test/rate_limiter_test.c src/route/rate_limiter.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	$(RM) test/frame_queue_test test/route_table_test test/diag_router_test test/config_test test/rate_limiter_test gateway
