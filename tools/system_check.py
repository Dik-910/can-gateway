#!/usr/bin/env python3
"""
system_check.py —— can-node + can-gateway 的跨仓库端到端联调

这个脚本回答一个问题：**两个仓库合起来，是不是真的组成了一条系统。**

验证的不只是「都能单独跑」，而是三件事同时成立：

  1. 周期数据上云，且上报的原始帧能被 can-node 的 DBC 正确解包
     —— 证明两端遵守同一份协议定义，不是各写各的
  2. UDS 诊断请求经网关转发到节点，响应原路返回
     —— 证明网关的转发不改写、不丢帧、不打乱 ISO-TP 帧序
  3. 诊断传输期间，周期数据的上报不中断
     —— 证明网关「同时做转发和采集」这件事成立

第 3 条是重点。网关的价值就在于同时承担两种流量：
如果诊断一来采集就停，那它只是个转发器，不是网关。

用法：
    # 1) 建 vcan（重启后消失）
    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan
    sudo ip link add dev vcan1 type vcan
    sudo ip link set up vcan0 && sudo ip link set up vcan1

    # 2) 起 broker（mosquitto 或 tools/run_broker.py）

    # 3) 跑
    make system-check
    # 或
    python3 tools/system_check.py --node-bin ../can-node/node
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time

# 本地开发时依赖可能装在独立目录；CI 用系统包，走默认 import
for _cand in (os.environ.get("MG_PYLIB"), "/home/dikl/work/.pylibs"):
    if _cand and os.path.isdir(_cand):
        sys.path.insert(0, _cand)
        break

import can
import cantools
import isotp
import paho.mqtt.client as mqtt
import udsoncan
from udsoncan.client import Client
from udsoncan.connections import PythonIsoTpConnection
import udsoncan.services as uds  # noqa: F401  (保留给后续扩展)

passed = 0
failed = 0


def check(cond, what, detail=""):
    global passed, failed
    if cond:
        print(f"  [PASS] {what}" + (f"  ({detail})" if detail else ""))
        passed += 1
    else:
        print(f"  [FAIL] {what}" + (f"  ({detail})" if detail else ""))
        failed += 1


# ---------------------------------------------------------------- 环境检查

def vcan_ready(names):
    """两个接口都得存在且 UP，否则后面全是误导性的失败"""
    try:
        out = subprocess.run(["ip", "-br", "link", "show", "type", "vcan"],
                             capture_output=True, text=True, timeout=5).stdout
    except Exception as e:
        print(f"cannot list vcan: {e}")
        return False, ""

    missing = [n for n in names if f"{n}" not in out]
    return (not missing), out


def broker_ready(host="127.0.0.1", port=1883):
    import socket

    try:
        socket.create_connection((host, port), timeout=2).close()
        return True
    except Exception:
        return False


# ---------------------------------------------------------------- MQTT 采集

class Collector:
    """收上行消息。paho 的回调在自己的线程里跑，用锁保护列表。"""

    def __init__(self, topic):
        self.topic = topic
        self.msgs = []
        self.lock = threading.Lock()
        self.connected = False
        self.client = mqtt.Client(client_id="syscheck-sub")

    def _on_connect(self, c, u, flags, rc):
        self.connected = (rc == 0)
        c.subscribe(self.topic, qos=0)

    def _on_message(self, c, u, msg):
        try:
            d = json.loads(msg.payload.decode())
        except Exception:
            return

        # 本地到达时间，用来判断「上行的间隔」。
        #
        # 不能用消息里的 ts：那是网关记录的「最后一条帧的采集时间」。
        # 数据不变时它不更新，两条 force 导出的消息会带着同一个 ts，
        # 算出来的间隔恒为 0 —— 断言永远通过，等于没测。
        # 我们要测的是「云端多久看到一次」，那就必须用云端的时钟。
        d["_rx"] = time.time()

        with self.lock:
            self.msgs.append(d)

    def start(self):
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.connect("127.0.0.1", 1883, 10)
        self.client.loop_start()

    def stop(self):
        self.client.loop_stop()
        try:
            self.client.disconnect()
        except Exception:
            pass

    def snapshot(self):
        with self.lock:
            return list(self.msgs)


# ---------------------------------------------------------------- 进程管理

def spawn(cmd, log_path):
    f = open(log_path, "w")
    p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT)
    p._log = f  # 保住文件句柄，进程退出时再关
    return p


def stop(p, sig=signal.SIGINT, wait=3.0):
    if p is None or p.poll() is not None:
        return
    try:
        p.send_signal(sig)
        p.wait(timeout=wait)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait(timeout=2)
    except Exception:
        pass
    try:
        p._log.close()
    except Exception:
        pass


# ---------------------------------------------------------------- UDS 探针

def uds_read_vin(ifname, timeout=5.0):
    """
    从 tester 侧读 VIN（17 字节，必须走 ISO-TP 多帧）。
    返回 (ok, detail)。
    """
    bus = can.interface.Bus(channel=ifname, interface="socketcan")
    try:
        addr = isotp.Address(isotp.AddressingMode.Normal_11bits,
                             txid=0x7E0, rxid=0x7E8)
        stack = isotp.CanStack(bus=bus, address=addr,
                               params={"blocksize": 8, "stmin": 5})
        conn = PythonIsoTpConnection(stack)

        config = dict(udsoncan.configs.default_client_config)
        config["data_identifiers"] = {0xF190: "17s"}

        with Client(conn, request_timeout=timeout, config=config) as client:
            r = client.read_data_by_identifier([0xF190])

            if not r.positive:
                return False, f"negative response {r.code_name}"

            v = r.service_data.values[0xF190]
            if isinstance(v, tuple):
                v = v[0]

            return True, v.decode("ascii", "replace")
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"
    finally:
        try:
            bus.shutdown()
        except Exception:
            pass


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--node-bin", default="../can-node/node",
                    help="can-node 编译出的 node 可执行文件")
    ap.add_argument("--dbc", default="../can-node/docs/can_matrix.dbc",
                    help="协议定义。必须来自 can-node —— 那是唯一来源")
    ap.add_argument("--gateway", default="./gateway")
    ap.add_argument("--config", default="examples/system.conf")
    ap.add_argument("--node-if", default="vcan1")
    ap.add_argument("--tester-if", default="vcan0")
    ap.add_argument("--topic", default="gw/can")
    ap.add_argument("--window", type=int, default=500, help="网关聚合窗口(ms)")
    a = ap.parse_args()

    for p in (a.node_bin, a.dbc):
        if not os.path.exists(p):
            print(f"missing: {p}")
            print("先编译 can-node：cd ../can-node && make node")
            return 1

    ok, listing = vcan_ready([a.node_if, a.tester_if])
    if not ok:
        print("vcan not ready. run:")
        print("  sudo modprobe vcan")
        print(f"  sudo ip link add dev {a.tester_if} type vcan")
        print(f"  sudo ip link add dev {a.node_if} type vcan")
        print(f"  sudo ip link set up {a.tester_if} && sudo ip link set up {a.node_if}")
        print(f"\ncurrent vcan:\n{listing}")
        return 1

    if not broker_ready():
        print("no MQTT broker on 127.0.0.1:1883")
        print("  mosquitto 或 python3 tools/run_broker.py")
        return 1

    db = cantools.database.load_file(a.dbc)

    print("=== system check: can-node -> can-gateway -> MQTT ===")
    print(f"protocol: {a.dbc}")
    print(f"node on {a.node_if}, tester on {a.tester_if}, topic {a.topic}\n")

    os.makedirs(".system", exist_ok=True)
    collector = Collector(a.topic)
    node = gw = None

    try:
        # 先订阅再启动设备。这是真实部署顺序：云端先在线，设备后上线。
        #
        # 反过来（先起设备再订阅）会漏掉首批消息，而且漏掉的恰好可能是
        # 「数据恒定的 ID 的第一次上报」—— 那个 ID 之后要等 force 间隔
        # 才会再发一次，于是测试会误判成「这个 ID 没上报」。
        collector.start()
        time.sleep(0.8)

        # 节点跑 14 秒，留足诊断与采集时间
        node = spawn([a.node_bin, "--if", a.node_if, "--seconds", "14"],
                     ".system/node.log")
        time.sleep(0.4)
        gw = spawn([a.gateway, "--config", a.config, "--stats", "4"],
                   ".system/gateway.log")

        print("Phase A: periodic data only (2s)")
        time.sleep(2.0)
        before = collector.snapshot()

        print("Phase B: UDS read VIN through the gateway")
        t_diag = time.time()
        uds_ok, uds_detail = uds_read_vin(a.tester_if)
        diag_cost = time.time() - t_diag
        time.sleep(2.0)
        after = collector.snapshot()

        print(f"  diagnostic took {diag_cost*1000:.0f} ms\n")

        all_msgs = after

        # ---------------- 断言 1：上行有数据 ----------------
        print("Uplink")
        check(len(all_msgs) > 0, "messages reached MQTT", f"{len(all_msgs)} msgs")

        by_id = {}
        for m in all_msgs:
            try:
                cid = int(m.get("id", "0"), 16) if isinstance(m.get("id"), str) else int(m.get("id", 0))
            except ValueError:
                continue
            by_id.setdefault(cid, []).append(m)

        check(0x180 in by_id, "0x180 (NodeStatus) reported",
              f"{len(by_id.get(0x180, []))} msgs")
        check(0x181 in by_id, "0x181 (BusStatus) reported",
              f"{len(by_id.get(0x181, []))} msgs")

        # ---------------- 断言 2：DBC 解包一致 ----------------
        print("\nProtocol conformance (decode with can-node's DBC)")
        st = db.get_message_by_name("NodeStatus")

        decoded_states = []
        decode_errors = 0

        for m in by_id.get(0x180, []):
            try:
                raw = bytes.fromhex(m["d"])
                dd = st.decode(raw, decode_choices=False, allow_truncated=True)
                decoded_states.append(dd)
            except Exception:
                decode_errors += 1

        check(decode_errors == 0, "every 0x180 payload decodes",
              f"{len(decoded_states)} decoded, {decode_errors} errors")

        if decoded_states:
            states = [d.get("NodeState", -1) for d in decoded_states]
            uptimes = [d.get("UptimeS", 0) for d in decoded_states]

            check(all(0 <= s <= 3 for s in states),
                  "NodeState within 0..3", f"values {sorted(set(states))}")
            check(uptimes == sorted(uptimes),
                  "UptimeS is non-decreasing", f"{uptimes[0]} -> {uptimes[-1]}")
            # 节点确实在跑：末值应明显大于 0
            check(uptimes[-1] > 0, "UptimeS advanced", f"{uptimes[-1]} s")

        bs = db.get_message_by_name("BusStatus")
        loads = []
        for m in by_id.get(0x181, []):
            try:
                dd = bs.decode(bytes.fromhex(m["d"]), decode_choices=False,
                               allow_truncated=True)
                loads.append(dd.get("BusLoadPermille", -1))
            except Exception:
                pass

        if loads:
            # 注意：这里只验证字段落在合法区间，不验证数值正确。
            # 节点侧目前没有实现负载率测量，该字段恒为 0。
            # 真正的负载率由网关自己按位时间算（见 stats 里的 load=）。
            check(all(0 <= x <= 1000 for x in loads),
                  "BusLoadPermille in valid range",
                  f"{sorted(set(loads))[:5]} (node does not measure it yet)")

        # ---------------- 断言 3：诊断经网关 ----------------
        print("\nDiagnostic through the gateway")
        check(uds_ok, "UDS read VIN succeeded", uds_detail)

        if uds_ok:
            check(uds_detail == "LSVAA1234567890AB",
                  "VIN matches node's value", uds_detail)

        # ---------------- 断言 4：诊断期间采集不中断 ----------------
        print("\nCollection survives diagnostic burst")
        # 用 0x180 测连续性，不用 0x181。
        #
        # 0x180 的 UptimeS 每 100ms 变一次，所以每个窗口都必然上报，
        # 间隔稳定在一个窗口 —— 任何卡顿都会立刻显现。
        # 0x181 的数据恒定，本来就要靠 force 间隔才上报（本例 2 秒），
        # 拿它算间隔会把「正常的 force 周期」误判成「被卡住了」。
        rx_180 = sorted(m.get("_rx", 0.0) for m in by_id.get(0x180, []))

        if len(rx_180) >= 2:
            gaps_ms = [(b - a) * 1000.0 for a, b in zip(rx_180, rx_180[1:])]
            worst = max(gaps_ms)

            # 一个窗口一条；允许偶尔错过一个窗口，超过 3 个说明真的被卡住了
            limit = a.window * 3
            check(worst <= limit,
                  "no long gap in 0x180 uplink",
                  f"worst gap {worst:.0f} ms, limit {limit} ms")
        else:
            check(False, "not enough 0x181 samples to judge continuity",
                  f"{len(ts_181)} samples")

        # 诊断前后都要有数据，不能是「诊断一来就停了」
        n_before = sum(1 for m in before if m.get("id") == "0x181")
        n_after = sum(1 for m in after if m.get("id") == "0x181")
        check(n_after > n_before,
              "uplink kept growing after the diagnostic",
              f"{n_before} -> {n_after}")

        # ---------------- 断言 5：网关没有丢帧/限速 ----------------
        print("\nGateway health")

    except Exception as e:
        # 不让异常直接崩掉：清理和统计断言还要跑，否则排查时连日志都拿不到
        print(f"\n  !! unexpected error: {type(e).__name__}: {e}")
        check(False, "no unexpected exception")
    finally:
        collector.stop()

        if gw is not None:
            stop(gw, signal.SIGINT, wait=3.0)
        if node is not None:
            stop(node, signal.SIGTERM, wait=3.0)

        # 统计要在进程退出后读，因为 SIGINT 时才打印最终值
        try:
            log = open(".system/gateway.log", encoding="utf-8", errors="replace").read()
        except Exception:
            log = ""

        stats = [ln for ln in log.splitlines() if "stats:" in ln]
        if stats:
            last = stats[-1]
            check("send_failed=0" in last, "no send failures", last.split("stats:")[-1].strip()[:80])
            rate_zero = "rate_limited=0" in last
            check(rate_zero, "no rate limiting during the test",
                  "" if rate_zero else last.split("stats:")[-1].strip()[:80])
        else:
            check(False, "gateway printed stats", "no stats line in log")

        try:
            nodelog = open(".system/node.log", encoding="utf-8", errors="replace").read()
        except Exception:
            nodelog = ""

        # 节点侧：诊断请求确实到达，且没有 ISO-TP 超时
        if "diag requests" in nodelog:
            reqs = 0
            for ln in nodelog.splitlines():
                if "diag requests" in ln:
                    try:
                        reqs = int(ln.split(":")[-1].strip())
                    except ValueError:
                        pass
            check(reqs > 0, "node saw the forwarded request", f"{reqs} requests")

        if "isotp tx timeouts" in nodelog:
            tmo = 0
            for ln in nodelog.splitlines():
                if "isotp tx timeouts" in ln:
                    try:
                        tmo = int(ln.split(":")[-1].strip())
                    except ValueError:
                        pass
            check(tmo == 0, "no ISO-TP timeout on the node", f"{tmo} timeouts")

    print(f"\nResult: {passed} passed, {failed} failed")
    if failed:
        print("logs: .system/node.log  .system/gateway.log")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
