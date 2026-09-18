#pragma once

namespace NCommon {

class NonTransferable {
protected:
    NonTransferable() = default;

    ~NonTransferable() = default;

public:
    NonTransferable(const NonTransferable&) = delete;
    NonTransferable(NonTransferable&&) = delete;

    NonTransferable& operator=(const NonTransferable&) = delete;
    NonTransferable& operator=(NonTransferable&&) = delete;
};

} // namespace NCommon
