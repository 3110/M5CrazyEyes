#pragma once

#include <M5Unified.h>

class CrazyEyes {
public:
    static constexpr const char* NAME = "M5CrazyEyes";
    static constexpr const char* VERSION = "v0.0.3";

    // 開いた状態の段階です。0が完全に開いた状態で，数が増えるほど眠そうに
    // なります。閉じた状態はこれとは別に持ちます。
    static constexpr uint8_t OPEN_LEVEL_COUNT = 4;

    CrazyEyes(void);
    virtual ~CrazyEyes(void) = default;

    virtual bool begin(const int bgColor = TFT_WHITE);
    virtual bool update(void);
    virtual void show(void);
    virtual bool setOpened(const bool opened);
    virtual bool blink(void);
    virtual bool toggleBatteryGauge(void);
    virtual bool setGaze(const int32_t offset);
    virtual bool setOpenLevel(const uint8_t level);
    virtual uint8_t getOpenLevel(void) const { return this->_open_level; }

private:
    bool cacheEye(M5Canvas& canvas, const uint8_t* jpeg, const size_t size);
    void scanSclera(const M5Canvas& canvas, const uint8_t level);
    void drawEye(void);
    void drawIris(void);
    void drawIrisCircle(const int32_t cx, const int32_t cy, const int32_t r,
                        const uint16_t color);
    void drawBatteryGauge(void);
    uint16_t batteryGaugeColor(void) const;
    bool readBattery(const uint32_t now);
    bool updateBattery(const uint32_t now);

    static constexpr int32_t MAX_HEIGHT = 480;

    volatile bool _is_opened;
    M5Canvas _opened_eye[OPEN_LEVEL_COUNT];
    M5Canvas _closed_eye;
    bool _is_cached;
    uint8_t _open_level;
    int32_t _gaze;
    // 段階ごとの，行ごとに白目が見えている左右の端です。虹彩をこの範囲へ
    // 収めることで，まぶたからはみ出さずに描けます。
    int16_t _sclera_left[OPEN_LEVEL_COUNT][MAX_HEIGHT];
    int16_t _sclera_right[OPEN_LEVEL_COUNT][MAX_HEIGHT];
    bool _shows_battery;
    bool _is_charging;
    bool _is_pulse_on;
    int32_t _battery_level;
    uint32_t _battery_polled_at;
    uint32_t _pulsed_at;
};
