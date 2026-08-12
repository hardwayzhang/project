# COW `do_wp_page` 事件观测 Demo

一个在 Linux 上演示 **写时复制(Copy-On-Write, COW)** 的完整示例：父进程申请并写入一段
私有匿名内存，`fork` 出子进程后，子进程首次写入这些页会触发内核的写保护缺页处理函数
`do_wp_page()`，为子进程复制出独立的物理页。本项目提供 **两种方式** 来观测子进程的
COW 事件（调用栈 + 改写内存大小汇总）：

- **方式一**：通过 `perf record -e probe:do_wp_page` 追踪子进程的 COW 事件堆栈；
- **方式二**：在观测进程内用 **`perf_event_open()` C 接口** 订阅 `do_wp_page` 事件
  （原理与 `perf record -e probe:do_wp_page` 一致），直接读取环形缓冲区里的调用栈。

---

## 1. COW 原理简述

1. 父进程 `mmap` 一段 `MAP_PRIVATE | MAP_ANONYMOUS` 内存并写入，使物理页真正驻留。
2. `fork()` 后，父子进程共享这些物理页，页表项被标记为 **只读**。
3. 子进程首次 **写** 某一页时，MMU 触发写保护缺页：
   `缺页异常 → handle_mm_fault() → ... → do_wp_page()`。
4. `do_wp_page()` 为写入方复制一份新的私有物理页并恢复可写。

因此 **一次 `do_wp_page` ≈ 一次 COW ≈ 复制了一个页(通常 4KiB)**。本 demo 让子进程对
每一页各写一次，从而 `do_wp_page` 事件数 ≈ 写入页数，`COW 内存 ≈ 事件数 × 页大小`。

> 为保证按 4KiB 基页计数，demo 对目标内存执行了 `madvise(MADV_NOHUGEPAGE)`，避免透明大页。

---

## 2. 目录结构

```
.
├── Makefile                     # 构建
├── setup.sh                     # 环境/权限检测 (可 --fix)
├── scripts/
│   └── perf_cow_watch.sh        # 方式一: perf record/probe 记录与分析脚本
└── src/
    ├── cow_demo.c               # 主程序: 内存申请/fork/编排两种方式
    ├── symbolize.c              # 内核(/proc/kallsyms)+用户态(ELF)地址符号化
    └── symbolize.h
```

---

## 3. 编译

```bash
make            # 生成 ./cow_demo
make check      # 等价于 ./setup.sh, 检测运行环境
make clean
```

编译使用 `-g -fno-omit-frame-pointer`，以便 perf 能基于帧指针回溯 **子进程用户态调用栈**。

---

## 4. 环境检测与权限

两种方式都涉及内核追踪能力与权限，先运行检测：

```bash
./setup.sh          # 只检测, 输出 PASS/WARN/FAIL
sudo ./setup.sh --fix   # 检测并尝试修复(放宽 sysctl / 挂载 tracefs / 安装 perf)
```

检测项与所需权限：

| 能力 | 用途 | 要求 |
| --- | --- | --- |
| `perf_event_paranoid` | 观测其它进程 / 采集内核栈 | 观测其它进程需 `<= 0`；采集内核调用栈需 `<= 1`；否则以 **root** 运行 |
| `kptr_restrict = 0` | 解析内核符号地址 | 非 0 时内核帧只显示地址 |
| `perf` 已安装 | **方式一** | `apt-get install linux-tools-$(uname -r) linux-tools-generic` |
| tracefs + `kprobe_events` 可写 | **方式二 kprobe 后端** | 需内核启用 kprobe/ftrace，且有写权限(通常需 root) |
| `do_wp_page` 在 `/proc/kallsyms` | 两种方式 | 内核需导出该符号 |

> **最简单的做法**：以 `root` 运行 demo；或先 `sudo ./setup.sh --fix` 放宽 sysctl，
> 之后普通用户也能运行方式二。

---

## 5. 方式一：`perf record -e probe:do_wp_page`

由主程序在 **`fork` 出子进程后启动** 记录脚本，并在 **子进程退出前停止**：

```bash
sudo ./cow_demo --method 1 --size 16
```

内部流程（`src/cow_demo.c` 的 `run_method1` + `scripts/perf_cow_watch.sh`）：

1. `fork` 子进程（阻塞等待放行）。
2. 启动脚本 `record` 模式：`perf probe --add do_wp_page` 后前台运行
   `perf record -e probe:do_wp_page -g -p <子进程pid> -o <tmp>/perf.data`。
3. 热身若干毫秒（`--warmup`，默认 500ms）等待 perf 完成 attach，然后放行子进程写入(COW)。
4. 子进程写完并通知父进程后，父进程向记录进程发送 `SIGINT` 停止 perf（写出 `perf.data`），
   **此时子进程仍未退出**，满足“退出前停止脚本”。
5. 放行子进程退出；运行脚本 `report` 模式：`perf script` / `perf report` 输出调用栈，
   并按 `事件数 × 页大小` 汇总 COW 内存。

脚本也可单独使用：

```bash
scripts/perf_cow_watch.sh record --pid <PID> --out <DIR>   # 前台记录, Ctrl-C 停止
scripts/perf_cow_watch.sh report --out <DIR> --pages <N>   # 分析并汇总
```

---

## 6. 方式二：`perf_event_open()` 订阅 `do_wp_page`

由主程序自身用 C 接口订阅事件，无需外部 `perf` 命令：

```bash
sudo ./cow_demo --method 2 --size 16 --backend auto
```

实现要点（`src/cow_demo.c` 的 `run_method2`）：

1. 经 tracefs 注册 kprobe：向 `<tracefs>/kprobe_events` 写入
   `p:cowdemo/do_wp_page do_wp_page`，读取 `.../events/cowdemo/do_wp_page/id` 得到
   tracepoint id。
2. `perf_event_open(PERF_TYPE_TRACEPOINT, .config=id, ...)` 挂到子进程上，
   开启 `PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN`（同时采内核态与用户态调用栈）。
3. `mmap` 环形缓冲区，`PERF_EVENT_IOC_ENABLE` 后放行子进程；子进程逐页写入触发 COW。
4. 子进程写完后（仍存活）抓取 `/proc/<pid>/maps`，解析每条样本的调用栈：
   - 内核帧用 `/proc/kallsyms` 符号化（会看到 `do_wp_page`、`handle_mm_fault` 等）；
   - 用户帧用子进程 ELF 符号表符号化（会看到 `cow_touch_pages`、`child_main`、`main`）。
5. 统计事件数并输出 **文本报告 + COW 内存汇总**（`事件数 × 页大小`）。

### 回退后端 `--backend swfault`

若内核未开放 kprobe/tracefs（例如受限容器），可用可移植回退后端：

```bash
sudo ./cow_demo --method 2 --backend swfault --size 16
```

它改用 `perf_event_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS)` 订阅子进程的
**软件缺页事件**。采集通路（perf 环形缓冲 + `PERF_SAMPLE_CALLCHAIN`）与 kprobe 完全一致，
依然能采到 **子进程用户态调用栈** 并按缺页数汇总 COW 内存；只是事件源不是 `do_wp_page`
内核探针。程序会明确打印告警。`--backend auto`（默认）会优先尝试 kprobe，失败自动回退。

---

## 7. 命令行参数

```
--method N       观测方式: 1=perf record 脚本, 2=perf_event_open (默认 2)
--size M         申请内存大小, 单位 MiB (默认 16)
--backend B      方式二后端: auto|kprobe|swfault (默认 auto)
--script PATH    方式一脚本路径 (默认 scripts/perf_cow_watch.sh)
--warmup MS      方式一启动记录后的热身毫秒数 (默认 500)
--max-print N    最多打印多少条样本调用栈 (默认 3)
--max-frames N   每条调用栈最多打印多少帧 (默认 24)
--verbose        打印更多诊断信息
--help           显示帮助
```

---

## 8. 输出示例（方式二）

```
  [样本 #1] 调用栈 (自顶向下):
        #0  [u] cow_touch_pages+0x20 (cow_demo)
        #1  [u] child_main+0x25 (cow_demo)
        #2  [u] main+0x14b4 (cow_demo)
        #3  [u] __libc_start_main+0x8b (libc.so.6)
        #4  [u] _start+0x25 (cow_demo)

--------------------- COW 内存汇总 ---------------------
  目标子进程 PID     : 17184
  采集事件源         : probe:do_wp_page
  申请/写入内存      : 16 MiB (4096 页, 页大小 4096 字节)
  采集到事件数       : 4096
  估算 COW 内存      : 16777216 字节 (16.00 MiB) = 事件数 x 页大小
--------------------------------------------------------
```

`[k]` 为内核帧，`[u]` 为用户帧。使用 kprobe 后端时，内核栈顶即 `do_wp_page`。

---

## 9. 已知限制

- `do_wp_page` 触发表示处理写保护缺页；本 demo 通过“fork + 私有匿名内存 + 子进程逐页首写”
  的受控场景，确保每次事件对应一次 COW。
- 探针本身拿不到“改写字节数”，故按 `事件数 × 页大小` 估算 COW 内存。
- 方式一、方式二均需相应权限（见第 4 节），且依赖内核导出 `do_wp_page` 符号。
- 在完全禁用了 kprobe/ftrace/tracefs 的受限内核上，方式一与方式二 kprobe 后端无法运行，
  此时可用方式二的 `--backend swfault` 回退演示同样的采集通路与 COW 汇总。
