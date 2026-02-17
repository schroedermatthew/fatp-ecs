#pragma once

/**
 * @file EntityTemplate.h
 * @brief JSON-driven entity templates for data-driven spawning.
 */

// FAT-P components used:
// - JsonLite: Parse entity template definitions from JSON strings
// - FastHashMap: Template registry keyed by name
//
// EntityTemplate stores a parsed JSON object describing an entity archetype.
// A ComponentFactory callback system lets users register per-component-type
// deserializers that read from JsonValue and call registry.add<T>().
//
// JSON format:
// {
//   "components": {
//     "Position": { "x": 10.0, "y": 20.0 },
//     "Health":   { "current": 100, "max": 100 }
//   }
// }

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fat_p/FastHashMap.h>
#include <fat_p/JsonLite.h>

#include "Entity.h"

namespace fatp_ecs
{

class Registry;

// =============================================================================
// Component Factory
// =============================================================================

// A factory function that reads a JsonValue and applies a component to an
// entity. Users register one per component type name.
using ComponentFactory = std::function<void(Registry&, Entity, const fat_p::JsonValue&)>;

// =============================================================================
// EntityTemplate
// =============================================================================

/// @brief A parsed entity template that can stamp out entities from JSON data.
struct EntityTemplate
{
    std::string name;
    fat_p::JsonObject components;
};

// =============================================================================
// TemplateRegistry
// =============================================================================

/**
 * @brief Registry of entity templates and component factories.
 *
 * @note Thread-safety: NOT thread-safe.
 */
class TemplateRegistry
{
public:
    TemplateRegistry() = default;

    // =========================================================================
    // Component Factory Registration
    // =========================================================================

    /**
     * @brief Register a factory for a named component type.
     *
     * @param componentName The JSON key for this component (e.g., "Position").
     * @param factory       Callback that adds the component to an entity.
     */
    void registerComponent(std::string componentName, ComponentFactory factory)
    {
        mFactories.insert_or_assign(std::move(componentName), std::move(factory));
    }

    // =========================================================================
    // Template Management
    // =========================================================================

    /**
     * @brief Parse and register an entity template from a JSON string.
     *
     * @param name     Template name for later lookup.
     * @param jsonStr  JSON string describing the template.
     * @return true if parsing and registration succeeded.
     */
    bool addTemplate(std::string name, std::string_view jsonStr)
    {
        fat_p::JsonValue parsed = fat_p::parse_json(jsonStr);
        if (!parsed.is_object())
        {
            return false;
        }

        auto& obj = std::get<fat_p::JsonObject>(parsed);
        auto compIt = obj.find("components");
        if (compIt == obj.end() || !compIt->second.is_object())
        {
            return false;
        }

        EntityTemplate tmpl;
        tmpl.name = name;
        tmpl.components = std::get<fat_p::JsonObject>(compIt->second);

        mTemplates.insert_or_assign(std::move(name), std::move(tmpl));
        return true;
    }

    /**
     * @brief Spawn an entity from a named template.
     *
     * @param registry      The ECS registry to create the entity in.
     * @param templateName  Name of a previously registered template.
     * @return The created entity, or NullEntity if template not found.
     */
    Entity spawn(Registry& registry, const std::string& templateName) const;

    /// @brief Check if a template exists.
    [[nodiscard]] bool hasTemplate(const std::string& name) const
    {
        return mTemplates.find(name) != nullptr;
    }

    /// @brief Number of registered templates.
    [[nodiscard]] std::size_t templateCount() const noexcept
    {
        return mTemplates.size();
    }

    /// @brief Number of registered component factories.
    [[nodiscard]] std::size_t factoryCount() const noexcept
    {
        return mFactories.size();
    }

private:
    fat_p::FastHashMap<std::string, EntityTemplate> mTemplates;
    fat_p::FastHashMap<std::string, ComponentFactory> mFactories;
};

} // namespace fatp_ecs
