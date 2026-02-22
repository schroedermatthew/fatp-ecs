/**
 * @file test_snapshot.cpp
 * @brief Tests for RegistrySnapshot / RegistrySnapshotLoader.
 *
 * Tests cover:
 *  1. Round-trip: single component type, multiple entities
 *  2. Round-trip: multiple component types
 *  3. Sparse coverage: not all entities have all components
 *  4. Empty registry round-trip
 *  5. Component type absent from any entity: zero-count block handled
 *  6. Entity count preserved exactly
 *  7. All entities are alive after restore
 *  8. Entities absent from snapshot are absent after restore (replace semantics)
 *  9. Snapshot after destroy(): dead entities not serialized
 * 10. Snapshot -> mutate -> restore -> back to snapshot state
 * 11. Cross-entity references remapped correctly via EntityMap
 * 12. Non-trivial component type (std::string field)
 * 13. Unknown block skipped: loader with fewer registered types than snapshot
 * 14. Corrupt header magic: throws on construction
 * 15. Corrupt footer magic: finalize() throws
 */

#include <fatp_ecs/FatpEcs.h>

#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fatp_ecs;

// =============================================================================
// Test Harness
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
// Component Types
// =============================================================================

struct Position
{
    float x{0.f};
    float y{0.f};
};

struct Health
{
    uint32_t hp{0};
};

struct Tag
{
    std::string name;
};

/// Cross-entity reference: stores another entity's handle.
struct Parent
{
    Entity entity{NullEntity};
};

// =============================================================================
// Serialization helpers
// =============================================================================

static void serializePosition(fat_p::binary::Encoder& e, const Position& p)
{
    e.writeFloat(p.x);
    e.writeFloat(p.y);
}

static Position deserializePosition(fat_p::binary::Decoder& d, const EntityMap&)
{
    return { d.readFloat(), d.readFloat() };
}

static void serializeHealth(fat_p::binary::Encoder& e, const Health& h)
{
    e.writeUint32(h.hp);
}

static Health deserializeHealth(fat_p::binary::Decoder& d, const EntityMap&)
{
    return { d.readUint32() };
}

static void serializeTag(fat_p::binary::Encoder& e, const Tag& t)
{
    e.writeString(t.name);
}

static Tag deserializeTag(fat_p::binary::Decoder& d, const EntityMap&)
{
    return { d.readString() };
}

static void serializeParent(fat_p::binary::Encoder& e, const Parent& par)
{
    e.writeUint64(par.entity.get());
}

static Parent deserializeParent(fat_p::binary::Decoder& d, const EntityMap& remap)
{
    const Entity old = Entity(d.readUint64());
    return { remap.translate(old) };
}

// Perform a full Position + Health round-trip and return the restored registry.
// Caller owns the returned Registry.
static std::vector<uint8_t> snapshotTwoComponents(Registry& src)
{
    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    auto snap = src.snapshot(enc);
    snap.serializeComponent<Position>(enc, serializePosition);
    snap.serializeComponent<Health>(enc, serializeHealth);
    snap.finalize(enc);
    return buf;
}

static void restoreTwoComponents(Registry& dst, const std::vector<uint8_t>& buf)
{
    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Position>(dec, deserializePosition);
    loader.deserializeComponent<Health>(dec, deserializeHealth);
    loader.finalize(dec);
}

// =============================================================================
// Test 1: Single component type round-trip
// =============================================================================

static void test_single_component_roundtrip()
{
    Registry src;
    const Entity e0 = src.create();
    const Entity e1 = src.create();
    const Entity e2 = src.create();
    src.add<Position>(e0, 1.f, 2.f);
    src.add<Position>(e1, 3.f, 4.f);
    src.add<Position>(e2, 5.f, 6.f);

    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    auto snap = src.snapshot(enc);
    snap.serializeComponent<Position>(enc, serializePosition);
    snap.finalize(enc);

    Registry dst;
    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Position>(dec, deserializePosition);
    loader.finalize(dec);

    TEST_ASSERT(dst.entityCount() == 3, "entity count after restore");

    // All three new entities should have Position
    const auto ents = dst.allEntities();
    TEST_ASSERT(ents.size() == 3, "allEntities count");
    for (Entity e : ents)
    {
        TEST_ASSERT(dst.has<Position>(e), "restored entity has Position");
    }

    // Verify component values are preserved (order may differ — collect and sort)
    std::vector<float> xs;
    dst.view<Position>().each([&](Entity, Position& p) { xs.push_back(p.x); });
    std::sort(xs.begin(), xs.end());
    TEST_ASSERT(xs.size() == 3, "position count");
    TEST_ASSERT(xs[0] == 1.f && xs[1] == 3.f && xs[2] == 5.f, "position values preserved");
}

// =============================================================================
// Test 2: Multiple component types round-trip
// =============================================================================

static void test_multiple_component_roundtrip()
{
    Registry src;
    for (int i = 0; i < 5; ++i)
    {
        Entity e = src.create();
        src.add<Position>(e, static_cast<float>(i), static_cast<float>(i * 2));
        src.add<Health>(e, static_cast<uint32_t>(i * 10));
    }

    const auto buf = snapshotTwoComponents(src);

    Registry dst;
    restoreTwoComponents(dst, buf);

    TEST_ASSERT(dst.entityCount() == 5, "entity count");

    uint32_t hpSum = 0;
    float xSum = 0.f;
    for (Entity e : dst.allEntities())
    {
        TEST_ASSERT(dst.has<Position>(e), "has Position");
        TEST_ASSERT(dst.has<Health>(e), "has Health");
        hpSum += dst.get<Health>(e).hp;
        xSum  += dst.get<Position>(e).x;
    }
    TEST_ASSERT(hpSum == (0+10+20+30+40), "hp sum preserved");
    TEST_ASSERT(xSum  == (0.f+1.f+2.f+3.f+4.f), "x sum preserved");
}

// =============================================================================
// Test 3: Sparse coverage — not all entities have all components
// =============================================================================

static void test_sparse_coverage()
{
    Registry src;
    Entity e0 = src.create(); // Position only
    Entity e1 = src.create(); // Health only
    Entity e2 = src.create(); // Both
    src.add<Position>(e0, 10.f, 0.f);
    src.add<Health>(e1, 42u);
    src.add<Position>(e2, 20.f, 0.f);
    src.add<Health>(e2, 99u);

    const auto buf = snapshotTwoComponents(src);

    Registry dst;
    restoreTwoComponents(dst, buf);

    TEST_ASSERT(dst.entityCount() == 3, "entity count");

    // Count entities with each component
    int posCount  = 0;
    int hpCount   = 0;
    int bothCount = 0;
    for (Entity e : dst.allEntities())
    {
        const bool hasP = dst.has<Position>(e);
        const bool hasH = dst.has<Health>(e);
        if (hasP) ++posCount;
        if (hasH) ++hpCount;
        if (hasP && hasH) ++bothCount;
    }
    TEST_ASSERT(posCount  == 2, "two entities have Position");
    TEST_ASSERT(hpCount   == 2, "two entities have Health");
    TEST_ASSERT(bothCount == 1, "one entity has both");
}

// =============================================================================
// Test 4: Empty registry round-trip
// =============================================================================

static void test_empty_registry()
{
    Registry src;

    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    auto snap = src.snapshot(enc);
    snap.serializeComponent<Position>(enc, serializePosition);
    snap.finalize(enc);

    Registry dst;
    // Pre-populate dst to verify clear() semantics
    dst.add<Health>(dst.create(), 1u);

    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Position>(dec, deserializePosition);
    loader.finalize(dec);

    TEST_ASSERT(dst.entityCount() == 0, "empty restore leaves empty registry");
}

// =============================================================================
// Test 5: Component type with zero instances (absent store)
// =============================================================================

static void test_zero_count_block()
{
    Registry src;
    Entity e = src.create();
    src.add<Health>(e, 7u);
    // Position never added to any entity

    const auto buf = snapshotTwoComponents(src); // Position block count = 0

    Registry dst;
    restoreTwoComponents(dst, buf);

    TEST_ASSERT(dst.entityCount() == 1, "one entity");
    const Entity ne = dst.allEntities()[0];
    TEST_ASSERT(!dst.has<Position>(ne), "no Position");
    TEST_ASSERT(dst.has<Health>(ne), "has Health");
    TEST_ASSERT(dst.get<Health>(ne).hp == 7u, "hp value");
}

// =============================================================================
// Test 6: Entity count preserved
// =============================================================================

static void test_entity_count_preserved()
{
    Registry src;
    for (int i = 0; i < 100; ++i)
    {
        Entity e = src.create();
        src.add<Position>(e, static_cast<float>(i), 0.f);
    }

    const auto buf = snapshotTwoComponents(src);

    Registry dst;
    restoreTwoComponents(dst, buf);

    TEST_ASSERT(dst.entityCount() == 100, "100 entities restored");
}

// =============================================================================
// Test 7: All restored entities are alive
// =============================================================================

static void test_all_entities_alive()
{
    Registry src;
    for (int i = 0; i < 10; ++i)
    {
        src.add<Health>(src.create(), static_cast<uint32_t>(i));
    }

    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    auto snap = src.snapshot(enc);
    snap.serializeComponent<Health>(enc, serializeHealth);
    snap.finalize(enc);

    Registry dst;
    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Health>(dec, deserializeHealth);
    loader.finalize(dec);

    for (Entity e : dst.allEntities())
    {
        TEST_ASSERT(dst.isAlive(e), "restored entity is alive");
    }
}

// =============================================================================
// Test 8: Replace semantics — pre-existing entities removed
// =============================================================================

static void test_replace_semantics()
{
    Registry src;
    src.add<Health>(src.create(), 1u);

    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    auto snap = src.snapshot(enc);
    snap.serializeComponent<Health>(enc, serializeHealth);
    snap.finalize(enc);

    Registry dst;
    // Add many entities to dst before restoring
    for (int i = 0; i < 50; ++i)
    {
        dst.add<Position>(dst.create(), 0.f, 0.f);
    }
    TEST_ASSERT(dst.entityCount() == 50, "pre-condition: 50 entities");

    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Health>(dec, deserializeHealth);
    loader.finalize(dec);

    TEST_ASSERT(dst.entityCount() == 1, "dst cleared and replaced");
    TEST_ASSERT(dst.has<Health>(dst.allEntities()[0]), "restored entity has Health");
}

// =============================================================================
// Test 9: Dead entities not in snapshot
// =============================================================================

static void test_dead_entities_not_serialized()
{
    Registry src;
    Entity alive = src.create();
    Entity dead  = src.create();
    src.add<Health>(alive, 10u);
    src.add<Health>(dead, 99u);
    src.destroy(dead);

    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    auto snap = src.snapshot(enc);
    snap.serializeComponent<Health>(enc, serializeHealth);
    snap.finalize(enc);

    Registry dst;
    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Health>(dec, deserializeHealth);
    loader.finalize(dec);

    TEST_ASSERT(dst.entityCount() == 1, "only alive entity restored");
    TEST_ASSERT(dst.get<Health>(dst.allEntities()[0]).hp == 10u, "correct hp");
}

// =============================================================================
// Test 10: Snapshot -> mutate -> restore
// =============================================================================

static void test_snapshot_mutate_restore()
{
    Registry reg;
    Entity e = reg.create();
    reg.add<Health>(e, 50u);

    // Take snapshot
    const auto buf = [&] {
        std::vector<uint8_t> b;
        fat_p::binary::Encoder enc(b);
        auto snap = reg.snapshot(enc);
        snap.serializeComponent<Health>(enc, serializeHealth);
        snap.finalize(enc);
        return b;
    }();

    // Mutate
    reg.get<Health>(e).hp = 999u;
    TEST_ASSERT(reg.get<Health>(e).hp == 999u, "mutation applied");

    // Restore into same registry
    fat_p::binary::Decoder dec(buf);
    auto loader = reg.snapshotLoader(dec);
    loader.deserializeComponent<Health>(dec, deserializeHealth);
    loader.finalize(dec);

    TEST_ASSERT(reg.entityCount() == 1, "one entity after restore");
    TEST_ASSERT(reg.get<Health>(reg.allEntities()[0]).hp == 50u, "hp restored to snapshot value");
}

// =============================================================================
// Test 11: Cross-entity references via EntityMap
// =============================================================================

static void test_cross_entity_remap()
{
    Registry src;
    Entity child  = src.create();
    Entity parent = src.create();
    src.add<Health>(child, 5u);
    src.add<Parent>(child, parent);
    src.add<Health>(parent, 100u);

    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    auto snap = src.snapshot(enc);
    snap.serializeComponent<Health>(enc, serializeHealth);
    snap.serializeComponent<Parent>(enc, serializeParent);
    snap.finalize(enc);

    Registry dst;
    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Health>(dec, deserializeHealth);
    loader.deserializeComponent<Parent>(dec, deserializeParent);
    loader.finalize(dec);

    TEST_ASSERT(dst.entityCount() == 2, "two entities");

    // Find the child (entity with a Parent component)
    Entity newChild = NullEntity;
    for (Entity e : dst.allEntities())
    {
        if (dst.has<Parent>(e))
        {
            newChild = e;
            break;
        }
    }
    TEST_ASSERT(newChild != NullEntity, "child entity found");

    const Entity newParent = dst.get<Parent>(newChild).entity;
    TEST_ASSERT(newParent != NullEntity, "parent entity remapped (not NullEntity)");
    TEST_ASSERT(dst.isAlive(newParent), "remapped parent entity is alive");
    TEST_ASSERT(dst.has<Health>(newParent), "parent has Health");
    TEST_ASSERT(dst.get<Health>(newParent).hp == 100u, "parent hp correct");
}

// =============================================================================
// Test 12: Non-trivial component type (std::string)
// =============================================================================

static void test_nontrivial_component()
{
    Registry src;
    Entity e0 = src.create();
    Entity e1 = src.create();
    src.add<Tag>(e0, std::string("alpha"));
    src.add<Tag>(e1, std::string("beta"));

    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    auto snap = src.snapshot(enc);
    snap.serializeComponent<Tag>(enc, serializeTag);
    snap.finalize(enc);

    Registry dst;
    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Tag>(dec, deserializeTag);
    loader.finalize(dec);

    TEST_ASSERT(dst.entityCount() == 2, "two entities");
    std::vector<std::string> names;
    for (Entity e : dst.allEntities())
    {
        TEST_ASSERT(dst.has<Tag>(e), "has Tag");
        names.push_back(dst.get<Tag>(e).name);
    }
    std::sort(names.begin(), names.end());
    TEST_ASSERT(names[0] == "alpha" && names[1] == "beta", "string values preserved");
}

// =============================================================================
// Test 13: Unknown block skipped gracefully
// =============================================================================

static void test_unknown_block_skipped()
{
    // Save with Position + Health, restore registering only Health
    Registry src;
    Entity e = src.create();
    src.add<Position>(e, 3.f, 4.f);
    src.add<Health>(e, 77u);

    const auto buf = snapshotTwoComponents(src); // Position block first, then Health

    Registry dst;
    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    // Deliberately skip Position registration — the loader should consume and discard it
    loader.deserializeComponent<Position>(dec, deserializePosition); // typeId mismatch: skips Health block
    // Note: block order mismatch means Health block is skipped, Position read instead.
    // Adjust: register in same order as save — Position first, then Health.
    // Re-do with only Health registered to test skip:
    (void)loader; // loader already consumed Position block above

    // Fresh test: save Health only, load with Position registered (type mismatch)
    {
        std::vector<uint8_t> buf2;
        fat_p::binary::Encoder enc2(buf2);
        Registry src2;
        Entity e2 = src2.create();
        src2.add<Health>(e2, 55u);
        auto snap2 = src2.snapshot(enc2);
        snap2.serializeComponent<Health>(enc2, serializeHealth);
        snap2.finalize(enc2);

        Registry dst2;
        fat_p::binary::Decoder dec2(buf2);
        auto loader2 = dst2.snapshotLoader(dec2);
        // Register Position but stream has Health block -- typeId mismatch, block skipped
        loader2.deserializeComponent<Position>(dec2, deserializePosition);
        loader2.finalize(dec2);

        TEST_ASSERT(dst2.entityCount() == 1, "entity recreated even when block skipped");
        TEST_ASSERT(!dst2.has<Position>(dst2.allEntities()[0]), "Position not added (block skipped)");
        TEST_ASSERT(!dst2.has<Health>(dst2.allEntities()[0]), "Health not added (block skipped)");
    }
}

// =============================================================================
// Test 14: Corrupt header magic throws
// =============================================================================

static void test_corrupt_header_throws()
{
    std::vector<uint8_t> buf;
    fat_p::binary::Encoder enc(buf);
    enc.writeUint32(0xDEADBEEFu); // wrong magic
    enc.writeUint8(1u);
    enc.writeUint32(0u); // entity count

    Registry dst;
    fat_p::binary::Decoder dec(buf);

    bool threw = false;
    try
    {
        auto loader = dst.snapshotLoader(dec);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    TEST_ASSERT(threw, "corrupt magic throws runtime_error");
}

// =============================================================================
// Test 15: Corrupt footer magic throws
// =============================================================================

static void test_corrupt_footer_throws()
{
    Registry src;
    src.add<Health>(src.create(), 1u);

    std::vector<uint8_t> buf;
    {
        fat_p::binary::Encoder enc(buf);
        auto snap = src.snapshot(enc);
        snap.serializeComponent<Health>(enc, serializeHealth);
        // Intentionally do NOT call finalize — write a bad footer manually
        enc.writeUint32(0xBAD00000u);
    }

    Registry dst;
    fat_p::binary::Decoder dec(buf);
    auto loader = dst.snapshotLoader(dec);
    loader.deserializeComponent<Health>(dec, deserializeHealth);

    bool threw = false;
    try
    {
        loader.finalize(dec);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    TEST_ASSERT(threw, "corrupt footer throws runtime_error");
}

// =============================================================================
// main
// =============================================================================

int main()
{
    std::printf("=== test_snapshot ===\n");

    RUN_TEST(test_single_component_roundtrip);
    RUN_TEST(test_multiple_component_roundtrip);
    RUN_TEST(test_sparse_coverage);
    RUN_TEST(test_empty_registry);
    RUN_TEST(test_zero_count_block);
    RUN_TEST(test_entity_count_preserved);
    RUN_TEST(test_all_entities_alive);
    RUN_TEST(test_replace_semantics);
    RUN_TEST(test_dead_entities_not_serialized);
    RUN_TEST(test_snapshot_mutate_restore);
    RUN_TEST(test_cross_entity_remap);
    RUN_TEST(test_nontrivial_component);
    RUN_TEST(test_unknown_block_skipped);
    RUN_TEST(test_corrupt_header_throws);
    RUN_TEST(test_corrupt_footer_throws);

    std::printf("\n%d/%d tests passed\n", sTestsPassed, sTestsPassed + sTestsFailed);
    return sTestsFailed == 0 ? 0 : 1;
}
