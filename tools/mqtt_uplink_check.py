#!/usr/bin/env python3
"""
mqtt_uplink_check.py —— can-gateway 上行采集的端到端验证

验证的不是「MQTT 能发」，而是三件容易做错的事：

  1. 周期性帧被聚合：0x180 每 100ms 一帧，4 秒 40 帧，
     但只应该产生 1 条 MQTT 消息（首次出现）。逐帧上报就是灾难。
  2. 变化被捕捉：0x200 的数据每秒改一次，应该产生多条消息。
  3. 帧计数对得上：消息里的 n 应该接近窗口内实际收到的帧数。

第 1 条是关键。如果实现退化成逐帧上报，这里会收到几十条，
而不是一条 —— 而「能收到消息」这个断言照样通过。

用法（需要 vcan0 与运行中的 gateway）：
    python3 tools/mqtt_uplink_check.py [--if vcan0] [--seconds 5]
"""
import argparse
import json
import os
import sys
import threading
import time

for _cand in (os.environ.get("MG_PYLIB"), "/home/dikl/work/.pylibs"):
    if _cand and os.path.isdir(_cand):
        sys.path.insert(0, _cand)
        break

import can            # noqa: E402
import paho.mqtt.client as mqtt  # noqa: E402

TOPIC = "gw/can"

MSGS = []
BAD = []
PASS, FAIL = [], []


def check(cond, what, detail=""):
    # 不要写 (cond and PASS or FAIL)：PASS 是空列表时是 falsy，
    # 结果永远落到 FAIL，全部断言都被记成失败。
    if cond:
        PASS.append(what)
    else:
        FAIL.append(what)
    print(f"  [{'PASS' if cond else 'FAIL'}] {what}" + (f"  ({detail})" if detail else ""))


def on_message(cl, userdata, msg):
    raw = msg.payload.decode("utf-8", "replace")
    try:
        obj = json.loads(raw)
        if not isinstance(obj, dict):
            raise ValueError("not an object")
        MSGS.append(obj)
    except Exception as e:
        BAD.append((str(e), raw[:100]))


def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--if", dest="ifname", default="vcan0")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--window", type=int, default=1000, help="gateway 的聚合窗口(ms)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    args = ap.parse_args()

    print("=== can-gateway MQTT uplink check ===")

    # --- 先订阅，避免漏掉开始的几条 ---
    cli = make_client("uplink-check")
    cli.on_message = on_message
    try:
        cli.connect(args.host, args.port, 15)
    except Exception as e:
        print(f"  [FAIL] cannot connect broker: {e}")
        return 2
    cli.loop_start()
    cli.subscribe(TOPIC, 1)
    time.sleep(0.3)

    # --- 往 CAN 上发帧 ---
    try:
        bus = can.interface.Bus(channel=args.ifname, interface="socketcan")
    except Exception as e:
        print(f"  [FAIL] cannot open {args.ifname}: {e}")
        print("  提示：sudo modprobe vcan && sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0")
        cli.loop_stop()
        return 2

    print(f"  sending on {args.ifname} for {args.seconds}s")
    print("    0x180  每 100ms，数据固定   -> 期望只上报 1 次")
    print("    0x200  每 1s，数据变化      -> 期望每次变化都上报")

    t0 = time.monotonic()
    next_periodic = t0
    next_change = t0
    counter = 0

    while time.monotonic() - t0 < args.seconds:
        now = time.monotonic()

        if now >= next_periodic:
            bus.send(can.Message(arbitration_id=0x180, data=[1, 0, 0, 0, 2, 0, 0, 0],
                                 is_extended_id=False))
            next_periodic += 0.1

        if now >= next_change:
            counter += 1
            bus.send(can.Message(arbitration_id=0x200,
                                 data=[counter & 0xFF, 0x13, 0x2E, 0xFB, 0, 0, 0, 0],
                                 is_extended_id=False))
            next_change += 1.0

        time.sleep(0.005)

    # 等到窗口闭合（gateway 默认 1000ms）
    time.sleep(1.5)
    bus.shutdown()
    cli.loop_stop()
    cli.disconnect()

    # --- 断言 ---
    print("\nAssertions")
    check(len(BAD) == 0, "all payloads are valid json objects",
          f"{len(BAD)} bad" if BAD else "")

    m180 = [m for m in MSGS if m.get("id") == "0x180"]
    m200 = [m for m in MSGS if m.get("id") == "0x200"]

    check(len(m180) >= 1, "periodic id 0x180 reported", f"{len(m180)} msg")
    check(len(m200) >= 1, "changing id 0x200 reported", f"{len(m200)} msg")

    # 核心断言：周期帧不能被逐帧上报
    check(len(m180) <= 2,
          "periodic id aggregated, not per-frame",
          f"{len(m180)} msgs for ~{int(args.seconds * 10)} frames")

    if m180:
        m = m180[0]
        # n 是「单个聚合窗口内的帧数」，不是整轮测试的总帧数。
        # 0x180 每 100ms 一帧 -> 1 秒窗口约 10 帧。
        expect = max(1, int(args.window / 100))
        check(m.get("n", 0) >= expect * 0.6,
              "frame count covers one window",
              f"n={m.get('n')} vs ~{expect} frames per window")
        check(m.get("ch") == args.ifname or m.get("ch", "").startswith("vcan"),
              "channel name carried", f"ch={m.get('ch')}")
        check(isinstance(m.get("d"), str) and len(m["d"]) == 16,
              "payload hex is 8 bytes", f"d={m.get('d')}")

    # 变化要被捕捉到
    vals = {m.get("d", "")[:2] for m in m200}
    check(len(vals) >= 2, "payload changes detected", f"{len(vals)} distinct values")

    print(f"\nResult: {len(PASS)} passed, {len(FAIL)} failed")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
