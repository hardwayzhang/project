// Copyright (c) Project Team. All Rights Reserved.
#include "DSMemConsoleCommands.h"

#if DS_MEM_PROFILER_ENABLED

#include "DSMallocProfiler.h"
#include "DSMemMonitor.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogDSMemCmd, Log, All);

FDSMemConsoleCommands::FDSMemConsoleCommands()
{
	RegisterAll();
}

FDSMemConsoleCommands::~FDSMemConsoleCommands()
{
	UnregisterAll();
}

void FDSMemConsoleCommands::RegisterAll()
{
	IConsoleManager& CM = IConsoleManager::Get();

	// dsmem.snapshot [label]
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("dsmem.snapshot"),
		TEXT("Take a DSMemProfiler snapshot. Args: [label=Manual]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString Label = Args.Num() > 0 ? Args[0] : TEXT("Manual");
			FDSMemMonitor::Get().TakeAndSaveSnapshot(Label);
		}),
		ECVF_Default
	));

	// dsmem.report live|allocs
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("dsmem.report"),
		TEXT("Print DS memory report. Args: live | allocs"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString Mode = Args.Num() > 0 ? Args[0].ToLower() : TEXT("live");
			FDSMemSnapshot Snap = FDSMemMonitor::Get().TakeSnapshot(TEXT("Report"));
			if (Mode == TEXT("allocs"))
			{
				Snap.SortByAllocCount();
			}
			else
			{
				Snap.SortByLiveBytes();
			}
			Snap.LogSummary(FString::Printf(TEXT("Report[%s]"), *Mode));
		}),
		ECVF_Default
	));

	// dsmem.reset
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("dsmem.reset"),
		TEXT("Reset all DSMemProfiler counters (peak, total, alloc counts)."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (FDSMallocProfiler* P = FDSMallocProfiler::Get())
			{
				P->ResetStats();
				UE_LOG(LogDSMemCmd, Log, TEXT("DSMemProfiler stats reset."));
			}
		}),
		ECVF_Default
	));

	// dsmem.startmonitor [sec]
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("dsmem.startmonitor"),
		TEXT("Start periodic DS memory monitoring. Args: [interval_seconds=10]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			float Interval = 10.f;
			if (Args.Num() > 0)
			{
				Interval = FCString::Atof(*Args[0]);
			}
			FDSMemMonitor::Get().Start(Interval);
		}),
		ECVF_Default
	));

	// dsmem.stopmonitor
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("dsmem.stopmonitor"),
		TEXT("Stop periodic DS memory monitoring."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FDSMemMonitor::Get().Stop();
		}),
		ECVF_Default
	));

	// dsmem.setwarnthreshold <MB>
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("dsmem.setwarnthreshold"),
		TEXT("Set growth anomaly warning threshold in MiB. Args: <MiB>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogDSMemCmd, Warning, TEXT("Usage: dsmem.setwarnthreshold <MiB>"));
				return;
			}
			float MB = FCString::Atof(*Args[0]);
			FDSMemMonitor::Get().GrowthWarnThresholdBytes = (int64)(MB * 1024.0f * 1024.0f);
			UE_LOG(LogDSMemCmd, Log, TEXT("DSMemMonitor warn threshold set to %.1f MiB"), MB);
		}),
		ECVF_Default
	));

	// dsmem.diff
	RegisteredCommands.Add(CM.RegisterConsoleCommand(
		TEXT("dsmem.diff"),
		TEXT("Show delta between the last two DS memory snapshots."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			const TArray<FDSMemSnapshot>& History = FDSMemMonitor::Get().GetHistory();
			if (History.Num() < 2)
			{
				UE_LOG(LogDSMemCmd, Warning,
					TEXT("dsmem.diff requires at least 2 snapshots in history. Run dsmem.startmonitor first."));
				return;
			}

			const FDSMemSnapshot& Prev = History[History.Num() - 2];
			const FDSMemSnapshot& Curr = History[History.Num() - 1];
			TArray<FDSModuleMemSnapshot> Diffs = Curr.DiffModules(Prev);

			Diffs.Sort([](const FDSModuleMemSnapshot& A, const FDSModuleMemSnapshot& B)
			{
				return A.LiveBytes > B.LiveBytes;
			});

			UE_LOG(LogDSMemCmd, Log, TEXT("=== DSMemProfiler DIFF (T=%.1fs -> T=%.1fs) ==="),
				Prev.TimestampSec, Curr.TimestampSec);
			UE_LOG(LogDSMemCmd, Log, TEXT("  %-30s %12s %12s %12s"),
				TEXT("Module"), TEXT("DeltaLive"), TEXT("DeltaAlloc"), TEXT("DeltaCount"));

			for (const auto& D : Diffs)
			{
				if (D.LiveBytes == 0 && D.AllocCount == 0) continue;
				UE_LOG(LogDSMemCmd, Log, TEXT("  %-30s %+11.2f MiB %+11.2f MiB %+12lld"),
					*D.ModuleName,
					(double)D.LiveBytes       / (1024.0 * 1024.0),
					(double)D.TotalAllocBytes / (1024.0 * 1024.0),
					D.AllocCount);
			}
		}),
		ECVF_Default
	));

	UE_LOG(LogDSMemCmd, Log, TEXT("DSMemProfiler console commands registered."));
}

void FDSMemConsoleCommands::UnregisterAll()
{
	for (IConsoleCommand* Cmd : RegisteredCommands)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Cmd);
	}
	RegisteredCommands.Empty();
}

#endif // DS_MEM_PROFILER_ENABLED
