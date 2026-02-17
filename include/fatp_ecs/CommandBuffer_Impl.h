#pragma once

/**
 * @file CommandBuffer_Impl.h
 * @brief Implementation of CommandBuffer methods that depend on Registry.
 *
 * @details
 * This file must be included AFTER both CommandBuffer.h and Registry.h.
 * It resolves the circular dependency: CommandBuffer needs Registry for
 * flush(), and Registry doesn't need CommandBuffer directly.
 *
 * Include order in your code:
 * 1. Registry.h (or FatpEcs.h)
 * 2. CommandBuffer.h
 * 3. CommandBuffer_Impl.h
 *
 * Or just include FatpEcs.h which handles the order correctly.
 */

#include "CommandBuffer.h"
#include "Registry.h"

namespace fatp_ecs
{

// =============================================================================
// CommandBuffer::flush
// =============================================================================

inline void CommandBuffer::flush(Registry& registry)
{
    for (auto& cmd : mCommands)
    {
        if (cmd.apply)
        {
            cmd.apply(registry);
        }
    }
    mCommands.clear();
}

// =============================================================================
// CommandBuffer static helpers
// =============================================================================

inline Entity CommandBuffer::createEntity(Registry& reg)
{
    return reg.create();
}

inline void CommandBuffer::destroyEntity(Registry& reg, Entity entity)
{
    reg.destroy(entity);
}

template <typename T>
void CommandBuffer::addComponent(Registry& reg, Entity entity, T&& component)
{
    reg.add<std::remove_cvref_t<T>>(entity, std::forward<T>(component));
}

template <typename T>
void CommandBuffer::removeComponent(Registry& reg, Entity entity)
{
    reg.remove<T>(entity);
}

} // namespace fatp_ecs
