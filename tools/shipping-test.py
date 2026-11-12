#!/usr/bin/env python3
"""
shipping-test.py - 出荷前検査 (esp32c3-bmp581、SKU 11521)

USB でつないだ基板を 1 台ずつ検査し、結果を CSV に記録します。
シリアルで判定できる項目は自動で判定し、目で見る項目と、手で操作する項目は、画面の指示に従って操作します。

  python tools/shipping-test.py                       検査を始める (1 台ずつ、続けて何台でも)
  python tools/shipping-test.py --qwiic               Qwiic の検査もする (Modulino Buzzer をつないでおく)
  python tools/shipping-test.py --out D:/inspection   記録の保存先 (既定: inspection/)
  python tools/shipping-test.py --summary             保存済みの記録から、まとめ (気圧のばらつきなど) を出す

準備: 試験用の単三電池 2 本、USB-C ケーブル、(--qwiic なら) Modulino Buzzer と Qwiic ケーブル
必要なもの: Python 3、pyserial、Arduino IDE の esp32 ボード (esptool.exe。書き込みに使う)
"""
import argparse
import csv
import glob
import hashlib
import json
import os
import queue
import re
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial が必要です: pip install pyserial")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLASHER = os.path.join(ROOT, "docs", "esp32c3-bmp581", "flasher")
BAUD = 115200
ESP_VID = 0x303A
AP_SSID = "pressure-monitor"

# 判定の基準
PRESSURE_HPA = (870.0, 1085.0)      # 気圧の値として、ありうる範囲
TEMPERATURE_C = (5.0, 45.0)
BATTERY_ON_MV = (2000, 3900)        # 試験用の電池 2 本 (SW1 ON)
BATTERY_OFF_MV = 1000               # SW1 OFF (これ未満を「電池なし」とみなす。ファームウェアと同じ)
NOISE_MAX_PA = 0.30                 # 1 サンプルあたりのノイズ (以前の基板の実測 0.13〜0.19 Pa)
TICKS_MIN = 28                      # 1 秒あたりの処理回数 (30 Hz)
LIFT_MIN_PA = 3.0                   # 持ち上げたときの気圧の低下 (3 Pa ≒ 25 cm)
BATCH_DEV_HPA = 1.0                 # まとめ: 同じ日の検査の中央値からの差

DEFAULTS = {                        # 工場出荷時の設定 (settings.cpp の SETTINGS_DEFAULT)
    "display_mode": "1 ",
    "settings.brightness": "192",
    "settings.custom": "limit=50 range=753 hpf_shift=8 trail_step=1",
    "settings.role": "standalone group=39",
    "brownouts": "0",
    "wifi": "off",
    "log.state": "none",
}

FIELDS = ["date", "time", "operator", "lot", "name", "mac", "port", "result",
          "firmware", "flashed", "boot", "power_st", "led_boot", "pressure_hpa", "temperature_c", "noise_pa",
          "ticks_min", "i2c_err", "lift_pa", "led_lift", "battery_on_mv", "battery_off_mv",
          "usb_switch", "wifi_signal", "reset_button", "qwiic", "factory_reset", "log_cleared",
          "defaults", "visual", "ng_items", "note"]


# ---------------------------------------------------------------- 表示と入力

def say(msg=""):
    print(msg, flush=True)


def head(msg):
    say("\n" + "=" * 60 + "\n" + msg + "\n" + "=" * 60)


def ask(msg, choices="yn"):
    """y/n などを聞く。Enter だけのときや、ほかの文字のときは、もう一度聞く (押し間違いを防ぐ)"""
    while True:
        try:
            a = input(f"  {msg} [{'/'.join(choices)}] ").strip().lower()
        except EOFError:
            sys.exit("\n入力がありません。端末から実行してください")
        if len(a) == 1 and a in choices:
            return a
        say(f"  {' か '.join(choices)} を入力して、Enter を押してください")


def wait_enter(msg):
    try:
        input(f"  {msg}（準備ができたら Enter）")
    except EOFError:
        sys.exit("\n入力がありません。端末から実行してください")


# ---------------------------------------------------------------- シリアル

class Board:
    """基板とのシリアル。読み取りは別のスレッドで行い、USB が切れてもつながり直す"""

    def __init__(self, port):
        self.port = port
        self.q = queue.Queue()
        self.log = []                   # (時刻, 行) すべて。記録に残す
        self.events = []                # ("disconnected"/"connected", 時刻)
        self.s = None
        self.stop = False
        self.paused = False
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _open(self):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = self.port, BAUD, 0.2
        s.dtr = False                   # 開いたときに、基板をリセットしない
        s.rts = False
        s.open()
        return s

    def _run(self):
        pending = b""
        while not self.stop:
            if self.paused:
                time.sleep(0.1)
                continue
            if self.s is None:
                try:
                    self.s = self._open()
                    self.events.append(("connected", time.time()))
                    pending = b""
                except Exception:
                    time.sleep(0.3)
                    continue
            try:
                data = self.s.read(4096)
            except Exception:
                self._drop()
                continue
            pending += data
            *lines, pending = pending.split(b"\n")
            for raw in lines:
                line = raw.decode("utf-8", "replace").rstrip("\r")
                now = time.time()
                self.log.append((now, line))
                self.q.put((now, line))

    def _drop(self):
        with self.lock:
            try:
                if self.s:
                    self.s.close()
            except Exception:
                pass
            if self.s is not None:
                self.events.append(("disconnected", time.time()))
            self.s = None

    def release(self):
        """書き込みのために、ポートを手放す"""
        self.paused = True
        time.sleep(0.3)
        self._drop()
        self.events.clear()

    def resume(self):
        self.paused = False

    def close(self):
        self.stop = True
        self._drop()

    def connected(self):
        return self.s is not None

    def send(self, cmd, timeout=10):
        until = time.time() + timeout
        while time.time() < until:
            with self.lock:
                if self.s is not None:
                    try:
                        self.s.write((cmd + "\n").encode())
                        self.log.append((time.time(), ">> " + cmd))
                        return True
                    except Exception:
                        pass
            time.sleep(0.2)
        return False

    def flush(self):
        while not self.q.empty():
            self.q.get_nowait()

    def lines(self, seconds):
        """seconds 秒のあいだ、届いた行を返す"""
        until = time.time() + seconds
        while True:
            rest = until - time.time()
            if rest <= 0:
                return
            try:
                yield self.q.get(timeout=min(rest, 0.2))
            except queue.Empty:
                continue

    def wait_for(self, pattern, seconds):
        """pattern (正規表現) の行が届くまで待つ。届いた行のリストと、一致したもの"""
        got = []
        for t, line in self.lines(seconds):
            got.append(line)
            m = re.search(pattern, line)
            if m:
                return got, m
        return got, None

    def status(self, seconds=4):
        """status を送り、名前=値 の辞書にする"""
        self.flush()
        if not self.send("status"):
            return {}
        st = {}
        for t, line in self.lines(seconds):
            m = re.match(r"# status ([\w.]+)=(.*)", line)
            if m:
                st[m.group(1)] = m.group(2).strip()
                if m.group(1) == "end":
                    break
        return st

    def reboot(self, cmd="normal", seconds=15):
        """再起動して、起動の表示 ("ready." または設定モードの開始) まで集める"""
        self.flush()
        self.send(cmd)
        got, m = self.wait_for(r"^ready\.|# settings mode: Wi-Fi AP started", seconds)
        return got, m is not None


def find_ports():
    return [p for p in serial.tools.list_ports.comports() if p.vid == ESP_VID]


def choose_port(args, skip):
    """検査する基板のポート。--port がなければ、ESP32-C3 のポートを探す"""
    if args.port:
        return args.port
    while True:
        ports = [p for p in find_ports() if p.device not in skip]
        if len(ports) == 1:
            p = ports[0]
            say(f"  ポート: {p.device}（{p.serial_number or ''}）")
            return p.device
        if len(ports) > 1:
            say("  ESP32-C3 のポートが複数あります。検査する基板以外は外すか、--port で指定してください:")
            for p in ports:
                say(f"    {p.device}  {p.serial_number or ''}")
        else:
            say("  基板が見つかりません。USB-C でつないでください")
        time.sleep(2)


# ---------------------------------------------------------------- 書き込み

def latest_release():
    with open(os.path.join(FLASHER, "releases.json"), encoding="utf-8") as f:
        rel = json.load(f)["releases"][0]
    folder = os.path.join(FLASHER, rel["path"])
    with open(os.path.join(folder, "flash_config.json"), encoding="utf-8") as f:
        cfg = json.load(f)
    for item in cfg["files"]:           # 書き込むファイルが、リリースのときと同じか確かめる
        p = os.path.join(folder, item["path"])
        with open(p, "rb") as f:
            if hashlib.sha256(f.read()).hexdigest() != item["sha256"]:
                sys.exit(f"リリースのファイルが壊れています: {p}")
    return rel["version"], folder, cfg


def find_esptool():
    pats = [os.path.expandvars(r"%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\*\esptool.exe"),
            os.path.expanduser("~/.arduino15/packages/esp32/tools/esptool_py/*/esptool"),
            os.path.expanduser("~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool")]
    found = sorted(p for pat in pats for p in glob.glob(pat))
    return found[-1] if found else None


def flash(board, folder, cfg):
    esptool = find_esptool()
    if not esptool:
        say("  esptool が見つかりません。書き込みツール (https://ksasao.github.io/pressure-monitor/esp32c3-bmp581/flasher/) で書き込んでください")
        return False
    board.release()
    cmd = [esptool, "--chip", "esp32c3", "--port", board.port, "--baud", "921600",
           "--before", "default-reset", "--after", "hard-reset", "write-flash"]
    for item in cfg["files"]:
        cmd += [hex(item["address"]), os.path.join(folder, item["path"])]
    say("  書き込んでいます（約 30 秒）...")
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    board.log.append((time.time(), "esptool: " + (r.stdout + r.stderr).replace("\n", " | ")[-2000:]))
    board.resume()
    if r.returncode != 0:
        say("  書き込みに失敗しました:\n" + (r.stdout + r.stderr)[-1500:])
        return False
    say("  書き込みました（照合も一致）")
    return True


# ---------------------------------------------------------------- 計測

def csv_rows(lines):
    """気圧の CSV の行 (millis,pressure_hPa,raw,hpf,temperature_C,altitude_m,led_value,battery_mV)"""
    rows = []
    for t, line in lines:
        p = line.split(",")
        if len(p) >= 8 and p[0].isdigit():
            try:
                rows.append((t, int(p[0]), float(p[1])))
            except ValueError:
                pass
    return rows


def perf_lines(lines):
    out = []
    for t, line in lines:
        m = re.search(r"# perf ticks=(\d+) period_max=(\d+)ms .*i2c_err=(\d+)", line)
        if m:
            out.append(tuple(int(x) for x in m.groups()))
    return out


def noise_pa(pressures_hpa):
    """1 サンプルあたりのノイズ [Pa] (隣り合う値の差の標準偏差 / √2。ゆっくりした変化は入らない)"""
    d = [(b - a) * 100.0 for a, b in zip(pressures_hpa, pressures_hpa[1:])]
    return statistics.pstdev(d) / (2 ** 0.5) if len(d) > 10 else None


def wlan_scan():
    """Windows に Wi-Fi のスキャンを頼む (結果は netsh で読む)"""
    if os.name != "nt":
        return
    try:
        import ctypes
        import ctypes.wintypes as w

        class GUID(ctypes.Structure):
            _fields_ = [("d1", w.DWORD), ("d2", w.WORD), ("d3", w.WORD), ("d4", ctypes.c_ubyte * 8)]

        class INFO(ctypes.Structure):
            _fields_ = [("guid", GUID), ("desc", ctypes.c_wchar * 256), ("state", w.DWORD)]

        class LIST(ctypes.Structure):
            _fields_ = [("n", w.DWORD), ("i", w.DWORD), ("info", INFO * 1)]

        wl = ctypes.windll.wlanapi
        h, ver = w.HANDLE(), w.DWORD()
        if wl.WlanOpenHandle(2, None, ctypes.byref(ver), ctypes.byref(h)) != 0:
            return
        pl = ctypes.POINTER(LIST)()
        if wl.WlanEnumInterfaces(h, None, ctypes.byref(pl)) == 0:
            arr = ctypes.cast(ctypes.addressof(pl.contents.info), ctypes.POINTER(INFO))
            for k in range(pl.contents.n):
                wl.WlanScan(h, ctypes.byref(arr[k].guid), None, None, None)
            wl.WlanFreeMemory(pl)
        wl.WlanCloseHandle(h, None)
    except Exception:
        pass


def wlan_networks():
    """[(SSID, BSSID, シグナル %)]"""
    try:
        out = subprocess.run(["netsh", "wlan", "show", "networks", "mode=bssid"],
                             capture_output=True).stdout.decode("mbcs", "replace")
    except Exception:
        return []
    nets, ssid, bssid = [], None, None
    for line in out.splitlines():
        m = re.match(r"\s*SSID \d+\s*:\s*(.*)$", line)
        if m:
            ssid = m.group(1).strip()
            continue
        m = re.match(r"\s*BSSID \d+\s*:\s*([0-9a-f:]{17})", line, re.I)
        if m:
            bssid = m.group(1).lower()
            continue
        m = re.search(r":\s*(\d+)%", line)
        if m and ssid is not None and bssid:
            nets.append((ssid, bssid, int(m.group(1))))
            bssid = None
    return nets


def ap_mac(mac):
    """ESP32 の Wi-Fi アクセスポイントの MAC (ステーションの MAC + 1)"""
    n = (int(mac.replace(":", ""), 16) + 1) & 0xFFFFFFFFFFFF
    return ":".join(f"{(n >> s) & 0xFF:02x}" for s in range(40, -8, -8))


# ---------------------------------------------------------------- 検査

class Unit:
    def __init__(self, args):
        self.r = {k: "" for k in FIELDS}
        self.ng = []
        self.args = args

    def judge(self, key, ok, value, label):
        self.r[key] = f"{'OK' if ok else 'NG'} {value}".strip()
        say(f"  [{'OK' if ok else 'NG'}] {label}: {value}")
        if not ok:
            self.ng.append(label)
        return ok


def manual(u, key, question, label):
    """目で見る項目。r で、もう一度やり直す"""
    a = ask(question + "（y: はい、n: いいえ）")
    return u.judge(key, a == "y", "", label)


def test_unit(args, version, folder, cfg, skip):
    u = Unit(args)
    now = datetime.now()
    u.r.update(date=now.strftime("%Y-%m-%d"), time=now.strftime("%H:%M:%S"),
               operator=args.operator, lot=args.lot)

    head("準備")
    say("  1. 試験用の単三電池 2 本を、向きに注意して入れます")
    say("  2. SW1 を ON にします")
    say("  3. USB-C で PC につなぎます（ケーブルは抜き差ししやすい位置に）")
    if args.qwiic:
        say("  4. Qwiic コネクタに Modulino Buzzer をつなぎます")
    wait_enter("")
    port = choose_port(args, skip)
    u.r["port"] = port
    b = Board(port)
    try:
        run_steps(u, b, version, folder, cfg)
    except KeyboardInterrupt:
        u.ng.append("中断")
        u.r["note"] = "中断"
        say("\n  中断しました")
    finally:
        b.close()
    u.r["ng_items"] = " / ".join(u.ng)
    u.r["result"] = "合格" if not u.ng else "不合格"
    return u, b


def run_steps(u, b, version, folder, cfg):
    # ---- 1. ファームウェア
    head("1. ファームウェア")
    time.sleep(1.0)
    st = b.status()
    fw = st.get("firmware", "")
    u.r["flashed"] = "no"
    if fw != version:
        say(f"  今のファームウェア: {fw or '（応答なし）'} → {version} を書き込みます")
        if not flash(b, folder, cfg):
            u.judge("firmware", False, fw or "応答なし", "ファームウェア")
            return
        u.r["flashed"] = "yes"
        b.wait_for(r"^ready\.", 15)
        st = b.status()
        fw = st.get("firmware", "")
    u.judge("firmware", fw == version, fw, f"ファームウェアが {version}")
    u.r["name"], u.r["mac"] = st.get("name", ""), st.get("mac", "")
    say(f"  装置の名前: {u.r['name']}（MAC {u.r['mac']}）")

    # ---- 2. 起動と LED
    head("2. 起動と LED（目で確認）")
    say("  再起動します。5 個の LED を見ていてください。")
    say("  白で全点灯 → 赤、緑、青が、順に 5 個を流れます")
    wait_enter("LED が見える位置で")
    got, ready = b.reboot("normal")
    text = "\n".join(got)
    ok = ready and f"=== PressureMonitor {version} ===" in text and "BMP581 found" in text \
        and "brownout" not in text
    u.judge("boot", ok, "BMP581 検出" if "BMP581 found" in text else "BMP581 なし",
            "起動（センサの検出、ブラウンアウトなし）")
    st_ok = "# power: ST (IO3) found, USB" in text
    u.judge("power_st", st_ok, "USB 給電を検出" if st_ok else "検出できません",
            "電源の状態（TPS2116 の ST → IO3）")
    if u.args.qwiic:
        u.judge("qwiic", "Modulino Buzzer found" in text,
                "Buzzer 検出" if "Modulino Buzzer found" in text else "Buzzer なし", "Qwiic（Modulino Buzzer）")
    manual(u, "led_boot", "5 個とも、白 → 赤 → 緑 → 青 で光りましたか（欠けや色違いなし）", "LED（起動時の点灯）")

    # ---- 3. センサと電池電圧
    head("3. センサと電池電圧（自動）")
    st = b.status()
    try:
        p, t = float(st.get("pressure_hpa", "nan")), float(st.get("temperature_c", "nan"))
    except ValueError:
        p, t = float("nan"), float("nan")
    u.judge("pressure_hpa", PRESSURE_HPA[0] <= p <= PRESSURE_HPA[1], f"{p:.2f}", "気圧の値 [hPa]")
    u.judge("temperature_c", TEMPERATURE_C[0] <= t <= TEMPERATURE_C[1], f"{t:.1f}", "温度 [℃]")
    mv = int(st.get("battery_mv", "0") or 0)
    u.judge("battery_on_mv", BATTERY_ON_MV[0] <= mv <= BATTERY_ON_MV[1], mv, "電池電圧（SW1 ON）[mV]")

    # ---- 4. ノイズと処理の周期
    head("4. ノイズと処理の周期（自動、約 12 秒）")
    say("  基板に触らず、机の上に置いたままにしてください")
    wait_enter("")
    b.flush()
    lines = list(b.lines(12))
    rows = csv_rows(lines)
    perf = perf_lines(lines)
    n = noise_pa([r[2] for r in rows])
    u.judge("noise_pa", n is not None and n <= NOISE_MAX_PA,
            f"{n:.3f}" if n is not None else "データなし", f"気圧のノイズ [Pa]（{NOISE_MAX_PA} 以下）")
    tmin = min((x[0] for x in perf), default=0)
    err = max((x[2] for x in perf), default=-1)
    u.judge("ticks_min", tmin >= TICKS_MIN, tmin, f"1 秒あたりの処理回数（{TICKS_MIN} 以上）")
    u.judge("i2c_err", err == 0, err, "I2C のエラー回数（0）")

    # ---- 5. センサの反応と LED の色
    head("5. センサの反応と LED の色")
    say("  Enter を押したら、基板を 50 cm ほど持ち上げて 3 秒止め、元の高さに下ろしてください")
    say("  持ち上げるとオレンジ、下ろすと青に光ります（20 秒以内に）")
    wait_enter("")
    b.flush()
    rows = csv_rows(b.lines(20))
    if len(rows) > 60:
        base = statistics.median(r[2] for r in rows[:10])
        drop = (min(r[2] for r in rows) - base) * 100.0
    else:
        drop = 0.0
    u.judge("lift_pa", -drop >= LIFT_MIN_PA, f"{drop:.1f}", f"持ち上げたときの気圧の変化 [Pa]（-{LIFT_MIN_PA} 以下）")
    manual(u, "led_lift", "持ち上げたときにオレンジ、下ろしたときに青に光りましたか", "LED（気圧の変化の色）")

    # ---- 6. SW1 と電池電圧
    head("6. SW1（電源スイッチ）")
    say("  SW1 を OFF にしてください（USB はつないだまま）")
    wait_enter("")
    b.reboot("normal")
    st = b.status()
    off = int(st.get("battery_mv", "0") or 0)
    u.judge("battery_off_mv", off < BATTERY_OFF_MV, off, f"電池電圧（SW1 OFF）[mV]（{BATTERY_OFF_MV} 未満）")
    say("  SW1 を ON に戻してください")
    wait_enter("")
    b.reboot("normal")
    st = b.status()
    on = int(st.get("battery_mv", "0") or 0)
    u.judge("battery_on_mv", BATTERY_ON_MV[0] <= on <= BATTERY_ON_MV[1], on, "電池電圧（SW1 を ON に戻した）[mV]")

    # ---- 7. USB と電池の切り替え
    head("7. USB と電池の切り替え（TPS2116）")
    say("  USB-C を抜き、5 秒以上待ってから、挿し直してください（60 秒以内）")
    say("  抜いている間も、電池で動き続けることを確かめます（再起動しないこと）")
    b.flush()
    b.events.clear()
    last_ms, last_t, after = None, None, None
    rebooted = False
    until = time.time() + 75
    for t, line in b.lines(75):
        if line.startswith("=== PressureMonitor"):
            rebooted = True
        rs = csv_rows([(t, line)])
        if not rs:
            continue
        disc = [e for e in b.events if e[0] == "disconnected"]
        if not disc:
            last_ms, last_t = rs[0][1], t
        elif any(e[0] == "connected" and e[1] > disc[0][1] for e in b.events):
            after = (rs[0][1], t)
            break
    if after and last_ms is not None:
        dms = (after[0] - last_ms) / 1000.0
        dt = after[1] - last_t
        ok = not rebooted and abs(dms - dt) < 1.5 and dt >= 4
        u.judge("usb_switch", ok, f"抜いていた時間 {dt:.1f} 秒、基板の時計 {dms:.1f} 秒",
                "USB を抜いても電池で動き続ける（再起動なし）")
    else:
        u.judge("usb_switch", False, "抜き差しを検出できませんでした", "USB を抜いても電池で動き続ける")

    # ---- 8. Wi-Fi
    head("8. Wi-Fi（自動、約 30 秒）")
    say("  設定モードで再起動し、PC で基板の Wi-Fi を探します（基板は PC から 1 m 以内に）")
    b.reboot("settings", 20)
    want = ap_mac(u.r["mac"]) if u.r["mac"] else ""
    found = None
    until = time.time() + 30
    while time.time() < until and not found:
        wlan_scan()
        time.sleep(4)
        nets = [x for x in wlan_networks() if x[0] == AP_SSID]
        found = next((x for x in nets if x[1] == want), None)
        if not found and nets:          # BSSID が MAC + 1 でない場合も、一番強いものを使う
            found = max(nets, key=lambda x: x[2])
    if found:
        note = "" if found[1] == want else "、BSSID が MAC + 1 と違う"
        u.judge("wifi_signal", found[2] >= 40, f"{found[2]}%（{found[1]}{note}）", "Wi-Fi の電波（シグナル 40% 以上）")
    else:
        u.judge("wifi_signal", False, "見つかりません", "Wi-Fi の電波")
    b.reboot("normal")

    # ---- 9. RESET ボタン
    head("9. RESET ボタン")
    say("  RESET ボタンを 1 回押してください（30 秒以内）")
    b.flush()
    got, m = b.wait_for(r"# reset reason: (\w+)", 30)
    u.judge("reset_button", m is not None, m.group(1) if m else "反応なし", "RESET ボタン")
    b.wait_for(r"^ready\.", 10)

    # ---- 10. BOOT ボタンと工場出荷時の設定
    head("10. BOOT ボタン（工場出荷時の設定に戻す）")
    say("  BOOT ボタンを押し続けてください。1.5 秒で水色、20 秒で全ての LED が赤く点滅します")
    say("  赤の点滅が始まったら、ボタンを離してください（60 秒以内）")
    b.flush()
    got, m1 = b.wait_for(r"# BOOT long press detected", 60)
    got2, m2 = b.wait_for(r"# all settings reset to default|rebooting into settings", 40) if m1 else ([], None)
    ok = bool(m2) and "all settings" in m2.group(0)
    if m2 and not ok:
        say("  20 秒より前に離したため、設定モードになりました。通常モードに戻して、やり直します")
        b.reboot("normal")
        say("  もう一度、BOOT ボタンを、赤く点滅するまで（20 秒）押し続けてください")
        b.flush()
        got, m1 = b.wait_for(r"# BOOT long press detected", 60)
        got2, m2 = b.wait_for(r"# all settings reset to default", 40) if m1 else ([], None)
        ok = bool(m2)
    u.judge("factory_reset", ok, "長押し・20 秒押しを検出" if ok else "検出できません",
            "BOOT ボタン（長押しで工場出荷時の設定に戻る）")
    b.wait_for(r"^ready\.", 15)

    # ---- 11. 記録の消去と、出荷時の状態
    head("11. 出荷時の状態（自動）")
    b.flush()
    b.send("clear")
    b.wait_for(r"# rec: clear requested", 5)
    time.sleep(2)
    st = b.status()
    u.judge("log_cleared", st.get("log.state") == "none", st.get("log.state", "?"), "基板の中の記録が空")
    bad = [f"{k}={st.get(k, '?')}" for k, v in DEFAULTS.items() if not st.get(k, "").startswith(v)]
    u.judge("defaults", not bad, "既定値" if not bad else ", ".join(bad), "設定が工場出荷時のまま")

    # ---- 12. 外観
    head("12. 外観（目で確認）")
    items = [
        "部品の欠け・浮き・はんだのブリッジがない（特に BMP581、USB-C、TPS2116 のまわり）",
        "基板の縁（V カットの跡、アンテナの下）にバリやひびがない",
        "電池ボックスの端子が曲がっておらず、電池がしっかり固定される",
        "SW1、BOOT、RESET が引っかからずに動く",
        "表面・裏面に傷、汚れ、フラックスの残りがない",
    ]
    vis = []
    for it in items:
        if ask(it + "（y: はい、n: いいえ）") != "y":
            vis.append(it.split("（")[0])
    u.judge("visual", not vis, "" if not vis else " / ".join(vis), "外観")

    head("後片づけ")
    say("  1. USB-C を抜き、SW1 を OFF にして、試験用の電池を外します")
    if u.args.qwiic:
        say("  2. Modulino Buzzer を外します")
    note = input("  備考があれば入力してください（なければ Enter）: ").strip()
    u.r["note"] = note


# ---------------------------------------------------------------- 記録

def save(u, b, out):
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "records.csv")
    new = not os.path.exists(path)
    if not new:                         # 列が増えたときは、前の記録を、今の列で書き直す
        with open(path, encoding="utf-8-sig") as f:
            rd = csv.DictReader(f)
            old_fields, old_rows = rd.fieldnames, list(rd)
        if old_fields != FIELDS:
            with open(path, "w", newline="", encoding="utf-8-sig") as f:
                w = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
                w.writeheader()
                w.writerows(old_rows)
    with open(path, "a", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS, extrasaction="ignore")
        if new:
            w.writeheader()
        w.writerow(u.r)
    stamp = f"{u.r['date'].replace('-', '')}-{u.r['time'].replace(':', '')}"
    logp = os.path.join(out, "logs", f"{u.r['name'] or 'unknown'}_{stamp}.txt")
    os.makedirs(os.path.dirname(logp), exist_ok=True)
    with open(logp, "w", encoding="utf-8") as f:
        for t, line in b.log:
            f.write(datetime.fromtimestamp(t).strftime("%H:%M:%S.%f")[:-3] + " " + line + "\n")
    return path, logp


def summary(out):
    path = os.path.join(out, "records.csv")
    if not os.path.exists(path):
        sys.exit(f"記録がありません: {path}")
    with open(path, encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    head(f"まとめ（{path}）")
    for r in rows:
        say(f"  {r['date']} {r['time']}  {r['name']:10s} {r['result']}  {r['ng_items']}")
    ok = [r for r in rows if r["result"] == "合格"]
    say(f"\n  合格 {len(ok)} 台 / 全 {len(rows)} 件（同じ基板を検査し直した分も含みます）")
    by_day = {}
    for r in rows:
        m = re.search(r"([\d.]+)$", r["pressure_hpa"])
        if m:
            by_day.setdefault(r["date"], []).append((r["name"], float(m.group(1)), r["time"]))
    for day, vals in by_day.items():
        med = statistics.median(v[1] for v in vals)
        say(f"\n  {day} の気圧の中央値 {med:.2f} hPa（±{BATCH_DEV_HPA} hPa を超えるものに ! を付けます）")
        for name, v, t in vals:
            say(f"    {'!' if abs(v - med) > BATCH_DEV_HPA else ' '} {name:10s} {v:8.2f} ({v - med:+.2f})  {t}")
    say("\n  気圧は、検査した時刻の天気の変化も含みます。! があれば、その基板を基準の基板と並べて確かめてください")


def main():
    ap = argparse.ArgumentParser(description="出荷前検査 (esp32c3-bmp581)")
    ap.add_argument("--port", help="ポート (既定: ESP32-C3 のポートを探す)")
    ap.add_argument("--out", default=os.path.join(ROOT, "inspection"), help="記録の保存先")
    ap.add_argument("--operator", default="", help="検査した人")
    ap.add_argument("--lot", default="", help="ロット (例: 2026-11 V1.1)")
    ap.add_argument("--qwiic", action="store_true", help="Qwiic の検査もする (Modulino Buzzer)")
    ap.add_argument("--summary", action="store_true", help="保存済みの記録のまとめを出す")
    args = ap.parse_args()
    if args.summary:
        summary(args.out)
        return

    version, folder, cfg = latest_release()
    head(f"出荷前検査（ファームウェア {version}）")
    say(f"  記録の保存先: {args.out}")
    skip = set()
    others = find_ports()
    if others and not args.port:
        say("  すでにつながっている ESP32-C3 があります。検査しない基板なら y で除外します:")
        for p in others:
            if ask(f"{p.device}（{p.serial_number}）を除外しますか") == "y":
                skip.add(p.device)
    while True:
        u, b = test_unit(args, version, folder, cfg, skip)
        path, logp = save(u, b, args.out)
        head(f"結果: {u.r['name']}  {u.r['result']}")
        if u.ng:
            say("  不合格の項目: " + " / ".join(u.ng))
        say(f"  記録: {path}\n  ログ: {logp}")
        if ask("次の基板を検査しますか（y: 続ける、n: 終わる）") != "y":
            break
    summary(args.out)


if __name__ == "__main__":
    main()
