#pragma once

/**
 * @file EntityNames.h
 * @brief Named entity registry backed by StringPool and FlatMap.
 */

// FAT-P components used:
// - StringPool: Intern entity names for pointer-comparison equality
// - FlatMap: Sorted name-to-entity mapping for debug/editor lookups
// - FastHashMap: Entity-to-name reverse mapping for O(1) lookup by entity
//
// EntityNames provides bidirectional name<->entity mapping. Names are interned
// via StringPool so comparison is pointer equality (O(1) vs O(n) strcmp).
// The FlatMap provides sorted iteration for debug UIs. FastHashMap provides
// O(1) reverse lookup from entity to name.

#include <cstddef>
#include <string>
#include <string_view>

#include <fat_p/FastHashMap.h>
#include <fat_p/FlatMap.h>
#include <fat_p/StringPool.h>

#include "Entity.h"

namespace fatp_ecs
{

/**
 * @brief Bidirectional name<->entity mapping with interned strings.
 *
 * @note Thread-safety: NOT thread-safe.
 */
class EntityNames
{
public:
    EntityNames() = default;

    /**
     * @brief Assign a name to an entity.
     *
     * @param entity The entity to name.
     * @param name   The name string (interned automatically).
     * @return The interned name pointer, or nullptr if the name is already taken.
     */
    const char* setName(Entity entity, std::string_view name)
    {
        const char* interned = mPool.intern(name);
        std::string key(interned);

        // Check if name is already assigned to a different entity
        auto existingIt = mNameToEntity.find(key);
        if (existingIt != mNameToEntity.end() && existingIt->second != entity)
        {
            return nullptr;
        }

        // Remove any previous name for this entity
        auto* oldName = mEntityToName.find(entity);
        if (oldName != nullptr)
        {
            mNameToEntity.erase(std::string(*oldName));
        }

        mNameToEntity.insert_or_assign(key, entity);
        mEntityToName.insert_or_assign(entity, interned);
        return interned;
    }

    /// @brief Find an entity by name.
    [[nodiscard]] Entity findByName(std::string_view name) const
    {
        const char* interned = mPool.find(name);
        if (interned == nullptr)
        {
            return NullEntity;
        }
        std::string key(interned);
        auto it = mNameToEntity.find(key);
        if (it == mNameToEntity.end())
        {
            return NullEntity;
        }
        return it->second;
    }

    /// @brief Get the name of an entity, or nullptr if unnamed.
    [[nodiscard]] const char* getName(Entity entity) const
    {
        auto* name = mEntityToName.find(entity);
        if (name == nullptr)
        {
            return nullptr;
        }
        return *name;
    }

    /// @brief Remove an entity's name mapping.
    bool removeName(Entity entity)
    {
        auto* name = mEntityToName.find(entity);
        if (name == nullptr)
        {
            return false;
        }
        mNameToEntity.erase(std::string(*name));
        mEntityToName.erase(entity);
        return true;
    }

    /// @brief Number of named entities.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return mEntityToName.size();
    }

    /// @brief Clear all name mappings. Does NOT clear the string pool.
    void clear()
    {
        mNameToEntity.clear();
        mEntityToName.clear();
    }

private:
    fat_p::StringPool<> mPool;
    fat_p::FlatMap<std::string, Entity> mNameToEntity;
    fat_p::FastHashMap<Entity, const char*> mEntityToName;
};

} // namespace fatp_ecs
