#pragma once

#include <M5Unified.h>

class CrazyEyes {
public:
    static constexpr const char* NAME = "M5CrazyEyes";
    static constexpr const char* VERSION = "v0.0.2";

    CrazyEyes(void);
    virtual ~CrazyEyes(void) = default;

    virtual bool begin(const int bgColor = TFT_WHITE);
    virtual bool update(void);
    virtual void show(void);
    virtual bool setOpened(const bool opened);
    virtual bool blink(void);
    virtual bool toggleBatteryGauge(void);
    virtual bool setGaze(const int32_t offset);

private:
    bool cacheEye(M5Canvas& canvas, const uint8_t* jpeg, const size_t size);
    void drawEye(void);
    void drawIris(void);
    void drawBatteryGauge(void);
    uint16_t batteryGaugeColor(void) const;
    bool readBattery(const uint32_t now);
    bool updateBattery(const uint32_t now);

    volatile bool _is_opened;
    M5Canvas _opened_eye;
    M5Canvas _closed_eye;
    bool _is_cached;
    int32_t _gaze;
    bool _shows_battery;
    bool _is_charging;
    bool _is_pulse_on;
    int32_t _battery_level;
    uint32_t _battery_polled_at;
    uint32_t _pulsed_at;
};
