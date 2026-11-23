#pragma once

#include <cstdint>
#include <string>

namespace NMetadataFixture {

struct Position {
    float X = 0;
    float Y = 0;
};

struct Label {
    std::string Text;
};

enum class EMode : std::uint16_t {
    IDLE,
    ACTIVE,
};

int ReadPosition();
int ReadMode();

} // namespace NMetadataFixture
