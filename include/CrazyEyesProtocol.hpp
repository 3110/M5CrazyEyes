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

inline bool isValidEyeId(const uint8_t value) {
    return value == static_cast<uint8_t>(EyeId::Left) ||
           value == static_cast<uint8_t>(EyeId::Right);
}

inline bool isValidEyeCommand(const uint8_t value) {
    return value <= static_cast<uint8_t>(EyeCommand::FastBlink);
}
