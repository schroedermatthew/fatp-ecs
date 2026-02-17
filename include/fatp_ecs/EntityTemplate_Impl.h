#pragma once

/**
 * @file EntityTemplate_Impl.h
 * @brief Implementation of EntityTemplate methods that depend on Registry.
 *
 * @details
 * Include order: Registry.h, EntityTemplate.h, EntityTemplate_Impl.h.
 * Or just include FatpEcs.h which handles the order correctly.
 */

#include "EntityTemplate.h"
#include "Registry.h"

namespace fatp_ecs
{

inline Entity TemplateRegistry::spawn(Registry& registry,
                                      const std::string& templateName) const
{
    auto* tmpl = mTemplates.find(templateName);
    if (tmpl == nullptr)
    {
        return NullEntity;
    }

    Entity entity = registry.create();

    for (const auto& [compName, compData] : tmpl->components)
    {
        auto* factory = mFactories.find(compName);
        if (factory != nullptr)
        {
            (*factory)(registry, entity, compData);
        }
    }

    return entity;
}

} // namespace fatp_ecs
