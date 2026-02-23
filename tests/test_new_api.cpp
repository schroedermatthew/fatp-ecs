/**
 * @file test_new_api.cpp
 * @brief Tests for newly added Registry API methods and the View hot-loop fix.
 *
 * Covers:
 *   - replace<T>()          — overwrite an existing component, fire onComponentUpdated
 *   - emplace_or_replace<T>() — upsert semantics
 *   - emplace_context<T>()  — store a context object
 *   - ctx<T>()              — retrieve (asserts if missing)
 *   - try_ctx<T>()          — retrieve or nullptr
 *   - erase_context<T>()    — remove a context object
 *   - View single-component hot loop — verify correct data via raw-pointer path
 */

#include <cassert>
#include <cstdio>
#include <string>

#include <fatp_ecs/FatpEcs.h>

// =============================================================================
// Minimal test harness (same style as the rest of the suite)
// =============================================================================

static int gTestsPassed = 0;
static int gTestsFailed = 0;

#define TEST_ASSERT(condition, msg)                                        \
    do                                                                     \
    {                                                                      \
        if (!(condition))                                                  \
        {                                                                  \
            std::printf("  FAIL: %s (line %d)\n", msg, __LINE__);         \
            ++gTestsFailed;                                                \
            return;                                                        \
        }                                                                  \
    } while (false)

#define RUN_TEST(fn)                         \
    do                                       \
    {                                        \
        std::printf("  %s\n", #fn);          \
        fn();                                \
        if (gTestsFailed == 0)               \
            ++gTestsPassed;                  \
    } while (false)

// Keep a local failure counter so RUN_TEST can detect per-test failure.
// We reset it each time a test function runs by wrapping gTestsFailed tracking.
#undef RUN_TEST
#define RUN_TEST(fn)                                           \
    do                                                         \
    {                                                          \
        std::printf("  %s ... ", #fn);                         \
        int failsBefore = gTestsFailed;                        \
        fn();                                                  \
        if (gTestsFailed == failsBefore)                       \
        {                                                      \
            ++gTestsPassed;                                    \
            std::printf("OK\n");                               \
        }                                                      \
    } while (false)

// =============================================================================
// Test component types
// =============================================================================

struct Health   { int hp;  };
struct Position { float x, y; };

struct PhysicsConfig
{
    float gravity;
    float timestep;
    bool  operator==(const PhysicsConfig&) const = default;
};

// =============================================================================
// replace<T> tests
// =============================================================================

static void test_replace_basic()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Health>(e, Health{100});

    reg.replace<Health>(e, Health{50});
    TEST_ASSERT(reg.get<Health>(e).hp == 50, "replace<T> should update the stored value");
}

static void test_replace_fires_updated_event()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Health>(e, Health{100});

    int updatedCount = 0;
    auto conn = reg.events().onComponentUpdated<Health>().connect(
        [&](fatp_ecs::Entity, const Health&) { ++updatedCount; });

    reg.replace<Health>(e, Health{75});
    TEST_ASSERT(updatedCount == 1, "replace<T> should fire onComponentUpdated exactly once");
}

static void test_replace_does_not_fire_added_event()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Health>(e, Health{100});

    int addedCount = 0;
    auto conn = reg.events().onComponentAdded<Health>().connect(
        [&](fatp_ecs::Entity, const Health&) { ++addedCount; });

    reg.replace<Health>(e, Health{60});
    TEST_ASSERT(addedCount == 0, "replace<T> should NOT fire onComponentAdded");
}

// =============================================================================
// emplace_or_replace<T> tests
// =============================================================================

static void test_emplace_or_replace_inserts_when_absent()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    TEST_ASSERT(!reg.has<Health>(e), "entity should not have Health yet");

    reg.emplace_or_replace<Health>(e, Health{80});
    TEST_ASSERT(reg.has<Health>(e),         "component should exist after emplace_or_replace");
    TEST_ASSERT(reg.get<Health>(e).hp == 80, "inserted value should be 80");
}

static void test_emplace_or_replace_replaces_when_present()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Health>(e, Health{100});

    reg.emplace_or_replace<Health>(e, Health{25});
    TEST_ASSERT(reg.get<Health>(e).hp == 25, "emplace_or_replace should overwrite existing value");
}

static void test_emplace_or_replace_fires_added_on_insert()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();

    int addedCount   = 0;
    int updatedCount = 0;
    auto c1 = reg.events().onComponentAdded<Health>().connect(
        [&](fatp_ecs::Entity, const Health&) { ++addedCount; });
    auto c2 = reg.events().onComponentUpdated<Health>().connect(
        [&](fatp_ecs::Entity, const Health&) { ++updatedCount; });

    reg.emplace_or_replace<Health>(e, Health{10});
    TEST_ASSERT(addedCount   == 1, "should fire onComponentAdded on insert");
    TEST_ASSERT(updatedCount == 0, "should NOT fire onComponentUpdated on insert");
}

static void test_emplace_or_replace_fires_updated_on_replace()
{
    fatp_ecs::Registry reg;
    auto e = reg.create();
    reg.add<Health>(e, Health{100});

    int addedCount   = 0;
    int updatedCount = 0;
    auto c1 = reg.events().onComponentAdded<Health>().connect(
        [&](fatp_ecs::Entity, const Health&) { ++addedCount; });
    auto c2 = reg.events().onComponentUpdated<Health>().connect(
        [&](fatp_ecs::Entity, const Health&) { ++updatedCount; });

    reg.emplace_or_replace<Health>(e, Health{55});
    TEST_ASSERT(addedCount   == 0, "should NOT fire onComponentAdded on replace");
    TEST_ASSERT(updatedCount == 1, "should fire onComponentUpdated on replace");
}

// =============================================================================
// ctx() / emplace_context() / try_ctx() / erase_context() tests
// =============================================================================

static void test_ctx_basic_store_and_retrieve()
{
    fatp_ecs::Registry reg;
    reg.emplace_context<PhysicsConfig>(PhysicsConfig{9.81f, 0.016f});

    auto& cfg = reg.ctx<PhysicsConfig>();
    TEST_ASSERT(cfg.gravity  == 9.81f,  "gravity should be 9.81");
    TEST_ASSERT(cfg.timestep == 0.016f, "timestep should be 0.016");
}

static void test_ctx_const_access()
{
    fatp_ecs::Registry reg;
    reg.emplace_context<PhysicsConfig>(PhysicsConfig{1.0f, 0.5f});

    const fatp_ecs::Registry& creg = reg;
    const auto& cfg = creg.ctx<PhysicsConfig>();
    TEST_ASSERT(cfg.gravity == 1.0f, "const ctx<T> should return correct value");
}

static void test_ctx_replace_existing()
{
    fatp_ecs::Registry reg;
    reg.emplace_context<PhysicsConfig>(PhysicsConfig{9.81f, 0.016f});
    reg.emplace_context<PhysicsConfig>(PhysicsConfig{1.62f, 0.033f}); // moon gravity

    auto& cfg = reg.ctx<PhysicsConfig>();
    TEST_ASSERT(cfg.gravity  == 1.62f,  "emplace_context should replace the previous value");
    TEST_ASSERT(cfg.timestep == 0.033f, "timestep should be updated too");
}

static void test_ctx_multiple_types()
{
    fatp_ecs::Registry reg;
    reg.emplace_context<PhysicsConfig>(PhysicsConfig{9.81f, 0.016f});
    reg.emplace_context<int>(42);

    TEST_ASSERT(reg.ctx<PhysicsConfig>().gravity == 9.81f, "PhysicsConfig gravity mismatch");
    TEST_ASSERT(reg.ctx<int>() == 42,                      "int context mismatch");
}

static void test_try_ctx_returns_nullptr_when_absent()
{
    fatp_ecs::Registry reg;
    auto* p = reg.try_ctx<PhysicsConfig>();
    TEST_ASSERT(p == nullptr, "try_ctx should return nullptr when context not stored");
}

static void test_try_ctx_returns_pointer_when_present()
{
    fatp_ecs::Registry reg;
    reg.emplace_context<PhysicsConfig>(PhysicsConfig{9.81f, 0.016f});

    auto* p = reg.try_ctx<PhysicsConfig>();
    TEST_ASSERT(p != nullptr,          "try_ctx should return non-null when context is stored");
    TEST_ASSERT(p->gravity == 9.81f,   "try_ctx pointer should point to correct data");
}

static void test_erase_context()
{
    fatp_ecs::Registry reg;
    reg.emplace_context<PhysicsConfig>(PhysicsConfig{9.81f, 0.016f});

    bool erased = reg.erase_context<PhysicsConfig>();
    TEST_ASSERT(erased,                                   "erase_context should return true when type was present");
    TEST_ASSERT(reg.try_ctx<PhysicsConfig>() == nullptr,  "context should be gone after erase");

    bool erasedAgain = reg.erase_context<PhysicsConfig>();
    TEST_ASSERT(!erasedAgain, "erase_context should return false when type was already absent");
}

// =============================================================================
// View hot-loop fix — single-component iteration correctness
// =============================================================================

static void test_view_single_component_correct_values()
{
    // Regression test: previously store->dataAt(i) dispatched virtually every
    // iteration. This verifies the raw-pointer path produces correct output.
    fatp_ecs::Registry reg;

    constexpr int N = 1000;
    std::vector<fatp_ecs::Entity> entities;
    entities.reserve(N);
    for (int i = 0; i < N; ++i)
    {
        auto e = reg.create();
        reg.add<Health>(e, Health{i * 3});
        entities.push_back(e);
    }

    // Iterate and accumulate — if the raw-pointer fix is wrong (e.g. wrong
    // offset or stale pointer) the sum won't match.
    long long sum = 0;
    reg.view<Health>().each([&](fatp_ecs::Entity, const Health& h) {
        sum += h.hp;
    });

    // Expected sum: 0*3 + 1*3 + ... + 999*3 = 3 * N*(N-1)/2
    long long expected = 3LL * N * (N - 1) / 2;
    TEST_ASSERT(sum == expected, "View single-component iteration must produce correct sum");
}

static void test_view_single_component_mutation()
{
    fatp_ecs::Registry reg;
    constexpr int N = 200;

    for (int i = 0; i < N; ++i)
    {
        auto e = reg.create();
        reg.add<Position>(e, Position{static_cast<float>(i), 0.0f});
    }

    // Mutate via view
    reg.view<Position>().each([](fatp_ecs::Entity, Position& p) {
        p.y = p.x * 2.0f;
    });

    // Verify mutations
    int checked = 0;
    reg.view<Position>().each([&](fatp_ecs::Entity, const Position& p) {
        TEST_ASSERT(p.y == p.x * 2.0f, "mutated position.y should equal x*2");
        ++checked;
    });
    TEST_ASSERT(checked == N, "should have checked all N entities");
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    std::printf("=== FAT-P ECS New API Tests ===\n\n");

    std::printf("[replace<T>]\n");
    RUN_TEST(test_replace_basic);
    RUN_TEST(test_replace_fires_updated_event);
    RUN_TEST(test_replace_does_not_fire_added_event);

    std::printf("\n[emplace_or_replace<T>]\n");
    RUN_TEST(test_emplace_or_replace_inserts_when_absent);
    RUN_TEST(test_emplace_or_replace_replaces_when_present);
    RUN_TEST(test_emplace_or_replace_fires_added_on_insert);
    RUN_TEST(test_emplace_or_replace_fires_updated_on_replace);

    std::printf("\n[ctx() / emplace_context() / try_ctx() / erase_context()]\n");
    RUN_TEST(test_ctx_basic_store_and_retrieve);
    RUN_TEST(test_ctx_const_access);
    RUN_TEST(test_ctx_replace_existing);
    RUN_TEST(test_ctx_multiple_types);
    RUN_TEST(test_try_ctx_returns_nullptr_when_absent);
    RUN_TEST(test_try_ctx_returns_pointer_when_present);
    RUN_TEST(test_erase_context);

    std::printf("\n[View single-component hot-loop]\n");
    RUN_TEST(test_view_single_component_correct_values);
    RUN_TEST(test_view_single_component_mutation);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                gTestsPassed, gTestsFailed);

    return gTestsFailed > 0 ? 1 : 0;
}
