#pragma once

/**
 * @file FatpEcs.h
 * @brief Umbrella header for the FAT-P ECS framework.
 */

// Core types
#include "Entity.h"
#include "ComponentMask.h"
#include "TypeId.h"
#include "ComponentStore.h"
#include "EventBus.h"
#include "Registry.h"
#include "View.h"

// Deferred operations (must follow Registry.h)
#include "CommandBuffer.h"
#include "CommandBuffer_Impl.h"

// Parallel execution
#include "Scheduler.h"
