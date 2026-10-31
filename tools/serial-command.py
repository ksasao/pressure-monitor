#!/usr/bin/env python3
"""
serial-command.py - 基板 (esp32c3-bmp581) に、シリアルからコマンドを送る

BOOT ボタンの操作の代わりのコマンドを送り、基板が出したメッセージ (# で始まる行) を表示します。
試験のときや、ケースに入れてボタンを押しにくいときに使います。
コマンドの一覧は firmware/esp32c3-bmp581/README.md の「シリアルからの操作」を見てください。

  python tools/serial-command.py COM6 settings          設定モード (Wi-Fi) で再起動する
  python tools/serial-command.py COM6 normal            通常モードで再起動する
  python tools/serial-command.py COM6 short short       短押しを 2 回 (番号を見せてから、次のモードへ)
  python tools/serial-command.py --list                 シリアルポートの一覧
  python tools/serial-command.py COM6 settings --wait 8 --all

ポートを開くときに DTR・RTS を False にするので、ポートを開いただけでは、基板はリセットされません。
再起動するコマンドのあとは、USB がつながり直すのを待って、起動時のメッセージも表示します。

必要なもの: Python 3、pyserial (pip install pyserial)
"""
import argparse
import sys
import time

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
                    help="送るコマンド (settings、normal、long、short、help)。複数なら順に送る")
    ap.add_argument("--wait", type=float, default=6.0,
                    help="コマンドごとに、応答を待つ秒数 (既定 6。再起動の起動メッセージまで見るには 6 以上)")
    ap.add_argument("--all", action="store_true",
                    help="# の行だけでなく、気圧の CSV の行も表示する")
    ap.add_argument("--list", action="store_true", help="シリアルポートの一覧を表示する")
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
        if not send(s, cmd, time.time() + 10):
            sys.exit(f"{args.port} に送れませんでした (基板がつながっているか確認してください)")
        show(s, args.wait, args.all)
    s.close()


if __name__ == "__main__":
    main()
