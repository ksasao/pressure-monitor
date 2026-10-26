/*
 * settings.h - 設定値 (設定モードで変更・保存できる値)
 *
 * 値はフラッシュ (NVS) に保存され、電源を切っても残ります。
 */
#pragma once

#include <stdint.h>
#include <Arduino.h>

// 感度に関する値の単位は 1/64 Pa。1 Pa ≒ 8.5 cm の高さ変化です。
// (標準モードとドライブモードの値は、modes.cpp にあります)
//
//   deltaLimit     [カスタム] この変化量未満は消灯。50 ≒ 0.78 Pa ≒ 6.6 cm
//   deltaRange     [カスタム] この変化量で白に達する。753 ≒ 11.8 Pa ≒ 100 cm (細かい動き向け)
//                  広い範囲 (エレベータ等) を見るなら 15000 (≒ 20 m)
//   hpfShift       [カスタム] HPF の時定数。約 2^hpfShift サンプル。30Hz で 8 なら 8.5 秒。
//                  大きいほどゆっくりした変化まで拾い、戻りが遅くなる
//   trailStepTicks [カスタム] 色を後ろへ送る間隔 [ティック]。1 = 毎フレーム。
//                  大きくすると LED2..5 に長い時間の履歴が流れる (LED1 は常にライブ値)
//   brightness     [標準・ドライブ・カスタム共通] LED の明るさの最大値 (0〜255)。初期値は 192。
//                  255 = 抑えない。固定色モードには掛からない (固定色は hue/sat/val で決める)
//   mode           今の表示モード (modes.h)。BOOT ボタンで切り替えると保存される
//   hue, sat, val  [固定色] 色相 (0〜359 度)、彩度 (0〜100 %)、明度 (0〜100 %)
//   valMod         [固定色] 気圧の変化を明度に反映する量 (0〜100 %)。0 = 反映しない。
//                  フィルタの出力 (標準モードの感度で -1〜+1) を掛けて、明度に足す。
//                  気圧が上がると (+) 明るく、下がると (-) 暗くなる。
//                  結果は 0〜100 % に収める。履歴は、標準モードと同じく 5 個の LED に流れる
//   role           複数台の表示での役割 (link.h の LinkRole)。0 = 単体, 1 = 送信, 2 = ハブ。
//   group          [送信・ハブ] グループ番号 (1〜255)。同じ番号どうしだけが、やり取りする
//   hubChannel     [ハブ] 自分でアクセスポイントになるときのチャンネル (1〜13)
//                  再起動すると有効になる
//   nodeName       装置の名前 (UTF-8 で NODE_NAME_MAX バイトまで)。ハブの画面や記録で、装置を
//                  見分けるのに使う。空なら "ps-" + MAC アドレスの下 6 桁 (settingsNodeName())
//   sound          Modulino Buzzer (Qwiic) の音色。0 = 鳴らさない、1 = キラキラ、2 = さわやか (sound.h)。
//                  初期値はキラキラ。Modulino Buzzer をつないでいなければ、何もしない
//   soundPitch     音の高さ。0 = 高い (+1 オクターブ)、1 = 標準、2 = 低い (-1 オクターブ)、
//                  3 = とても低い (-2 オクターブ)。圧電ブザーは約 4kHz で最も大きく、低い音ほど
//                  小さく鳴るので、音の高さを変えると、音量も変わる (高いほど大きい)
//   wifiSsid, wifiPass  [ハブ] 接続する Wi-Fi (ルーター)。SSID が空なら、ハブが自分で
//                  アクセスポイントになる (hub.h)
//
// 感度は、ログの hpf 列を見て調整してください。
// 静止時の hpf の振れ幅より deltaLimit が小さいと、暗い色がちらつきます
static const size_t NODE_NAME_MAX = 16;

// 音色 (sound)
enum SoundKind : int32_t {
    SOUND_OFF     = 0,
    SOUND_SPARKLE = 1,      // キラキラ
    SOUND_FRESH   = 2,      // さわやか
    SOUND_COUNT
};

// 音の高さ (soundPitch)
enum SoundPitch : int32_t {
    SOUND_PITCH_HIGH   = 0,     // 高い (+1 オクターブ。最も大きい)
    SOUND_PITCH_NORMAL = 1,     // 標準
    SOUND_PITCH_LOW    = 2,     // 低い (-1 オクターブ)
    SOUND_PITCH_LOWER  = 3,     // とても低い (-2 オクターブ。最も小さい)
    SOUND_PITCH_COUNT
};

struct AppSettings {
    int32_t deltaLimit;
    int32_t deltaRange;
    int32_t hpfShift;
    int32_t trailStepTicks;
    int32_t brightness;
    int32_t mode;
    int32_t hue;
    int32_t sat;
    int32_t val;
    int32_t valMod;
    int32_t role;
    int32_t group;
    int32_t hubChannel;
    int32_t sound;
    int32_t soundPitch;
    char    nodeName[NODE_NAME_MAX + 1];
    char    wifiSsid[33];
    char    wifiPass[65];
};

extern AppSettings        g_set;              // 現在の設定
extern const AppSettings  SETTINGS_DEFAULT;   // 既定値

void settingsLoad();      // 保存された設定を読む (なければ既定値)
void settingsSave();      // 現在の設定を保存する (mode を除く)
void settingsSaveMode();  // 今の表示モードだけを保存する
void settingsSaveRole();  // 今の役割だけを保存する
void settingsReset();     // 既定値に戻して保存する (表示モード、役割、グループ番号、チャンネル、装置の名前、Wi-Fi は変えない)
void settingsClamp();     // 範囲外の値を直す

// 保存されている値を全て消して、初期値に戻す (表示モード、役割、装置の名前、Wi-Fi、
// ハブの前回の IP アドレス、ブラウンアウトの累計も含む)
void settingsFactoryReset();

// 装置の名前。設定がなければ、"ps-" + MAC アドレスの下 6 桁 (例: ps-1a2b3c)
String settingsNodeName();
String settingsDefaultNodeName();

// UTF-8 の文字列を、maxBytes バイト以内に切り詰めてコピーする (文字の途中では切らない。
// 制御文字は除く)。dst は maxBytes + 1 バイト以上
void settingsCopyName(char *dst, const char *src, size_t maxBytes);

// ハブが、最後に Wi-Fi (ルーター) に接続したときの IP アドレス。設定ページに表示する
String settingsHubLastIp();
void   settingsSaveHubLastIp(const String &ip);

// ブラウンアウト (電源電圧の落ち込みによるリセット) の累計。保存される
uint32_t settingsBrownoutCount();
void     settingsAddBrownout();
