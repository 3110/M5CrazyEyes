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
           static_cast<uint8_t>(EyeCommand::FastBlink);
}
