# pressure-monitor

気圧の「変化」を光で見せる小さな装置です。センサを持ち上げたり下ろしたりするだけで、高さの違いによるごくわずかな気圧の差が、LED の色と明るさになって見えます。

![手を動かすと、気圧の変化に応じて LED が青やオレンジに光る様子](docs/assets/images/esp32c3-bmp581/demo.gif)

手を動かして気圧を変化させたときの様子です（基板 14 台を並べて撮影）。

- 気圧が **上がる**（センサを下げる）と **青**、**下がる**（持ち上げる）と **オレンジ**
- 変化が大きいほど明るくなり、最大で白になる
- 最新の色が LED1 に出て、1 フレームごとに後ろの LED へ流れていく
- 気圧の絶対値ではなく、ハイパスフィルタで取り出した「変化」だけを表示する（ゆっくりしたドリフトは消える）

## 実装一覧

組み合わせ（マイコン-気圧センサ）ごとにフォルダを分けています。

| 名前 | マイコン | 気圧センサ | LED | 状態 |
|---|---|---|---|---|
| [esp32c3-bmp581](firmware/esp32c3-bmp581/) | ESP32-C3 | BMP581 | WS2812B × 5 | 動作確認済み |

## フォルダ構成

```
pressure-monitor/
├── firmware/                  ファームウェア
│   └── esp32c3-bmp581/        ESP32-C3 + BMP581 (Arduino スケッチ)
├── hardware/                  回路図・基板データ
│   └── esp32c3-bmp581/
├── docs/                      GitHub Pages の資料 (使い方、回路図など)
│   ├── index.md
│   ├── esp32c3-bmp581/
│   └── assets/images/
├── LICENSE
└── README.md
```

新しいマイコンやセンサの組み合わせを追加するときは、同じ名前（`<マイコン>-<センサ>`）で `firmware/`、`hardware/`、`docs/` の 3 か所にフォルダを作ります。

## クイックスタート（esp32c3-bmp581）

1. Arduino IDE 2.x に、ESP32 のボードパッケージ（Espressif Systems 製）を入れる
2. ライブラリマネージャから **SparkFun BMP581 Arduino Library** と **FastLED**（3.7.0 以降）を入れる
3. ボードは **ESP32C3 Dev Module**、**USB CDC On Boot** は **Enabled** にする
4. [`firmware/esp32c3-bmp581/esp32c3-bmp581.ino`](firmware/esp32c3-bmp581/esp32c3-bmp581.ino) を開いて書き込む

詳しい手順、ピン割り当て、調整できるパラメータは [firmware/esp32c3-bmp581/README.md](firmware/esp32c3-bmp581/README.md) を見てください。

## ドキュメント

使い方や回路図は GitHub Pages にまとめます（`docs/` フォルダ）。

## ライセンス

[Apache License 2.0](LICENSE)
