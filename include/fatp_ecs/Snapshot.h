#pragma once

/**
 * @file Snapshot.h
 * @brief Save and restore full registry state via user-provided per-component callbacks.
 */

// Overview:
//
// RegistrySnapshot serializes the live entity set and any registered component
// stores into a flat binary buffer. RegistrySnapshotLoader deserializes that
// buffer into a cleared registry, recreating entities and components.
//
// Entity handle remapping (Path A):
//   Serialized entities are stored as raw uint64_t values (packed index+generation).
//   On restore each serialized entity is recreated via registry.create(), producing
//   a fresh handle. A temporary EntityMap records old Entity -> new Entity so user
//   callbacks can fix up cross-entity references inside component data.
//
// Type-erasure:
//   IComponentStore has no knowledge of T, so it cannot call user callbacks.
//   serializeComponent<T>() / deserializeComponent<T>() are template methods on
//   the snapshot objects themselves; they resolve T at instantiation time and
//   operate directly on typed ComponentStore<T> accessed via Registry::tryGetStore<T>().
//
// Wire format (all integers little-endian, typed via BinaryLite tags):
//
//   [Header]
//     magic:   uint32  (0x46415053 == "FAPS")
//     version: uint8   (1)
//
//   [Entity table]
//     count:   uint32
//     N x entity: uint64   (raw Entity::get() value -- index+generation packed)
//
//   [Component blocks]  -- one block per serializeComponent<T>() call, in order
//     typeId:  uint32
//     count:   uint32
//     N x {
//       entity: uint64        (raw old Entity value)
//       blob:   bytes         (BinaryLite Bytes tag + user-written content)
//     }
//
//   [Footer]
//     magic:   uint32  (0x454E4400 == "END\0")
//
// FAT-P components used:
//   - BinaryLite (Encoder/Decoder): little-endian serialization without the
//     full FatPBinary stack. Each value carries a type tag for integrity.
//   - FastHashMap: EntityMap lookup O(1) per component entry; discarded after restore.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <fat_p/BinaryLite.h>
#include <fat_p/FastHashMap.h>

#include "ComponentStore.h"
#include "Entity.h"
#include "TypeId.h"

namespace fatp_ecs
{

class Registry;

// =============================================================================
// EntityMap -- old-to-new entity translation used during restore
// =============================================================================

/**
 * @brief Maps serialized (old) entity handles to freshly created (new) handles.
 *
 * Passed by const-ref to every deserializeComponent<T>() callback so the user
 * can remap cross-entity component fields.
 *
 * @example
 * @code
 *   loader.deserializeComponent<Parent>(dec,
 *       [](fat_p::binary::Decoder& d, const EntityMap& remap) -> Parent {
 *           Entity old = Entity(d.readRaw<uint64_t>());
 *           return Parent{ remap.translate(old) };
 *       });
 * @endcode
 *
 * @note Thread-safety: NOT thread-safe.
 */
class EntityMap
{
public:
    /// @brief Returns the new entity corresponding to oldEntity, or NullEntity
    ///        if oldEntity was not part of the snapshot.
    [[nodiscard]] Entity translate(Entity oldEntity) const noexcept
    {
        const Entity* found = mMap.find(oldEntity.get());
        return found ? *found : NullEntity;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return mMap.size();
    }

    // Internal: called by RegistrySnapshotLoader during construction.
    void insert(Entity oldEntity, Entity newEntity)
    {
        mMap.insert(oldEntity.get(), newEntity);
    }

private:
    fat_p::FastHashMap<uint64_t, Entity> mMap;
};

// =============================================================================
// RegistrySnapshot -- save side
// =============================================================================

/**
 * @brief Serializes registry state into a binary buffer.
 *
 * Obtained from Registry::snapshot(enc). The header and entity table are written
 * to enc immediately on construction. Call serializeComponent<T>() for each
 * component type to include, then finalize(enc) to write the footer.
 *
 * @code
 *   std::vector<uint8_t> buf;
 *   fat_p::binary::Encoder enc(buf);
 *
 *   auto snap = registry.snapshot(enc);
 *   snap.serializeComponent<Position>(enc,
 *       [](fat_p::binary::Encoder& e, const Position& p) {
 *           e.writeFloat(p.x);
 *           e.writeFloat(p.y);
 *       });
 *   snap.finalize(enc);
 *   // buf now contains the complete snapshot.
 * @endcode
 *
 * @note Thread-safety: NOT thread-safe.
 */
class RegistrySnapshot
{
public:
    template <typename T>
    using SerializeFn = std::function<void(fat_p::binary::Encoder&, const T&)>;

    // Use Registry::snapshot(enc), not this constructor directly.
    RegistrySnapshot(const Registry& registry, fat_p::binary::Encoder& enc);

    RegistrySnapshot(const RegistrySnapshot&) = delete;
    RegistrySnapshot& operator=(const RegistrySnapshot&) = delete;
    RegistrySnapshot(RegistrySnapshot&&) = default;
    RegistrySnapshot& operator=(RegistrySnapshot&&) = delete;

    /**
     * @brief Serialize all instances of component T.
     *
     * Writes a component block: typeId, count, then per-entity: the raw entity
     * value followed by a Bytes-tagged blob containing whatever the callback wrote.
     * Entities that lack T are never visited. A zero-count block is written if T
     * has never been added to any entity, keeping save/load block counts aligned.
     */
    template <typename T>
    void serializeComponent(fat_p::binary::Encoder& enc, SerializeFn<T> fn)
    {
        const TypeId tid = typeId<T>();
        enc.writeUint32(static_cast<uint32_t>(tid));

        const ComponentStore<T>* store = mRegistry.template tryGetStore<T>();
        if (store == nullptr || store->empty())
        {
            enc.writeUint32(0u);
            return;
        }

        enc.writeUint32(static_cast<uint32_t>(store->size()));

        const Entity*     ents = store->denseEntities();
        const std::size_t n    = store->denseEntityCount();

        for (std::size_t i = 0; i < n; ++i)
        {
            enc.writeUint64(ents[i].get());

            // Serialize component into a scratch buffer, then emit as a
            // length-framed Bytes block so the loader can skip unknown types.
            std::vector<uint8_t> blobBuf;
            fat_p::binary::Encoder blobEnc(blobBuf);
            fn(blobEnc, store->dataAt(i));
            enc.writeBytes(blobBuf);
        }
    }

    /// @brief Write the snapshot footer. Call after all serializeComponent() calls.
    void finalize(fat_p::binary::Encoder& enc) const
    {
        enc.writeUint32(kFooterMagic);
    }

    static constexpr uint32_t kHeaderMagic = 0x46415053u; // "FAPS"
    static constexpr uint8_t  kVersion     = 1u;
    static constexpr uint32_t kFooterMagic = 0x454E4400u; // "END\0"

private:
    const Registry& mRegistry;
};

// =============================================================================
// RegistrySnapshotLoader -- restore side
// =============================================================================

/**
 * @brief Restores registry state from a buffer produced by RegistrySnapshot.
 *
 * Obtained from Registry::snapshotLoader(dec). The registry is cleared and all
 * entities are recreated during construction. Call deserializeComponent<T>() for
 * each block in stream order. Call finalize(dec) to verify the footer.
 *
 * @code
 *   fat_p::binary::Decoder dec(buf);
 *   auto loader = registry.snapshotLoader(dec);
 *
 *   loader.deserializeComponent<Position>(dec,
 *       [](fat_p::binary::Decoder& d, const EntityMap&) -> Position {
 *           return { d.readFloat(), d.readFloat() };
 *       });
 *   loader.finalize(dec);
 * @endcode
 *
 * @note Thread-safety: NOT thread-safe.
 */
class RegistrySnapshotLoader
{
public:
    template <typename T>
    using DeserializeFn = std::function<T(fat_p::binary::Decoder&, const EntityMap&)>;

    // Use Registry::snapshotLoader(dec), not this constructor directly.
    // Clears the registry, reads header, recreates all entities.
    // Throws std::runtime_error on corrupt header or version mismatch.
    RegistrySnapshotLoader(Registry& registry, fat_p::binary::Decoder& dec);

    RegistrySnapshotLoader(const RegistrySnapshotLoader&) = delete;
    RegistrySnapshotLoader& operator=(const RegistrySnapshotLoader&) = delete;
    RegistrySnapshotLoader(RegistrySnapshotLoader&&) = default;
    RegistrySnapshotLoader& operator=(RegistrySnapshotLoader&&) = delete;

    /**
     * @brief Read and restore one component block.
     *
     * Reads the next block header. If typeId matches T, each entity's blob is
     * decoded and the component is added via registry.add<T>(). If typeId does
     * not match, the entire block is skipped (blobs consumed, no components added).
     */
    template <typename T>
    void deserializeComponent(fat_p::binary::Decoder& dec, DeserializeFn<T> fn)
    {
        const uint32_t storedTypeId = dec.readUint32();
        const uint32_t count        = dec.readUint32();

        if (storedTypeId != static_cast<uint32_t>(typeId<T>()))
        {
            // Type mismatch: skip the whole block
            for (uint32_t i = 0; i < count; ++i)
            {
                dec.readUint64(); // entity raw value
                dec.readBytes();  // blob (tagged Bytes -- consumed and discarded)
            }
            return;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            const uint64_t              rawOld  = dec.readUint64();
            const std::vector<uint8_t>  blobBuf = dec.readBytes();

            const Entity oldEntity = Entity(rawOld);
            const Entity newEntity = mEntityMap.translate(oldEntity);

            if (newEntity == NullEntity)
            {
                continue; // entity not in snapshot table -- blob already consumed
            }

            fat_p::binary::Decoder blobDec(blobBuf);
            T component = fn(blobDec, mEntityMap);
            mRegistry.add<T>(newEntity, std::move(component));
        }
    }

    /// @brief Verify footer magic. Throws on mismatch. Call after all deserializeComponent().
    void finalize(fat_p::binary::Decoder& dec) const
    {
        const uint32_t footer = dec.readUint32();
        if (footer != RegistrySnapshot::kFooterMagic)
        {
            throw std::runtime_error(
                "RegistrySnapshotLoader::finalize: corrupt footer magic");
        }
    }

    /// @brief Old-to-new entity map. Valid after construction.
    [[nodiscard]] const EntityMap& entityMap() const noexcept
    {
        return mEntityMap;
    }

private:
    Registry& mRegistry;
    EntityMap mEntityMap;
};

} // namespace fatp_ecs

// Out-of-line implementations requiring the full Registry definition.
#include "Snapshot_Impl.h"
