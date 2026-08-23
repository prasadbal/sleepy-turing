#include <catch2/catch_test_macros.hpp>
#include <memory/pool.h>

using namespace marketlib::memory;

struct Order { int id{}; double price{}; int qty{}; };

TEST_CASE("PoolAllocator: basic alloc/free", "[memory][pool]") {
    PoolAllocator<Order, 8> pool;
    REQUIRE(pool.available() == 8);
    Order* o = pool.construct(1, 100.5, 10);
    REQUIRE(o != nullptr);
    REQUIRE(o->id == 1);
    REQUIRE(pool.available() == 7);
    pool.deallocate(o);
    REQUIRE(pool.available() == 8);
}

TEST_CASE("PoolAllocator: returns nullptr when exhausted", "[memory][pool]") {
    PoolAllocator<Order, 2> pool;
    auto* a = pool.allocate();
    auto* b = pool.allocate();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(pool.allocate() == nullptr);
    pool.deallocate(new (a) Order{});
    REQUIRE(pool.allocate() != nullptr);
}

TEST_CASE("ArenaAllocator: bump allocation and reset", "[memory][arena]") {
    ArenaAllocator<1024> arena;
    REQUIRE(arena.used() == 0);
    int* p = arena.construct<int>(42);
    REQUIRE(p != nullptr);
    REQUIRE(*p == 42);
    arena.reset();
    REQUIRE(arena.used() == 0);
}

TEST_CASE("ArenaAllocator: returns nullptr when exhausted", "[memory][arena]") {
    ArenaAllocator<16> arena;
    REQUIRE(arena.allocate(16) != nullptr);
    REQUIRE(arena.allocate(1)  == nullptr);
}
