// Copyright (c) Project Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HAL/MallocAnsi.h"
#include "Containers/Map.h"
#include "Containers/Array.h"
#include "HAL/CriticalSection.h"
#include "DSMemModuleTag.h"

#if DS_MEM_PROFILER_ENABLED

/**
 * Per-module allocation statistics block.
 * Updated lock-free via atomics for hot-path alloc/free; reads are guarded by a shared lock.
 */
struct FDSModuleMemStats
{
	/** Unique tag name (e.g. "AI", "Inventory", "Network") */
	FName ModuleName;

	/** Current live bytes allocated under this tag */
	std::atomic<int64> LiveBytes{ 0 };

	/** Peak live bytes since last reset */
	std::atomic<int64> PeakBytes{ 0 };

	/** Total bytes ever allocated (monotone counter) */
	std::atomic<int64> TotalAllocBytes{ 0 };

	/** Total allocation call count */
	std::atomic<int64> AllocCount{ 0 };

	/** Total free call count */
	std::atomic<int64> FreeCount{ 0 };

	/** Number of allocations that exceeded FDSMallocProfiler::LargeAllocThresholdBytes */
	std::atomic<int64> LargeAllocCount{ 0 };

	void RecordAlloc(int64 Bytes)
	{
		int64 New = LiveBytes.fetch_add(Bytes, std::memory_order_relaxed) + Bytes;
		TotalAllocBytes.fetch_add(Bytes, std::memory_order_relaxed);
		AllocCount.fetch_add(1, std::memory_order_relaxed);

		int64 Prev = PeakBytes.load(std::memory_order_relaxed);
		while (New > Prev && !PeakBytes.compare_exchange_weak(Prev, New, std::memory_order_relaxed)) {}
	}

	void RecordFree(int64 Bytes)
	{
		LiveBytes.fetch_sub(Bytes, std::memory_order_relaxed);
		FreeCount.fetch_add(1, std::memory_order_relaxed);
	}

	void Reset()
	{
		LiveBytes.store(0, std::memory_order_relaxed);
		PeakBytes.store(0, std::memory_order_relaxed);
		TotalAllocBytes.store(0, std::memory_order_relaxed);
		AllocCount.store(0, std::memory_order_relaxed);
		FreeCount.store(0, std::memory_order_relaxed);
		LargeAllocCount.store(0, std::memory_order_relaxed);
	}
};

/**
 * FDSMallocProfiler
 *
 * Wraps the engine's default GMalloc to intercept every Malloc/Realloc/Free call.
 * Each call is attributed to the "current module tag" stored in a thread-local variable
 * (set via FDSMemModuleScope).
 *
 * Design goals:
 *  - Zero overhead when DS_MEM_PROFILER_ENABLED == 0 (entire class is #if'd out).
 *  - Minimal overhead when enabled: one TLS read + two atomic adds per alloc.
 *  - No heap allocation inside the profiler itself (uses pre-allocated slot array).
 *  - Safe to install very early (PostEngineInit) after GMalloc is stable.
 */
class DSMEMPROFILER_API FDSMallocProfiler : public FMalloc
{
public:
	/** Size threshold above which an allocation is flagged as a "large alloc" */
	static constexpr int64 LargeAllocThresholdBytes = 1 * 1024 * 1024; // 1 MiB

	/** Maximum distinct module tags supported (pre-allocated). */
	static constexpr int32 MaxModules = 128;

	explicit FDSMallocProfiler(FMalloc* InInner);
	virtual ~FDSMallocProfiler() override;

	// ---- FMalloc interface ----
	virtual void* Malloc(SIZE_T Count, uint32 Alignment) override;
	virtual void* Realloc(void* Original, SIZE_T Count, uint32 Alignment) override;
	virtual void  Free(void* Original) override;
	virtual SIZE_T GetAllocSize(void* Original) override;
	virtual bool  GetAllocationSize(void* Original, SIZE_T& SizeOut) override;
	virtual void  Trim(bool bTrimThreadCaches) override;
	virtual const TCHAR* GetDescriptiveName() override { return TEXT("DSMallocProfiler"); }

	// ---- Profiler API ----

	/** Install this profiler as GMalloc. Must be called from game thread. */
	static void Install();

	/** Remove the profiler wrapper and restore original GMalloc. */
	static void Uninstall();

	static FDSMallocProfiler* Get();

	/**
	 * Get or create a stats slot for the given module name.
	 * Thread-safe. Returns nullptr if MaxModules is reached.
	 */
	FDSModuleMemStats* GetOrCreateModuleStats(FName ModuleName);

	/** Returns a snapshot copy of all module stats (safe to read off game thread). */
	TArray<FDSModuleMemStats*> GetAllModuleStats() const;

	/** Returns the slot index for a given stats pointer (used by TLS scope push). */
	int32 GetSlotIndexForStats(const FDSModuleMemStats* Stats) const
	{
		if (!Stats) return -1;
		const ptrdiff_t Offset = Stats - Slots;
		if (Offset >= 0 && Offset < MaxModules)
		{
			return (int32)Offset;
		}
		return -1;
	}

	/** Reset peak + total counters for all modules. */
	void ResetStats();

	/** Returns global live bytes (sum of all tagged + untagged allocations). */
	int64 GetTotalLiveBytes() const { return GlobalLiveBytes.load(std::memory_order_relaxed); }

	/** Bytes allocated without any module scope active (engine/untracked code). */
	int64 GetUntaggedLiveBytes() const { return UntaggedStats.LiveBytes.load(std::memory_order_relaxed); }

private:
	FMalloc* Inner{ nullptr };

	// Pre-allocated stats slots (no heap alloc inside profiler)
	FDSModuleMemStats Slots[MaxModules];
	std::atomic<int32> SlotCount{ 0 };

	// Name -> slot index map (guarded by RegistryLock for writes; reads use slot index cache in TLS)
	mutable FRWLock RegistryLock;
	TMap<FName, int32> NameToSlotIndex;

	// Stats for allocations with no active scope tag
	FDSModuleMemStats UntaggedStats;

	// Global counter (all allocs regardless of tag)
	std::atomic<int64> GlobalLiveBytes{ 0 };

	// Per-allocation header stored just before the returned pointer to track size+tag
	struct FAllocHeader
	{
		uint32 Magic;         // 0xDEADC0DE sanity check
		int32  SlotIndex;     // -1 == untagged
		uint32 UserSize;      // user-requested bytes (capped at 4 GiB per alloc for header)
		uint32 _Pad;
	};
	static constexpr uint32 HeaderMagic = 0xDEADC0DE;
	static constexpr SIZE_T HeaderSize  = sizeof(FAllocHeader);

	/** Pointer to the single installed instance */
	static FDSMallocProfiler* GInstance;
};

#endif // DS_MEM_PROFILER_ENABLED
