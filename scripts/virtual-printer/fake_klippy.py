#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fake_klippy.py — 虚拟 Klippy 主机进程（KlipperScreen-esp 调试用）

在 klippy 的 unix-socket JSON-RPC 协议层模拟一台完整的 3D 打印机：
真实 Moonraker 连接它后，与连接真打印机行为一致（对象订阅、GCode、
SD 卡打印、暂停/恢复/取消、急停/重启、探针/网床、温度热模型……）。

协议参考 klippy/webhooks.py：ETX(\x03) 分隔的 JSON-RPC 2.0。

仅依赖 Python 标准库；Linux 用 unix domain socket，Windows/MSYS2 用
tcp://（MSYS2 的 asyncio 不支持 AF_UNIX 客户端，见 README）。

用法:
  python3 fake_klippy.py --config printer.cfg --socket /path/klippy.sock \
      --gcodes /path/gcodes --log /path/klippy.log [--klipper-repo ../klipper]
  python3 fake_klippy.py ... --socket tcp://127.0.0.1:7126   # Windows/MSYS2

调速: --bps <bytes/sec> 控制虚拟打印走带速度（默认 65536）。
"""
import argparse
import copy
import json
import logging
import math
import os
import random
import re
import socket
import subprocess
import sys
import threading
import time

ETX = b"\x03"
AMBIENT = 25.0

# ---------------------------------------------------------------- 工具

def _to_number(v):
    try:
        return int(v)
    except ValueError:
        try:
            return float(v)
        except ValueError:
            return v

def git_version(repo):
    if repo and os.path.isdir(os.path.join(repo, ".git")):
        try:
            out = subprocess.run(
                ["git", "-C", repo, "describe", "--always", "--tags",
                 "--long", "--dirty"],
                capture_output=True, text=True, timeout=5)
            v = out.stdout.strip()
            if v:
                return v
        except Exception:
            pass
    return "v0.0.0-sim"

# ---------------------------------------------------------------- 配置解析

class PrinterCfg:
    """解析 printer.cfg（INI）。sections: {name_lower: {opt: str}}"""

    def __init__(self, path):
        self.path = path
        self.sections = {}      # 保序
        self.raw = ""
        self.reload(path)

    def reload(self, path):
        self.sections = {}
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            self.raw = f.read()
        cur = None
        for line in self.raw.splitlines():
            s = line.strip()
            if not s or s.startswith("#") or s.startswith(";"):
                continue
            m = re.match(r"^\[([^\]]+)\]", s)
            if m:
                cur = m.group(1).strip().lower()
                self.sections.setdefault(cur, {})
                continue
            if cur and "=" in s:
                k, v = s.split("=", 1)
                v = v.split("#", 1)[0].split(";", 1)[0].strip() \
                    if not v.strip().startswith("\n") else v.strip()
                # 多行值（gcode 宏）: 保留原始缩进行
                self.sections[cur].setdefault(k.strip().lower(), "")
        # 二次解析拿宏多行 body
        self._parse_macro_bodies()

    def _parse_macro_bodies(self):
        cur = None
        curkey = None
        buf = []
        for line in self.raw.splitlines():
            m = re.match(r"^\[([^\]]+)\]", line.strip())
            if m:
                if cur and curkey and buf:
                    self.sections[cur][curkey] = "\n".join(buf).strip("\n")
                cur = m.group(1).strip().lower()
                curkey = None
                buf = []
                continue
            if cur is None:
                continue
            if line.startswith((" ", "\t")):
                if curkey:
                    buf.append(line.strip())
            else:
                if cur and curkey and buf:
                    self.sections[cur][curkey] = "\n".join(buf).strip("\n")
                    buf = []
                if "=" in line:
                    k, v = line.split("=", 1)
                    curkey = k.strip().lower()
                    v = v.strip()
                    if v:
                        self.sections[cur][curkey] = v
        if cur and curkey and buf:
            self.sections[cur][curkey] = "\n".join(buf).strip("\n")

    def get(self, sec, opt, default=None):
        return self.sections.get(sec, {}).get(opt, default)

    def getf(self, sec, opt, default=0.0):
        try:
            return float(self.sections.get(sec, {}).get(opt, default))
        except (ValueError, TypeError):
            return float(default)

    def sections_with_prefix(self, prefix):
        return [s for s in self.sections if s.startswith(prefix)]

    def typed_settings(self):
        out = {}
        for sec, opts in self.sections.items():
            d = {}
            for k, v in opts.items():
                if "\n" in v:
                    d[k] = v
                    continue
                lv = v.strip().lower()
                if lv in ("true", "false"):
                    d[k] = lv == "true"
                else:
                    d[k] = _to_number(v)
            out[sec] = d
        return out

# ---------------------------------------------------------------- 热模型

class Heater:
    """一阶热模型: dT = k_heat*power - k_cool*(T-amb)。带噪声保证订阅推送。"""
    def __init__(self, name, min_temp=-20, max_temp=275, heat_rate=2.2,
                 cool_rate=0.35):
        self.name = name
        self.min_temp = float(min_temp)
        self.max_temp = float(max_temp)
        self.temp = AMBIENT
        self.target = 0.0
        self.power = 0.0
        self.heat_rate = heat_rate
        self.cool_rate = cool_rate
        self._phase = random.random() * 6.28

    def tick(self, dt):
        amb = AMBIENT
        if self.target > amb:
            if self.temp < self.target - 2:
                self.power = 0.85
            elif self.temp < self.target:
                self.power = max(0.15, (self.target - self.temp) / 2.0 * 0.7)
            elif self.temp < self.target + 0.8:
                self.power = 0.25
            else:
                self.power = 0.0
        else:
            self.power = 0.0
        d = (self.power * self.heat_rate
             - self.cool_rate * (self.temp - amb)) * dt
        self.temp += d
        # 噪声: ADC 采样抖动 —— 订阅者依赖温度变化驱动持续推送
        self._phase += dt * 1.7
        self.noise = math.sin(self._phase) * 0.12 + random.uniform(-0.06, 0.06)
        t = self.temp + self.noise
        self.temp = max(amb - 2, min(self.max_temp + 5, self.temp))
        return t

    def reading(self):
        t = max(self.min_temp, min(self.max_temp + 5,
                                   self.temp + getattr(self, "noise", 0.0)))
        return round(t, 2)

    def status(self, can_extrude_from=0.0):
        s = {"temperature": self.reading(),
             "target": round(self.target, 2),
             "power": round(self.power, 3)}
        return s

class TempSensor:
    def __init__(self, name, min_temp=-20, max_temp=120):
        self.name = name
        self.min_temp = float(min_temp)
        self.max_temp = float(max_temp)
        self.temp = AMBIENT
        self.measured_min = 99999999.0
        self.measured_max = -99999999.0
        self._phase = random.random() * 6.28

    def tick(self, dt, bias=0.0):
        self._phase += dt * 0.9
        self.temp = AMBIENT + bias + math.sin(self._phase) * 0.3 \
            + random.uniform(-0.1, 0.1)
        self.measured_min = min(self.measured_min, self.temp)
        self.measured_max = max(self.measured_max, self.temp)

    def status(self):
        return {"temperature": round(self.temp, 2),
                "measured_min_temp": round(self.measured_min, 2),
                "measured_max_temp": round(self.measured_max, 2)}

# ---------------------------------------------------------------- 打印引擎

class SDCardPrint:
    def __init__(self, printer):
        self.printer = printer
        self.reset()

    def reset(self):
        self.file_path = None
        self.file_size = 0
        self.file_position = 0
        self.is_active = False
        self.lines = []
        self.line_start = []     # 每行起始字节偏移
        self.next_line = 0

    def load(self, filename):
        gc_root = self.printer.gcodes_dir
        rel = filename.lstrip("/")
        path = os.path.normpath(os.path.join(gc_root, rel))
        if not path.startswith(os.path.abspath(gc_root)) \
                and not path.startswith(os.path.normpath(os.path.abspath(gc_root))):
            raise PrinterError("Invalid file path")
        if not os.path.isfile(path):
            raise PrinterError("Unable to open file: %s" % filename)
        with open(path, "rb") as f:
            data = f.read()
        self.file_path = path
        self.file_size = len(data)
        self.file_position = 0
        self.is_active = True
        text = data.decode("utf-8", errors="replace")
        self.lines = text.splitlines()
        self.line_start = []
        off = 0
        for ln in self.lines:
            self.line_start.append(off)
            off += len(ln.encode("utf-8", errors="replace")) + 1
        self.next_line = 0
        self._scan_metadata()
        return path

    def _scan_metadata(self):
        """从 gcode 提取层数/耗材/对象（PrusaSlicer/Klipper 风格注释）"""
        p = self.printer
        p.total_layer = None
        p.current_layer = None
        p.filament_total = 0.0
        for ln in self.lines[:400]:
            m = re.search(r"^;\s*LAYER_COUNT:\s*(\d+)", ln, re.I)
            if m:
                p.total_layer = int(m.group(1))
            m = re.search(r"^;\s*filament used \[mm\]\s*=\s*([\d.]+)", ln, re.I)
            if m:
                p.filament_total = float(m.group(1))
            m = re.search(r"^;\s*FILAMENT_USED:\s*([\d.]+)", ln, re.I)
            if m:
                p.filament_total = float(m.group(1))
        p.excl_objects = []
        p.excl_excluded = []
        for ln in self.lines[:2000]:
            m = re.match(r"EXCLUDE_OBJECT_DEFINE\s+(.*)", ln)
            if m:
                params = dict(kv.split("=", 1) for kv in m.group(1).split()
                              if "=" in kv)
                obj = {"name": params.get("NAME", "?")}
                for k in ("CENTER", "POLYGON"):
                    if k in params:
                        try:
                            obj[k.lower()] = json.loads(params[k])
                        except Exception:
                            pass
                p.excl_objects.append(obj)

    def tick(self, dt):
        """按字节速率走带并解释执行"""
        p = self.printer
        if not self.is_active or p.paused:
            return
        p.sd_byte_budget += p.print_bps * dt
        while self.is_active and self.next_line < len(self.lines):
            idx = self.next_line
            line = self.lines[idx]
            cost = len(line.encode("utf-8", errors="replace")) + 1
            if cost > p.sd_byte_budget:
                break
            p.sd_byte_budget -= cost
            self.file_position = self.line_start[idx] + cost
            self.next_line += 1
            line = line.strip()
            if not line:
                continue
            m = re.match(r"^;LAYER:\s*(\d+)", line)
            if m:
                p.current_layer = int(m.group(1))
            if line.startswith(";"):
                continue
            try:
                p.run_gcode_line(line, from_sd=True)
            except PrinterError as e:
                p.respond("// SD print warning: %s" % e)
            except Exception as e:
                p.respond("// SD print error: %s" % e)
        if self.is_active and self.next_line >= len(self.lines):
            self.is_active = False
            self.file_position = self.file_size
            p.finish_print("complete", "")

    def progress(self):
        if not self.file_size:
            return 0.0
        return min(1.0, self.file_position / float(self.file_size))

    def status(self):
        return {"file_path": self.file_path,
                "progress": round(self.progress(), 6),
                "is_active": self.is_active,
                "file_position": self.file_position,
                "file_size": self.file_size}

class PrinterError(Exception):
    pass

# ---------------------------------------------------------------- 打印机

class Printer:
    def __init__(self, args):
        self.args = args
        self.cfg = PrinterCfg(args.config)
        self.gcodes_dir = os.path.abspath(args.gcodes)
        self.lock = threading.RLock()
        self.state = "startup"
        self.state_message = "Printer is starting"
        self.boot_time = time.monotonic()
        self.ready_at = self.boot_time + float(args.startup_delay)
        self.shutdown_requested = False

        # 运动状态
        self.pos = [0.0, 0.0, 0.0, 0.0]          # 机器坐标 x,y,z,e
        self.gcode_offset = [0.0, 0.0, 0.0, 0.0]  # G92
        self.homing_origin = [0.0, 0.0, 0.0, 0.0]  # SET_GCODE_OFFSET
        self.homed_axes = ""
        self.absolute_coord = True
        self.absolute_extrude = True
        self.speed_factor = 1.0
        self.extrude_factor = 1.0
        self.speed = 0.0                           # 最近一次 F（mm/s）
        self.axis_min = [0.0, 0.0, 0.0]
        self.axis_max = [220.0, 220.0, 250.0]
        self.max_velocity = 300.0
        self.max_accel = 3000.0
        self.min_cruise_ratio = 0.5
        self.square_corner_velocity = 5.0
        self.pressure_advance = 0.0
        self.smooth_time = 0.04
        self.min_extrude_temp = 170.0

        # 加热器/传感器/风扇
        self.heaters = {}          # name -> Heater
        self.sensors = {}          # temperature_sensor -> TempSensor
        self.fans = {}             # fan name -> speed
        self.fan_rpm = {}
        self.heater_fans = {}      # heater_fan 联动
        self.controller_fans = {}
        self.filament_sensors = {}  # name -> {enabled, detected}
        self.output_pins = {}
        self.macros = {}           # name -> {body, desc, variables}
        self.probe_cfg = None
        self.probe_z_offset = 0.0
        self.probe_last_query = 0
        self.probe_last_z = None
        self.probe_last_pos = None
        self.has_bed_mesh = False
        self.bed_mesh = {"profile_name": "", "mesh_min": [0, 0],
                         "mesh_max": [0, 0], "probed_matrix": [[]],
                         "mesh_matrix": [[]], "profiles": {}}
        self.has_exclude = False
        self.excl_objects = []
        self.excl_excluded = []
        self.excl_current = None
        self.has_manual_probe = False
        self.manual_probe_active = False
        self.has_fw_retract = False
        self.fw_retract = {"retract_length": 1.0, "retract_speed": 30.0,
                           "unretract_extra_length": 0.0,
                           "unretract_speed": 30.0}
        self.has_respond = False

        # 打印状态
        self.sd = SDCardPrint(self)
        self.paused = False
        self.print_state = "standby"   # standby/printing/paused/complete/cancelled/error
        self.print_message = ""
        self.print_filename = ""
        self.print_start_time = None
        self.pause_start_time = None
        self.paused_total = 0.0
        self.filament_used = 0.0
        self.total_layer = None
        self.current_layer = None
        self.filament_total = 0.0
        self.sd_byte_budget = 0.0
        self.print_bps = float(args.bps)

        # 显示/其他
        self.disp_message = ""
        self.disp_progress = None      # M73
        self.disp_progress_expire = 0.0
        self.idle_state = "Ready"
        self.idle_timeout = 600.0
        self.last_activity = time.monotonic()
        self.endstop_last = {}
        self.save_config_pending = False
        self.save_config_pending_items = {}

        self.version = git_version(args.klipper_repo)
        self.start_time = time.time()
        self._load_persist()
        self._build_from_config()

    # ---------------- 配置 -> 对象 ----------------
    def _build_from_config(self):
        cfg = self.cfg
        with self.lock:
            self.heaters = {}
            self.sensors = {}
            self.fans = {}
            self.fan_rpm = {}
            self.heater_fans = {}
            self.controller_fans = {}
            self.filament_sensors = {}
            self.output_pins = {}
            self.macros = {}
            self.probe_cfg = None
            self.has_bed_mesh = False
            self.has_exclude = cfg.get("exclude_object", "enabled") is not None \
                or "exclude_object" in cfg.sections
            self.has_manual_probe = "manual_probe" in cfg.sections
            self.has_fw_retract = "firmware_retraction" in cfg.sections
            self.has_respond = "respond" in cfg.sections

            for i, ax in enumerate(("x", "y", "z")):
                sec = "stepper_" + ax
                if sec in cfg.sections:
                    self.axis_min[i] = cfg.getf(sec, "position_min", 0.0)
                    self.axis_max[i] = cfg.getf(sec, "position_max",
                                                self.axis_max[i])
            if "printer" in cfg.sections:
                self.max_velocity = cfg.getf("printer", "max_velocity", 300.0)
                self.max_accel = cfg.getf("printer", "max_accel", 3000.0)
                self.min_cruise_ratio = cfg.getf(
                    "printer", "minimum_cruise_ratio", 0.5)
                self.square_corner_velocity = cfg.getf(
                    "printer", "square_corner_velocity", 5.0)

            ex = cfg.get("extruder", "min_extrude_temp")
            if ex is not None:
                self.min_extrude_temp = cfg.getf("extruder",
                                                 "min_extrude_temp", 170.0)
            self.pressure_advance = cfg.getf("extruder", "pressure_advance", 0.0)
            self.smooth_time = cfg.getf("extruder", "pressure_advance_smooth_time",
                                        0.04)

            for sec in list(cfg.sections):
                if sec == "extruder" or re.match(r"extruder\d+$", sec) or \
                        sec == "heater_bed" or sec.startswith("heater_generic "):
                    mn = cfg.getf(sec, "min_temp", -20.0)
                    mx = cfg.getf(sec, "max_temp", 275.0)
                    if sec == "heater_bed":
                        hr, cr = 0.9, 0.08
                    elif sec.startswith("heater_generic"):
                        hr, cr = 1.5, 0.15
                    else:
                        hr, cr = 2.5, 0.30
                    self.heaters[sec] = Heater(sec, mn, mx, hr, cr)
                elif sec.startswith("temperature_sensor "):
                    name = sec.split(" ", 1)[1]
                    self.sensors[name] = TempSensor(
                        name, cfg.getf(sec, "min_temp", -20.0),
                        cfg.getf(sec, "max_temp", 120.0))
                elif sec == "fan" or sec.startswith("fan_generic "):
                    name = sec if sec == "fan" else sec.split(" ", 1)[1]
                    self.fans.setdefault(name, 0.0)
                    self.fan_rpm.setdefault(name, None)
                elif sec.startswith("heater_fan "):
                    name = sec.split(" ", 1)[1]
                    self.heater_fans[name] = {
                        "speed": 0.0, "rpm": None,
                        "fan_speed": cfg.getf(sec, "fan_speed", 1.0),
                        "heater": [h.strip() for h in
                                   cfg.get(sec, "heater", "extruder").split(",")],
                        "temp": cfg.getf(sec, "heater_temp", 50.0)}
                elif sec.startswith("controller_fan "):
                    name = sec.split(" ", 1)[1]
                    self.controller_fans[name] = {
                        "speed": 0.0, "rpm": None,
                        "fan_speed": cfg.getf(sec, "fan_speed", 1.0)}
                elif sec.startswith("filament_switch_sensor ") or \
                        sec.startswith("filament_motion_sensor "):
                    name = sec.split(" ", 1)[1]
                    self.filament_sensors[name] = {
                        "enabled": True, "detected": True}
                elif sec.startswith("output_pin "):
                    name = sec.split(" ", 1)[1]
                    self.output_pins[name] = cfg.getf(sec, "value", 0.0)
                elif sec.startswith("gcode_macro "):
                    name = sec.split(" ", 1)[1]
                    body = cfg.get(sec, "gcode", "")
                    desc = cfg.get(sec, "description", "G-Code macro")
                    variables = {}
                    for k, v in cfg.sections[sec].items():
                        if k.startswith("variable_"):
                            variables[k[9:]] = _to_number(v)
                    self.macros[name.upper()] = {
                        "body": body, "desc": desc, "variables": variables}
                elif sec == "probe" or sec == "bltouch":
                    self.probe_cfg = sec
                    self.probe_z_offset = cfg.getf(sec, "z_offset", 0.0)
                elif sec == "bed_mesh":
                    self.has_bed_mesh = True
                elif sec == "idle_timeout":
                    self.idle_timeout = cfg.getf(sec, "timeout", 600.0)

            if "virtual_sdcard" not in cfg.sections:
                logging.warning("printer.cfg 缺少 [virtual_sdcard]")
            if "pause_resume" not in cfg.sections:
                logging.warning("printer.cfg 缺少 [pause_resume]")
            if "display_status" not in cfg.sections:
                logging.warning("printer.cfg 缺少 [display_status]")

    # ---------------- 持久化（SAVE_CONFIG） ----------------
    def _persist_path(self):
        d = os.path.dirname(os.path.abspath(self.args.log))
        return os.path.join(d, "fake_klippy_saved.json")

    def _load_persist(self):
        try:
            with open(self._persist_path(), "r", encoding="utf-8") as f:
                data = json.load(f)
            self.probe_z_offset = data.get("probe_z_offset", 0.0)
            profiles = data.get("bed_mesh_profiles", {})
            self._persisted_mesh_profiles = profiles
        except Exception:
            self._persisted_mesh_profiles = {}

    def _save_persist(self):
        data = {"probe_z_offset": self.probe_z_offset,
                "bed_mesh_profiles": self.bed_mesh.get("profiles", {})}
        try:
            with open(self._persist_path(), "w", encoding="utf-8") as f:
                json.dump(data, f, indent=1)
        except Exception:
            logging.exception("save persist failed")

    # ---------------- 状态输出 ----------------
    def object_names(self):
        names = ["webhooks", "gcode", "configfile", "mcu", "system_stats",
                 "gcode_move", "toolhead", "virtual_sdcard", "print_stats",
                 "display_status", "pause_resume", "idle_timeout",
                 "query_endstops"]
        names += list(self.heaters.keys())
        names += ["temperature_sensor " + n for n in self.sensors]
        if "fan" in self.fans:
            names.append("fan")
        names += ["fan_generic " + n for n in self.fans if n != "fan"]
        names += ["heater_fan " + n for n in self.heater_fans]
        names += ["controller_fan " + n for n in self.controller_fans]
        names += ["filament_switch_sensor " + n for n in self.filament_sensors]
        names += ["output_pin " + n for n in self.output_pins]
        if self.probe_cfg:
            names.append(self.probe_cfg)
        if self.has_bed_mesh:
            names.append("bed_mesh")
        if self.has_exclude:
            names.append("exclude_object")
        if self.has_manual_probe:
            names.append("manual_probe")
        if self.has_fw_retract:
            names.append("firmware_retraction")
        names += ["gcode_macro " + n for n in sorted(self.macros)]
        return names

    def _heater_status(self, name):
        h = self.heaters[name]
        s = h.status()
        if name.startswith("extruder"):
            s["pressure_advance"] = self.pressure_advance
            s["smooth_time"] = self.smooth_time
            s["can_extrude"] = h.reading() >= self.min_extrude_temp
        return s

    def get_object_status(self, name):
        """对齐 klippy 各对象 get_status() 字段"""
        now = time.monotonic()
        if name == "webhooks":
            return {"state": self.state, "state_message": self.state_message}
        if name == "gcode":
            return {"commands": self._command_help_status()}
        if name == "configfile":
            return {"config": {s: dict(o) for s, o in self.cfg.sections.items()},
                    "settings": self.cfg.typed_settings(),
                    "warnings": [],
                    "save_config_pending": self.save_config_pending,
                    "save_config_pending_items":
                        dict(self.save_config_pending_items)}
        if name == "mcu":
            return {"mcu_version": self.version,
                    "mcu_build_versions": "sim-py" + sys.version.split()[0],
                    "mcu_constants": {"ADC_MAX": 1023, "BUS_FREQ": 100000000,
                                      "CLOCK_FREQ": 16000000, "MCU": "sim",
                                      "STATS_SUMSQ_BASE": 256},
                    "last_stats": self._mcu_stats(now)}
        if name == "system_stats":
            return {"sysload": 0.08, "cputime": round(now - self.boot_time, 3),
                    "memavail": 512 * 1024}
        if name == "gcode_move":
            gp = [self.pos[i] - self.gcode_offset[i] - self.homing_origin[i]
                  for i in range(4)]
            return {"speed_factor": self.speed_factor,
                    "speed": self.speed * 60.0,
                    "extrude_factor": self.extrude_factor,
                    "absolute_coordinates": self.absolute_coord,
                    "absolute_extrude": self.absolute_extrude,
                    "homing_origin": list(self.homing_origin),
                    "position": list(self.pos),
                    "gcode_position": gp}
        if name == "toolhead":
            return {"position": list(self.pos),
                    "homed_axes": self.homed_axes,
                    "print_time": max(0.0, now - self.boot_time),
                    "estimated_print_time": max(0.0, now - self.boot_time),
                    "stalls": 0,
                    "extruder": "extruder" if "extruder" in self.heaters else "",
                    "max_velocity": self.max_velocity,
                    "max_accel": self.max_accel,
                    "minimum_cruise_ratio": self.min_cruise_ratio,
                    "square_corner_velocity": self.square_corner_velocity,
                    "axis_minimum": list(self.axis_min),
                    "axis_maximum": list(self.axis_max)}
        if name in self.heaters:
            return self._heater_status(name)
        if name.startswith("temperature_sensor "):
            return self.sensors[name.split(" ", 1)[1]].status()
        if name == "fan":
            return {"speed": self.fans.get("fan", 0.0), "rpm": None}
        if name.startswith("fan_generic "):
            n = name.split(" ", 1)[1]
            return {"speed": self.fans.get(n, 0.0), "rpm": None}
        if name.startswith("heater_fan "):
            n = name.split(" ", 1)[1]
            hf = self.heater_fans[n]
            return {"speed": hf["speed"], "rpm": None,
                    "temperature": round(self._linked_heater_temp(hf), 2)}
        if name.startswith("controller_fan "):
            n = name.split(" ", 1)[1]
            cf = self.controller_fans[n]
            return {"speed": cf["speed"], "rpm": None}
        if name.startswith("filament_switch_sensor ") or \
                name.startswith("filament_motion_sensor "):
            n = name.split(" ", 1)[1]
            fs = self.filament_sensors[n]
            return {"enabled": fs["enabled"],
                    "filament_detected": fs["detected"]}
        if name.startswith("output_pin "):
            n = name.split(" ", 1)[1]
            return {"value": self.output_pins.get(n, 0.0)}
        if name in ("probe", "bltouch") and self.probe_cfg:
            return {"name": self.probe_cfg, "z_offset": self.probe_z_offset,
                    "last_query": self.probe_last_query,
                    "last_probe_position": self.probe_last_pos,
                    "last_z_result": self.probe_last_z}
        if name == "bed_mesh" and self.has_bed_mesh:
            return copy.deepcopy(self.bed_mesh)
        if name == "exclude_object" and self.has_exclude:
            return {"objects": self.excl_objects,
                    "excluded_objects": self.excl_excluded,
                    "current_object": self.excl_current}
        if name == "manual_probe" and self.has_manual_probe:
            return {"is_active": self.manual_probe_active}
        if name == "firmware_retraction" and self.has_fw_retract:
            return dict(self.fw_retract)
        if name == "virtual_sdcard":
            return self.sd.status()
        if name == "print_stats":
            total = print_dur = 0.0
            if self.print_start_time is not None:
                total = now - self.print_start_time
                paused = self.paused_total
                if self.pause_start_time is not None:
                    paused += now - self.pause_start_time
                print_dur = max(0.0, total - paused)
            return {"filename": self.print_filename,
                    "total_duration": round(total, 3),
                    "print_duration": round(print_dur, 3),
                    "filament_used": round(self.filament_used, 3),
                    "state": self.print_state,
                    "message": self.print_message,
                    "info": {"total_layer": self.total_layer,
                             "current_layer": self.current_layer}}
        if name == "display_status":
            prog = self.disp_progress
            if prog is not None and now > self.disp_progress_expire:
                prog = None
                self.disp_progress = None
            if prog is None:
                prog = self.sd.progress()
            return {"progress": round(prog, 6), "message": self.disp_message}
        if name == "pause_resume":
            return {"is_paused": self.paused}
        if name == "idle_timeout":
            pt = 0.0
            if self.idle_state == "Printing" and self.print_start_time:
                pt = now - self.print_start_time
            return {"state": self.idle_state, "printing_time": round(pt, 3),
                    "idle_timeout": self.idle_timeout}
        if name == "query_endstops":
            return {"last_query": dict(self.endstop_last)}
        if name.startswith("gcode_macro "):
            m = self.macros.get(name.split(" ", 1)[1].upper())
            return dict(m["variables"]) if m else {}
        return None

    def _mcu_stats(self, now):
        kb = int((now - self.boot_time) * 512)
        return {"bytes_read": kb, "bytes_retransmit": 0, "freq": 16000000,
                "bytes_write": int(kb * 0.6), "bytes_invalid": 0,
                "send_seq": 99, "receive_seq": 99, "retransmit_seq": 0,
                "srtt": 0.001, "rttvar": 0.0002, "rto": 0.025,
                "ready_bytes": 0, "upcoming_bytes": 0,
                "mcu_awake": 0.01, "mcu_task_avg": 0.0001,
                "mcu_task_stddev": 0.00002}

    def _command_help_status(self):
        cmds = {}
        for c in sorted(GCODE_HELP):
            cmds[c] = {"help": GCODE_HELP[c]}
        for name, m in sorted(self.macros.items()):
            cmds[name] = {"help": m["desc"]}
        return cmds

    def _linked_heater_temp(self, hf):
        temps = [self.heaters[h].reading() for h in hf["heater"]
                 if h in self.heaters]
        return max(temps) if temps else AMBIENT

    # ---------------- 模拟 tick ----------------
    def tick(self, dt):
        with self.lock:
            now = time.monotonic()
            if self.state == "startup" and now >= self.ready_at:
                self.state = "ready"
                self.state_message = "Printer is ready"
                logging.info("Printer is ready")
            for h in self.heaters.values():
                h.tick(dt)
            for name, s in self.sensors.items():
                s.tick(dt)
            # heater_fan / controller_fan 联动
            for hf in self.heater_fans.values():
                t = self._linked_heater_temp(hf)
                hf["speed"] = hf["fan_speed"] if t > hf["temp"] else 0.0
            for cf in self.controller_fans.values():
                active = any(h.target > 0 or h.reading() > 50
                             for h in self.heaters.values())
                cf["speed"] = cf["fan_speed"] if active else 0.0
            # 打印走带
            if self.state == "ready":
                self.sd.tick(dt)
            # idle_timeout
            if self.print_state == "printing":
                self.idle_state = "Printing"
                self.last_activity = now
            elif self.print_state in ("paused",):
                self.idle_state = "Printing"
            elif now - self.last_activity < self.idle_timeout:
                self.idle_state = "Ready"
            else:
                self.idle_state = "Idle"

    # ---------------- 打印生命周期 ----------------
    def start_print(self, filename):
        with self.lock:
            if self.state != "ready":
                raise PrinterError("Printer is not ready")
            self.sd.reset()
            self.sd.load(filename)
            self.print_filename = filename
            self.print_state = "printing"
            self.print_message = ""
            self.paused = False
            self.print_start_time = time.monotonic()
            self.pause_start_time = None
            self.paused_total = 0.0
            self.filament_used = 0.0
            self.sd_byte_budget = 0.0
            self.idle_state = "Printing"
            self.last_activity = time.monotonic()
            self.excl_current = None
            self.excl_excluded = []
            self.respond("File opened: %s Size: %d"
                         % (filename, self.sd.file_size))
            self.respond("File selected")

    def pause_print(self):
        with self.lock:
            if self.print_state == "printing":
                self.paused = True
                self.pause_start_time = time.monotonic()
                self.print_state = "paused"
                self.respond("// Print paused")

    def resume_print(self):
        with self.lock:
            if self.paused:
                self.paused = False
                if self.pause_start_time is not None:
                    self.paused_total += time.monotonic() - self.pause_start_time
                    self.pause_start_time = None
                if self.print_state == "paused":
                    self.print_state = "printing"
                self.respond("// Print resumed")

    def cancel_print(self):
        with self.lock:
            if self.print_state in ("printing", "paused"):
                self.sd.is_active = False
                self.finish_print("cancelled", "")

    def finish_print(self, state, msg):
        self.print_state = state
        self.print_message = msg
        self.paused = False
        self.pause_start_time = None
        self.idle_state = "Ready"
        self.last_activity = time.monotonic()
        if state == "complete":
            self.respond("Done printing file")
        elif state == "cancelled":
            self.respond("// Print cancelled")

    # ---------------- gcode 执行 ----------------
    def respond(self, msg):
        server = getattr(self, "server", None)
        if server:
            server.broadcast_gcode_response(msg)

    def respond_info(self, msg):
        self.respond("// " + msg)

    def run_gcode_line(self, line, from_sd=False):
        """执行单行 gcode（宏展开由调用方处理）"""
        # 去掉行号/校验和/注释
        line = re.sub(r"\s*;.*$", "", line).strip()
        line = re.sub(r"^N\d+\s+", "", line)
        line = re.sub(r"\*\d+$", "", line).strip()
        if not line:
            return
        parts = line.split(None, 1)
        cmd = parts[0].upper()
        rest = parts[1] if len(parts) > 1 else ""
        params = {}
        for m in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\"[^\"]*\"|\S+)",
                             rest):
            v = m.group(2)
            if v.startswith('"') and v.endswith('"'):
                v = v[1:-1]
            params[m.group(1).upper()] = v
        # 传统单字母参数（G1 X10 Y20 F3000）
        if re.match(r"^[GMT]\d+", cmd):
            for m in re.finditer(r"([A-Za-z])(-?\d+(?:\.\d+)?)", rest):
                params.setdefault(m.group(1).upper(), m.group(2))

        handler = getattr(self, "gc_" + cmd.replace(".", "_"), None)
        if handler is not None:
            handler(params, rest, from_sd)
            return
        if cmd in self.macros:
            self._run_macro(cmd, params)
            return
        raise PrinterError("Unknown command:\"%s\"" % cmd)

    def _run_macro(self, name, params):
        m = self.macros[name]
        body = m["body"]
        # 极简模板：替换 {params.X|default("..")} 与 {printer...} 为占位
        def sub(mo):
            expr = mo.group(1).strip()
            pm = re.match(r"params\.(\w+)(?:\s*\|\s*default\(([^)]*)\))?", expr)
            if pm:
                key = pm.group(1).upper()
                if key in params:
                    return str(params[key])
                d = pm.group(2)
                if d is not None:
                    return d.strip().strip('"').strip("'")
                return ""
            return "0"
        body = re.sub(r"\{([^{}]*)\}", sub, body)
        # 去掉 jinja 控制行
        lines = [ln for ln in body.splitlines()
                 if not ln.strip().startswith("{%")]
        for ln in lines:
            ln = ln.strip()
            if not ln or ln.startswith((";", "#")):
                continue
            for sub_ln in ln.split("\n"):
                self.run_gcode_line(sub_ln)

    # --- 运动 ---
    def gc_G28(self, params, rest, from_sd):
        axes = [a for a in ("X", "Y", "Z") if a in params] or ["X", "Y", "Z"]
        time.sleep(0.9)
        with self.lock:
            for a in axes:
                if a not in self.homed_axes:
                    self.homed_axes += a.lower()
            self.homed_axes = "".join(sorted(set(self.homed_axes)))
            for a in axes:
                i = "XYZ".index(a)
                self.pos[i] = self.axis_min[i]
            self.pos[2] = max(self.pos[2], 2.0)
        self.last_activity = time.monotonic()

    def gc_G0(self, params, rest, from_sd):
        self._move(params)

    def gc_G1(self, params, rest, from_sd):
        self._move(params)

    def gc_G2(self, params, rest, from_sd):
        self._move(params)

    def gc_G3(self, params, rest, from_sd):
        self._move(params)

    def _move(self, params):
        with self.lock:
            if "F" in params:
                self.speed = float(params["F"]) / 60.0
            for i, a in enumerate(("X", "Y", "Z", "E")):
                if a not in params:
                    continue
                v = float(params[a])
                if a in "XYZ" and a.lower() not in self.homed_axes \
                        and not self.args.lax:
                    raise PrinterError("Must home axis first: %s" % a.lower())
                if a == "E":
                    if not self.absolute_extrude:
                        v = self.pos[3] + v
                    ex = self.heaters.get("extruder")
                    if v > self.pos[3] and ex is not None and \
                            ex.reading() < self.min_extrude_temp and \
                            not self.args.lax:
                        raise PrinterError(
                            "Extrude below minimum temp\n"
                            "See the 'min_extrude_temp' config option")
                    delta = v - self.pos[3]
                    if delta > 0 and self.print_state == "printing":
                        self.filament_used += delta * self.extrude_factor
                    self.pos[3] = v
                else:
                    if not self.absolute_coord:
                        v = self.pos[i] + v
                    # gcode 坐标 -> 机器坐标
                    self.pos[i] = v + self.gcode_offset[i] + self.homing_origin[i]
            self.last_activity = time.monotonic()

    def gc_G4(self, params, rest, from_sd):
        ms = float(params.get("P", params.get("S", 0)))
        if "S" in params:
            ms = ms * 1000.0
        time.sleep(min(ms / 1000.0, 5.0))

    def gc_G90(self, params, rest, from_sd):
        self.absolute_coord = True

    def gc_G91(self, params, rest, from_sd):
        self.absolute_coord = False

    def gc_G92(self, params, rest, from_sd):
        with self.lock:
            for i, a in enumerate(("X", "Y", "Z", "E")):
                if a in params:
                    # 当前 gcode 逻辑位置设为 v：调整 offset
                    want = float(params[a])
                    self.gcode_offset[i] = self.pos[i] - self.homing_origin[i] - want

    def gc_G10(self, params, rest, from_sd):
        pass  # firmware retract

    def gc_G11(self, params, rest, from_sd):
        pass

    def gc_M82(self, params, rest, from_sd):
        self.absolute_extrude = True

    def gc_M83(self, params, rest, from_sd):
        self.absolute_extrude = False

    def gc_M400(self, params, rest, from_sd):
        pass

    # --- 温度 ---
    def _set_heater(self, name, target, wait=False):
        h = self.heaters.get(name)
        if h is None:
            raise PrinterError("Unknown heater: %s" % name)
        if target > h.max_temp:
            raise PrinterError("%s target %s above max_temp %s"
                               % (name, target, h.max_temp))
        h.target = float(target)
        self.last_activity = time.monotonic()
        if wait:
            deadline = time.monotonic() + 120.0
            while abs(h.reading() - h.target) > 1.0:
                if time.monotonic() > deadline:
                    break
                if self.state != "ready":
                    raise PrinterError("Printer is shutdown")
                time.sleep(0.1)

    def gc_M104(self, params, rest, from_sd):
        self._set_heater("extruder", float(params.get("S", 0.0)))

    def gc_M109(self, params, rest, from_sd):
        self._set_heater("extruder", float(params.get("S", 0.0)), wait=True)

    def gc_M140(self, params, rest, from_sd):
        self._set_heater("heater_bed", float(params.get("S", 0.0)))

    def gc_M190(self, params, rest, from_sd):
        self._set_heater("heater_bed", float(params.get("S", 0.0)), wait=True)

    def gc_M141(self, params, rest, from_sd):
        for name in self.sensors:
            pass

    def gc_SET_HEATER_TEMPERATURE(self, params, rest, from_sd):
        name = params.get("HEATER", "extruder")
        self._set_heater(name, float(params.get("TARGET", 0.0)))

    def gc_TURN_OFF_HEATERS(self, params, rest, from_sd):
        for h in self.heaters.values():
            h.target = 0.0

    # --- 风扇/引脚 ---
    def gc_M106(self, params, rest, from_sd):
        s = float(params.get("S", 255.0))
        if "fan" in self.fans:
            self.fans["fan"] = max(0.0, min(1.0, s / 255.0))

    def gc_M107(self, params, rest, from_sd):
        if "fan" in self.fans:
            self.fans["fan"] = 0.0

    def gc_SET_FAN_SPEED(self, params, rest, from_sd):
        n = params.get("FAN", "")
        if n in self.fans:
            self.fans[n] = max(0.0, min(1.0, float(params.get("SPEED", 0))))
        else:
            raise PrinterError("Unknown fan: %s" % n)

    def gc_SET_PIN(self, params, rest, from_sd):
        n = params.get("PIN", "")
        if n in self.output_pins:
            self.output_pins[n] = float(params.get("VALUE", 0))
        else:
            raise PrinterError("Unknown pin: %s" % n)

    # --- 查询/响应 ---
    def gc_M114(self, params, rest, from_sd):
        p = self.pos
        self.respond("X:%.3f Y:%.3f Z:%.3f E:%.3f" % (p[0], p[1], p[2], p[3]))

    def gc_M117(self, params, rest, from_sd):
        self.disp_message = rest.split("=", 1)[-1] if "=" in rest else rest

    def gc_M118(self, params, rest, from_sd):
        msg = rest
        if "=" in rest:
            msg = rest.split("=", 1)[1]
        self.respond(msg)

    def gc_RESPOND(self, params, rest, from_sd):
        msg = params.get("MSG", rest.split("MSG=", 1)[-1] if "MSG" in rest
                         else rest)
        prefix = params.get("PREFIX", "// ")
        if params.get("TYPE"):
            prefix = {"error": "!! ", "echo": "", "command": "// ",
                      "warning": "// "}.get(params["TYPE"], "// ")
        self.respond(prefix + msg.strip('"'))

    def gc_M73(self, params, rest, from_sd):
        if "P" in params:
            self.disp_progress = max(0.0, min(1.0, float(params["P"]) / 100.0))
            self.disp_progress_expire = time.monotonic() + 5.0

    def gc_QUERY_ENDSTOPS(self, params, rest, from_sd):
        with self.lock:
            out = []
            for i, a in enumerate(("x", "y", "z")):
                triggered = 1 if abs(self.pos[i] - self.axis_min[i]) < 0.01 else 0
                self.endstop_last[a] = triggered
                out.append("%s:%s" % (a, "TRIGGERED" if triggered else "open"))
            self.respond(" ".join(out))

    def gc_QUERY_PROBE(self, params, rest, from_sd):
        if not self.probe_cfg:
            raise PrinterError("Unknown command:\"QUERY_PROBE\"")
        st = "TRIGGERED" if self.probe_last_query else "open"
        self.respond("%s: %s" % (self.probe_cfg, st))

    def gc_PROBE(self, params, rest, from_sd):
        if not self.probe_cfg:
            raise PrinterError("Unknown command:\"PROBE\"")
        z = round(random.uniform(-0.05, 0.05), 3)
        with self.lock:
            self.pos[2] = 0.0
            self.probe_last_z = z
            self.probe_last_pos = list(self.pos[:3])
        self.respond_info("probe at %.3f,%.3f is z=%.6f"
                          % (self.pos[0], self.pos[1], z))

    def gc_PROBE_CALIBRATE(self, params, rest, from_sd):
        self.probe_z_offset = 0.0
        self.respond_info("PROBE_CALIBRATE at %.3f,%.3f: use TESTZ/ACCEPT"
                          % (self.pos[0], self.pos[1]))

    def gc_TESTZ(self, params, rest, from_sd):
        pass

    def gc_ACCEPT(self, params, rest, from_sd):
        pass

    def gc_ABORT(self, params, rest, from_sd):
        self.emergency_stop("ABORT command issued")

    def gc_Z_OFFSET_APPLY_PROBE(self, params, rest, from_sd):
        self.homing_origin[2] = -self.probe_z_offset

    def gc_Z_OFFSET_APPLY_ENDSTOP(self, params, rest, from_sd):
        pass

    def gc_BED_MESH_CALIBRATE(self, params, rest, from_sd):
        if not self.has_bed_mesh:
            raise PrinterError("Unknown command:\"BED_MESH_CALIBRATE\"")
        time.sleep(1.5)
        nx, ny = 7, 7
        random.seed(42)
        mat = [[round(random.uniform(-0.15, 0.15)
                     + 0.08 * math.sin(x * 0.9) + 0.06 * math.cos(y * 0.8), 3)
                for x in range(nx)] for y in range(ny)]
        mn = (self.axis_min[0] + 10, self.axis_min[1] + 10)
        mx = (self.axis_max[0] - 10, self.axis_max[1] - 10)
        self.bed_mesh.update({
            "profile_name": "default",
            "mesh_min": list(mn), "mesh_max": list(mx),
            "probed_matrix": mat, "mesh_matrix": mat,
            "profiles": {**self._persisted_mesh_profiles,
                         "default": {"points": mat,
                                     "mesh_params": {"min_x": mn[0],
                                                     "min_y": mn[1],
                                                     "max_x": mx[0],
                                                     "max_y": mx[1]}}}})
        self.respond_info("Mesh Bed Leveling complete")

    def gc_BED_MESH_PROFILE(self, params, rest, from_sd):
        if not self.has_bed_mesh:
            raise PrinterError("Unknown command:\"BED_MESH_PROFILE\"")
        profiles = self.bed_mesh["profiles"]
        if "SAVE" in params:
            nm = params["SAVE"]
            profiles[nm] = {"points": self.bed_mesh["probed_matrix"],
                            "mesh_params": {
                                "min_x": self.bed_mesh["mesh_min"][0],
                                "min_y": self.bed_mesh["mesh_min"][1],
                                "max_x": self.bed_mesh["mesh_max"][0],
                                "max_y": self.bed_mesh["mesh_max"][1]}}
            self.respond_info("Profile '%s' saved" % nm)
        elif "LOAD" in params:
            nm = params["LOAD"]
            prof = profiles.get(nm) or self._persisted_mesh_profiles.get(nm)
            if prof is None:
                raise PrinterError("Unknown bed mesh profile '%s'" % nm)
            pts = prof.get("points", [[]])
            mp = prof.get("mesh_params", {})
            self.bed_mesh.update({
                "profile_name": nm, "probed_matrix": pts,
                "mesh_matrix": pts,
                "mesh_min": [mp.get("min_x", 0), mp.get("min_y", 0)],
                "mesh_max": [mp.get("max_x", 0), mp.get("max_y", 0)]})
            self.respond_info("Profile '%s' loaded" % nm)
        elif "REMOVE" in params:
            nm = params["REMOVE"]
            profiles.pop(nm, None)
            self._persisted_mesh_profiles.pop(nm, None)
            self.respond_info("Profile '%s' removed" % nm)

    # --- 速度/流量/其他设置 ---
    def gc_M220(self, params, rest, from_sd):
        self.speed_factor = float(params.get("S", 100)) / 100.0

    def gc_M221(self, params, rest, from_sd):
        self.extrude_factor = float(params.get("S", 100)) / 100.0

    def gc_SET_VELOCITY_LIMIT(self, params, rest, from_sd):
        if "VELOCITY" in params:
            self.max_velocity = float(params["VELOCITY"])
        if "ACCEL" in params:
            self.max_accel = float(params["ACCEL"])
        if "MINIMUM_CRUISE_RATIO" in params:
            self.min_cruise_ratio = float(params["MINIMUM_CRUISE_RATIO"])
        if "SQUARE_CORNER_VELOCITY" in params:
            self.square_corner_velocity = float(params["SQUARE_CORNER_VELOCITY"])

    def gc_SET_PRESSURE_ADVANCE(self, params, rest, from_sd):
        if "ADVANCE" in params:
            self.pressure_advance = float(params["ADVANCE"])
        if "SMOOTH_TIME" in params:
            self.smooth_time = float(params["SMOOTH_TIME"])

    def gc_SET_GCODE_OFFSET(self, params, rest, from_sd):
        moves = {"X": 0, "Y": 1, "Z": 2}
        for k, i in moves.items():
            if k in params:
                self.homing_origin[i] = float(params[k])
            if k + "_ADJUST" in params:
                self.homing_origin[i] += float(params[k + "_ADJUST"])

    def gc_SET_IDLE_TIMEOUT(self, params, rest, from_sd):
        if "TIMEOUT" in params:
            self.idle_timeout = float(params["TIMEOUT"])

    def gc_SET_GCODE_VARIABLE(self, params, rest, from_sd):
        mac = params.get("MACRO", "").upper()
        var = params.get("VARIABLE", "")
        if mac not in self.macros:
            raise PrinterError("Unknown gcode_macro '%s'" % mac)
        m = self.macros[mac]
        if var not in m["variables"]:
            raise PrinterError("Unknown gcode_macro variable '%s'" % var)
        m["variables"][var] = _to_number(params.get("VALUE", "0"))

    def gc_SET_FILAMENT_SENSOR(self, params, rest, from_sd):
        n = params.get("SENSOR", "")
        if n not in self.filament_sensors:
            raise PrinterError("Unknown filament sensor: %s" % n)
        if "ENABLE" in params:
            self.filament_sensors[n]["enabled"] = bool(int(params["ENABLE"]))

    def gc_QUERY_FILAMENT_SENSOR(self, params, rest, from_sd):
        n = params.get("SENSOR", "")
        fs = self.filament_sensors.get(n)
        if fs is None:
            raise PrinterError("Unknown filament sensor: %s" % n)
        self.respond_info("Filament Sensor %s: filament %s"
                          % (n, "detected" if fs["detected"] else "not detected"))

    def gc_SET_RETRACTION(self, params, rest, from_sd):
        for k_src, k_dst in (("RETRACT_LENGTH", "retract_length"),
                             ("RETRACT_SPEED", "retract_speed"),
                             ("UNRETRACT_EXTRA_LENGTH", "unretract_extra_length"),
                             ("UNRETRACT_SPEED", "unretract_speed")):
            if k_src in params:
                self.fw_retract[k_dst] = float(params[k_src])

    def gc_EXCLUDE_OBJECT(self, params, rest, from_sd):
        if not self.has_exclude:
            raise PrinterError("Unknown command:\"EXCLUDE_OBJECT\"")
        if "NAME" in params:
            nm = params["NAME"]
            if nm not in self.excl_excluded:
                self.excl_excluded.append(nm)
            if self.excl_current == nm:
                self.excl_current = None
        if "CURRENT" in params:
            self.excl_current = params["CURRENT"]
        if params.get("RESET"):
            self.excl_objects = []
            self.excl_excluded = []
            self.excl_current = None

    def gc_EXCLUDE_OBJECT_DEFINE(self, params, rest, from_sd):
        pass

    def gc_SET_PRINT_STATS_INFO(self, params, rest, from_sd):
        if "TOTAL_LAYER" in params:
            self.total_layer = int(float(params["TOTAL_LAYER"]))
        if "CURRENT_LAYER" in params:
            self.current_layer = int(float(params["CURRENT_LAYER"]))
        if "FILAMENT_TOTAL" in params:
            self.filament_total = float(params["FILAMENT_TOTAL"])

    # --- SD 卡/打印控制 ---
    def gc_SDCARD_PRINT_FILE(self, params, rest, from_sd):
        fn = params.get("FILENAME", "")
        m = re.search(r'FILENAME\s*=\s*"([^"]*)"', rest)
        if m:
            fn = m.group(1)
        if not fn:
            raise PrinterError("FILENAME required")
        self.start_print(fn)

    def gc_SDCARD_RESET_FILE(self, params, rest, from_sd):
        if self.print_state in ("printing", "paused"):
            self.finish_print("cancelled", "")
        self.sd.reset()

    def gc_M21(self, params, rest, from_sd):
        self.respond("SD card ok")

    def gc_M23(self, params, rest, from_sd):
        pass

    def gc_M24(self, params, rest, from_sd):
        self.resume_print()

    def gc_M25(self, params, rest, from_sd):
        self.pause_print()

    def gc_PAUSE(self, params, rest, from_sd):
        if "PAUSE" in self.macros:
            self._run_macro("PAUSE", {})
        self.pause_print()

    def gc_RESUME(self, params, rest, from_sd):
        if "RESUME" in self.macros:
            self._run_macro("RESUME", {})
        self.resume_print()

    def gc_CANCEL_PRINT(self, params, rest, from_sd):
        if "CANCEL_PRINT" in self.macros:
            self._run_macro("CANCEL_PRINT", {})
        if self.print_state in ("printing", "paused"):
            self.cancel_print()

    # --- 系统 ---
    def gc_M84(self, params, rest, from_sd):
        pass

    def gc_M18(self, params, rest, from_sd):
        pass

    def gc_M112(self, params, rest, from_sd):
        self.emergency_stop("Emergency stop requested")

    def gc_SAVE_CONFIG(self, params, rest, from_sd):
        with self.lock:
            self.save_config_pending = True
            items = {}
            if self.probe_z_offset:
                items["probe"] = {"z_offset": self.probe_z_offset}
            if self.bed_mesh.get("profiles"):
                items["bed_mesh default"] = {}
            self.save_config_pending_items = items
            self._save_persist()
        self.respond_info("SAVE_CONFIG will update the printer config file\n"
                          "and restart the printer.")
        threading.Timer(0.8, self.request_restart).start()

    def gc_RESTART(self, params, rest, from_sd):
        self.respond_info("Restarting printer")
        threading.Timer(0.3, self.request_restart).start()

    def gc_FIRMWARE_RESTART(self, params, rest, from_sd):
        self.respond_info("Firmware restarting")
        threading.Timer(0.3, self.request_restart).start()

    def gc_FIRMWARE_STATUS(self, params, rest, from_sd):
        self.respond_info("Klipper firmware: simulated (fake_klippy)")

    def gc_M115(self, params, rest, from_sd):
        self.respond("FIRMWARE_NAME:Klipper FIRMWARE_VERSION:%s "
                     "MACHINE:Simulated" % self.version)

    def gc_HELP(self, params, rest, from_sd):
        self.respond_info("Available commands: " + ", ".join(
            sorted(GCODE_HELP.keys())))

    # --- 状态机 ---
    def emergency_stop(self, reason):
        with self.lock:
            if self.state == "shutdown":
                return
            self.state = "shutdown"
            self.state_message = reason
            logging.warning("EMERGENCY STOP: %s", reason)
            self.respond("!! %s" % reason)

    def request_restart(self):
        srv = getattr(self, "server", None)
        if srv:
            srv.request_restart()

GCODE_HELP = {
    "G0": "Linear move", "G1": "Linear move", "G2": "Arc move",
    "G3": "Arc move", "G4": "Dwell", "G10": "Retract", "G11": "Unretract",
    "G28": "Home", "G90": "Absolute positioning",
    "G91": "Relative positioning", "G92": "Set position",
    "M18": "Disable motors", "M82": "Absolute extrusion",
    "M83": "Relative extrusion", "M84": "Disable motors",
    "M104": "Set hotend temperature", "M105": "Get temperatures",
    "M106": "Set fan speed", "M107": "Fan off",
    "M109": "Wait for hotend temperature", "M112": "Emergency stop",
    "M114": "Get position", "M115": "Get firmware info",
    "M117": "Set display message", "M118": "Send message",
    "M140": "Set bed temperature", "M141": "Set chamber temperature",
    "M190": "Wait for bed temperature", "M220": "Set speed factor",
    "M221": "Set extrude factor", "M400": "Wait for moves",
    "M73": "Set print progress", "M21": "Init SD card",
    "M23": "Select SD file", "M24": "Resume SD print", "M25": "Pause SD print",
    "RESPOND": "Send message to host",
    "QUERY_ENDSTOPS": "Report endstop status",
    "QUERY_PROBE": "Report probe status",
    "QUERY_FILAMENT_SENSOR": "Report filament sensor status",
    "PROBE": "Probe Z", "PROBE_CALIBRATE": "Calibrate probe z offset",
    "TESTZ": "Adjust probe test height", "ACCEPT": "Accept calibration",
    "ABORT": "Abort and shutdown",
    "BED_MESH_CALIBRATE": "Run bed mesh calibration",
    "BED_MESH_PROFILE": "Manage bed mesh profiles",
    "Z_OFFSET_APPLY_PROBE": "Apply probe z offset",
    "Z_OFFSET_APPLY_ENDSTOP": "Apply endstop z offset",
    "SET_GCODE_OFFSET": "Set gcode offset",
    "SET_HEATER_TEMPERATURE": "Set heater target",
    "SET_FAN_SPEED": "Set fan speed", "SET_PIN": "Set output pin",
    "SET_VELOCITY_LIMIT": "Set velocity limits",
    "SET_PRESSURE_ADVANCE": "Set pressure advance",
    "SET_IDLE_TIMEOUT": "Set idle timeout",
    "SET_GCODE_VARIABLE": "Set macro variable",
    "SET_FILAMENT_SENSOR": "Configure filament sensor",
    "SET_RETRACTION": "Set firmware retraction",
    "SET_PRINT_STATS_INFO": "Set print stats info",
    "EXCLUDE_OBJECT": "Exclude object from print",
    "EXCLUDE_OBJECT_DEFINE": "Define printable objects",
    "SDCARD_PRINT_FILE": "Start SD print", "SDCARD_RESET_FILE": "Reset SD",
    "PAUSE": "Pause print", "RESUME": "Resume print",
    "CANCEL_PRINT": "Cancel print",
    "TURN_OFF_HEATERS": "Turn off all heaters",
    "SAVE_CONFIG": "Save config and restart",
    "RESTART": "Restart host", "FIRMWARE_RESTART": "Firmware restart",
    "FIRMWARE_STATUS": "Report firmware status",
    "HELP": "Show command help",
}

# ---------------------------------------------------------------- JSON-RPC 服务

ENDPOINTS = ["info", "emergency_stop", "list_endpoints",
             "register_remote_method", "gcode/help", "gcode/script",
             "gcode/restart", "gcode/firmware_restart",
             "gcode/subscribe_output", "objects/list", "objects/query",
             "objects/subscribe", "pause_resume/pause", "pause_resume/resume",
             "pause_resume/cancel", "query_endstops/status"]

class Client:
    def __init__(self, server, conn, addr):
        self.server = server
        self.printer = server.printer
        self.conn = conn
        self.addr = addr
        self.send_lock = threading.Lock()
        self.partial = b""
        self.closed = False
        self.subscriptions = {}        # obj -> None|fields
        self.sub_template = {"method": "process_status_update"}
        self.last_sent = {}            # obj -> {field: value}
        self.gcode_output = False
        self.gcode_template = {"method": "process_gcode_response"}
        self.remote_methods = {}       # name -> template

    def send(self, obj):
        data = json.dumps(obj, separators=(",", ":"),
                          default=str).encode() + ETX
        with self.send_lock:
            if self.closed:
                return
            try:
                self.conn.sendall(data)
            except OSError:
                self.close()

    def close(self):
        if self.closed:
            return
        self.closed = True
        try:
            self.conn.close()
        except OSError:
            pass

    def run(self):
        try:
            while not self.closed and not self.server.restarting:
                try:
                    data = self.conn.recv(65536)
                except OSError:
                    break
                if not data:
                    break
                self.partial += data
                while ETX in self.partial:
                    req, self.partial = self.partial.split(ETX, 1)
                    if req.strip():
                        self.handle_request(req)
        finally:
            self.close()
            self.server.remove_client(self)

    def handle_request(self, raw):
        try:
            req = json.loads(raw)
            rid = req.get("id")
            method = req.get("method")
            params = req.get("params", {}) or {}
        except Exception:
            logging.warning("bad request: %r", raw[:200])
            return
        try:
            result = self.dispatch(method, params)
            if rid is not None:
                self.send({"id": rid, "result": result if result is not None
                           else {}})
        except PrinterError as e:
            if rid is not None:
                self.send({"id": rid, "error": {"error": "WebRequestError",
                                                "message": str(e)}})
        except Exception as e:
            logging.exception("internal error handling %s", method)
            if rid is not None:
                self.send({"id": rid, "error": {
                    "error": "WebRequestError",
                    "message": "Internal Error on WebRequest: %s (%s)"
                               % (method, e)}})

    # ---------------- endpoints ----------------
    def dispatch(self, method, params):
        p = self.printer
        if method == "info":
            ci = params.get("client_info")
            if ci:
                logging.info("client %s identified: %s", self.addr, ci)
            return p.info_response()
        if method == "list_endpoints":
            return {"endpoints": ENDPOINTS}
        if method == "emergency_stop":
            p.emergency_stop("Shutdown due to webhooks request")
            return {}
        if method == "register_remote_method":
            name = params.get("remote_method")
            tmpl = params.get("response_template",
                              {"method": params.get("remote_method")})
            if name:
                self.remote_methods[name] = tmpl
                logging.info("client %s registered remote method %s",
                             self.addr, name)
            return {}
        if method == "gcode/help":
            return p._command_help_status()
        if method == "gcode/script":
            script = params.get("script", "")
            if p.state == "shutdown" and not re.match(
                    r"^\s*(RESTART|FIRMWARE_RESTART)\s*$", script, re.I):
                raise PrinterError("Printer is shutdown")
            if p.state != "ready" and p.state != "shutdown":
                raise PrinterError("Printer is not ready")
            try:
                for line in script.splitlines():
                    line = line.strip()
                    if line:
                        p.run_gcode_line(line)
            except PrinterError as e:
                p.respond("!! %s" % e)
                raise
            return {}
        if method == "gcode/restart":
            threading.Timer(0.2, p.request_restart).start()
            return {}
        if method == "gcode/firmware_restart":
            threading.Timer(0.2, p.request_restart).start()
            return {}
        if method == "gcode/subscribe_output":
            self.gcode_output = True
            self.gcode_template = params.get(
                "response_template", {"method": "process_gcode_response"})
            return {}
        if method == "objects/list":
            return {"objects": p.object_names()}
        if method == "objects/query":
            return self.handle_query(params.get("objects", {}))
        if method == "objects/subscribe":
            objs = params.get("objects", {}) or {}
            self.sub_template = params.get(
                "response_template", {"method": "process_status_update"})
            with self.server.sub_lock:
                self.subscriptions = objs
                self.last_sent = {}
            return self.handle_query(objs)
        if method == "pause_resume/pause":
            p.gc_PAUSE({}, "", False)
            return "ok"
        if method == "pause_resume/resume":
            p.gc_RESUME({}, "", False)
            return "ok"
        if method == "pause_resume/cancel":
            p.gc_CANCEL_PRINT({}, "", False)
            return "ok"
        if method == "query_endstops/status":
            return {"last_query": dict(p.endstop_last)}
        raise PrinterError("webhooks: No registered callback for path '%s'"
                           % method)

    def handle_query(self, objects):
        p = self.printer
        now = time.monotonic()
        status = {}
        with p.lock:
            for obj, fields in objects.items():
                st = p.get_object_status(obj)
                if st is None:
                    st = {}
                if fields is not None:
                    st = {k: st.get(k) for k in fields}
                status[obj] = st
        return {"eventtime": now, "status": status}

    # ---------------- 订阅推送 ----------------
    def push_subscription_diffs(self):
        if not self.subscriptions or self.closed:
            return
        p = self.printer
        now = time.monotonic()
        diff = {}
        with p.lock:
            for obj, fields in self.subscriptions.items():
                st = p.get_object_status(obj)
                if st is None:
                    st = {}
                if fields is not None:
                    st = {k: st.get(k) for k in fields}
                last = self.last_sent.get(obj, {})
                changed = {k: v for k, v in st.items()
                           if k not in last or last[k] != v}
                if changed:
                    diff[obj] = changed
                    self.last_sent.setdefault(obj, {}).update(
                        copy.deepcopy(changed))
        if diff:
            msg = dict(self.sub_template)
            msg["params"] = {"eventtime": now, "status": diff}
            self.send(msg)

    def push_gcode_response(self, line):
        if not self.gcode_output or self.closed:
            return
        msg = dict(self.gcode_template)
        msg["params"] = {"response": line}
        self.send(msg)

class Server:
    def __init__(self, printer, listen):
        self.printer = printer
        printer.server = self
        self.listen = listen
        self.clients = []
        self.clients_lock = threading.Lock()
        self.sub_lock = threading.Lock()
        self.restarting = False
        self.restart_flag = threading.Event()
        self.sock = None

    # ---------------- 生命周期 ----------------
    def open_listener(self):
        if self.listen.startswith("tcp://"):
            hp = self.listen[6:]
            host, _, port = hp.rpartition(":")
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((host or "127.0.0.1", int(port)))
            s.listen(4)
        else:
            try:
                os.remove(self.listen)
            except OSError:
                pass
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.bind(self.listen)
            os.chmod(self.listen, 0o777)
            s.listen(4)
        self.sock = s
        logging.info("fake_klippy listening on %s", self.listen)

    def accept_loop(self):
        while True:
            try:
                conn, addr = self.sock.accept()
            except OSError:
                if self.restarting:
                    return
                raise
            c = Client(self, conn, addr)
            with self.clients_lock:
                self.clients.append(c)
            threading.Thread(target=c.run, daemon=True,
                             name="klippy-client").start()
            logging.info("client connected: %s", addr)

    def remove_client(self, c):
        with self.clients_lock:
            if c in self.clients:
                self.clients.remove(c)

    def broadcast_gcode_response(self, line):
        logging.info("gcode: %s", line)
        with self.clients_lock:
            clients = list(self.clients)
        for c in clients:
            c.push_gcode_response(line)

    def sub_loop(self):
        while True:
            time.sleep(0.25)
            with self.clients_lock:
                clients = list(self.clients)
            for c in clients:
                try:
                    c.push_subscription_diffs()
                except Exception:
                    logging.exception("subscription push failed")

    def sim_loop(self):
        last = time.monotonic()
        while True:
            time.sleep(0.1)
            now = time.monotonic()
            self.printer.tick(now - last)
            last = now

    def request_restart(self):
        if self.restarting:
            return
        threading.Thread(target=self._do_restart, daemon=True).start()

    def _do_restart(self):
        logging.info("restarting fake klippy...")
        self.restarting = True
        with self.clients_lock:
            clients = list(self.clients)
        for c in clients:
            c.close()
        try:
            self.sock.close()
        except OSError:
            pass
        time.sleep(0.6)
        # 重置打印机状态（保留持久化）
        p = self.printer
        with p.lock:
            p.state = "startup"
            p.state_message = "Printer is starting"
            p.boot_time = time.monotonic()
            p.ready_at = p.boot_time + float(p.args.startup_delay)
            p.print_state = "standby"
            p.print_message = ""
            p.print_filename = ""
            p.print_start_time = None
            p.paused = False
            p.sd.reset()
            p.homed_axes = ""
            p.pos = [0.0, 0.0, 0.0, 0.0]
            p.gcode_offset = [0.0, 0.0, 0.0, 0.0]
            p.save_config_pending = False
            p.save_config_pending_items = {}
            for h in p.heaters.values():
                h.target = 0.0
                h.temp = AMBIENT
            for k in p.fans:
                p.fans[k] = 0.0
            p.cfg.reload(p.cfg.path)
            p._build_from_config()
            p._load_persist()
            if "probe" in p.cfg.sections and p.probe_z_offset == 0.0:
                pass
        self.open_listener()
        self.restarting = False
        threading.Thread(target=self.accept_loop, daemon=True).start()
        logging.info("fake_klippy restarted")

    def run(self):
        self.open_listener()
        threading.Thread(target=self.accept_loop, daemon=True).start()
        threading.Thread(target=self.sub_loop, daemon=True).start()
        threading.Thread(target=self.sim_loop, daemon=True).start()
        while True:
            time.sleep(3600)

# ---------------------------------------------------------------- info

def _info_response(self):
    return {
        "state": self.state,
        "state_message": self.state_message,
        "hostname": socket.gethostname(),
        "klipper_path": os.path.abspath(self.args.klipper_repo)
        if self.args.klipper_repo else os.getcwd(),
        "python_path": sys.executable,
        "process_id": os.getpid(),
        "user_id": getattr(os, "getuid", lambda: 1000)(),
        "group_id": getattr(os, "getgid", lambda: 1000)(),
        "log_file": os.path.abspath(self.args.log),
        "config_file": os.path.abspath(self.args.config),
        "software_version": self.version,
        "cpu_info": "Simulated MCU (fake_klippy)",
    }

Printer.info_response = _info_response

# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="Simulated klippy host")
    ap.add_argument("--config", required=True, help="printer.cfg 路径")
    ap.add_argument("--socket", required=True,
                    help="unix socket 路径 或 tcp://host:port")
    ap.add_argument("--gcodes", required=True, help="gcode 文件目录")
    ap.add_argument("--log", required=True, help="klippy.log 路径")
    ap.add_argument("--klipper-repo", default=None,
                    help="klipper 源码目录（取版本号，可选）")
    ap.add_argument("--bps", default=16384, type=int,
                    help="虚拟打印走带速度 bytes/s（默认 16384）")
    ap.add_argument("--startup-delay", default=1.5, type=float)
    ap.add_argument("--lax", action="store_true",
                    help="宽松模式：不检查归位/挤出温度")
    args = ap.parse_args()

    # klippy.log 落到文件 + 控制台
    os.makedirs(os.path.dirname(os.path.abspath(args.log)), exist_ok=True)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        handlers=[logging.FileHandler(args.log, encoding="utf-8"),
                  logging.StreamHandler(sys.stdout)])
    logging.info("Starting fake_klippy...")
    logging.info("config: %s gcodes: %s socket: %s",
                 args.config, args.gcodes, args.socket)

    printer = Printer(args)
    server = Server(printer, args.socket)
    server.run()

if __name__ == "__main__":
    main()
