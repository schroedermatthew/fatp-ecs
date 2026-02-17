#pragma once

/**
 * @file FrameAllocator.h
 * @brief Per-frame temporary allocator backed by ObjectPool.
 */

// FAT-P components used:
// - ObjectPool: Block-based object pool with O(1) acquire/release
//
// FrameAllocator wraps ObjectPool for per-frame temporary allocations.
// Systems acquire temporary objects during a frame, then releaseAll()
// returns them in bulk between frames. This avoids per-object new/delete
// in hot paths like collision detection or spatial queries.

#include <cassert>
#include <cstddef>
#include <vector>

#include <fat_p/ObjectPool.h>

namespace fatp_ecs
{

/**
 * @brief Per-frame temporary allocator backed by ObjectPool.
 *
 * @tparam T The object type to pool.
 *
 * @note Thread-safety: NOT thread-safe. Use one per thread or synchronize externally.
 */
template <typename T>
class FrameAllocator
{
public:
    /**
     * @brief Construct a frame allocator.
     *
     * @param blockSize Number of objects per ObjectPool block.
     */
    explicit FrameAllocator(std::size_t blockSize = 256)
        : mPool(blockSize)
    {
    }

    /**
     * @brief Acquire a temporary object for the current frame.
     *
     * @tparam Args Constructor argument types.
     * @param args Arguments forwarded to T's constructor.
     * @return Pointer to the acquired object.
     */
    template <typename... Args>
    [[nodiscard]] T* acquire(Args&&... args)
    {
        T* obj = mPool.acquire(std::forward<Args>(args)...);
        mAcquired.push_back(obj);
        return obj;
    }

    /// @brief Release all objects acquired this frame back to the pool.
    void releaseAll()
    {
        for (T* obj : mAcquired)
        {
            mPool.release(obj);
        }
        mAcquired.clear();
    }

    /// @brief Number of objects currently acquired this frame.
    [[nodiscard]] std::size_t activeCount() const noexcept
    {
        return mAcquired.size();
    }

    /// @brief Total capacity across all pool blocks.
    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return mPool.capacity();
    }

    /// @brief Number of objects available for acquisition without new allocation.
    [[nodiscard]] std::size_t available() const noexcept
    {
        return mPool.available();
    }

private:
    fat_p::ObjectPool<T> mPool;
    std::vector<T*> mAcquired;
};

} // namespace fatp_ecs
