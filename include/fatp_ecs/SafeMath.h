#pragma once

/**
 * @file SafeMath.h
 * @brief Overflow-safe arithmetic wrappers for gameplay calculations.
 */

// FAT-P components used:
// - CheckedArithmetic: Overflow-detecting integer arithmetic
//   - checked_add, checked_sub, checked_mul with policy-based error handling
//
// Game logic frequently does integer math that must not silently overflow:
// health clamping, damage calculation, score accumulation, resource counts.
// SafeMath provides thin wrappers that use CheckedArithmetic with a
// clamping policy (saturate at min/max instead of throwing) for the
// common gameplay case, plus the raw checked_* functions for cases
// where overflow should be an error.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <fat_p/CheckedArithmetic.h>

namespace fatp_ecs
{

// =============================================================================
// Clamped Arithmetic — saturates instead of overflowing
// =============================================================================

// These are the bread-and-butter for gameplay math. Health can't go below 0
// or above max. Score can't overflow. Resource counts stay in bounds.

/**
 * @brief Add with saturation at numeric limits.
 *
 * @tparam T Integral type.
 * @param a First operand.
 * @param b Second operand.
 * @return a + b, clamped to [min, max] of T.
 */
template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr T clampedAdd(T a, T b) noexcept
{
    auto result = fat_p::checked_add<fat_p::ReturnExpectedPolicy>(a, b);
    if (result.has_value())
    {
        return result.value();
    }
    return (b > 0) ? std::numeric_limits<T>::max()
                   : std::numeric_limits<T>::min();
}

/**
 * @brief Subtract with saturation at numeric limits.
 *
 * @tparam T Integral type.
 * @param a First operand.
 * @param b Second operand.
 * @return a - b, clamped to [min, max] of T.
 */
template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr T clampedSub(T a, T b) noexcept
{
    auto result = fat_p::checked_sub<fat_p::ReturnExpectedPolicy>(a, b);
    if (result.has_value())
    {
        return result.value();
    }
    return (b > 0) ? std::numeric_limits<T>::min()
                   : std::numeric_limits<T>::max();
}

/**
 * @brief Multiply with saturation at numeric limits.
 *
 * @tparam T Integral type.
 * @param a First operand.
 * @param b Second operand.
 * @return a * b, clamped to [min, max] of T.
 */
template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr T clampedMul(T a, T b) noexcept
{
    auto result = fat_p::checked_mul<fat_p::ReturnExpectedPolicy>(a, b);
    if (result.has_value())
    {
        return result.value();
    }
    bool sameSign = (a > 0) == (b > 0);
    return sameSign ? std::numeric_limits<T>::max()
                    : std::numeric_limits<T>::min();
}

// =============================================================================
// Gameplay Helpers
// =============================================================================

/**
 * @brief Apply damage to a health value, clamping to [0, maxHp].
 *
 * @param currentHp Current health.
 * @param damage    Damage to apply (positive = hurt, negative = heal).
 * @param maxHp     Maximum health cap.
 * @return New health value in [0, maxHp].
 */
[[nodiscard]] inline constexpr int applyDamage(int currentHp,
                                               int damage,
                                               int maxHp) noexcept
{
    int result = clampedSub(currentHp, damage);
    return std::clamp(result, 0, maxHp);
}

/**
 * @brief Apply healing to a health value, clamping to [0, maxHp].
 *
 * @param currentHp Current health.
 * @param healing   Amount to heal.
 * @param maxHp     Maximum health cap.
 * @return New health value in [0, maxHp].
 */
[[nodiscard]] inline constexpr int applyHealing(int currentHp,
                                                int healing,
                                                int maxHp) noexcept
{
    int result = clampedAdd(currentHp, healing);
    return std::clamp(result, 0, maxHp);
}

/**
 * @brief Accumulate score safely (saturates at INT32_MAX).
 *
 * @param currentScore Current score.
 * @param points       Points to add.
 * @return New score, saturated at max int.
 */
[[nodiscard]] inline constexpr int addScore(int currentScore,
                                            int points) noexcept
{
    return clampedAdd(currentScore, points);
}

} // namespace fatp_ecs
