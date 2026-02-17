# FAT-P ECS Framework

An Entity Component System framework built on [FAT-P](https://github.com/schroedermatthew/FatP), using 19 FAT-P components for real architectural reasons. Header-only, C++20, zero dependencies beyond FAT-P.

[![CI](https://github.com/schroedermatthew/fatp-ecs/actions/workflows/ci.yml/badge.svg)](https://github.com/schroedermatthew/fatp-ecs/actions/workflows/ci.yml)

## Quick Start

```cpp
#include <fatp_ecs/FatpEcs.h>
using namespace fatp_ecs;

Registry registry;

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

// Lifecycle events
auto conn = registry.events().onEntityCreated.connect(
    [](Entity e) { /* ... */ });

// Deferred operations (safe during iteration)
CommandBuffer cmd;
registry.view<Health>().each([&](Entity e, Health& hp) {
    if (hp.hp <= 0) cmd.destroy(e);
});
cmd.flush(registry);

// Parallel system execution
Scheduler scheduler(4);
scheduler.addSystem("Physics",
    [](Registry& r) { /* ... */ },
    makeComponentMask<Position>(),    // writes
    makeComponentMask<Velocity>());   // reads
scheduler.run(registry);

// Data-driven spawning from JSON templates
TemplateRegistry templates;
templates.registerComponent("Position", myPositionFactory);
templates.addTemplate("goblin", R"({
    "components": { "Position": {"x": 0, "y": 0}, "Health": {"current": 50, "max": 50} }
})");
Entity goblin = templates.spawn(registry, "goblin");

// Overflow-safe gameplay math
int hp = applyDamage(currentHp, damage, maxHp);  // clamped to [0, maxHp]
int score = addScore(currentScore, points);       // saturates at INT_MAX
```

## Building

Header-only. Requires C++20 and FAT-P as a sibling directory or via `FATP_INCLUDE_DIR`.

### Build Scripts

The easiest way to build everything:

**Windows (PowerShell):**
```powershell
.\build.ps1 -Clean              # full clean build with SDL2 visual demo
.\build.ps1 -NoVisual           # skip SDL2, terminal demo + tests only
.\build.ps1 -Debug              # debug build
```

**Windows (batch):**
```bat
build.bat clean                 :: full clean build with SDL2 visual demo
build.bat novisual              :: skip SDL2
build.bat debug                 :: debug build
```

**Linux / macOS (bash):**
```bash
./build.sh --clean              # full clean build
./build.sh --no-visual          # skip SDL2
./build.sh --debug              # debug build
```

### Manual CMake

**Terminal demo + tests only (no SDL2 required):**
```bash
cmake -B build -DFATP_INCLUDE_DIR=../FatP/include
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

**With SDL2 visual demo (Windows / vcpkg):**
```bash
cmake -B build -DFATP_INCLUDE_DIR=../FatP/include -DFATP_ECS_BUILD_VISUAL_DEMO=ON -DCMAKE_TOOLCHAIN_FILE="<vcpkg-root>/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

Run `vcpkg integrate install` to find your toolchain path. SDL2 and SDL2_ttf are installed automatically from `vcpkg.json`.

**With SDL2 visual demo (Linux):**
```bash
sudo apt install libsdl2-dev libsdl2-ttf-dev
cmake -B build -DFATP_INCLUDE_DIR=../FatP/include -DFATP_ECS_BUILD_VISUAL_DEMO=ON
cmake --build build
```

**Direct compilation (no CMake):**
```bash
g++ -std=c++20 -O2 -I include -I /path/to/FatP/include your_code.cpp -lpthread
```

### CMake Options

| Option | Default | Description |
|---|---|---|
| `FATP_INCLUDE_DIR` | auto-detect | Path to FAT-P include directory |
| `FATP_ECS_BUILD_TESTS` | `ON` | Build test executables |
| `FATP_ECS_BUILD_DEMO` | `ON` | Build terminal demo |
| `FATP_ECS_BUILD_VISUAL_DEMO` | `OFF` | Build SDL2 visual demo (requires SDL2, SDL2_ttf) |

## Demo

### Terminal Demo

Runs a headless space battle simulation exercising all 19 FAT-P components:

```bash
build/Release/demo.exe
build/Release/demo.exe --wave-size 100 --turrets 8 --frames 500
```

### Visual Demo (SDL2)

Real-time rendering of the space battle. The actual C++ ECS ticks every frame — SDL2 draws the result. Frame time shown is the real end-to-end cost.

```bash
build/Release/visual_demo.exe
build/Release/visual_demo.exe --wave-size 100 --turrets 8
```

**Controls:**

| Key | Action |
|---|---|
| Space | Pause / resume |
| 1 / 2 / 3 | Speed 1x / 2x / 5x |
| F | Toggle vsync (capped 60fps vs uncapped) |
| R | Reset simulation |
| + / - | Increase / decrease wave size |
| Escape | Quit |

## FAT-P Component Usage

19 FAT-P components integrated with architectural justification:

| FAT-P Component | ECS Role | Phase |
|---|---|---|
| **StrongId** | Type-safe Entity handles (64-bit: index + generation) | 1 |
| **SparseSetWithData** | O(1) component storage with cache-friendly dense iteration | 1 |
| **SlotMap** | Entity allocator with generational ABA safety | 1 |
| **FastHashMap** | Type-erased component store registry | 1 |
| **SmallVector** | Stack-allocated entity query results | 1 |
| **Signal** | Observer pattern for entity/component lifecycle events | 2 |
| **ThreadPool** | Work-stealing parallel system execution | 2 |
| **BitSet** | Component masks for archetype matching and dependency analysis | 2 |
| **WorkQueue** | Job dispatch (via ThreadPool internals) | 2 |
| **ObjectPool** | Per-frame temporary allocator with bulk reset | 3 |
| **StringPool** | Interned entity names for pointer-equality comparison | 3 |
| **FlatMap** | Sorted name-to-entity mapping for debug/editor tools | 3 |
| **JsonLite** | Data-driven entity template definitions | 3 |
| **StateMachine** | Compile-time AI state machines with context binding | 3 |
| **FeatureManager** | Runtime system enable/disable toggles | 3 |
| **CheckedArithmetic** | Overflow-safe health/damage/score calculations | 3 |
| **AlignedVector** | SIMD-friendly aligned component storage | 3 |
| **LockFreeQueue** | Thread-safe parallel command buffer | 2 |
| **CircularBuffer** | Available for deferred command queues | — |

## Architecture

**Entity** is a 64-bit StrongId. Lower 32 bits are the slot index, upper 32 are the generation counter. This enables O(1) ABA-safe entity validation via the SlotMap allocator.

**ComponentStore\<T\>** wraps SparseSetWithData with an EntityIndex policy that extracts the 32-bit slot index for sparse array indexing while storing full 64-bit entities in the dense array.

**View\<Ts...\>** iterates the intersection of component stores. Compile-time pivot dispatch selects the smallest store as the iteration driver, probing the others with O(1) `has()`.

**EventBus** provides Signal-based lifecycle events (onEntityCreated, onEntityDestroyed, onComponentAdded\<T\>, onComponentRemoved\<T\>). Lazy signal creation means no overhead for unobserved component types.

**Scheduler** analyzes system read/write ComponentMasks via BitSet intersection to identify non-conflicting systems and runs them in parallel on the ThreadPool.

**CommandBuffer** records structural mutations during iteration. Flush applies them atomically between frames. ParallelCommandBuffer adds mutex protection for multi-threaded systems.

**FrameAllocator** wraps ObjectPool for per-frame temporary allocations (collision pairs, spatial query results) with O(1) acquire and bulk releaseAll().

**EntityNames** provides bidirectional name-to-entity mapping with StringPool interning and FlatMap sorted iteration.

**TemplateRegistry** parses JSON entity definitions via JsonLite and stamps out entities through registered ComponentFactory callbacks.

**SystemToggle** wraps FeatureManager for runtime system enable/disable without recompilation.

**SafeMath** provides clamped arithmetic via CheckedArithmetic with gameplay helpers (applyDamage, applyHealing, addScore) that saturate instead of overflowing.

## Files

```
include/fatp_ecs/
├── Entity.h              — StrongId-based entity type + EntityIndex policy
├── TypeId.h              — Atomic compile-time type-to-integer mapping
├── ComponentMask.h       — BitSet wrapper for archetype matching
├── ComponentStore.h      — SparseSetWithData wrapper with entity tracking
├── EventBus.h            — Signal-based lifecycle events
├── Registry.h            — Central coordinator (SlotMap + FastHashMap)
├── View.h                — Multi-component iteration with pivot dispatch
├── CommandBuffer.h       — Deferred operations (single + parallel)
├── CommandBuffer_Impl.h  — Registry-dependent implementations
├── Scheduler.h           — ThreadPool-based parallel system execution
├── FrameAllocator.h      — ObjectPool-backed per-frame temp allocator
├── EntityNames.h         — StringPool + FlatMap entity naming
├── EntityTemplate.h      — JsonLite-driven entity spawning
├── EntityTemplate_Impl.h — Registry-dependent template implementations
├── SystemToggle.h        — FeatureManager-backed system toggles
├── SafeMath.h            — CheckedArithmetic gameplay math
└── FatpEcs.h             — Umbrella header

demo/
├── Simulation.h          — Shared simulation logic (components, AI, systems)
├── main.cpp              — Terminal demo (headless, prints stats)
└── visual_main.cpp       — SDL2 visual demo (real-time rendering)

tests/
├── test_ecs.cpp          — Phase 1: Core ECS (27 tests)
├── test_ecs_phase2.cpp   — Phase 2: Events & Parallelism (37 tests)
└── test_ecs_phase3.cpp   — Phase 3: Gameplay Infrastructure (28 tests)
```

## Test Results

92 tests across 3 phases, passing on GCC-14, Clang-18, and MSVC 19.44 in both Debug and Release configurations.

```
Phase 1 — Core ECS:              27 passed
Phase 2 — Events & Parallelism:  37 passed
Phase 3 — Gameplay Infrastructure: 28 passed
Total:                            92 passed
```

## CI

GitHub Actions runs a 6-configuration matrix on every push:

| Compiler | Debug | Release |
|---|---|---|
| GCC 14 (Ubuntu 24.04) | Pass | Pass |
| Clang 18 (Ubuntu 24.04) | Pass | Pass |
| MSVC 19.44 (Windows Server 2022) | Pass | Pass |

All configurations compile clean with `-Wall -Wextra -Wpedantic -Werror` (GCC/Clang) and `/W4 /WX /Zc:preprocessor` (MSVC).
