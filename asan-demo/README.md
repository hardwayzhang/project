# Linux 下基于 ASan 的内存检测使用示例

本目录提供一组可直接编译运行的示例，演示如何在 Linux 上使用
**AddressSanitizer（简称 ASan）** 检测常见的 C/C++ 内存错误。

AddressSanitizer 是 GCC / Clang 内置的运行时内存错误检测器，无需安装额外工具，
只要在编译和链接时加上 `-fsanitize=address` 即可。它能在程序运行时即时发现：

- 堆缓冲区溢出（heap-buffer-overflow）
- 栈缓冲区溢出（stack-buffer-overflow）
- 全局缓冲区溢出（global-buffer-overflow）
- 释放后使用（heap-use-after-free，悬垂指针）
- 重复释放（double-free）
- 内存泄漏（memory leak，由内置的 LeakSanitizer 检测）

## 目录结构

```
asan-demo/
├── Makefile                 # 用 -fsanitize=address 编译所有示例
├── run_demo.sh              # 一键编译并依次运行全部示例
├── README.md               # 本文档
└── src/
    ├── correct_example.c        # 正确示例：无任何报错（对照组）
    ├── heap_buffer_overflow.c   # 堆缓冲区溢出
    ├── heap_use_after_free.c    # 释放后使用
    ├── stack_buffer_overflow.c  # 栈缓冲区溢出
    ├── global_buffer_overflow.c # 全局缓冲区溢出
    ├── double_free.c            # 重复释放
    └── memory_leak.c            # 内存泄漏
```

## 环境要求

- Linux 系统（x86_64 已验证）
- GCC（含 `libasan`）或 Clang（含 `compiler-rt`）
- `make`

检查工具是否就绪：

```bash
gcc --version
gcc -print-file-name=libasan.so   # 能打印出 libasan.so 的路径即可
```

> 说明：本示例默认使用 **gcc**。某些系统上 `cc` 可能指向未安装运行时库的
> clang，导致链接失败。Makefile 已显式指定 `CC := gcc`。

## 快速开始

```bash
cd asan-demo

# 方式一：一键编译并运行全部示例
make run

# 方式二：分步执行
make            # 编译所有示例到 bin/
./run_demo.sh   # 运行全部示例并打印报告
make clean      # 清理生成物
```

切换为 clang 编译（需已安装 clang 的 sanitizer 运行时）：

```bash
make CC=clang run
```

## 关键编译选项说明

Makefile 中使用的编译参数：

| 选项 | 作用 |
| --- | --- |
| `-fsanitize=address` | **启用 AddressSanitizer**（核心选项，编译和链接都要带上） |
| `-g` | 生成调试信息，报告里能显示源码文件名和行号 |
| `-fno-omit-frame-pointer` | 保留帧指针，使报错的调用栈回溯更准确 |
| `-O1` | 轻度优化，ASan 官方推荐（`-O0` 也可，但运行更慢） |
| `-Wall -Wextra` | 打开常规编译告警 |

最小化的手动编译示例：

```bash
gcc -fsanitize=address -g -O1 -o demo src/heap_buffer_overflow.c
./demo
```

## 运行时配置：`ASAN_OPTIONS`

ASan 的行为可通过环境变量 `ASAN_OPTIONS` 调整，常用项：

```bash
# 开启内存泄漏检测（部分平台默认未开启）
ASAN_OPTIONS=detect_leaks=1 ./bin/memory_leak

# 遇到第一个错误后继续运行而非中止（默认遇错即停）
ASAN_OPTIONS=halt_on_error=0 ./bin/heap_buffer_overflow

# 将报告写入文件（生成 asan.log.<pid>）
ASAN_OPTIONS=log_path=asan.log ./bin/double_free

# 多项组合用冒号分隔
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 ./bin/memory_leak
```

如需让栈回溯显示符号（函数名/行号）而非仅有地址，可设置
`ASAN_SYMBOLIZER_PATH` 指向 `llvm-symbolizer`，或安装 `binutils`（gcc 默认即可）。

## 如何读懂 ASan 报告

以堆释放后使用为例，报告核心结构如下：

```
==PID==ERROR: AddressSanitizer: heap-use-after-free on address 0x... 
READ of size 4 at 0x... thread T0          <- 出错的操作类型与地址
    #0 ... in main src/heap_use_after_free.c:20   <- 出错位置（你的代码）
freed by thread T0 here:                    <- 这块内存在哪里被 free
    #1 ... in main src/heap_use_after_free.c:17
previously allocated by thread T0 here:     <- 这块内存最初在哪里分配
    #1 ... in main src/heap_use_after_free.c:11
SUMMARY: AddressSanitizer: heap-use-after-free src/heap_use_after_free.c:20 in main
```

阅读要点：

1. **第一行** 给出错误类型（如 `heap-use-after-free`）。
2. **操作行**（READ/WRITE of size N）说明是读还是写、访问了几个字节。
3. **`#0` 栈帧** 通常就是你代码里出问题的那一行。
4. 对于 use-after-free / double-free，还会给出 **分配点** 和 **释放点**，
   非常有助于定位问题。
5. **SUMMARY** 是一行总结，便于在日志中检索。

## 各示例预期结果

| 示例 | 预期 ASan 报告 | 退出码 |
| --- | --- | --- |
| `correct_example` | 无报告，正常输出结果 | 0 |
| `heap_buffer_overflow` | heap-buffer-overflow | 非 0 |
| `heap_use_after_free` | heap-use-after-free | 非 0 |
| `stack_buffer_overflow` | stack-buffer-overflow | 非 0 |
| `global_buffer_overflow` | global-buffer-overflow | 非 0 |
| `double_free` | attempting double-free | 非 0 |
| `memory_leak` | LeakSanitizer: detected memory leaks | 非 0 |

> 注：示例中对越界下标使用了 `volatile`，目的是防止编译器在优化阶段
> 直接把越界访问消除掉，从而保证 ASan 一定能在运行时捕获到错误。

## 常见问题

- **链接报错找不到 `libclang_rt.asan`**：说明 `cc` 指向的 clang 缺少
  sanitizer 运行时。改用 gcc（`make CC=gcc`）或安装对应运行时即可。
- **内存泄漏没有被检测到**：加上 `ASAN_OPTIONS=detect_leaks=1`。
- **报告里只有地址没有行号**：确认编译时带了 `-g`，并安装了符号化工具。
- **ASan 与 Valgrind 的关系**：两者都能查内存错误。ASan 需要重新编译、
  运行开销更小（约 2 倍）、能精准定位到行；Valgrind 无需重编译但更慢。
  实践中常优先用 ASan。
