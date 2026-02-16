#pragma once

/**
 * @file FatpEcs.h
 * @brief Umbrella header for the FAT-P ECS framework.
 *
 * Include this single header to get the full API:
 *
 * Phase 1: Entity, ComponentStore, Registry, View
 * Phase 2: EventBus, ComponentMask, CommandBuffer, Scheduler
 */

// Phase 1 - Core
#include "Entity.h"
#include "ComponentStore.h"
#include "ComponentMask.h"
#include "EventBus.h"
#include "Registry.h"
#include "View.h"

// Phase 2 - Events, Parallelism, Deferred Operations
#include "CommandBuffer.h"
#include "CommandBuffer_Impl.h"
#include "Scheduler.h"
