#pragma once

#include <memory>
#include <optional>

#include <engine/engine_config.h>
#include <lib/common/error/error.h>
#include <lib/common/wrapper/non_transferable.h>

namespace NEngine {

enum class EEngineState {
    CREATED,
    RUNNING,
    STOPPING,
    STOPPED,
};

class Engine final: private NCommon::NonTransferable {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();

    void Start();

    void Stop() noexcept;

    bool Update();

    bool Draw();

    [[nodiscard]] EEngineState GetState() const noexcept;

    [[nodiscard]] std::optional<NCommon::ErrorInfo> GetLastError() const;

    void ClearLastError();

private:
    class Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace NEngine
