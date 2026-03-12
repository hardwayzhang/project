// Copyright (c) Project Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "DSMallocProfiler.h"

#if DS_MEM_PROFILER_ENABLED

/**
 * A point-in-time snapshot of one module's memory statistics.
 * Safe to hold, copy, and serialize from any thread.
 */
struct DSMEMPROFILER_API FDSModuleMemSnapshot
{
	FString ModuleName;
	int64   LiveBytes       { 0 };
	int64   PeakBytes       { 0 };
	int64   TotalAllocBytes { 0 };
	int64   AllocCount      { 0 };
	int64   FreeCount       { 0 };
	int64   LargeAllocCount { 0 };
	double  SnapshotTimeSec { 0.0 };

	/** Bytes that were allocated but never freed (TotalAllocBytes - freed bytes). */
	int64 GetLeakedEstimateBytes() const
	{
		return FMath::Max(0LL, TotalAllocBytes - (TotalAllocBytes - LiveBytes));
	}

	/** Average allocation size. */
	double GetAvgAllocBytes() const
	{
		return (AllocCount > 0) ? (double)TotalAllocBytes / (double)AllocCount : 0.0;
	}
};

/**
 * FDSMemSnapshot
 *
 * A complete snapshot of all modules at a single point in time.
 * Used for periodic reporting, delta diffing, and CSV/JSON export.
 */
struct DSMEMPROFILER_API FDSMemSnapshot
{
	double  TimestampSec    { 0.0 };
	int64   GlobalLiveBytes { 0 };
	int64   UntaggedBytes   { 0 };
	TArray<FDSModuleMemSnapshot> Modules;

	/** Compute diff between two snapshots (this - Baseline). */
	TArray<FDSModuleMemSnapshot> DiffModules(const FDSMemSnapshot& Baseline) const;

	/** Sort modules by LiveBytes descending. */
	void SortByLiveBytes();

	/** Sort modules by AllocCount descending (allocation frequency). */
	void SortByAllocCount();

	/** Export to CSV string. */
	FString ToCSV() const;

	/** Export to JSON string (for structured logging / external tools). */
	FString ToJSON() const;

	/** Print a human-readable summary to the log. */
	void LogSummary(const FString& Label) const;
};

/**
 * FDSMemMonitor
 *
 * Periodic monitoring service that:
 *  1. Takes memory snapshots at a configurable interval.
 *  2. Detects modules whose live bytes grew beyond a configurable threshold.
 *  3. Writes CSV snapshots to disk (<ProjectSavedDir>/MemProfiler/).
 *  4. Fires log warnings for anomalous modules.
 *
 * Lifecycle: created by the plugin module, ticked via FTicker.
 */
class DSMEMPROFILER_API FDSMemMonitor
{
public:
	FDSMemMonitor();
	~FDSMemMonitor();

	/** Start periodic monitoring (SnapshotIntervalSec > 0 required). */
	void Start(float SnapshotIntervalSec = 10.f);

	/** Stop monitoring (does not flush). */
	void Stop();

	/** Force an immediate snapshot and log it. */
	FDSMemSnapshot TakeSnapshot(const FString& Label = TEXT("Manual")) const;

	/** Take a snapshot and write it to disk. Returns the file path written. */
	FString TakeAndSaveSnapshot(const FString& Label = TEXT("Periodic"));

	/** Returns the most recent snapshot. */
	const FDSMemSnapshot& GetLastSnapshot() const { return LastSnapshot; }

	/** Returns all snapshots accumulated this session (may be large). */
	const TArray<FDSMemSnapshot>& GetHistory() const { return History; }

	/** Threshold: warn if a module's live bytes grew by more than this since last snapshot. */
	int64 GrowthWarnThresholdBytes{ 50 * 1024 * 1024 }; // 50 MiB

	/** Max history entries kept in memory. Older entries are evicted. */
	int32 MaxHistoryEntries{ 60 };

	/** Whether to write CSV files to disk on each periodic snapshot. */
	bool bWriteCSVToDisk{ true };

	static FDSMemMonitor& Get();

private:
	bool OnTick(float DeltaTime);
	void CheckGrowthAnomaly(const FDSMemSnapshot& NewSnapshot, const FDSMemSnapshot& PrevSnapshot) const;
	FString GetOutputDir() const;

	FTSTicker::FDelegateHandle TickerHandle;
	float SnapshotInterval{ 10.f };
	float TimeSinceLastSnapshot{ 0.f };

	FDSMemSnapshot LastSnapshot;
	TArray<FDSMemSnapshot> History;

	static FDSMemMonitor* GInstance;
};

#endif // DS_MEM_PROFILER_ENABLED
