#pragma once

/**
 * @file FatpEcs.h
 * @brief Umbrella header for the FAT-P ECS framework.
 */

// Core types (Phase 1)
#include "Entity.h"
#include "ComponentMask.h"
#include "TypeId.h"
#include "ComponentStore.h"
#include "EventBus.h"
#include "Registry.h"
#include "RuntimeView.h"
#include "View.h"

// Deferred operations (Phase 2 — must follow Registry.h)
#include "CommandBuffer.h"
#include "CommandBuffer_Impl.h"

// Parallel execution (Phase 2)
#include "Scheduler.h"

// Gameplay infrastructure (Phase 3)
#include "FrameAllocator.h"
#include "EntityNames.h"
#include "EntityTemplate.h"
#include "EntityTemplate_Impl.h"
#include "SystemToggle.h"
#include "SafeMath.h"
