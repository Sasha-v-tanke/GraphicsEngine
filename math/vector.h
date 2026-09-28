#pragma once

#include <cstddef>

namespace NMath {

struct Vec2 {
    float X = 0.0F;
    float Y = 0.0F;
};

struct Vec3 {
    float X = 0.0F;
    float Y = 0.0F;
    float Z = 0.0F;
};

struct Vec4 {
    float X = 0.0F;
    float Y = 0.0F;
    float Z = 0.0F;
    float W = 0.0F;
};

constexpr Vec2 operator+(Vec2 left, Vec2 right) noexcept {
    return {
            .X = left.X + right.X,
            .Y = left.Y + right.Y,
    };
}

constexpr Vec2 operator-(Vec2 left, Vec2 right) noexcept {
    return {
            .X = left.X - right.X,
            .Y = left.Y - right.Y,
    };
}

constexpr Vec2 operator*(Vec2 vector, float scale) noexcept {
    return {
            .X = vector.X * scale,
            .Y = vector.Y * scale,
    };
}

constexpr Vec3 operator+(Vec3 left, Vec3 right) noexcept {
    return {
            .X = left.X + right.X,
            .Y = left.Y + right.Y,
            .Z = left.Z + right.Z,
    };
}

constexpr Vec3 operator-(Vec3 left, Vec3 right) noexcept {
    return {
            .X = left.X - right.X,
            .Y = left.Y - right.Y,
            .Z = left.Z - right.Z,
    };
}

constexpr Vec3 operator*(Vec3 vector, float scale) noexcept {
    return {
            .X = vector.X * scale,
            .Y = vector.Y * scale,
            .Z = vector.Z * scale,
    };
}

constexpr Vec4 operator+(Vec4 left, Vec4 right) noexcept {
    return {
            .X = left.X + right.X,
            .Y = left.Y + right.Y,
            .Z = left.Z + right.Z,
            .W = left.W + right.W,
    };
}

constexpr Vec4 operator-(Vec4 left, Vec4 right) noexcept {
    return {
            .X = left.X - right.X,
            .Y = left.Y - right.Y,
            .Z = left.Z - right.Z,
            .W = left.W - right.W,
    };
}

constexpr Vec4 operator*(Vec4 vector, float scale) noexcept {
    return {
            .X = vector.X * scale,
            .Y = vector.Y * scale,
            .Z = vector.Z * scale,
            .W = vector.W * scale,
    };
}

[[nodiscard]] float Dot(Vec3 left, Vec3 right) noexcept;
[[nodiscard]] Vec3 Cross(Vec3 left, Vec3 right) noexcept;
[[nodiscard]] float Length(Vec3 vector) noexcept;
[[nodiscard]] Vec3 Normalize(Vec3 vector) noexcept;

} // namespace NMath
