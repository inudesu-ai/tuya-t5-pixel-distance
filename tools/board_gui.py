# -*- coding: utf-8 -*-
"""Tuya T5AI Pixel 机器狗点阵屏控制台（跨平台）

四个功能页：
1. 表情控制  —— 通过 MQTT 发布 payload 切换屏幕图案
2. 设备配置  —— 修改 WiFi/Broker 等编译期配置，一键构建并刷写固件
3. 串口监控  —— 实时查看设备日志并解析 WiFi/MQTT 状态
4. 设置      —— 自定义 SDK 根目录、应用源码目录、tyutool 路径等，
                保存于 tools/board_gui_settings.json（已被 git 忽略）

支持 Windows / Linux / macOS：
- Windows 构建走 powershell + export.ps1，其余系统走 bash + export.sh
- 刷写工具优先用设置中的自定义路径，其次找 SDK 内置 tyutool_cli(.exe)，
  最后在 PATH 中查找

依赖: pyserial, paho-mqtt (pip install pyserial paho-mqtt)
"""

import glob
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from tkinter import font as tkfont

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None

# ---------------------------------------------------------------- 平台相关
IS_WINDOWS = sys.platform == "win32"

if IS_WINDOWS:
    UI_FONT, MONO_FONT = "Microsoft YaHei", "Microsoft YaHei"
elif sys.platform == "darwin":
    UI_FONT, MONO_FONT = "SF Pro Text", "Menlo"
else:
    UI_FONT, MONO_FONT = "Sans", "Monospace"

# ---------------------------------------------------------------- 设置持久化
WORKSPACE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SETTINGS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "board_gui_settings.json")


def _default_sdk_root():
    """在工作区内自动探测 TuyaOpen* 目录。"""
    candidates = sorted(glob.glob(os.path.join(WORKSPACE, "TuyaOpen*")))
    for path in candidates:
        if os.path.isdir(path):
            return path
    return ""


DEFAULT_SETTINGS = {
    # TuyaOpen SDK 根目录（含 export.ps1/export.sh 与 tos.py）
    "sdk_root": _default_sdk_root(),
    # 工作区内的应用源码目录（构建前会同步到 SDK）
    "app_src_dir": os.path.join(WORKSPACE, "app", "tuya_t5_pixel_distance", "src"),
    # 应用在 SDK 内的相对路径
    "sdk_app_subdir": os.path.join("apps", "tuya_t5_pixel", "tuya_t5_pixel_distance"),
    # 刷写工具路径，留空则自动查找
    "tyutool_path": "",
    # tyutool -d 参数（芯片型号）
    "flash_device": "t5",
    # 记住上次使用的串口/波特率
    "flash_port": "",
    "log_port": "",
    "log_baud": "460800",
}


def load_settings():
    settings = dict(DEFAULT_SETTINGS)
    try:
        with open(SETTINGS_FILE, "r", encoding="utf-8") as f:
            saved = json.load(f)
        if isinstance(saved, dict):
            for key in DEFAULT_SETTINGS:
                if key in saved:
                    settings[key] = saved[key]
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings):
    try:
        with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
            json.dump(settings, f, ensure_ascii=False, indent=2)
        return True
    except OSError as e:
        messagebox.showerror("保存失败", "无法写入 %s\n%s" % (SETTINGS_FILE, e))
        return False


# ---------------------------------------------------------------- 路径推导
def mqtt_display_c(settings):
    return os.path.join(settings["app_src_dir"], "mqtt_display.c")


def wifi_credentials_h(settings):
    """git 忽略的本地 WiFi 凭证头文件。"""
    return os.path.join(settings["app_src_dir"], "wifi_credentials.h")


def sdk_app_dir(settings):
    return os.path.join(settings["sdk_root"], settings["sdk_app_subdir"])


def sdk_src_dir(settings):
    return os.path.join(sdk_app_dir(settings), "src")


def find_qio_bin(settings):
    """在应用 dist 目录内查找最新的 QIO 固件。"""
    pattern = os.path.join(sdk_app_dir(settings), "dist", "**", "*_QIO_*.bin")
    matches = glob.glob(pattern, recursive=True)
    if not matches:
        return None
    return max(matches, key=os.path.getmtime)


def find_tyutool(settings):
    """按优先级查找刷写工具：自定义路径 > SDK 内置 > PATH。"""
    custom = settings.get("tyutool_path", "").strip()
    if custom and os.path.isfile(custom):
        return custom
    exe = "tyutool_cli.exe" if IS_WINDOWS else "tyutool_cli"
    builtin = os.path.join(settings["sdk_root"], "tools", "tyutool", exe)
    if os.path.isfile(builtin):
        return builtin
    return shutil.which("tyutool_cli")


def build_command(settings):
    """按操作系统生成构建命令（source export 脚本后运行 tos.py build）。"""
    sdk = settings["sdk_root"]
    app = sdk_app_dir(settings)
    tos = os.path.join(sdk, "tos.py")
    if IS_WINDOWS:
        script = ("cd '%s'; . .\\export.ps1; cd '%s'; python '%s' build"
                  % (sdk, app, tos))
        ps = (shutil.which("powershell")
              or r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe")
        return [ps, "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-Command", script]
    python = sys.executable or "python3"
    script = ("cd '%s' && . ./export.sh && cd '%s' && '%s' '%s' build"
              % (sdk, app, python, tos))
    return ["bash", "-c", script]


# ---------------------------------------------------------------- 编译期配置
# 编译期配置宏 -> (界面标签, 是否字符串, 所在文件: wifi=凭证头文件 / mqtt=mqtt_display.c)
CONFIG_MACROS = [
    ("MQTT_DISPLAY_WIFI_SSID", "WiFi 名称 (SSID)", True, "wifi"),
    ("MQTT_DISPLAY_WIFI_PSWD", "WiFi 密码", True, "wifi"),
    ("MQTT_DISPLAY_BROKER_HOST", "MQTT Broker 地址", True, "mqtt"),
    ("MQTT_DISPLAY_BROKER_PORT", "MQTT Broker 端口", False, "mqtt"),
    ("MQTT_DISPLAY_TOPIC", "订阅主题 (Topic)", True, "mqtt"),
]

# 表情按钮: (payload, 显示文字, 说明)
EXPRESSIONS = [
    ("forward", "↑ 前进", "绿色上箭头"),
    ("backward", "↓ 后退", "橙色下箭头"),
    ("turn_left", "↶ 左转", "青色左转弧"),
    ("turn_right", "↷ 右转", "青色右转弧"),
    ("heart", "♥ 比心", "红色爱心"),
    ("smile", "☺ 开心", "笑脸 + 音效"),
    ("idle", "◌ 待机", "恢复距离显示"),
]


def _config_file(settings, file_key):
    if file_key == "wifi":
        return wifi_credentials_h(settings)
    return mqtt_display_c(settings)


def read_config(settings):
    """从 mqtt_display.c / wifi_credentials.h 读取当前编译期配置。"""
    values = {}
    texts = {}
    for macro, _label, is_str, file_key in CONFIG_MACROS:
        if file_key not in texts:
            try:
                with open(_config_file(settings, file_key), "r", encoding="utf-8") as f:
                    texts[file_key] = f.read()
            except OSError:
                texts[file_key] = ""
        if is_str:
            m = re.search(r'#define\s+%s\s+"([^"]*)"' % macro, texts[file_key])
        else:
            m = re.search(r'#define\s+%s\s+(\d+)' % macro, texts[file_key])
        if m:
            values[macro] = m.group(1)
    return values


def write_config(settings, values):
    """把新配置写回对应源文件；凭证头文件缺失时自动从模板创建。"""
    wifi_h = wifi_credentials_h(settings)
    if not os.path.isfile(wifi_h):
        example = wifi_h + ".example"
        if os.path.isfile(example):
            shutil.copy2(example, wifi_h)
        else:
            raise ValueError("缺少 %s（模板 %s 也不存在）" % (wifi_h, example))
    for file_key in ("wifi", "mqtt"):
        path = _config_file(settings, file_key)
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        for macro, _label, is_str, key in CONFIG_MACROS:
            if key != file_key or macro not in values:
                continue
            if is_str:
                pattern = r'(#define\s+%s\s+)"[^"]*"' % macro
                repl = r'\g<1>"%s"' % values[macro]
            else:
                pattern = r'(#define\s+%s\s+)\d+' % macro
                repl = r'\g<1>%s' % values[macro]
            text, count = re.subn(pattern, repl, text)
            if count == 0:
                raise ValueError("未在 %s 找到宏 %s" % (os.path.basename(path), macro))
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)


class BoardGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("T5AI Pixel 机器狗控制台")
        self.geometry("880x640")
        self.minsize(760, 560)

        # 全局无衬线字体
        _style = ttk.Style(self)
        for cls in ("TLabel", "TButton", "TCheckbutton", "TRadiobutton",
                    "TCombobox", "TEntry", "TNotebook.Tab", "TLabelframe.Label",
                    "TFrame", "TLabelframe"):
            _style.configure(cls, font=(UI_FONT, 10))
        self.option_add("*Font", (UI_FONT, 10))

        self.settings = load_settings()
        self.log_queue = queue.Queue()
        self.serial_port = None
        self.serial_thread = None
        self.serial_stop = threading.Event()
        self.mqtt_client = None
        self.mqtt_connected = False
        self.busy = False  # 构建/刷写互斥

        notebook = ttk.Notebook(self)
        notebook.pack(fill="both", expand=True, padx=6, pady=6)
        self.tab_expr = ttk.Frame(notebook)
        self.tab_cfg = ttk.Frame(notebook)
        self.tab_serial = ttk.Frame(notebook)
        self.tab_settings = ttk.Frame(notebook)
        notebook.add(self.tab_expr, text=" 表情控制 ")
        notebook.add(self.tab_cfg, text=" 设备配置 ")
        notebook.add(self.tab_serial, text=" 串口监控 ")
        notebook.add(self.tab_settings, text=" 设置 ")

        self._build_expr_tab()
        self._build_cfg_tab()
        self._build_serial_tab()
        self._build_settings_tab()

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(100, self._poll_queues)

    # ------------------------------------------------------------ 表情控制页
    def _build_expr_tab(self):
        cfg = read_config(self.settings)
        top = ttk.LabelFrame(self.tab_expr, text="MQTT 连接")
        top.pack(fill="x", padx=10, pady=8)

        ttk.Label(top, text="Broker:").grid(row=0, column=0, padx=4, pady=6, sticky="e")
        self.var_host = tk.StringVar(value=cfg.get("MQTT_DISPLAY_BROKER_HOST", ""))
        ttk.Entry(top, textvariable=self.var_host, width=18).grid(row=0, column=1, padx=2)
        ttk.Label(top, text="端口:").grid(row=0, column=2, padx=4, sticky="e")
        self.var_port = tk.StringVar(value=cfg.get("MQTT_DISPLAY_BROKER_PORT", "1883"))
        ttk.Entry(top, textvariable=self.var_port, width=7).grid(row=0, column=3, padx=2)
        ttk.Label(top, text="Topic:").grid(row=0, column=4, padx=4, sticky="e")
        self.var_topic = tk.StringVar(value=cfg.get("MQTT_DISPLAY_TOPIC", ""))
        ttk.Entry(top, textvariable=self.var_topic, width=34).grid(row=0, column=5, padx=2)

        self.btn_mqtt = ttk.Button(top, text="连接", command=self._toggle_mqtt)
        self.btn_mqtt.grid(row=0, column=6, padx=8)
        self.lbl_mqtt = ttk.Label(top, text="未连接", foreground="red")
        self.lbl_mqtt.grid(row=0, column=7, padx=4)

        grid = ttk.LabelFrame(self.tab_expr, text="发送表情 / 动作图案 (QoS 1)")
        grid.pack(fill="both", expand=False, padx=10, pady=8)
        self.expr_buttons = []
        for i, (payload, label, desc) in enumerate(EXPRESSIONS):
            btn = tk.Button(grid, text="%s\n%s" % (label, desc), width=14, height=3,
                            font=(UI_FONT, 11),
                            state="disabled",
                            command=lambda p=payload: self._publish(p))
            btn.grid(row=i // 4, column=i % 4, padx=10, pady=10)
            self.expr_buttons.append(btn)

        custom = ttk.Frame(self.tab_expr)
        custom.pack(fill="x", padx=10, pady=4)
        ttk.Label(custom, text="自定义 payload:").pack(side="left")
        self.var_custom = tk.StringVar()
        ttk.Entry(custom, textvariable=self.var_custom, width=30).pack(side="left", padx=6)
        self.btn_custom = ttk.Button(custom, text="发送", state="disabled",
                                     command=lambda: self._publish(self.var_custom.get().strip()))
        self.btn_custom.pack(side="left")

        self.txt_expr = tk.Text(self.tab_expr, height=10, state="disabled",
                                font=(MONO_FONT, 9))
        self.txt_expr.pack(fill="both", expand=True, padx=10, pady=8)

    def _toggle_mqtt(self):
        if mqtt is None:
            messagebox.showerror("缺少依赖", "请先安装 paho-mqtt:\npip install paho-mqtt")
            return
        if self.mqtt_client is not None:
            self._mqtt_disconnect()
            return
        host = self.var_host.get().strip()
        try:
            port = int(self.var_port.get().strip())
        except ValueError:
            messagebox.showerror("参数错误", "端口必须是数字")
            return
        self._expr_log("连接 %s:%d ..." % (host, port))
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id="t5-pixel-gui-%d" % (time.time() % 100000))
        client.on_connect = lambda c, u, f, rc, prop=None: self.log_queue.put(
            ("mqtt_state", rc == 0))
        client.on_disconnect = lambda c, u, f, rc, prop=None: self.log_queue.put(
            ("mqtt_state", False))
        try:
            client.connect_async(host, port, keepalive=30)
            client.loop_start()
        except Exception as e:
            self._expr_log("连接失败: %s" % e)
            return
        self.mqtt_client = client
        self.btn_mqtt.config(text="断开")

    def _mqtt_disconnect(self):
        if self.mqtt_client is not None:
            try:
                self.mqtt_client.loop_stop()
                self.mqtt_client.disconnect()
            except Exception:
                pass
            self.mqtt_client = None
        self.mqtt_connected = False
        self.btn_mqtt.config(text="连接")
        self.lbl_mqtt.config(text="未连接", foreground="red")
        self._set_expr_buttons(False)
        self._expr_log("已断开")

    def _set_expr_buttons(self, enabled):
        state = "normal" if enabled else "disabled"
        for btn in self.expr_buttons:
            btn.config(state=state)
        self.btn_custom.config(state=state)

    def _publish(self, payload):
        if not payload:
            return
        if self.mqtt_client is None or not self.mqtt_connected:
            self._expr_log("未连接 broker，无法发送")
            return
        topic = self.var_topic.get().strip()
        result = self.mqtt_client.publish(topic, payload, qos=1)
        if result.rc == 0:
            self._expr_log("已发送: %s -> %s" % (payload, topic))
        else:
            self._expr_log("发送失败 rc=%d: %s" % (result.rc, payload))

    def _expr_log(self, line):
        self._append_text(self.txt_expr, line)

    # ------------------------------------------------------------ 设备配置页
    def _build_cfg_tab(self):
        frame = ttk.LabelFrame(self.tab_cfg, text="编译期配置 (mqtt_display.c + wifi_credentials.h)")
        frame.pack(fill="x", padx=10, pady=8)
        cfg = read_config(self.settings)
        self.cfg_vars = {}
        for row, (macro, label, _is_str, _file_key) in enumerate(CONFIG_MACROS):
            ttk.Label(frame, text=label + ":").grid(row=row, column=0, padx=6, pady=4, sticky="e")
            var = tk.StringVar(value=cfg.get(macro, ""))
            ttk.Entry(frame, textvariable=var, width=44).grid(row=row, column=1, padx=6, pady=4, sticky="w")
            self.cfg_vars[macro] = var

        bar = ttk.Frame(self.tab_cfg)
        bar.pack(fill="x", padx=10, pady=4)
        ttk.Label(bar, text="刷写串口:").pack(side="left")
        ports = self._list_ports()
        default_flash = self.settings.get("flash_port") or (ports[0] if ports else "")
        self.var_flash_port = tk.StringVar(value=default_flash)
        self.cmb_flash = ttk.Combobox(bar, textvariable=self.var_flash_port, width=14,
                                      values=ports)
        self.cmb_flash.pack(side="left", padx=4)
        ttk.Button(bar, text="刷新", command=lambda: self.cmb_flash.config(
            values=self._list_ports())).pack(side="left", padx=2)

        self.btn_save = ttk.Button(bar, text="保存配置", command=self._save_config)
        self.btn_save.pack(side="left", padx=8)
        self.btn_build = ttk.Button(bar, text="构建固件", command=lambda: self._run_job(["build"]))
        self.btn_build.pack(side="left", padx=4)
        self.btn_flash = ttk.Button(bar, text="刷写固件", command=lambda: self._run_job(["flash"]))
        self.btn_flash.pack(side="left", padx=4)
        self.btn_all = ttk.Button(bar, text="一键: 保存+构建+刷写",
                                  command=self._save_build_flash)
        self.btn_all.pack(side="left", padx=8)

        self.txt_cfg = tk.Text(self.tab_cfg, height=20, state="disabled", font=(MONO_FONT, 9))
        self.txt_cfg.pack(fill="both", expand=True, padx=10, pady=8)

    def _save_config(self):
        values = {m: v.get().strip() for m, v in self.cfg_vars.items()}
        port = values.get("MQTT_DISPLAY_BROKER_PORT", "")
        if not port.isdigit():
            messagebox.showerror("参数错误", "Broker 端口必须是数字")
            return False
        try:
            write_config(self.settings, values)
        except Exception as e:
            messagebox.showerror("保存失败", str(e))
            return False
        self._cfg_log("配置已写入 %s 与 %s" % (mqtt_display_c(self.settings),
                                              wifi_credentials_h(self.settings)))
        # 同步表情页的连接参数
        self.var_host.set(values["MQTT_DISPLAY_BROKER_HOST"])
        self.var_port.set(values["MQTT_DISPLAY_BROKER_PORT"])
        self.var_topic.set(values["MQTT_DISPLAY_TOPIC"])
        return True

    def _save_build_flash(self):
        if self._save_config():
            self._run_job(["build", "flash"])

    def _run_job(self, steps):
        if self.busy:
            messagebox.showwarning("请稍候", "已有构建/刷写任务在运行")
            return
        if not os.path.isdir(self.settings["sdk_root"]):
            messagebox.showerror("SDK 未配置",
                                 "SDK 根目录不存在，请先到「设置」页配置:\n%s"
                                 % self.settings["sdk_root"])
            return
        self.busy = True
        for b in (self.btn_save, self.btn_build, self.btn_flash, self.btn_all):
            b.config(state="disabled")
        threading.Thread(target=self._job_worker, args=(steps,), daemon=True).start()

    def _job_worker(self, steps):
        ok = True
        try:
            if "build" in steps:
                self.log_queue.put(("cfg", ">>> 同步源码到 SDK ..."))
                self._sync_src()
                self.log_queue.put(("cfg", ">>> 开始构建 (需要数分钟) ..."))
                ok = self._stream_process(build_command(self.settings), "cfg")
                if ok:
                    self.log_queue.put(("cfg", ">>> 构建成功"))
                else:
                    self.log_queue.put(("cfg", ">>> 构建失败，终止"))
            if ok and "flash" in steps:
                qio_bin = find_qio_bin(self.settings)
                tyutool = find_tyutool(self.settings)
                if qio_bin is None:
                    self.log_queue.put(("cfg", ">>> 未在 %s 下找到 *_QIO_*.bin，请先构建"
                                        % os.path.join(sdk_app_dir(self.settings), "dist")))
                    ok = False
                elif tyutool is None:
                    self.log_queue.put(("cfg", ">>> 未找到 tyutool_cli，请到「设置」页指定路径"))
                    ok = False
                else:
                    port = self.var_flash_port.get().strip()
                    self.log_queue.put(("cfg", ">>> 刷写 %s -> %s ..." % (qio_bin, port)))
                    ok = self._stream_process(
                        [tyutool, "write", "-d", self.settings["flash_device"],
                         "-f", qio_bin, "-p", port], "cfg")
                    self.log_queue.put(("cfg", ">>> 刷写%s" % ("完成" if ok else "失败")))
        except Exception as e:
            self.log_queue.put(("cfg", ">>> 异常: %s" % e))
        finally:
            self.log_queue.put(("job_done", None))

    def _sync_src(self):
        src_dir = self.settings["app_src_dir"]
        dst_dir = sdk_src_dir(self.settings)
        os.makedirs(dst_dir, exist_ok=True)
        for name in os.listdir(src_dir):
            src = os.path.join(src_dir, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(dst_dir, name))

    def _stream_process(self, cmd, tag):
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                cwd=WORKSPACE, text=True, encoding="utf-8",
                                errors="replace")
        for line in proc.stdout:
            line = line.rstrip()
            if line:
                self.log_queue.put((tag, line))
        proc.wait()
        return proc.returncode == 0

    def _cfg_log(self, line):
        self._append_text(self.txt_cfg, line)

    # ------------------------------------------------------------ 串口监控页
    def _build_serial_tab(self):
        top = ttk.Frame(self.tab_serial)
        top.pack(fill="x", padx=10, pady=8)
        ttk.Label(top, text="日志串口:").pack(side="left")
        ports = self._list_ports()
        default_log = self.settings.get("log_port") or (ports[-1] if ports else "")
        self.var_ser_port = tk.StringVar(value=default_log)
        self.cmb_ser = ttk.Combobox(top, textvariable=self.var_ser_port, width=14,
                                    values=ports)
        self.cmb_ser.pack(side="left", padx=4)
        ttk.Label(top, text="波特率:").pack(side="left")
        self.var_baud = tk.StringVar(value=self.settings.get("log_baud", "460800"))
        ttk.Combobox(top, textvariable=self.var_baud, width=8,
                     values=["115200", "460800", "921600"]).pack(side="left", padx=4)
        ttk.Button(top, text="刷新", command=lambda: self.cmb_ser.config(
            values=self._list_ports())).pack(side="left", padx=2)
        self.btn_ser = ttk.Button(top, text="打开", command=self._toggle_serial)
        self.btn_ser.pack(side="left", padx=8)

        status = ttk.LabelFrame(self.tab_serial, text="设备状态 (从日志解析)")
        status.pack(fill="x", padx=10, pady=4)
        self.lbl_wifi = ttk.Label(status, text="WiFi: 未知", font=(UI_FONT, 10, "bold"))
        self.lbl_wifi.pack(side="left", padx=14, pady=6)
        self.lbl_dev_mqtt = ttk.Label(status, text="MQTT: 未知", font=(UI_FONT, 10, "bold"))
        self.lbl_dev_mqtt.pack(side="left", padx=14)
        self.lbl_ip = ttk.Label(status, text="IP: -", font=(UI_FONT, 10, "bold"))
        self.lbl_ip.pack(side="left", padx=14)
        self.lbl_pattern = ttk.Label(status, text="图案: -", font=(UI_FONT, 10, "bold"))
        self.lbl_pattern.pack(side="left", padx=14)

        self.txt_serial = tk.Text(self.tab_serial, state="disabled", font=(MONO_FONT, 9))
        self.txt_serial.pack(fill="both", expand=True, padx=10, pady=8)

    def _list_ports(self):
        if serial is None:
            return []
        return [p.device for p in serial.tools.list_ports.comports()]

    def _toggle_serial(self):
        if serial is None:
            messagebox.showerror("缺少依赖", "请先安装 pyserial:\npip install pyserial")
            return
        if self.serial_port is not None:
            self._close_serial()
            return
        port = self.var_ser_port.get().strip()
        try:
            baud = int(self.var_baud.get().strip())
            self.serial_port = serial.Serial(port, baud, timeout=0.5)
        except Exception as e:
            messagebox.showerror("打开失败", "%s: %s" % (port, e))
            self.serial_port = None
            return
        self.serial_stop.clear()
        self.serial_thread = threading.Thread(target=self._serial_worker, daemon=True)
        self.serial_thread.start()
        self.btn_ser.config(text="关闭")
        self._append_text(self.txt_serial, "--- 已打开 %s @ %d ---" % (port, baud))

    def _close_serial(self):
        self.serial_stop.set()
        if self.serial_port is not None:
            try:
                self.serial_port.close()
            except Exception:
                pass
            self.serial_port = None
        self.btn_ser.config(text="打开")
        self._append_text(self.txt_serial, "--- 已关闭 ---")

    def _serial_worker(self):
        buf = b""
        while not self.serial_stop.is_set():
            try:
                data = self.serial_port.read(4096)
            except Exception:
                self.log_queue.put(("serial", "--- 串口读取异常，已断开 ---"))
                self.log_queue.put(("serial_closed", None))
                return
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip()
                if line:
                    self.log_queue.put(("serial", line))
                    self._parse_status(line)

    def _parse_status(self, line):
        if "WiFi connected" in line:
            self.log_queue.put(("wifi_state", ("已连接", "green")))
            m = re.search(r"ip=([0-9.]+)", line)
            if m:
                self.log_queue.put(("ip", m.group(1)))
        elif "WiFi disconnected" in line or "WiFi connect failed" in line:
            self.log_queue.put(("wifi_state", ("断开", "red")))
        elif "MQTT connected" in line:
            self.log_queue.put(("dev_mqtt_state", ("已连接", "green")))
        elif "MQTT disconnected" in line or ("MQTT connect" in line and "failed" in line):
            self.log_queue.put(("dev_mqtt_state", ("断开", "red")))
        m = re.search(r"MQTT display command: (\w+)", line)
        if m:
            self.log_queue.put(("pattern", m.group(1)))

    # ------------------------------------------------------------ 设置页
    SETTING_FIELDS = [
        # (key, 标签, browse 类型: dir/file/None)
        ("sdk_root", "SDK 根目录 (含 tos.py)", "dir"),
        ("app_src_dir", "应用源码目录 (工作区)", "dir"),
        ("sdk_app_subdir", "应用在 SDK 内的相对路径", None),
        ("tyutool_path", "tyutool_cli 路径 (留空自动查找)", "file"),
        ("flash_device", "刷写芯片型号 (-d 参数)", None),
    ]

    def _build_settings_tab(self):
        frame = ttk.LabelFrame(self.tab_settings, text="构建 / 刷写环境")
        frame.pack(fill="x", padx=10, pady=8)
        self.setting_vars = {}
        for row, (key, label, browse) in enumerate(self.SETTING_FIELDS):
            ttk.Label(frame, text=label + ":").grid(row=row, column=0, padx=6, pady=4, sticky="e")
            var = tk.StringVar(value=self.settings.get(key, ""))
            ttk.Entry(frame, textvariable=var, width=58).grid(row=row, column=1, padx=6, pady=4, sticky="w")
            self.setting_vars[key] = var
            if browse:
                ttk.Button(frame, text="浏览...", width=8,
                           command=lambda v=var, b=browse: self._browse(v, b)
                           ).grid(row=row, column=2, padx=4)

        bar = ttk.Frame(self.tab_settings)
        bar.pack(fill="x", padx=10, pady=4)
        ttk.Button(bar, text="保存设置", command=self._save_settings).pack(side="left")
        ttk.Button(bar, text="恢复默认", command=self._reset_settings).pack(side="left", padx=8)
        ttk.Button(bar, text="检查环境", command=self._check_env).pack(side="left")

        self.txt_settings = tk.Text(self.tab_settings, height=16, state="disabled",
                                    font=(MONO_FONT, 9))
        self.txt_settings.pack(fill="both", expand=True, padx=10, pady=8)
        self._settings_log("当前系统: %s (%s)" % (sys.platform, os.name))
        self._settings_log("设置文件: %s" % SETTINGS_FILE)
        self._check_env()

    def _browse(self, var, kind):
        if kind == "dir":
            path = filedialog.askdirectory(initialdir=var.get() or WORKSPACE)
        else:
            path = filedialog.askopenfilename(initialdir=os.path.dirname(var.get()) or WORKSPACE)
        if path:
            var.set(os.path.normpath(path))

    def _collect_settings(self):
        for key, var in self.setting_vars.items():
            self.settings[key] = var.get().strip()
        self.settings["flash_port"] = self.var_flash_port.get().strip()
        self.settings["log_port"] = self.var_ser_port.get().strip()
        self.settings["log_baud"] = self.var_baud.get().strip()

    def _save_settings(self):
        self._collect_settings()
        if save_settings(self.settings):
            self._settings_log("设置已保存到 %s" % SETTINGS_FILE)
            self._check_env()

    def _reset_settings(self):
        for key, var in self.setting_vars.items():
            var.set(DEFAULT_SETTINGS.get(key, ""))
        self._settings_log("已恢复默认值（未保存，点「保存设置」生效）")

    def _check_env(self):
        """校验各路径并输出推导结果。"""
        self._collect_settings()
        s = self.settings
        checks = [
            ("SDK 根目录", s["sdk_root"],
             os.path.isdir(s["sdk_root"])),
            ("tos.py", os.path.join(s["sdk_root"], "tos.py"),
             os.path.isfile(os.path.join(s["sdk_root"], "tos.py"))),
            ("export 脚本", os.path.join(s["sdk_root"], "export.ps1" if IS_WINDOWS else "export.sh"),
             os.path.isfile(os.path.join(s["sdk_root"], "export.ps1" if IS_WINDOWS else "export.sh"))),
            ("应用源码目录", s["app_src_dir"],
             os.path.isdir(s["app_src_dir"])),
            ("mqtt_display.c", mqtt_display_c(s),
             os.path.isfile(mqtt_display_c(s))),
            ("wifi_credentials.h", wifi_credentials_h(s),
             os.path.isfile(wifi_credentials_h(s))),
            ("SDK 应用目录", sdk_app_dir(s),
             os.path.isdir(sdk_app_dir(s))),
        ]
        self._settings_log("---- 环境检查 ----")
        for name, path, ok in checks:
            self._settings_log("[%s] %s: %s" % ("OK" if ok else "缺失", name, path))
        tyutool = find_tyutool(s)
        self._settings_log("[%s] 刷写工具: %s" % ("OK" if tyutool else "缺失", tyutool or "未找到"))
        qio = find_qio_bin(s)
        self._settings_log("[%s] QIO 固件: %s" % ("OK" if qio else "待构建", qio or "未找到"))

    def _settings_log(self, line):
        self._append_text(self.txt_settings, line)

    # ------------------------------------------------------------ 公共部分
    def _append_text(self, widget, line):
        widget.config(state="normal")
        widget.insert("end", line + "\n")
        if int(widget.index("end-1c").split(".")[0]) > 2000:
            widget.delete("1.0", "200.0")
        widget.see("end")
        widget.config(state="disabled")

    def _poll_queues(self):
        try:
            while True:
                tag, payload = self.log_queue.get_nowait()
                if tag == "serial":
                    self._append_text(self.txt_serial, payload)
                elif tag == "cfg":
                    self._append_text(self.txt_cfg, payload)
                elif tag == "mqtt_state":
                    self.mqtt_connected = bool(payload)
                    if self.mqtt_connected:
                        self.lbl_mqtt.config(text="已连接", foreground="green")
                        self._set_expr_buttons(True)
                        self._expr_log("Broker 已连接")
                    else:
                        self.lbl_mqtt.config(text="未连接", foreground="red")
                        self._set_expr_buttons(False)
                elif tag == "wifi_state":
                    text, color = payload
                    self.lbl_wifi.config(text="WiFi: %s" % text, foreground=color)
                elif tag == "dev_mqtt_state":
                    text, color = payload
                    self.lbl_dev_mqtt.config(text="MQTT: %s" % text, foreground=color)
                elif tag == "ip":
                    self.lbl_ip.config(text="IP: %s" % payload)
                elif tag == "pattern":
                    self.lbl_pattern.config(text="图案: %s" % payload)
                elif tag == "serial_closed":
                    self.serial_port = None
                    self.btn_ser.config(text="打开")
                elif tag == "job_done":
                    self.busy = False
                    for b in (self.btn_save, self.btn_build, self.btn_flash, self.btn_all):
                        b.config(state="normal")
        except queue.Empty:
            pass
        self.after(100, self._poll_queues)

    def _on_close(self):
        # 记住串口选择
        self._collect_settings()
        try:
            with open(SETTINGS_FILE, "w", encoding="utf-8") as f:
                json.dump(self.settings, f, ensure_ascii=False, indent=2)
        except OSError:
            pass
        self.serial_stop.set()
        if self.serial_port is not None:
            try:
                self.serial_port.close()
            except Exception:
                pass
        if self.mqtt_client is not None:
            try:
                self.mqtt_client.loop_stop()
                self.mqtt_client.disconnect()
            except Exception:
                pass
        self.destroy()


if __name__ == "__main__":
    app = BoardGUI()
    app.mainloop()
