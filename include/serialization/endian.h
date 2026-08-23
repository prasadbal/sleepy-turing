#pragma once
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <span>

namespace marketlib::serialization {

template<std::integral T>
constexpr T from_big_endian(T val) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        return std::byteswap(val);
    return val;
}

template<std::integral T>
constexpr T to_big_endian(T val) noexcept { return from_big_endian(val); }

template<std::integral T>
constexpr T from_little_endian(T val) noexcept {
    if constexpr (std::endian::native == std::endian::big)
        return std::byteswap(val);
    return val;
}

template<typename T>
requires std::is_trivially_copyable_v<T>
[[nodiscard]] T read_at(const std::byte* ptr) noexcept {
    T val; std::memcpy(&val, ptr, sizeof(T)); return val;
}

template<typename T>
requires std::is_trivially_copyable_v<T>
[[nodiscard]] T read_at(std::span<const std::byte> buf, std::size_t offset = 0) noexcept {
    return read_at<T>(buf.data() + offset);
}

template<std::integral T>
[[nodiscard]] T read_be(const std::byte* ptr) noexcept {
    return from_big_endian(read_at<T>(ptr));
}

template<std::integral T>
[[nodiscard]] T read_be(std::span<const std::byte> buf, std::size_t offset = 0) noexcept {
    return read_be<T>(buf.data() + offset);
}

[[nodiscard]] constexpr bool has_bytes(std::span<const std::byte> buf,
                                       std::size_t required) noexcept {
    return buf.size() >= required;
}

} // namespace marketlib::serialization
