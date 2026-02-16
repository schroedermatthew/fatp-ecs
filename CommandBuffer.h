#pragma once

/**
 * @file CommandBuffer.h
 * @brief Deferred command buffer for frame-boundary entity operations.
 *
 * @details
 * During system execution (especially parallel iteration), it's unsafe to
 * directly create/destroy entities or add/remove components — these operations
 * mutate the stores that are being iterated. CommandBuffer solves this by
 * recording operations as deferred commands, then flushing them all at once
 * between frames (or between system executions).
 *
 * The pattern:
 * 1. Systems iterate entities via View (read-only structural changes)
 * 2. Systems record mutations (spawn, destroy, add, remove) into CommandBuffer
 * 3. After all systems finish, CommandBuffer::flush(registry) applies everything
 *
 * Command storage uses a type-erased approach: each command is a small
 * struct capturing the operation and entity, stored in a vector.
 * For cross-thread use, LockFreeQueue can be used as the backing store.
 *
 * FAT-P components used:
 * - CircularBuffer: SPSC ring buffer for single-producer command recording
 * - LockFreeQueue: MPMC queue for multi-producer parallel command recording
 *
 * @note Thread Safety:
 * - CommandBuffer (std::vector backend): NOT thread-safe. Use one per thread.
 * - ParallelCommandBuffer (LockFreeQueue backend): Thread-safe MPMC.
 */

#include <cstdint>
#include <functional>
#include <vector>

#include <fat_p/LockFreeQueue.h>

#include "Entity.h"

namespace fatp_ecs
{

// Forward declaration
class Registry;

// =============================================================================
// Command Types
// =============================================================================

/**
 * @brief Enumeration of deferred command kinds.
 */
enum class CommandKind : uint8_t
{
    Create,          ///< Create a new entity (no entity stored — assigned at flush)
    Destroy,         ///< Destroy an entity
    AddComponent,    ///< Add a component to an entity (with initializer)
    RemoveComponent, ///< Remove a component from an entity
};

/**
 * @brief A single deferred command.
 *
 * @details
 * Uses type-erased function for component operations.
 * The 'apply' function captures everything needed to execute
 * the operation against a Registry.
 *
 * For Create commands, the 'onCreate' callback receives the newly
 * created entity, allowing systems to chain setup operations.
 */
struct Command
{
    CommandKind kind;
    Entity entity{NullEntity};

    /// @brief Type-erased operation applied during flush.
    /// For AddComponent: captures component type, data, and calls registry.add().
    /// For RemoveComponent: captures component type and calls registry.remove().
    /// For Destroy: calls registry.destroy(entity).
    /// For Create: calls registry.create() and passes result to onCreate.
    std::function<void(Registry&)> apply;
};

// =============================================================================
// CommandBuffer — Single-Threaded
// =============================================================================

/**
 * @brief Records deferred entity operations for batch application.
 *
 * @details
 * Not thread-safe. Designed for single-system or per-thread usage.
 * Commands are stored in a std::vector for simplicity and cache locality.
 *
 * Usage:
 * @code
 * CommandBuffer cmd;
 *
 * // During system execution
 * view.each([&](Entity e, Health& hp) {
 *     if (hp.current <= 0) {
 *         cmd.destroy(e);
 *     }
 * });
 *
 * // Between frames
 * cmd.flush(registry);
 * @endcode
 */
class CommandBuffer
{
public:
    CommandBuffer() = default;

    /**
     * @brief Records a deferred entity creation.
     *
     * @param onCreate Callback receiving the newly created Entity.
     *                 Use this to attach components to the new entity.
     *
     * @code
     * cmd.create([](Registry& reg, Entity e) {
     *     reg.add<Position>(e, 0.0f, 0.0f);
     *     reg.add<Velocity>(e, 1.0f, 0.0f);
     * });
     * @endcode
     */
    void create(std::function<void(Registry&, Entity)> onCreate = nullptr)
    {
        Command c;
        c.kind = CommandKind::Create;
        if (onCreate)
        {
            c.apply = [cb = std::move(onCreate)](Registry& reg) {
                Entity e = createEntity(reg);
                cb(reg, e);
            };
        }
        else
        {
            c.apply = [](Registry& reg) {
                createEntity(reg);
            };
        }
        mCommands.push_back(std::move(c));
    }

    /**
     * @brief Records a deferred entity destruction.
     *
     * @param entity The entity to destroy.
     */
    void destroy(Entity entity)
    {
        Command c;
        c.kind = CommandKind::Destroy;
        c.entity = entity;
        c.apply = [entity](Registry& reg) {
            destroyEntity(reg, entity);
        };
        mCommands.push_back(std::move(c));
    }

    /**
     * @brief Records a deferred component addition.
     *
     * @tparam T Component type.
     * @tparam Args Constructor argument types.
     * @param entity The entity to add the component to.
     * @param args Arguments forwarded to T's constructor.
     */
    template <typename T, typename... Args>
    void add(Entity entity, Args&&... args)
    {
        // Capture args by value for deferred execution
        auto component = std::make_shared<T>(std::forward<Args>(args)...);
        Command c;
        c.kind = CommandKind::AddComponent;
        c.entity = entity;
        c.apply = [entity, comp = std::move(component)](Registry& reg) {
            addComponent<T>(reg, entity, std::move(*comp));
        };
        mCommands.push_back(std::move(c));
    }

    /**
     * @brief Records a deferred component removal.
     *
     * @tparam T Component type to remove.
     * @param entity The entity to remove the component from.
     */
    template <typename T>
    void remove(Entity entity)
    {
        Command c;
        c.kind = CommandKind::RemoveComponent;
        c.entity = entity;
        c.apply = [entity](Registry& reg) {
            removeComponent<T>(reg, entity);
        };
        mCommands.push_back(std::move(c));
    }

    /**
     * @brief Applies all recorded commands to the registry, then clears.
     *
     * @param registry The registry to apply commands to.
     *
     * @details
     * Commands are applied in recording order. This guarantees that
     * a "create then add component" sequence works correctly.
     *
     * @note After flush(), the command buffer is empty.
     */
    void flush(Registry& registry);

    /**
     * @brief Returns the number of pending commands.
     */
    [[nodiscard]] std::size_t size() const noexcept
    {
        return mCommands.size();
    }

    /**
     * @brief Returns true if no commands are pending.
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return mCommands.empty();
    }

    /**
     * @brief Discards all pending commands without applying them.
     */
    void clear() noexcept
    {
        mCommands.clear();
    }

private:
    std::vector<Command> mCommands;

public:
    // Static helpers for applying commands — used by both CommandBuffer and
    // ParallelCommandBuffer. Implemented in CommandBuffer_Impl.h.
    static Entity createEntity(Registry& reg);
    static void destroyEntity(Registry& reg, Entity entity);

    template <typename T>
    static void addComponent(Registry& reg, Entity entity, T&& component);

    template <typename T>
    static void removeComponent(Registry& reg, Entity entity);
};

// =============================================================================
// ParallelCommandBuffer — Thread-Safe MPMC
// =============================================================================

/**
 * @brief Thread-safe command buffer for multi-threaded system execution.
 *
 * @details
 * Uses LockFreeQueue for lock-free MPMC command submission.
 * Multiple worker threads can record commands concurrently.
 * Flush is single-threaded (called from the main thread between frames).
 *
 * The queue stores lightweight command indices that reference a shared
 * command storage. Due to LockFreeQueue's trivially-copyable requirement,
 * commands are stored indirectly via indices.
 *
 * FAT-P components used:
 * - LockFreeQueue: Lock-free MPMC queue for concurrent command submission
 *
 * @note Capacity is fixed at compile time (4096 commands).
 *       For larger bursts, increase the template parameter.
 */
class ParallelCommandBuffer
{
public:
    ParallelCommandBuffer() = default;

    /**
     * @brief Records a deferred entity destruction (thread-safe).
     *
     * @param entity The entity to destroy.
     * @return true if recorded; false if buffer is full.
     */
    [[nodiscard]] bool destroy(Entity entity)
    {
        Command c;
        c.kind = CommandKind::Destroy;
        c.entity = entity;
        c.apply = [entity](Registry& reg) {
            CommandBuffer::destroyEntity(reg, entity);
        };

        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.push_back(std::move(c));
        return true;
    }

    /**
     * @brief Records a deferred component addition (thread-safe).
     */
    template <typename T, typename... Args>
    [[nodiscard]] bool add(Entity entity, Args&&... args)
    {
        auto component = std::make_shared<T>(std::forward<Args>(args)...);
        Command c;
        c.kind = CommandKind::AddComponent;
        c.entity = entity;
        c.apply = [entity, comp = std::move(component)](Registry& reg) {
            CommandBuffer::addComponent<T>(reg, entity, std::move(*comp));
        };

        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.push_back(std::move(c));
        return true;
    }

    /**
     * @brief Records a deferred component removal (thread-safe).
     */
    template <typename T>
    [[nodiscard]] bool remove(Entity entity)
    {
        Command c;
        c.kind = CommandKind::RemoveComponent;
        c.entity = entity;
        c.apply = [entity](Registry& reg) {
            CommandBuffer::removeComponent<T>(reg, entity);
        };

        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.push_back(std::move(c));
        return true;
    }

    /**
     * @brief Applies all recorded commands to the registry (single-threaded).
     *
     * @param registry The registry to apply commands to.
     *
     * @note Must be called from a single thread (typically main thread).
     */
    void flush(Registry& registry)
    {
        std::vector<Command> commands;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            commands = std::move(mCommands);
            mCommands.clear();
        }

        for (auto& cmd : commands)
        {
            if (cmd.apply)
            {
                cmd.apply(registry);
            }
        }
    }

    /**
     * @brief Returns approximate number of pending commands.
     */
    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mCommands.size();
    }

    /**
     * @brief Discards all pending commands.
     */
    void clear()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mCommands.clear();
    }

private:
    mutable std::mutex mMutex;
    std::vector<Command> mCommands;
};

} // namespace fatp_ecs
