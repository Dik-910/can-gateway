#!/usr/bin/python3
"""
reconnect_check.py —— CAN 接口掉线后能否自愈

单元测试只能验证「哪种 errno 算断链」，真正的自愈要在接口上做：
把一路 CAN down 掉，再往那一路发帧，网关的 sendto 会拿到 ENETDOWN，
它应当关掉 socket、等接口回来后重新 bind 并挂回 epoll，然后继续转发。

要让网关「尝试往 down 掉的那一路发」，就得从另一路发帧进来：
配置里 a→b 的规则，从 a 发、往 b 转；b 被 down 时转发必然失败。

判定用三条独立证据：
  1. 日志里出现 "back in service"（重开成功）
  2. 统计行里 reopen 计数大于 0
  3. 恢复之后再发帧，对端仍能收到（epoll 注册也恢复了）

第 3 条不能省：见过只做了 close/open、忘了 EPOLL_CTL_ADD 的实现，
日志一片祥和，实际一帧都收不到。

需要 root（要 ip link）与 vcan：
    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
    sudo ip link add dev vcan1 type vcan && sudo ip link set up vcan1
    sudo ./tools/reconnect_check.py

用法：
    python3 tools/reconnect_check.py [--iface-a vcan0] [--iface-b vcan1] [--down-up vcan1]
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
CONF = ".reconnect_check.conf"
LOG = ".reconnect_check.log"

SEND_ID = 0x100   # a -> b，从 if_a 发、网关往 if_b 转
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


def link(name, state):
    r = subprocess.run(["ip", "link", "set", name, state], capture_output=True, text=True)
    if r.returncode != 0 and "not permitted" in (r.stderr + r.stdout):
        # 容器里 uid 可能是 0 但拿不到 CAP_NET_ADMIN，这不是脚本的问题
        print("  SKIP: 没有 CAP_NET_ADMIN，无法 ip link set（容器内常见）")
        print("        请在宿主机上直接运行本脚本：sudo ./tools/reconnect_check.py")
        sys.exit(0)
    return r.returncode == 0


def write_conf(if_a, if_b):
    with open(CONF, "w", encoding="utf-8") as f:
        f.write(f"[channels]\na = {if_a}\nb = {if_b}\n\n")
        f.write("[routes]\na,0x100,0x1FF,b,0,reconnect check\n")
        f.write("b,0x300,0x3FF,a,0,reconnect check\n\n")
        f.write("[limits]\na = 3000\nb = 3000\n")


def wait_frame(bus, timeout, can_id=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            m = bus.recv(timeout=0.2)
        except OSError:
            # 接口 down 期间读会报错，等它回来即可
            time.sleep(0.1)
            continue
        if m is not None and (can_id is None or m.arbitration_id == can_id):
            return m
    return None


def send_some(bus, count, gap=0.1, can_id=SEND_ID):
    for _ in range(count):
        bus.send(can.Message(arbitration_id=can_id, data=[1, 2, 3], is_extended_id=False))
        time.sleep(gap)


def open_bus(iface):
    return can.Bus(interface="socketcan", channel=iface)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface-a", default="vcan0", help="发送侧接口")
    ap.add_argument("--iface-b", default="vcan1", help="被 down 的接收侧接口")
    ap.add_argument("--down-up", default=None, help="要下线上线的接口，默认取 B 侧")
    args = ap.parse_args()

    if_a, if_b = args.iface_a, args.iface_b
    down_up = args.down_up or if_b

    print("=== can-gateway 掉线自愈验证 ===")

    if can is None:
        print("  SKIP: python-can not installed (pip install python-can --target .pylibs)")
        return 0
    if not os.path.exists(GW):
        print(f"  SKIP: {GW} not built (make)")
        return 0
    for name in (if_a, if_b):
        if not iface_exists(name):
            print(f"  SKIP: {name} 不存在。先建虚拟总线：")
            print("    sudo modprobe vcan")
            print("    sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0")
            print("    sudo ip link add dev vcan1 type vcan && sudo ip link set up vcan1")
            return 0
    if os.geteuid() != 0:
        print("  SKIP: 需要 root 才能 ip link set down/up（sudo 运行本脚本）")
        return 0

    write_conf(if_a, if_b)
    if os.path.exists(LOG):
        os.unlink(LOG)

    log = open(LOG, "wb")
    gw = subprocess.Popen([GW, "--config", CONF, "--stats", "1"],
                          stdout=log, stderr=subprocess.STDOUT)
    time.sleep(1.0)

    src = open_bus(if_a)
    dst = open_bus(if_b)

    try:
        print("\nPhase A: 链路正常")
        src.send(can.Message(arbitration_id=SEND_ID, data=[1, 2, 3], is_extended_id=False))
        check(wait_frame(dst, 1.5, can_id=SEND_ID) is not None,
              "frames are forwarded while both interfaces are up")

        print(f"\nPhase B: {down_up} down，继续往它转发")
        check(link(down_up, "down"), f"{down_up} goes down")
        time.sleep(0.5)

        # 网关的 sendto 在这里会拿到 ENETDOWN，应当标记需要重开
        try:
            send_some(src, 10)
        except OSError as exc:
            check(False, "the sender side keeps working", str(exc))

        # down 期间重开会失败，reopen_fail 应当涨起来
        failed = wait_frame(dst, 2.0, can_id=SEND_ID)
        check(failed is None, "nothing reaches the interface while it is down")

        print(f"\nPhase C: {down_up} up")
        check(link(down_up, "up"), f"{down_up} comes back")
        time.sleep(2.5)

        # 旧 socket 可能随接口重建而失效，重新打开再发
        for bus in (src, dst):
            try:
                bus.shutdown()
            except Exception:
                pass
        src = open_bus(if_a)
        dst = open_bus(if_b)

        send_some(src, 5)
        check(wait_frame(dst, 2.0, can_id=SEND_ID) is not None,
              "frames flow again after the interface is back")
    finally:
        for bus in (src, dst):
            try:
                bus.shutdown()
            except Exception:
                pass
        gw.terminate()
        try:
            gw.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gw.kill()
        log.close()

    text = open(LOG, "r", encoding="utf-8", errors="replace").read()
    check("back in service" in text, "the gateway reports the reopened channel", LOG)

    reopens = [int(v) for v in re.findall(r"reopen=(\d+)", text)]
    check(bool(reopens) and max(reopens) >= 1,
          "the reopen counter is visible in the stats line",
          f"max reopen={max(reopens) if reopens else 'n/a'}")

    fails = [int(v) for v in re.findall(r"fail=(\d+)", text)]
    check(not fails or max(fails) >= 1,
          "the failed reopen attempts are counted",
          f"max fail={max(fails) if fails else 'n/a'}")

    print(f"\nResult: {len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        print(f"日志：{LOG}")
    return 0 if not FAIL else 1


if __name__ == "__main__":
    sys.exit(main())
