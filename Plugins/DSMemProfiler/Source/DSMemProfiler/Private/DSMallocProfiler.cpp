// Copyright (c) Project Team. All Rights Reserved.
#include "DSMallocProfiler.h"

#if DS_MEM_PROFILER_ENABLED

#include "DSMemModuleTag.h"
#include "HAL/MallocAnsi.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeRWLock.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogDSMalloc, Log, All);

FDSMallocProfiler* FDSMallocProfiler::GInstance = nullptr;

// ---------------------------------------------------------------------------
// Construction / Installation
// ---------------------------------------------------------------------------

FDSMallocProfiler::FDSMallocProfiler(FMalloc* InInner)
	: Inner(InInner)
{
	UntaggedStats.ModuleName = FName("__Untagged__");
}

FDSMallocProfiler::~FDSMallocProfiler()
{
}

void FDSMallocProfiler::Install()
{
	if (GInstance)
	{
		UE_LOG(LogDSMalloc, Warning, TEXT("DSMallocProfiler already installed."));
		return;
	}

	// GMalloc must exist before we wrap it
	check(GMalloc);

	GInstance = new FDSMallocProfiler(GMalloc);
	GMalloc = GInstance;

	UE_LOG(LogDSMalloc, Log, TEXT("DSMallocProfiler installed (wrapping %s). HeaderSize=%d bytes."),
		GInstance->Inner->GetDescriptiveName(), (int32)HeaderSize);
}

void FDSMallocProfiler::Uninstall()
{
	if (!GInstance)
	{
		return;
	}

	check(GMalloc == GInstance);
	GMalloc = GInstance->Inner;

	// Do NOT delete GInstance — outstanding alloc headers still reference it
	GInstance = nullptr;

	UE_LOG(LogDSMalloc, Log, TEXT("DSMallocProfiler uninstalled."));
}

FDSMallocProfiler* FDSMallocProfiler::Get()
{
	return GInstance;
}

// ---------------------------------------------------------------------------
// Module Stats Registry
// ---------------------------------------------------------------------------

FDSModuleMemStats* FDSMallocProfiler::GetOrCreateModuleStats(FName ModuleName)
{
	// Fast path: try read lock first
	{
		FReadScopeLock ReadLock(RegistryLock);
		if (const int32* Idx = NameToSlotIndex.Find(ModuleName))
		{
			return &Slots[*Idx];
		}
	}

	// Slow path: write lock + double-check
	FWriteScopeLock WriteLock(RegistryLock);
	if (const int32* Idx = NameToSlotIndex.Find(ModuleName))
	{
		return &Slots[*Idx];
	}

	int32 NewIdx = SlotCount.fetch_add(1, std::memory_order_relaxed);
	if (NewIdx >= MaxModules)
	{
		SlotCount.fetch_sub(1, std::memory_order_relaxed);
		UE_LOG(LogDSMalloc, Error,
			TEXT("DSMallocProfiler: MaxModules (%d) exceeded, tag '%s' will be untracked."),
			MaxModules, *ModuleName.ToString());
		return nullptr;
	}

	Slots[NewIdx].ModuleName = ModuleName;
	NameToSlotIndex.Add(ModuleName, NewIdx);
	return &Slots[NewIdx];
}

TArray<FDSModuleMemStats*> FDSMallocProfiler::GetAllModuleStats() const
{
	TArray<FDSModuleMemStats*> Out;
	int32 Count = SlotCount.load(std::memory_order_relaxed);
	Out.Reserve(Count + 1);

	for (int32 i = 0; i < Count; ++i)
	{
		Out.Add(const_cast<FDSModuleMemStats*>(&Slots[i]));
	}
	Out.Add(const_cast<FDSModuleMemStats*>(&UntaggedStats));
	return Out;
}

void FDSMallocProfiler::ResetStats()
{
	int32 Count = SlotCount.load(std::memory_order_relaxed);
	for (int32 i = 0; i < Count; ++i)
	{
		Slots[i].Reset();
	}
	UntaggedStats.Reset();
	UE_LOG(LogDSMalloc, Log, TEXT("DSMallocProfiler stats reset."));
}

// ---------------------------------------------------------------------------
// FMalloc interface — hot path
// ---------------------------------------------------------------------------

void* FDSMallocProfiler::Malloc(SIZE_T Count, uint32 Alignment)
{
	if (Count == 0)
	{
		return nullptr;
	}

	// Allocate extra space for our header. We ensure alignment is at least
	// alignof(FAllocHeader) and the user pointer is aligned to 'Alignment'.
	const SIZE_T TotalAlignment = FMath::Max(Alignment, (uint32)alignof(FAllocHeader));
	const SIZE_T TotalSize      = HeaderSize + Count + TotalAlignment; // generous padding

	uint8* RawPtr = (uint8*)Inner->Malloc(TotalSize, TotalAlignment);
	if (!RawPtr)
	{
		return nullptr;
	}

	// Place header at RawPtr, then align user pointer
	FAllocHeader* Header = (FAllocHeader*)RawPtr;
	uint8* UserPtr = (uint8*)Align(RawPtr + HeaderSize, Alignment ? Alignment : 1);

	int32 SlotIdx = FDSMemModuleScope::GetCurrentSlotIndex();

	Header->Magic    = HeaderMagic;
	Header->SlotIndex = SlotIdx;
	Header->UserSize = (uint32)FMath::Min<SIZE_T>(Count, MAX_uint32);
	Header->_Pad     = 0;

	// Record stats
	if (SlotIdx >= 0 && SlotIdx < SlotCount.load(std::memory_order_relaxed))
	{
		Slots[SlotIdx].RecordAlloc((int64)Count);
		if ((int64)Count >= LargeAllocThresholdBytes)
		{
			Slots[SlotIdx].LargeAllocCount.fetch_add(1, std::memory_order_relaxed);
		}
	}
	else
	{
		UntaggedStats.RecordAlloc((int64)Count);
	}
	GlobalLiveBytes.fetch_add((int64)Count, std::memory_order_relaxed);

	// Store back-pointer from user pointer to header.
	// We use a "cookie" pointer stored just before the user pointer.
	// Layout: [FAllocHeader][optional padding][*Header cookie (8 bytes)][user data]
	// The cookie lives at UserPtr - sizeof(void*), pointing back to Header.
	FAllocHeader** CookiePtr = (FAllocHeader**)(UserPtr - sizeof(void*));
	*CookiePtr = Header;

	return UserPtr;
}

void* FDSMallocProfiler::Realloc(void* Original, SIZE_T Count, uint32 Alignment)
{
	if (!Original)
	{
		return Malloc(Count, Alignment);
	}
	if (Count == 0)
	{
		Free(Original);
		return nullptr;
	}

	// Retrieve old header
	FAllocHeader** CookiePtr = (FAllocHeader**)((uint8*)Original - sizeof(void*));
	FAllocHeader*  OldHeader = *CookiePtr;

	int64 OldSize   = 0;
	int32 OldSlot   = -1;
	bool  bValidHdr = (OldHeader && OldHeader->Magic == HeaderMagic);

	if (bValidHdr)
	{
		OldSize = (int64)OldHeader->UserSize;
		OldSlot = OldHeader->SlotIndex;
	}

	// Allocate new block (will record alloc stats)
	void* NewPtr = Malloc(Count, Alignment);
	if (!NewPtr)
	{
		return nullptr;
	}

	// Copy data
	FMemory::Memcpy(NewPtr, Original, FMath::Min<SIZE_T>(Count, (SIZE_T)FMath::Max<int64>(OldSize, 0)));

	// Undo old stats (Free path records dealloc)
	Free(Original);

	return NewPtr;
}

void FDSMallocProfiler::Free(void* Original)
{
	if (!Original)
	{
		return;
	}

	FAllocHeader** CookiePtr = (FAllocHeader**)((uint8*)Original - sizeof(void*));
	FAllocHeader*  Header    = *CookiePtr;

	if (!Header || Header->Magic != HeaderMagic)
	{
		// Not one of our allocations (e.g. allocated before profiler was installed)
		Inner->Free(Original);
		return;
	}

	int64 Size    = (int64)Header->UserSize;
	int32 SlotIdx = Header->SlotIndex;

	// Invalidate header to catch double-free
	Header->Magic = 0xDEFACED;

	if (SlotIdx >= 0 && SlotIdx < SlotCount.load(std::memory_order_relaxed))
	{
		Slots[SlotIdx].RecordFree(Size);
	}
	else
	{
		UntaggedStats.RecordFree(Size);
	}
	GlobalLiveBytes.fetch_sub(Size, std::memory_order_relaxed);

	// Free the original raw pointer (which is at Header)
	Inner->Free((void*)Header);
}

SIZE_T FDSMallocProfiler::GetAllocSize(void* Original)
{
	if (!Original) return 0;
	return Inner->GetAllocSize(Original);
}

bool FDSMallocProfiler::GetAllocationSize(void* Original, SIZE_T& SizeOut)
{
	if (!Original) return false;
	return Inner->GetAllocationSize(Original, SizeOut);
}

void FDSMallocProfiler::Trim(bool bTrimThreadCaches)
{
	Inner->Trim(bTrimThreadCaches);
}

#endif // DS_MEM_PROFILER_ENABLED
