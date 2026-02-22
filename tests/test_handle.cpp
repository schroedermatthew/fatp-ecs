/**
 * @file test_handle.cpp
 * @brief Tests for Handle and ConstHandle (entt::handle equivalent).
 *
 * Tests cover:
 *  1.  Default-constructed handle is invalid
 *  2.  handle() factory produces a valid handle
 *  3.  handle.entity() returns the wrapped entity
 *  4.  handle.registry() returns the wrapped registry
 *  5.  handle.isAlive() reflects entity liveness
 *  6.  handle.has<T>() delegates to registry
 *  7.  handle.add<T>() adds a component and returns a ref
 *  8.  handle.get<T>() returns the component
 *  9.  handle.tryGet<T>() returns ptr or nullptr
 * 10.  handle.remove<T>() removes the component
 * 11.  handle.patch<T>(fn) modifies in-place and fires signal
 * 12.  handle.patch<T>() no-arg overload fires signal
 * 13.  handle.destroy() destroys entity and nullifies handle
 * 14.  Handle is copyable; both copies refer to same entity
 * 15.  ConstHandle is implicitly constructible from Handle
 * 16.  ConstHandle.has / get / tryGet work correctly
 * 17.  constHandle() factory produces a valid ConstHandle
 * 18.  operator bool() on valid and invalid handles
 * 19.  operator== / operator!= between handles
 * 20.  tryGet on null handle returns nullptr without crashing
 */

#include <fatp_ecs/FatpEcs.h>

#include <cassert>
#include <cstdio>

using namespace fatp_ecs;

// =============================================================================
// Test harness
// =============================================================================

static int sTestsPassed = 0;
static int sTestsFailed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
            ++sTestsFailed;                                                 \
            return;                                                         \
        }                                                                   \
    } while (0)

#define RUN_TEST(fn)                                    \
    do                                                  \
    {                                                   \
        std::printf("  Running: %s\n", #fn);            \
        fn();                                           \
        ++sTestsPassed;                                 \
    } while (0)

// =============================================================================
// Component types
// =============================================================================

struct Position { float x{0.f}; float y{0.f}; };
struct Health   { uint32_t hp{100}; };
struct Tag      {};

// =============================================================================
// Tests
// =============================================================================

static void test_default_handle_invalid()
{
    Handle h;
    TEST_ASSERT(!h.valid(),         "default handle not valid");
    TEST_ASSERT(!static_cast<bool>(h), "operator bool false for default");
    TEST_ASSERT(h.entity() == NullEntity, "entity is NullEntity");
    TEST_ASSERT(h.registry() == nullptr,  "registry is nullptr");
}

static void test_factory_produces_valid_handle()
{
    Registry reg;
    Entity e = reg.create();
    Handle h = reg.handle(e);
    TEST_ASSERT(h.valid(),  "handle from factory is valid");
    TEST_ASSERT(static_cast<bool>(h), "operator bool true");
}

static void test_entity_accessor()
{
    Registry reg;
    Entity e = reg.create();
    Handle h = reg.handle(e);
    TEST_ASSERT(h.entity() == e, "entity() returns wrapped entity");
}

static void test_registry_accessor()
{
    Registry reg;
    Entity e = reg.create();
    Handle h = reg.handle(e);
    TEST_ASSERT(h.registry() == &reg, "registry() returns pointer to registry");
}

static void test_is_alive()
{
    Registry reg;
    Entity e = reg.create();
    Handle h = reg.handle(e);
    TEST_ASSERT(h.isAlive(), "alive before destroy");
    reg.destroy(e);
    TEST_ASSERT(!h.isAlive(), "not alive after destroy");
}

static void test_has()
{
    Registry reg;
    Entity e = reg.create();
    Handle h = reg.handle(e);
    TEST_ASSERT(!h.has<Position>(), "no Position yet");
    reg.add<Position>(e, 1.f, 2.f);
    TEST_ASSERT(h.has<Position>(), "has Position after add");
}

static void test_add()
{
    Registry reg;
    Handle h = reg.handle(reg.create());
    Position& p = h.add<Position>(3.f, 4.f);
    TEST_ASSERT(p.x == 3.f && p.y == 4.f, "add returns correct component");
    TEST_ASSERT(h.has<Position>(), "has component after add");
}

static void test_get()
{
    Registry reg;
    Handle h = reg.handle(reg.create());
    (void)h.add<Health>(42u);
    TEST_ASSERT(h.get<Health>().hp == 42u, "get returns correct value");
}

static void test_try_get()
{
    Registry reg;
    Handle h = reg.handle(reg.create());
    TEST_ASSERT(h.tryGet<Health>() == nullptr, "tryGet null when absent");
    (void)h.add<Health>(7u);
    TEST_ASSERT(h.tryGet<Health>() != nullptr, "tryGet non-null when present");
    TEST_ASSERT(h.tryGet<Health>()->hp == 7u,  "tryGet value correct");
}

static void test_remove()
{
    Registry reg;
    Handle h = reg.handle(reg.create());
    (void)h.add<Tag>();
    TEST_ASSERT(h.has<Tag>(), "has Tag");
    TEST_ASSERT(h.remove<Tag>(), "remove returns true");
    TEST_ASSERT(!h.has<Tag>(), "no Tag after remove");
    TEST_ASSERT(!h.remove<Tag>(), "remove returns false when absent");
}

static void test_patch_with_func()
{
    Registry reg;
    Handle h = reg.handle(reg.create());
    (void)h.add<Health>(50u);

    bool patched = h.patch<Health>([](Health& hp){ hp.hp = 99u; });
    TEST_ASSERT(patched, "patch returns true");
    TEST_ASSERT(h.get<Health>().hp == 99u, "patch modified component");
}

static void test_patch_no_arg()
{
    Registry reg;
    bool signalFired = false;
    auto conn = reg.events().onComponentUpdated<Health>().connect(
        [&](Entity, Health&){ signalFired = true; });

    Handle h = reg.handle(reg.create());
    (void)h.add<Health>(10u);
    signalFired = false;

    bool result = h.patch<Health>();
    TEST_ASSERT(result, "patch() returns true");
    TEST_ASSERT(signalFired, "onComponentUpdated fired by patch()");
}

static void test_destroy_nullifies_handle()
{
    Registry reg;
    Handle h = reg.handle(reg.create());
    TEST_ASSERT(h.isAlive(), "alive before destroy");
    bool result = h.destroy();
    TEST_ASSERT(result, "destroy returns true");
    TEST_ASSERT(!h.isAlive(), "not alive after destroy");
    TEST_ASSERT(h.entity() == NullEntity, "entity nullified after destroy");
    TEST_ASSERT(!h.destroy(), "second destroy returns false");
}

static void test_handle_copyable()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Health>(e, 77u);

    Handle h1 = reg.handle(e);
    Handle h2 = h1;  // copy

    TEST_ASSERT(h2.entity() == h1.entity(), "copy has same entity");
    TEST_ASSERT(h2.get<Health>().hp == 77u, "copy sees same component");

    // Modifying via h1 visible through h2
    h1.get<Health>().hp = 33u;
    TEST_ASSERT(h2.get<Health>().hp == 33u, "mutation via h1 visible via h2");
}

static void test_const_handle_from_handle()
{
    Registry reg;
    Handle h = reg.handle(reg.create());
    (void)h.add<Position>(5.f, 6.f);

    ConstHandle ch = h;  // implicit conversion
    TEST_ASSERT(ch.valid(), "ConstHandle valid after conversion");
    TEST_ASSERT(ch.has<Position>(), "ConstHandle.has works");
    TEST_ASSERT(ch.get<Position>().x == 5.f, "ConstHandle.get works");
}

static void test_const_handle_try_get()
{
    Registry reg;
    Handle h = reg.handle(reg.create());
    ConstHandle ch = h;

    TEST_ASSERT(ch.tryGet<Health>() == nullptr, "tryGet null when absent");
    (void)h.add<Health>(11u);
    TEST_ASSERT(ch.tryGet<Health>() != nullptr, "tryGet non-null after add");
    TEST_ASSERT(ch.tryGet<Health>()->hp == 11u, "tryGet value");
}

static void test_const_handle_factory()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Health>(e, 55u);

    ConstHandle ch = reg.constHandle(e);
    TEST_ASSERT(ch.valid(), "constHandle factory produces valid handle");
    TEST_ASSERT(ch.isAlive(), "isAlive via constHandle");
    TEST_ASSERT(ch.get<Health>().hp == 55u, "get via constHandle");
}

static void test_operator_bool()
{
    Registry reg;
    Handle null_h;
    Handle valid_h = reg.handle(reg.create());

    TEST_ASSERT(!null_h,  "null handle is false");
    TEST_ASSERT(valid_h,  "valid handle is true");
}

static void test_equality()
{
    Registry reg;
    Entity e1 = reg.create();
    Entity e2 = reg.create();

    Handle h1a = reg.handle(e1);
    Handle h1b = reg.handle(e1);
    Handle h2  = reg.handle(e2);

    TEST_ASSERT(h1a == h1b, "same entity same registry: equal");
    TEST_ASSERT(h1a != h2,  "different entities: not equal");
}

static void test_try_get_null_handle()
{
    Handle h;  // null
    // Must not crash
    const Health* p = h.tryGet<Health>();
    TEST_ASSERT(p == nullptr, "tryGet on null handle returns nullptr");
}

// =============================================================================
// main
// =============================================================================

int main()
{
    std::printf("=== test_handle ===\n");

    RUN_TEST(test_default_handle_invalid);
    RUN_TEST(test_factory_produces_valid_handle);
    RUN_TEST(test_entity_accessor);
    RUN_TEST(test_registry_accessor);
    RUN_TEST(test_is_alive);
    RUN_TEST(test_has);
    RUN_TEST(test_add);
    RUN_TEST(test_get);
    RUN_TEST(test_try_get);
    RUN_TEST(test_remove);
    RUN_TEST(test_patch_with_func);
    RUN_TEST(test_patch_no_arg);
    RUN_TEST(test_destroy_nullifies_handle);
    RUN_TEST(test_handle_copyable);
    RUN_TEST(test_const_handle_from_handle);
    RUN_TEST(test_const_handle_try_get);
    RUN_TEST(test_const_handle_factory);
    RUN_TEST(test_operator_bool);
    RUN_TEST(test_equality);
    RUN_TEST(test_try_get_null_handle);

    std::printf("\n%d/%d tests passed\n", sTestsPassed, sTestsPassed + sTestsFailed);
    return sTestsFailed == 0 ? 0 : 1;
}
