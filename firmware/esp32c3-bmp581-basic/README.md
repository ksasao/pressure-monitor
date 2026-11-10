# firmware / esp32c3-bmp581-basic

気圧ゆらぎセンサ開発ボード（ESP32-C3 + BMP581）で、独自のプログラムを作りたい方向けの、短いサンプルです（Arduino IDE 向け）。

スケッチ: [`esp32c3-bmp581-basic.ino`](esp32c3-bmp581-basic.ino)

次の 4 つの使い方を、それぞれ短い関数にまとめています。必要な関数だけを、自分のスケッチに写して使えます。

| 使い方 | 関数 | 使うピン |
|---|---|---|
| 気圧と温度を読む | `sensorBegin()`、`sensorRead()` | I2C（SDA: IO4、SCL: IO5）。BMP581 のアドレスは 0x46 |
| BOOT ボタンの状態を読む | `bootPressed()` | IO9（押すと LOW） |
| LED を点灯する | `ledBegin()`、`ledShow()`、`showPressureChange()`、`showAll()` | IO10（WS2812B × 5。ボードパッケージの RMT で送る） |
| 電池電圧を読む | `batteryMilliVolts()` | IO1（ADC。電池電圧の 1/2） |
| 電源の状態を読む | `poweredByUsb()` | IO3（USB で HIGH、電池で LOW） |

Qwiic コネクタは、BMP581 と同じ I2C バス（SDA: IO4、SCL: IO5）につながっています。Qwiic に I2C のセンサなどをつなぐときは、`Wire` をそのまま使えます（`Wire.begin()` は `sensorBegin()` の中で呼んでいます）。BMP581 のアドレス（0x46）と重ならない機器を使ってください。

## 動作

- 起動したときの気圧を基準にして、そこからの変化を、点灯する LED の数で表示します（1 Pa ごとに 1 個。1 Pa は高さ約 8.5 cm なので、約 9 cm ごとに 1 個）。
- 気圧が上がる（ボードを下げる）と青、下がる（持ち上げる）とオレンジです。
- BOOT ボタンを押すと、その時の気圧を新しい基準にします。押している間は、LED が白く光ります。
- BMP581 が見つからないときは、LED が赤く点滅します。
- シリアルモニタ（115200bps）に、1 秒ごとに、次の値を CSV で出します。

```
pressure_hPa,delta_Pa,temperature_C,boot,battery_mV,power
1013.2541,+0.37,24.81,released,2874,battery
```

| 列 | 内容 |
|---|---|
| `pressure_hPa` | 気圧 [hPa] |
| `delta_Pa` | 基準からの気圧の変化 [Pa] |
| `temperature_C` | BMP581 の温度 [°C]。基板の発熱を含むので、気温より高めになります |
| `boot` | BOOT ボタンの状態（`pressed` / `released`） |
| `battery_mV` | 電池電圧 [mV]。1000 未満なら、電池が入っていないか、電源スイッチ（SW1）が OFF |
| `power` | 給電元（`USB` / `battery`） |

## 必要なもの

| 項目 | 内容 |
|---|---|
| 開発環境 | Arduino IDE 2.x |
| ボードパッケージ | ESP32（Espressif Systems 製） |
| ライブラリ | **SparkFun BMP581 Arduino Library**（ライブラリマネージャで入れられます）。LED は、ESP32 のボードパッケージに入っている RMT の機能（`rmtInit()`、`rmtWrite()`）で点灯するので、LED のライブラリは不要です |

## ボード設定

| 設定 | 値 |
|---|---|
| Board | ESP32C3 Dev Module |
| USB CDC On Boot | **Enabled**（Disabled だと、シリアルモニタに何も出ません） |

そのほかは、既定のままで書き込めます。arduino-cli では `--fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc` を指定します。

書き込めないときは、BOOT ボタンを押したまま RESET ボタンを押して離し、そのあと BOOT ボタンを離すと、書き込みモードに入ります。

## 注意

- このサンプルを書き込むと、元のファームウェア（気圧の変化の表示、設定モード、記録など）は消えます。元に戻すときは、[書き込みツール](https://ksasao.github.io/pressure-monitor/esp32c3-bmp581/flasher/)から書き込み直してください。
- 電源の状態（IO3）は、2026-10-26 版（V1.1）の基板で使えます。2026-10-05 版（V1.0）の基板は、IO3 に何もつながっていないので、`power` の値は意味を持ちません。
- 省電力の処理（ライトスリープ）はしていません。電池で長く動かす場合は、[元のファームウェア](../esp32c3-bmp581/)の `power.cpp` を参考にしてください。
- LED の明るさは、電池での電流を抑えるため、`BRIGHTNESS`（既定 32）で小さめにしています。
- LED のライブラリを使わないのは、ビルドを速くするためです（FastLED は、ビルドするファイルが多く、時間がかかります）。たくさんの LED や、凝った効果を使うときは、FastLED や Adafruit NeoPixel を使うこともできます。
- シリアルの出力は、`Serial.setTxTimeoutMs(0)` で、待たないようにしています。こうしないと、USB で PC につないだまま、シリアルモニタを開いていないときに、出力のたびに待たされて、LED の更新が遅くなります。
- IO2 と IO8 は、起動のときの動作を決めるピン（ストラッピングピン）で、基板上で 10kΩ でプルアップしています。別の用途に使うときは、起動のときに LOW にならないようにしてください。
