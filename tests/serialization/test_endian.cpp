#include <catch2/catch_test_macros.hpp>
#include <serialization/endian.h>
#include <array>

using namespace marketlib::serialization;

TEST_CASE("from_big_endian: round-trips correctly", "[endian]") {
    REQUIRE(from_big_endian(from_big_endian(std::uint16_t{0xABCD}))           == 0xABCD);
    REQUIRE(from_big_endian(from_big_endian(std::uint32_t{0xDEADBEEF}))       == 0xDEADBEEF);
    REQUIRE(from_big_endian(from_big_endian(std::uint64_t{0x0102030405060708ULL}))
                                                                               == 0x0102030405060708ULL);
}

TEST_CASE("read_at: reads struct from byte buffer without UB", "[endian]") {
    std::array<std::byte, 4> buf{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x86}, std::byte{0xA0}
    };
    // 0x000186A0 = 100000 (ITCH price in units of 1/10000 = $10.0000)
    const auto val = from_big_endian(read_at<std::uint32_t>(std::span{buf}));
    REQUIRE(val == 100'000u);
}

TEST_CASE("read_be: combined read+convert", "[endian]") {
    std::array<std::byte, 8> buf{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x27}, std::byte{0x10}
    };
    REQUIRE(read_be<std::uint64_t>(std::span{buf}) == 10'000ULL);
}

TEST_CASE("has_bytes: bounds check", "[endian]") {
    std::array<std::byte, 4> buf{};
    REQUIRE(has_bytes(std::span{buf}, 4));
    REQUIRE_FALSE(has_bytes(std::span{buf}, 5));
}
