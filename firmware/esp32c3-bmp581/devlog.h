/*
 * devlog.h - 基板の中への記録 (設定モードの表示ページから操作する)
 *
 * 気圧のサンプル (センサが 30Hz で測った値) を、フラッシュの空き領域 (パーティション "spiffs"。Partition Scheme が
 * No OTA なら約 1.9MB) に記録します。いっぱいになると、自動で止まります。
 *
 * 記録の間隔は、記録を始めるときに選びます。
 *   1/30 秒 (毎サンプル) : 約 16 時間 (気圧の変化が大きいほど短い)
 *   1 秒                 : 約 2.8 日 (その間の約 30 サンプルの平均)
 *   30 秒                : 約 84 日 (その間の約 900 サンプルの平均)
 * 平均するのは、気圧、温度、変化量 (HPF の出力) です。LED の表示は、どの間隔でも 30Hz のままです。
 *
 * 保存の方法は 2 つあります。
 *   /log.bin : 記録をそのまま送り、表示ページが CSV を作る (速い。1/30 秒の 5 時間分で約 0.6MB)。
 *              Chrome や Safari などのブラウザで使う
 *   /log.csv : 基板が CSV を作って送る (遅い。1 行 88 バイト)。Wi-Fi のログイン画面 (自動で開く
 *              画面) で使う。ログイン画面は、ブラウザの中で作ったファイル (blob:) を保存できないが、
 *              ダウンロードのアドレスに、もう一度アクセスして保存するので、基板が返せば保存できる
 * どちらも、同じ内容の CSV になります。
 *
 * 記録を始めるときに「Wi-Fi をオフ」を選ぶと、基板は再起動して通常モード (Wi-Fi オフ、
 * ライトスリープあり) になり、記録を続けます。止めるときは、BOOT ボタンの長押しで設定モードに
 * 入り、表示ページの「停止」を押します。記録中は、LED5 が赤く点滅します。
 *
 * 時刻: 記録を始めるときに、スマホの時計を、基板の時計 (システム時刻) に設定します。
 * この時計は、ソフトウェアの再起動 (設定モードとの切り替え) では止まりません。
 * 電源が切れると失われるので、そこで記録は終わります (それまでの記録は残る)。
 * 停止するときに、もう一度スマホの時計を受け取り、その間の時計のずれを、直線で補正します。
 * 毎サンプルの記録では、センサが測った時刻を、読んだ時刻から求めます (devlog.cpp の fifoTiming)。
 * センサは 30Hz で測りますが、その間隔は内蔵の発振器で決まり、少し速い (実測 約 33.305ms) ので、
 * フレームごとに、間隔も求めて記録します。
 *
 * 記録の形式 (版 6。サンプルの差分を詰めて、約 1.1 バイト/サンプル)
 *   先頭の 4KB: ヘッダ (LogHeader) と、停止の情報 (StopInfo、64 バイト目から)
 *     ヘッダには、記録を始めたときの表示モード、役割、Wi-Fi をオフにしたか、装置の名前、
 *     記録の間隔を書く (役割などは版 3 から、記録の間隔は版 4 から。版 2〜5 の記録も読める)
 *   4KB 以降: フレームを順に並べる
 *     サンプルのフレーム (17 + blen バイト。最大 240 サンプル、約 8 秒分。版 6 から)
 *       0  tag     0xA9
 *       1  session 記録ごとの番号 (古い記録の残りと区別する)
 *       2  n       先頭のあとのサンプル数 (0〜239)
 *       3  shift   HPF のシフト量 (そのときの表示モードの感度)
 *       4  rel     先頭のサンプルを測った時刻 (記録を始めてからの時間 [ms]、uint32)
 *       8  raw     先頭のサンプルの気圧 (1/64 Pa、24 ビット)
 *      11  tempC   温度 [0.01 ℃] (int16)
 *      13  blen    符号のバイト数 (uint16)
 *      15  period  センサの測定の間隔 [µs] (uint16)。k 番目のサンプルの時刻は
 *                  rel + (k x period + 500) / 1000 [ms]
 *      17  符号 x n (d = 前のサンプルとの気圧の差)
 *            0xxxxxxx           : d = 7 ビット (-64〜63)       1 バイト
 *            10dddddd dddddddd  : d = 14 ビット (-8192〜8191)  2 バイト
 *            0xC0, d (int16)    : そのほか                     3 バイト
 *          サンプルの符号の前に、温度の符号 (0xC1, 温度の差 (int8)) が入ることがある。
 *          温度は、変わっていれば、約 1 秒 (30 サンプル) ごとに書き直す。
 *          サンプルが抜けたとき (基板が 1 秒以上止まって、センサの FIFO があふれたときなど) は、
 *          新しいフレームにする
 *     版 5 のサンプルのフレーム (15 + blen バイト。最大 120 サンプル、約 4 秒分。読むだけ)
 *       0  tag     0xA6
 *       1  session 記録ごとの番号 (古い記録の残りと区別する)
 *       2  n       先頭のあとのサンプル数 (0〜119)
 *       3  shift   HPF のシフト量 (そのときの表示モードの感度)
 *       4  rel     先頭のサンプルの、記録を始めてからの時間 [ms] (uint32)
 *       8  raw     先頭のサンプルの気圧 (1/64 Pa、24 ビット)
 *      11  tempC   温度 [0.01 ℃] (int16)
 *      13  blen    符号のバイト数 (uint16)
 *      15  符号 x n (dt = 前のサンプルからの時間 [ms]、d = 気圧の差)
 *            0xxxxxxx                    : dt = 33、d = 7 ビット (-64〜63)          1 バイト
 *            10ttt ddd dddddddd          : dt = 32 + ttt、d = 11 ビット (-1024〜1023) 2 バイト
 *            0xC0, dt (uint8), d (int16) : そのほか                                  4 バイト
 *          サンプルの符号の前に、温度の符号 (0xC1, 温度の差 (int8)) が入ることがある。
 *          温度は、変わっていれば、約 1 秒 (30 サンプル) ごとに書き直す
 *     再起動のフレーム (6 バイト): 0xB0, session, rel (uint32)。HPF は、ここで初めからになる
 *     電池電圧のフレーム (4 バイト): 0xBA, session, mV (uint16)。後に続くサンプルの値。
 *       電池電圧は 1 分ごとに測るので、値が変わったときだけ書く (版 3 から)
 *     平均したサンプルのフレーム (14 バイト。記録の間隔が 1 秒以上のとき。版 4 から)
 *       0  tag     0xA7
 *       1  session
 *       2  rel     平均したサンプルの時刻の平均 [ms] (uint32)
 *       6  raw     気圧の平均 (1/64 Pa、24 ビット)
 *       9  tempC   温度の平均 [0.01 ℃] (int16)
 *      11  hpf     変化量 (基板の HPF の出力) の平均 (1/64 Pa、符号付き 24 ビット)
 *       間隔の区切りは、記録を始めてからの時間で、間隔の倍数にする
 *     平均したサンプルの、前の平均との差のフレーム (8 バイト。版 5 から)
 *       0  tag     0xA8
 *       1  session
 *       2  drel    時刻の、前 + 記録の間隔からのずれ [ms] (int8)
 *       3  draw    気圧の差 (int16)
 *       5  dtemp   温度の差 [0.01 ℃] (int8)
 *       6  hpf     変化量 (int16)
 *       差が入りきらないときと、再起動のあとの最初は、0xA7 で書く
 *     版 4 までのサンプルのフレーム (13 + 3 x n バイト。約 1 秒分。読むだけ)
 *       0xA5, session, n (0〜29), shift, rel (uint32), raw (24 ビット), tempC (int16)、
 *       差分 x n: dt (uint8)、draw (int16)
 *   書き込みは、フレームの本体を書いてから、最後に tag を書く (途中で電源が切れても、
 *   tag のないフレームは読まない)。サンプルのフレームは、閉じたときに書くので、電源が切れると、
 *   最後の数秒は残らない (再起動するときは、先に書く。devlogFlush)。
 *   毎サンプルの記録では、変化量 (HPF の出力) は記録せず、CSV を作るときに、気圧から同じ計算で
 *   求め直します。
 */
#pragma once

#include <stdint.h>

#include "settings.h"          // NODE_NAME_MAX

class AsyncWebServerRequest;
class HighPassFilter;

// 起動時に呼ぶ (通常モード、設定モードとも)。hpf は、気圧のハイパスフィルタ。
// 記録の途中で再起動したときは、続きから記録する
void devlogBegin(HighPassFilter *hpf);
void devlogService(uint32_t nowMs);       // ループから毎回呼ぶ (ページからの操作を行う)

// サンプルを渡す (センサが測った値ごと。古い順)。shift は、そのサンプルの HPF のシフト量。batteryMv は、
// 最後に測った電池電圧 (変わったときだけ記録する)。hpf は、HPF の出力 (記録の間隔が 1 秒以上のときに
// 平均する)。age は、同じときに読んだ値のうち、この値よりあとに測った値の数 (測った時刻を求める)。
// 記録中でなければ何もしない
void devlogPush(int32_t raw, int32_t shift, float tempC, uint32_t batteryMv, int32_t hpf, uint8_t age);

// サンプルが抜けたかもしれない (センサの FIFO があふれた、読めなかった)。次のサンプルから新しい
// フレームにして、時刻を合わせ直す
void devlogMarkGap();

// 作りかけのフレームを書く (再起動の前に呼ぶ。記録は、再起動のあとも続く)
void devlogFlush();

// ページからの操作 (Web サーバのタスクから呼ぶ。実際の処理は devlogService で行う)
//   epochMs: スマホの時計 (UNIX 時刻 [ms])、tzMin: スマホのタイムゾーン (Date.getTimezoneOffset())
//   wifiOff: 記録を始めたあと、再起動して Wi-Fi をオフにする
//   intervalMs: 記録の間隔。0 = 毎サンプル (30Hz)、1000 = 1 秒、30000 = 30 秒 (その間の平均)
void devlogRequestStart(int64_t epochMs, int32_t tzMin, bool wifiOff, uint32_t intervalMs);
void devlogRequestStop(int64_t epochMs);
void devlogRequestClear();

bool     devlogAvailable();               // 記録の領域 (パーティション spiffs) がある
bool     devlogRecording();
bool     devlogFull();                    // いっぱいで止まった
uint32_t devlogCount();                   // 記録したサンプル数
uint32_t devlogCapacity();                // 記録できるサンプル数の見込み (記録した数を含む)
uint32_t devlogIntervalMs();              // 今の記録の間隔 [ms]。0 = 毎サンプル (30Hz)

// 記録の状態 (シリアルの status コマンドで出す)
struct DevlogInfo {
    bool     available;         // 記録の領域がある
    uint32_t partitionBytes;    // 記録の領域の大きさ
    uint32_t usedBytes;         // 使った大きさ (ヘッダのセクタを含む)
    bool     hasLog;            // 記録がある
    uint8_t  version;           // 記録の形式の版
    int64_t  startEpochMs;      // 記録を始めた時刻 (スマホの時計、UNIX 時刻 [ms])
    int16_t  tzMin;             // 記録を始めたときのスマホのタイムゾーン (Date.getTimezoneOffset())
    uint32_t intervalMs;        // 記録の間隔。0 = 毎サンプル
    const char *endReason;      // "recording"、"stopped"、"full"、"power_loss"、"none" (記録なし)
    uint32_t samples;           // 記録したサンプル数
    uint32_t capacity;          // 記録できるサンプル数の見込み (記録した数を含む)
    char     name[NODE_NAME_MAX + 1];   // 記録したときの装置の名前 (版 3 から)
    uint8_t  role, mode;        // 記録したときの役割、表示モード (版 3 から)
    bool     wifiOff;           // Wi-Fi をオフにして記録した
    float    sensorPeriodUs;    // センサの測定の間隔の見込み [µs]
    bool     sensorPeriodMeasured;   // 実測した値か (false = 30Hz の値のまま)
};
void devlogGetInfo(DevlogInfo &info);

// /log.csv の応答 (CSV を少しずつ作って返す)。行の長さをそろえるので、大きさが先に決まる
// (1 行目は # で始まるコメント (装置の名前、役割など)、2 行目は列の名前、3 行目からデータ。
//  データは 1 行 88 バイト。毎サンプルの記録の 5 時間分で約 48MB)
void devlogHandleCsv(AsyncWebServerRequest *req);

// /log.bin の応答 (記録をそのまま送る。表示ページが、/log.csv と同じ CSV を作る)
void devlogHandleBin(AsyncWebServerRequest *req);

// /log.bin と同じものを、writeAll で書き出す (シリアルの dump コマンド)。書き出す前に、全体の大きさで
// begin を呼ぶ。bytes に書いたバイト数、crc に CRC-32 (zlib.crc32 と同じ) が入る。記録がない、または
// writeAll が false を返したら false
bool devlogDump(void (*begin)(uint32_t total), bool (*writeAll)(const uint8_t *p, size_t n),
                uint32_t &bytes, uint32_t &crc);

// CSV のダウンロードの進み具合 (表示ページに出す)
bool devlogDownloading();                 // 送っている途中 (設定モードを自動で終了しない)
void devlogDownloadProgress(uint32_t &sent, uint32_t &total);
