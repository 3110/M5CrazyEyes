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

// 虹彩と瞳は画像に含めず，実行時に円で描きます。視線を動かしても白目からは
// み出さないことを，元のSVGの寸法から確認済みです。
static constexpr int32_t IRIS_RADIUS = 122;
static constexpr int32_t PUPIL_RADIUS = 79;
static constexpr int32_t GAZE_MAX_OFFSET = 40;
// 白目(255)と肌色(138)のちょうど中間あたりに置きます。
static constexpr uint8_t SCLERA_MIN_BLUE = 190;
// これより短い白の連なりは，デコード誤差による点として無視します。
static constexpr int32_t MIN_SCLERA_RUN = 8;
// これ以下の隙間は，デコード誤差による抜けとみなして連結します。
static constexpr int32_t MAX_SCLERA_GAP = 4;
static constexpr uint16_t IRIS_COLOR = m5gfx::color565(0x29, 0xC7, 0xFF);
static constexpr uint16_t PUPIL_COLOR = m5gfx::color565(0x0D, 0x0D, 0x0D);

// 電池残量は円形ディスプレイの外周に沿ったアークで表示します。
static constexpr int32_t GAUGE_OUTER_RADIUS = 228;
static constexpr int32_t GAUGE_INNER_RADIUS = 214;
static constexpr float GAUGE_START_DEGREE = -90.0F;  // 12時の位置から時計回り
static constexpr int32_t GAUGE_HIGH_LEVEL = 50;
static constexpr int32_t GAUGE_LOW_LEVEL = 20;
// 残量は電圧から求めるため負荷で数%揺れます。刻んで表示のちらつきを抑えます。
static constexpr int32_t BATTERY_LEVEL_STEP = 5;
static constexpr uint32_t BATTERY_POLL_MS = 10000;
static constexpr uint32_t BATTERY_PULSE_MS = 600;

static constexpr uint16_t GAUGE_TRACK_COLOR = m5gfx::color565(48, 48, 48);
static constexpr uint16_t GAUGE_HIGH_COLOR = m5gfx::color565(0, 208, 96);
static constexpr uint16_t GAUGE_MIDDLE_COLOR = m5gfx::color565(255, 176, 0);
static constexpr uint16_t GAUGE_LOW_COLOR = m5gfx::color565(240, 48, 48);
static constexpr uint16_t GAUGE_CHARGING_COLOR = m5gfx::color565(0, 176, 240);
static constexpr uint16_t GAUGE_PULSE_COLOR = m5gfx::color565(160, 240, 255);

#define DECLARE_EYE_JPEG(var, name)                                   \
    extern const uint8_t var##_START[] asm(                           \
        "_binary_data_crazy_eyes_" name "_jpg_start");                \
    extern const uint8_t var##_END[] asm(                             \
        "_binary_data_crazy_eyes_" name "_jpg_end")

DECLARE_EYE_JPEG(OPEN_EYE, "open");
DECLARE_EYE_JPEG(OPEN_EYE1, "open1");
DECLARE_EYE_JPEG(OPEN_EYE2, "open2");
DECLARE_EYE_JPEG(OPEN_EYE3, "open3");

// 開いた状態の段階ごとの画像です。数が増えるほどまぶたが下りています。
struct EyeImage {
    const uint8_t* start;
    const uint8_t* end;
};
static const EyeImage OPEN_EYE_IMAGES[CrazyEyes::OPEN_LEVEL_COUNT] = {
    {OPEN_EYE_START, OPEN_EYE_END},
    {OPEN_EYE1_START, OPEN_EYE1_END},
    {OPEN_EYE2_START, OPEN_EYE2_END},
    {OPEN_EYE3_START, OPEN_EYE3_END},
};

extern const uint8_t CLOSE_EYE_START[] asm(
    "_binary_data_crazy_eyes_close_jpg_start");
extern const uint8_t CLOSE_EYE_END[] asm(
    "_binary_data_crazy_eyes_close_jpg_end");
static const size_t CLOSE_EYE_SIZE = (CLOSE_EYE_END - CLOSE_EYE_START);

CrazyEyes::CrazyEyes(void)
    : _is_opened(true),
      _is_cached(false),
      _open_level(0),
      _shows_battery(false),
      _is_charging(false),
      _is_pulse_on(false),
      _battery_level(-1),
      _battery_polled_at(0),
      _pulsed_at(0),
      _gaze(0) {
}

bool CrazyEyes::begin(const int bgColor) {
    M5.begin();

    // JPEGのデコードは1フレームあたり150ms以上かかるため，起動時に一度だけ
    // デコードしてPSRAM上のキャンバスへ保持します。
    this->_is_cached =
        cacheEye(this->_closed_eye, CLOSE_EYE_START, CLOSE_EYE_SIZE);
    for (uint8_t i = 0; i < OPEN_LEVEL_COUNT && this->_is_cached; ++i) {
        const EyeImage& image = OPEN_EYE_IMAGES[i];
        this->_is_cached =
            cacheEye(this->_opened_eye[i], image.start, image.end - image.start);
        if (this->_is_cached) {
            scanSclera(this->_opened_eye[i], i);
        }
    }
    if (!this->_is_cached) {
        for (uint8_t i = 0; i < OPEN_LEVEL_COUNT; ++i) {
            this->_opened_eye[i].deleteSprite();
        }
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

    // 目が静止していても，残量の変化と充電中の点滅を反映します。
    if (this->_shows_battery && updateBattery(millis())) {
        M5.Lcd.startWrite();
        drawBatteryGauge();
        M5.Lcd.endWrite();
    }
    return true;
}

bool CrazyEyes::readBattery(const uint32_t now) {
    this->_battery_polled_at = now;

    const int32_t level = M5.Power.getBatteryLevel();
    const int32_t stepped =
        level < 0 ? -1 : (level / BATTERY_LEVEL_STEP) * BATTERY_LEVEL_STEP;
    const bool charging =
        M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
    if (stepped == this->_battery_level && charging == this->_is_charging) {
        return false;
    }

    this->_battery_level = stepped;
    this->_is_charging = charging;
    if (!charging) {
        this->_is_pulse_on = false;
    }
    return true;
}

bool CrazyEyes::updateBattery(const uint32_t now) {
    bool changed = false;

    if (now - this->_battery_polled_at >= BATTERY_POLL_MS) {
        changed = readBattery(now);
    }

    if (this->_is_charging && now - this->_pulsed_at >= BATTERY_PULSE_MS) {
        this->_pulsed_at = now;
        this->_is_pulse_on = !this->_is_pulse_on;
        changed = true;
    }
    return changed;
}

uint16_t CrazyEyes::batteryGaugeColor(void) const {
    if (this->_is_charging) {
        return this->_is_pulse_on ? GAUGE_PULSE_COLOR : GAUGE_CHARGING_COLOR;
    }
    if (this->_battery_level > GAUGE_HIGH_LEVEL) {
        return GAUGE_HIGH_COLOR;
    }
    if (this->_battery_level > GAUGE_LOW_LEVEL) {
        return GAUGE_MIDDLE_COLOR;
    }
    return GAUGE_LOW_COLOR;
}

void CrazyEyes::drawBatteryGauge(void) {
    const int32_t cx = M5.Display.width() / 2;
    const int32_t cy = M5.Display.height() / 2;

    M5.Lcd.fillArc(cx, cy, GAUGE_INNER_RADIUS, GAUGE_OUTER_RADIUS,
                   GAUGE_START_DEGREE, GAUGE_START_DEGREE + 360.0F,
                   GAUGE_TRACK_COLOR);
    if (this->_battery_level <= 0) {
        // 読み取りに失敗したときは目盛りだけを残します。
        return;
    }

    M5.Lcd.fillArc(cx, cy, GAUGE_INNER_RADIUS, GAUGE_OUTER_RADIUS,
                   GAUGE_START_DEGREE,
                   GAUGE_START_DEGREE + 3.6F * this->_battery_level,
                   batteryGaugeColor());
}

bool CrazyEyes::toggleBatteryGauge(void) {
    this->_shows_battery = !this->_shows_battery;
    if (this->_shows_battery) {
        readBattery(millis());
    }

    // 消すときはキャンバスを描き直してアークを消去します。
    show();
    return this->_shows_battery;
}

// 白目の見えている範囲を行ごとに記録しておけば，まぶたの形が変わっても虹彩
// を正しく切れます。
//
// 判定には青成分だけを使います。白目(255)と肌色(138)は青が大きく離れていて，
// 輪郭線はどちらも青が小さいので混ざりません。この余裕があるため，高コント
// ラストな輪郭線の周りで出るJPEGのデコード誤差に影響されません。
//
// キャンバスは RGB565 をバイトを入れ替えた形式で保持しているため，生の値から
// ビットを取り出さず lgfx::swap565_t を通します。
static bool isSclera(const uint16_t raw) {
    return lgfx::swap565_t(raw).B8() >= SCLERA_MIN_BLUE;
}

void CrazyEyes::scanSclera(const M5Canvas& canvas, const uint8_t level) {
    const int32_t w = canvas.width();
    const int32_t h = canvas.height();
    const uint16_t* buffer = static_cast<const uint16_t*>(canvas.getBuffer());

    for (int32_t y = 0; y < MAX_HEIGHT; ++y) {
        this->_sclera_left[level][y] = 0;
        this->_sclera_right[level][y] = -1;
        if (buffer == nullptr || y >= h) {
            continue;
        }

        const uint16_t* row = buffer + y * w;
        // 数画素の隙間は連結したうえで，最も長い連なりを白目とみなします。
        int32_t bestLeft = 0;
        int32_t bestRight = -1;
        int32_t runLeft = -1;
        int32_t runRight = -1;
        for (int32_t x = 0; x < w; ++x) {
            if (!isSclera(row[x])) {
                continue;
            }
            if (runLeft < 0) {
                runLeft = x;
            } else if (x - runRight > MAX_SCLERA_GAP) {
                if (runRight - runLeft > bestRight - bestLeft) {
                    bestLeft = runLeft;
                    bestRight = runRight;
                }
                runLeft = x;
            }
            runRight = x;
        }
        if (runLeft >= 0 && runRight - runLeft > bestRight - bestLeft) {
            bestLeft = runLeft;
            bestRight = runRight;
        }
        if (bestRight - bestLeft + 1 >= MIN_SCLERA_RUN) {
            this->_sclera_left[level][y] = static_cast<int16_t>(bestLeft);
            this->_sclera_right[level][y] = static_cast<int16_t>(bestRight);
        }
    }
}

// 白目の見えている範囲だけを塗ります。半目では虹彩がスライス状に見えます。
void CrazyEyes::drawIrisCircle(const int32_t cx, const int32_t cy,
                               const int32_t r, const uint16_t color) {
    const int16_t* left = this->_sclera_left[this->_open_level];
    const int16_t* right = this->_sclera_right[this->_open_level];
    const int32_t bottom = M5.Display.height() < MAX_HEIGHT
                               ? M5.Display.height()
                               : MAX_HEIGHT;

    for (int32_t y = cy - r; y <= cy + r; ++y) {
        if (y < 0 || y >= bottom || left[y] > right[y]) {
            continue;
        }

        const int32_t dy = y - cy;
        const int32_t dx =
            static_cast<int32_t>(sqrtf(static_cast<float>(r * r - dy * dy)));
        int32_t x0 = cx - dx;
        int32_t x1 = cx + dx;
        if (x0 < left[y]) {
            x0 = left[y];
        }
        if (x1 > right[y]) {
            x1 = right[y];
        }
        if (x0 <= x1) {
            M5.Lcd.drawFastHLine(x0, y, x1 - x0 + 1, color);
        }
    }
}

void CrazyEyes::drawIris(void) {
    const int32_t cx = M5.Display.width() / 2 + this->_gaze;
    const int32_t cy = M5.Display.height() / 2;

    drawIrisCircle(cx, cy, IRIS_RADIUS, IRIS_COLOR);
    drawIrisCircle(cx, cy, PUPIL_RADIUS, PUPIL_COLOR);
}

bool CrazyEyes::setOpenLevel(const uint8_t level) {
    const uint8_t clamped =
        level >= OPEN_LEVEL_COUNT ? OPEN_LEVEL_COUNT - 1 : level;
    if (clamped == this->_open_level) {
        return false;
    }

    this->_open_level = clamped;
    if (this->_is_opened) {
        show();
    }
    return true;
}

bool CrazyEyes::setGaze(const int32_t offset) {
    const int32_t clamped =
        offset < -GAZE_MAX_OFFSET
            ? -GAZE_MAX_OFFSET
            : (offset > GAZE_MAX_OFFSET ? GAZE_MAX_OFFSET : offset);
    if (clamped == this->_gaze) {
        return false;
    }

    this->_gaze = clamped;
    if (this->_is_opened) {
        show();
    }
    return true;
}

void CrazyEyes::drawEye(void) {
    if (this->_is_cached) {
        M5Canvas& canvas = this->_is_opened
                               ? this->_opened_eye[this->_open_level]
                               : this->_closed_eye;
        canvas.pushSprite(&M5.Lcd, 0, 0);
        if (this->_is_opened) {
            drawIris();
        }
        return;
    }

    if (this->_is_opened) {
        const EyeImage& image = OPEN_EYE_IMAGES[this->_open_level];
        M5.Lcd.drawJpg(image.start, image.end - image.start, 0, 0,
                       M5.Display.width(), M5.Display.height(), 0, 0, 0.0F,
                       0.0F, middle_center);
        drawIris();
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
    if (this->_shows_battery) {
        drawBatteryGauge();
    }
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
