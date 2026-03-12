// Copyright (c) Project Team. All Rights Reserved.
#include "DSMemProfilerModule.h"
#include "Modules/ModuleManager.h"

#if DS_MEM_PROFILER_ENABLED
#include "DSMallocProfiler.h"
#include "DSMemMonitor.h"
#include "DSMemConsoleCommands.h"
#include "Misc/ConfigCacheIni.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogDSMemProfiler, Log, All);
#endif

void FDSMemProfilerModule::StartupModule()
{
#if DS_MEM_PROFILER_ENABLED
	UE_LOG(LogDSMemProfiler, Log, TEXT("DSMemProfiler: StartupModule (Linux DS build)."));

	// Install the malloc wrapper (must be on game thread)
	FDSMallocProfiler::Install();

	// Create monitor
	Monitor = MakeUnique<FDSMemMonitor>();

	// Read config from DefaultDSMemProfiler.ini
	float SnapshotInterval  = 10.f;
	int32 WarnThresholdMiB  = 50;
	bool  bAutoStart        = true;
	bool  bWriteCSV         = true;

	const FString ConfigSection = TEXT("DSMemProfiler");

	GConfig->GetFloat( *ConfigSection, TEXT("SnapshotIntervalSec"),   SnapshotInterval, GGameIni);
	GConfig->GetInt(   *ConfigSection, TEXT("WarnThresholdMiB"),      WarnThresholdMiB, GGameIni);
	GConfig->GetBool(  *ConfigSection, TEXT("AutoStart"),             bAutoStart,       GGameIni);
	GConfig->GetBool(  *ConfigSection, TEXT("WriteCSVToDisk"),        bWriteCSV,        GGameIni);

	Monitor->GrowthWarnThresholdBytes = (int64)WarnThresholdMiB * 1024 * 1024;
	Monitor->bWriteCSVToDisk          = bWriteCSV;

	if (bAutoStart)
	{
		Monitor->Start(SnapshotInterval);
	}

	// Register console commands
	ConsoleCommands = MakeUnique<FDSMemConsoleCommands>();

	UE_LOG(LogDSMemProfiler, Log,
		TEXT("DSMemProfiler ready. interval=%.1fs warnThreshold=%d MiB autoStart=%s"),
		SnapshotInterval, WarnThresholdMiB, bAutoStart ? TEXT("true") : TEXT("false"));
#else
	// No-op on non-Linux-DS builds
#endif
}

void FDSMemProfilerModule::ShutdownModule()
{
#if DS_MEM_PROFILER_ENABLED
	UE_LOG(LogDSMemProfiler, Log, TEXT("DSMemProfiler: ShutdownModule."));

	ConsoleCommands.Reset();

	if (Monitor)
	{
		Monitor->Stop();
		// Take a final snapshot before shutdown
		Monitor->TakeAndSaveSnapshot(TEXT("Shutdown"));
		Monitor.Reset();
	}

	FDSMallocProfiler::Uninstall();
#endif
}

IMPLEMENT_MODULE(FDSMemProfilerModule, DSMemProfiler)
