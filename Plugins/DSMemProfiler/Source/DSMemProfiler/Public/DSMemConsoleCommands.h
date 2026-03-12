// Copyright (c) Project Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#if DS_MEM_PROFILER_ENABLED

/**
 * FDSMemConsoleCommands
 *
 * Registers the following console commands (available in Linux DS builds only):
 *
 *   dsmem.snapshot [label]     — take a snapshot, log to output, write CSV/JSON to Saved/MemProfiler/
 *   dsmem.report live          — print all modules sorted by live bytes
 *   dsmem.report allocs        — print all modules sorted by alloc count (finds hot allocators)
 *   dsmem.reset                — reset all counters (peak, total, counts)
 *   dsmem.startmonitor [sec]   — start periodic monitoring every <sec> seconds (default 10)
 *   dsmem.stopmonitor          — stop periodic monitoring
 *   dsmem.setwarnthreshold MB  — set growth anomaly warning threshold in MiB
 *   dsmem.diff                 — diff the last two snapshots and log delta per module
 */
class DSMEMPROFILER_API FDSMemConsoleCommands
{
public:
	FDSMemConsoleCommands();
	~FDSMemConsoleCommands();

private:
	TArray<IConsoleCommand*> RegisteredCommands;

	void RegisterAll();
	void UnregisterAll();
};

#endif // DS_MEM_PROFILER_ENABLED
