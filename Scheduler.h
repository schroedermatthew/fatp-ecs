#pragma once

/**
 * @file Scheduler.h
 * @brief ThreadPool-based parallel system execution for the ECS framework.
 *
 * @details
 * The Scheduler provides two levels of parallelism:
 *
 * 1. **System-Level Parallelism (Scheduler::run)**:
 *    Systems are registered with read/write component masks. The scheduler
 *    analyzes dependencies via BitSet intersection and runs non-conflicting
 *    systems in parallel on the ThreadPool.
 *
 * 2. **Data-Level Parallelism (par_each)**:
 *    A single system's iteration can be split across threads. The component
 *    dense array is partitioned into chunks, and each chunk is processed by
 *    a different ThreadPool worker.
 *
 * FAT-P components used:
 * - ThreadPool: Work-stealing thread pool for task execution
 * - BitSet: Component masks for dependency analysis (via ComponentMask)
 * - WorkQueue: (Available for fire-and-forget jobs if needed)
 *
 * @note Thread Safety: The Scheduler itself is not thread-safe.
 *       It's meant to be driven from a single "main loop" thread.
 *       The parallelism is internal to the Scheduler's execution.
 */

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

// Forward declarations
class Registry;
template <typename... Ts>
class View;

// =============================================================================
// System Descriptor
// =============================================================================

/**
 * @brief Describes a system's execution requirements.
 *
 * @details
 * A system is a function that operates on entity components.
 * The descriptor carries the function plus metadata for dependency analysis:
 * - writeMask: components the system modifies (exclusive access needed)
 * - readMask: components the system reads (shared access OK)
 *
 * Two systems can run in parallel iff neither's writeMask overlaps with
 * the other's readMask or writeMask (basic readers-writer analysis).
 */
struct SystemDescriptor
{
    std::string name;                          ///< Debug name for logging
    std::function<void(Registry&)> execute;    ///< The system function
    ComponentMask writeMask;                    ///< Components written (exclusive)
    ComponentMask readMask;                     ///< Components read (shared)

    /**
     * @brief Check if this system conflicts with another.
     *
     * Two systems conflict if:
     * - A's writes overlap with B's reads or writes, OR
     * - B's writes overlap with A's reads or writes
     */
    [[nodiscard]] bool conflictsWith(const SystemDescriptor& other) const noexcept
    {
        // My writes vs their reads or writes
        if (writeMask.intersects(other.readMask) || writeMask.intersects(other.writeMask))
        {
            return true;
        }
        // Their writes vs my reads
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

/**
 * @brief Manages system registration and parallel execution.
 *
 * @details
 * The Scheduler provides a simple API for registering systems and
 * executing them with automatic parallelism based on component
 * dependency analysis.
 *
 * Usage:
 * @code
 * Registry registry;
 * Scheduler scheduler(4);  // 4 worker threads
 *
 * // Register systems with component access declarations
 * scheduler.addSystem("Physics", [](Registry& reg) {
 *     reg.view<Position, Velocity>().each([](Entity e, Position& p, Velocity& v) {
 *         p.x += v.dx;
 *         p.y += v.dy;
 *     });
 * }, makeComponentMask<Position, Velocity>(),   // writes
 *    makeComponentMask<>());                     // reads
 *
 * scheduler.addSystem("Render", [](Registry& reg) {
 *     reg.view<Position, Sprite>().each([](Entity e, Position& p, Sprite& s) {
 *         // draw...
 *     });
 * }, makeComponentMask<>(),                      // writes
 *    makeComponentMask<Position, Sprite>());      // reads
 *
 * // Physics writes Position; Render reads Position → sequential
 * // But two read-only systems could run in parallel
 * scheduler.run(registry);
 * @endcode
 */
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
     * @param name Debug name for the system.
     * @param execute The system function.
     * @param writeMask Components this system writes.
     * @param readMask Components this system reads.
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

    /**
     * @brief Execute all registered systems with dependency-based parallelism.
     *
     * @param registry The registry to pass to each system.
     *
     * @details
     * Uses a simple greedy scheduling algorithm:
     * 1. Start with all systems unscheduled.
     * 2. Find systems that don't conflict with any currently-running system.
     * 3. Submit those to the ThreadPool.
     * 4. Wait for the batch to complete.
     * 5. Repeat until all systems have run.
     *
     * This guarantees correctness (no conflicting parallel access) while
     * extracting available parallelism. It's not globally optimal but is
     * simple and predictable.
     */
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

                // Check if this system conflicts with any already-batched system
                bool canRun = true;

                // My writes vs batch reads or writes
                if (sys.writeMask.intersects(batchReadMask) ||
                    sys.writeMask.intersects(batchWriteMask))
                {
                    canRun = false;
                }
                // My reads vs batch writes
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
                // Single system — run inline, no thread pool overhead
                mSystems[batch[0]].execute(registry);
            }
            else
            {
                // Multiple systems — submit to thread pool
                std::vector<std::future<void>> futures;
                futures.reserve(batch.size());

                for (std::size_t idx : batch)
                {
                    futures.push_back(mPool.submit([&registry, &sys = mSystems[idx]]() {
                        sys.execute(registry);
                    }));
                }

                // Wait for all to complete
                for (auto& f : futures)
                {
                    f.get();
                }
            }

            // Mark completed
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
     * @param count Total number of items to process.
     * @param func Function called for each chunk [begin, end).
     * @param minChunkSize Minimum items per chunk (default: 64).
     *
     * @details
     * Splits [0, count) into chunks of at least minChunkSize items.
     * Each chunk is submitted to the ThreadPool. The calling thread
     * also processes one chunk to avoid idle-waiting.
     *
     * Usage:
     * @code
     * auto& positions = store->data();
     * auto& velocities = velStore->data();
     *
     * scheduler.parallel_for(positions.size(),
     *     [&](std::size_t begin, std::size_t end) {
     *         for (std::size_t i = begin; i < end; ++i) {
     *             positions[i].x += velocities[i].dx;
     *         }
     *     });
     * @endcode
     */
    template <typename Func>
    void parallel_for(std::size_t count, Func&& func, std::size_t minChunkSize = 64)
    {
        if (count == 0)
        {
            return;
        }

        const std::size_t numThreads = mPool.thread_count();
        const std::size_t chunkSize = std::max(minChunkSize, (count + numThreads - 1) / numThreads);
        const std::size_t numChunks = (count + chunkSize - 1) / chunkSize;

        if (numChunks <= 1)
        {
            // Small enough to run single-threaded
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

        // Wait for worker chunks
        for (auto& f : futures)
        {
            f.get();
        }
    }

    /**
     * @brief Returns the number of registered systems.
     */
    [[nodiscard]] std::size_t systemCount() const noexcept
    {
        return mSystems.size();
    }

    /**
     * @brief Returns the number of worker threads in the pool.
     */
    [[nodiscard]] std::size_t threadCount() const noexcept
    {
        return mPool.thread_count();
    }

    /**
     * @brief Clears all registered systems.
     */
    void clearSystems() noexcept
    {
        mSystems.clear();
    }

    /**
     * @brief Direct access to the underlying ThreadPool.
     *
     * @details
     * Use this for custom parallelism patterns that don't fit
     * the system model (e.g., parallel asset loading, AI planning).
     */
    [[nodiscard]] fat_p::ThreadPool& pool() noexcept
    {
        return mPool;
    }

private:
    fat_p::ThreadPool mPool;
    std::vector<SystemDescriptor> mSystems;
};

} // namespace fatp_ecs
