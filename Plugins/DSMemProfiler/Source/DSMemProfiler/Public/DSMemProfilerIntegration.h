// Copyright (c) Project Team. All Rights Reserved.
#pragma once

/**
 * DSMemProfilerIntegration.h
 *
 * Drop-in integration guide for game modules.
 *
 * Step 1: Add "DSMemProfiler" to your module's PrivateDependencyModuleNames in .Build.cs
 *
 *   PrivateDependencyModuleNames.AddRange(new string[] { "DSMemProfiler" });
 *
 * Step 2: Include this header in any .cpp that does notable allocations:
 *
 *   #include "DSMemProfilerIntegration.h"
 *
 * Step 3: Wrap allocation-heavy code sections with DS_MEM_SCOPE:
 *
 *   void UMyAIController::RunBehaviorTree()
 *   {
 *       DS_MEM_SCOPE("AI");
 *       // Everything allocated in this stack frame is attributed to "AI"
 *       ...
 *   }
 *
 * The macro compiles to nothing on non-Linux-DS builds (zero overhead).
 *
 * Recommended tagging strategy (match your game's subsystems):
 *
 *   "AI"         — Behavior trees, perception, pathfinding
 *   "Inventory"  — Item data, containers, equipment
 *   "Network"    — RPC data, replication buffers
 *   "Physics"    — PhysX queries, collision responses
 *   "UI"         — Widgets, UMG (rare on DS but included for completeness)
 *   "GameState"  — Match data, player state arrays
 *   "Audio"      — Sound cues (can run on DS for 3D positional)
 *   "Combat"     — Damage calculation, projectile pools
 *   "Streaming"  — Level streaming, async load callbacks
 *
 * ---- Example: Tagging at component Tick level ----
 *
 *   void UInventoryComponent::TickComponent(...)
 *   {
 *       DS_MEM_SCOPE("Inventory");
 *       SyncWithDatabase();       // any allocs inside are tagged
 *       UpdateContainerCaches();
 *   }
 *
 * ---- Example: Tagging a request handler ----
 *
 *   void AMyGameMode::HandlePlayerJoin(APlayerController* PC)
 *   {
 *       DS_MEM_SCOPE("GameState");
 *       SpawnDefaultPawnFor(PC, ...);
 *       InitializePlayerInventory(PC);
 *   }
 *
 * ---- Reading results at runtime ----
 *
 *   // Via server console (connect with UE4 remote console or -ExecCmds):
 *   dsmem.snapshot MyLabel          // take + save snapshot
 *   dsmem.report live               // top modules by live bytes
 *   dsmem.report allocs             // top modules by alloc count (hot allocators)
 *   dsmem.diff                      // delta between last two periodic snapshots
 *   dsmem.startmonitor 5            // snapshot every 5 seconds
 *   dsmem.setwarnthreshold 20       // warn if module grows >20 MiB between snapshots
 *   dsmem.reset                     // reset all counters
 *
 * ---- Output files ----
 *
 *   <ProjectRoot>/Saved/MemProfiler/MemSnap_<timestamp>_<label>.csv
 *   <ProjectRoot>/Saved/MemProfiler/MemSnap_<timestamp>_<label>.json
 *
 * The CSV is directly importable into Excel / Google Sheets for charting.
 * The JSON can be fed to custom dashboards or monitoring pipelines.
 */

#include "DSMemModuleTag.h"
