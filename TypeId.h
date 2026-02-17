#pragma once

/**
 * @file TypeId.h
 * @brief Compile-time type identification for component type registration.
 */

// TypeId assigns a unique integer to each C++ type at first use. This is
// the backbone of the type-erased component store registry and the
// per-component-type event signal map. Extracted into its own header so
// that both EventBus.h and Registry.h can use it independently without
// circular includes.

#include <cstddef>

#include "ComponentMask.h"

namespace fatp_ecs
{

// =============================================================================
// TypeId
// =============================================================================

using TypeId = std::size_t;

namespace detail
{

inline TypeId nextTypeId() noexcept
{
    static TypeId counter = 0;
    return counter++;
}

} // namespace detail

template <typename T>
TypeId typeId() noexcept
{
    static const TypeId id = detail::nextTypeId();
    return id;
}

/// @brief Builds a ComponentMask from a list of component types.
template <typename... Ts>
ComponentMask makeComponentMask()
{
    ComponentMask mask;
    (mask.set(typeId<Ts>()), ...);
    return mask;
}

} // namespace fatp_ecs
