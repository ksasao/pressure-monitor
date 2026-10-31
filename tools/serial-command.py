#!/usr/bin/env python3
"""
serial-command.py - 基板 (esp32c3-bmp581) に、シリアルからコマンドを送る

BOOT ボタンの操作の代わりのコマンドや、基板の中への記録の操作 (開始、停止、消去、CSV の保存) を送り、
基板が出したメッセージ (# で始まる行) を表示します。
試験のときや、ケースに入れてボタンを押しにくいときに使います。
コマンドの一覧は firmware/esp32c3-bmp581/README.md の「シリアルからの操作」を見てください。

  python tools/serial-command.py COM6 settings          設定モード (Wi-Fi) で再起動する
  python tools/serial-command.py COM6 normal            通常モードで再起動する
  python tools/serial-command.py COM6 short short       短押しを 2 回 (番号を見せてから、次のモードへ)
  python tools/serial-command.py COM6 status --wait 1   設定、Wi-Fi の状態、記録の量などをまとめて表示する
  python tools/serial-command.py COM6 start --interval 1   記録を始める (1/30、1、30 秒間隔。PC の時計を送る)
  python tools/serial-command.py COM6 stop              記録を止める (PC の時計で、基板の時計のずれを補正する)
  python tools/serial-command.py COM6 save              記録を受け取り、CSV に保存する (/log.csv と同じ内容)
  python tools/serial-command.py COM6 clear --yes       記録を消去する
  python tools/serial-command.py --list                 シリアルポートの一覧
  python tools/serial-command.py COM6 settings --wait 8 --all

ポートを開くときに DTR・RTS を False にするので、ポートを開いただけでは、基板はリセットされません。
再起動するコマンドのあとは、USB がつながり直すのを待って、起動時のメッセージも表示します。

必要なもの: Python 3、pyserial (pip install pyserial)。save は、同じフォルダの logbin.py を使います
"""
import argparse
import os
import re
import sys
import time
import zlib
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial が必要です: pip install pyserial")

BAUD = 115200


def open_port(port):
    """DTR・RTS を False にしてから開く (開いたときに、基板をリセットしない)"""
    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def reopen(s, until):
    """再起動で USB が切れたあと、つながり直すまで待つ (until まで)"""
    try:
        s.close()
    except Exception:
        pass
    while time.time() < until:
        time.sleep(0.5)
        try:
            s.open()
            return True
        except Exception:
            pass
    return False


def send(s, cmd, until):
    """コマンドを 1 行送る。つながっていなければ、つながり直してから送る"""
    data = (cmd + "\n").encode()
    while True:
        try:
            s.write(data)
            return True
        except Exception:
            if not reopen(s, until):
                return False


def show(s, seconds, show_all):
    """seconds 秒のあいだ、受け取った行を表示する (show_all でなければ # の行だけ。# perf は除く)"""
    until = time.time() + seconds
    pending = b""
    while time.time() < until:
        try:
            pending += s.read(4096)
        except Exception:               # 再起動で USB が切れた
            pending = b""
            reopen(s, until)
            continue
        *lines, pending = pending.split(b"\n")
        for raw in lines:
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if show_all or (line.startswith(("#", "===")) and not line.startswith("# perf")):
                print("  " + line, flush=True)


# 記録の間隔 (--interval) と、基板に送る値 [ms]
INTERVALS = {"1/30": 0, "1": 1000, "30": 30000}


def tz_offset_min():
    """JavaScript の Date.getTimezoneOffset() と同じ (UTC - 地方時 [分]。JST は -540)"""
    off = datetime.now().astimezone().utcoffset()
    return -int(off.total_seconds() // 60)


def to_device_command(cmd, args):
    """使う人のコマンドを、基板に送る行にする。送らないもの (clear の確認でやめた) は None"""
    if cmd == "start":
        now = int(time.time() * 1000)
        return f"start {INTERVALS[args.interval]} {now} {tz_offset_min()}"
    if cmd == "stop":
        return f"stop {int(time.time() * 1000)}"
    if cmd == "clear" and not args.yes:
        if not sys.stdin.isatty():
            print("  clear は、記録を消去します。続けるときは --yes を付けてください", flush=True)
            return None
        try:
            answer = input("  基板の中の記録を、全て消去します。よろしいですか？ [y/N] ")
        except EOFError:                # 入力がない (Windows では、NUL も端末に見えることがある)
            answer = ""
        if answer.strip().lower() != "y":
            print("  やめました (確認を省くときは --yes)", flush=True)
            return None
    return cmd


def read_line(s, until):
    """1 行読む (改行まで)。時間切れなら None"""
    buf = b""
    while time.time() < until:
        c = s.read(1)
        if not c:
            continue
        if c == b"\n":
            return buf.decode("utf-8", "replace").rstrip("\r")
        buf += c
    return None


def save(s, args):
    """記録を受け取り (dump)、CSV にして保存する。記録 (/log.bin の形) も残すなら --bin"""
    import logbin
    s.reset_input_buffer()
    if not send(s, "dump", time.time() + 10):
        sys.exit("基板に送れませんでした")
    until = time.time() + 30
    total = None
    while total is None:                # 「# dump: begin <バイト数>」まで、ほかの行は読み飛ばす
        line = read_line(s, until)
        if line is None:
            sys.exit("基板から応答がありません")
        if line.startswith("# dump: error"):
            msg = line.split("error", 1)[1].strip()
            sys.exit("保存できません: " + ("記録中です。先に stop を送ってください" if "recording" in msg
                                            else "記録がありません" if "no_log" in msg else msg))
        m = re.match(r"# dump: begin (\d+)", line)
        if m:
            total = int(m.group(1))
    data = bytearray()
    t0 = time.time()
    last = t0
    s.timeout = 1.0
    while len(data) < total:
        chunk = s.read(min(65536, total - len(data)))
        if chunk:
            data += chunk
            last = time.time()
            print(f"\r  受信中 {len(data) / 1048576:.2f} / {total / 1048576:.2f} MB", end="", flush=True)
        elif time.time() - last > 5:
            sys.exit("\n途中で止まりました (USB の接続を確かめて、もう一度 save を送ってください)")
    print(f"\r  受信しました {total / 1048576:.2f} MB ({time.time() - t0:.1f} 秒)            ", flush=True)
    end = None
    until = time.time() + 5
    while end is None:
        line = read_line(s, until)
        if line is None:
            sys.exit("終わりの行がありません")
        m = re.match(r"# dump: end (\d+) crc32=([0-9a-f]{8})", line)
        if m:
            end = (int(m.group(1)), int(m.group(2), 16))
    crc = zlib.crc32(bytes(data)) & 0xFFFFFFFF
    if end[0] != total or end[1] != crc:
        sys.exit(f"受け取ったデータが壊れています (CRC {crc:08x}、基板 {end[1]:08x})。もう一度 save を送ってください")

    # ファイル名は、表示ページと同じ: pressure-monitor_<装置の名前>_<日時>.csv
    head = bytes(data[20:20 + int.from_bytes(data[8:12], "little")]).decode("utf-8", "replace")
    m = re.search(r",name=([^,\r\n]*)", head)
    name = re.sub(r'[\\/:*?"<>|\s]', "_", m.group(1)) if m and m.group(1) != "unknown" else ""
    fname = f"pressure-monitor_{name + '_' if name else ''}{datetime.now().strftime('%Y%m%d-%H%M%S')}.csv"
    path = args.out or fname
    if os.path.isdir(path):
        path = os.path.join(path, fname)
    if args.bin:
        bpath = os.path.splitext(path)[0] + ".bin"
        open(bpath, "wb").write(data)
        print(f"  記録: {bpath}", flush=True)
    t1 = time.time()
    csv = logbin.bin_to_csv(bytes(data),
                            lambda f: print(f"\r  CSV を作成中 {int(f * 100)}%", end="", flush=True))
    open(path, "wb").write(csv)
    rows = csv.count(b"\n") - 2
    print(f"\r  CSV: {path} ({len(csv) / 1048576:.1f} MB、{rows:,} 行、作成 {time.time() - t1:.1f} 秒)",
          flush=True)


def main():
    # 画面の文字コードで表せない文字が来ても、止まらないようにする (Windows のコンソールなど)
    try:
        sys.stdout.reconfigure(errors="replace")
    except AttributeError:
        pass
    ap = argparse.ArgumentParser(
        description="基板 (esp32c3-bmp581) に、シリアルからコマンドを送る (BOOT ボタンの操作の代わり)")
    ap.add_argument("port", nargs="?", help="シリアルポート (例: COM6、/dev/ttyACM0)")
    ap.add_argument("commands", nargs="*",
                    help="コマンド (settings、normal、long、short、status、start、stop、save、clear、help)。"
                         "複数なら順に送る")
    ap.add_argument("--wait", type=float, default=6.0,
                    help="コマンドごとに、応答を待つ秒数 (既定 6。再起動の起動メッセージまで見るには 6 以上)")
    ap.add_argument("--all", action="store_true",
                    help="# の行だけでなく、気圧の CSV の行も表示する")
    ap.add_argument("--list", action="store_true", help="シリアルポートの一覧を表示する")
    ap.add_argument("--interval", choices=list(INTERVALS), default="1/30",
                    help="start の記録の間隔 [秒] (既定 1/30。1 と 30 は、その間の平均)")
    ap.add_argument("--out", help="save の保存先 (ファイル名、またはフォルダ)。既定は今のフォルダ")
    ap.add_argument("--bin", action="store_true", help="save で、記録 (/log.bin の形) も .bin で残す")
    ap.add_argument("--yes", action="store_true", help="clear の確認を省く")
    args = ap.parse_args()

    if args.list:
        for p in serial.tools.list_ports.comports():
            print(f"{p.device}\t{p.description}")
        return
    if not args.port or not args.commands:
        ap.error("ポートとコマンドを指定してください (例: COM6 settings)")

    try:
        s = open_port(args.port)
    except serial.SerialException as e:
        sys.exit(f"{args.port} を開けません: {e}")
    time.sleep(0.5)
    s.reset_input_buffer()
    for cmd in args.commands:
        print(">> " + cmd, flush=True)
        if cmd == "save":
            save(s, args)
            continue
        line = to_device_command(cmd, args)
        if line is None:
            continue
        if not send(s, line, time.time() + 10):
            sys.exit(f"{args.port} に送れませんでした (基板がつながっているか確認してください)")
        show(s, args.wait, args.all)
    s.close()


if __name__ == "__main__":
    main()
