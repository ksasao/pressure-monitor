# tools

開発と試験に使うスクリプトです。

| スクリプト | 内容 | 必要なもの |
|---|---|---|
| [`release.mjs`](release.mjs) | ファームウェアをビルドし、書き込みツール（`docs/<variant>/flasher/`）にリリースを置く | Node.js、Arduino IDE 2.x（arduino-cli） |
| [`serve-docs.mjs`](serve-docs.mjs) | `docs/` を `http://localhost:8000/` で公開する（書き込みツールを、GitHub Pages に上げる前に試す） | Node.js |
| [`serial-command.py`](serial-command.py) | 基板に、シリアルから BOOT ボタンの操作の代わりのコマンドを送る | Python 3、pyserial |

`release.mjs` と `serve-docs.mjs` の使い方は、[RELEASING.md](../RELEASING.md) を見てください。

## serial-command.py

基板（esp32c3-bmp581）に、シリアルからコマンドを送り、基板が出したメッセージ（`#` で始まる行）を表示します。BOOT ボタンを押さずに、設定モード（Wi-Fi）と通常モードを切り替えられます。試験のときや、ケースに入れてボタンを押しにくいときに使います。

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
| `help` | コマンドの一覧を出す |

コマンドは、続けて書くと順に送ります。たとえば、`short short` は、モードの番号を見せてから、次のモードへ進めます（2 回目は、番号を見せている間に届くように、`--wait 1` などで待ち時間を短くします）。

```bash
python tools/serial-command.py COM6 short short --wait 1
```

| オプション | 内容 |
|---|---|
| `--wait 秒` | コマンドごとに、応答を待つ秒数（既定 6）。再起動するコマンドで、起動時のメッセージまで見るには、6 秒くらい必要です |
| `--all` | `#` の行だけでなく、気圧の CSV の行も表示する |
| `--list` | シリアルポートの一覧を表示する |

### 注意

- ポートを開くときに、DTR・RTS を False にします。そのため、ポートを開いただけでは、基板はリセットされません（Arduino IDE のシリアルモニタなどで開くと、リセットされることがあります）。
- 再起動するコマンド（`settings`、`normal`、`long`）のあとは、USB がいったん切れます。スクリプトは、つながり直すのを待って、起動時のメッセージも表示します。
- ほかのアプリ（シリアルモニタなど）がポートを開いていると、使えません。
- 工場出荷時に戻す操作（BOOT ボタンの超長押し）は、誤って送らないように、コマンドにしていません。
- コマンドの処理は、ファームウェアの `handleSerialCommand`（[`esp32c3-bmp581.ino`](../firmware/esp32c3-bmp581/esp32c3-bmp581.ino)）です。説明は [firmware/esp32c3-bmp581/README.md](../firmware/esp32c3-bmp581/README.md) の「シリアルからの操作」にもあります。
