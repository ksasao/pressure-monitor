---
layout: default
title: AI でファームウェアを書き換える
---

# AI でファームウェアを書き換える（Vibe Coding）

[esp32c3-bmp581 のページへ戻る](../)

作りたいものを AI（Claude、ChatGPT、Gemini など）に言葉で伝えて、ファームウェアを書いてもらう方法です。プログラムを書いたことがなくても、AI とやり取りしながら、基板の動きを自由に変えられます。

AI が正しいコードを書けるように、この基板の情報（ピン割り当て、部品、注意点）を 1 つのファイルにまとめました。**最初に、このファイルを AI に渡してください。**

**[基板の情報（AGENTS.md）](https://github.com/ksasao/pressure-monitor/blob/main/firmware/esp32c3-bmp581-basic/AGENTS.md)**

**ご注意:** 独自に書き換えたファームウェア（AI に書いてもらったものを含みます）の動作は、サポートの対象外です。[書き込みツール](../flasher/)で元のファームウェアに戻しても起きる不具合は、お問い合わせください。

## 用意するもの

| もの | 内容 |
|---|---|
| パソコン | Windows、macOS、Linux |
| Arduino IDE 2.x | ボードパッケージ「esp32」（Espressif Systems 製、3.x）と、ライブラリ「SparkFun BMP581 Arduino Library」を入れます。入れ方は、[サンプルの README](https://github.com/ksasao/pressure-monitor/tree/main/firmware/esp32c3-bmp581-basic) をご覧ください |
| USB-C ケーブル | データ通信ができるもの（充電専用のケーブルでは、書き込めません） |
| AI | チャットで使うもの（ChatGPT、Claude、Gemini など）でも、パソコンの中で直接ファイルを書き換えるもの（Claude Code、Codex、Cursor など）でも使えます |

## 進め方

### 1. 基板の中の記録を保存しておく

元のファームウェアで基板の中に記録したデータがある場合は、先に CSV で保存してください。独自のファームウェアを書き込むと、記録が消えることがあります。設定（明るさなど）は、通常は残ります。

### 2. サンプルを書き込んで、動くことを確かめる

まず、[サンプル（esp32c3-bmp581-basic）](https://github.com/ksasao/pressure-monitor/tree/main/firmware/esp32c3-bmp581-basic)を、Arduino IDE で書き込みます。ボードは「ESP32C3 Dev Module」、**「USB CDC On Boot」は「Enabled」**にします。

基板を持ち上げると LED がオレンジに、下げると青に光れば、開発環境は整っています。AI に頼むときも、このサンプルを出発点にすると、うまくいきやすくなります。

### 3. AI に基板の情報を渡す

| 使う AI | 渡し方 |
|---|---|
| チャットの AI（ChatGPT、Claude、Gemini など） | [AGENTS.md](https://raw.githubusercontent.com/ksasao/pressure-monitor/main/firmware/esp32c3-bmp581-basic/AGENTS.md) を開いて、全文をコピーし、最初のメッセージに貼り付けます。サンプルのスケッチ（`esp32c3-bmp581-basic.ino`）も貼り付けると、さらに確実です |
| ファイルを直接書き換える AI（Claude Code、Codex、Cursor など） | リポジトリをダウンロード（または `git clone`）して、`firmware/esp32c3-bmp581-basic` のフォルダで AI を起動します。AGENTS.md（Claude Code は CLAUDE.md）を、AI が自動で読みます |

### 4. 作りたいものを伝える

できるだけ具体的に伝えます。「何を」「どんなときに」「どう見せるか」を書くと、思った通りのものになりやすくなります。

| 頼み方の例 |
|---|
| サンプルをもとに、気圧の変化を Arduino IDE のシリアルプロッタでグラフにできるようにしてください |
| BOOT ボタンを押すたびに、LED の色が 赤 → 緑 → 青 の順に変わるようにしてください |
| 電池の電圧が 2.2V を下回ったら、LED1 を赤く点滅させてください |
| 基板を 1 m 持ち上げたら、5 個の LED が虹色に光るようにしてください |
| Qwiic コネクタにつないだ温湿度センサ（SHT40）の湿度も測って、シリアルに出してください |

### 5. ビルドして書き込む

AI が書いたスケッチを Arduino IDE に貼り付けて、書き込みます。ファイルを直接書き換える AI なら、ビルドと書き込みも任せられます。

**エラーが出たら、エラーの文章を、そのまま AI に貼り付けて、「直してください」と頼みます。** 動きが思った通りでないときも、「LED が光らない」「2 秒に 1 回しか変わらない」のように、起きていることを伝えると、AI が原因を探します。

### 6. 元に戻す

元のファームウェアには、[書き込みツール](../flasher/)から、いつでも戻せます。

## うまくいかないとき

| 起きていること | 試すこと |
|---|---|
| 書き込めない、ポートが表示されない | BOOT ボタンを押したまま、RESET ボタンを押して離し、BOOT ボタンを離します（書き込みモードに入ります）。ケーブルが、データ通信のできるものかも確かめてください |
| シリアルモニタに何も出ない | 「USB CDC On Boot」を「Enabled」にして、書き込み直します |
| `ledcSetup` や `timerBegin` などで、コンパイルエラーになる | AI が、古いボードパッケージ（2.x）の書き方をしています。「ESP32 のボードパッケージ 3.x の書き方に直してください」と頼みます |
| LED やボタンの反応が、急に遅くなる | シリアルの出力で待たされています。`setup()` に `Serial.setTxTimeoutMs(0);` があるか、AI に確かめてもらいます |
| 電池で動かすと、途中でリセットする | LED が明るすぎるか、Wi-Fi の電流が大きすぎます。LED を暗くするか、Wi-Fi の送信の強さを下げるよう、AI に頼みます |
| 省電力にしたら、パソコンから見えなくなった | 眠っている間は、USB が使えません。BOOT ボタンを押したまま RESET ボタンを押すと、書き込めます |
| AI が書いた Python のスクリプトで、シリアルを読んだら、基板が止まった | USB ケーブルを抜き差しします。スクリプトは、ポートを開く前に DTR と RTS を False にする必要があります（AGENTS.md に書いてあります） |

## 参考

- [サンプル（esp32c3-bmp581-basic）](https://github.com/ksasao/pressure-monitor/tree/main/firmware/esp32c3-bmp581-basic): 気圧と温度、BOOT ボタン、LED、電池電圧、電源の状態の使い方
- [元のファームウェアのソース](https://github.com/ksasao/pressure-monitor/tree/main/firmware/esp32c3-bmp581): 省電力、記録、設定モード（Wi-Fi）などの、本格的な使い方
- [回路図](../#回路図)
