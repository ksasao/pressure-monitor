# ファームウェアのリリース手順

ファームウェアは、GitHub Pages の書き込みツールから配布します。利用者は、パソコンの Chrome / Edge でページを開き、USB でつないだ基板に、ブラウザから書き込めます（Arduino IDE は不要です）。

- 書き込みツール: <https://ksasao.github.io/pressure-monitor/esp32c3-bmp581/flasher/>
- バージョンは、リリース日（`YYYY-MM-DD`）です。同じ日に 2 回以上リリースしたときは、`YYYY-MM-DD.2` のようになります。
- リリースは、全て書き込みツールに残り、利用者が選べます（既定は最新）。`?v=2026-10-20` を付けたアドレスで、バージョンを指定して開くこともできます。
- 書き込んだバージョンは、基板のシリアル（起動時の `=== PressureMonitor 2026-10-20 ===`）、設定ページ、ハブの画面で確認できます。

## 全体の流れ

```
1. Arduino IDE で、実機で動作を確認する         （手作業）
2. ソースの変更をコミットする                     （git commit）
3. node tools/release.mjs --notes "変更点"         （ビルドして、docs にバイナリを置く）
4. node tools/serve-docs.mjs で、手元で書き込みを試す（任意）
5. コミットしてプッシュする                       （GitHub Pages に反映される）
```

3 と 5 は、`node tools/release.mjs --notes "変更点" --push` の 1 回でまとめて行えます。

## 準備（初回だけ）

| もの | 内容 |
|---|---|
| Arduino IDE 2.x | ボードパッケージとライブラリは、下の「Arduino IDE の設定」のとおり。リリースのスクリプトは、IDE に同梱の `arduino-cli` を使います |
| Node.js 18 以降 | リリースのスクリプトを動かすために使います。`npm install` は不要です |
| git | コミットとプッシュに使います |

`arduino-cli` は、次の順に探します: 環境変数 `ARDUINO_CLI` → `PATH` → Arduino IDE に同梱のもの（Windows は `%LOCALAPPDATA%\Programs\Arduino IDE\...`、macOS は `/Applications/Arduino IDE.app/...`）。IDE と同じボードパッケージとライブラリを使うので、IDE で入れたものが、そのままビルドに使われます。

## 1. Arduino IDE で行うこと（リリース前の確認）

### Arduino IDE の設定

| 項目 | 値 |
|---|---|
| ボード | ESP32C3 Dev Module |
| USB CDC On Boot | Enabled |
| Partition Scheme | No OTA (2MB APP/2MB SPIFFS) |
| そのほか | 既定のまま（Flash Size 4MB など） |

リリースのスクリプトは、この設定と同じ条件（FQBN `esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=no_ota`）でビルドします。設定を変えたときは、[`tools/release.mjs`](tools/release.mjs) の `fqbn` も合わせてください。

動作を確認したボードパッケージとライブラリ:

| 名前 | バージョン |
|---|---|
| esp32（Espressif Systems） | 3.3.12 |
| SparkFun BMP581 Arduino Library | （ライブラリマネージャの最新） |
| FastLED | 3.10.6 |
| ESP Async WebServer（ESP32Async） | 3.12.1 |
| Async TCP（ESP32Async） | 3.5.0 |

更新したときは、この表も更新してください。

### 実機での確認

IDE から実機に書き込み、少なくとも次を確認します（変更した機能は、重点的に）。

- [ ] 電源を入れると、白で全点灯したあと、赤 → 緑 → 青の自己診断が流れる
- [ ] センサを上下に動かすと、LED が青やオレンジに光る
- [ ] BOOT ボタンの短押しで、モードの番号が青く表示され、続けて押すとモードが変わる
- [ ] BOOT ボタンの長押しで設定モードに入り、スマホで気圧の表示と設定のページが開ける
- [ ] 設定を変えて保存し、「保存して終了」で通常モードに戻る
- [ ] （複数台の表示を変えたとき）送信の装置とハブで、ハブの画面にデータが表示される
- [ ] シリアルモニタ（115200bps）に、CSV と `# perf` の行が出る

### コミット

確認したソースを、コミットしておきます。リリースのスクリプトは、`firmware/<組み合わせ>/` にコミットしていない変更があると、止まります（どのソースからビルドしたかを、リリースに記録するためです）。

## 2. リリースを作る

```bash
node tools/release.mjs --notes "固定色モードで、気圧の変化を明るさに反映できるようにした"
```

スクリプトが行うこと:

1. [`firmware/esp32c3-bmp581/version.h`](firmware/esp32c3-bmp581/version.h) の `FIRMWARE_VERSION` を、今日の日付にする
2. `arduino-cli` でビルドする（初回は 10 分ほど。2 回目からは、変更したファイルだけをビルドするので速くなります）
3. できたバイナリに、そのバージョンの文字列が含まれていることを確かめる
4. `docs/esp32c3-bmp581/flasher/releases/<バージョン>/` に、バイナリ 4 つ（bootloader、partitions、boot_app0、application）と `flash_config.json`（書き込むアドレスと SHA-256）を置く
5. `docs/esp32c3-bmp581/flasher/releases.json`（リリースの一覧）の先頭に、加える

リリースノートは、書き込みツールでバージョンを選んだときに表示されます。改行を含めたいときは、ファイルに書いて `--notes-file notes.txt` で渡します。`--notes` を省くと、入力を求められます。

### オプション

| オプション | 内容 |
|---|---|
| `--notes "..."` / `--notes-file <file>` | リリースノート |
| `--version 2026-10-20` | バージョンを指定する（既定は今日の日付） |
| `--force` | 同じバージョンのリリースを作り直す |
| `--commit` | 作ったあと、`docs/.../flasher` と `version.h` をコミットする |
| `--push` | コミットして、プッシュする（`--commit` を含む） |
| `--skip-build` | ビルドせず、Arduino IDE で書き出したバイナリを使う（下記） |
| `--allow-dirty` | コミットしていない変更があっても進める（試すときだけ） |
| `--list` | リリースの一覧を表示する |
| `--remove 2026-10-20` | リリースを、一覧と docs から削除する（コミットとプッシュは手で行う） |

### arduino-cli を使わない場合（Arduino IDE で書き出す）

1. `node tools/release.mjs --notes "..." --skip-build` を実行する。`version.h` を書き換えたあと、「書き出したバイナリがない（またはバージョンが古い）」と言って止まります
2. Arduino IDE で、メニューの「スケッチ」→「コンパイルしたバイナリをエクスポート」を実行する（`firmware/esp32c3-bmp581/build/` に出力されます。このフォルダは git に含めません）
3. もう一度、同じコマンドを実行する

## 3. 手元で書き込みを試す（任意）

```bash
node tools/serve-docs.mjs
```

Chrome で <http://localhost:8000/esp32c3-bmp581/flasher/> を開き、作ったバージョンを選んで、実機に書き込みます（Web Serial は、HTTPS か localhost でしか使えないため、ファイルを直接開くのではなく、このサーバを使います）。

## 4. 公開する

```bash
git add docs/esp32c3-bmp581/flasher firmware/esp32c3-bmp581/version.h
git commit -m "ファームウェア 2026-10-20 をリリース"
git push
```

GitHub Pages（`main` ブランチの `docs/`）に、数分で反映されます。書き込みツールを開いて、新しいバージョンが「（最新）」として表示されることを確認してください。

## 注意

- **通信の形式を変えたとき**（`link.h` の `LINK_VERSION`）は、古いバージョンの装置とは、複数台の表示で通信できません。リリースノートに「全ての装置に書き込んでください」と書いてください。
- **パーティションの構成を変えたとき**（Partition Scheme）は、リリースノートで「全消去」を勧めてください。
- 書き込みツールは、既定では設定（NVS）を消さずに書き込みます。「全消去」に印を付けると、設定も初期値に戻ります。
- バイナリは git に含めるので、リリースごとに約 1.3MB 増えます。古いリリースが不要になったら、`--remove` で削除できます（git の履歴には残ります）。
- リリースの後に手元でビルドしたものも、次のリリースまでは、同じバージョンを表示します。

## ファイルの構成

```
tools/
├── release.mjs                     リリースを作るスクリプト
├── serve-docs.mjs                  docs/ を localhost で公開する (確認用)
└── serial-command.py               シリアルから BOOT ボタンの操作を送る (試験用。tools/README.md)
docs/esp32c3-bmp581/flasher/        書き込みツール (GitHub Pages)
├── index.html, flasher.js, style.css
├── releases.json                   リリースの一覧 (新しい順)
└── releases/<バージョン>/
    ├── bootloader.bin              0x0
    ├── partitions.bin              0x8000
    ├── boot_app0.bin               0xe000
    ├── application.bin             0x10000
    └── flash_config.json           書き込むアドレス、サイズ、SHA-256、ビルドしたコミット
firmware/esp32c3-bmp581/version.h   ファームウェアのバージョン (スクリプトが書き換える)
```
