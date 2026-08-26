#include "CrazyEyes.hpp"

// -DCRAZY_EYES_MEASURE_DRAW=1 を指定すると，1フレームの描画時間と描画間隔を
// シリアルへ出力します。描画性能を確認するときに使用します。
#ifndef CRAZY_EYES_MEASURE_DRAW
#define CRAZY_EYES_MEASURE_DRAW 0
#endif

#if CRAZY_EYES_MEASURE_DRAW
#include <inttypes.h>
#endif

static constexpr const char* TAG = "CrazyEyes";

extern const uint8_t OPEN_EYE_START[] asm(
    "_binary_data_crazy_eyes_open_jpg_start");
extern const uint8_t OPEN_EYE_END[] asm("_binary_data_crazy_eyes_open_jpg_end");
static const size_t OPEN_EYE_SIZE = (OPEN_EYE_END - OPEN_EYE_START);

extern const uint8_t CLOSE_EYE_START[] asm(
    "_binary_data_crazy_eyes_close_jpg_start");
extern const uint8_t CLOSE_EYE_END[] asm(
    "_binary_data_crazy_eyes_close_jpg_end");
static const size_t CLOSE_EYE_SIZE = (CLOSE_EYE_END - CLOSE_EYE_START);

CrazyEyes::CrazyEyes(void) : _is_opened(true), _is_cached(false) {
}

bool CrazyEyes::begin(const int bgColor) {
    M5.begin();

    // JPEGのデコードは1フレームあたり150ms以上かかるため，起動時に一度だけ
    // デコードしてPSRAM上のキャンバスへ保持します。
    this->_is_cached =
        cacheEye(this->_opened_eye, OPEN_EYE_START, OPEN_EYE_SIZE) &&
        cacheEye(this->_closed_eye, CLOSE_EYE_START, CLOSE_EYE_SIZE);
    if (!this->_is_cached) {
        this->_opened_eye.deleteSprite();
        this->_closed_eye.deleteSprite();
        ESP_LOGW(TAG, "Failed to cache the eye images. Decoding on every draw.");
    }
    return true;
}

bool CrazyEyes::cacheEye(M5Canvas& canvas, const uint8_t* jpeg,
                         const size_t size) {
    canvas.setPsram(true);
    canvas.setColorDepth(M5.Display.getColorDepth());
    if (canvas.createSprite(M5.Display.width(), M5.Display.height()) ==
        nullptr) {
        ESP_LOGW(TAG, "Failed to allocate a canvas (%dx%d)", M5.Display.width(),
                 M5.Display.height());
        return false;
    }
    return canvas.drawJpg(jpeg, size, 0, 0, canvas.width(), canvas.height(), 0,
                          0, 0.0F, 0.0F, middle_center);
}

bool CrazyEyes::update(void) {
    M5.update();
    return true;
}

void CrazyEyes::drawEye(void) {
    if (this->_is_cached) {
        M5Canvas& canvas =
            this->_is_opened ? this->_opened_eye : this->_closed_eye;
        canvas.pushSprite(&M5.Lcd, 0, 0);
        return;
    }

    if (this->_is_opened) {
        M5.Lcd.drawJpg(OPEN_EYE_START, OPEN_EYE_SIZE, 0, 0, M5.Display.width(),
                       M5.Display.height(), 0, 0, 0.0F, 0.0F, middle_center);
    } else {
        M5.Lcd.drawJpg(CLOSE_EYE_START, CLOSE_EYE_SIZE, 0, 0,
                       M5.Display.width(), M5.Display.height(), 0, 0, 0.0F,
                       0.0F, middle_center);
    }
}

void CrazyEyes::show(void) {
#if CRAZY_EYES_MEASURE_DRAW
    static uint32_t prev_started_at = 0;

    const uint32_t started_at = micros();
#endif
    M5.Lcd.startWrite();
    drawEye();
    M5.Lcd.endWrite();
#if CRAZY_EYES_MEASURE_DRAW
    const uint32_t finished_at = micros();
    ESP_LOGI(TAG, "%s: draw=%" PRIu32 "us interval=%" PRIu32 "us",
             this->_is_opened ? "open" : "close", finished_at - started_at,
             prev_started_at == 0 ? 0 : started_at - prev_started_at);
    prev_started_at = started_at;
#endif
}

bool CrazyEyes::setOpened(const bool opened) {
    if (this->_is_opened == opened) {
        return false;
    }

    this->_is_opened = opened;
    show();
    return true;
}

bool CrazyEyes::blink(void) {
    const bool prev = this->_is_opened;
    setOpened(!prev);
    return prev;
}
