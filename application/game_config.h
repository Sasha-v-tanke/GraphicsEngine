#pragma once

namespace NApplication {

enum class ERenderingMode {
    DISABLED,
    STANDARD_3D,
};

struct GameConfig {
    ERenderingMode RenderingMode = ERenderingMode::STANDARD_3D;
};

} // namespace NApplication
