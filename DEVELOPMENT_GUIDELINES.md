# FAT-P ECS Development Guidelines

## Relationship to FAT-P Library Development Guidelines

This document governs the `fatp-ecs` project, a standalone ECS framework that consumes
the [Fat-P](https://github.com/schroedermatthew/FatP) library. It inherits the **coding
standards** from the Fat-P Library Development Guidelines (hereafter "the parent guidelines")
and adapts them for a consumer project that is not itself a Fat-P component.

**Authority:** This document is authoritative for `fatp-ecs`. Where it conflicts with the
parent guidelines, this document wins for ECS code. Where this document is silent, the
parent guidelines apply.

**Parent document location:** `FatP/Read_Me/Fat-P_Library_Development_Guidelines.md`

---

## 1. What Transfers From Fat-P

### 1.1 Core Technical Requirements

| Requirement | FAT-P Rule | ECS Status |
|-------------|-----------|------------|
| C++ Standard | C++20 minimum | **Adopted** |
| Architecture | Header-only | **Adopted** |
| Dependencies | std + FAT-P only; no third-party libraries | **Adopted** |
| Compiler warnings | Clean under `-Wall -Wextra -Wpedantic -Werror` | **Adopted** |

### 1.2 Formatting Standards (Adopted Verbatim)

These rules are inherited from parent guidelines §5.2 without modification:

| Setting | Value |
|---------|-------|
| Brace style | Allman (opening brace on its own line) |
| Indent | 4 spaces, no tabs |
| Column limit | Target 100, hard limit 120 |
| Braces on control statements | Always (no braceless `if`/`for`/`while`) |
| Parameter packing | One per line when wrapping |
| Pointer/reference alignment | Left (`int* ptr`, not `int *ptr`) |
| Namespace indentation | None |
| Short forms | Never (no single-line `if`, `for`, functions, etc.) |

The full `.clang-format` from parent guidelines §5.2 applies to this project.

### 1.3 Naming Conventions (Adopted Verbatim)

Inherited from parent guidelines §5.3:

| Element | Style | Example |
|---------|-------|---------|
| Class/Struct | PascalCase | `Registry`, `ComponentStore`, `EventBus` |
| Function/Method | camelCase | `create()`, `isAlive()`, `tryGet()` |
| Class instance member | `m` prefix + PascalCase | `mStorage`, `mEntities`, `mEvents` |
| Aggregate/struct member | camelCase (no prefix) | `kind`, `entity`, `apply` |
| Static member variable | `s` prefix + PascalCase | `sInstanceCount` |
| Local variable | camelCase | `denseIdx`, `sparseIndex` |
| Template parameter | PascalCase | `T`, `Ts`, `Func`, `Args` |
| Type alias (STL-compatible) | snake_case | `value_type`, `size_type` |
| Type alias (project-specific) | PascalCase | `IndexType`, `StorageType`, `DataType` |
| Preprocessor macro | SCREAMING_SNAKE | `FATP_ECS_VERSION` |
| Compile-time constant (`constexpr`) | `k` prefix + PascalCase | `kMaxComponentTypes` |
| Namespace | lowercase | `fatp_ecs` |

**The dividing line for `m` prefix:** If the type has a user-declared constructor, destructor, or
private/protected members, use `m` prefix. If it's a simple aggregate (all public, no user-declared
special members), use plain camelCase.

**STL-compatible names retain snake_case:** `begin()`, `end()`, `size()`, `empty()`, `data()`,
`push_back()`, `pop_back()`. All other methods use camelCase.

### 1.4 Header Guards

Use `#pragma once` exclusively. No `#ifndef`/`#define`/`#endif` guards.

### 1.5 Header-Only Enforcement

All ECS code lives in `.h` files. The `inline` rules from parent guidelines §5.5 apply:

| Entity | Needs `inline`? |
|--------|-----------------|
| Function defined in header (namespace scope) | **YES** |
| Variable defined in header (namespace scope) | **YES** (`inline` or `constexpr`) |
| Function defined inside class body | No (implicitly inline) |
| Static member variable | **YES** (`inline static`) |
| Template function/class | No |
| `constexpr` function/variable | No |

Anonymous namespaces in headers are strongly discouraged. If used, a justification comment is
required immediately above.

### 1.6 Documentation Comments

Two types of comments serve different purposes:

| Type | Purpose | Audience |
|------|---------|----------|
| Doxygen (`/** */` or `///`) | Contract specification | IDE tooltips, API users |
| Regular (`//`) | Design rationale, implementation notes | Code readers, maintainers |

**Doxygen rules:**
- Brief: one line for `@brief`, 2-3 lines total maximum for simple methods
- Contract-focused: parameters, return values, preconditions, exceptions
- Use `///` for single-line briefs on trivial accessors
- Use `/** */` for multi-line documentation
- Do not put design rationale in Doxygen; use regular `//` comments
- The litmus test: "Would this make sense as an IDE tooltip?"

**Required Doxygen tags:**

| For classes | `@brief`, `@tparam` (if templated) |
|-------------|-------------------------------------|
| For methods | `@brief`, `@param` (if params), `@return` (if non-void), `@throws` (if can throw) |

**Complexity and thread-safety** use `@note` with standardized format:
```cpp
/// @note Complexity: O(1) amortized.
/// @note Thread-safety: NOT thread-safe.
```

**Standardized thread-safety phrases:**

| Phrase | Meaning |
|--------|---------|
| "NOT thread-safe" | No concurrent access allowed |
| "Thread-safe for concurrent reads" | Multiple readers OK, no writers |
| "Thread-safe" | Full concurrent access OK |

**Design rationale goes in regular comments**, not Doxygen:
```cpp
// The parallel entity vector is kept in sync via SparseSetWithData::indexOf(),
// which enables mirroring the swap-with-back erasure pattern during remove().
```

### 1.7 `[[nodiscard]]` Usage

Inherited from parent guidelines §5.10. Apply `[[nodiscard]]` to:

| Category | Example |
|----------|---------|
| Predicates | `isAlive()`, `has()`, `empty()` |
| Lookup functions | `tryGet()`, `get()`, `mask()` |
| Factory functions | `create()` if ignoring the handle is almost certainly a bug |

Do NOT apply to:

| Category | Example |
|----------|---------|
| Mutators with useful side effects | `add()`, `remove()`, `destroy()` |
| Methods where discarding is legitimate | `flush()`, `clear()` |

**Litmus test:** "If I grep typical usage, will most calls use the return value?" If mixed, omit it.

### 1.8 Include Ordering

Includes are grouped with blank line separators, alphabetical within each group:

1. Standard library headers
2. FAT-P headers
3. ECS project headers

```cpp
#include <cstddef>
#include <cstdint>
#include <vector>

#include <fat_p/SparseSet.h>

#include "Entity.h"
```

### 1.9 Using Directives

| Scope | Allowed |
|-------|---------|
| Global scope | **NO** |
| Function/block scope | **YES** |
| Namespace alias | **YES** |

---

## 2. What Does NOT Transfer From Fat-P

These parent guideline elements are FAT-P-specific and do not apply to this project:

| Parent Guideline Element | Why It Doesn't Apply |
|--------------------------|----------------------|
| **FATP_META blocks** | Component inventory system for the FAT-P repo. ECS headers are not FAT-P components. |
| **Layer system** (Foundation/Containers/Concurrency/Domain/Integration/Testing) | Internal FAT-P dependency hierarchy. The ECS is a consumer sitting above all layers. |
| **Layer verification protocol** | Depends on FATP_META.layer which we don't use. |
| **FatPTest.h test framework** | `FATP_TEST_CASE`, `TestRunner`, `FATP_RUN_TEST_NS`, nested `fat_p::testing::` namespaces. The ECS uses its own lightweight test macros. |
| **Vocabulary bans** (§8.2) | Applies to FAT-P component documentation (Overviews, User Manuals, Companion Guides), not consumer projects. |
| **Container documentation template** (§7) | Six-section structure (Intent, Guarantees, etc.) for FAT-P containers. |
| **Teaching document structure** | Four-Part Arc, Foundations, Handbooks, etc. |
| **Naming Rule 1** ("names describe the invariant, not the algorithm") | Applies to FAT-P component naming. ECS classes describe their role (`Registry`, `View`, `Scheduler`), not invariants. |
| **Canonical container names / adjective table** | FAT-P naming taxonomy for containers. |
| **Inventory count maintenance** | README/Authors.md count sync for the FAT-P repo. |
| **Benchmark environment reference** | FAT-P-specific hardware specs. |

---

## 3. ECS-Specific Rules

These are additional rules specific to the ECS project that are not covered by the parent guidelines.

### 3.1 Namespace

All ECS code lives in `namespace fatp_ecs`. This is separate from `fat_p::` to make clear
that the ECS is a consumer, not a FAT-P component.

### 3.2 File Header Format

ECS headers use `#pragma once` followed by a Doxygen file header. No FATP_META block.

```cpp
#pragma once

/**
 * @file ComponentStore.h
 * @brief Type-erased component storage backed by SparseSetWithData.
 */
```

The `@file` tag is required. A `@brief` is required. Additional `@details` or `@see` are optional.
Keep it short — if the design rationale needs multiple paragraphs, put it in a regular comment
block below the Doxygen header.

### 3.3 FAT-P Component Usage Documentation

Every ECS header that uses FAT-P components should have a regular comment block (not Doxygen)
listing which FAT-P components it uses and why:

```cpp
// FAT-P components used:
// - SparseSetWithData: Per-component-type storage with O(1) add/remove/get
//   - indexOf(): Keeps the parallel entity vector in sync on erase
// - FastHashMap: Type-erased component store registry
```

This block goes after the Doxygen file header and before the includes.

### 3.4 Entity Convention

Entity is a 64-bit strong type wrapping a SlotMap handle. The lower 32 bits are the slot index,
the upper 32 bits are the generation counter. Code that works with entities must always
preserve the full 64-bit value — never discard the generation.

When iterating component stores, always use the full Entity from `ComponentStore::entities()`,
never reconstruct from the SparseSet's dense index array (which only has the 32-bit index).

### 3.5 Test Structure

The ECS uses a lightweight test framework defined in each test file:

```cpp
#define TEST(name) ...
#define CHECK(expr) ...
#define PASS(name) ...
```

Tests are standalone `.cpp` files compiled directly. Each test file has a `main()` that
runs all tests and reports pass/fail counts.

**Requirements:**
- Every test must compile and pass under `-Wall -Wextra -Wpedantic -Werror`
- Suppress specific expected warnings explicitly (e.g., `(void)reg.create()` for nodiscard)
- Test both happy paths and failure modes
- Scale tests should verify behavior at 10K+ entities minimum

### 3.6 Circular Dependency Resolution

The ECS has a known circular dependency: CommandBuffer needs Registry (to call `create()`,
`destroy()`, `add()`, `remove()`), but Registry should not depend on CommandBuffer.

Resolution pattern:
- `CommandBuffer.h` declares methods but defers implementation of Registry-dependent code
- `CommandBuffer_Impl.h` provides the implementations, included after Registry.h
- `FatpEcs.h` (umbrella header) includes everything in the correct order

Any new circular dependencies should follow this same `_Impl.h` pattern.

---

## 4. AI Operational Rules

These rules apply to any AI assistant working on the ECS project. They are inherited from
parent guidelines §11 and adapted for the ECS context.

### 4.1 Inherited Verbatim

| Rule | Parent Reference |
|------|-----------------|
| **No unsolicited code** — respect Review / Implementation / Modification modes | §5.1.1 |
| **Complete files only** — never provide truncated files | §5.1 |
| **No AI comments** — no `NEW`, `FIXED`, `CHANGED` markers | §5.1 |
| **Always compile** — compile before delivering; if you can't, say so | §5.1 |
| **Compilation honesty** — never claim compiled/ran without actually doing it | §5.1.2 |
| **The Band-Aid Rule** — if you know the root cause, fix the root cause | §11.3.12 |
| **No backwards compat** — no deprecated aliases or compatibility shims | §5.1 |
| **Deliverable packaging** — Modified Files (N) manifest with repo-relative paths | §11.4 |
| **Download links for modified files only** — do not attach unchanged files | §11.4 |
| **Input validation** — flag truncation, list missing dependencies | §11.4 |
| **No inference** — don't guess at unspecified behavior | §11.3 |
| **Evidence requirements for reviews** — verbatim quotes, counterexamples | §11.8 |

### 4.2 Not Applicable

| Parent Rule | Why Not Applicable |
|-------------|-------------------|
| FATP_META verification | No FATP_META in ECS headers |
| Layer verification protocol | No layer system |
| Vocabulary enforcement | No vocabulary bans in consumer projects |
| Inventory count maintenance | No FAT-P repo counts to maintain |
| FatPTest.h test macros | ECS uses its own test macros |

---

## 5. Quick Reference Checklist

### Before Submitting Code:

- [ ] Allman braces, 4-space indent, no tabs
- [ ] Lines under 120 columns (target 100)
- [ ] `mPascalCase` for class members, camelCase for aggregate members
- [ ] `kPrefix` for constexpr constants
- [ ] camelCase for functions (except STL-compatible: `size()`, `begin()`, etc.)
- [ ] `#pragma once`, no include guards
- [ ] `inline` on namespace-scope functions/variables in headers
- [ ] No `using namespace` at global scope
- [ ] Doxygen on public API: brief, params, return, throws
- [ ] Design rationale in regular `//` comments, not Doxygen
- [ ] `[[nodiscard]]` only where discarding is almost certainly a bug
- [ ] Compiles clean under `-Wall -Wextra -Wpedantic -Werror`
- [ ] FAT-P component usage documented in comment block
- [ ] Full 64-bit Entity preserved everywhere (never discard generation)

### Before Submitting Tests:

- [ ] All tests pass
- [ ] Tests cover happy paths and failure modes
- [ ] Scale test at 10K+ entities
- [ ] No manual `cout` for test counting
- [ ] nodiscard warnings suppressed with `(void)` where intentional

### Before Submitting Files (AI):

- [ ] Modified Files (N) manifest present
- [ ] Download links match manifest (modified files only)
- [ ] Actually compiled and ran tests (with evidence)
- [ ] No AI comments (`NEW`, `FIXED`, etc.)
- [ ] No truncated files
