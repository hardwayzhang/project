// Copyright (c) Project Team. All Rights Reserved.
#include "DSMemModuleTag.h"

#if DS_MEM_PROFILER_ENABLED

#include "DSMallocProfiler.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogDSMemTag, Verbose, All);

// ---------------------------------------------------------------------------
// Thread-local scope stack
// ---------------------------------------------------------------------------

thread_local FDSMemModuleScope::FTLSStack FDSMemModuleScope::TLSStack;

FDSMemModuleScope::FDSMemModuleScope(const ANSICHAR* TagName)
{
	FTLSStack& Stack = TLSStack;
	if (Stack.Depth >= MaxScopeDepth)
	{
		// Silently skip if stack is full; bPushed = false means destructor is a no-op
		UE_LOG(LogDSMemTag, Warning, TEXT("DSMemModuleScope: scope stack overflow (depth=%d)"), Stack.Depth);
		return;
	}

	// Resolve slot index (GetOrCreate is cheap after first registration)
	int32 SlotIdx = -1;
	if (FDSMallocProfiler* Profiler = FDSMallocProfiler::Get())
	{
		FName TagFName(TagName);
		FDSModuleMemStats* Stats = Profiler->GetOrCreateModuleStats(TagFName);
		SlotIdx = Profiler->GetSlotIndexForStats(Stats);
	}

	Stack.SlotIndices[Stack.Depth] = SlotIdx;
	Stack.TagNames[Stack.Depth]    = TagName;
	++Stack.Depth;
	bPushed = true;
}

FDSMemModuleScope::~FDSMemModuleScope()
{
	if (bPushed)
	{
		FTLSStack& Stack = TLSStack;
		check(Stack.Depth > 0);
		--Stack.Depth;
	}
}

int32 FDSMemModuleScope::GetCurrentSlotIndex()
{
	const FTLSStack& Stack = TLSStack;
	if (Stack.Depth == 0)
	{
		return -1;
	}
	return Stack.SlotIndices[Stack.Depth - 1];
}

const ANSICHAR* FDSMemModuleScope::GetCurrentTagName()
{
	const FTLSStack& Stack = TLSStack;
	if (Stack.Depth == 0)
	{
		return "__Untagged__";
	}
	return Stack.TagNames[Stack.Depth - 1];
}

#endif // DS_MEM_PROFILER_ENABLED
