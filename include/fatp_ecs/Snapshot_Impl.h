#pragma once

/**
 * @file Snapshot_Impl.h
 * @brief Out-of-line implementations for RegistrySnapshot, RegistrySnapshotLoader,
 *        and the Registry::snapshot() / Registry::snapshotLoader() factory methods.
 *
 * Included from FatpEcs.h after both Registry.h and Snapshot.h are fully
 * defined. Do not include this file directly.
 */

#include "Registry.h"
#include "Snapshot.h"

namespace fatp_ecs
{

// =============================================================================
// RegistrySnapshot out-of-line
// =============================================================================

inline RegistrySnapshot::RegistrySnapshot(const Registry& registry,
                                          fat_p::binary::Encoder& enc)
    : mRegistry(registry)
{
    // Write header immediately on construction so the stream is always in a
    // consistent state — the encoder is live from the moment the snapshot is created.
    enc.writeUint32(kHeaderMagic);
    enc.writeUint8(kVersion);

    const auto entities = mRegistry.allEntities();
    enc.writeUint32(static_cast<uint32_t>(entities.size()));
    for (Entity entity : entities)
    {
        enc.writeUint64(entity.get());
    }
}

// =============================================================================
// RegistrySnapshotLoader out-of-line
// =============================================================================

inline RegistrySnapshotLoader::RegistrySnapshotLoader(Registry& registry,
                                                      fat_p::binary::Decoder& dec)
    : mRegistry(registry)
{
    // Replace semantics: always start from a clean slate
    mRegistry.clear();

    // Validate header
    const uint32_t magic = dec.readUint32();
    if (magic != RegistrySnapshot::kHeaderMagic)
    {
        throw std::runtime_error(
            "RegistrySnapshotLoader: invalid snapshot magic (expected 0x46415053)");
    }

    const uint8_t version = dec.readUint8();
    if (version != RegistrySnapshot::kVersion)
    {
        throw std::runtime_error(
            "RegistrySnapshotLoader: unsupported snapshot version " +
            std::to_string(static_cast<int>(version)));
    }

    // Recreate all entities and build old→new translation map
    const uint32_t entityCount = dec.readUint32();
    for (uint32_t i = 0; i < entityCount; ++i)
    {
        const uint64_t rawOld = dec.readUint64();
        const Entity   oldEnt = Entity(rawOld);
        const Entity   newEnt = mRegistry.create();
        mEntityMap.insert(oldEnt, newEnt);
    }
}

// =============================================================================
// Registry factory methods
// =============================================================================

inline RegistrySnapshot Registry::snapshot(fat_p::binary::Encoder& enc)
{
    return RegistrySnapshot(*this, enc);
}

inline RegistrySnapshotLoader Registry::snapshotLoader(fat_p::binary::Decoder& dec)
{
    return RegistrySnapshotLoader(*this, dec);
}

} // namespace fatp_ecs
