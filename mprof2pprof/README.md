# mprof2pprof — UE4 `.mprof` 离线转 pprof JSON 示例工具

本工具演示如何把 **UE4 `MallocProfiler` 生成的 `.mprof` 文件**(典型场景:服务器 DS 进程 dump 出来、且 **不含符号信息**)离线转换为 **pprof 兼容的 JSON 文件**,可被 `pprof`、Flamegraph 等工具消费。

实现重点对应需求中明确指出的两项:

1. **把 mprof 中的 `ProgramCounter` 还原为正确的符号** —— 参见 [`mprof2pprof/symbolizer.py`](mprof2pprof/symbolizer.py)。
2. **理解 mprof 和 pprof 两种文件格式并做正确的字段映射** —— 参见 [`mprof2pprof/mprof_format.py`](mprof2pprof/mprof_format.py) 与 [`mprof2pprof/pprof_writer.py`](mprof2pprof/pprof_writer.py)。

工程结构:

```
mprof2pprof/
├── mprof2pprof/
│   ├── mprof_format.py   # mprof 二进制格式定义 + 解析器
│   ├── symbolizer.py     # PC → 符号 解析(调用 addr2line / llvm-symbolizer)
│   ├── pprof_writer.py   # 构造 pprof 模型并序列化为 JSON
│   └── cli.py            # 命令行入口:`python -m mprof2pprof.cli ...`
├── examples/
│   └── generate_sample_mprof.py  # 编译一个真实 ELF + 写出格式完全合法的 .mprof
└── tests/
    └── test_roundtrip.py # 真正跑一遍:生成 → 转换 → 校验 JSON
```

---

## 一、UE4 `.mprof` 文件格式

> 源码参考:`Engine/Source/Runtime/Core/Private/ProfilingDebugging/MallocProfiler.cpp`、`Engine/Source/Runtime/Core/Public/ProfilingDebugging/MallocProfiler.h`。
>
> 本工具实现的是「文档化 v6」schema —— 它与引擎主流版本一致,且与 `examples/generate_sample_mprof.py` 写出来的字节流逐字段对应。若目标版本不同,只需扩展 `mprof_format._parse_header()` 中的字段顺序。

### 1.1 整体布局

`.mprof` 是一个 `FArchive` 写出的小端二进制文件,大致结构如下:

| 区域 | 长度 | 说明 |
| --- | --- | --- |
| Header | 固定 + 若干 `FString` | Magic、版本号、平台、可执行文件名、各表的 offset/count |
| Token Stream | 变长 | 实时记录的 malloc / free / realloc / marker,以 `SUBTYPE_EndOfStream` 收尾 |
| NameTable | 变长 | `TArray<FString>`,所有用到的字符串(若 DS 端写了符号才有用) |
| CallStackAddressTable | 变长 | `TArray<FCallStackAddressInfo>`,每个唯一 PC 一行 |
| CallStackTable | 变长 | `TArray<FCallStackInfo>`,每条调用栈由一串 AddressTable 索引组成 |
| ModulesTable | 变长 | `TArray<FModuleInfo>`,运行时加载的镜像列表 |
| MetaDataTable | 变长 | 任意 `TPair<Key,Value>` 元数据 |

Header 末尾给出后五个表的 file offset,所以解析器需要 seek-and-read 而不能纯顺序读。

### 1.2 关键结构体

```text
FProfilerHeader {
    uint32  Magic                 = 0xDA15F7D8
    uint32  VersionNumber
    FString PlatformName                       // "Linux" / "Win64" / ...
    uint32  bShouldSerializeSymbolInfo         // DS 默认 false
    FString ExecutableName
    uint32  NameTableOffset,  NameTableEntries
    uint32  CSAddrTableOffset,CSAddrTableEntries
    uint32  CSTableOffset,    CSTableEntries
    uint32  ModulesOffset,    ModuleEntries
    uint32  MetaDataOffset,   MetaDataEntries
}

FModuleInfo {
    FString Name                  // 镜像文件名(.exe / .so / dylib)
    uint64  BaseAddress           // 运行时基址(ASLR 之后)
    uint64  PreferredBaseAddress  // 链接时基址(ELF p_vaddr / PE ImageBase)
    uint64  Size
    FString BuildId
}

FCallStackAddressInfo {
    uint64  ProgramCounter        // 抓栈时的绝对运行时地址
    int32   ModuleIndex           // 该 PC 落在哪个 Module 上
    int32   FilenameNameIndex     // -1 表示未符号化
    int32   FunctionNameIndex     // -1
    int32   LineNumber            // -1
}

FCallStackInfo {
    uint32  CRC
    uint32  Depth
    int32   AddressIndices[Depth] // 从最外层到最内层
}
```

### 1.3 Token Stream 编码

每个 token 头部是一个 `uint32`,低 2 位 = `EProfilingPayloadType`,高 30 位 = 子负载(MALLOC/FREE/REALLOC 时是 `CallStackIndex`,OTHER 时是 `EProfilingPayloadSubType`)。具体见 [`mprof_format.py`](mprof2pprof/mprof_format.py) 顶部注释。

### 1.4 `FString` 编码

```text
int32 Len ; if Len >= 0: ANSI, Len 字节(含尾 \0)
            if Len <  0: UTF-16LE, |Len| 个 code unit(含尾 \0)
            if Len == 0: 空串
```

---

## 二、pprof 文件格式 (`profile.proto`)

参考 https://github.com/google/pprof/blob/main/proto/profile.proto 。pprof 原生是 protobuf,但 JSON 表示(就是把 protobuf JSON-encode)被广泛支持,本工具直接产出 JSON。

```text
Profile {
  repeated string   string_table        // 索引 0 必须是空串
  repeated ValueType sample_type        // 每个 Sample.value[i] 的含义
  repeated Sample   sample              // 一条采样 = 一个调用栈 + value[]
  repeated Mapping  mapping             // 一段加载到内存的镜像
  repeated Location location            // 一个 PC,可展开成多帧(inlined)
  repeated Function function            // 一个函数(name/file)
  ValueType period_type
  int64    period
  int64    time_nanos
  int64    default_sample_type
}

Sample   { repeated uint64 location_id ; repeated int64 value ; repeated Label label }
Location { uint64 id ; uint64 mapping_id ; uint64 address ; repeated Line line ; bool is_folded }
Line     { uint64 function_id ; int64 line }
Function { uint64 id ; int64 name ; int64 system_name ; int64 filename ; int64 start_line }
Mapping  { uint64 id ; uint64 memory_start ; uint64 memory_limit ; ... ; int64 filename ; int64 build_id }
```

注意:

- 字符串通通进 `string_table`,其他字段引用其下标。
- `Sample.location_id` 必须 **leaf → root**(与 UE4 的 root → leaf 顺序相反,转换时要 reverse)。
- `Location.line` 用于内联展开,**outer → inner** 顺序。
- 所有 `id` 从 1 开始,0 表示「未设置」。

---

## 三、mprof → pprof 字段映射

| mprof | pprof | 备注 |
| --- | --- | --- |
| `ModuleInfo`               | `Mapping`        | `memoryStart = BaseAddress`,`memoryLimit = Base + Size` |
| `CallStackAddressInfo.PC`  | `Location.address` |  |
| `CallStackAddressInfo` + 符号化结果 | `Location.line[*]` + `Function` | 每个 inlined 帧一条 `Line` |
| `CallStackInfo.AddressIndices` | `Sample.locationId` | **顺序反转**(UE4 root→leaf,pprof leaf→root) |
| Tokens 聚合 | `Sample.value` | 默认采样类型 = heap 4 通道(见下) |
| `MetaDataTable`            | `Sample.label`   | 写入一个值全 0 的 synthetic sample |

每条聚合后的 `Sample.value` 共 4 个 int64,语义与 Go heap profile 对齐:

```
[ alloc_objects (count), alloc_space (bytes),
  inuse_objects (count), inuse_space (bytes) ]
```

`free` 会从对应 callstack 的 `inuse_*` 中减去,`realloc` 拆成「旧 ptr free + 新 ptr malloc」。

---

## 四、PC → 符号 的解析(重点 1)

DS 抓栈时只能拿到 **运行时绝对地址**;要把它变回 `function + file:line`,需要:

```text
binary_offset = PC - Module.BaseAddress             // 抹掉 ASLR
file_vma      = binary_offset + Module.PreferredBaseAddress
```

`file_vma` 就是 `addr2line` / `llvm-symbolizer` 接受的「带 ELF VMA 的地址」。对 PIE / 共享库,`PreferredBaseAddress` 一般是 0(也即 `file_vma == binary_offset`);对 `-no-pie` 的可执行文件,通常是 `0x400000` 之类的链接基址。

`mprof_format.py` 把 `ModuleInfo` 中的 `BaseAddress`、`PreferredBaseAddress` 都读出来,`pprof_writer.build_pprof_from_mprof()` 调用 `Symbolizer.resolve_batch()` 时就按上式预先算好 offset。

`symbolizer.py` 的实现要点:

- 自动探测 `llvm-symbolizer`,没有就退化到 GNU `addr2line`,两者命令行参数都用 `-a -f -C -i`(展开 inline / demangle / 在每条输出前打印源地址)。
- 同一个二进制的所有地址一次喂入 stdin,避免反复 fork。
- 输出按「以 `0x` 开头的行作为分隔」切片,容错地处理 `(discriminator N)`、列号等 GNU 风格附加信息。
- 内置缓存 `(binary, offset) -> [Symbol...]`,跨 callstack 的重复 PC 只解析一次。
- 没有对应二进制 / 没有 debug info 时,优雅降级为 `module+0xoff`,这样后续仍可用 pprof 浏览,只是缺函数名。

CLI 支持给副模块单独指定符号文件:

```bash
python -m mprof2pprof.cli capture.mprof \
    --binary ./MyGameServer \
    --module-binary libUE4Engine.so=./symbols/libUE4Engine.so \
    --module-binary GamePlugin.so=./symbols/GamePlugin.so \
    --output capture.pprof.json
```

---

## 五、运行示例

```bash
# 1. 生成 demo:编译一个真实的 ELF(用 gcc -O0 -g)并写出一个完全合法的 .mprof
python3 examples/generate_sample_mprof.py
#   ↳ examples/out/fakeserver     真二进制(供 addr2line 用)
#   ↳ examples/out/capture.mprof  v6 格式 mprof

# 2. 离线转 pprof JSON
python3 -m mprof2pprof.cli examples/out/capture.mprof \
    --binary examples/out/fakeserver \
    --output examples/out/capture.pprof.json --summary

# 3. 跑端到端测试
python3 -m pytest tests/ -v
```

输出 JSON 片段(节选):

```jsonc
"location": [
  { "id": 1, "address": 139637976732173,
    "line": [ { "functionId": 1, "line": 21 } ],   // main, fakeserver.c:21
    "mappingId": 1 },
  ...
],
"sample": [
  { "locationId": [4, 3, 2, 1],                    // leaf→root: Leaf→Middle→Outer→main
    "value": [2, 384, 1, 256] },                   // 2 次 alloc / 384B / 现仍占用 1*256B
  ...
]
```

---

## 六、把它接入真实 DS 流程

1. 在游戏服务器上启用 `-MALLOCPROFILER` 命令行,跑出 `.mprof`。
2. 把 `.mprof` 和 **配对的 `MyGameServer`(带 `.debug` 或不带 strip 的版本)** 拷到分析机。
3. 执行 `python3 -m mprof2pprof.cli capture.mprof --binary ./MyGameServer -o out.json`。
4. 在分析机上用 `pprof -http=:8080 out.json` 浏览火焰图、`top` 列表、调用图。

> 若运行机和分析机的二进制不是同一个 build,务必通过 `Module.BuildId` (mprof 中已写入) 校验,否则符号地址会错位。
