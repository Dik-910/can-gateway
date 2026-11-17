# 系统视角：仓库间的依赖与验证

各仓库的 README 描述自身功能。本文说明仓库之间的接口依赖、协议定义的归属，以及跨仓链路如何验证。

依赖关系以可验证为准：拆除一侧后另一侧失效，才算存在耦合。

---

## 1. 仓库分工

| 仓库 | 平台 | 职责 | 与其他仓库的关系 |
|---|---|---|---|
| **can-node** | STM32（裸机） | CAN 节点：ISO-TP、UDS、Bootloader | 提供 DBC；协议栈被 bms-node 复制 |
| **bms-node** | STM32（裸机） | 储能 BMS 节点：采样、SOC、保护 | 复制 can-node 的协议栈，共用同一份 DBC |
| **can-gateway** | Linux 用户态 | 多路 CAN 路由、诊断会话映射、限速、MQTT 上云 | 转发 can-node 的帧；用 can-node 的 DBC 校验上报内容 |
| **mcu-gateway** | FreeRTOS（MCU） | Modbus/RS485 接入、MQTT 上云 | 不在 CAN 链路上，见第 4 节 |

前三者存在协议层的实际依赖。mcu-gateway 属于另一条产品线，与 CAN 链路没有物理连接。

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

诊断与采集走同一张网卡、同一个 epoll 循环：

- **诊断**：tester 的请求从 `vcan0` 进入，按会话映射改写 ID 后从 `vcan1` 发出；响应原路返回
- **采集**：`vcan1` 上的周期帧不经过路由规则，采集发生在路由匹配之前，聚合后进入 MQTT

两条路径对延迟的要求不同：诊断转发的延迟需可控，上报允许尽力而为。网关在同一事件循环中处理两者，互不阻塞。

---

## 3. 三处跨仓依赖

### 3.1 协议定义单一来源

`can_matrix.dbc` 只存在于 `can-node/docs/` 下。其他仓库的处理方式：

| 位置 | 做法 | 理由 |
|---|---|---|
| bms-node | 复制 DBC 生成的代码，在 `src/PROTOCOL_STACK.md` 记录来源 commit | 两仓库可独立构建，代价是需要同步 |
| can-gateway | 不内嵌，测试时用 `--dbc` 指向 can-node 的那份 | 内嵌会产生两份可能漂移的定义 |

网关不做 DBC 解析：上报内容为原始 `id + dlc + hex`，信号语义由云端解析（云端持有 DBC）。网关因此不依赖信号含义，新增报文无需修改网关代码。

不做解析不等于不校验。`tools/system_check.py` 使用 can-node 的 DBC 解析网关上报的 hex，校验字段值范围（状态在 0..3、运行时长单调递增），以此检查两端是否遵守同一份协议。

### 3.2 协议栈复制与同步点

bms-node 的 `src/can/`、`src/net/`、`src/uds/` 从 can-node 复制，不使用 submodule。复制的目的是使两个仓库都能独立构建、独立运行 CI。

代价是协议栈的修改需要在两处落地，因此约定三个同步点：

- `bms-node/src/PROTOCOL_STACK.md` 记录来源 commit
- `bms-node/tools/sync_stack.sh` 一条命令完成同步并更新 commit 号
- 同步后必须重跑 `make test`

该代价在实际开发中发生过：can-node 修复 ISO-TP 超时问题后，bms-node 的副本落后于主干，直至下一次同步。

### 3.3 端到端链路的验证

`make system-check` 启动节点、网关与订阅端，执行以下断言：

| 断言 | 验证内容 |
|---|---|
| 上报的 `0x180` 能被 can-node 的 DBC 解析，且字段在合理范围 | 两端协议一致 |
| `NodeState ∈ 0..3`、`UptimeS` 单调递增 | 上报内容与节点运行状态一致 |
| UDS 读 VIN 成功且内容正确 | 网关转发不改写、不丢帧、不打乱 ISO-TP 帧序 |
| 诊断期间 `0x181` 上报的最长间隔 < 3 个窗口 | 诊断流量不影响周期采集 |
| 网关 `send_failed=0`、`rate_limited=0` | 转发链路未过载，限速未误触发 |

第四项覆盖并发场景：若诊断请求导致采集停滞，间隔将升至秒级，断言失败。

---

## 4. mcu-gateway 与 CAN 链路的边界

mcu-gateway 运行在 MCU 上，接入 Modbus/RS485 与 MQTT，与 CAN 总线没有物理连接，因此不在上述拓扑中。

它与 can-gateway 解决的是同类问题在不同约束下的两个实现：

| | can-gateway | mcu-gateway |
|---|---|---|
| 首要目标 | 确定性延迟 | 吞吐 + 低资源占用 |
| 并发模型 | 单线程 epoll | FreeRTOS 任务 + 队列 |
| 请求-响应 | 诊断会话映射（按 ID 定位） | 会话状态机（按从机+功能码匹配） |
| 丢帧容忍 | 诊断不能丢，遥测可丢 | 轮询周期内可重试 |
| 内存 | 不受限 | 静态分配，队列定深满则丢 |

两者的取舍差异集中在延迟与资源占用，不共享代码。

---

## 5. 运行方式

```bash
# 1) vcan（运行时接口，每次开机需重建）
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

仅观察上行数据：

```bash
mosquitto_sub -t gw/can -v
```

---

## 6. 已知边界

- 节点与网关运行在同一台机器的两个 vcan 接口上，不是两块独立板卡。CAN 层行为由内核子系统提供，但 vcan 没有位时间，仲裁延迟不可测。总线时序数据见 `can-node/examples/bus_load_test.c`，该程序按位时间计算，与实测存在差异
- bms-node 的协议栈副本由 `sync_stack.sh` 维护，不保证随时与 can-node 主干一致
- 网关只实现 11 位标准帧，未支持扩展帧与 CAN FD
- 诊断会话映射只验证过单个 tester。多 tester 并发时的 ID 冲突检测有单元测试覆盖，但没有真实并发场景的端到端验证
