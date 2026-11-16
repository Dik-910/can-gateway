CC      ?= gcc
CFLAGS  += -std=c99 -Wall -Wextra -I. -Isrc/util -Isrc/channel -Isrc/route -Isrc/diag -Isrc/uplink -Ithirdparty
LDLIBS  += -lpthread

# MQTT 宏放在 CPPFLAGS：命令行传 CFLAGS=... 时不会覆盖掉，
# 否则会编出没有 MQTT 的版本而测试照常"通过"。
HAVE_MQTT := $(shell test -f /usr/include/MQTTClient.h && echo 1)
ifeq ($(HAVE_MQTT),1)
CPPFLAGS += -DGW_HAVE_MQTT
LDLIBS   += -lpaho-mqtt3c
endif

.PHONY: all test clean

all: gateway test

# PC 侧单元测试，不依赖 CAN 接口
test: test/frame_queue_test test/route_table_test test/diag_router_test test/config_test test/rate_limiter_test test/aggregator_test
	./test/frame_queue_test
	./test/route_table_test
	./test/diag_router_test
	./test/config_test
	./test/rate_limiter_test
	./test/aggregator_test

test/frame_queue_test: test/frame_queue_test.c src/util/frame_queue.c
	$(CC) $(CFLAGS) $^ -o $@

test/route_table_test: test/route_table_test.c src/route/route_table.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

# 网关可执行文件
UPLINK_SRC := src/uplink/uplink.c src/uplink/aggregator.c

gateway: src/app/gateway_main.c src/route/route_engine.c src/route/route_table.c src/route/rate_limiter.c src/diag/diag_router.c \
         src/channel/can_channel.c src/util/frame_queue.c src/util/config.c $(UPLINK_SRC) thirdparty/log/log.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

test/aggregator_test: test/aggregator_test.c src/uplink/aggregator.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

test/diag_router_test: test/diag_router_test.c src/diag/diag_router.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

test/config_test: test/config_test.c src/util/config.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

test/rate_limiter_test: test/rate_limiter_test.c src/route/rate_limiter.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

# 端到端联调：需要 vcan0/vcan1 与运行中的 broker。
# 本地建 vcan：
#   sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
#   （vcan1 同理）
.PHONY: uplink-check
uplink-check: gateway
	@if [ -z "$(HAVE_MQTT)" ]; then \
	  echo "SKIP: Paho C not found (install libpaho-mqtt-dev)"; exit 0; fi
	@echo "--- starting gateway ---"
	@./gateway --config examples/gateway.conf --stats 3 > .gateway.log 2>&1 & \
	 GW=$$!; sleep 2; \
	 python3 tools/mqtt_uplink_check.py $(UP_ARGS); RC=$$?; \
	 kill -INT $$GW 2>/dev/null; wait $$GW 2>/dev/null; \
	 echo "--- gateway log ---"; tail -10 .gateway.log; \
	 exit $$RC

clean:
	$(RM) test/frame_queue_test test/route_table_test test/diag_router_test test/config_test test/rate_limiter_test test/aggregator_test gateway
