#pragma once

#include <M5Unified.h>

class CrazyEyes {
public:
    static constexpr const char* VERSION = "v0.0.1";

    CrazyEyes(void);
    virtual ~CrazyEyes(void) = default;

    virtual bool begin(const int bgColor = TFT_WHITE);
    virtual bool update(void);
    virtual void show(void);
    virtual bool setOpened(const bool opened);
    virtual bool blink(void);

private:
    bool cacheEye(M5Canvas& canvas, const uint8_t* jpeg, const size_t size);
    void drawEye(void);

    volatile bool _is_opened;
    M5Canvas _opened_eye;
    M5Canvas _closed_eye;
    bool _is_cached;
};
