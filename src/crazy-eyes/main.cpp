#include <EspNowManager.h>
#include <M5PM1.h>
#include <Preferences.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "CrazyEyes.hpp"
#include "CrazyEyesProtocol.hpp"

// -DCRAZY_EYES_GAZE_DEBUG=1 を指定すると，加速度と視線量をシリアルへ出力し
// ます。取り付け方を変えて，傾ける向きと視線の向きを確認するときに使います。
#ifndef CRAZY_EYES_GAZE_DEBUG
#define CRAZY_EYES_GAZE_DEBUG 0
#endif

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
constexpr uint32_t GAZE_UPDATE_MS = 20;
// 画面の横方向に対応する重力成分がこの値で振り切れます。sin(30度)です。
constexpr float GAZE_TILT_LIMIT = 0.5F;
// 傾けた側へ瞳が寄ります。向きが逆なら 1.0F にしてください。
constexpr float GAZE_DIRECTION = -1.0F;
constexpr float GAZE_MAX_OFFSET = 40.0F;
// バネと減衰です。頭を止めたあとに瞳がわずかに揺り戻します。
constexpr float GAZE_SPRING = 0.28F;
constexpr float GAZE_DAMPING = 0.45F;
constexpr int32_t GAZE_MIN_STEP = 2;
// 活動が途切れてからこの間隔で一段ずつ眠くなり，最後に目を閉じます。
constexpr uint32_t SLEEP_STEP_MS = 30000;
constexpr uint32_t SLEEP_STEP_COUNT = CrazyEyes::OPEN_LEVEL_COUNT;
// 加速度の緩やかな平均からのずれで動きを見ます。直前のサンプルとの差分だと
// IMUのノイズをそのまま拾ってしまうためです。
constexpr float MOTION_FILTER = 0.05F;
constexpr float MOTION_THRESHOLD = 0.10F;

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
uint32_t lastActivityAt = 0;
uint32_t sleepStep = 0;
const char* lastActivitySource = "boot";
bool hasMotionReference = false;
float refAx = 0.0F;
float refAy = 0.0F;
float refAz = 0.0F;
float gazePosition = 0.0F;
float gazeVelocity = 0.0F;
int32_t gazeShown = 0;
uint32_t lastGazeAt = 0;

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
// 何か起きたら，一段ずつ戻すのではなく即座に見開きます。
void markActivity(const uint32_t now, const char* source) {
    lastActivityAt = now;
    lastActivitySource = source;
    if (sleepStep == 0) {
        return;
    }

    const bool wasAsleep = sleepStep >= SLEEP_STEP_COUNT;
    sleepStep = 0;
    eyes.setOpenLevel(0);
    if (wasAsleep) {
        eyes.setOpened(true);
    }
}

// 動きもタッチも受信もない状態が続くと，だんだん目が落ちていきます。
void updateSleep(const uint32_t now) {
    uint32_t step = (now - lastActivityAt) / SLEEP_STEP_MS;
    if (step > SLEEP_STEP_COUNT) {
        step = SLEEP_STEP_COUNT;
    }
    if (step == sleepStep) {
        return;
    }

    sleepStep = step;
    if (step >= SLEEP_STEP_COUNT) {
        eyes.setOpened(false);
        return;
    }
    eyes.setOpenLevel(static_cast<uint8_t>(step));
}

void applyRemoteEye(const bool opened) {
    if (touchState == TouchState::Idle && sleepStep < SLEEP_STEP_COUNT) {
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

void applyCommand(const uint8_t raw, const uint32_t now) {
    if (!isAutomaticEyeCommand(raw)) {
        markActivity(now, "command");
    }

    switch (toEyeCommand(raw)) {
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

// 画面の横方向に対応する加速度の軸です。この機体ではX軸が画面の縦方向で，
// 横方向はY軸でした。取り付け方を変えたときはここを差し替えます。
inline float gazeTiltSource(const float ax, const float ay, const float az) {
    return ay;
}

// 重力の画面横方向成分から，瞳を寄せる量を決めます。目標へバネで追従させる
// ことで，頭を止めたあとに少し揺り戻し，生き物らしい動きになります。
void updateGaze(const uint32_t now) {
    if (!M5.Imu.isEnabled() || now - lastGazeAt < GAZE_UPDATE_MS) {
        return;
    }
    lastGazeAt = now;

    M5.Imu.update();
    float ax = 0.0F;
    float ay = 0.0F;
    float az = 0.0F;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) {
        return;
    }

    if (!hasMotionReference) {
        hasMotionReference = true;
        refAx = ax;
        refAy = ay;
        refAz = az;
    }
    const float motion =
        fabsf(ax - refAx) + fabsf(ay - refAy) + fabsf(az - refAz);
    refAx += (ax - refAx) * MOTION_FILTER;
    refAy += (ay - refAy) * MOTION_FILTER;
    refAz += (az - refAz) * MOTION_FILTER;
    if (motion > MOTION_THRESHOLD) {
        markActivity(now, "motion");
    }

#if CRAZY_EYES_GAZE_DEBUG
    static uint32_t sleepLoggedAt = 0;
    static float motionPeak = 0.0F;
    if (motion > motionPeak) {
        motionPeak = motion;
    }
    if (now - sleepLoggedAt >= 2000) {
        sleepLoggedAt = now;
        Serial.printf("sleep: idle=%us step=%u motion=%.3f peak=%.3f by=%s\n",
                      static_cast<unsigned>((now - lastActivityAt) / 1000),
                      static_cast<unsigned>(sleepStep), motion, motionPeak,
                      lastActivitySource);
        motionPeak = 0.0F;
    }
#endif

    float tilt = GAZE_DIRECTION * gazeTiltSource(ax, ay, az) / GAZE_TILT_LIMIT;
    tilt = tilt < -1.0F ? -1.0F : (tilt > 1.0F ? 1.0F : tilt);

    const float target = tilt * GAZE_MAX_OFFSET;
    gazeVelocity += (target - gazePosition) * GAZE_SPRING;
    gazeVelocity *= 1.0F - GAZE_DAMPING;
    gazePosition += gazeVelocity;

    const int32_t offset = static_cast<int32_t>(lroundf(gazePosition));
    if (offset > gazeShown - GAZE_MIN_STEP &&
        offset < gazeShown + GAZE_MIN_STEP) {
        return;
    }

    gazeShown = offset;
    eyes.setGaze(offset);

#if CRAZY_EYES_GAZE_DEBUG
    static uint32_t loggedAt = 0;
    if (now - loggedAt >= 500) {
        loggedAt = now;
        Serial.printf("accel x=%+.2f y=%+.2f z=%+.2f -> gaze=%+d\n", ax, ay, az,
                      static_cast<int>(offset));
    }
#endif
}

void updateTouch(const uint32_t now) {
    if (!M5.Touch.isEnabled()) {
        return;
    }

    if (M5.Touch.getDetail().isPressed()) {
        markActivity(now, "touch");
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
    uint8_t raw = 0;
    if (xQueueReceive(commandQueue, &raw, 0) == pdTRUE) {
        applyCommand(raw, now);
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

    const uint8_t raw = data[1];
    if (commandQueue != nullptr) {
        xQueueOverwrite(commandQueue, &raw);
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
    Serial.printf("IMU: %s\n", M5.Imu.isEnabled() ? "enabled" : "disabled");
    if (!loadEyeId()) {
        stopWithError();
    }

    lastActivityAt = millis();
    commandQueue = xQueueCreate(1, sizeof(uint8_t));
    if (commandQueue == nullptr || !espnow.begin(ESP_NOW_CHANNEL) ||
        !espnow.registerCallback(onDataReceived)) {
        Serial.println("Failed to initialize ESP-NOW receiver.");
        stopWithError();
    }

    eyes.show();
}

void loop(void) {
    eyes.update();

    const uint32_t now = millis();
    if (M5.BtnA.wasClicked()) {
        markActivity(now, "button");
        Serial.printf("Battery gauge: %s\n",
                      eyes.toggleBatteryGauge() ? "on" : "off");
    }

    updateGaze(now);
    updateTouch(now);
    updateEye(now);
    updateSleep(now);
}
