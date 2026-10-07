#include "high_pass_filter.h"

// 内部計算の精度を上げるための追加シフト
static const int32_t INTERNAL_PRECISION_SHIFT = 8;

// 0 方向ではなく「ゼロから遠ざかる方向」に丸める (round half away from zero)
static inline int64_t roundShiftAway(int64_t v, int32_t shift)
{
    int64_t a = (v < 0) ? -v : v;
    int64_t r = (a + (1LL << (shift - 1))) >> shift;
    return (v < 0) ? -r : r;
}

int32_t HighPassFilter::update(int32_t x, int32_t shift)
{
    if (shift < 1)  shift = 1;      // シフト量の範囲外を防ぐ
    if (shift > 30) shift = 30;

    int64_t cx = (int64_t)x << INTERNAL_PRECISION_SHIFT;

    if (!init_) {
        prevX_ = cx;
        y_     = 0;
        init_  = true;
    }

    int64_t diff  = cx - prevX_;
    int64_t decay = roundShiftAway(y_, shift);

    y_     = y_ + diff - decay;
    prevX_ = cx;

    return (int32_t)roundShiftAway(y_, INTERNAL_PRECISION_SHIFT);
}

void HighPassFilter::reset()
{
    prevX_ = 0;
    y_     = 0;
    init_  = false;
}
