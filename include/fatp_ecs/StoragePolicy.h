#pragma once

/**
 * @file StoragePolicy.h
 * @brief Storage policy concept and built-in policies for ComponentStore.
 *
 * A storage policy is a class template P<T> that names a container_type
 * and provides a static make() factory. It controls what container backs
 * the dense component data array inside ComponentStore<T, P>.
 *
 * Built-in policies
 * -----------------
 *   DefaultStoragePolicy          std::vector<T>                      zero overhead
 *   AlignedStoragePolicy<N>       fat_p::AlignedVector<T, N>          SIMD/cache-line aligned
 *   ConcurrentStoragePolicy<Lock> std::vector<T> guarded by Lock      thread-safe component writes
 *
 * Custom policy requirements
 * --------------------------
 * @code
 * template <typename T>
 * struct MyPolicy {
 *     using container_type = MyContainer<T>;
 *     static container_type make() { return {}; }
 * };
 * @endcode
 *
 * container_type must support:
 *   push_back(T&&), pop_back(),
 *   operator[](size_t) -> T&,
 *   data() -> T*,
 *   size() -> size_t,
 *   begin() / end()
 *
 * FAT-P headers used:
 *   AlignedVector.h       — AlignedStoragePolicy
 *   ConcurrencyPolicies.h — ConcurrentStoragePolicy
 */

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <vector>

#include <fat_p/AlignedVector.h>
#include <fat_p/ConcurrencyPolicies.h>

namespace fatp_ecs
{

// =============================================================================
// StorageContainer concept
// =============================================================================

template <typename C>
concept StorageContainer = requires(C c, typename C::value_type v, std::size_t i)
{
    typename C::value_type;
    c.push_back(std::move(v));
    c.pop_back();
    { c[i] }     -> std::convertible_to<typename C::value_type&>;
    { c.data() } -> std::convertible_to<typename C::value_type*>;
    { c.size() } -> std::convertible_to<std::size_t>;
    c.begin();
    c.end();
};

// =============================================================================
// StoragePolicy concept
// =============================================================================

template <template <typename> class P>
concept StoragePolicy =
    requires {
        typename P<char>::container_type;
        { P<char>::make() } -> std::same_as<typename P<char>::container_type>;
    } &&
    StorageContainer<typename P<char>::container_type>;

// =============================================================================
// DefaultStoragePolicy — std::vector<T>
// =============================================================================

template <typename T>
struct DefaultStoragePolicy
{
    using container_type = std::vector<T>;
    static container_type make() { return {}; }
};

static_assert(StoragePolicy<DefaultStoragePolicy>);

// =============================================================================
// AlignedStoragePolicy<Alignment> — fat_p::AlignedVector<T, Alignment>
//
// Usage: registry.useStorage<Transform, AlignedStoragePolicy<64>>();
// =============================================================================

template <std::size_t Alignment>
struct AlignedStoragePolicy
{
    template <typename T>
    struct Policy
    {
        using container_type = fat_p::AlignedVector<T, Alignment>;
        static container_type make() { return {}; }
    };
};

// Convenience alias so callers write AlignedStoragePolicy<64> directly as the
// template-template argument:
//   registry.useStorage<Transform, AlignedStoragePolicy<64>::Policy>();
// Or via the useAlignedStorage<T, N> shorthand on Registry.

static_assert(StoragePolicy<AlignedStoragePolicy<64>::Policy>);

// =============================================================================
// ConcurrentStoragePolicy<LockPolicy> — guarded std::vector<T>
//
// Usage: registry.useStorage<Health, ConcurrentStoragePolicy<fat_p::SharedMutexPolicy>::Policy>();
// =============================================================================

template <typename LockPolicy>
struct ConcurrentStoragePolicy
{
    template <typename T>
    struct Policy
    {
        class container_type
        {
        public:
            using value_type = T;

            container_type() = default;
            container_type(const container_type&) = delete;
            container_type& operator=(const container_type&) = delete;

            container_type(container_type&& o) noexcept
                : mData(std::move(o.mData)) {}

            container_type& operator=(container_type&& o) noexcept
            {
                if (this != &o) mData = std::move(o.mData);
                return *this;
            }

            // Structural mutations — called only from the single-threaded
            // Registry add/remove paths (exclusive access implied).
            void push_back(T&& v)        { auto g = mLock.lock(); mData.push_back(std::move(v)); }
            void push_back(const T& v)   { auto g = mLock.lock(); mData.push_back(v); }
            void pop_back()              { auto g = mLock.lock(); mData.pop_back(); }

            // Mutable read — exclusive (caller may write through the ref)
            T& operator[](std::size_t i)       { auto g = mLock.lock(); return mData[i]; }
            // Const read — shared
            const T& operator[](std::size_t i) const { auto g = mLock.lock_shared(); return mData[i]; }

            // Raw pointer access — caller manages lifetime / external locking
            T*       data() noexcept       { return mData.data(); }
            const T* data() const noexcept { return mData.data(); }

            std::size_t size()  const noexcept { return mData.size(); }
            bool        empty() const noexcept { return mData.empty(); }

            auto begin() noexcept       { return mData.begin(); }
            auto end()   noexcept       { return mData.end(); }
            auto begin() const noexcept { return mData.begin(); }
            auto end()   const noexcept { return mData.end(); }

            // RAII lock access for scoped operations
            [[nodiscard]] auto readLock()  const { return mLock.lock_shared(); }
            [[nodiscard]] auto writeLock()       { return mLock.lock(); }

        private:
            std::vector<T>     mData;
            mutable LockPolicy mLock;
        };

        static container_type make() { return {}; }
    };
};

static_assert(StoragePolicy<ConcurrentStoragePolicy<fat_p::SingleThreadedPolicy>::Policy>);

} // namespace fatp_ecs
