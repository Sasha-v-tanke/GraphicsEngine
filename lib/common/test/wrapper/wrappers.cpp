#include <type_traits>

#include <lib/common/wrapper/non_copyable.h>
#include <lib/common/wrapper/non_transferable.h>

namespace {

class NonCopyableType: public NCommon::NonCopyable {};

class NonTransferableType: public NCommon::NonTransferable {};

static_assert(std::is_default_constructible_v<NonCopyableType>);
static_assert(std::is_destructible_v<NonCopyableType>);

static_assert(!std::is_copy_constructible_v<NonCopyableType>);
static_assert(!std::is_copy_assignable_v<NonCopyableType>);

static_assert(std::is_move_constructible_v<NonCopyableType>);
static_assert(std::is_move_assignable_v<NonCopyableType>);

static_assert(std::is_default_constructible_v<NonTransferableType>);
static_assert(std::is_destructible_v<NonTransferableType>);

static_assert(!std::is_copy_constructible_v<NonTransferableType>);
static_assert(!std::is_copy_assignable_v<NonTransferableType>);

static_assert(!std::is_move_constructible_v<NonTransferableType>);
static_assert(!std::is_move_assignable_v<NonTransferableType>);

static_assert(!std::is_default_constructible_v<NCommon::NonCopyable>);
static_assert(!std::is_destructible_v<NCommon::NonCopyable>);

static_assert(!std::is_default_constructible_v<NCommon::NonTransferable>);
static_assert(!std::is_destructible_v<NCommon::NonTransferable>);

} // namespace
