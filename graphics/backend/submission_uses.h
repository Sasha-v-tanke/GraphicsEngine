#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <vector>

#include <GraphicsEngine/lib/common/wrapper/non_transferable.h>

namespace NGraphics::NBackend {

// Native representation owners (allocations, views, bindings, immutable versions).
// Capture during recording, before logical owners can unload or replace them.
using SubmissionUseSet = std::vector<std::shared_ptr<const void>>;

// One completion owner/timeline; externally serialized with submission and collection.
// The native owner must drain or establish device loss before destroying this queue.
class SubmissionRetirementQueue final: private NCommon::NonTransferable {
private:
    struct Entry {
        std::uint64_t Completion = 0;
        SubmissionUseSet Uses;
        bool Accepted = false;
    };
    using Entries = std::list<Entry>;

public:
    using RetiredUses = Entries;
    class Prepared final {
    public:
        Prepared(Prepared&& other) noexcept;
        Prepared& operator=(Prepared&&) = delete;
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;
        ~Prepared();

        // Allocation-free publication immediately after native acceptance. For device loss,
        // retain uncertain uses through the owner's separate shutdown cleanup path too.
        void Commit() noexcept;

    private:
        Prepared(SubmissionRetirementQueue& owner, Entries::iterator entry) noexcept;
        SubmissionRetirementQueue* m_owner;
        Entries::iterator m_entry;
        friend class SubmissionRetirementQueue;
    };

    // All allocations and validation precede native submit. Destruction rolls back an
    // unaccepted reservation; no bookkeeping after acceptance may throw or allocate.
    [[nodiscard]] Prepared Prepare(std::uint64_t completion, std::span<const std::shared_ptr<const void>> uses);
    // Destroy detached owners outside native locks: deleters may reenter backend code.
    [[nodiscard]] RetiredUses TakeCompleted(std::uint64_t observedCompletion) noexcept;
    // Only after successful native idle/drain, or confirmed device loss with admission stopped.
    [[nodiscard]] RetiredUses TakeAfterShutdown() noexcept;
    [[nodiscard]] std::size_t GetPendingSubmissionCount() const noexcept;
    [[nodiscard]] std::size_t GetPendingUseCount() const noexcept;

private:
    Entries m_entries;
    std::uint64_t m_lastAccepted = 0;
    bool m_closed = false;
};

} // namespace NGraphics::NBackend
