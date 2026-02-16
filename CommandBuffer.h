#pragma once

/**
 * @file CommandBuffer.h
 * @brief Deferred command buffer for frame-boundary entity operations.
 */

// During system execution (especially parallel iteration), it's unsafe to
// directly create/destroy entities or add/remove components — these mutate
// the stores being iterated. CommandBuffer records operations as deferred
// commands, then flushes them all at once between frames.
//
// Pattern: Systems record mutations → CommandBuffer::flush() applies them.
//
// CommandBuffer (std::vector backend) is NOT thread-safe; use one per thread.
// ParallelCommandBuffer (mutex-protected) is thread-safe for concurrent recording.

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "Entity.h"

namespace fatp_ecs
{

class Registry;

// =============================================================================
// Command Types
// =============================================================================

/// @brief Enumeration of deferred command kinds.
enum class CommandKind : uint8_t
{
    Create,
    Destroy,
    AddComponent,
    RemoveComponent,
};

/// @brief A single deferred command with type-erased apply function.
struct Command
{
    CommandKind kind;
    Entity entity{NullEntity};
    std::function<void(Registry&)> apply;
};

// =============================================================================
// CommandBuffer — Single-Threaded
// =============================================================================

/// @brief Records deferred entity operations for batch application.
/// @note Thread-safety: NOT thread-safe. Use one per thread.
class CommandBuffer
{
public:
    CommandBuffer() = default;

    // =========================================================================
    // Static helpers — used by both CommandBuffer and ParallelCommandBuffer.
    // Implemented in CommandBuffer_Impl.h (depends on Registry).
    // =========================================================================

    static Entity createEntity(Registry& reg);
    static void destroyEntity(Registry& reg, Entity entity);

    template <typename T>
    static void addComponent(Registry& reg, Entity entity, T&& component);

    template <typename T>
    static void removeComponent(Registry& reg, Entity entity);

    // =========================================================================
    // Command recording
    // =========================================================================

    /**
     * @brief Records a deferred entity creation.
     *
     * @param onCreate Optional callback receiving the newly created Entity.
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

    /// @brief Records a deferred entity destruction.
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

    // =========================================================================
    // Flush / Query
    // =========================================================================

    /// @brief Applies all recorded commands to the registry, then clears.
    void flush(Registry& registry);

    [[nodiscard]] std::size_t size() const noexcept
    {
        return mCommands.size();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return mCommands.empty();
    }

    /// @brief Discards all pending commands without applying them.
    void clear() noexcept
    {
        mCommands.clear();
    }

private:
    std::vector<Command> mCommands;
};

// =============================================================================
// ParallelCommandBuffer — Thread-Safe
// =============================================================================

// Uses a mutex-protected vector for concurrent command recording from
// multiple worker threads. Flush is single-threaded (main thread between
// frames). The mutex approach is simple and correct; for extremely high
// contention a sharded design could be substituted.

/// @brief Thread-safe command buffer for multi-threaded system execution.
/// @note Thread-safety: Recording is thread-safe. Flush must be single-threaded.
class ParallelCommandBuffer
{
public:
    ParallelCommandBuffer() = default;

    /// @brief Records a deferred entity destruction (thread-safe).
    bool destroy(Entity entity)
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

    /// @brief Records a deferred component addition (thread-safe).
    template <typename T, typename... Args>
    bool add(Entity entity, Args&&... args)
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

    /// @brief Records a deferred component removal (thread-safe).
    template <typename T>
    bool remove(Entity entity)
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

    /// @brief Applies all recorded commands (single-threaded).
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

    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mCommands.size();
    }

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
