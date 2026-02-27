# fatp-ecs

An ECS built to show what [FAT-P](https://github.com/schroedermatthew/FatP) can do.

The premise: take a library of focused, production-quality components — sparse sets, slot maps, signals, hash maps — and assemble them into something non-trivial. The result should be competitive with EnTT, the industry-standard ECS, out of the box. Not as a nice-to-have. As a baseline expectation.

This is that result. 19 FAT-P components. EnTT API parity. Faster on most benchmarks. Built in a weekend with an AI pair-programmer.

[![CI](https://github.com/schroedermatthew/fatp-ecs/actions/workflows/ci.yml/badge.svg)](https://github.com/schroedermatthew/fatp-ecs/actions/workflows/ci.yml)

---

## Performance

Benchmarked against [EnTT](https://github.com/skypjack/entt) v3.14, the industry reference. All benchmarks use round-robin execution with randomized order, statistical reporting (median of 20 batches), and CPU frequency monitoring via [FatPBenchmarkRunner](https://github.com/schroedermatthew/FatP).

fatp-ecs uses 64-bit entity IDs throughout. All comparisons are against EnTT configured with 64-bit IDs.

### vs EnTT-64 at scale (GCC-14, GitHub Actions)

Rows 1–10: N=1M entities. Fragmented and Churn at N=100K.

| Category | fatp-ecs | EnTT-64 | ratio |
|---|---|---|---|
| Create entities | 7.76 ns | 12.46 ns | **0.62x** |
| Destroy entities | 6.52 ns | 12.91 ns | **0.51x** |
| Add 1 component | 14.20 ns | 13.58 ns | 1.05x |
| Add 3 components | 41.61 ns | 40.00 ns | 1.04x |
| Remove component | 5.70 ns | 15.81 ns | **0.36x** |
| Get component | 3.14 ns | 4.85 ns | **0.65x** |
| 1-comp iteration | 0.66 ns | 0.88 ns | **0.75x** |
| 2-comp iteration | 1.34 ns | 4.11 ns | **0.33x** |
| Sparse iteration | 1.56 ns | 4.20 ns | **0.37x** |
| 3-comp iteration | 3.09 ns | 7.64 ns | **0.40x** |
| Fragmented iter  | 0.64 ns | 0.83 ns | **0.77x** |
| Churn (create+destroy) | 16.03 ns | 31.41 ns | **0.51x** |

**Bold** = fatp-ecs faster. Ratio below 1.0x means fatp-ecs wins by that factor.

Add component is slightly slower because fatp-ecs fires lifecycle events on every `add()`. This is deliberate — `onComponentAdded<T>` is always wired up, not opt-in. The overhead is ~0.6–0.7 ns per add on GCC at scale. Everything else is faster.

### Cross-compiler summary (N=1M except Frag/Churn at N=100K, vs EnTT-64)

| Category | GCC-13 | GCC-14 | Clang-16 | Clang-17 | MSVC |
|---|---|---|---|---|---|
| Create | **0.53x** | **0.62x** | **0.61x** | **0.61x** | **0.75x** |
| Destroy | **0.51x** | **0.51x** | **0.59x** | **0.53x** | **0.44x** |
| Add 1 | 1.19x | 1.05x | **0.96x** | **0.99x** | **0.90x** |
| Add 3 | 1.15x | 1.04x | **0.91x** | **0.94x** | 1.01x |
| Remove | **0.37x** | **0.36x** | **0.49x** | **0.48x** | **0.33x** |
| Get | **0.62x** | **0.65x** | **0.92x** | **0.89x** | 1.07x |
| 1-comp iter | **0.67x** | **0.75x** | **0.62x** | **0.64x** | **0.49x** |
| 2-comp iter | **0.32x** | **0.33x** | **0.68x** | **0.62x** | **0.31x** |
| Sparse iter | **0.38x** | **0.37x** | **0.63x** | **0.52x** | **0.32x** |
| 3-comp iter | **0.40x** | **0.40x** | **0.63x** | **0.62x** | **0.44x** |
| Fragmented | **0.76x** | **0.77x** | **0.66x** | **0.68x** | **0.66x** |
| Churn | **0.45x** | **0.51x** | **0.49x** | **0.50x** | **0.34x** |

The iteration advantage is consistent across all five compilers. MSVC shows the strongest gains in sparse iteration (0.32x) and churn (0.34x). Add component is at or near parity on Clang and MSVC — the event system overhead is effectively inlined away on those toolchains. GCC-13 shows a modest add overhead; entity creation is not a per-frame hot path.

### Running benchmarks

```bash
# Local (Windows, vcpkg)
cmake -B build -DFATP_ECS_BUILD_BENCH=ON -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --target benchmark
build\Release\benchmark.exe

# CI (manual dispatch)
# Go to Actions > "fatp-ecs Benchmarks" > Run workflow
```

Environment variables: `FATP_BENCH_BATCHES` (default 20), `FATP_BENCH_WARMUP_RUNS` (default 3), `FATP_BENCH_NO_STABILIZE=1` (skip CPU wait), `FATP_BENCH_VERBOSE_STATS=1` (detailed output).

---

## API

fatp-ecs matches the EnTT registry API surface. Code written against EnTT compiles against fatp-ecs with minimal changes — mostly namespace and header swaps.

```cpp
#include <fatp_ecs/FatpEcs.h>
using namespace fatp_ecs;

Registry registry;

// Entity lifecycle
Entity player   = registry.create();
Entity restored = registry.create(saved_hint); // hint-based, for snapshot restore

// Component operations — EnTT names work directly
registry.emplace<Position>(player, 0.f, 0.f);      // alias for add<T>()
registry.emplace_or_replace<Velocity>(player, 1.f, 0.5f);
registry.patch<Health>(player, [](Health& h) { h.hp -= 10; });
registry.erase<Poison>(player);                     // asserting remove

// Presence queries
bool alive    = registry.valid(player);
bool hasCombo = registry.all_of<Position, Velocity>(player);
bool hasAny   = registry.any_of<Frozen, Stunned>(player);
bool clean    = registry.none_of<Dead, Disabled>(player);
Position* p   = registry.try_get<Position>(player); // nullptr if missing
Position& pos = registry.get_or_emplace<Position>(player, 0.f, 0.f);

// Entity enumeration
registry.each([](Entity e) { /* all live entities */ });
registry.orphans([&](Entity e) { registry.destroy(e); });

// Bulk operations
registry.clear<Frozen>();  // remove Frozen from every entity, fires events
registry.clear();          // destroy everything

// Direct store access (for tooling / custom sorting)
if (auto* s = registry.storage<Position>()) {
    std::printf("%zu entities have Position\n", s->size());
}

// Views
registry.view<Position, Velocity>().each(
    [](Entity e, Position& pos, Velocity& vel) {
        pos.x += vel.dx;
        pos.y += vel.dy;
    });

// Views with exclude filters
registry.view<Position>(Exclude<Frozen>{}).each(
    [](Entity e, Position& pos) { /* skip frozen entities */ });

// Groups (contiguous layout, fastest iteration)
auto& grp = registry.group<Position, Velocity>();
if (auto* g = registry.group_if_exists<Position, Velocity>()) {
    g->each([](Entity e, Position& p, Velocity& v) { /* ... */ });
}

// Signals — EnTT names
auto c1 = registry.on_construct<Health>().connect([](Entity e, Health& h) { /* ... */ });
auto c2 = registry.on_destroy<Health>().connect([](Entity e) { /* ... */ });
auto c3 = registry.on_update<Health>().connect([](Entity e, Health& h) { /* ... */ });

// Observers — watch for component combinations appearing
auto obs = registry.observe(OnAdded<Position>{}, OnAdded<Velocity>{});
obs.each([](Entity e) { /* e now has both Position and Velocity */ });

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
int hp    = applyDamage(currentHp, damage, maxHp); // clamped to [0, maxHp]
int score = addScore(currentScore, points);         // saturates at INT_MAX
```

### EnTT migration reference

| EnTT | fatp-ecs | Notes |
|---|---|---|
| `registry.emplace<T>()` | `registry.emplace<T>()` | ✓ direct alias |
| `registry.replace<T>()` | `registry.replace<T>()` | ✓ |
| `registry.patch<T>()` | `registry.patch<T>()` | ✓ |
| `registry.erase<T>()` | `registry.erase<T>()` | ✓ asserting remove |
| `registry.remove<T>()` | `registry.remove<T>()` | ✓ returns bool |
| `registry.get<T>()` | `registry.get<T>()` | ✓ |
| `registry.try_get<T>()` | `registry.try_get<T>()` | ✓ |
| `registry.get_or_emplace<T>()` | `registry.get_or_emplace<T>()` | ✓ |
| `registry.contains<T>()` | `registry.contains<T>()` | ✓ |
| `registry.all_of<Ts...>()` | `registry.all_of<Ts...>()` | ✓ |
| `registry.any_of<Ts...>()` | `registry.any_of<Ts...>()` | ✓ |
| `registry.none_of<Ts...>()` | `registry.none_of<Ts...>()` | ✓ |
| `registry.valid()` | `registry.valid()` | ✓ |
| `registry.alive()` | `registry.alive()` | ✓ |
| `registry.each()` | `registry.each()` | ✓ |
| `registry.orphans()` | `registry.orphans()` | ✓ |
| `registry.clear<T>()` | `registry.clear<T>()` | ✓ fires events |
| `registry.storage<T>()` | `registry.storage<T>()` | ✓ |
| `registry.on_construct<T>()` | `registry.on_construct<T>()` | ✓ |
| `registry.on_destroy<T>()` | `registry.on_destroy<T>()` | ✓ |
| `registry.on_update<T>()` | `registry.on_update<T>()` | ✓ |
| `registry.create(hint)` | `registry.create(hint)` | ✓ slot-index hint |
| `registry.group_if_exists<Ts...>()` | `registry.group_if_exists<Ts...>()` | ✓ |
| `registry.view<Ts>(exclude<Xs>)` | `registry.view<Ts>(Exclude<Xs>{})` | syntax differs |
| `registry.emplace_or_replace<T>()` | `registry.emplace_or_replace<T>()` | ✓ |

---

## How FAT-P makes this possible

Each FAT-P component maps directly to an ECS problem:

| FAT-P Component | ECS Role |
|---|---|
| **StrongId** | Type-safe 64-bit Entity handles (index + generation) |
| **SparseSetWithData** | O(1) component storage with cache-friendly dense iteration |
| **SlotMap** | Entity allocator with generational ABA safety + `insert_at()` for hint-based create |
| **FastHashMap** | Type-erased component store registry |
| **SmallVector** | Stack-allocated entity query results |
| **Signal** | Observer pattern for entity/component lifecycle events |
| **ThreadPool** | Work-stealing parallel system execution |
| **BitSet** | Component masks for archetype matching and dependency analysis |
| **WorkQueue** | Job dispatch (via ThreadPool internals) |
| **ObjectPool** | Per-frame temporary allocator with bulk reset |
| **StringPool** | Interned entity names for pointer-equality comparison |
| **FlatMap** | Sorted name-to-entity mapping for debug/editor tools |
| **JsonLite** | Data-driven entity template definitions |
| **StateMachine** | Compile-time AI state machines with context binding |
| **FeatureManager** | Runtime system enable/disable toggles |
| **CheckedArithmetic** | Overflow-safe health/damage/score calculations |
| **AlignedVector** | SIMD-friendly aligned component storage |
| **LockFreeQueue** | Thread-safe parallel command buffer |
| **CircularBuffer** | Deferred command queues |

The components weren't designed for an ECS. They were designed to be useful individually. The ECS is what happens when you compose them.

---

## Building

Header-only. Requires C++20 and FAT-P as a sibling directory or via `FATP_INCLUDE_DIR`.

### Build scripts

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

**Linux / macOS:**
```bash
./build.sh --clean              # full clean build
./build.sh --no-visual          # skip SDL2
./build.sh --debug              # debug build
```

### Manual CMake

```bash
# Tests only (no SDL2 required)
cmake -B build -DFATP_INCLUDE_DIR=../FatP/include
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# With SDL2 visual demo (Windows / vcpkg)
cmake -B build -DFATP_INCLUDE_DIR=../FatP/include -DFATP_ECS_BUILD_VISUAL_DEMO=ON \
      -DCMAKE_TOOLCHAIN_FILE="<vcpkg-root>/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release

# With SDL2 visual demo (Linux)
sudo apt install libsdl2-dev libsdl2-ttf-dev
cmake -B build -DFATP_INCLUDE_DIR=../FatP/include -DFATP_ECS_BUILD_VISUAL_DEMO=ON
cmake --build build

# Direct compilation
g++ -std=c++20 -O2 -I include -I /path/to/FatP/include your_code.cpp -lpthread
```

### CMake options

| Option | Default | Description |
|---|---|---|
| `FATP_INCLUDE_DIR` | auto-detect | Path to FAT-P include directory |
| `FATP_ECS_BUILD_TESTS` | `ON` | Build test executables |
| `FATP_ECS_BUILD_DEMO` | `ON` | Build terminal demo |
| `FATP_ECS_BUILD_VISUAL_DEMO` | `OFF` | Build SDL2 visual demo (requires SDL2, SDL2_ttf) |
| `FATP_ECS_BUILD_BENCH` | `OFF` | Build benchmark suite (requires EnTT via vcpkg) |

---

## Demo

### Terminal demo

Headless space battle simulation exercising all 19 FAT-P components:

```bash
build/Release/demo.exe
build/Release/demo.exe --wave-size 100 --turrets 8 --frames 500
```

### Visual demo (SDL2)

Real-time rendering of the space battle. The ECS ticks every frame; SDL2 draws the result. Frame time shown is the real end-to-end cost.

```bash
build/Release/visual_demo.exe
build/Release/visual_demo.exe --wave-size 100 --turrets 8
```

| Key | Action |
|---|---|
| Space | Pause / resume |
| 1 / 2 / 3 | Speed 1x / 2x / 5x |
| F | Toggle vsync (capped 60fps vs uncapped) |
| R | Reset simulation |
| + / - | Increase / decrease wave size |
| Escape | Quit |

---

## Tests

18 test suites, 539 tests, all passing across the full CI matrix.

```
Phase 1 — Core ECS:                27 passed
Phase 2 — Events & Parallelism:    37 passed
Phase 3 — Gameplay Infrastructure: 28 passed
Exclude filters:                   15 passed
Patch:                             15 passed
Observer:                          21 passed
OwningGroup:                       16 passed
Sort:                              15 passed
Snapshot:                          15 passed
Handle:                            20 passed
EntityCopy:                        16 passed
ProcessScheduler:                  20 passed
Clear stress:                     138 passed
RuntimeView:                       17 passed
StoragePolicy:                     65 passed
New API:                           16 passed
NonOwningGroup:                    14 passed
EnTT parity:                       44 passed
Total:                            539 passed
```

---

## CI

GitHub Actions runs a 12-job matrix on every push:

| Job | Configurations |
|---|---|
| Linux GCC | GCC-12 (C++20), GCC-13 (C++20 Debug+Release), GCC-14 (C++23) |
| Linux Clang | Clang-16 (C++20), Clang-17 (C++23) |
| Windows MSVC | C++20 (Debug+Release), C++23 |
| Sanitizers | AddressSanitizer, UndefinedBehaviorSanitizer |
| Gate | CI Success (aggregates all jobs) |

Benchmarks run separately via manual dispatch across GCC-13, GCC-14, Clang-16, Clang-17, and MSVC.
