#!/usr/bin/env python3
"""
diag_timeout_check.py —— 诊断请求的会话监督

单元测试验证的是判定逻辑，但「转发之后有没有真的登记」「到期有没有真的重发」
只能在接口上看。三段验证：

  A. 请求改写并转发，响应回来后统计为 answered，不重传
  B. 节点不回：同一个请求应在窗口到期后再出现一次，随后判超时
  C. 节点回 0x7F xx 78：窗口延长，不重传

需要 root 与 vcan：
    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
    sudo ip link add dev vcan1 type vcan && sudo ip link set up vcan1
    sudo ./tools/diag_timeout_check.py

用法：
    python3 tools/diag_timeout_check.py [--iface-a vcan0] [--iface-b vcan1]
"""
import argparse
import os
import re
import subprocess
import sys
import time

for _cand in (os.environ.get("MG_PYLIB"), "/home/dikl/work/.pylibs"):
    if _cand and os.path.isdir(_cand):
        sys.path.insert(0, _cand)
        break

try:
    import can
except ImportError:
    can = None

GW = "./gateway"
CONF = ".diag_timeout_check.conf"
LOG = ".diag_timeout_check.log"

TESTER_REQ = 0x7E0   # tester 侧请求
TESTER_RSP = 0x7E8   # tester 侧响应
NODE_REQ = 0x7E1     # 网关改写后发给节点的请求
NODE_RSP = 0x7E8     # 节点回响应用的 ID

PASS, FAIL = [], []


def check(cond, what, detail=""):
    if cond:
        PASS.append(what)
        print(f"  [PASS] {what}" + (f"  ({detail})" if detail else ""))
    else:
        FAIL.append(what)
        print(f"  [FAIL] {what}" + (f"  ({detail})" if detail else ""))


def iface_exists(name):
    try:
        out = subprocess.run(["ip", "link", "show", name], capture_output=True, text=True)
        return out.returncode == 0
    except FileNotFoundError:
        return False


def write_conf(if_a, if_b):
    with open(CONF, "w", encoding="utf-8") as f:
        f.write("[channels]\n")
        f.write(f"a = {if_a}\n")
        f.write(f"b = {if_b}\n\n")
        # 没有静态路由：诊断帧必须靠映射表转发，否则说明走错了分支
        f.write("[diag]\n")
        f.write(f"a,0x{TESTER_REQ:03X},0x{TESTER_RSP:03X},b,0x{NODE_REQ:03X},0x{NODE_RSP:03X}\n")


def collect(bus, timeout, can_id=None, want=1):
    """在 timeout 内收集至多 want 条匹配帧，返回 (帧列表, 首帧时刻列表)"""
    frames, stamps = [], []
    deadline = time.time() + timeout
    while time.time() < deadline and len(frames) < want:
        m = bus.recv(timeout=0.1)
        if m is None:
            continue
        if can_id is not None and m.arbitration_id != can_id:
            continue
        frames.append(m)
        stamps.append(time.time())
    return frames, stamps


def drain(bus, ms=300):
    end = time.time() + ms / 1000.0
    while time.time() < end:
        bus.recv(timeout=0.05)


def stats_text():
    try:
        return open(LOG, "r", encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def last_stat(text, key):
    vals = re.findall(rf"{key}=(\d+)", text)
    return int(vals[-1]) if vals else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface-a", default="vcan0", help="tester 侧接口")
    ap.add_argument("--iface-b", default="vcan1", help="节点侧接口")
    args = ap.parse_args()

    print("=== can-gateway 诊断超时与重传验证 ===")

    if can is None:
        print("  SKIP: python-can not installed (pip install python-can --target .pylibs)")
        return 0
    if not os.path.exists(GW):
        print(f"  SKIP: {GW} not built (make)")
        return 0
    for name in (args.iface_a, args.iface_b):
        if not iface_exists(name):
            print(f"  SKIP: {name} 不存在。先建虚拟总线：")
            print("    sudo modprobe vcan")
            print("    sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0")
            print("    sudo ip link add dev vcan1 type vcan && sudo ip link set up vcan1")
            return 0

    write_conf(args.iface_a, args.iface_b)
    if os.path.exists(LOG):
        os.unlink(LOG)

    log = open(LOG, "wb")
    # 统计周期 1 秒：窗口是 150ms，太长的周期看不出重传发生在哪一轮
    gw = subprocess.Popen([GW, "--config", CONF, "--stats", "1"],
                          stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.0)

    tester = can.Bus(interface="socketcan", channel=args.iface_a)
    node = can.Bus(interface="socketcan", channel=args.iface_b)

    try:
        print("\nPhase A: 节点正常响应")
        drain(node)
        tester.send(can.Message(arbitration_id=TESTER_REQ, data=[0x02, 0x10, 0x03],
                                is_extended_id=False))
        frames, _ = collect(node, 1.0, can_id=NODE_REQ)
        check(len(frames) == 1, "request is forwarded once", f"got {len(frames)}")
        if frames:
            check(bytes(frames[0].data[:2]) == b"\x02\x10",
                  "payload is carried through unchanged", frames[0].data.hex())

        node.send(can.Message(arbitration_id=NODE_RSP,
                              data=[0x06, 0x50, 0x03, 0x00, 0x32, 0x01, 0xAA, 0xAA],
                              is_extended_id=False))
        rsp, _ = collect(tester, 1.0, can_id=TESTER_RSP)
        check(len(rsp) == 1, "response is routed back to the tester", f"got {len(rsp)}")

        time.sleep(1.2)
        check(last_stat(stats_text(), "answered") == 1, "answered counted once",
              str(last_stat(stats_text(), "answered")))
        check(last_stat(stats_text(), "retry") == 0, "no retransmission for an answered request")

        print("\nPhase B: 节点不响应")
        drain(node)
        tester.send(can.Message(arbitration_id=TESTER_REQ, data=[0x02, 0x3E, 0x00],
                                is_extended_id=False))
        frames, stamps = collect(node, 1.2, can_id=NODE_REQ, want=2)
        check(len(frames) == 2, "the request is sent twice when nobody answers",
              f"got {len(frames)}")
        if len(frames) == 2:
            gap = stamps[1] - stamps[0]
            check(0.10 <= gap <= 0.60, "the gap matches the supervision window",
                  f"{gap * 1000:.0f} ms")

        time.sleep(0.6)
        text = stats_text()
        check((last_stat(text, "timeout") or 0) >= 1, "the unanswered request is counted as timeout",
              str(last_stat(text, "timeout")))
        check((last_stat(text, "retry") or 0) >= 1, "the retransmission is counted",
              str(last_stat(text, "retry")))

        print("\nPhase C: 节点回 0x7F xx 78")
        drain(node)
        retry_before = last_stat(stats_text(), "retry") or 0
        tester.send(can.Message(arbitration_id=TESTER_REQ, data=[0x02, 0x31, 0x01],
                                is_extended_id=False))
        first, _ = collect(node, 1.0, can_id=NODE_REQ)
        check(len(first) == 1, "request forwarded before the pending response")

        node.send(can.Message(arbitration_id=NODE_RSP,
                              data=[0x03, 0x7F, 0x31, 0x78, 0xAA, 0xAA, 0xAA, 0xAA],
                              is_extended_id=False))
        rsp, _ = collect(tester, 1.0, can_id=TESTER_RSP)
        check(len(rsp) == 1, "response pending is routed back to the tester", f"got {len(rsp)}")

        # 原本的 150ms 窗口早已过去，延长到 2000ms，这段时间内不应重传
        again, _ = collect(node, 1.2, can_id=NODE_REQ)
        check(len(again) == 0, "no retransmission while the node reports pending",
              f"got {len(again)}")
        check((last_stat(stats_text(), "retry") or 0) == retry_before,
              "retry counter unchanged")
        check((last_stat(stats_text(), "pending_ext") or 0) >= 1,
              "extension is counted", str(last_stat(stats_text(), "pending_ext")))

        # 补上最终响应，确认延长之后仍能正常结算
        node.send(can.Message(arbitration_id=NODE_RSP,
                              data=[0x02, 0x71, 0x01, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA],
                              is_extended_id=False))
        rsp, _ = collect(tester, 1.0, can_id=TESTER_RSP)
        check(len(rsp) == 1, "final response still reaches the tester after extension")
    finally:
        tester.shutdown()
        node.shutdown()
        gw.terminate()
        try:
            gw.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gw.kill()
        log.close()

    text = stats_text()
    check("diag: tracked=" in text, "the stats line carries the diag counters")
    for key in ("tracked", "answered", "timeout", "retry", "pending_ext"):
        check(last_stat(text, key) is not None, f"stats expose {key}")

    print(f"\nResult: {len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        print(f"日志：{LOG}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
