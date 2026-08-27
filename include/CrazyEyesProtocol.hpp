#pragma once

#include <stddef.h>
#include <stdint.h>

enum class EyeId : uint8_t {
    Left = 'L',
    Right = 'R',
};

enum class EyeCommand : uint8_t {
    Open = 0,
    Close = 1,
    FastBlink = 2,
    // 短くまばたきします。表示デバイスがタッチされたときに，反対の目へ同じ
    // 反応をさせるために使います。
    Blink = 3,
    // 目の状態は変えず，活動があったことだけを伝えます。表示デバイス同士で
    // 眠さを揃えるために使います。
    Awake = 4,
    // 反対の目が触られている間，押し続けている合図として送ります。受け取った
    // 側はタッチと同じ扱いにするため，コントローラーからの指示より優先します。
    Touch = 5,
};

constexpr size_t EYE_COMMAND_PACKET_SIZE = 2;

// コントローラーが自動で送るコマンドに立てる印です。ランダムまばたきは「誰も
// 操作していない」という意味の通信なので，受信側が活動として数えないように
// 区別できるようにします。
constexpr uint8_t EYE_COMMAND_AUTO_FLAG = 0x80;

inline EyeCommand toEyeCommand(const uint8_t value) {
    return static_cast<EyeCommand>(value & ~EYE_COMMAND_AUTO_FLAG);
}

inline bool isAutomaticEyeCommand(const uint8_t value) {
    return (value & EYE_COMMAND_AUTO_FLAG) != 0;
}

inline bool isValidEyeId(const uint8_t value) {
    return value == static_cast<uint8_t>(EyeId::Left) ||
           value == static_cast<uint8_t>(EyeId::Right);
}

inline bool isValidEyeCommand(const uint8_t value) {
    return (value & ~EYE_COMMAND_AUTO_FLAG) <=
           static_cast<uint8_t>(EyeCommand::Touch);
}
