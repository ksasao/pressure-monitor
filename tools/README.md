# tools

開発と試験に使うスクリプトです。

| スクリプト | 内容 | 必要なもの |
|---|---|---|
| [`release.mjs`](release.mjs) | ファームウェアをビルドし、書き込みツール（`docs/<variant>/flasher/`）にリリースを置く | Node.js、Arduino IDE 2.x（arduino-cli） |
| [`serve-docs.mjs`](serve-docs.mjs) | `docs/` を `http://localhost:8000/` で公開する（書き込みツールを、GitHub Pages に上げる前に試す） | Node.js |
| [`serial-command.py`](serial-command.py) | 基板に、シリアルからコマンドを送る（BOOT ボタンの操作の代わり、状態の表示、記録の開始・停止・消去・保存） | Python 3、pyserial |
| [`logbin.py`](logbin.py) | 基板の記録（`/log.bin` の形）を、`/log.csv` と同じ内容の CSV に戻す | Python 3 |

`release.mjs` と `serve-docs.mjs` の使い方は、[RELEASING.md](../RELEASING.md) を見てください。

## serial-command.py

基板（esp32c3-bmp581）に、シリアルからコマンドを送り、基板が出したメッセージ（`#` で始まる行）を表示します。BOOT ボタンを押さずに、設定モード（Wi-Fi）と通常モードを切り替えたり、Wi-Fi を使わずに、基板の中への記録を始めたり、CSV で保存したりできます。試験のときや、ケースに入れてボタンを押しにくいときに使います。

### 準備

```bash
pip install pyserial
```

基板を USB で PC につなぎます。ポートの名前は、次のコマンドで確かめられます（Windows は `COM6` など、Linux は `/dev/ttyACM0` など、macOS は `/dev/cu.usbmodem*`）。

```bash
python tools/serial-command.py --list
```

### 使い方

```bash
python tools/serial-command.py COM6 settings
```

```
>> settings
  # serial: settings
  # rebooting into settings (Wi-Fi) mode
  === PressureMonitor 2026-10-31 ===
  # reset reason: SW -> settings mode
  # settings mode: Wi-Fi AP started (open, no password)
  #   URL  : http://192.168.4.1/
  ...
```

| コマンド | 動作 |
|---|---|
| `settings` | 設定モード（Wi-Fi）で再起動する |
| `normal` | 通常モードで再起動する |
| `long` | 長押しと同じ（設定モードと通常モードを切り替える） |
| `short` | 短押しと同じ（モードの番号を見せる。見せている間に送ると、次のモードへ） |
| `status` | 今の設定、Wi-Fi・無線の状態、基板の中の記録の量などを、まとめて出す（[firmware/esp32c3-bmp581/README.md](../firmware/esp32c3-bmp581/README.md) の「シリアルからの操作」） |
| `start` | 基板の中への記録を始める（間隔は `--interval`）。PC の時計とタイムゾーンを、基板に送る |
| `stop` | 記録を止める。PC の時計を送り、その間の基板の時計のずれを補正する |
| `save` | 記録を受け取り、CSV に保存する（下の「記録の操作」） |
| `clear` | 記録を消去する（確認する。`--yes` で確認を省く） |
| `help` | コマンドの一覧を出す |

コマンドは、続けて書くと順に送ります。たとえば、`short short` は、モードの番号を見せてから、次のモードへ進めます（2 回目は、番号を見せている間に届くように、`--wait 1` などで待ち時間を短くします）。

```bash
python tools/serial-command.py COM6 short short --wait 1
```

設定や記録の状態は、`status` でまとめて見られます。

```bash
python tools/serial-command.py COM6 status --wait 1
```

```
>> status
  # status firmware=2026-10-31
  # status name=ps-3aa614
  # status mac=68:c6:3a:3a:a6:14
  # status boot_mode=normal
  ...
  # status battery_mv=2516
  # status usb_host=yes
  # status sensor=ok
  # status pressure_hpa=1008.9617
  # status temperature_c=27.38
  # status display_mode=1 標準
  # status sense=limit=50 range=753 hpf_shift=8 trail_step=1 hold_shift=0
  ...
  # status wifi=off
  # status espnow=off
  # status log.area=ok used=20338 total=1966080 (1%)
  # status log.state=stopped
  # status log.start=2026-10-31 13:04:44 UTC+09:00
  # status log.interval=1/30s
  # status log.samples=14540 capacity=1756387 remaining=16.1h
  # status log.format=v6
  # status log.recorded_as=name=ps-3aa614 role=standalone mode=1 wifi=off
  # status sensor_period_us=33311.7 (measured)
  # status end=ok
```

### 記録の操作

Wi-Fi を使わずに、基板の中への記録を始め、止め、CSV で保存できます。

```bash
python tools/serial-command.py COM6 start --interval 1/30
python tools/serial-command.py COM6 stop status
python tools/serial-command.py COM6 save --out C:/data
```

```
>> save
  受信しました 0.01 MB (0.0 秒)
  CSV: C:/data\pressure-monitor_ps-3aa614_20261031-135648.csv (0.3 MB、3,274 行、作成 0.0 秒)
```

- `start` は、今のモードのまま記録します。通常モード（Wi-Fi オフ）なら、そのまま省電力で記録が続きます（「記録して Wi-Fi をオフにする」と同じ）。設定モードなら、Wi-Fi のまま記録します。記録を始めると、前の記録は消えます。
- 役割が送信・ハブのときは、無線が動き続け、電池の消費が大きい旨の警告が出ます（表示ページと違い、役割は切り替えません）。
- `stop` は、PC の時計を送ります。表示ページの「■ 停止」と同じく、記録の間の基板の時計のずれを、直線で補正します。
- `save` は、記録を `/log.bin` と同じ形で受け取り（`dump`）、[`logbin.py`](logbin.py) で CSV に戻します。表示ページの「CSV 保存」と同じ内容（1 バイトも違わない）の CSV になります。受け取ったデータは CRC-32 で確かめます。ファイル名も表示ページと同じ `pressure-monitor_<装置の名前>_<日時>.csv` です。
- 記録中は保存できません。先に `stop` を送ってください（送っている間は、基板の処理が止まり、記録に抜けができるため）。
- 受け取る量は、1/30 秒の 16 時間分でも約 2MB です。CSV の作成は、117 万行（約 11 時間分）で約 7 秒でした（PC による）。

| オプション | 内容 |
|---|---|
| `--wait 秒` | コマンドごとに、応答を待つ秒数（既定 6）。再起動するコマンドで、起動時のメッセージまで見るには、6 秒くらい必要です |
| `--all` | `#` の行だけでなく、気圧の CSV の行も表示する |
| `--list` | シリアルポートの一覧を表示する |
| `--interval 1/30\|1\|30` | `start` の記録の間隔 [秒]（既定 1/30。1 と 30 は、その間の平均） |
| `--out パス` | `save` の保存先（ファイル名、またはフォルダ）。既定は今のフォルダ |
| `--bin` | `save` で、記録（`/log.bin` の形）も `.bin` で残す |
| `--yes` | `clear` の確認を省く |

## logbin.py

基板の記録（`/log.bin` の形）から、基板の `/log.csv` と同じ内容の CSV を作ります。`serial-command.py save --bin` で残した `.bin` や、ブラウザで `http://192.168.4.1/log.bin` を開いて保存したファイルに使えます。計算は、表示ページの JavaScript（`monitor_page.cpp` の `binToCsv`）と同じで、記録の形式（版 2〜6）は [`devlog.h`](../firmware/esp32c3-bmp581/devlog.h) にあります。

```bash
python tools/logbin.py log.bin
python tools/logbin.py log.bin out.csv
```

### 注意

- ポートを開くときに、DTR・RTS を False にします。そのため、ポートを開いただけでは、基板はリセットされません（Arduino IDE のシリアルモニタなどで開くと、リセットされることがあります）。
- 再起動するコマンド（`settings`、`normal`、`long`）のあとは、USB がいったん切れます。スクリプトは、つながり直すのを待って、起動時のメッセージも表示します。
- ほかのアプリ（シリアルモニタなど）がポートを開いていると、使えません。
- 工場出荷時に戻す操作（BOOT ボタンの超長押し）は、誤って送らないように、コマンドにしていません。
- `start`、`stop`、`clear`、`dump` は、2026-10-31 版のあとに加えたコマンドです。古いファームウェアでは、コマンドの一覧が出るだけで、何もしません。
- コマンドの処理は、ファームウェアの `handleSerialCommand`（[`esp32c3-bmp581.ino`](../firmware/esp32c3-bmp581/esp32c3-bmp581.ino)）です。説明は [firmware/esp32c3-bmp581/README.md](../firmware/esp32c3-bmp581/README.md) の「シリアルからの操作」にもあります。
