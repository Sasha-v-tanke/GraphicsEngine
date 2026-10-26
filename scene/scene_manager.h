#pragma once

#include "scene.h"
#include "scene_handle.h"

#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace NScene {

// Registry/publication operations belong to the creating thread. Acquired scenes may be retained by World jobs.
class SceneManager final: private NCommon::NonTransferable {
public:
    SceneManager();

    [[nodiscard]] SceneHandle CreateScene(std::string name = {});
    [[nodiscard]] SceneHandle RegisterScene(std::unique_ptr<Scene> scene);
    [[nodiscard]] bool IsRegistered(SceneHandle handle) const;
    [[nodiscard]] std::shared_ptr<Scene> Acquire(SceneHandle handle) const;

    void Activate(SceneHandle handle);
    void Deactivate();
    void Unload(SceneHandle handle);

    [[nodiscard]] std::optional<SceneHandle> GetActiveHandle() const;
    [[nodiscard]] std::shared_ptr<Scene> AcquireActive() const;

private:
    struct Entry {
        std::shared_ptr<Scene> Value;
        std::uint64_t Generation = 1;
    };

    static std::uint64_t AcquireOwnerId();
    void ValidateThread() const;
    void ValidateHandle(SceneHandle handle) const;
    [[nodiscard]] bool Contains(SceneHandle handle) const noexcept;

    std::uint64_t m_ownerId;
    std::thread::id m_thread;
    std::vector<Entry> m_entries;
    std::vector<std::size_t> m_freeSlots;
    std::optional<SceneHandle> m_active;
};

} // namespace NScene
