# 系统视角：这几个仓库怎么组成一条链

单个仓库的 README 讲的是「它自己做什么」。这份文档讲另一件事：
**它们凭什么算一个系统，而不是四个各写各的项目。**

判断标准不是「代码风格一致」或「目录结构像」——那些是表面。
真正的耦合必须能被验证：拆掉一边，另一边会坏。

---

## 1. 谁和谁在同一条线上

| 仓库 | 平台 | 职责 | 与谁有关系 |
|---|---|---|---|
| **can-node** | STM32（裸机） | CAN 节点：ISO-TP、UDS、Bootloader | 提供 DBC；协议栈被 bms-node 复制 |
| **bms-node** | STM32（裸机） | 储能 BMS 节点：采样、SOC、保护 | 复制 can-node 的协议栈，共用同一份 DBC |
| **can-gateway** | Linux 用户态 | 多路 CAN 路由、诊断会话映射、限速、MQTT 上云 | 转发 can-node 的帧；用 can-node 的 DBC 校验上报内容 |
| **mcu-gateway** | FreeRTOS（MCU） | Modbus/RS485 接入、MQTT 上云 | **不在 CAN 线上**，见第 4 节 |

前三个存在真实的物理/协议关系。第四个是另一条产品线，硬连只会让两边的设计都变脏。

---

## 2. 拓扑

```
              ┌──────────────┐
   tester ────┤   vcan0      │
 (udsoncan)   └──────┬───────┘
                     │ 0x7E0 / 0x7E8
              ┌──────▼───────────────────────────────┐
              │  can-gateway (Linux)                 │
              │  路由表 / 诊断会话映射 / 限速          │
              │  旁路采集 → 聚合 → MQTT               │
              └──────┬───────────────────┬───────────┘
                     │ vcan1             │ MQTT
                     │ 0x7E0/0x7E8       │ gw/can
              ┌──────▼──────────┐  ┌─────▼──────────┐
              │  can-node        │  │  mosquitto     │
              │  ISO-TP / UDS    │  │  (or cloud)    │
              │  周期 0x180/0x181│  └────────────────┘
              └──────────────────┘
```

两个方向的数据走的是**同一张网卡、同一个 epoll 循环**：

- **诊断**：tester 的请求从 `vcan0` 进来，按会话映射改写 ID 后从 `vcan1` 出去；响应原路返回
- **采集**：`vcan1` 上的周期帧不需要路由规则，采集发生在路由匹配之前，聚合后进 MQTT

这正是网关的价值所在：**它同时承担确定性延迟的转发和尽力而为的上报。**
如果只能做其中一件，那它要么是路由器，要么是数据采集器。

---

## 3. 三处真实的耦合点

### 3.1 协议定义只有一个来源

`can_matrix.dbc` 只存在 `can-node/docs/` 下。其他仓库的做法：

| 位置 | 做法 | 理由 |
|---|---|---|
| bms-node | **复制** DBC 生成代码，并在 `src/PROTOCOL_STACK.md` 记录来源 commit | 两仓库都能独立构建，代价是要同步 |
| can-gateway | **不内嵌**，测试时用 `--dbc` 指向 can-node 的那份 | 内嵌一份会变成两份互相漂移的定义 |

网关刻意**不做 DBC 解析**：它上报原始 `id + dlc + hex`，语义解析交给云端（云端本来就有 DBC）。
这样网关不需要知道信号含义，加一个新报文不用改网关代码。

但「不做解析」不等于「不校验」。`tools/system_check.py` 用 can-node 的 DBC 去解网关上报的 hex，
**字段值必须合理**（状态在 0..3、运行时长单调递增）。这是对「两端遵守同一份协议」的实际检验。

### 3.2 协议栈复制 + 明确的同步点

bms-node 的 `src/can/`、`src/net/`、`src/uds/` 是从 can-node 复制的（不是 submodule）。
复制是有意的：两个仓库各自能 `make`、跑 CI、单独发给别人看。

代价是协议栈修 bug 要同步两处。所以：

- `bms-node/src/PROTOCOL_STACK.md` 记录来源 commit
- `bms-node/tools/sync_stack.sh` 一条命令同步并更新 commit 号
- 同步后必须重跑 `make test` —— 协议栈是地基

**这个代价是真实存在的，不是纸面约定。** can-node 修 ISO-TP 超时那次，bms-node 的副本就落后了，
CHANGELOG 里专门记了待同步。

### 3.3 端到端链路可以被一条命令验证

`make system-check` 拉起节点、网关、订阅端，然后断言：

| 断言 | 证明什么 |
|---|---|
| 上报的 `0x180` 能被 can-node 的 DBC 解开，且字段合理 | 两端协议一致 |
| `NodeState ∈ 0..3`、`UptimeS` 单调递增 | 上报的不是随机字节，是真的节点状态 |
| UDS 读 VIN 成功且内容正确 | 网关转发不改写、不丢帧、不打乱 ISO-TP 帧序 |
| **诊断期间 `0x181` 上报的最长间隔 < 3 个窗口** | 诊断突发没有把采集卡死 |
| 网关 `send_failed=0`、`rate_limited=0` | 转发链路没有过载或误限速 |

第四条是这份测试里最关键的。它把「网关同时做两件事」这个说法变成了可测的命题：
如果诊断一来采集就停，间隔会拉长到秒级，断言立刻红。

---

## 4. mcu-gateway 为什么不在这条线上

它跑在 MCU 上，接的是 Modbus/RS485 和 MQTT，与 CAN 总线没有物理关系。
把它画进上面那张图，等于编一个不存在的连接。

那它和这条线是什么关系？**是同一个问题在另一种约束下的另一个答案。**

| | can-gateway | mcu-gateway |
|---|---|---|
| 首要目标 | 确定性延迟 | 吞吐 + 低资源占用 |
| 并发模型 | 单线程 epoll | FreeRTOS 任务 + 队列 |
| 请求-响应 | 诊断会话映射（按 ID 定位） | 会话状态机（按从机+功能码匹配） |
| 丢帧容忍 | 诊断不能丢，遥测可丢 | 轮询周期内可重试 |
| 内存 | 不受限 | 静态分配，队列定深满则丢 |

两者都把「网关」这件事做了一遍，但取舍完全不同。**这个对比本身是设计判断的证明**，
比把它们拼成一张图有用——拼起来只能说明「我做过两个网关」。

---

## 5. 怎么跑

```bash
# 1) vcan（重启即消失，每次开机都要建）
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link add dev vcan1 type vcan
sudo ip link set up vcan0 && sudo ip link set up vcan1

# 2) broker
mosquitto -p 1883 -d          # 或 python3 tools/run_broker.py

# 3) 节点
cd ../can-node && make node

# 4) 系统联调
cd ../can-gateway && make system-check
```

只看上行数据流：

```bash
mosquitto_sub -t gw/can -v
```

---

## 6. 已知边界

- **节点跑在同一台机器上**（两个 vcan 接口），不是两块真实板子。CAN 层行为真实（走内核子系统），
  但时序是虚拟的——vcan 没有位时间，测不出真实仲裁延迟。总线时序数字见
  `can-node/examples/bus_load_test.c`，那是按位时间算的，和实测是两回事
- **bms-node 的协议栈副本落后于 can-node**，同步方式见 3.2
- **网关只做 11 位标准帧**，扩展帧和 CAN FD 没实现
- **诊断会话映射只验证过单个 tester**。多 tester 并发时的 ID 冲突检测有单元测试，
  但没有真实并发场景的端到端验证
