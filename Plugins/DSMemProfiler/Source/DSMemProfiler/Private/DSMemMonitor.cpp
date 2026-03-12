// Copyright (c) Project Team. All Rights Reserved.
#include "DSMemMonitor.h"

#if DS_MEM_PROFILER_ENABLED

#include "DSMallocProfiler.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Containers/Ticker.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogDSMemMonitor, Log, All);

FDSMemMonitor* FDSMemMonitor::GInstance = nullptr;

// ---------------------------------------------------------------------------
// Snapshot helpers
// ---------------------------------------------------------------------------

static FDSModuleMemSnapshot SnapshotFromStats(const FDSModuleMemStats* Stats, double Now)
{
	FDSModuleMemSnapshot S;
	S.ModuleName       = Stats->ModuleName.ToString();
	S.LiveBytes        = Stats->LiveBytes.load(std::memory_order_relaxed);
	S.PeakBytes        = Stats->PeakBytes.load(std::memory_order_relaxed);
	S.TotalAllocBytes  = Stats->TotalAllocBytes.load(std::memory_order_relaxed);
	S.AllocCount       = Stats->AllocCount.load(std::memory_order_relaxed);
	S.FreeCount        = Stats->FreeCount.load(std::memory_order_relaxed);
	S.LargeAllocCount  = Stats->LargeAllocCount.load(std::memory_order_relaxed);
	S.SnapshotTimeSec  = Now;
	return S;
}

// ---------------------------------------------------------------------------
// FDSMemSnapshot
// ---------------------------------------------------------------------------

TArray<FDSModuleMemSnapshot> FDSMemSnapshot::DiffModules(const FDSMemSnapshot& Baseline) const
{
	TMap<FString, const FDSModuleMemSnapshot*> BaseMap;
	for (const auto& M : Baseline.Modules)
	{
		BaseMap.Add(M.ModuleName, &M);
	}

	TArray<FDSModuleMemSnapshot> Diffs;
	Diffs.Reserve(Modules.Num());

	for (const auto& M : Modules)
	{
		FDSModuleMemSnapshot D = M;
		if (const FDSModuleMemSnapshot* B = BaseMap.FindRef(M.ModuleName))
		{
			D.LiveBytes        -= B->LiveBytes;
			D.TotalAllocBytes  -= B->TotalAllocBytes;
			D.AllocCount       -= B->AllocCount;
			D.FreeCount        -= B->FreeCount;
			D.LargeAllocCount  -= B->LargeAllocCount;
		}
		Diffs.Add(D);
	}
	return Diffs;
}

void FDSMemSnapshot::SortByLiveBytes()
{
	Modules.Sort([](const FDSModuleMemSnapshot& A, const FDSModuleMemSnapshot& B)
	{
		return A.LiveBytes > B.LiveBytes;
	});
}

void FDSMemSnapshot::SortByAllocCount()
{
	Modules.Sort([](const FDSModuleMemSnapshot& A, const FDSModuleMemSnapshot& B)
	{
		return A.AllocCount > B.AllocCount;
	});
}

FString FDSMemSnapshot::ToCSV() const
{
	FString Out;
	Out.Reserve(4096);
	Out += TEXT("Timestamp,Module,LiveBytes,PeakBytes,TotalAllocBytes,AllocCount,FreeCount,LargeAllocCount,AvgAllocBytes\n");

	auto AppendModule = [&](const FDSModuleMemSnapshot& M)
	{
		Out += FString::Printf(
			TEXT("%.3f,%s,%lld,%lld,%lld,%lld,%lld,%lld,%.1f\n"),
			TimestampSec, *M.ModuleName,
			M.LiveBytes, M.PeakBytes, M.TotalAllocBytes,
			M.AllocCount, M.FreeCount, M.LargeAllocCount,
			M.GetAvgAllocBytes());
	};

	for (const auto& M : Modules)
	{
		AppendModule(M);
	}
	return Out;
}

FString FDSMemSnapshot::ToJSON() const
{
	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject());
	Root->SetNumberField(TEXT("timestamp_sec"), TimestampSec);
	Root->SetNumberField(TEXT("global_live_bytes"), (double)GlobalLiveBytes);
	Root->SetNumberField(TEXT("untagged_bytes"), (double)UntaggedBytes);

	TArray<TSharedPtr<FJsonValue>> ModArray;
	for (const auto& M : Modules)
	{
		TSharedRef<FJsonObject> MObj = MakeShareable(new FJsonObject());
		MObj->SetStringField(TEXT("module"), M.ModuleName);
		MObj->SetNumberField(TEXT("live_bytes"), (double)M.LiveBytes);
		MObj->SetNumberField(TEXT("peak_bytes"), (double)M.PeakBytes);
		MObj->SetNumberField(TEXT("total_alloc_bytes"), (double)M.TotalAllocBytes);
		MObj->SetNumberField(TEXT("alloc_count"), (double)M.AllocCount);
		MObj->SetNumberField(TEXT("free_count"), (double)M.FreeCount);
		MObj->SetNumberField(TEXT("large_alloc_count"), (double)M.LargeAllocCount);
		MObj->SetNumberField(TEXT("avg_alloc_bytes"), M.GetAvgAllocBytes());
		ModArray.Add(MakeShareable(new FJsonValueObject(MObj)));
	}
	Root->SetArrayField(TEXT("modules"), ModArray);

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}

void FDSMemSnapshot::LogSummary(const FString& Label) const
{
	UE_LOG(LogDSMemMonitor, Log, TEXT("=== DSMemProfiler Snapshot [%s] T=%.1fs ==="), *Label, TimestampSec);
	UE_LOG(LogDSMemMonitor, Log, TEXT("  GlobalLive=%.2f MiB  Untagged=%.2f MiB"),
		(double)GlobalLiveBytes / (1024.0 * 1024.0),
		(double)UntaggedBytes   / (1024.0 * 1024.0));
	UE_LOG(LogDSMemMonitor, Log, TEXT("  %-30s %10s %10s %12s %10s %10s"),
		TEXT("Module"), TEXT("Live(MiB)"), TEXT("Peak(MiB)"), TEXT("TotalAlloc"), TEXT("AllocCnt"), TEXT("LargeAlloc"));

	for (const auto& M : Modules)
	{
		UE_LOG(LogDSMemMonitor, Log,
			TEXT("  %-30s %10.2f %10.2f %12.2f %10lld %10lld"),
			*M.ModuleName,
			(double)M.LiveBytes       / (1024.0 * 1024.0),
			(double)M.PeakBytes       / (1024.0 * 1024.0),
			(double)M.TotalAllocBytes / (1024.0 * 1024.0),
			M.AllocCount,
			M.LargeAllocCount);
	}
}

// ---------------------------------------------------------------------------
// FDSMemMonitor
// ---------------------------------------------------------------------------

FDSMemMonitor::FDSMemMonitor()
{
	GInstance = this;
}

FDSMemMonitor::~FDSMemMonitor()
{
	Stop();
	if (GInstance == this)
	{
		GInstance = nullptr;
	}
}

FDSMemMonitor& FDSMemMonitor::Get()
{
	check(GInstance);
	return *GInstance;
}

void FDSMemMonitor::Start(float InSnapshotIntervalSec)
{
	SnapshotInterval = FMath::Max(InSnapshotIntervalSec, 1.f);
	TimeSinceLastSnapshot = 0.f;

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FDSMemMonitor::OnTick), 0.f);

	UE_LOG(LogDSMemMonitor, Log,
		TEXT("DSMemMonitor started (interval=%.1fs, warnThreshold=%.1f MiB, writeCsv=%s)"),
		SnapshotInterval,
		(double)GrowthWarnThresholdBytes / (1024.0 * 1024.0),
		bWriteCSVToDisk ? TEXT("yes") : TEXT("no"));
}

void FDSMemMonitor::Stop()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
		UE_LOG(LogDSMemMonitor, Log, TEXT("DSMemMonitor stopped."));
	}
}

bool FDSMemMonitor::OnTick(float DeltaTime)
{
	TimeSinceLastSnapshot += DeltaTime;
	if (TimeSinceLastSnapshot >= SnapshotInterval)
	{
		TimeSinceLastSnapshot = 0.f;
		TakeAndSaveSnapshot(TEXT("Periodic"));
	}
	return true; // keep ticking
}

FDSMemSnapshot FDSMemMonitor::TakeSnapshot(const FString& Label) const
{
	FDSMallocProfiler* Profiler = FDSMallocProfiler::Get();
	if (!Profiler)
	{
		return {};
	}

	FDSMemSnapshot Snap;
	Snap.TimestampSec    = FPlatformTime::Seconds();
	Snap.GlobalLiveBytes = Profiler->GetTotalLiveBytes();
	Snap.UntaggedBytes   = Profiler->GetUntaggedLiveBytes();

	TArray<FDSModuleMemStats*> AllStats = Profiler->GetAllModuleStats();
	Snap.Modules.Reserve(AllStats.Num());

	for (FDSModuleMemStats* Stats : AllStats)
	{
		if (Stats)
		{
			Snap.Modules.Add(SnapshotFromStats(Stats, Snap.TimestampSec));
		}
	}

	Snap.SortByLiveBytes();
	Snap.LogSummary(Label);
	return Snap;
}

FString FDSMemMonitor::TakeAndSaveSnapshot(const FString& Label)
{
	FDSMemSnapshot Snap = TakeSnapshot(Label);

	if (!LastSnapshot.Modules.IsEmpty())
	{
		CheckGrowthAnomaly(Snap, LastSnapshot);
	}

	// Maintain history
	History.Add(Snap);
	if (History.Num() > MaxHistoryEntries)
	{
		History.RemoveAt(0, History.Num() - MaxHistoryEntries);
	}
	LastSnapshot = Snap;

	FString FilePath;
	if (bWriteCSVToDisk)
	{
		FString Dir = GetOutputDir();
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		if (!PF.DirectoryExists(*Dir))
		{
			PF.CreateDirectoryTree(*Dir);
		}

		FString TimeStr = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString CsvFile = FPaths::Combine(Dir, FString::Printf(TEXT("MemSnap_%s_%s.csv"), *TimeStr, *Label));
		FString JsonFile = FPaths::Combine(Dir, FString::Printf(TEXT("MemSnap_%s_%s.json"), *TimeStr, *Label));

		FFileHelper::SaveStringToFile(Snap.ToCSV(), *CsvFile);
		FFileHelper::SaveStringToFile(Snap.ToJSON(), *JsonFile);

		UE_LOG(LogDSMemMonitor, Log, TEXT("DSMemMonitor: snapshot saved to %s"), *CsvFile);
		FilePath = CsvFile;
	}

	return FilePath;
}

void FDSMemMonitor::CheckGrowthAnomaly(const FDSMemSnapshot& NewSnap, const FDSMemSnapshot& PrevSnap) const
{
	TArray<FDSModuleMemSnapshot> Diffs = NewSnap.DiffModules(PrevSnap);
	for (const auto& D : Diffs)
	{
		if (D.LiveBytes > GrowthWarnThresholdBytes)
		{
			UE_LOG(LogDSMemMonitor, Warning,
				TEXT("DSMemProfiler ANOMALY: Module '%s' grew by %.2f MiB in the last snapshot interval (now %.2f MiB live). AllocCnt delta=%lld"),
				*D.ModuleName,
				(double)D.LiveBytes / (1024.0 * 1024.0),
				(double)NewSnap.Modules.FindByPredicate([&](const FDSModuleMemSnapshot& M){ return M.ModuleName == D.ModuleName; })->LiveBytes / (1024.0 * 1024.0),
				D.AllocCount);
		}
	}
}

FString FDSMemMonitor::GetOutputDir() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MemProfiler"));
}

#endif // DS_MEM_PROFILER_ENABLED
