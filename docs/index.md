---
layout: default
title: pressure-monitor
---

# pressure-monitor

気圧の「変化」を光で見せる小さな装置です。センサを持ち上げたり下ろしたりするだけで、高さの違いによるごくわずかな気圧の差が、LED の色と明るさになって見えます。

- 気圧が **上がる**（センサを下げる）と **青**、**下がる**（持ち上げる）と **オレンジ**
- 変化が大きいほど明るくなり、最大で白になる
- 最新の色が LED1 に出て、1 フレームごとに後ろの LED へ流れていく

## 実装一覧

| 名前 | マイコン | 気圧センサ | 状態 |
|---|---|---|---|
| [esp32c3-bmp581](esp32c3-bmp581/) | ESP32-C3 | BMP581 | 動作確認済み |

esp32c3-bmp581 版については、[よくある質問](esp32c3-bmp581/faq/)もご覧ください。

## ソースコード

[github.com/ksasao/pressure-monitor](https://github.com/ksasao/pressure-monitor)
