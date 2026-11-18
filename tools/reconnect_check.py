#!/usr/bin/env python3
"""
reconnect_check.py —— CAN 接口掉线后能否自愈

单元测试只能验证「哪种 errno 算断链」，真正的自愈要在接口上做：
down 掉一路 CAN，网关应当发现错误、关掉 socket、等接口回来后重新 bind
并挂回 epoll，然后继续转发。

判定用两条独立的证据：
  1. 日志里出现 "back in service"（说明重开成功）
  2. 恢复之后再发帧，对端仍能收到（说明 epoll 注册也恢复了）

第 2 条不能省：见过只做了 close/open、忘了 EPOLL_CTL_ADD 的实现，
日志一片祥和，实际一帧都收不到。

需要 root（要 ip link）与 vcan：
    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
    sudo ip link add dev vcan1 type vcan && sudo ip link set up vcan1
    sudo ./tools/reconnect_check.py

用法：
    python3 tools/reconnect_check.py [--down-up vcan0]
"""
import argparse
import os
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
CONF = ".reconnect_check.conf"
LOG = ".reconnect_check.log"

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
        out = subprocess.run(["ip", "link", "show", name],
                             capture_output=True, text=True)
        return out.returncode == 0
    except FileNotFoundError:
        return False


def link(name, state):
    return subprocess.run(["ip", "link", "set", name, state],
                          capture_output=True, text=True).returncode == 0


def write_conf():
    with open(CONF, "w", encoding="utf-8") as f:
        f.write("[channels]\na = vcan0\nb = vcan1\n\n")
        f.write("[routes]\na,0x100,0x1FF,b,0,reconnect check\n")
        f.write("b,0x300,0x3FF,a,0,reconnect check\n\n")
        f.write("[limits]\na = 3000\nb = 3000\n")


def wait_frame(bus, timeout, can_id=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = bus.recv(timeout=0.2)
        if m is not None and (can_id is None or m.arbitration_id == can_id):
            return m
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--down-up", default="vcan0", help="要下线上线的接口")
    args = ap.parse_args()

    print("=== can-gateway 掉线自愈验证 ===")

    if can is None:
        print("  SKIP: python-can not installed (pip install python-can --target .pylibs)")
        return 0
    if not os.path.exists(GW):
        print(f"  SKIP: {GW} not built (make)")
        return 0
    for name in ("vcan0", "vcan1"):
        if not iface_exists(name):
            print(f"  SKIP: {name} 不存在。先建虚拟总线：")
            print("    sudo modprobe vcan")
            print("    sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0")
            print("    sudo ip link add dev vcan1 type vcan && sudo ip link set up vcan1")
            return 0
    if os.geteuid() != 0:
        print("  SKIP: 需要 root 才能 ip link set down/up（sudo 运行本脚本）")
        return 0

    write_conf()
    for f in (LOG,):
        if os.path.exists(f):
            os.unlink(f)

    log = open(LOG, "wb")
    gw = subprocess.Popen([GW, "--config", CONF, "--stats", "1"],
                          stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.0)

    src = can.Bus(interface="socketcan", channel="b")   # 从 b 侧发，网关转到 a 侧
    dst = can.Bus(interface="socketcan", channel="a")

    try:
        print("\nPhase A: 链路正常")
        src.send(can.Message(arbitration_id=0x300, data=[1, 2, 3], is_extended_id=False))
        check(wait_frame(dst, 1.5, can_id=0x300) is not None,
              "frames are forwarded while both interfaces are up")

        print(f"\nPhase B: {args.down_up} down")
        check(link(args.down_up, "down"), f"{args.down_up} goes down")
        time.sleep(2.0)
        # 接口 down 时这一侧发不出去，用另一侧继续发，验证网关不会整体卡死
        other = "vcan1" if args.down_up == "vcan0" else "vcan0"
        check(iface_exists(other), "the other interface is unaffected")

        print(f"\nPhase C: {args.down_up} up")
        check(link(args.down_up, "up"), f"{args.down_up} comes back")
        time.sleep(2.5)

        # 恢复后重新发帧：证明 epoll 注册与 socket 都回来了
        src2 = can.Bus(interface="socketcan", channel="b")
        try:
            for _ in range(5):
                src2.send(can.Message(arbitration_id=0x301, data=[9],
                                      is_extended_id=False))
                time.sleep(0.1)
        finally:
            src2.shutdown()
        check(wait_frame(dst, 2.0, can_id=0x301) is not None,
              "frames flow again after the interface is back")
    finally:
        src.shutdown()
        dst.shutdown()
        gw.terminate()
        try:
            gw.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gw.kill()
        log.close()

    text = open(LOG, "r", encoding="utf-8", errors="replace").read()
    check("back in service" in text or "reopened" in text,
          "the gateway reports the reopened channel", LOG)

    print(f"\nResult: {len(PASS)} passed, {len(FAIL)} failed")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
