/*
 * devlog.h - 基板の中への記録 (設定モードの表示ページから操作する)
 *
 * 気圧のサンプル (30Hz) を、フラッシュの空き領域 (パーティション "spiffs"。Partition Scheme が
 * No OTA なら約 1.9MB) に記録します。記録は CSV として、http://192.168.4.1/log.csv から
 * ダウンロードできます。約 5.5 時間記録できます (いっぱいになると、自動で止まる)。
 *
 * ブラウザの中に記録する方式にしないのは、Android の Wi-Fi のログイン画面 (自動で開く画面) が、
 * ブラウザの中で作ったファイル (blob:) を保存できないためです。ログイン画面は、ダウンロードの
 * アドレスに、もう一度アクセスして保存するので、基板が CSV を返せば、保存できます。
 *
 * 記録を始めるときに「Wi-Fi をオフ」を選ぶと、基板は再起動して通常モード (Wi-Fi オフ、
 * ライトスリープあり) になり、記録を続けます。止めるときは、BOOT ボタンの長押しで設定モードに
 * 入り、表示ページの「停止」を押します。記録中は、LED5 が赤く点滅します。
 *
 * 時刻: 記録を始めるときに、スマホの時計を、基板の時計 (システム時刻) に設定します。
 * この時計は、ソフトウェアの再起動 (設定モードとの切り替え) では止まりません。
 * 電源が切れると失われるので、そこで記録は終わります (それまでの記録は残る)。
 * 停止するときに、もう一度スマホの時計を受け取り、その間の時計のずれを、直線で補正します。
 *
 * 記録の形式 (サンプルの差分を詰めて、約 3.3 バイト/サンプル)
 *   先頭の 4KB: ヘッダ (LogHeader) と、停止の情報 (StopInfo、64 バイト目から)
 *   4KB 以降: フレームを順に並べる
 *     サンプルのフレーム (13 + 3 x n バイト。約 1 秒分)
 *       0  tag     0xA5
 *       1  session 記録ごとの番号 (古い記録の残りと区別する)
 *       2  n       差分のサンプル数 (0〜29)
 *       3  shift   HPF のシフト量 (そのときの表示モードの感度)
 *       4  rel     記録を始めてからの時間 [ms] (uint32)
 *       8  raw     気圧 (1/64 Pa、24 ビット)
 *      11  tempC   温度 [0.01 ℃] (int16)
 *      13  差分 x n: dt (前のサンプルからの時間 [ms]、uint8)、draw (気圧の差、int16)
 *     再起動のフレーム (6 バイト): 0xB0, session, rel (uint32)。HPF は、ここで初めからになる
 *   書き込みは、フレームの本体を書いてから、最後に tag を書く (途中で電源が切れても、
 *   tag のないフレームは読まない)。
 *   変化量 (HPF の出力) は記録せず、CSV を作るときに、気圧から同じ計算で求め直します。
 */
#pragma once

#include <stdint.h>

class AsyncWebServerRequest;
class HighPassFilter;

// 起動時に呼ぶ (通常モード、設定モードとも)。hpf は、気圧のハイパスフィルタ。
// 記録の途中で再起動したときは、続きから記録する
void devlogBegin(HighPassFilter *hpf);
void devlogService(uint32_t nowMs);       // ループから毎回呼ぶ (ページからの操作を行う)

// サンプルを渡す (毎ティック)。shift は、そのサンプルの HPF のシフト量。記録中でなければ何もしない
void devlogPush(int32_t raw, int32_t shift, float tempC);

// ページからの操作 (Web サーバのタスクから呼ぶ。実際の処理は devlogService で行う)
//   epochMs: スマホの時計 (UNIX 時刻 [ms])、tzMin: スマホのタイムゾーン (Date.getTimezoneOffset())
//   wifiOff: 記録を始めたあと、再起動して Wi-Fi をオフにする
void devlogRequestStart(int64_t epochMs, int32_t tzMin, bool wifiOff);
void devlogRequestStop(int64_t epochMs);
void devlogRequestClear();

bool     devlogAvailable();               // 記録の領域 (パーティション spiffs) がある
bool     devlogRecording();
bool     devlogFull();                    // いっぱいで止まった
uint32_t devlogCount();                   // 記録したサンプル数
uint32_t devlogCapacity();                // 記録できるサンプル数の見込み (記録した数を含む)

// /log.csv の応答 (CSV を少しずつ作って返す)。行の長さをそろえるので、大きさが先に決まる
// (1 行 83 バイト。5.5 時間分で約 49MB)
void devlogHandleCsv(AsyncWebServerRequest *req);

// CSV のダウンロードの進み具合 (表示ページに出す)
bool devlogDownloading();                 // 送っている途中 (設定モードを自動で終了しない)
void devlogDownloadProgress(uint32_t &sent, uint32_t &total);
