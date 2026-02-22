#pragma once

/**
 * @file Handle_Impl.h
 * @brief Out-of-line implementations for Handle and ConstHandle.
 *
 * Included at the bottom of Handle.h. Do not include directly.
 * Requires Registry to be fully defined — ensured by FatpEcs.h include order.
 */

#include "Registry.h"
#include "EntityHandle.h"

namespace fatp_ecs
{

// =============================================================================
// Handle out-of-line
// =============================================================================

inline bool Handle::isAlive() const noexcept
{
    return valid() && mRegistry->isAlive(mEntity);
}

inline bool Handle::destroy()
{
    if (!valid())
    {
        return false;
    }
    const bool result = mRegistry->destroy(mEntity);
    mEntity = NullEntity; // invalidate so subsequent calls are safe
    return result;
}

template <typename T, typename... Args>
T& Handle::add(Args&&... args)
{
    return mRegistry->add<T>(mEntity, std::forward<Args>(args)...);
}

template <typename T>
bool Handle::remove()
{
    return mRegistry->remove<T>(mEntity);
}

template <typename T>
bool Handle::has() const
{
    return valid() && mRegistry->has<T>(mEntity);
}

template <typename T>
T& Handle::get()
{
    return mRegistry->get<T>(mEntity);
}

template <typename T>
const T& Handle::get() const
{
    return mRegistry->get<T>(mEntity);
}

template <typename T>
T* Handle::tryGet()
{
    return valid() ? mRegistry->tryGet<T>(mEntity) : nullptr;
}

template <typename T>
const T* Handle::tryGet() const
{
    return valid() ? mRegistry->tryGet<T>(mEntity) : nullptr;
}

template <typename T, typename Func>
bool Handle::patch(Func&& func)
{
    return mRegistry->patch<T>(mEntity, std::forward<Func>(func));
}

template <typename T>
bool Handle::patch()
{
    return mRegistry->patch<T>(mEntity);
}

// =============================================================================
// ConstHandle out-of-line
// =============================================================================

inline bool ConstHandle::isAlive() const noexcept
{
    return valid() && mRegistry->isAlive(mEntity);
}

template <typename T>
bool ConstHandle::has() const
{
    return valid() && mRegistry->has<T>(mEntity);
}

template <typename T>
const T& ConstHandle::get() const
{
    return mRegistry->get<T>(mEntity);
}

template <typename T>
const T* ConstHandle::tryGet() const
{
    return valid() ? mRegistry->tryGet<T>(mEntity) : nullptr;
}

// =============================================================================
// Registry factory methods
// =============================================================================

inline Handle Registry::handle(Entity entity) noexcept
{
    return Handle(*this, entity);
}

inline ConstHandle Registry::constHandle(Entity entity) const noexcept
{
    return ConstHandle(*this, entity);
}

} // namespace fatp_ecs
