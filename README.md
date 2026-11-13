# can-gateway

车载 CAN 网关：在多路 CAN 之间路由报文，并中转诊断请求。

```
   节点 A ──┐                  ┌── 节点 C
            ├── CAN 1 ── 网关 ──┤
   节点 B ──┘                  └── CAN 2 ── 节点 D
```

## 与 smartgateway 的关系

本项目的骨架来自 [smartgateway](https://github.com/Dik-910/smartgateway)（Linux IoT 网关），
但两者优化目标不同，所以只复用了一部分：

| 原模块 | 处理 | 原因 |
|---|---|---|
| `thirdparty/log`（rxi/log, MIT） | **直接复用** | 成熟稳定，无改动必要 |
| `app_common_getCurrentTime` | 复用思路，重写 | 只需毫秒时间戳 |
| `app_buffer` 双缓冲 | **换成环形帧队列** | CAN 帧定长 16 字节，环形队列 O(1) 读写、零 memmove；双缓冲区适合变长数据流 |
| `app_device` 单例 Device | **换成多实例 can_channel** | 网关至少两路 CAN，单例假设不成立 |
| `app_pool` 线程池 | **换成单线程 epoll** | 见下方说明 |
| `app_message`（JSON 转换） | 移除 | CAN 报文不需要转 JSON |

### 为什么不用线程池

IoT 网关追求吞吐，线程池排队无所谓；车载网关要的是**确定性延迟**。

线程池里，一个诊断报文可能排在几十个周期报文后面，延迟从 1ms 变成 20ms 且不可预测。
所以改成单线程 epoll 循环：读、路由、写都在同一线程，延迟可估算。

代价是单核利用率受限。对本场景（每路 CAN 满载也就几千帧/秒）完全够用。

## 当前状态

| 模块 | 状态 |
|---|---|
| `src/util/frame_queue.c` | 已实现，测试覆盖回绕、溢出、单槽 |
| `src/channel/can_channel.c` | 已实现，SocketCAN 多实例 |
| `src/route/route_table.c` | 已实现，范围匹配 + ID 改写 + 首命即返回 |
| `src/route/route_engine.c` | 已实现，epoll 主循环 |
| `src/diag/diag_router.c` | 已实现，多 tester 会话映射 |

单元测试 87 项：frame_queue 36 / route_table 24 / diag_router 27。

## 诊断路由

多个 tester 同时诊断多个节点时，节点都按标准回 `0x7E8`，网关无法判断该交给谁。
解决方式是维护映射表，把两侧 ID 解耦：

```
tester1  0x7E0 ──┐                    ┌── 节点1  请求 0x7E0 / 响应 0x7E8
                  ├── 网关映射表 ──────┤
tester2  0x7E1 ──┘                    └── 节点2  请求 0x7E2 / 响应 0x7EA
```

- 请求方向：按 `tester_channel + tester_req_id` 查表，改写为节点侧 ID
- 响应方向：按 `target_channel + target_rsp_id` 反查，改写回 tester 侧 ID

**约束**：`target_rsp_id` 在表内必须唯一。响应帧里只有 CAN ID 能定位来源，
两个节点回同一个 ID 就无法分辨——这是协议决定的，配置时直接拒绝。

## 构建

```bash
make test       # PC 侧单元测试，不依赖 CAN 接口
```

## 运行环境

需要 Linux 与 SocketCAN。开发阶段用 vcan：

```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link add dev vcan1 type vcan
sudo ip link set up vcan0
sudo ip link set up vcan1
```

## 相关项目

- [can-node](https://github.com/Dik-910/can-node) —— CAN 节点固件（UDS / ISO-TP / Bootloader），
  作为本网关的下游设备
- [smartgateway](https://github.com/Dik-910/smartgateway) —— 前身，Linux IoT 网关

## 许可证

木兰宽松许可证，第 2 版。见 [LICENSE](LICENSE)。
`thirdparty/log` 为 rxi/log，遵循其 MIT 许可，版权声明保留在源文件中。
