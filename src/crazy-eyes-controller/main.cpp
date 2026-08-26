#include <EspNowManager.h>
#include <M5Unified.h>

#include "CrazyEyesProtocol.hpp"

namespace {
constexpr uint8_t KEY1_PIN = 0;
constexpr uint8_t KEY2_PIN = 17;
constexpr EyeId KEY1_EYE_ID = EyeId::Left;
constexpr EyeId KEY2_EYE_ID = EyeId::Right;
constexpr uint32_t FIRST_TAP_MAX_MS = 250;
constexpr uint32_t SECOND_PRESS_WINDOW_MS = 300;
constexpr uint32_t CONTROL_REPEAT_MS = 200;
constexpr uint32_t OPEN_REPEAT_COUNT = 3;
constexpr uint32_t OPEN_REPEAT_MS = 50;

struct KeyState {
    m5::Button_Class button;
    bool firstTapArmed = false;
    bool fastBlinking = false;
    uint32_t pressedAt = 0;
    uint32_t firstTapReleasedAt = 0;
    uint32_t lastSentAt = 0;
    uint8_t openRepeatLeft = 0;
};

EspNowManager espnow;
KeyState key1;
KeyState key2;
bool espnowReady = false;

void broadcastCommand(const EyeId eyeId, const EyeCommand command) {
    const uint8_t packet[EYE_COMMAND_PACKET_SIZE] = {
        static_cast<uint8_t>(eyeId),
        static_cast<uint8_t>(command),
    };
    if (!espnow.broadcast(packet, sizeof(packet))) {
        ESP_LOGE("broadcastCommand",
                 "Failed to broadcast command: eye_id=%c command=0x%02X",
                 packet[0], packet[1]);
    }
}

void sendCommand(KeyState& state, const EyeId eyeId,
                 const EyeCommand command, const uint32_t now) {
    state.lastSentAt = now;
    if (espnowReady) {
        broadcastCommand(eyeId, command);
    }
}

void updateKey(KeyState& state, const EyeId eyeId, const uint32_t now) {
    if (state.firstTapArmed &&
        now - state.firstTapReleasedAt > SECOND_PRESS_WINDOW_MS) {
        state.firstTapArmed = false;
    }

    if (state.button.wasPressed()) {
        state.openRepeatLeft = 0;
        state.pressedAt = now;
        state.fastBlinking =
            state.firstTapArmed &&
            now - state.firstTapReleasedAt <= SECOND_PRESS_WINDOW_MS;
        state.firstTapArmed = false;
        sendCommand(state, eyeId,
                    state.fastBlinking ? EyeCommand::FastBlink
                                       : EyeCommand::Close,
                    now);
    }

    if (state.button.isPressed() &&
        now - state.lastSentAt >= CONTROL_REPEAT_MS) {
        sendCommand(state, eyeId,
                    state.fastBlinking ? EyeCommand::FastBlink
                                       : EyeCommand::Close,
                    now);
    }

    if (state.button.wasReleased()) {
        sendCommand(state, eyeId, EyeCommand::Open, now);
        state.openRepeatLeft = OPEN_REPEAT_COUNT - 1;

        if (!state.fastBlinking && now - state.pressedAt <= FIRST_TAP_MAX_MS) {
            state.firstTapArmed = true;
            state.firstTapReleasedAt = now;
        } else {
            state.firstTapArmed = false;
        }
        state.fastBlinking = false;
    }

    // 開くコマンドは取りこぼしても目が開くように複数回送信します。
    if (state.openRepeatLeft > 0) {
        if (state.button.isPressed()) {
            state.openRepeatLeft = 0;
        } else if (now - state.lastSentAt >= OPEN_REPEAT_MS) {
            --state.openRepeatLeft;
            sendCommand(state, eyeId, EyeCommand::Open, now);
        }
    }
}
}  // namespace

void setup(void) {
    Serial.begin(115200);
    esp_log_level_set("*", static_cast<esp_log_level_t>(LOG_LOCAL_LEVEL));

    M5.begin();
    pinMode(KEY1_PIN, INPUT);
    pinMode(KEY2_PIN, INPUT);

    espnowReady = espnow.begin(ESP_NOW_CHANNEL);
    if (!espnowReady) {
        ESP_LOGE("setup", "Failed to initialize ESP-NOW");
    }
}

void loop(void) {
    M5.update();

    const uint32_t now = millis();
    key1.button.setRawState(now, digitalRead(KEY1_PIN) == LOW);
    key2.button.setRawState(now, digitalRead(KEY2_PIN) == LOW);

    updateKey(key1, KEY1_EYE_ID, now);
    updateKey(key2, KEY2_EYE_ID, now);

    delay(10);
}
