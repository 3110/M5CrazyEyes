#include <EspNowManager.h>
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
constexpr uint32_t FAST_BLINK_INTERVAL_MS = 100;
constexpr uint32_t CONTROL_TIMEOUT_MS = 600;

enum class EyeMode : uint8_t {
    Open,
    Closed,
    FastBlink,
};

CrazyEyes eyes;
EspNowManager espnow;
EyeId eyeId = EyeId::Left;
QueueHandle_t commandQueue = nullptr;
EyeMode eyeMode = EyeMode::Open;
uint32_t lastControlAt = 0;
uint32_t lastBlinkAt = 0;

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
    drawMessage(value == EyeId::Left ? "LEFT" : "RIGHT", nullptr, 5);
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

void applyCommand(const EyeCommand command, const uint32_t now) {
    switch (command) {
        case EyeCommand::Open:
            eyeMode = EyeMode::Open;
            eyes.setOpened(true);
            break;

        case EyeCommand::Close:
            eyeMode = EyeMode::Closed;
            lastControlAt = now;
            eyes.setOpened(false);
            break;

        case EyeCommand::FastBlink:
            if (eyeMode != EyeMode::FastBlink) {
                eyes.setOpened(false);
                lastBlinkAt = now;
            }
            eyeMode = EyeMode::FastBlink;
            lastControlAt = now;
            break;
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
        eyes.setOpened(true);
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

    eyes.begin();
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
    updateEye(millis());
}
