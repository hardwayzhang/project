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
├── common/
│   ├── asan_default_options.c   # 定义 __asan_default_options()，内置默认选项
│   └── asan_activation.h        # 手动激活/反激活 ASan 的封装（内部符号）
└── src/
    ├── correct_example.c        # 正确示例：无任何报错（对照组）
    ├── heap_buffer_overflow.c   # 堆缓冲区溢出
    ├── heap_use_after_free.c    # 释放后使用
    ├── stack_buffer_overflow.c  # 栈缓冲区溢出
    ├── global_buffer_overflow.c # 全局缓冲区溢出
    ├── double_free.c            # 重复释放
    ├── memory_leak.c            # 内存泄漏
    └── fork_child_overflow.c    # 父进程 fork 子进程，子进程越界
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

## 推荐：用 `__asan_default_options` 把默认选项编进程序

每次都靠环境变量 `ASAN_OPTIONS` 容易忘记设置（例如忘了 `detect_leaks=1`
就漏掉泄漏检测）。更稳妥的做法是 **在程序里定义弱符号函数
`__asan_default_options()`**，ASan 启动时会自动调用它取得默认配置：

```c
/* common/asan_default_options.c */
const char *__asan_default_options(void) {
    return "detect_leaks=1"      /* 开启内存泄漏检测 */
           ":halt_on_error=1"    /* 遇到第一个错误即停止 */
           ":abort_on_error=0";  /* 用退出码结束而非 abort 信号 */
}
```

只要把这个文件链接进可执行程序（本仓库 Makefile 已对每个示例自动链接），
运行时就 **无需再设置 `ASAN_OPTIONS`**：

```bash
# 不带任何环境变量，泄漏检测依然生效（来自 __asan_default_options）
./bin/memory_leak
```

**优先级（后者覆盖前者）：**

```
编译内置默认  <  __asan_default_options()  <  运行时 ASAN_OPTIONS 环境变量
```

也就是说，内置默认值随时可被环境变量临时覆盖，便于调试：

```bash
# 临时关掉泄漏检测，覆盖掉 __asan_default_options 里的 detect_leaks=1
ASAN_OPTIONS=detect_leaks=0 ./bin/memory_leak   # 不再报告泄漏
```

> 小贴士：同理还有 `__lsan_default_options()`（LeakSanitizer）、
> `__lsan_default_suppressions()`（泄漏白名单）、
> `__ubsan_default_options()`（UBSan）等弱符号函数可用。

## 父子进程示例：start_deactivated 启动 + 子进程手动激活（fork）

`src/fork_child_overflow.c` 演示一个更进阶的用法：**父进程以“未激活”状态
启动 ASan（`start_deactivated=1`），fork 子进程后在子进程里手动激活检查**。

流程：

1. 通过自带的 `__asan_default_options()` 设置 `start_deactivated=1`；
2. 父进程启动后显式调用 `asan_deactivate()`，进入未激活状态（原因见下）；
3. `fork()` 出子进程，子进程继承未激活状态：
   - 越界#1（未激活）→ **不被拦截**；
   - 调用 `asan_activate()` 手动激活；
   - 越界#2（已激活）→ **被 ASan 捕获并报告**；
4. 父进程 `waitpid` 读取子进程退出码，自身正常退出。

运行（无需设置环境变量）：

```bash
./bin/fork_child_overflow
```

实测输出（节选）：

```text
[parent ...] 以 start_deactivated=1 启动
[parent ...] 已显式 asan_deactivate()，当前未激活
[child  ...] 越界#1（激活前，预期不被拦截）：
    [deactivated] 写入 arr[8]=4660 成功（说明本次未被 ASan 拦截）
[child  ...] 手动调用 asan_activate() 激活 ASan 检查
[child  ...] 越界#2（激活后，预期被 ASan 捕获）：
==PID==ERROR: AddressSanitizer: heap-buffer-overflow ...
    #0 ... in do_overflow src/fork_child_overflow.c:48
[parent ...] 子进程(...) 以退出码 1 结束（非 0，说明激活后被 ASan 终止）
[parent ...] 父进程自身没有触发检查，正常退出
```

### 实现要点与注意事项

- **激活/反激活接口**：由运行时内部函数 `__asan::AsanActivate()` /
  `AsanDeactivate()` 完成，封装在 `common/asan_activation.h` 里的
  `asan_activate()` / `asan_deactivate()`。**它们不是公开稳定 API**，
  仅供学习/演示，勿用于生产代码。

- **必须静态链接 ASan 运行时**，否则上述内部符号无法解析：
  - gcc：加 `-static-libasan`（本示例 Makefile 已自动加）；
  - clang：默认即静态链接。
  - 若用 gcc 默认的共享 `libasan.so`，符号不导出，链接会失败。

- **为什么还要显式 `asan_deactivate()`？** `start_deactivated=1` 主要面向
  “主程序未插桩、运行时随后被加载”的场景（如 Android：dlopen 带插桩的 `.so`
  时才自动激活）。当主程序本身用 `-fsanitize=address` 插桩时，运行时在启动阶段
  会因检测到已插桩模块而 **自动激活**，使 `start_deactivated` 看起来“没生效”。
  因此这里在 `main` 入口再显式反激活一次，才能真正进入未激活状态来演示。

- **多进程日志**：多个进程同时报错时 stderr 上的报告可能交错，建议配合
  `ASAN_OPTIONS=log_path=asan.log`，每个进程写到各自的 `asan.log.<pid>`，
  按 pid 区分，互不干扰。

## 问题一：把 ASan 错误日志写入文件（而非输出到终端）

ASan 默认把报告写到 **stderr（fd 2）**。要改为写入文件，推荐用
`ASAN_OPTIONS=log_path=<前缀>`，运行时会生成 `<前缀>.<pid>` 文件，
并且 **不再向终端打印**：

```bash
# 生成 ./asan.log.<pid>，终端保持干净
ASAN_OPTIONS=log_path=asan.log ./bin/heap_buffer_overflow

# 也可写到绝对路径目录
ASAN_OPTIONS=log_path=/var/log/myapp/asan.log ./bin/heap_buffer_overflow
```

相关可选项：

| 选项 | 作用 |
| --- | --- |
| `log_path=前缀` | 报告写入 `前缀.<pid>`，不再输出到 stderr |
| `log_exe_name=1` | 文件名中加入可执行文件名，便于区分多个程序 |
| `log_to_syslog=1` | 写入 syslog |

> 为什么不直接用 `2> file` 重定向？也可以，但当程序自身还会往 stderr
> 打印业务日志时会混在一起；`log_path` 能把 ASan 报告单独隔离到独立文件，
> 且每个进程一个文件，更适合多进程/服务场景。

实测：使用 `log_path` 后终端无任何 ASan 输出，报告完整写入文件：

```text
$ ASAN_OPTIONS=log_path=/tmp/asan_demo.log ./bin/heap_buffer_overflow
(终端无 ASan 输出)
$ cat /tmp/asan_demo.log.*
==9496==ERROR: AddressSanitizer: heap-buffer-overflow ...
    #0 0x... in main src/heap_buffer_overflow.c:24
```

## 问题二：让报告带上文件名/行号（符号信息）的最佳实践

如果报告里栈帧只有裸地址，例如：

```text
#0 0x5114b1  (/.../bin/heap_buffer_overflow+0x5114b1)
```

说明 **符号化（symbolization）没有生效**。最佳实践按重要性排序如下：

1. **编译时必须带 `-g`（且不要 strip 可执行文件）。**
   这是行号的来源。即使符号器可用，缺了 `-g` 也只能得到函数名、得不到
   `文件:行号`。本仓库 Makefile 已默认带 `-g`。

2. **保留默认的 `symbolize=1`，不要关掉。**
   `ASAN_OPTIONS=symbolize=0` 会强制只打印裸地址（常见于复现这种现象）。

3. **保证运行环境能找到一个“符号器”：**
   - **GCC**：其 `libasan` 内置 `libbacktrace`，只要二进制带 `-g`，
     **无需任何外部工具** 就能直接打印 `文件:行号`（本仓库即如此）。
   - **Clang**：依赖外部的 `llvm-symbolizer`。需安装 LLVM 工具链，
     并确保它在 `PATH` 中，或显式指定：
     ```bash
     export ASAN_SYMBOLIZER_PATH=$(command -v llvm-symbolizer)
     ./your_program
     ```

4. **`-fno-omit-frame-pointer`**：让调用栈回溯更完整、准确（本仓库已带）。

### 已经拿到“只有地址”的旧日志怎么办？离线还原行号

很多线上场景日志早已生成、且只有 `(可执行文件+偏移)`。
只要你 **手里有那个带 `-g` 的同一份二进制**，就能离线还原：

方法 A：`addr2line`（binutils 自带，最通用）

```bash
# 偏移取自报告中 (binary+0xXXXX) 的 0xXXXX
addr2line -f -e bin/heap_buffer_overflow 0x13db
# 输出:
#   main
#   /workspace/asan-demo/src/heap_buffer_overflow.c:24
```

方法 B：`llvm-symbolizer`（若已安装）

```bash
echo 'bin/heap_buffer_overflow 0x13db' | llvm-symbolizer
```

方法 C：LLVM 自带的 `asan_symbolize.py`，可直接把整段日志管道进去自动替换：

```bash
cat asan.log.1234 | asan_symbolize.py
```

> 注意：对 PIE（位置无关可执行文件）而言，报告里 `(binary+偏移)` 的 **偏移**
> 已经是相对二进制基址的值，可直接喂给 `addr2line`；不要用运行时的绝对
> 地址（如 `0x5114b1`）去查，那个会因 ASLR 每次不同。

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
| `fork_child_overflow` | 子进程 heap-buffer-overflow（父进程正常退出） | 0（父进程） |

> 注：示例中对越界下标使用了 `volatile`，目的是防止编译器在优化阶段
> 直接把越界访问消除掉，从而保证 ASan 一定能在运行时捕获到错误。

## 常见问题

- **链接报错找不到 `libclang_rt.asan`**：说明 `cc` 指向的 clang 缺少
  sanitizer 运行时。改用 gcc（`make CC=gcc`）或安装对应运行时即可。
- **内存泄漏没有被检测到**：加上 `ASAN_OPTIONS=detect_leaks=1`。
- **报告里只有地址没有行号**：见上文「问题二」。先确认编译带 `-g` 且未
  `strip`；GCC 无需外部工具，Clang 需 `llvm-symbolizer` 在 `PATH` 中；
  旧日志可用 `addr2line -f -e <二进制> <偏移>` 离线还原。
- **想把报告写入文件**：见上文「问题一」，用 `ASAN_OPTIONS=log_path=前缀`。
- **ASan 与 Valgrind 的关系**：两者都能查内存错误。ASan 需要重新编译、
  运行开销更小（约 2 倍）、能精准定位到行；Valgrind 无需重编译但更慢。
  实践中常优先用 ASan。
