# can-gateway

[![ci](https://github.com/Dik-910/can-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/Dik-910/can-gateway/actions/workflows/ci.yml)

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
| `src/route/route_engine.c` | 已实现，epoll 主循环、断链重连、诊断超时重传 |
| `src/diag/diag_router.c` | 已实现，多 tester 会话映射与请求监督 |
| `src/route/rate_limiter.c` | 已实现，洪水防护与限速 |
| `src/util/config.c` | 已实现，INI 配置驱动 |

单元测试 286 项：frame_queue 36 / route_table 36 / diag_router 27 / diag_timeout 83 /
config 25 / rate_limiter 22 / aggregator 30 / channel_reconnect 27。

断言总数以各套输出的 `Result: N passed` 行求和为准。aggregator 只打印汇总行，
不逐条打印 `[PASS]`，因此按 `[PASS]` 行统计会得到 256（286 − 30）。

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

## 诊断会话监督

映射表解决了「送给谁」，但没解决「送丢了」。总线上的单帧请求会因仲裁失败、
错误帧或发送缓冲满而丢掉，节点收不到就不会有响应，tester 只能等到自己的
P2 超时后整条请求重来。

网关在转发之后登记一条待响应项，窗口内没有响应就重传一次：

| 时刻 | 动作 |
|---|---|
| 请求转发成功 | 记录帧内容与节点侧 ID，截止时刻 = now + 150 ms |
| 响应到达 | 结算，计 `answered` |
| 响应为 `03 7F xx 78` | 节点仍在处理，截止时刻推到 now + 2000 ms，并停用重传 |
| 150 ms 到期且未重传过 | 原样重发一次，计 `retry` |
| 重传后仍无响应 | 计 `timeout` 并释放，由 tester 自己决定是否重发 |

几个刻意的取舍：

- **窗口取 150 ms，不是 UDS 的 P2 = 50 ms。** P2 是节点自身的响应时间指标，
  不含报文往返与节点侧排队。窗口比 P2 宽一档才有意义；再宽就会晚于 tester
  自己的超时，重传等于白发。
- **只重传单帧请求。** 多帧请求的后续帧依赖接收方的流控状态和序号，
  单独重发某一帧会破坏 ISO-TP 会话。这类请求仍纳入监督，但只统计超时不重传。
- **不伪造响应。** 网关不代节点回否定响应，也不替 tester 判超时。网关和 tester
  的超时是两层独立判断，重叠是有意的：网关负责救回「偶发丢失」，tester 负责
  兜底「节点真的没有响应」。
- **收到 `0x78` 之后不再重传。** 节点已经明确表示在处理，此时重发只会让它
  收到重复请求。

统计里 `diag:` 一行给出 `tracked` / `answered` / `timeout` / `retry` /
`pending_ext` 五项。判定逻辑在 `test/diag_timeout_test`（83 项，不需要 vcan）；
转发、重传与 0x78 延长的完整链路用 `make diag-check` 验证：

```bash
sudo ./tools/diag_timeout_check.py   # 需要 vcan0/vcan1 与 root
```

这个脚本只收发帧、不改接口状态，不需要 CAP_NET_ADMIN。

## 速率限制

网关只转发不限速，会把单个节点的故障扩散成整网故障——某个节点失陷后疯狂发报文，
占满总线带宽，其他节点的关键报文就发不出去了。

两级限制：

```ini
[limits]
a = 3000          # 通道总速率（兜底，防未知 ID 洪水）
id a,0x181,60     # 单 ID 速率（精确保护关键周期报文）
```

- 用固定窗口（每秒清零），不是滑动窗口。边界可能有 2 倍突发，但对洪水防护够用，
  且内存恒定。真要平滑限速得上令牌桶。
- 被单 ID 限制拒绝的帧不占通道额度，否则无效帧会吃掉通道预算。
- **诊断帧不受限速影响**：刷写时有大量传输帧，限速会导致升级失败。

实测（0x181 限到 10 帧/秒，实际发送 50 帧/秒）：3 秒 `forwarded=60`、`rate_limited=120`，
目标总线上 2 秒收到 `0x181` 正好 20 帧。

## 掉线自愈

接口被 down、被删或驱动卸载之后，socket 不再产生新帧。此时进程存活、统计不变，
外部只能观察到转发计数停止增长，无法区分「总线安静」与「网关卡死」。

处理分三层：

| 层 | 职责 |
|---|---|
| 通道 | 对读/写失败的 `errno` 分类，判定哪些表示 socket 已废（`ENETDOWN` `ENODEV` `EBADF` `EPERM`） |
| 引擎 | 标记 `need_reopen`，按 1 秒间隔退避重开，成功后重新 `EPOLL_CTL_ADD` |
| 统计 | 每路输出 `reopen=` `fail=`，处于待重开状态时追加 `DOWN` 标记 |

两条实现约束：

**1. `ENOBUFS` 不归入断链**

发送缓冲满表示瞬时过载，链路本身可用。将 `ENOBUFS` 判为致命错误会引入一次
不必要的重开，而重开期间该通道确实收不到帧。

**2. 重开之后必须重新注册 epoll**

新 fd 不继承旧注册。仅做 `close` + `open` 的实现日志正常，但收不到任何帧。

重开的判定与计数在 `test/channel_reconnect_test`（27 项，不需要 vcan）；
真实的 down/up 用 `make reconnect-check` 验证，它检查两件事：
日志出现 `back in service`，以及恢复后再发帧对端能收到。

```bash
sudo ./tools/reconnect_check.py      # 需要 vcan0/vcan1 与 root
```

脚本要 `ip link set down/up`，所以需要 CAP_NET_ADMIN。容器里 uid 可能是 0 但该能力
被收走，此时脚本输出 `SKIP` 并以 0 退出，不会误报成失败。

## 可观测性

统计按 `--stats <秒>` 周期输出：

```
stats: forwarded=360 (diag 0) no_route=0 rate_limited=0 send_failed=0 rewrite_failed=0
  diag: tracked=120 answered=118 timeout=2 retry=2 pending_ext=1
  channel[0] vcan0  rx=360 tx=0   load=1.5% err=0 (bus 0 ctrl 0) reopen=0 fail=0
  channel[1] vcan1  rx=0   tx=360 load=1.5% err=0 (bus 0 ctrl 0) reopen=0 fail=0
```

- **负载率按位时间累加**计算（`47 + 8*dlc` 加约 1/6 位填充），不是按帧数估。
  60 帧/秒 × 129 位 ÷ 500000 = 1.5%，与输出一致。
- **错误帧统计**来自 `CAN_RAW_ERR_FILTER`。不订阅的话总线出问题只能看到「报文变少了」，
  看不到原因；错误帧区分总线错误与控制器错误（含 Bus Off）。

## MQTT 上行

网关把总线上的数据上报到 MQTT。这一步最容易做错的地方不是「能不能发」，
而是**发多少**。

### 为什么必须聚合

`0x181` 是 20ms 周期，单个 ID 就是 50 帧/秒。总线上十几个 ID，
逐帧上报就是每秒几百条、一天四百万条。broker 扛不住，云端存储也扛不住，
而其中绝大多数是重复值。

所以按窗口聚合：

```
窗口内：每帧只更新条目（计数 + 最新数据 + dirty 标记），不做任何 IO
窗口到：只导出 dirty 的条目；长期不变的按 force 间隔强制导出一次
```

两个细节：

- **只在变化时上报**会带来一个副作用：一个恒定 25℃ 的传感器，
  半小时后云端再也没收到过它，看板上那个点会「冻结」。
  所以有 `force` 机制，30 秒没变化也强制发一次。
- **投递失败要保留 dirty**。队列满时如果清了 dirty，这一批数据就永久丢了；
  保留的话下个窗口会重试。

### 为什么需要独立线程

主循环是单线程 epoll，追求确定性延迟。而 MQTT publish 会遇到 TCP 缓冲满、
broker 慢、网络抖动——任何一次阻塞都会直接拖住转发，
把「1ms 的转发延迟」变成「几百毫秒」。

```
主循环 ──入队（非阻塞，满则丢并计数）──> 有界队列 ──> uplink 线程 ──> broker
```

队列深 64，满了就丢。这和「宁可丢帧也不阻塞接收」是同一个判断：
**不能把「网络慢」放大成「转发停摆」**。

### 采集与转发解耦

采集发生在路由匹配之前，所以**没有路由规则的帧（no_route）也会被上报**。
总线上有些帧本来就是给远端 ECU 看的，网关不需要转发，
但云端仍然想知道它还在发。旁路监测的价值不该被路由表限制。

### 上报格式

```json
{"gw":"can-gateway","ch":"vcan0","id":"0x180","n":10,"dlc":8,"d":"0100000002000000","ts":1234567}
```

`n` 是窗口内的帧数，`d` 是原始 8 字节 hex。

**网关不做 DBC 解析**，只上报原始帧 + 计数。原因很实际：
DBC 在 can-node 仓库维护，网关内嵌一份会变成两份互相漂移的协议定义；
而原始帧体积小、无损，语义解析交给云端做——它本来就有 DBC。

### 配置

```ini
[mqtt]
addr = tcp://127.0.0.1:1883
topic = gw/can
client_id = can-gateway-1
window = 1000            # 聚合窗口（毫秒）
channels = a,b           # 只采集这两路；留空表示全部
```

`channels` 里写了不存在的通道名会在启动时直接失败，不静默忽略——
否则会出现「明明配了采集却一条数据都没有」。

### 验证

```bash
# 需要 vcan0/vcan1 和运行中的 broker
make uplink-check
```

验证的不是「MQTT 能发」，而是三件事：

| 断言 | 为什么重要 |
|---|---|
| `periodic id aggregated, not per-frame` | 0x180 发 50 帧只应产生 1 条消息。逐帧上报也能「收到消息」，但那是灾难 |
| `frame count covers the window` | 消息里的 `n` 要对得上窗口内实际帧数 |
| `payload changes detected` | 变化的值必须被捕捉到，否则聚合就成了丢数据 |

## 端到端验证

与 [can-node](https://github.com/Dik-910/can-node) 串联实测（vcan0 ↔ vcan1）：

| 场景 | 结果 |
|---|---|
| 周期报文转发 | can-node 发 `0x180`/`0x181` → 网关 → vcan1 收到 **50 / 251 帧**（5 秒，对应 100ms 与 20ms 周期），帧内容逐字节一致 |
| 诊断链路 | udsoncan 从 vcan0 发请求 → 网关 → can-node（监听 vcan1）→ 响应原路返回，**14 项断言全部通过** |

诊断链路的响应包含 17 字节 VIN（需要 ISO-TP 多帧），**经网关双向逐帧转发后仍能被
正确重组**——说明转发不会打乱连续帧的序号序列，也不会在转发过程中丢帧。

复现方式：

```bash
# 终端 1：节点监听 vcan1
./can-node/ecu-server --if vcan1 --seconds 40

# 终端 2：网关
./can-gateway/gateway

# 终端 3：tester 从 vcan0 发请求
python3 can-node/tools/udsoncan_interop.py
```

## 构建与验证

```bash
make test           # 单元测试，不依赖 CAN 接口
make uplink-check   # MQTT 上行端到端（需要 vcan + broker）
make system-check   # 跨仓库：can-node -> 本网关 -> MQTT（需要 ../can-node 已编译）
make reconnect-check # 接口掉线自愈（需要 root + vcan0/vcan1）
make diag-check     # 诊断超时与重传（需要 root + vcan0/vcan1）
```

`make system-check` 验证的不只是「都能跑」，而是三个仓库合起来成立：

| 断言 | 证明什么 |
|---|---|
| 上报帧能被 can-node 的 DBC 解开且字段合理 | 两端遵守同一份协议定义 |
| UDS 读 VIN 经网关转发成功 | 转发不改写、不丢帧、不打乱 ISO-TP 帧序 |
| **诊断期间周期数据上报的最长间隔 < 3 个窗口** | 诊断突发没有把采集卡死 |

系统拓扑、仓库边界与耦合点见 [docs/system.md](docs/system.md)。

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
- [bms-node](https://github.com/Dik-910/bms-node) —— 储能 BMS 节点，协议栈复制自 can-node
- [mcu-gateway](https://github.com/Dik-910/mcu-gateway) —— FreeRTOS 版多协议网关。
  **不在 CAN 线上**，是同一个问题在另一种约束（吞吐优先、资源受限）下的不同取舍，
  对比见 [docs/system.md](docs/system.md) 第 4 节
- [smartgateway](https://github.com/Dik-910/smartgateway) —— 前身，Linux IoT 网关

## 许可证

木兰宽松许可证，第 2 版。见 [LICENSE](LICENSE)。
`thirdparty/log` 为 rxi/log，遵循其 MIT 许可，版权声明保留在源文件中。
