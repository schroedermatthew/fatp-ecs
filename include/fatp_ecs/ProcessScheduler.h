#pragma once

/**
 * @file ProcessScheduler.h
 * @brief Cooperative process model with delta-time ticking and chained continuations.
 */

// Overview:
//
// A Process is a cooperative task that runs every tick until it signals
// completion (succeeded), failure (failed), or is externally aborted.
// Processes can chain a "next" process that runs immediately after the
// current one finishes successfully.
//
// EnTT equivalent: entt::process / entt::scheduler<Delta>
//
// Process lifecycle:
//
//   [uninitialized]
//       | first tick
//       v
//   [running] --update()--> [running]  (while alive())
//       |
//       |--succeed()--> [succeeded] --> run next process (if any)
//       |--fail()-----> [failed]    --> next process NOT run
//       |--abort()----> [aborted]   --> next process NOT run
//
// User-defined processes inherit from Process<Derived, Delta, Data> and
// override:
//   void init()                        -- called once before first update
//   void update(Delta delta, Data& data) -- called every tick while running
//   void succeeded()                   -- called on successful completion
//   void failed()                      -- called on failure
//   void aborted()                     -- called on abort
//
// ProcessScheduler<Delta, Data> owns a list of process chains. Each call
// to update(delta, data) ticks all live processes. Completed processes are
// removed; their successors (if any) are queued.
//
// FAT-P components used:
//   (none — self-contained; only standard library types)

#include <cstddef>
#include <memory>
#include <vector>

namespace fatp_ecs
{

// =============================================================================
// ProcessState
// =============================================================================

enum class ProcessState : uint8_t
{
    Uninitialized, ///< Not yet ticked.
    Running,       ///< Currently active.
    Succeeded,     ///< Finished successfully.
    Failed,        ///< Finished with failure.
    Aborted,       ///< Externally aborted.
};

// =============================================================================
// IProcess — type-erased base
// =============================================================================

/**
 * @brief Type-erased base for ProcessScheduler storage.
 *
 * Do not inherit from this directly. Inherit from Process<Derived, Delta, Data>.
 */
template <typename Delta, typename Data>
class IProcess
{
public:
    virtual ~IProcess() = default;

    /// @brief Tick the process. Returns true if it is still running after the tick.
    virtual bool tick(Delta delta, Data& data) = 0;

    /// @brief Externally abort this process.
    virtual void abort() = 0;

    /// @brief True if the process completed successfully (not failed/aborted).
    [[nodiscard]] virtual bool succeeded() const noexcept = 0;

    /// @brief Attach a successor. Runs after this process succeeds.
    virtual void setNext(std::unique_ptr<IProcess> next) = 0;

    /// @brief Take ownership of the successor (called by scheduler on completion).
    [[nodiscard]] virtual std::unique_ptr<IProcess> takeNext() = 0;

    IProcess() = default;
    IProcess(const IProcess&) = delete;
    IProcess& operator=(const IProcess&) = delete;
    IProcess(IProcess&&) = delete;
    IProcess& operator=(IProcess&&) = delete;
};

// =============================================================================
// Process<Derived, Delta, Data> — CRTP base for user processes
// =============================================================================

/**
 * @brief CRTP base class for user-defined processes.
 *
 * @tparam Derived  The user's concrete process type.
 * @tparam Delta    The delta-time type (e.g. float, double, uint32_t).
 * @tparam Data     Optional shared context passed to every tick (default: void*).
 *
 * @example
 * @code
 *   struct CountdownProcess : fatp_ecs::Process<CountdownProcess, float>
 *   {
 *       explicit CountdownProcess(float duration) : mRemaining(duration) {}
 *
 *       void onUpdate(float dt, void*&)
 *       {
 *           mRemaining -= dt;
 *           if (mRemaining <= 0.f) succeed();
 *       }
 *
 *   private:
 *       float mRemaining;
 *   };
 *
 *   fatp_ecs::ProcessScheduler<float> scheduler;
 *   scheduler.attach<CountdownProcess>(3.0f)
 *            .then<FlashProcess>();
 *
 *   // Each frame:
 *   scheduler.update(deltaTime);
 * @endcode
 *
 * @note Thread-safety: NOT thread-safe.
 */
template <typename Derived, typename Delta, typename Data = void*>
class Process : public IProcess<Delta, Data>
{
public:
    // =========================================================================
    // Lifecycle hooks — override in Derived
    // =========================================================================

    /// @brief Called once before the first update(). Override to initialize state.
    void onInit() {}

    /// @brief Called every tick while the process is running.
    ///        Call succeed(), fail(), or do nothing to remain running.
    void onUpdate(Delta /*delta*/, Data& /*data*/) {}

    /// @brief Called once when succeed() transitions the process to Succeeded.
    void onSucceeded() {}

    /// @brief Called once when fail() transitions the process to Failed.
    void onFailed() {}

    /// @brief Called once when abort() transitions the process to Aborted.
    void onAborted() {}

    // =========================================================================
    // State signals — call from update() to drive transitions
    // =========================================================================

    /// @brief Signal successful completion. Successor (if any) will be queued.
    void succeed() noexcept { mState = ProcessState::Succeeded; }

    /// @brief Signal failure. No successor will run.
    void fail() noexcept { mState = ProcessState::Failed; }

    // =========================================================================
    // Queries
    // =========================================================================

    [[nodiscard]] ProcessState state() const noexcept { return mState; }
    [[nodiscard]] bool alive() const noexcept { return mState == ProcessState::Running; }
    [[nodiscard]] bool succeeded() const noexcept override { return mState == ProcessState::Succeeded; }
    [[nodiscard]] bool failed()    const noexcept { return mState == ProcessState::Failed; }
    [[nodiscard]] bool aborted()   const noexcept { return mState == ProcessState::Aborted; }

    // =========================================================================
    // IProcess implementation
    // =========================================================================

    bool tick(Delta delta, Data& data) override
    {
        if (mState == ProcessState::Uninitialized)
        {
            mState = ProcessState::Running;
            static_cast<Derived*>(this)->onInit();
        }

        if (mState == ProcessState::Running)
        {
            static_cast<Derived*>(this)->onUpdate(delta, data);
        }

        if (mState == ProcessState::Succeeded)
        {
            static_cast<Derived*>(this)->onSucceeded();
            return false; // done
        }
        if (mState == ProcessState::Failed)
        {
            static_cast<Derived*>(this)->onFailed();
            return false;
        }
        if (mState == ProcessState::Aborted)
        {
            static_cast<Derived*>(this)->onAborted();
            return false;
        }

        return true; // still running
    }

    void abort() override
    {
        if (mState == ProcessState::Running || mState == ProcessState::Uninitialized)
        {
            mState = ProcessState::Aborted;
            static_cast<Derived*>(this)->onAborted();
        }
    }

    void setNext(std::unique_ptr<IProcess<Delta, Data>> next) override
    {
        mNext = std::move(next);
    }

    [[nodiscard]] std::unique_ptr<IProcess<Delta, Data>> takeNext() override
    {
        return std::move(mNext);
    }

private:
    ProcessState mState{ProcessState::Uninitialized};
    std::unique_ptr<IProcess<Delta, Data>> mNext;
};

// =============================================================================
// ProcessHandle — returned by scheduler.attach(), enables .then() chaining
// =============================================================================

/**
 * @brief Fluent builder returned by ProcessScheduler::attach().
 *
 * Allows chaining successor processes:
 * @code
 *   scheduler.attach<FadeOut>(0.5f)
 *            .then<Explode>()
 *            .then<Cleanup>();
 * @endcode
 *
 * ProcessHandle holds a raw pointer to the tail of the current chain.
 * Attaching a new successor via then<T>() appends to the tail and updates
 * the handle to point to the new tail.
 */
template <typename Delta, typename Data>
class ProcessHandle
{
public:
    explicit ProcessHandle(IProcess<Delta, Data>* tail) noexcept
        : mTail(tail)
    {
    }

    /**
     * @brief Attach a successor process to the current tail.
     *
     * @tparam T     Process type (must inherit from Process<T, Delta, Data>).
     * @tparam Args  Constructor arguments for T.
     * @return       New handle pointing to the appended successor.
     */
    template <typename T, typename... Args>
    ProcessHandle then(Args&&... args)
    {
        auto next = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = next.get();
        if (mTail != nullptr)
        {
            mTail->setNext(std::move(next));
        }
        mTail = raw;
        return ProcessHandle(mTail);
    }

private:
    IProcess<Delta, Data>* mTail;
};

// =============================================================================
// ProcessScheduler<Delta, Data>
// =============================================================================

/**
 * @brief Owns and ticks a collection of process chains.
 *
 * @tparam Delta  Delta-time type passed to each process on every tick.
 * @tparam Data   Shared context passed by reference to each process tick.
 *                Defaults to void* (pass nullptr if unused).
 *
 * @code
 *   fatp_ecs::ProcessScheduler<float> sched;
 *
 *   sched.attach<FadeOutProcess>(0.3f)
 *        .then<SpawnExplosionProcess>();
 *
 *   // Game loop:
 *   while (running)
 *   {
 *       sched.update(dt);
 *   }
 * @endcode
 *
 * @note Thread-safety: NOT thread-safe. Drive from a single main-loop thread.
 */
template <typename Delta, typename Data = void*>
class ProcessScheduler
{
public:
    using ProcessPtr = std::unique_ptr<IProcess<Delta, Data>>;

    ProcessScheduler() = default;

    // Move-only: owns the process list.
    ProcessScheduler(const ProcessScheduler&) = delete;
    ProcessScheduler& operator=(const ProcessScheduler&) = delete;
    ProcessScheduler(ProcessScheduler&&) = default;
    ProcessScheduler& operator=(ProcessScheduler&&) = default;

    // =========================================================================
    // Process registration
    // =========================================================================

    /**
     * @brief Attach a new top-level process.
     *
     * @tparam T     Process type.
     * @tparam Args  Constructor arguments forwarded to T.
     * @return       ProcessHandle for chaining successors via .then<U>().
     */
    template <typename T, typename... Args>
    ProcessHandle<Delta, Data> attach(Args&&... args)
    {
        auto proc = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = proc.get();
        mProcesses.push_back(std::move(proc));
        return ProcessHandle<Delta, Data>(raw);
    }

    // =========================================================================
    // Execution
    // =========================================================================

    /**
     * @brief Tick all live processes once.
     *
     * Completed processes are removed. Successful processes have their
     * successor (if any) queued for the next update call.
     *
     * @param delta Time step passed to each process.
     * @param data  Shared context (default: nullptr).
     */
    void update(Delta delta, Data data = Data{})
    {
        // Process in-place: swap-and-pop for O(1) removal without invalidating
        // indices of unvisited elements. Successors are collected and appended
        // after the main loop so they start ticking from the next update call.
        std::vector<ProcessPtr> successors;

        std::size_t i = 0;
        while (i < mProcesses.size())
        {
            bool stillRunning = mProcesses[i]->tick(delta, data);

            if (!stillRunning)
            {
                if (mProcesses[i]->succeeded())
                {
                    auto next = mProcesses[i]->takeNext();
                    if (next != nullptr)
                    {
                        successors.push_back(std::move(next));
                    }
                }
                // Swap-and-pop
                mProcesses[i] = std::move(mProcesses.back());
                mProcesses.pop_back();
                // Do NOT increment i — re-check the swapped-in element.
            }
            else
            {
                ++i;
            }
        }

        for (auto& s : successors)
        {
            mProcesses.push_back(std::move(s));
        }
    }

    /**
     * @brief Abort all running processes immediately.
     *
     * Aborted processes have their aborted() hook called. No successors run.
     * The process list is cleared after all abort hooks fire.
     */
    void abortAll()
    {
        for (auto& proc : mProcesses)
        {
            proc->abort();
        }
        mProcesses.clear();
    }

    // =========================================================================
    // Queries
    // =========================================================================

    /// @brief Number of live processes (not counting queued successors).
    [[nodiscard]] std::size_t size() const noexcept { return mProcesses.size(); }

    /// @brief True if no processes are currently live.
    [[nodiscard]] bool empty() const noexcept { return mProcesses.empty(); }

private:
    std::vector<ProcessPtr> mProcesses;
};

} // namespace fatp_ecs
