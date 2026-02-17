#pragma once

/**
 * @file SystemToggle.h
 * @brief Runtime system toggling backed by FeatureManager.
 */

// FAT-P components used:
// - FeatureManager: Feature flag management with dependency tracking
// - Expected: Error handling for feature registration
//
// SystemToggle wraps FeatureManager to provide runtime enable/disable of
// ECS systems. The Scheduler checks isEnabled() before executing each
// system, allowing gameplay toggles like disabling physics, enabling
// debug rendering, or pausing AI without recompiling.

#include <string>

#include <fat_p/FeatureManager.h>

namespace fatp_ecs
{

/**
 * @brief Runtime system toggle backed by FeatureManager.
 *
 * @note Thread-safety: Thread-safe (FeatureManager uses internal synchronization).
 */
class SystemToggle
{
public:
    SystemToggle() = default;

    /**
     * @brief Register a system as a toggleable feature.
     *
     * @param systemName Name matching the system's registration name.
     * @param enabled    Initial state (default: enabled).
     * @return true if registration succeeded.
     */
    bool registerSystem(const std::string& systemName, bool enabled = true)
    {
        auto result = mFeatures.addFeature(systemName);
        if (!result.has_value())
        {
            return false;
        }

        if (enabled)
        {
            auto enableResult = mFeatures.enable(systemName);
            return enableResult.has_value();
        }
        return true;
    }

    /// @brief Check if a system is currently enabled.
    [[nodiscard]] bool isEnabled(const std::string& systemName) const
    {
        return mFeatures.isEnabled(systemName);
    }

    /**
     * @brief Enable a system.
     *
     * @param systemName The system to enable.
     * @return true if the state changed.
     */
    bool enable(const std::string& systemName)
    {
        auto result = mFeatures.enable(systemName);
        return result.has_value();
    }

    /**
     * @brief Disable a system.
     *
     * @param systemName The system to disable.
     * @return true if the state changed.
     */
    bool disable(const std::string& systemName)
    {
        auto result = mFeatures.disable(systemName);
        return result.has_value();
    }

    /// @brief Direct access to the underlying FeatureManager.
    [[nodiscard]] fat_p::feature::FeatureManager<>& features() noexcept
    {
        return mFeatures;
    }

private:
    fat_p::feature::FeatureManager<> mFeatures;
};

} // namespace fatp_ecs
