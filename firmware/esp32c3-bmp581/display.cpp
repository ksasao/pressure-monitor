#include "display.h"

#include <Arduino.h>

#include "config.h"
#include "modes.h"

static CRGB leds[NUM_LEDS];

// LED1 が今まさに表示している色 (ライブ値)
static CRGB s_head = CRGB::Black;

// LED2..LED5 用の履歴。LED i は i フレーム前の LED1 の色
static CRGB s_trail[NUM_LEDS];

// 状態表示 (最後の LED の上書き)
static bool s_statusEnabled = false;
static CRGB s_statusColor   = CRGB::Black;

// 最後に LED へ送った内容。内容が変わらないときは、送らない (config.h の LED_SKIP_UNCHANGED)
static CRGB     s_lastSent[NUM_LEDS];
static uint8_t  s_lastSentBrightness = 255;
static bool     s_lastSentValid = false;
static uint32_t s_lastSentMs    = 0;

// ライトスリープから復帰してから、まだ LED へ送っていないか。
// 復帰した直後の、最初の送信は化けることがあるので、その 1 回だけ 2 回送る
static bool s_afterWake = false;

// モード番号の表示 (BOOT ボタンを押したとき、番号の数だけ LED を青で点灯する)
static uint8_t  s_overlayCount   = 0;
static uint32_t s_overlayUntilMs = 0;
static bool     s_overlayOn      = false;

static void applyStatus()
{
    if (s_statusEnabled) leds[NUM_LEDS - 1] = s_statusColor;
}

// FastLED.show() を、そのまま呼ぶ。s_lastSent と食い違うので、記録を無効にする
static void showRaw()
{
    FastLED.setBrightness(255);     // 合図や自己診断は、明るさの設定に関わらず見えるように
    FastLED.show();
    s_lastSentValid = false;
}

static bool isBlack(const CRGB &c)
{
    return c.r == 0 && c.g == 0 && c.b == 0;
}

// 今の表示 (leds) が、最後に送った内容と同じか。
// memcmp / memcpy は、FastLED の同名の関数と曖昧になるので、使わない
static bool sameAsLastSent(uint8_t brightness)
{
    if (!s_lastSentValid || brightness != s_lastSentBrightness) return false;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        if (leds[i].r != s_lastSent[i].r ||
            leds[i].g != s_lastSent[i].g ||
            leds[i].b != s_lastSent[i].b) return false;
    }
    return true;
}

static void rememberSent(uint8_t brightness)
{
    s_lastSentBrightness = brightness;
    for (uint8_t i = 0; i < NUM_LEDS; i++) s_lastSent[i] = leds[i];
    s_lastSentValid = true;
}

// 表示の更新。
//   - 内容が前回と同じなら、送らない (LED は状態を保つ)。送信がなければ、
//     ライトスリープ復帰直後の送信の乱れも、起きようがない。
//     ただし、LED_REFRESH_MS ごとに 1 回は、送り直す (乱れの自動復帰のため)
//   - ライトスリープから復帰した直後の最初の送信だけ、念のため、同じフレームを
//     2 回送る。CPU を 80MHz にしているので、化けることはないはずだが、
//     (40MHz では化けた。config.h の CPU_MHZ を参照)、点灯し始めのときだけで、
//     電力への影響はほぼない
//   - force = true のときは、内容が同じでも、必ず送る (診断用の全点灯モード)
static void showFrame(uint8_t brightness, bool force = false)
{
    uint32_t now = millis();

    bool refresh = (LED_REFRESH_MS > 0) && ((now - s_lastSentMs) >= LED_REFRESH_MS);
    if (!force && LED_SKIP_UNCHANGED && sameAsLastSent(brightness) && !refresh) return;

    FastLED.setBrightness(brightness);
    FastLED.show();
    if (s_afterWake) {
        FastLED.wait(5);
        delay(1);
        FastLED.show();
    }

    s_afterWake = false;
    rememberSent(brightness);
    s_lastSentMs = now;
}

static void ledSweep(const CHSV &color, uint16_t stepMs)
{
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        leds[i] = color;
        showRaw();
        delay(stepMs);
    }
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    showRaw();
}

// モード番号の表示中か
static bool overlayActive()
{
    if (!s_overlayOn) return false;
    if ((int32_t)(millis() - s_overlayUntilMs) >= 0) { s_overlayOn = false; return false; }
    return true;
}

// 番号の数だけ、先頭から青で点灯する。明るさは、設定に関わらず一定
static void renderOverlay()
{
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        leds[i] = (i < s_overlayCount) ? CRGB(0, 0, 160) : CRGB::Black;
    }
    applyStatus();
    showFrame(255);
}

void displayBegin(uint8_t brightness)
{
    for (uint8_t i = 0; i < NUM_LEDS; i++) s_trail[i] = CRGB::Black;

    FastLED.addLeds<WS2812B, PIN_NEO, GRB>(leds, NUM_LEDS);

    // 最初の送信で、すぐに白にする (黒を挟まない)
    fill_solid(leds, NUM_LEDS, CRGB::White);
    FastLED.setBrightness(brightness);
    FastLED.show();
    s_lastSentValid = false;
}

// 起動時の自己診断: 全 LED を順に R -> G -> B
// N 個目で止まる場合は、その手前の LED かデータ配線が原因です
void displaySelfTest()
{
    ledSweep(CHSV(0,   255, SELFTEST_VALUE), 60);   // red
    ledSweep(CHSV(96,  255, SELFTEST_VALUE), 60);   // green
    ledSweep(CHSV(160, 255, SELFTEST_VALUE), 60);   // blue
}

// エラー表示: 全点灯で赤く明滅
void displayError()
{
    static bool on = false;
    on = !on;
    fill_solid(leds, NUM_LEDS,
               on ? CHSV(0, 255, ERROR_VALUE) : CHSV(0, 0, 0));
    showRaw();
}

void displayFill(const CRGB &c)
{
    fill_solid(leds, NUM_LEDS, c);
    showRaw();
}

void displayWait()
{
    FastLED.wait(5);        // 送信の完了を待つ (最大 5ms)。通常は 0.5ms 以内
}

bool displayIsDark()
{
    if (!isBlack(s_head)) return false;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        if (!isBlack(s_trail[i])) return false;     // 履歴にまだ色が残っている
        if (!isBlack(leds[i]))    return false;     // 今、点灯している
    }
    return true;
}

void displayNoteWake()
{
    s_afterWake = true;
}

void displaySetHead(const CRGB &c)
{
    s_head = c;
}

// 履歴を 1 段後ろにずらして、先頭に今の色を入れる
void displayTrailShift()
{
    for (uint8_t i = NUM_LEDS - 1; i >= 1; i--) {
        s_trail[i] = s_trail[i - 1];
    }
    s_trail[0] = s_head;
}

// 30Hz で呼ばれる。LED1 はライブ値、LED2..5 は履歴。
// brightness は、全ての色に掛かる明るさの最大値 (255 なら、パレットの値のまま)
void displayRenderTrail(uint8_t brightness)
{
    if (overlayActive()) { renderOverlay(); return; }

    leds[0] = s_head;
    for (uint8_t i = 1; i < NUM_LEDS; i++) leds[i] = s_trail[i];
    applyStatus();
    showFrame(brightness);
}

// 全 LED を同じ色にする。明るさの設定は掛けない (色の明度で決める)
void displayRenderSolid(const CRGB &c)
{
    if (overlayActive()) { renderOverlay(); return; }

    fill_solid(leds, NUM_LEDS, c);
    applyStatus();
    showFrame(255);
}

void displayShowModeNumber(uint8_t number, uint32_t durationMs)
{
    s_overlayCount   = number > NUM_LEDS ? NUM_LEDS : number;
    s_overlayUntilMs = millis() + durationMs;
    s_overlayOn      = true;
}

bool displayModeNumberActive()
{
    return overlayActive();
}

void displayClearTrail()
{
    s_head = CRGB::Black;
    for (uint8_t i = 0; i < NUM_LEDS; i++) s_trail[i] = CRGB::Black;
}

void displaySetStatus(bool enabled, const CRGB &c)
{
    s_statusEnabled = enabled;
    s_statusColor   = c;
}
