#pragma once

/**
 * @file Handle.h
 * @brief Ergonomic entity+registry wrapper for single-entity operations.
 */

// Overview:
//
// Handle bundles an Entity and a Registry* so that code working on a
// single entity does not need to thread both through every call site. It is
// a lightweight value type — just two pointers (Registry* + Entity uint64_t)
// — with no ownership semantics. The underlying registry and entity must
// outlive the handle.
//
// EnTT equivalent: entt::handle / entt::const_handle
//
// Usage:
//   Handle h = registry.handle(entity);
//   h.add<Position>(1.f, 2.f);
//   h.patch<Health>([](Health& hp){ hp.hp -= 10; });
//   if (h.has<Frozen>()) h.remove<Frozen>();
//   h.destroy();   // entity is gone; h is now invalid (null entity)
//
// Null handles:
//   Handle{} — registry is nullptr, entity is NullEntity.
//   Null handles return sensible defaults: has() = false, tryGet() = nullptr,
//   isAlive() = false. Calling add/get/destroy on a null handle is undefined.
//
// FAT-P components used:
//   (none — thin wrapper over Registry public API)

#include "Entity.h"

namespace fatp_ecs
{

class Registry;

// =============================================================================
// Handle
// =============================================================================

/**
 * @brief Mutable entity+registry pair for ergonomic single-entity operations.
 *
 * All methods forward to the underlying Registry using the stored entity.
 * The handle does not own either the registry or the entity.
 *
 * @note Thread-safety: NOT thread-safe (mirrors Registry).
 */
class Handle
{
public:
    // =========================================================================
    // Construction
    // =========================================================================

    /// @brief Null handle — registry is nullptr, entity is NullEntity.
    Handle() noexcept = default;

    /// @brief Construct from a registry and an entity.
    Handle(Registry& registry, Entity entity) noexcept
        : mRegistry(&registry)
        , mEntity(entity)
    {
    }

    // Copyable and movable — it is just a (pointer, value) pair.
    Handle(const Handle&) noexcept = default;
    Handle& operator=(const Handle&) noexcept = default;
    Handle(Handle&&) noexcept = default;
    Handle& operator=(Handle&&) noexcept = default;

    // =========================================================================
    // Identity
    // =========================================================================

    /// @brief The wrapped entity.
    [[nodiscard]] Entity entity() const noexcept { return mEntity; }

    /// @brief The underlying registry. nullptr for null handles.
    [[nodiscard]] Registry* registry() const noexcept { return mRegistry; }

    /// @brief True if this handle is non-null (registry set, entity not NullEntity).
    [[nodiscard]] bool valid() const noexcept
    {
        return mRegistry != nullptr && mEntity != NullEntity;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    // =========================================================================
    // Entity lifecycle
    // =========================================================================

    /// @brief True if the entity is currently alive in the registry.
    [[nodiscard]] bool isAlive() const noexcept;

    /**
     * @brief Destroy the entity.
     *
     * After this call the entity is dead. The handle's entity field becomes
     * NullEntity so subsequent isAlive() / has() calls return false correctly.
     *
     * @return true if the entity was alive and was destroyed; false otherwise.
     */
    bool destroy();

    // =========================================================================
    // Component operations
    // =========================================================================

    template <typename T, typename... Args>
    T& add(Args&&... args);

    template <typename T>
    bool remove();

    template <typename T>
    [[nodiscard]] bool has() const;

    template <typename T>
    [[nodiscard]] T& get();

    template <typename T>
    [[nodiscard]] const T& get() const;

    template <typename T>
    [[nodiscard]] T* tryGet();

    template <typename T>
    [[nodiscard]] const T* tryGet() const;

    template <typename T, typename Func>
    bool patch(Func&& func);

    template <typename T>
    bool patch();

    // =========================================================================
    // Comparison
    // =========================================================================

    [[nodiscard]] bool operator==(const Handle& other) const noexcept
    {
        return mRegistry == other.mRegistry && mEntity == other.mEntity;
    }

    [[nodiscard]] bool operator!=(const Handle& other) const noexcept
    {
        return !(*this == other);
    }

private:
    Registry* mRegistry{nullptr};
    Entity    mEntity{NullEntity};
};

// =============================================================================
// ConstHandle — read-only view of an entity
// =============================================================================

/**
 * @brief Read-only entity+registry pair.
 *
 * Like Handle but holds a const Registry* — no mutations allowed.
 * Implicitly constructible from Handle.
 */
class ConstHandle
{
public:
    ConstHandle() noexcept = default;

    ConstHandle(const Registry& registry, Entity entity) noexcept
        : mRegistry(&registry)
        , mEntity(entity)
    {
    }

    // Implicit conversion from mutable handle
    ConstHandle(const Handle& h) noexcept // NOLINT(google-explicit-constructor)
        : mRegistry(h.registry())
        , mEntity(h.entity())
    {
    }

    ConstHandle(const ConstHandle&) noexcept = default;
    ConstHandle& operator=(const ConstHandle&) noexcept = default;
    ConstHandle(ConstHandle&&) noexcept = default;
    ConstHandle& operator=(ConstHandle&&) noexcept = default;

    [[nodiscard]] Entity entity() const noexcept { return mEntity; }
    [[nodiscard]] const Registry* registry() const noexcept { return mRegistry; }
    [[nodiscard]] bool valid() const noexcept
    {
        return mRegistry != nullptr && mEntity != NullEntity;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] bool isAlive() const noexcept;

    template <typename T>
    [[nodiscard]] bool has() const;

    template <typename T>
    [[nodiscard]] const T& get() const;

    template <typename T>
    [[nodiscard]] const T* tryGet() const;

    [[nodiscard]] bool operator==(const ConstHandle& other) const noexcept
    {
        return mRegistry == other.mRegistry && mEntity == other.mEntity;
    }
    [[nodiscard]] bool operator!=(const ConstHandle& other) const noexcept
    {
        return !(*this == other);
    }

private:
    const Registry* mRegistry{nullptr};
    Entity          mEntity{NullEntity};
};

} // namespace fatp_ecs

// Out-of-line method bodies that require the full Registry definition.
#include "EntityHandle_Impl.h"
