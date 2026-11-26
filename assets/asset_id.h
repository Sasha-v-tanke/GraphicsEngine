#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace NAssets {

class AssetId final {
public:
    using Bytes = std::array<std::uint8_t, 16>;

    constexpr AssetId() = default;
    explicit constexpr AssetId(Bytes bytes) noexcept
        : m_bytes(bytes) {
    }

    [[nodiscard]] static std::expected<AssetId, std::string> Parse(std::string_view value) {
        if (value.size() != 32) {
            return std::unexpected("AssetId must contain exactly 32 lowercase hexadecimal characters");
        }
        Bytes bytes{};
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const auto decode = [](char c) constexpr -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                return -1;
            };
            const int high = decode(value[index * 2]);
            const int low = decode(value[index * 2 + 1]);
            if (high < 0 || low < 0) {
                return std::unexpected("AssetId must use lowercase hexadecimal characters");
            }
            bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
        }
        return AssetId{bytes};
    }

    [[nodiscard]] std::string ToString() const {
        static constexpr char Digits[] = "0123456789abcdef";
        std::string result(32, '0');
        for (std::size_t index = 0; index < m_bytes.size(); ++index) {
            result[index * 2] = Digits[m_bytes[index] >> 4];
            result[index * 2 + 1] = Digits[m_bytes[index] & 0x0f];
        }
        return result;
    }

    [[nodiscard]] constexpr const Bytes& GetBytes() const noexcept {
        return m_bytes;
    }
    [[nodiscard]] friend constexpr bool operator==(const AssetId&, const AssetId&) noexcept = default;
    [[nodiscard]] friend constexpr auto operator<=>(const AssetId&, const AssetId&) noexcept = default;

private:
    Bytes m_bytes{};
};

} // namespace NAssets
