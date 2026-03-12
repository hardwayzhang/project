# DSMemProfiler — UE4 Linux Dedicated Server Memory Profiler

A lightweight, production-safe UE4 plugin that intercepts every heap allocation on Linux Dedicated Server builds, attributes them to named game-module scopes, and provides periodic snapshots, anomaly detection, and on-disk reporting.

---

## Problem Statement

UE4 Linux DS instances accumulate memory over time. The default engine profiling tools (`stat memory`, Unreal Insights, `-tracealloc`) either require a GUI client, introduce too much overhead for production servers, or do not distinguish which **game subsystem** is responsible for growth.

This plugin addresses that with:

| Need | Solution |
|---|---|
| Which module uses the most memory? | Per-tag live bytes tracked via `FDSMallocProfiler` |
| Which module allocates most frequently? | Per-tag `AllocCount` + average alloc size |
| Is memory growing over time? | `FDSMemMonitor` takes periodic snapshots and diffs |
| Catch leaks early | Growth anomaly warning (configurable threshold) |
| Zero cost on non-DS builds | All code guarded by `#if DS_MEM_PROFILER_ENABLED` (set to 0 outside Linux Server/Editor targets) |
| Exportable data | CSV + JSON written to `Saved/MemProfiler/` each snapshot |
| Runtime control | Console commands via `dsmem.*` |

---

## Architecture

```
Game code  →  DS_MEM_SCOPE("AI")
                   │
                   ▼
          FDSMemModuleScope (TLS stack)
                   │
                   ▼  (every Malloc / Realloc / Free)
          FDSMallocProfiler  ←── wraps GMalloc
                   │
           ┌───────┴────────┐
           │                │
    FDSModuleMemStats[]   UntaggedStats
    (atomic counters)
           │
           ▼
    FDSMemMonitor  ──Ticker──►  periodic snapshot
           │
    FDSMemSnapshot  ──►  CSV / JSON / log
```

### Key files

| File | Purpose |
|---|---|
| `Public/DSMemModuleTag.h` | `DS_MEM_SCOPE("Tag")` macro + TLS scope stack |
| `Public/DSMallocProfiler.h` | `FMalloc` wrapper; intercepts all allocs |
| `Public/DSMemMonitor.h` | Periodic snapshot, anomaly detection, disk export |
| `Public/DSMemConsoleCommands.h` | `dsmem.*` console commands |
| `Public/DSMemProfilerIntegration.h` | Drop-in integration guide (include this in game code) |
| `Config/DefaultDSMemProfiler.ini` | Default settings (interval, threshold, etc.) |

---

## Installation

### 1. Copy plugin into your project

```
<YourProject>/Plugins/DSMemProfiler/
```

### 2. Enable in your `.uproject`

```json
{
  "Plugins": [
    {
      "Name": "DSMemProfiler",
      "Enabled": true
    }
  ]
}
```

### 3. Add dependency to game modules that want to use scopes

In `YourGameModule.Build.cs`:

```csharp
PrivateDependencyModuleNames.Add("DSMemProfiler");
```

### 4. Tag your subsystems

```cpp
#include "DSMemProfilerIntegration.h"

void UMyAIController::RunBehaviorTree()
{
    DS_MEM_SCOPE("AI");
    // All allocations here are attributed to "AI"
    ...
}

void UInventoryComponent::TickComponent(float DeltaTime, ...)
{
    DS_MEM_SCOPE("Inventory");
    SyncWithDatabase();
    UpdateContainerCaches();
}
```

The macro is **zero-cost** on non-Linux-DS builds — it compiles to nothing.

---

## Configuration (`DefaultDSMemProfiler.ini`)

```ini
[DSMemProfiler]
SnapshotIntervalSec=10.0     ; Seconds between automatic snapshots
AutoStart=true               ; Start monitoring immediately on server launch
WriteCSVToDisk=true          ; Write CSV + JSON to Saved/MemProfiler/
WarnThresholdMiB=50          ; Warn if a module grows >50 MiB between snapshots
```

Override per-environment using the standard UE4 config layering (`DefaultGame.ini`, `LinuxServer/Game.ini`, etc.).

---

## Console Commands

Connect to the running DS with `nc <host> <port>` (if console port is open) or via `-ExecCmds` at launch.

| Command | Description |
|---|---|
| `dsmem.snapshot [label]` | Take a snapshot, log to output, write CSV+JSON |
| `dsmem.report live` | Print all modules sorted by **live bytes** |
| `dsmem.report allocs` | Print all modules sorted by **alloc count** (find hot allocators) |
| `dsmem.diff` | Delta between the last two snapshots per module |
| `dsmem.startmonitor [sec]` | Start/restart periodic monitoring every `sec` seconds |
| `dsmem.stopmonitor` | Stop periodic monitoring |
| `dsmem.setwarnthreshold <MiB>` | Change growth anomaly warning threshold |
| `dsmem.reset` | Reset all counters (peak, total, alloc counts) |

---

## Output Files

Each snapshot produces two files in `<ProjectRoot>/Saved/MemProfiler/`:

### CSV — for Excel/Google Sheets charting

```
Timestamp,Module,LiveBytes,PeakBytes,TotalAllocBytes,AllocCount,FreeCount,LargeAllocCount,AvgAllocBytes
120.000,AI,52428800,67108864,1073741824,512000,511000,12,2048.0
120.000,Inventory,8388608,10485760,268435456,131072,130048,0,2048.0
```

### JSON — for monitoring pipelines / dashboards

```json
{
  "timestamp_sec": 120.0,
  "global_live_bytes": 524288000,
  "untagged_bytes": 104857600,
  "modules": [
    {
      "module": "AI",
      "live_bytes": 52428800,
      "peak_bytes": 67108864,
      "total_alloc_bytes": 1073741824,
      "alloc_count": 512000,
      "free_count": 511000,
      "large_alloc_count": 12,
      "avg_alloc_bytes": 2048.0
    }
  ]
}
```

---

## Diagnosing Common Issues

### High live bytes in one module → memory leak candidate

```
dsmem.report live
dsmem.diff
```

If `LiveBytes` for a module grows monotonically across `dsmem.diff` outputs, that module is not freeing allocations. Common causes:
- Objects added to a `TArray` / `TMap` but never removed
- Delegates / callbacks holding smart pointer references
- `NewObject<>` without a proper outer causing GC roots

### High alloc count → allocation frequency issue

```
dsmem.report allocs
```

If `AllocCount` is in the millions with small average sizes, the module is making frequent small allocations — a pool allocator or pre-sized containers (`Reserve()`, `Shrink()`) would help.

### High `LargeAllocCount` → oversized single allocations

Each large alloc (≥ 1 MiB) is flagged. Common causes:
- Textures / meshes loaded on the server when they shouldn't be
- Oversized network packet buffers
- Debug logging buffers not stripped in server builds

---

## Performance Overhead

| Path | Cost |
|---|---|
| `DS_MEM_SCOPE("Tag")` push | 1 TLS read + 1 TLS write (< 5 ns) |
| `DS_MEM_SCOPE` pop (destructor) | 1 TLS decrement (< 2 ns) |
| Per-alloc attribution | 2 atomic adds (< 10 ns; negligible vs. actual malloc) |
| Snapshot (every 10 s) | O(NumModules) reads + file I/O; off the hot path |
| Non-DS builds | **Zero** — all code is `#if DS_MEM_PROFILER_ENABLED` |

---

## Recommended Tagging Strategy

Tag at subsystem boundaries, not per-class. Aim for 10–30 tags total:

```
"AI"        – Behavior trees, EQS, perception
"Inventory" – Item data, containers
"Network"   – RPC data, replication buffers
"Physics"   – PhysX / Chaos queries
"GameState" – Match data, player state
"Combat"    – Damage, projectile pools
"Streaming" – Level streaming, async loads
"Audio"     – DS-side audio (if any)
"Analytics" – Telemetry buffers
```

---

## Limitations

- The per-alloc header approach adds `sizeof(FAllocHeader)` (16 bytes) + alignment padding per allocation. This increases total memory usage by ~1–5% in exchange for per-module attribution. Disable via `DS_MEM_PROFILER_ENABLED=0` if this is unacceptable in production.
- Allocations made before `PostEngineInit` (when the plugin installs) are not attributed; they appear as untagged.
- Thread safety: stats use `std::atomic` — lock-free and safe across all worker threads.
- `MaxModules = 128` — adjust in `DSMallocProfiler.h` if more tags are needed.
