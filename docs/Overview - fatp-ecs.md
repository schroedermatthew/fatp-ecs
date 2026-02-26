---
doc_id: OV-FATPECS-001
doc_type: "Overview"
title: "fatp-ecs"
fatp_components: ["SparseSet", "SlotMap", "FastHashMap", "SmallVector", "Signal", "ThreadPool", "BitSet", "WorkQueue", "ObjectPool", "StringPool", "FlatMap", "JsonLite", "StateMachine", "FeatureManager", "CheckedArithmetic", "AlignedVector", "LockFreeQueue", "CircularBuffer", "StrongId"]
topics: ["entity component system", "ECS architecture", "EnTT compatibility", "component storage", "sparse set iteration", "generational entity IDs", "signal-based lifecycle events", "parallel system execution", "snapshot serialization", "command buffer deferral"]
constraints: ["cache-friendly component layout", "ABA prevention in entity reuse", "virtual dispatch in hot loops", "safe mutation during iteration", "cross-entity reference integrity"]
cxx_standard: "C++20"
std_equivalent: null
boost_equivalent: null
build_modes: ["Debug", "Release"]
last_verified: "2026-02-25"
audience: ["C++ developers", "game developers", "systems programmers", "AI assistants"]
status: "reviewed"
---

# Overview - fatp-ecs

*FAT-P ECS — February 2026*

---

## Executive Summary

fatp-ecs is a header-only Entity Component System built from 19 FAT-P components. It targets the EnTT API—the industry-standard C++ ECS—so code written against EnTT compiles against fatp-ecs with namespace and header substitutions. On benchmark workloads at one million entities, fatp-ecs is faster than EnTT-64 across every measured operation except single-component add, where the delta traces entirely to always-on lifecycle events.

The thesis behind the project is compositional: an ECS is a non-trivial software system, and building one from production-quality primitives rather than from scratch produces a better result. FAT-P's SparseSet, SlotMap, Signal, and ThreadPool weren't designed for an ECS; they were designed to be independently correct under adversarial conditions. The ECS is what happens when you compose them.

---

## Overview Card

**Component:** fatp-ecs  
**Problem solved:** Structured, cache-friendly entity and component management for real-time simulations, games, and data-intensive systems  
**When to use:** You need high-performance iteration over large sets of heterogeneous entities; you're migrating from EnTT; you want a complete ECS with lifecycle events, snapshots, parallel execution, and gameplay utilities from a single header-only library  
**When NOT to use:** You need object-oriented inheritance hierarchies; your component count is small enough that `std::vector<std::tuple<...>>` suffices; you need introspection or scripting-language bindings that require runtime reflection  
**Key guarantee:** Full 64-bit entity IDs with generational ABA protection; EnTT-compatible surface API; O(1) add, remove, and get; cache-linear dense-array iteration  
**std equivalent:** None. No standard equivalent exists or is planned.  
**Boost equivalent:** None.  
**Other alternatives:** EnTT (skypjack/entt), flecs, EntityX  
**Read next:** User Manual - fatp-ecs, Companion Guide - fatp-ecs

---

## The Problem Domain

### Why ECS Exists

Game and simulation engines began with object-oriented designs. A `Character` class had virtual `update()`, `draw()`, and `collide()` methods. Derived classes—`Player`, `Enemy`, `Projectile`—overrode the methods they needed. The design was expressive.

Performance ruined it.

A virtual dispatch resolves a function pointer at runtime—it's a branch the CPU cannot predict far in advance and a memory load from the vtable. With two thousand enemies calling `update()` every frame, that's two thousand unpredictable branches. The CPU's out-of-order execution engine stalls. The instruction cache fills with code from different derived types. The data is scattered across the heap because each `new Enemy()` goes wherever the allocator finds space.

Profilers showed developers spending 60–80% of frame time in memory system overhead—not in gameplay logic.

The ECS pattern inverts the design. Entities are not objects; they are IDs. Components are not objects; they are plain data associated with an ID. Systems are not methods; they are loops that operate on all entities possessing a given set of components. The classic form:

```
Entity 42: Position(10, 5), Velocity(1, 0), Health(100)
Entity 43: Position(20, 8), Velocity(-1, 0)
Entity 44: Position(5, 3),  Health(50)
```

A physics system iterates all entities with Position and Velocity. The components live in dense arrays—all Position values packed together, all Velocity values packed together. The iteration is cache-linear. No virtual dispatch. No heap scatter.

### Why EnTT Became the Standard

The open-source ECS ecosystem converged on EnTT (by Michele "skypjack" Caini) as the reference implementation for two reasons: it achieves near-theoretical-maximum iteration throughput, and it exposed a clear API that developers could write production code against.

EnTT's key architectural insight is the sparse-set component store. Each component type gets its own sparse set: a sparse array of size max_entities that maps entity IDs to dense indices, plus a dense array of the components themselves. Iteration is a loop over the dense array—sequential memory access, hardware-prefetchable, branch-free after the setup cost.

EnTT exists in two configurations: 32-bit entity IDs (16 bits index, 16 bits generation) and 64-bit entity IDs (32 bits each). The 32-bit variant is the default for legacy compatibility; the 64-bit variant matches fatp-ecs's layout.

### What fatp-ecs Adds

fatp-ecs matches the EnTT-64 API and exceeds EnTT-64 performance in most operations, but that's table stakes. The substantive additions are:

**Always-on lifecycle events.** Every `add<T>()` fires `onComponentAdded<T>`. Every `remove<T>()` fires `onComponentRemoved<T>`. Every `patch<T>()` fires `onComponentUpdated<T>`. In EnTT, you opt in. In fatp-ecs, you can't opt out. The ECS event system costs ~3–4 ns per add on GCC but enables Observers, reactive systems, and debugging instrumentation without post-hoc retrofitting.

**Snapshot serialization.** Full registry save and restore with a binary wire format and entity-remapping through `EntityMap`. Snapshots support deterministic simulation replay, network state synchronization, and save games.

**Process scheduler.** A coroutine-style process system (distinct from the parallel system Scheduler) for multi-frame behaviors—animations, cutscenes, cooldowns—that span many ticks without per-frame state machine plumbing.

**Gameplay math.** `SafeMath` provides overflow-safe health, damage, and score arithmetic with saturation semantics. No undefined behavior when a buff doubles a character's already-maximum health.

**Entity templates.** JSON-driven entity blueprints through `TemplateRegistry`. Component factories map JSON keys to component construction. `templates.spawn(registry, "goblin")` creates a fully-formed entity from data, not code.

**Entity names.** `EntityNames` provides string-to-entity mapping with O(1) lookup via FAT-P's `StringPool`—useful for editor tools and debug overlays.

---

## Architecture: FAT-P as a Component Toolkit

Each FAT-P component maps to an ECS responsibility:

| FAT-P Component | ECS Role |
|---|---|
| **StrongId** | Type-safe 64-bit Entity handles (index + generation packed into one `uint64_t`) |
| **SlotMap** | Entity allocator with generational ABA safety; `insert_at()` enables hint-based create for snapshot restore |
| **SparseSetWithData** | Per-component-type dense storage: O(1) add/remove/get, cache-linear iteration |
| **FastHashMap** | Type-erased component store registry, keyed by `TypeId` |
| **SmallVector** | Stack-allocated scratch space for query results |
| **Signal** | Observer pattern for entity and component lifecycle events (via `EventBus`) |
| **ThreadPool** | Work-stealing parallel system execution (via `Scheduler`) |
| **BitSet** | Component masks for dependency analysis in the parallel `Scheduler` |
| **WorkQueue** | Job dispatch inside `ThreadPool` |
| **ObjectPool** | Per-frame temporary allocator with bulk reset (via `FrameAllocator`) |
| **StringPool** | Interned entity names: pointer equality for O(1) name comparison |
| **FlatMap** | Sorted name-to-entity mapping for debug tools and editors |
| **JsonLite** | Data-driven entity template definitions |
| **StateMachine** | Compile-time AI state machines with context binding |
| **FeatureManager** | Runtime system enable/disable toggles |
| **CheckedArithmetic** | Overflow-safe gameplay math (health, damage, score) |
| **AlignedVector** | SIMD-friendly aligned component storage (via `AlignedStoragePolicy`) |
| **LockFreeQueue** | Thread-safe command buffer recording for `ParallelCommandBuffer` |
| **CircularBuffer** | Deferred command queues |

The components weren't designed for an ECS. They were designed to be independently correct and useful. The ECS is the composition.

---

## Feature Inventory

### Registry

The `Registry` is the central coordinator. It owns the entity allocator, the per-component-type stores, and the event bus. All ECS operations go through a `Registry` instance.

Entity IDs are 64-bit values packing a 32-bit slot index (lower) and a 32-bit generation counter (upper). The generation makes every entity ID globally unique across the lifetime of the program: when slot 42 is destroyed and a new entity is allocated in that slot, the new entity's generation is different from the old one's. Any reference to the old entity fails `isAlive()` because the generation no longer matches.

```cpp
Registry registry;

Entity player = registry.create();
registry.add<Position>(player, 0.f, 0.f);
registry.add<Health>(player, 100);

bool alive = registry.valid(player);      // true
bool hasPos = registry.has<Position>(player); // true
```

Component stores are created lazily: the first `add<T>()` for a given type T allocates the store. The registry maintains a flat-array cache for the 64 most common TypeIDs so that repeated store lookups in hot loops pay one array index rather than a hash map probe.

### Views

A `View` is the primary iteration mechanism. It holds pointers to the component stores for the requested types, selects the smallest store as the pivot, and probes the others for intersection. Iteration is over the pivot's dense array—sequential in memory.

```cpp
// Every entity with both Position and Velocity
registry.view<Position, Velocity>().each(
    [](Entity e, Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
    });

// Exclude entities with Frozen
registry.view<Position>(Exclude<Frozen>{}).each(
    [](Entity e, Position& pos) { /* ... */ });
```

Views with exclude filters check excluded types per entity using the same sparse-array probe as include checks. If a type has never been registered (no entity ever had it), the exclude check trivially passes.

An important Clang optimization applies in `View.h`: before the iteration loop, raw pointers are extracted from every non-pivot store into local stack variables. This prevents Clang's alias analysis from reloading store metadata on each iteration, which otherwise adds 8–10 extra loads per entity in the 2- and 3-component cases.

### Owning Groups

Groups enforce a sort invariant on two or more component types. When you call `registry.group<Position, Velocity>()`, the registry sorts both stores so that entities with both types share the same dense-array prefix. The group iterates that prefix—all entities present in both stores, packed contiguously.

```cpp
auto& grp = registry.group<Position, Velocity>();
grp.each([](Entity e, Position& p, Velocity& v) { /* ... */ });
```

Groups have stricter requirements than views: a component type can belong to at most one owning group. The registry asserts this at creation time. For simpler use cases, non-owning groups provide the same iteration interface without the ownership constraint.

### Lifecycle Signals

The `EventBus` exposes per-component-type signals via EnTT-compatible accessors:

```cpp
auto c = registry.on_construct<Health>().connect(
    [](Entity e, Health& h) { h.armor = 5; });  // Default armor on spawn

registry.on_destroy<Health>().connect(
    [](Entity e) { /* cleanup */ });

registry.on_update<Health>().connect(
    [](Entity e, Health& h) { /* react to damage */ });
```

Unlike EnTT's opt-in design, fatp-ecs fires the added/removed/updated signals unconditionally on every corresponding operation. The cost is the signal emission check (one comparison against a slot count). When no listeners are connected, the check is a single branch that the CPU predicts correctly after the first iteration.

### Observers

An `Observer` accumulates entities that become dirty between frames. The dirty set is a `SparseSet<Entity>` that deduplicates naturally: an entity patched ten times in one frame appears once. Stale entity cleanup is automatic: the observer connects to `onEntityDestroyed` and removes destroyed entities from the dirty set immediately.

```cpp
auto obs = registry.observe(OnAdded<Position>{}, OnAdded<Velocity>{});

// Each frame
obs.each([](Entity e) {
    // e now has both Position and Velocity; react to this new state
});
obs.clear();
```

### Command Buffer

`CommandBuffer` records create, destroy, add, and remove operations as deferred commands. This solves a fundamental problem: it is unsafe to modify the stores being iterated inside a `view.each()` loop. The command buffer accumulates mutations and applies them all at once after the iteration completes.

```cpp
CommandBuffer cmd;
registry.view<Health>().each([&](Entity e, Health& hp) {
    if (hp.current <= 0)
        cmd.destroy(e);
});
cmd.flush(registry);
```

For parallel contexts, `ParallelCommandBuffer` provides a mutex-protected variant that multiple threads can record into simultaneously.

### Snapshot

`RegistrySnapshot` serializes the live entity set and registered component stores to a binary buffer. `RegistrySnapshotLoader` deserializes it into a cleared registry, recreating entities via hint-based `create()` so that entity IDs are preserved where slot availability permits. Cross-entity component references are remapped through `EntityMap`.

```cpp
// Save
RegistrySnapshot snap(registry);
snap.serializeComponent<Position>(enc,
    [](fat_p::binary::Encoder& e, const Position& p) {
        e.write(p.x); e.write(p.y);
    });
auto buffer = snap.finalize();

// Load
RegistrySnapshotLoader loader(registry);
loader.deserializeComponent<Position>(dec,
    [](fat_p::binary::Decoder& d, const EntityMap&) -> Position {
        float x = d.read<float>(), y = d.read<float>();
        return {x, y};
    });
loader.apply();
```

### Parallel Scheduler

The `Scheduler` manages system registration and parallel execution. Each system declares read and write component masks. The scheduler computes conflict relationships (a write mask overlapping any other system's read or write mask constitutes a conflict) and runs non-conflicting systems concurrently on FAT-P's `ThreadPool`.

```cpp
Scheduler scheduler(4);  // 4 worker threads

scheduler.addSystem("Physics",
    [](Registry& r) { /* position += velocity */ },
    makeComponentMask<Position>(),   // writes
    makeComponentMask<Velocity>());  // reads

scheduler.addSystem("Render",
    [](Registry& r) { /* read position, write draw call */ },
    makeComponentMask<DrawCall>(),   // writes
    makeComponentMask<Position>());  // reads

scheduler.run(registry);
// Physics and Render can run concurrently (non-conflicting)
```

### Process Scheduler

The `ProcessScheduler` manages multi-frame processes—behaviors that evolve over multiple ticks. A process is a CRTP class with `onInit()`, `update(delta)`, `onSucceed()`, `onFail()`, and `onAbort()` callbacks. Processes chain: when one succeeds, its successor runs automatically.

```cpp
struct WaitProcess : fatp_ecs::Process<WaitProcess, float> {
    float elapsed = 0;
    void update(float dt) {
        elapsed += dt;
        if (elapsed >= 2.0f) succeed();
    }
};

struct AttackProcess : fatp_ecs::Process<AttackProcess, float> {
    void onInit() { /* start attack animation */ }
    void update(float dt) { /* check if animation done */ }
};

ProcessScheduler ps;
auto h = ps.launch<WaitProcess>();
h.then<AttackProcess>();  // AttackProcess runs after WaitProcess succeeds
ps.update(dt);
```

### Entity Handle

`Handle` bundles an entity and a registry pointer for ergonomic single-entity access. It mirrors `entt::handle`.

```cpp
Handle h = registry.handle(player);
h.add<Shield>(100);
h.patch<Health>([](Health& hp) { hp.current -= 10; });
if (h.has<Frozen>()) h.remove<Frozen>();
h.destroy();
```

### Context Storage

The registry stores singleton-like context objects—shared state that systems need but that isn't a component.

```cpp
registry.emplace_context<PhysicsConfig>(9.81f, 0.016f);

// In any system
auto& cfg = registry.ctx<PhysicsConfig>();
if (auto* p = registry.try_ctx<RenderConfig>()) { /* present */ }
```

### Storage Policies

`ComponentStore<T>` accepts a policy template parameter that controls the underlying dense-array container:

```cpp
// Default: std::vector
registry.add<Position>(e, 0.f, 0.f);

// SIMD-aligned storage for vectorizable components
// (set via ComponentStore template parameter)
ComponentStore<SimdFloat4, AlignedStoragePolicy<16>> store;
```

Built-in policies: `DefaultStoragePolicy` (std::vector), `AlignedStoragePolicy<N>` (AlignedVector with N-byte alignment), `ConcurrentStoragePolicy<Lock>` (mutex-protected writes).

---

## Performance

fatp-ecs uses 64-bit entity IDs throughout. All comparisons are against EnTT-64. Benchmarks run on GitHub Actions, GCC-14, N=1M entities (Fragmented and Churn at N=100K), statistical reporting (median of 20 batches).

| Category | fatp-ecs | EnTT-64 | Ratio |
|---|---|---|---|
| Create entities | 7.94 ns | 12.73 ns | **0.62×** |
| Destroy entities | 8.91 ns | 12.86 ns | **0.69×** |
| Add 1 component | 18.95 ns | 13.68 ns | 1.39× |
| Add 3 components | 45.77 ns | 41.08 ns | 1.11× |
| Remove component | 5.83 ns | 15.17 ns | **0.38×** |
| Get component | 3.13 ns | 4.87 ns | **0.64×** |
| 1-comp iteration | 0.68 ns | 0.93 ns | **0.73×** |
| 2-comp iteration | 1.35 ns | 4.13 ns | **0.33×** |
| Sparse iteration | 1.70 ns | 4.22 ns | **0.40×** |
| 3-comp iteration | 2.96 ns | 7.69 ns | **0.38×** |
| Fragmented iter | 0.64 ns | 0.95 ns | **0.67×** |
| Churn | 16.40 ns | 31.22 ns | **0.53×** |

**Bold** = fatp-ecs wins; ratio below 1.0× means fatp-ecs is faster by that factor.

The add-component overhead is deliberate: fatp-ecs fires `onComponentAdded<T>` on every `add()` unconditionally. The signal emission check costs ~3–4 ns on GCC. Everything else is faster.

The 2-comp and 3-comp iteration wins (0.33× and 0.38×) reflect the Clang alias-analysis fix in View.h: pre-caching store pointers into stack locals before the loop prevents per-iteration metadata reloads that the default implementation triggers.

---

## EnTT Migration Reference

fatp-ecs matches the EnTT registry API surface. The primary differences are structural:

| EnTT | fatp-ecs | Notes |
|---|---|---|
| `#include <entt/entt.hpp>` | `#include <fatp_ecs/FatpEcs.h>` | Single umbrella header |
| `entt::registry` | `fatp_ecs::Registry` | Same API |
| `entt::entity` | `fatp_ecs::Entity` | 64-bit, same packing |
| `entt::null` | `fatp_ecs::NullEntity` | Same sentinel semantics |
| `registry.view<Ts>(entt::exclude<Xs>)` | `registry.view<Ts>(Exclude<Xs>{})` | Syntax differs; semantics identical |
| `registry.emplace<T>()` | `registry.emplace<T>()` | Direct alias for `add<T>()` |
| `registry.on_construct<T>()` | `registry.on_construct<T>()` | Always connected; cannot opt out |
| Groups | Groups (owning + non-owning) | Same concepts, same constraints |

All other method names (`patch`, `erase`, `remove`, `get`, `try_get`, `get_or_emplace`, `valid`, `alive`, `each`, `orphans`, `clear`, `storage`, `create(hint)`) are identical.

---

## Integration Points

```
FatpEcs.h (umbrella)
    → Registry.h           Entity lifecycle, component storage, event bus
    → View.h               Cache-linear multi-component iteration
    → OwningGroup.h        Sort-invariant contiguous group iteration
    → NonOwningGroup.h     Group iteration without ownership constraints
    → Observer.h           Reactive dirty-set tracking
    → CommandBuffer.h      Deferred mutation during iteration
    → Snapshot.h           Binary serialization and restore
    → Scheduler.h          Parallel system execution
    → ProcessScheduler.h   Multi-frame process chains
    → EntityHandle.h       Entity+registry ergonomic wrapper
    → EntityTemplate.h     JSON-driven entity blueprints
    → EntityNames.h        String-to-entity name registry
    → SafeMath.h           Overflow-safe gameplay arithmetic
    → FeatureManager.h     Runtime system toggle flags
```

Requires C++20 and FAT-P as a sibling directory or via `FATP_INCLUDE_DIR`. Header-only; link only `-lpthread` for parallel features.

---

## Test Coverage

18 test suites, 539 tests, passing across the full CI matrix (GCC-12/13/14, Clang-16/17, MSVC C++20/23, AddressSanitizer, UBSan):

| Suite | Tests |
|---|---|
| Core ECS (phase 1) | 27 |
| Events & Parallelism (phase 2) | 37 |
| Gameplay Infrastructure (phase 3) | 28 |
| Exclude filters | 15 |
| Patch | 15 |
| Observer | 21 |
| OwningGroup | 16 |
| Sort | 15 |
| Snapshot | 15 |
| Handle | 20 |
| EntityCopy | 16 |
| ProcessScheduler | 20 |
| Clear stress | 138 |
| RuntimeView | 17 |
| StoragePolicy | 65 |
| New API | 16 |
| NonOwningGroup | 14 |
| EnTT parity | 44 |
| **Total** | **539** |

---

## Final Assessment

fatp-ecs answers a question that usually produces hand-waving: can you build a production-quality ECS entirely from composable primitives, without bespoke data structures?

The answer is yes, with measurable evidence. The FAT-P components were individually designed to be correct under adversarial conditions—ABA prevention, exception safety, cache awareness, SIMD-friendly layouts. Assembling them into an ECS produces a system that outperforms the industry reference on most operations, passes 539 tests across 12 CI configurations, and exposes an API that existing EnTT codebases can adopt without rewriting their application logic.

The iteration wins (0.33× to 0.40× of EnTT's cost for multi-component cases) are not micro-optimizations. They come from structural decisions: sparse-set intersection as the iteration strategy, and store-pointer caching to prevent Clang alias-analysis regressions. These decisions are documented, measured, and reproducible.

The add-component overhead (1.39×) is the cost of always-on events—a deliberate design choice, not a deficiency. Systems that need lifecycle hooks don't pay the discovery cost of finding them; they're already wired.

---

*fatp-ecs — built from FAT-P components*
