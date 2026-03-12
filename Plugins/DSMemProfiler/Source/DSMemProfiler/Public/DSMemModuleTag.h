// Copyright (c) Project Team. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#if DS_MEM_PROFILER_ENABLED

/**
 * Thread-local module tag system.
 *
 * Usage (in game code):
 *
 *   void UMyInventoryComponent::Tick(float DeltaTime)
 *   {
 *       DS_MEM_SCOPE("Inventory");
 *       // all allocations inside this scope are attributed to "Inventory"
 *       ...
 *   }
 *
 * Scopes can be nested; the innermost wins.
 * The TLS stack has a small fixed depth (see MaxScopeDepth).
 */

class DSMEMPROFILER_API FDSMemModuleScope
{
public:
	static constexpr int32 MaxScopeDepth = 32;

	explicit FDSMemModuleScope(const ANSICHAR* TagName);
	~FDSMemModuleScope();

	/** Returns the current innermost tag index (-1 == no tag / untagged). */
	static int32 GetCurrentSlotIndex();

	/** Returns the current tag name (for debugging). */
	static const ANSICHAR* GetCurrentTagName();

private:
	struct FTLSStack
	{
		int32        SlotIndices[MaxScopeDepth];
		const ANSICHAR* TagNames[MaxScopeDepth];
		int32        Depth{ 0 };
	};

	static thread_local FTLSStack TLSStack;

	bool bPushed{ false };
};

// ---- Convenience macros ----

/** Open a named memory scope for the current block. */
#define DS_MEM_SCOPE(TagName) \
	FDSMemModuleScope _DSMemScope_##__LINE__(TagName)

/** Declare a scope for a UObject subsystem by class name. */
#define DS_MEM_SCOPE_OBJ(Obj) \
	FDSMemModuleScope _DSMemScope_##__LINE__(TCHAR_TO_ANSI(*Obj->GetClass()->GetName()))

#else // DS_MEM_PROFILER_ENABLED == 0

// All macros compile to nothing on non-Linux-DS builds.
#define DS_MEM_SCOPE(TagName)
#define DS_MEM_SCOPE_OBJ(Obj)

class DSMEMPROFILER_API FDSMemModuleScope
{
public:
	explicit FDSMemModuleScope(const ANSICHAR*) {}
	~FDSMemModuleScope() = default;
	static int32 GetCurrentSlotIndex() { return -1; }
	static const ANSICHAR* GetCurrentTagName() { return "disabled"; }
};

#endif // DS_MEM_PROFILER_ENABLED
