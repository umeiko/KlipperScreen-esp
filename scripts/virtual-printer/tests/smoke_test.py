#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""虚拟打印机端到端冒烟测试：模拟 KlipperScreen-esp 的握手与核心流程。

用法: python3 smoke_test.py [host] [port]
纯标准库实现 WebSocket 客户端（掩码帧），不依赖第三方包。
"""
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 7125

PASS = []
FAIL = []

def ok(name, cond, extra=""):
    (PASS if cond else FAIL).append(name)
    print(("PASS  " if cond else "FAIL  ") + name + ("  " + str(extra) if extra else ""))

class WS:
    """极简 WebSocket 客户端（RFC6455 客户端掩码）"""
    def __init__(self, host, port, path="/websocket"):
        self.sock = socket.create_connection((host, port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
               % (path, host, port, key))
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("ws handshake failed")
            resp += chunk
        status = resp.split(b"\r\n", 1)[0]
        if b"101" not in status:
            raise RuntimeError("ws handshake: " + status.decode())
        self.rx = b""
        self.next_id = 1
        self.pending = []  # rpc() 等待期间收到的通知先入缓冲，避免被吞

    def send_text(self, text):
        payload = text.encode()
        header = bytearray([0x81])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _read_exact(self, n):
        while len(self.rx) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("ws closed")
            self.rx += chunk
        out, self.rx = self.rx[:n], self.rx[n:]
        return out

    def read_msg(self, timeout=15):
        if self.pending:
            return self.pending.pop(0)
        return self._read_frame(timeout)

    def _read_frame(self, timeout=15):
        self.sock.settimeout(timeout)
        frags = b""
        while True:
            b1, b2 = self._read_exact(2)
            fin = b1 & 0x80
            opcode = b1 & 0x0F
            masked = b2 & 0x80
            ln = b2 & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._read_exact(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._read_exact(8))[0]
            mask = self._read_exact(4) if masked else b""
            data = self._read_exact(ln)
            if masked:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            if opcode == 9:  # ping -> pong
                hdr = bytearray([0x8A])
                hdr.append(0x80 | len(data))
                m = os.urandom(4)
                hdr += m
                self.sock.sendall(bytes(hdr) + bytes(
                    b ^ m[i % 4] for i, b in enumerate(data)))
                continue
            if opcode == 8:
                raise RuntimeError("ws close frame")
            frags += data
            if fin:
                return json.loads(frags)

    def rpc(self, method, params=None, timeout=20):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send_text(json.dumps(msg))
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = self._read_frame(timeout=max(1, deadline - time.time()))
            if m.get("id") == rid:
                return m
            self.pending.append(m)
        raise TimeoutError(method)


def main():
    ws = WS(HOST, PORT)
    # 1) identify + server.info 等 klippy_connected（与 App 相同顺序）
    ws.rpc("server.connection.identify",
           {"client_name": "smoke-test", "version": "0.0.1",
            "type": "display", "url": "http://example.com"})
    t0 = time.time()
    info = {}
    while time.time() - t0 < 25:
        info = ws.rpc("server.info").get("result", {})
        if info.get("klippy_connected") and \
                info.get("klippy_state") == "ready":
            break
        time.sleep(0.5)
    ok("moonraker klippy_connected+ready",
       info.get("klippy_connected") is True and
       info.get("klippy_state") == "ready", info.get("klippy_state"))

    # 2) objects.list
    r = ws.rpc("printer.objects.list").get("result", {})
    objs = r.get("objects", [])
    need = {"webhooks", "gcode_move", "toolhead", "extruder", "heater_bed",
            "fan", "print_stats", "virtual_sdcard", "display_status",
            "pause_resume", "idle_timeout", "query_endstops", "configfile",
            "mcu", "probe", "bed_mesh", "exclude_object",
            "filament_switch_sensor runout", "heater_fan hotend_fan",
            "temperature_sensor chamber", "gcode_macro START_PRINT"}
    ok("objects.list 覆盖", need.issubset(set(objs)),
       "missing=" + ",".join(sorted(need - set(objs))))

    # 3) subscribe（App 的最小集）
    sub = {"objects": {
        "webhooks": None,
        "print_stats": ["state", "filename", "print_duration",
                        "total_duration", "message"],
        "virtual_sdcard": ["progress", "is_active"],
        "display_status": ["progress", "message"],
        "gcode_move": ["speed_factor", "extrude_factor", "homing_origin"],
        "toolhead": ["position", "homed_axes"],
        "extruder": ["temperature", "target", "power"],
        "heater_bed": ["temperature", "target", "power"],
        "fan": ["speed"],
        "idle_timeout": ["state"],
        "manual_probe": ["is_active"],
        "pause_resume": ["is_paused"]}}
    r = ws.rpc("printer.objects.subscribe", sub).get("result", {})
    st = r.get("status", {})
    ok("subscribe 快照含 webhooks.state",
       st.get("webhooks", {}).get("state") == "ready", st.get("webhooks"))
    ok("快照含 extruder.temperature",
       isinstance(st.get("extruder", {}).get("temperature"), (int, float)))

    # 4) 加热流程
    ws.rpc("printer.gcode.script", {"script": "M104 S80"})
    ws.rpc("printer.gcode.script", {"script": "M140 S45"})
    time.sleep(3)
    r = ws.rpc("printer.objects.query",
               {"objects": {"extruder": ["temperature", "target"],
                            "heater_bed": ["temperature", "target"]}})\
        .get("result", {})
    ex = r["status"]["extruder"]
    bed = r["status"]["heater_bed"]
    ok("热端升温中", ex["target"] == 80.0 and ex["temperature"] > 26, ex)
    ok("热床升温中", bed["target"] == 45.0 and bed["temperature"] > 26, bed)

    # 5) 归位 + 移动 + 限位查询
    ws.rpc("printer.gcode.script", {"script": "G28"})
    r = ws.rpc("printer.objects.query",
               {"objects": {"toolhead": ["homed_axes", "position"]}})\
        .get("result", {})
    ok("G28 后 homed_axes=xyz",
       r["status"]["toolhead"]["homed_axes"] == "xyz",
       r["status"]["toolhead"])
    ws.rpc("printer.gcode.script", {"script": "G1 X50 Y60 Z10 F6000"})
    r = ws.rpc("printer.objects.query",
               {"objects": {"toolhead": ["position"]}}).get("result", {})
    pos = r["status"]["toolhead"]["position"]
    ok("G1 位置更新", abs(pos[0] - 50) < 0.01 and abs(pos[2] - 10) < 0.01, pos)
    ws.rpc("printer.gcode.script", {"script": "QUERY_ENDSTOPS"})
    r = ws.rpc("printer.query_endstops.status").get("result", {})
    ok("query_endstops", "stepper_x" in r and "stepper_z" in r, r)

    # 6) 订阅推送（4Hz 差速推送应在工作）
    msg = ws.read_msg(timeout=5)
    got_update = False
    for _ in range(20):
        if msg.get("method") == "notify_status_update":
            got_update = True
            break
        msg = ws.read_msg(timeout=5)
    ok("notify_status_update 推送", got_update)

    # 7) gcode 控制台输出
    ws.rpc("printer.gcode.script", {"script": "M118 smoke_echo_42"})
    got = False
    for _ in range(20):
        m = ws.read_msg(timeout=5)
        if m.get("method") == "notify_gcode_response" and \
                "smoke_echo_42" in str(m.get("params")):
            got = True
            break
    ok("notify_gcode_response (M118)", got)

    # 8) 文件列表 + 元数据 + 缩略图下载
    r = ws.rpc("server.files.list", {"root": "gcodes"}).get("result", [])
    names = [f.get("path") for f in r]
    ok("server.files.list", "demo_cube.gcode" in names, names)
    md = http_json("/server/files/metadata?filename=demo_cube.gcode")\
        .get("result", {})
    thumbs = md.get("thumbnails", [])
    ok("metadata thumbnails", len(thumbs) >= 2,
       [(t.get("width"), t.get("relative_path")) for t in thumbs])
    if thumbs:
        rel = sorted(thumbs, key=lambda t: t.get("width", 0))[0]["relative_path"]
        blob = http_raw("/server/files/gcodes/" + rel)
        ok("缩略图下载 PNG", blob[:8] == b"\x89PNG\r\n\x1a\n", len(blob))

    # 9) gcode_store / gcode.help
    r = ws.rpc("server.gcode_store", {"count": 50}).get("result", {})
    ok("server.gcode_store", "gcode_store" in r, list(r.keys()))
    r = ws.rpc("printer.gcode.help").get("result", {})
    ok("printer.gcode.help", "G28" in r and "START_PRINT" in r,
       len(r))

    # 10) 打印生命周期（quick_line.gcode 走带快）
    ws.rpc("printer.print.start", {"filename": "quick_line.gcode"})
    time.sleep(1.0)
    r = ws.rpc("printer.objects.query",
               {"objects": {"print_stats": None, "virtual_sdcard": None}})\
        .get("result", {})
    ps = r["status"]["print_stats"]
    ok("打印中", ps.get("state") == "printing", ps.get("state"))
    ok("virtual_sdcard is_active",
       r["status"]["virtual_sdcard"].get("is_active") is True)

    ws.rpc("printer.print.pause")
    time.sleep(0.5)
    r = ws.rpc("printer.objects.query",
               {"objects": {"pause_resume": None, "print_stats": ["state"]}})\
        .get("result", {})
    ok("暂停", r["status"]["pause_resume"].get("is_paused") is True
       and r["status"]["print_stats"].get("state") == "paused")

    ws.rpc("printer.print.resume")
    time.sleep(0.5)
    r = ws.rpc("printer.objects.query",
               {"objects": {"pause_resume": None, "print_stats": ["state"]}})\
        .get("result", {})
    ok("恢复", r["status"]["pause_resume"].get("is_paused") is False
       and r["status"]["print_stats"].get("state") == "printing")

    ws.rpc("printer.print.cancel")
    time.sleep(0.5)
    r = ws.rpc("printer.objects.query",
               {"objects": {"print_stats": ["state"]}}).get("result", {})
    ok("取消", r["status"]["print_stats"].get("state") == "cancelled",
       r["status"]["print_stats"])

    # 11) 完成一次完整打印（等它打完）
    ws.rpc("printer.print.start", {"filename": "quick_line.gcode"})
    t0 = time.time()
    done = False
    while time.time() - t0 < 120:
        r = ws.rpc("printer.objects.query",
                   {"objects": {"print_stats": ["state"],
                                "virtual_sdcard": ["progress"]}})\
            .get("result", {})
        if r["status"]["print_stats"].get("state") == "complete":
            done = True
            break
        time.sleep(1)
    ok("打印跑完到 complete", done,
       r["status"]["virtual_sdcard"].get("progress"))

    # 12) history 有记录
    r = ws.rpc("server.history.list", {"limit": 5}).get("result", {})
    ok("server.history.list", len(r.get("jobs", [])) >= 1,
       [j.get("filename") for j in r.get("jobs", [])])

    # 13) 急停 + 恢复
    ws.rpc("printer.emergency_stop")
    time.sleep(1.0)
    r = ws.rpc("printer.objects.query", {"objects": {"webhooks": None}})\
        .get("result", {})
    ok("急停后 klippy shutdown",
       r["status"]["webhooks"].get("state") == "shutdown")
    ws.rpc("printer.firmware_restart")
    t0 = time.time()
    back = False
    while time.time() - t0 < 30:
        try:
            info = ws.rpc("server.info", timeout=5).get("result", {})
            if info.get("klippy_state") == "ready":
                back = True
                break
        except Exception:
            pass
        time.sleep(0.8)
    ok("firmware_restart 后回到 ready", back)

    print("\n==== 结果: %d 通过, %d 失败 ====" % (len(PASS), len(FAIL)))
    if FAIL:
        print("失败项:", FAIL)
        sys.exit(1)


def http_json(path):
    with urllib.request.urlopen(
            "http://%s:%d%s" % (HOST, PORT, path), timeout=10) as r:
        return json.loads(r.read())


def http_raw(path):
    with urllib.request.urlopen(
            "http://%s:%d%s" % (HOST, PORT, path.replace(" ", "%20")),
            timeout=10) as r:
        return r.read()


if __name__ == "__main__":
    main()
