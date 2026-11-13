CC      ?= gcc
CFLAGS  += -std=c99 -Wall -Wextra -I. -Isrc/util -Isrc/channel -Isrc/route -Isrc/diag -Ithirdparty

.PHONY: all test clean

all: test

# PC 侧单元测试，不依赖 CAN 接口
test: test/frame_queue_test test/route_table_test
	./test/frame_queue_test
	./test/route_table_test

test/frame_queue_test: test/frame_queue_test.c src/util/frame_queue.c
	$(CC) $(CFLAGS) $^ -o $@

test/route_table_test: test/route_table_test.c src/route/route_table.c thirdparty/log/log.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	$(RM) test/frame_queue_test test/route_table_test gateway
