#include "service_manager.h"

#include <algorithm>
#include <limits>
#include <ranges>

namespace NApplication {

ServiceManager::IEntry::IEntry(std::type_index type, std::vector<std::type_index> dependencies)
    : Type(type)
    , Dependencies(std::move(dependencies)) {
}

ServiceManager::~ServiceManager() {
    (void)Stop();
    m_state = EServiceState::STOPPING;
    m_lifetime.reset();
    // Destruct dependents before their dependencies, even across repeated runs.
    for (auto index: std::views::reverse(m_order)) {
        m_entries[index].reset();
    }
    for (auto& entry: std::views::reverse(m_entries)) {
        entry.reset();
    }
}

ServiceManager::RegistrationGuard::RegistrationGuard(ServiceManager& manager, std::type_index type)
    : Manager(manager) {
    manager.ValidateOwner();
    if (manager.m_state != EServiceState::CONFIGURING || manager.m_registering) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Service registration is closed or reentrant");
    }
    if (manager.Find(type) != nullptr) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::ALREADY_EXISTS, "Service is already registered: {}", type.name());
    }
    manager.m_registering = true;
}

ServiceManager::RegistrationGuard::~RegistrationGuard() {
    Manager.m_registering = false;
}

void ServiceManager::ValidateOwner() const {
    if (std::this_thread::get_id() != m_owner) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Service lifecycle requires its owner thread");
    }
}

ServiceManager::IEntry* ServiceManager::Find(std::type_index type) noexcept {
    for (const auto& entry: m_entries) {
        if (entry != nullptr && entry->Type == type) {
            return entry.get();
        }
    }
    return nullptr;
}

const ServiceManager::IEntry* ServiceManager::Find(std::type_index type) const noexcept {
    for (const auto& entry: m_entries) {
        if (entry != nullptr && entry->Type == type) {
            return entry.get();
        }
    }
    return nullptr;
}

std::vector<std::size_t> ServiceManager::BuildOrder() const {
    // Stable Kahn ordering: choose the earliest registered ready service at every step.
    std::vector<std::size_t> order;
    order.reserve(m_entries.size());
    std::vector<bool> selected(m_entries.size(), false);
    for (const auto& entry: m_entries) {
        for (const auto dependency: entry->Dependencies) {
            if (Find(dependency) == nullptr) {
                GRAPHICS_ENGINE_THROW(NCommon::EError::NOT_FOUND,
                                      "Missing service dependency: {} requires {}",
                                      entry->Type.name(),
                                      dependency.name());
            }
        }
    }
    while (order.size() != m_entries.size()) {
        bool progress = false;
        for (std::size_t index = 0; index < m_entries.size(); ++index) {
            if (selected[index]) {
                continue;
            }
            const bool ready = std::ranges::all_of(m_entries[index]->Dependencies, [&](std::type_index dependency) {
                return std::ranges::any_of(order, [&](std::size_t predecessor) {
                    return m_entries[predecessor]->Type == dependency;
                });
            });
            if (ready) {
                order.push_back(index);
                selected[index] = true;
                progress = true;
                break;
            }
        }
        if (!progress) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Service dependency graph contains a cycle");
        }
    }
    return order;
}

std::uint64_t ServiceManager::Start() {
    ValidateOwner();
    if (m_registering || (m_state != EServiceState::CONFIGURING && m_state != EServiceState::STOPPED)) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Service startup is closed or reentrant");
    }
    if (m_generation == std::numeric_limits<std::uint64_t>::max()) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE, "Service generation exhausted");
    }
    if (m_state == EServiceState::CONFIGURING) {
        auto order = BuildOrder();
        std::vector<ServiceStopFailure> failures;
        failures.reserve(m_entries.size());
        for (const auto& entry: m_entries) {
            failures.push_back({entry->Type, {}});
        }
        m_order = std::move(order);
        m_stopFailures = std::move(failures);
    }
    for (auto& failure: m_stopFailures) {
        failure.Failure = {};
    }
    m_failure = {};
    ++m_generation;
    m_state = EServiceState::STARTING;
    const ServiceContext context{.Services = *this, .Generation = m_generation};
    try {
        for (auto index: m_order) {
            m_entries[index]->Start(context);
            ++m_started;
        }
        m_state = EServiceState::RUNNING;
    } catch (...) {
        m_failure = std::current_exception();
        StopStarted();
        std::rethrow_exception(m_failure);
    }
    return m_generation;
}

void ServiceManager::StopStarted() noexcept {
    m_state = EServiceState::STOPPING;
    bool failed = false;
    while (m_started != 0) {
        const auto index = m_order[--m_started];
        try {
            m_entries[index]->Stop();
        } catch (...) {
            failed = true;
            auto failure = std::current_exception();
            m_stopFailures[index].Failure = failure;
            if (m_failure == nullptr) {
                m_failure = failure;
            }
        }
    }
    m_state = failed ? EServiceState::FAILED : EServiceState::STOPPED;
}

bool ServiceManager::Stop() noexcept {
    if (std::this_thread::get_id() != m_owner || m_registering || m_state == EServiceState::STARTING ||
        m_state == EServiceState::STOPPING || m_state == EServiceState::FAILED) {
        return false;
    }
    if (m_state == EServiceState::RUNNING) {
        StopStarted();
    }
    return m_state != EServiceState::FAILED;
}

EServiceState ServiceManager::GetState() const noexcept {
    return m_state;
}

std::uint64_t ServiceManager::GetGeneration() const noexcept {
    return m_generation;
}

std::exception_ptr ServiceManager::GetFailure() const noexcept {
    return m_failure;
}

std::span<const ServiceStopFailure> ServiceManager::GetStopFailures() const noexcept {
    return m_stopFailures;
}

} // namespace NApplication
