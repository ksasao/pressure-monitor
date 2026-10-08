#include "display.h"

#include <Arduino.h>

#include "config.h"
#include "settings.h"

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
static bool     s_lastSentValid = false;
static uint32_t s_lastSentMs    = 0;

// ライトスリープから復帰してから、まだ LED へ送っていないか。
// 復帰した直後の、最初の送信は化けることがあるので、その 1 回だけ 2 回送る
static bool s_afterWake = false;

static void applyStatus()
{
    if (s_statusEnabled) leds[NUM_LEDS - 1] = s_statusColor;
}

// FastLED.show() を、そのまま呼ぶ。s_lastSent と食い違うので、記録を無効にする
static void showRaw()
{
    FastLED.show();
    s_lastSentValid = false;
}

static bool isBlack(const CRGB &c)
{
    return c.r == 0 && c.g == 0 && c.b == 0;
}

// 今の表示 (leds) が、最後に送った内容と同じか。
// memcmp / memcpy は、FastLED の同名の関数と曖昧になるので、使わない
static bool sameAsLastSent()
{
    if (!s_lastSentValid) return false;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        if (leds[i].r != s_lastSent[i].r ||
            leds[i].g != s_lastSent[i].g ||
            leds[i].b != s_lastSent[i].b) return false;
    }
    return true;
}

static void rememberSent()
{
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
static void showFrame(bool force = false)
{
    uint32_t now = millis();

    bool refresh = (LED_REFRESH_MS > 0) && ((now - s_lastSentMs) >= LED_REFRESH_MS);
    if (!force && LED_SKIP_UNCHANGED && sameAsLastSent() && !refresh) return;

    FastLED.show();
    if (s_afterWake) {
        FastLED.wait(5);
        delay(1);
        FastLED.show();
    }

    s_afterWake = false;
    rememberSent();
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

// 30Hz で呼ばれる。LED1 はライブ値、LED2..5 は履歴
static void renderTrail()
{
    leds[0] = s_head;
    for (uint8_t i = 1; i < NUM_LEDS; i++) leds[i] = s_trail[i];
    applyStatus();
    showFrame();
}

// 気圧の絶対値を 5 段階のバーで表示 (低圧=青 -> 高圧=赤)
// FastLED の hue は 0=赤 / 96=緑 / 160=青 の 8bit
static void renderBar(float hPa)
{
    float t = (hPa - BAR_MIN_HPA) / (BAR_MAX_HPA - BAR_MIN_HPA);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float lit = t * NUM_LEDS;   // 何個分点灯するか (小数)

    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        float f = lit - (float)i;           // この LED の点灯率 0..1
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;

        uint8_t hue = 160 - (uint8_t)(i * (160 / (NUM_LEDS - 1)));  // 160 -> 0
        leds[i] = CHSV(hue, 255, (uint8_t)(f * BAR_MAX_VALUE));
    }
    applyStatus();
    showFrame();
}

// 診断用 (モード 3): 全 LED を白で点灯する。内容が変わらなくても、毎ティック送る。
// 明るさは、設定の brightness (最大値) で抑えられる
static void renderAllOn()
{
    fill_solid(leds, NUM_LEDS, CRGB::White);
    applyStatus();
    showFrame(true);
}

void displayBegin()
{
    for (uint8_t i = 0; i < NUM_LEDS; i++) s_trail[i] = CRGB::Black;

    FastLED.addLeds<WS2812B, PIN_NEO, GRB>(leds, NUM_LEDS);
    displayApplyBrightness();
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    showRaw();
}

// 全ての色に掛かる明るさの最大値。255 なら、パレットの値のまま
void displayApplyBrightness()
{
    FastLED.setBrightness((uint8_t)g_set.brightness);
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

void displayRender(uint8_t mode, float hPa)
{
    switch (mode) {
        case 0: renderTrail();   break;
        case 1: renderBar(hPa);  break;
        case 3: renderAllOn();   break;     // 診断用の全点灯
        default:
            // 2 = 消灯。ただし状態表示があるときは、そのために更新する
            if (s_statusEnabled) {
                fill_solid(leds, NUM_LEDS, CRGB::Black);
                applyStatus();
                showFrame();
            }
            break;
    }
}

void displayModeChanged(uint8_t mode)
{
    if (mode == 2) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        applyStatus();
        showFrame();
    }
}

void displaySetStatus(bool enabled, const CRGB &c)
{
    s_statusEnabled = enabled;
    s_statusColor   = c;
}
