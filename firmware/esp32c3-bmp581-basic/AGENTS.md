# 気圧ゆらぎセンサ開発ボード: AI 向けの基板の情報

このファイルは、AI（Claude Code、Codex、Cursor、ChatGPT など）に、この基板のファームウェアを書いてもらうときに渡す情報です。
人が読むための説明は、[AI でファームウェアを書き換える](https://ksasao.github.io/pressure-monitor/esp32c3-bmp581/vibe-coding/) にあります。

## 基板

| 項目 | 内容 |
|---|---|
| マイコン | ESP32-C3-WROOM-02-N4（フラッシュ 4MB、Wi-Fi / Bluetooth LE） |
| 気圧センサ | Bosch BMP581（I2C、アドレス 0x46。SDO が GND） |
| LED | WS2812B-V6 × 5（デイジーチェーン、色の順番は GRB） |
| ボタン | BOOT（IO9）、RESET（EN） |
| 拡張 | Qwiic コネクタ（I2C、3.3V）。BMP581 と同じ I2C バス |
| 電源 | 単三電池 2 本（TPS61291 で 3.3V に昇圧。SW1 は電池の電源スイッチ）、または USB-C（HT7833 で 3.3V）。TPS2116 が自動で切り替え、USB を優先 |
| USB | ESP32-C3 の内蔵 USB（USB Serial/JTAG）。USB シリアル変換 IC はない |
| 回路図 | https://ksasao.github.io/pressure-monitor/esp32c3-bmp581/#回路図（2026-10-26 版、V1.1） |

## ピン割り当て

| GPIO | 用途 | 注意 |
|---|---|---|
| 4 | I2C SDA（BMP581、Qwiic） | 4.7kΩ でプルアップ |
| 5 | I2C SCL（BMP581、Qwiic） | 4.7kΩ でプルアップ |
| 10 | LED1 の DIN（330Ω 経由） | |
| 9 | BOOT ボタン（押すと LOW） | ストラッピングピン。リセットのときに LOW だと、書き込みモードで起動する。`INPUT_PULLUP` で読む |
| 1 | 電池電圧（1MΩ + 1MΩ で 1/2 に分圧、ADC1） | `analogReadMilliVolts()` の値を 2 倍する。SW1 が OFF か、電池がないときは 1000 mV 未満（SW1 が OFF で約 0.3V） |
| 3 | 電源の状態（TPS2116 の ST、1MΩ でプルアップ） | USB 給電で HIGH、電池で LOW。V1.1 の基板だけ。V1.0（2026-10-05 版）は未接続 |
| 18 / 19 | USB D- / D+ | 使わない（USB が使えなくなる） |
| 2 / 8 | 何もつながっていない（10kΩ でプルアップ） | ストラッピングピン。リセットのときに LOW にしない |
| 0、6、7、20、21 | 何もつながっていない | 基板の外には出ていない |

外部の機器をつなげるのは、Qwiic コネクタ（I2C）だけです。

## 開発環境

| 項目 | 内容 |
|---|---|
| IDE | Arduino IDE 2.x、または arduino-cli |
| ボードパッケージ | ESP32（Espressif Systems）**3.x**（3.3.12 で確認） |
| ボード | ESP32C3 Dev Module |
| 必須の設定 | **USB CDC On Boot: Enabled**（`Serial` が USB に出る。Disabled だと何も出ない） |
| FQBN | `esp32:esp32:esp32c3:CDCOnBoot=cdc` |
| 気圧センサのライブラリ | SparkFun BMP581 Arduino Library（1.0.1 で確認） |
| LED | ライブラリなしで、ボードパッケージの `rmtInit()` / `rmtWrite()` で送れる（サンプルの `ledShow()`）。FastLED も使えるが、ビルドが遅い |

arduino-cli でのビルドと書き込み:

```
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc <スケッチのフォルダ>
arduino-cli upload  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc -p <COM ポート> <スケッチのフォルダ>
```

## コードを書くときの決まり

- **ESP32 ボードパッケージ 3.x の API を使う。** 2.x の書き方は、コンパイルできない。
  - LEDC: `ledcAttach(pin, freq, bits)` / `ledcWrite(pin, duty)`（2.x の `ledcSetup()`、`ledcAttachPin()` は使えない）
  - タイマー: `timerBegin(frequency)`（2.x の `timerBegin(num, divider, countUp)` は使えない）
  - RMT: `rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, frequency)`
- **`setup()` で `Serial.setTxTimeoutMs(0);` を呼ぶ。** 呼ばないと、USB で PC につながっていて、シリアルモニタを開いていないときに、出力のたびに最大 0.1 秒ずつ待たされ、ループが遅くなる。
- I2C は `Wire.begin(4, 5)`。Qwiic の機器も、同じ `Wire` で使う。BMP581 のアドレス（0x46）と重ならないようにする。
- WS2812B のタイミング（RMT を 10MHz にしたとき）: 0 は HIGH 3・LOW 9、1 は HIGH 8・LOW 4（単位 0.1 µs）。上位ビットから、G、R、B の順に送る。
- LED の明るさは控えめにする（各色 0〜255 のうち 64 以下が目安）。電池では、白の全点灯と Wi-Fi の送信が重なると、電流が足りずにリセットすることがある。Wi-Fi を使うときは、`WiFi.setTxPower(WIFI_POWER_8_5dBm)` などで送信の強さを下げると、起きにくくなる。
- BMP581 は、気圧を 1/64 Pa 単位で出す（ライブラリの `pressure` は Pa の float）。1 Pa は高さ約 8.5 cm。30Hz でオーバーサンプリング 32 倍のとき、1 サンプルのノイズは約 0.12〜0.19 Pa。測る間隔（ODR）とオーバーサンプリングには、組み合わせの上限がある（30Hz なら 32 倍まで）。
- 温度は BMP581 の値。基板の発熱を含むので、気温より高めになる。
- ライトスリープやディープスリープを使うと、眠っている間は USB が使えず、PC からポートが見えなくなる。書き込めなくなったときは、BOOT を押したまま RESET を押して離し、BOOT を離すと、書き込みモードに入る。

## PC からシリアルを読むスクリプトを書くとき

ESP32-C3 の内蔵 USB では、DTR と RTS がリセットと書き込みモードの操作に使われます。pyserial の既定のまま（DTR・RTS が ON）でポートを開いて閉じると、基板が止まったままになり、RESET ボタンでも戻らないことがあります。**ポートを開く前に、DTR と RTS を False にしてください。**

```python
import serial
s = serial.Serial()
s.port = "COM5"
s.baudrate = 115200
s.timeout = 1
s.dtr = False
s.rts = False
s.open()
```

止まってしまったときは、USB ケーブルを抜き差しすると戻ります。

## 元のファームウェアについて

- 元のファームウェアは、ブラウザの書き込みツールで、いつでも書き戻せます: https://ksasao.github.io/pressure-monitor/esp32c3-bmp581/flasher/
- 元のファームウェアのソースは [`firmware/esp32c3-bmp581/`](../esp32c3-bmp581/) にあります。参考になるもの:
  - `sensor.cpp`: BMP581 の FIFO を使って、30Hz の値を取りこぼさずに読む
  - `power.cpp`: USB がつながっていないときのライトスリープ（電池を長く持たせる）
  - `devlog.cpp`: フラッシュへの記録
  - `high_pass_filter.cpp`、`palette.cpp`: 気圧の変化を色にする
- 元のファームウェアは、フラッシュの 0x210000 から約 1.9MB を、記録の領域に使っています。独自のスケッチで SPIFFS や LittleFS を使うと、この記録は消えます。
- 設定は NVS（0x9000）に保存されています。Arduino IDE の「Erase All Flash Before Sketch Upload」を Enabled にしなければ、独自のスケッチを書き込んでも残ります。
