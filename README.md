# FAT-P ECS Framework

An Entity Component System framework built on top of [FAT-P](https://github.com/schroedermatthew/FatP), using existing FAT-P components for real architectural reasons.

## Status: Phase 1 — Core (MVP) ✅

**27/27 tests passing.** Entity lifecycle, component storage, single and multi-component views, generational safety, and 10K entity scale test all verified.

## Architecture

### Entity
A 64-bit value packing slot index (lower 32) and generation counter (upper 32) into a `fat_p::StrongId`. Type-safe — cannot be confused with raw integers or other ID types.

### Component Storage
Each component type `T` gets its own `fat_p::SparseSetWithData<uint32_t, T>`, giving O(1) add/remove/lookup and cache-friendly dense iteration.

### Registry
Central coordinator owning all entity lifecycle and component storage. Type-erased component stores live in a `fat_p::FastHashMap<TypeId, unique_ptr<IComponentStore>>`.

### Views
`View<Ts...>` iterates entities possessing all component types. Walks the smallest store, probes others via O(1) `contains()`. Single-component views iterate the dense array directly.

## FAT-P Components Used (Phase 1)

| Component | ECS Role |
|-----------|----------|
| `StrongId` | Type-safe Entity identifier |
| `SparseSetWithData` | Per-component-type storage |
| `SlotMap` | Entity allocator with generational ABA safety |
| `FastHashMap` | Type-erased component store registry |
| `SmallVector` | Return buffers for entity queries |

## Quick Start

```cpp
#include <fatp_ecs/FatpEcs.h>

struct Position { float x, y; };
struct Velocity { float dx, dy; };

fatp_ecs::Registry registry;

auto e = registry.create();
registry.add<Position>(e, 10.0f, 20.0f);
registry.add<Velocity>(e, 1.0f, -0.5f);

registry.view<Position, Velocity>().each(
    [](fatp_ecs::Entity entity, Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
    });

registry.destroy(e);
```

## Building

```bash
g++ -std=c++20 -I include -I /path/to/FatP/include tests/test_ecs.cpp -o test_ecs
./test_ecs
```

Or with CMake:
```bash
cmake -B build -DFATP_INCLUDE_DIR=/path/to/FatP/include
cmake --build build
ctest --test-dir build
```

## Phasing

- **Phase 1 (current):** Entity, Registry, SparseSet storage, Views ✅
- **Phase 2:** Signal events, ThreadPool parallel iteration, command buffers
- **Phase 3:** StateMachine AI, ObjectPool frame allocator, JSON templates
- **Phase 4:** Complete demo application
