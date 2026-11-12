#include "submission_uses.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

#include <GraphicsEngine/lib/common/error/error.h>
#include <GraphicsEngine/lib/common/error/exception.h>

namespace NGraphics::NBackend {

SubmissionRetirementQueue::Prepared::Prepared(SubmissionRetirementQueue& owner, Entries::iterator entry) noexcept
    : m_owner(&owner)
    , m_entry(entry) {
}

SubmissionRetirementQueue::Prepared::Prepared(Prepared&& other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr))
    , m_entry(other.m_entry) {
}

SubmissionRetirementQueue::Prepared::~Prepared() {
    if (m_owner != nullptr) {
        m_owner->m_entries.erase(m_entry);
    }
}

void SubmissionRetirementQueue::Prepared::Commit() noexcept {
    if (m_owner != nullptr) {
        m_entry->Accepted = true;
        m_owner->m_lastAccepted = m_entry->Completion;
        m_owner = nullptr;
    }
}

SubmissionRetirementQueue::Prepared
SubmissionRetirementQueue::Prepare(std::uint64_t completion, std::span<const std::shared_ptr<const void>> uses) {
    if (m_closed || completion == 0 || completion <= m_lastAccepted ||
        std::ranges::any_of(m_entries, [](const Entry& entry) { return !entry.Accepted; }) ||
        std::ranges::any_of(uses, [](const auto& use) { return !use; })) {
        GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_ARGUMENT, "Invalid submission use-set reservation");
    }
    SubmissionUseSet captured(uses.begin(), uses.end());
    std::ranges::sort(captured, std::owner_less<std::shared_ptr<const void>>{});
    captured.erase(std::unique(captured.begin(),
                               captured.end(),
                               [](const auto& left, const auto& right) {
                                   return !left.owner_before(right) && !right.owner_before(left);
                               }),
                   captured.end());
    m_entries.push_back({completion, std::move(captured), false});
    return Prepared{*this, std::prev(m_entries.end())};
}

SubmissionRetirementQueue::RetiredUses
SubmissionRetirementQueue::TakeCompleted(std::uint64_t observedCompletion) noexcept {
    RetiredUses retired;
    // A prepared entry is not a GPU consumer yet. It belongs to its rollback guard.
    for (auto iterator = m_entries.begin(); iterator != m_entries.end();) {
        if (iterator->Accepted && iterator->Completion <= observedCompletion) {
            const auto completed = iterator++;
            retired.splice(retired.end(), m_entries, completed);
        } else {
            ++iterator;
        }
    }
    return retired;
}

SubmissionRetirementQueue::RetiredUses SubmissionRetirementQueue::TakeAfterShutdown() noexcept {
    m_closed = true;
    return TakeCompleted(std::numeric_limits<std::uint64_t>::max());
}

std::size_t SubmissionRetirementQueue::GetPendingSubmissionCount() const noexcept {
    return m_entries.size();
}

std::size_t SubmissionRetirementQueue::GetPendingUseCount() const noexcept {
    std::size_t count = 0;
    for (const auto& entry: m_entries) {
        count += entry.Uses.size();
    }
    return count;
}

} // namespace NGraphics::NBackend
