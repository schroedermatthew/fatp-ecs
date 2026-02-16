# FAT-P ECS Framework

A production-quality Entity Component System built entirely on [FAT-P](https://github.com/schroedermatthew/FatP) library components. Proves that 11 FAT-P components compose into a coherent, real-world framework.

## Quick Start

```cpp
#include "fatp_ecs/FatpEcs.h"
using namespace fatp_ecs;

Registry registry;

// Events
auto conn = registry.events().onEntityCreated.connect(
    [](Entity e) { /* ... */ });

// Create entities
Entity player = registry.create();
registry.add<Position>(player, 0.0f, 0.0f);
registry.add<Velocity>(player, 1.0f, 0.5f);
registry.add<Health>(player, 100, 100);

// Iterate with views
registry.view<Position, Velocity>().each(
    [](Entity e, Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
    });

// Deferred operations (safe during iteration)
CommandBuffer cmd;
registry.view<Health>().each([&](Entity e, Health& hp) {
    if (hp.current <= 0) cmd.destroy(e);
});
cmd.flush(registry);

// Parallel system execution
Scheduler scheduler(4);
scheduler.addSystem("Physics",
    [](Registry& r) { /* ... */ },
    makeComponentMask<Position>(),    // writes
    makeComponentMask<Velocity>());   // reads
scheduler.run(registry);
```

## Architecture

### Phase 1 — Core ECS
| FAT-P Component | ECS Role |
|---|---|
| **StrongId** | Type-safe Entity handles (64-bit: index + generation) |
| **SparseSetWithData** | O(1) component storage with cache-friendly dense iteration |
| **SlotMap** | Entity allocator with generational ABA safety |
| **FastHashMap** | Type-erased component store registry |
| **SmallVector** | Stack-allocated entity query results |

### Phase 2 — Events, Parallelism, Deferred Operations
| FAT-P Component | ECS Role |
|---|---|
| **Signal** | Observer pattern for entity/component lifecycle events |
| **ThreadPool** | Work-stealing parallel system execution |
| **BitSet** | Component masks for fast archetype matching and dependency analysis |
| **LockFreeQueue** | Thread-safe MPMC command buffer (via ParallelCommandBuffer) |
| **SparseSet::indexOf()** | Keeps parallel entity vectors in sync for generational safety |

Plus **WorkQueue** and **CircularBuffer** available for Phase 3 integration.

### Key Design Decisions

**Entity = 64-bit StrongId**: Lower 32 bits are the slot index, upper 32 are the generation counter. This enables O(1) ABA-safe entity validation.

**ComponentStore parallel entity vector**: SparseSet stores `uint32_t` indices in its dense array (losing generation info). ComponentStore maintains a parallel `std::vector<Entity>` with full 64-bit entities, kept in sync via `SparseSet::indexOf()` during swap-with-back erasure. Views read from this vector to yield correct generational Entity handles.

**Signal-based EventBus**: Lazy signal creation — no overhead for component types nobody listens to. ScopedConnection for automatic RAII cleanup.

**Scheduler dependency analysis**: Systems declare read/write ComponentMasks. Scheduler uses BitSet intersection to identify non-conflicting systems and runs them in parallel on the ThreadPool.

**CommandBuffer pattern**: Systems record structural mutations (create/destroy/add/remove) during iteration. Mutations are applied atomically between frames via `flush()`. ParallelCommandBuffer adds thread-safety for multi-threaded systems.

## Files

```
include/fatp_ecs/
├── Entity.h              — StrongId-based entity type
├── ComponentStore.h      — SparseSetWithData wrapper with entity tracking
├── ComponentMask.h       — BitSet-based component type masks
├── EventBus.h            — Signal-based lifecycle events
├── Registry.h            — Central coordinator (SlotMap + FastHashMap)
├── View.h                — Single/multi-component iteration
├── CommandBuffer.h       — Deferred operations (single + parallel)
├── CommandBuffer_Impl.h  — Registry-dependent implementations
├── Scheduler.h           — ThreadPool-based system execution
└── FatpEcs.h             — Umbrella header

tests/
├── test_ecs.cpp          — Phase 1 tests (27 tests)
└── test_ecs_phase2.cpp   — Phase 2 tests (37 tests)
```

## Building

Header-only. Just add the include paths:

```bash
g++ -std=c++20 -O2 -I include -I /path/to/FatP/include \
    your_code.cpp -lpthread
```

## Test Results

```
Phase 1: 27/27 passed ✓
Phase 2: 37/37 passed ✓
Total:   64/64 passed ✓
```

Compiled clean with `-Wall -Wextra -Wpedantic -Werror`.

## SparseSet Modification

Phase 2 exposed a real limitation: `SparseSetWithData::dense()` returns `uint32_t` indices, losing the Entity generation. The fix was adding `indexOf(T value)` to both `SparseSet` and `SparseSetWithData` — a 30-line non-breaking addition that exposes the sparse→dense mapping for callers maintaining parallel arrays.
