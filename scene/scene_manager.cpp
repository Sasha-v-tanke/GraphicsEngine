#include "scene_manager.h"

#include <atomic>
#include <limits>
#include <utility>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NScene {

SceneManager::SceneManager()
    : m_ownerId(AcquireOwnerId())
    , m_thread(std::this_thread::get_id()) {
}

std::uint64_t SceneManager::AcquireOwnerId() {
    static std::atomic<std::uint64_t> next{1};
    auto current = next.load(std::memory_order_relaxed);
    while (true) {
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "SceneManager identity space is exhausted");
        }
        if (next.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
            return current;
        }
    }
}

void SceneManager::ValidateThread() const {
    if (std::this_thread::get_id() != m_thread) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "SceneManager requires its owner thread");
    }
}

bool SceneManager::Contains(SceneHandle handle) const noexcept {
    return handle.m_ownerId == m_ownerId && handle.m_slotIndex < m_entries.size() &&
           m_entries[handle.m_slotIndex].Generation == handle.m_generation &&
           m_entries[handle.m_slotIndex].Value != nullptr;
}

void SceneManager::ValidateHandle(SceneHandle handle) const {
    if (!Contains(handle)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND, "Scene handle is stale, foreign or unregistered");
    }
}

SceneHandle SceneManager::CreateScene(std::string name) {
    ValidateThread();
    return RegisterScene(std::make_unique<Scene>(std::move(name)));
}

SceneHandle SceneManager::RegisterScene(std::unique_ptr<Scene> scene) {
    ValidateThread();
    if (scene == nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Cannot register a null scene");
    }
    std::shared_ptr<Scene> value{std::move(scene)};
    if (m_freeSlots.empty()) {
        m_entries.push_back({std::move(value), 1});
        return {m_ownerId, m_entries.size() - 1, 1};
    }
    const auto index = m_freeSlots.back();
    m_freeSlots.pop_back();
    m_entries[index].Value = std::move(value);
    return {m_ownerId, index, m_entries[index].Generation};
}

bool SceneManager::IsRegistered(SceneHandle handle) const {
    ValidateThread();
    return Contains(handle);
}

std::shared_ptr<Scene> SceneManager::Acquire(SceneHandle handle) const {
    ValidateThread();
    ValidateHandle(handle);
    return m_entries[handle.m_slotIndex].Value;
}

void SceneManager::Activate(SceneHandle handle) {
    ValidateThread();
    ValidateHandle(handle);
    m_active = handle;
}

void SceneManager::Deactivate() {
    ValidateThread();
    m_active.reset();
}

void SceneManager::Unload(SceneHandle handle) {
    ValidateThread();
    ValidateHandle(handle);
    auto& entry = m_entries[handle.m_slotIndex];
    if (entry.Generation != std::numeric_limits<std::uint64_t>::max()) {
        // Reserve reuse before invalidating anything: allocation failure leaves the registry unchanged.
        m_freeSlots.push_back(handle.m_slotIndex);
        ++entry.Generation;
    }
    if (m_active == handle) {
        m_active.reset();
    }
    // Release last ownership only after registry mutation is complete.
    auto retired = std::move(entry.Value);
}

std::optional<SceneHandle> SceneManager::GetActiveHandle() const {
    ValidateThread();
    return m_active;
}

std::shared_ptr<Scene> SceneManager::AcquireActive() const {
    ValidateThread();
    return m_active.has_value() ? Acquire(*m_active) : nullptr;
}

} // namespace NScene
