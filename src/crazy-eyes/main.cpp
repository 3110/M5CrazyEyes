#include <EspNowManager.h>
#include <M5PM1.h>
#include <Preferences.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "CrazyEyes.hpp"
#include "CrazyEyesProtocol.hpp"

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "crazy-eyes";
constexpr char EYE_ID_NAME[] = "eye_id";
// 物理ボタンと目の対応です。実機の配置に合わせて入れ替えられます。
constexpr EyeId BTN_A_EYE_ID = EyeId::Left;
constexpr EyeId BTN_B_EYE_ID = EyeId::Right;
constexpr uint32_t CONFIG_BUTTON_HOLD_MS = 1000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 20;
constexpr uint32_t EYE_ID_DISPLAY_MS = 1000;
constexpr uint8_t EYE_ID_TEXT_SIZE = 5;
constexpr uint8_t VERSION_TEXT_SIZE = 2;
constexpr uint32_t FAST_BLINK_INTERVAL_MS = 100;
constexpr uint32_t CONTROL_TIMEOUT_MS = 600;
constexpr uint8_t TOUCH_BLINK_COUNT = 3;
constexpr uint32_t TOUCH_BLINK_CLOSE_MS = 80;
constexpr uint32_t TOUCH_BLINK_OPEN_MS = 80;

enum class EyeMode : uint8_t {
    Open,
    Closed,
    FastBlink,
};

// タッチは遠隔の状態とは別のレイヤーです。有効な間は描画をこちらが持ち，
// 遠隔側は状態の更新だけを続けます。
enum class TouchState : uint8_t {
    Idle,
    Touching,
    Blinking,
};

CrazyEyes eyes;
EspNowManager espnow;
M5PM1 pm1;
EyeId eyeId = EyeId::Left;
QueueHandle_t commandQueue = nullptr;
EyeMode eyeMode = EyeMode::Open;
uint32_t lastControlAt = 0;
uint32_t lastBlinkAt = 0;
TouchState touchState = TouchState::Idle;
uint8_t touchBlinkLeft = 0;
bool touchBlinkClosed = false;
uint32_t touchPhaseAt = 0;
uint32_t touchPhaseMs = 0;

// 本体のステータスLEDを消します。全消灯してから給電を落とさないと，消灯前の
// 色がデータに残り，次に給電が入った瞬間に一瞬点灯します。
void turnOffStatusLed(void) {
    if (pm1.begin(&M5.In_I2C) != M5PM1_OK) {
        Serial.println("Failed to open M5PM1. Leaving the status LED as is.");
        return;
    }

    const m5pm1_rgb_t black[M5PM1_MAX_LED_COUNT] = {};
    pm1.setLeds(black, M5PM1_MAX_LED_COUNT, M5PM1_MAX_LED_COUNT,
                /*autoRefresh=*/true);
    pm1.setLedEnLevel(false);
}

void printEyeId(const EyeId value) {
    Serial.printf("%c (%s)", static_cast<uint8_t>(value),
                  value == EyeId::Left ? "left" : "right");
}

void drawMessage(const char* line1, const char* line2, const uint8_t textSize) {
    const int32_t cx = M5.Display.width() / 2;
    const int32_t cy = M5.Display.height() / 2;

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(textSize);
    M5.Display.setTextDatum(middle_center);
    if (line2 == nullptr) {
        M5.Display.drawString(line1, cx, cy);
        return;
    }

    const int32_t offset = M5.Display.fontHeight();
    M5.Display.drawString(line1, cx, cy - offset);
    M5.Display.drawString(line2, cx, cy + offset);
}

void showEyeId(const EyeId value) {
    drawMessage(value == EyeId::Left ? "LEFT" : "RIGHT", nullptr,
                EYE_ID_TEXT_SIZE);

    char title[32];
    snprintf(title, sizeof(title), "%s %s", CrazyEyes::NAME,
             CrazyEyes::VERSION);
    M5.Display.setTextSize(VERSION_TEXT_SIZE);
    M5.Display.drawString(title, M5.Display.width() / 2,
                          M5.Display.height() * 3 / 4);

    delay(EYE_ID_DISPLAY_MS);
}

// 起動時にAまたはBボタンを長押ししていれば，その目を選択します。
bool readEyeIdFromButton(EyeId& selected) {
    M5.update();
    delay(BUTTON_DEBOUNCE_MS);
    M5.update();

    m5::Button_Class* button = nullptr;
    if (M5.BtnA.isPressed()) {
        button = &M5.BtnA;
        selected = BTN_A_EYE_ID;
    } else if (M5.BtnB.isPressed()) {
        button = &M5.BtnB;
        selected = BTN_B_EYE_ID;
    }
    if (button == nullptr) {
        return false;
    }

    while (!button->pressedFor(CONFIG_BUTTON_HOLD_MS)) {
        M5.update();
        if (!button->isPressed()) {
            return false;
        }
        delay(10);
    }
    return true;
}

// 未設定のときは，どちらかのボタンが押されるまで画面で選択を待ちます。
EyeId waitEyeIdFromButton(void) {
    drawMessage("A = LEFT", "B = RIGHT", 3);

    Serial.println("Eye ID is not configured. Press A (left) or B (right).");
    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) {
            return BTN_A_EYE_ID;
        }
        if (M5.BtnB.wasPressed()) {
            return BTN_B_EYE_ID;
        }
        delay(10);
    }
}

// 値が変わるときだけ書き込みます。
bool saveEyeId(Preferences& preferences, const EyeId value) {
    const uint8_t storedEyeId = static_cast<uint8_t>(value);
    if (preferences.isKey(EYE_ID_NAME) &&
        preferences.getUChar(EYE_ID_NAME) == storedEyeId) {
        return true;
    }

    if (preferences.putUChar(EYE_ID_NAME, storedEyeId) !=
        sizeof(storedEyeId)) {
        Serial.println("Failed to save the eye ID.");
        return false;
    }

    Serial.print("Saved eye ID: ");
    printEyeId(value);
    Serial.println();
    return true;
}

bool loadEyeId(void) {
    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Failed to open NVS preferences.");
        return false;
    }

    EyeId selected = EyeId::Left;
    if (!readEyeIdFromButton(selected)) {
        const uint8_t storedEyeId = preferences.isKey(EYE_ID_NAME)
                                        ? preferences.getUChar(EYE_ID_NAME)
                                        : 0;
        selected = isValidEyeId(storedEyeId) ? static_cast<EyeId>(storedEyeId)
                                             : waitEyeIdFromButton();
    }

    if (!saveEyeId(preferences, selected)) {
        preferences.end();
        return false;
    }
    preferences.end();

    eyeId = selected;
    Serial.print("Using eye ID: ");
    printEyeId(eyeId);
    Serial.println();

    showEyeId(eyeId);
    return true;
}

[[noreturn]] void stopWithError(void) {
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("ERROR", M5.Display.width() / 2,
                          M5.Display.height() / 2);
    while (true) {
        M5.update();
        delay(100);
    }
}

// タッチ中は遠隔側の描画を止めます。状態の更新とタイムアウトの判定は続ける
// ため，指を離したときに正しい状態へ戻せます。
void applyRemoteEye(const bool opened) {
    if (touchState == TouchState::Idle) {
        eyes.setOpened(opened);
    }
}

// タッチの動作が終わったら，その時点の遠隔状態を描き直します。
void restoreRemoteEye(const uint32_t now) {
    if (eyeMode == EyeMode::FastBlink) {
        lastBlinkAt = now;
        eyes.setOpened(false);
        return;
    }
    eyes.setOpened(eyeMode != EyeMode::Closed);
}

void applyCommand(const EyeCommand command, const uint32_t now) {
    switch (command) {
        case EyeCommand::Open:
            eyeMode = EyeMode::Open;
            applyRemoteEye(true);
            break;

        case EyeCommand::Close:
            eyeMode = EyeMode::Closed;
            lastControlAt = now;
            applyRemoteEye(false);
            break;

        case EyeCommand::FastBlink:
            if (eyeMode != EyeMode::FastBlink) {
                applyRemoteEye(false);
                lastBlinkAt = now;
            }
            eyeMode = EyeMode::FastBlink;
            lastControlAt = now;
            break;
    }
}

// 指を離したあとのパチパチです。閉じると開くを交互に繰り返します。
void updateTouchBlink(const uint32_t now) {
    if (now - touchPhaseAt < touchPhaseMs) {
        return;
    }

    if (touchBlinkClosed) {
        eyes.setOpened(true);
        touchBlinkClosed = false;
        --touchBlinkLeft;
        touchPhaseAt = now;
        touchPhaseMs = TOUCH_BLINK_OPEN_MS;
        return;
    }

    if (touchBlinkLeft == 0) {
        touchState = TouchState::Idle;
        restoreRemoteEye(now);
        return;
    }

    eyes.setOpened(false);
    touchBlinkClosed = true;
    touchPhaseAt = now;
    touchPhaseMs = TOUCH_BLINK_CLOSE_MS;
}

void updateTouch(const uint32_t now) {
    if (!M5.Touch.isEnabled()) {
        return;
    }

    if (M5.Touch.getDetail().isPressed()) {
        if (touchState != TouchState::Touching) {
            touchState = TouchState::Touching;
            eyes.setOpened(false);
        }
        return;
    }

    if (touchState == TouchState::Touching) {
        touchState = TouchState::Blinking;
        touchBlinkLeft = TOUCH_BLINK_COUNT;
        touchBlinkClosed = false;
        touchPhaseAt = now;
        touchPhaseMs = 0;
        return;
    }

    if (touchState == TouchState::Blinking) {
        updateTouchBlink(now);
    }
}

void updateEye(const uint32_t now) {
    EyeCommand command;
    if (xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
        applyCommand(command, now);
    }

    if (eyeMode != EyeMode::Open &&
        now - lastControlAt >= CONTROL_TIMEOUT_MS) {
        eyeMode = EyeMode::Open;
        applyRemoteEye(true);
        return;
    }

    if (touchState != TouchState::Idle) {
        return;
    }

    if (eyeMode == EyeMode::FastBlink &&
        now - lastBlinkAt >= FAST_BLINK_INTERVAL_MS) {
        lastBlinkAt = now;
        eyes.blink();
    }
}
}  // namespace

void onDataReceived(const uint8_t* addr, const uint8_t* data, int len) {
    if (data == nullptr || len != EYE_COMMAND_PACKET_SIZE ||
        data[0] != static_cast<uint8_t>(eyeId) ||
        !isValidEyeCommand(data[1])) {
        return;
    }

    const EyeCommand command = static_cast<EyeCommand>(data[1]);
    if (commandQueue != nullptr) {
        xQueueOverwrite(commandQueue, &command);
    }

    ESP_LOGD("OnDataReceived", "From: %s", macToStr(addr).str);
    ESP_LOG_BUFFER_HEXDUMP("OnDataReceived", data, len, ESP_LOG_DEBUG);
}

void setup(void) {
    Serial.begin(115200);
    esp_log_level_set("*", static_cast<esp_log_level_t>(LOG_LOCAL_LEVEL));

    Serial.println();
    Serial.printf("%s %s\n", CrazyEyes::NAME, CrazyEyes::VERSION);

    eyes.begin();
    turnOffStatusLed();
    Serial.printf("Touch panel: %s\n",
                  M5.Touch.isEnabled() ? "enabled" : "disabled");
    if (!loadEyeId()) {
        stopWithError();
    }

    commandQueue = xQueueCreate(1, sizeof(EyeCommand));
    if (commandQueue == nullptr || !espnow.begin(ESP_NOW_CHANNEL) ||
        !espnow.registerCallback(onDataReceived)) {
        Serial.println("Failed to initialize ESP-NOW receiver.");
        stopWithError();
    }

    eyes.show();
}

void loop(void) {
    eyes.update();

    if (M5.BtnA.wasClicked()) {
        Serial.printf("Battery gauge: %s\n",
                      eyes.toggleBatteryGauge() ? "on" : "off");
    }

    const uint32_t now = millis();
    updateTouch(now);
    updateEye(now);
}
