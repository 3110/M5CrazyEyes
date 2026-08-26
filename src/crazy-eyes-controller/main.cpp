#include <EspNowManager.h>
#include <M5Unified.h>

#include <esp_random.h>

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
// 片方のキーを押したまま，もう片方をこの時間以内にタップするとトグルします。
constexpr uint32_t CHORD_TAP_MAX_MS = 250;
// タップしたキーは，押したままのキーよりこれだけ後から押されている必要が
// あります。両キーを同時に押して離しただけの操作と区別するためです。
constexpr uint32_t CHORD_MIN_OFFSET_MS = 200;
// キー操作がこの時間なければランダムまばたきを始めます。0で自動開始を止めます。
constexpr uint32_t IDLE_BLINK_DELAY_MS = 10000;
constexpr uint32_t RANDOM_BLINK_MIN_INTERVAL_MS = 2000;
constexpr uint32_t RANDOM_BLINK_MAX_INTERVAL_MS = 6000;
constexpr uint32_t RANDOM_BLINK_CLOSE_MIN_MS = 90;
constexpr uint32_t RANDOM_BLINK_CLOSE_MAX_MS = 150;
constexpr uint32_t RANDOM_BLINK_GAP_MS = 220;
constexpr uint32_t DOUBLE_BLINK_PERCENT = 15;
// キー操作では作れない応答パターンです。通常のまばたきと区別できます。
constexpr uint8_t ACK_ON_BLINK_COUNT = 3;
constexpr uint32_t ACK_ON_CLOSE_MS = 90;
constexpr uint32_t ACK_ON_GAP_MS = 90;
constexpr uint32_t ACK_OFF_CLOSE_MS = 600;

enum class Ack : uint8_t {
    None,
    On,
    Off,
};

struct KeyState {
    m5::Button_Class button;
    bool firstTapArmed = false;
    bool fastBlinking = false;
    uint32_t pressedAt = 0;
    uint32_t firstTapReleasedAt = 0;
    uint32_t lastSentAt = 0;
    uint8_t openRepeatLeft = 0;
};

// 両目を同時にまばたきさせる連続動作です。ランダムまばたきと応答パターンの
// どちらもこれで表現します。
struct BlinkSequence {
    uint8_t remaining = 0;
    bool closed = false;
    uint32_t phaseAt = 0;
    uint32_t waitMs = 0;
    uint32_t closeMs = 0;
    uint32_t gapMs = 0;
};

EspNowManager espnow;
KeyState key1;
KeyState key2;
bool espnowReady = false;

bool randomBlinkEnabled = true;
Ack pendingAck = Ack::None;
BlinkSequence sequence;
uint8_t sequenceOpenRepeatLeft = 0;
uint32_t sequenceOpenRepeatAt = 0;
uint32_t lastKeyActivityAt = 0;
uint32_t randomBlinkAt = 0;
uint32_t randomBlinkWaitMs = 0;

uint32_t randomBetween(const uint32_t min, const uint32_t max) {
    return min + esp_random() % (max - min + 1);
}

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

void broadcastBothEyes(const EyeCommand command) {
    if (!espnowReady) {
        return;
    }
    broadcastCommand(KEY1_EYE_ID, command);
    broadcastCommand(KEY2_EYE_ID, command);
}

void sendCommand(KeyState& state, const EyeId eyeId,
                 const EyeCommand command, const uint32_t now) {
    state.lastSentAt = now;
    if (espnowReady) {
        broadcastCommand(eyeId, command);
    }
}

bool isAnyKeyPressed(void) {
    return key1.button.isPressed() || key2.button.isPressed();
}

void startBlinkSequence(const uint8_t count, const uint32_t closeMs,
                        const uint32_t gapMs, const uint32_t now) {
    sequence.remaining = count;
    sequence.closed = false;
    sequence.closeMs = closeMs;
    sequence.gapMs = gapMs;
    sequence.phaseAt = now;
    sequence.waitMs = 0;
}

// 手動操作を優先します。閉じたまま残さないように開くコマンドを送ります。
void abortBlinkSequence(void) {
    if (sequence.remaining == 0) {
        return;
    }
    if (sequence.closed) {
        broadcastBothEyes(EyeCommand::Open);
    }
    sequence.remaining = 0;
    sequence.closed = false;
    sequenceOpenRepeatLeft = 0;
}

void updateBlinkSequence(const uint32_t now) {
    if (sequence.remaining == 0 || now - sequence.phaseAt < sequence.waitMs) {
        return;
    }

    if (!sequence.closed) {
        broadcastBothEyes(EyeCommand::Close);
        sequenceOpenRepeatLeft = 0;
        sequence.closed = true;
        sequence.phaseAt = now;
        sequence.waitMs = sequence.closeMs;
        return;
    }

    broadcastBothEyes(EyeCommand::Open);
    sequenceOpenRepeatLeft = OPEN_REPEAT_COUNT - 1;
    sequenceOpenRepeatAt = now;
    sequence.closed = false;
    sequence.phaseAt = now;
    sequence.waitMs = sequence.gapMs;
    --sequence.remaining;
}

void updateSequenceOpenRepeat(const uint32_t now) {
    if (sequenceOpenRepeatLeft == 0 ||
        now - sequenceOpenRepeatAt < OPEN_REPEAT_MS) {
        return;
    }
    --sequenceOpenRepeatLeft;
    sequenceOpenRepeatAt = now;
    broadcastBothEyes(EyeCommand::Open);
}

void scheduleRandomBlink(const uint32_t now) {
    randomBlinkAt = now;
    randomBlinkWaitMs = randomBetween(RANDOM_BLINK_MIN_INTERVAL_MS,
                                      RANDOM_BLINK_MAX_INTERVAL_MS);
}

void updateRandomBlink(const uint32_t now) {
    if (!randomBlinkEnabled || IDLE_BLINK_DELAY_MS == 0 ||
        sequence.remaining > 0 || pendingAck != Ack::None ||
        now - lastKeyActivityAt < IDLE_BLINK_DELAY_MS ||
        now - randomBlinkAt < randomBlinkWaitMs) {
        return;
    }

    // 人はときどき2回続けて瞬きます。
    const uint8_t count = esp_random() % 100 < DOUBLE_BLINK_PERCENT ? 2 : 1;
    startBlinkSequence(count,
                       randomBetween(RANDOM_BLINK_CLOSE_MIN_MS,
                                     RANDOM_BLINK_CLOSE_MAX_MS),
                       RANDOM_BLINK_GAP_MS, now);
    scheduleRandomBlink(now);
}

// 修飾キー方式です。押したままのキーと，短くタップしたキーの組み合わせを見ます。
// 両キーをほぼ同時に押した場合は，離す順序がずれてもトグルしません。
bool isChord(const KeyState& tapped, const KeyState& held, const uint32_t now) {
    if (!tapped.button.wasReleased() || !held.button.isPressed()) {
        return false;
    }
    if (now - tapped.pressedAt > CHORD_TAP_MAX_MS) {
        return false;
    }

    const int32_t offset =
        static_cast<int32_t>(tapped.pressedAt - held.pressedAt);
    return offset >= static_cast<int32_t>(CHORD_MIN_OFFSET_MS);
}

void updateChord(const uint32_t now) {
    if (!isChord(key1, key2, now) && !isChord(key2, key1, now)) {
        return;
    }

    randomBlinkEnabled = !randomBlinkEnabled;
    pendingAck = randomBlinkEnabled ? Ack::On : Ack::Off;
    // トグル直後の押下が高速まばたきにならないようにします。
    key1.firstTapArmed = false;
    key2.firstTapArmed = false;
    if (Serial) {
        Serial.printf("Random blink: %s\n", randomBlinkEnabled ? "on" : "off");
    }
}

// 応答パターンは，キーを離して開くコマンドを送り終えてから再生します。
void startPendingAck(const uint32_t now) {
    if (pendingAck == Ack::None || isAnyKeyPressed() ||
        key1.openRepeatLeft > 0 || key2.openRepeatLeft > 0) {
        return;
    }

    if (pendingAck == Ack::On) {
        startBlinkSequence(ACK_ON_BLINK_COUNT, ACK_ON_CLOSE_MS, ACK_ON_GAP_MS,
                           now);
    } else {
        startBlinkSequence(1, ACK_OFF_CLOSE_MS, 0, now);
    }
    pendingAck = Ack::None;
    lastKeyActivityAt = now;
    scheduleRandomBlink(now);
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
    // キーはプルアップします。USBを抜くとGPIO0の外部回路が外れるため，
    // プルアップなしでは押下の検出が不安定になります。
    pinMode(KEY1_PIN, INPUT_PULLUP);
    pinMode(KEY2_PIN, INPUT_PULLUP);

    espnowReady = espnow.begin(ESP_NOW_CHANNEL);
    if (!espnowReady) {
        ESP_LOGE("setup", "Failed to initialize ESP-NOW");
    }

    lastKeyActivityAt = millis();
    scheduleRandomBlink(lastKeyActivityAt);
}

void loop(void) {
    M5.update();

    const uint32_t now = millis();
    key1.button.setRawState(now, digitalRead(KEY1_PIN) == LOW);
    key2.button.setRawState(now, digitalRead(KEY2_PIN) == LOW);

    if (isAnyKeyPressed()) {
        lastKeyActivityAt = now;
        abortBlinkSequence();
    }

    updateKey(key1, KEY1_EYE_ID, now);
    updateKey(key2, KEY2_EYE_ID, now);
    updateChord(now);

    startPendingAck(now);
    updateRandomBlink(now);
    updateBlinkSequence(now);
    updateSequenceOpenRepeat(now);

    delay(10);
}
