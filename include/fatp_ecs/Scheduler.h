#pragma once

/**
 * @file Scheduler.h
 * @brief ThreadPool-based parallel system execution for the ECS framework.
 */

// FAT-P components used:
// - ThreadPool: Work-stealing thread pool with priority queues
// - BitSet: Component masks for dependency analysis (via ComponentMask)
//
// Two levels of parallelism:
//
// 1. System-level (Scheduler::run): Systems declare read/write component
//    masks. The scheduler uses BitSet intersection to identify non-conflicting
//    systems and runs them concurrently on the ThreadPool. Greedy batching:
//    collect all runnable non-conflicting systems, submit, wait, repeat.
//
// 2. Data-level (Scheduler::parallel_for): A single system's iteration is
//    split across threads. The dense array is partitioned into chunks, each
//    processed by a different worker. The calling thread processes the last
//    chunk to avoid idle-waiting.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include <fat_p/ThreadPool.h>

#include "ComponentMask.h"
#include "Entity.h"

namespace fatp_ecs
{

class Registry;

template <typename IncludePack, typename ExcludePack>
class ViewImpl;

// =============================================================================
// System Descriptor
// =============================================================================

// Two systems conflict if either's writeMask overlaps with the other's
// readMask or writeMask (basic readers-writer analysis).

/// @brief Describes a system's execution function and component access.
struct SystemDescriptor
{
    std::string name;
    std::function<void(Registry&)> execute;
    ComponentMask writeMask;
    ComponentMask readMask;

    /// @brief Returns true if this system conflicts with another.
    [[nodiscard]] bool conflictsWith(const SystemDescriptor& other) const noexcept
    {
        if (writeMask.intersects(other.readMask) ||
            writeMask.intersects(other.writeMask))
        {
            return true;
        }
        if (other.writeMask.intersects(readMask))
        {
            return true;
        }
        return false;
    }
};

// =============================================================================
// Scheduler
// =============================================================================

/// @brief Manages system registration and parallel execution.
/// @note Thread-safety: NOT thread-safe. Drive from a single main-loop thread.
class Scheduler
{
public:
    /**
     * @brief Construct a scheduler with its own ThreadPool.
     *
     * @param numThreads Number of worker threads (0 = hardware_concurrency).
     */
    explicit Scheduler(std::size_t numThreads = 0)
        : mPool(numThreads, /*spin_us=*/500)
    {
    }

    /**
     * @brief Register a system for scheduled execution.
     *
     * @param name      Debug name for the system.
     * @param execute   The system function.
     * @param writeMask Components this system writes.
     * @param readMask  Components this system reads.
     */
    void addSystem(std::string name,
                   std::function<void(Registry&)> execute,
                   ComponentMask writeMask = {},
                   ComponentMask readMask = {})
    {
        mSystems.push_back({
            std::move(name),
            std::move(execute),
            std::move(writeMask),
            std::move(readMask),
        });
    }

    /// @brief Execute all registered systems with dependency-based parallelism.
    void run(Registry& registry)
    {
        if (mSystems.empty())
        {
            return;
        }

        std::vector<bool> completed(mSystems.size(), false);
        std::size_t completedCount = 0;

        while (completedCount < mSystems.size())
        {
            // Collect non-conflicting systems for this batch
            std::vector<std::size_t> batch;
            ComponentMask batchWriteMask;
            ComponentMask batchReadMask;

            for (std::size_t i = 0; i < mSystems.size(); ++i)
            {
                if (completed[i])
                {
                    continue;
                }

                const auto& sys = mSystems[i];

                bool canRun = true;
                if (sys.writeMask.intersects(batchReadMask) ||
                    sys.writeMask.intersects(batchWriteMask))
                {
                    canRun = false;
                }
                if (canRun && sys.readMask.intersects(batchWriteMask))
                {
                    canRun = false;
                }

                if (canRun)
                {
                    batch.push_back(i);
                    batchWriteMask |= sys.writeMask;
                    batchReadMask |= sys.readMask;
                }
            }

            // Execute the batch
            if (batch.size() == 1)
            {
                mSystems[batch[0]].execute(registry);
            }
            else
            {
                std::vector<std::future<void>> futures;
                futures.reserve(batch.size());

                for (std::size_t idx : batch)
                {
                    futures.push_back(
                        mPool.submit([&registry, &sys = mSystems[idx]]() {
                            sys.execute(registry);
                        }));
                }

                for (auto& f : futures)
                {
                    f.get();
                }
            }

            for (std::size_t idx : batch)
            {
                completed[idx] = true;
            }
            completedCount += batch.size();
        }
    }

    /**
     * @brief Execute a function in parallel across chunks of data.
     *
     * @tparam Func Callable with signature void(std::size_t begin, std::size_t end).
     * @param count        Total number of items to process.
     * @param func         Function called for each chunk [begin, end).
     * @param minChunkSize Minimum items per chunk (default: 64).
     */
    template <typename Func>
    void parallel_for(std::size_t count, Func&& func,
                      std::size_t minChunkSize = 64)
    {
        if (count == 0)
        {
            return;
        }

        const std::size_t numThreads = mPool.thread_count();
        const std::size_t chunkSize =
            std::max(minChunkSize, (count + numThreads - 1) / numThreads);
        const std::size_t numChunks = (count + chunkSize - 1) / chunkSize;

        if (numChunks <= 1)
        {
            func(0, count);
            return;
        }

        // Submit all but the last chunk to the thread pool
        std::vector<std::future<void>> futures;
        futures.reserve(numChunks - 1);

        for (std::size_t chunk = 0; chunk < numChunks - 1; ++chunk)
        {
            std::size_t begin = chunk * chunkSize;
            std::size_t end = begin + chunkSize;

            futures.push_back(mPool.submit([&func, begin, end]() {
                func(begin, end);
            }));
        }

        // Process the last chunk on the calling thread
        {
            std::size_t begin = (numChunks - 1) * chunkSize;
            std::size_t end = count;
            func(begin, end);
        }

        for (auto& f : futures)
        {
            f.get();
        }
    }

    /// @brief Returns the number of registered systems.
    [[nodiscard]] std::size_t systemCount() const noexcept
    {
        return mSystems.size();
    }

    /// @brief Returns the number of worker threads in the pool.
    [[nodiscard]] std::size_t threadCount() const noexcept
    {
        return mPool.thread_count();
    }

    /// @brief Clears all registered systems.
    void clearSystems() noexcept
    {
        mSystems.clear();
    }

    /// @brief Direct access to the underlying ThreadPool.
    [[nodiscard]] fat_p::ThreadPool& pool() noexcept
    {
        return mPool;
    }

private:
    fat_p::ThreadPool mPool;
    std::vector<SystemDescriptor> mSystems;
};

} // namespace fatp_ecs
