# COW `do_wp_page` 事件观测 Demo

父进程申请并写入私有匿名内存，`fork()` 后子进程逐页首次写入，触发
`do_wp_page()` 完成 Copy-On-Write。本程序可同时或分别使用两种 watcher：

- 方式一：`perf record -e probe:do_wp_page`；
- 方式二：C 代码通过 `perf_event_open()` 订阅同一个 kprobe tracepoint。

两种方式都只采集并展示子进程用户态调用栈，不采集内核调用栈，不读取
`/proc/kallsyms`，也不解析内核地址。

## 构建与环境检测

```bash
make
./setup.sh          # 只检测
./setup.sh --fix    # 尝试安装 perf、挂载 tracefs、设置 perf_event_paranoid=-1
```

`setup.sh` 会实测 tracefs、`do_wp_page` kprobe 注册和当前用户权限，并明确打印
方式一、方式二各自需要的非 root 最小权限。

## 选择 watcher

```bash
# 不设置 --watch：默认同时启动方式一和方式二，观测同一个子进程
sudo ./cow_demo --size 16

# 只启动方式一
sudo ./cow_demo --watch method1 --size 16

# 只启动方式二
sudo ./cow_demo --watch method2 --size 16

# 显式同时启动
sudo ./cow_demo --watch both --size 16
```

默认 `both` 的执行顺序：

1. 父进程申请、预写内存并 `fork()`，子进程阻塞等待；
2. 启动方式一的 `perf record`；
3. 启动方式二的 `perf_event_open()` collector；
4. 两个 watcher 均完成启动尝试后，父进程放行子进程逐页写入；
5. 子进程写完后仍保持存活，父进程先停止两个 watcher；
6. 父进程允许子进程退出，输出两种方式的用户态调用栈和 COW 内存汇总。

如果其中一个 watcher 因环境依赖失败，另一个仍会继续运行，程序最终以非零状态报告
存在 watcher 启动失败。

## 命令行参数

```text
--watch W        method1|method2|both（默认 both）
--size M         内存大小，单位 MiB（默认 16）
--backend B      方式二后端：auto|kprobe|swfault（默认 auto）
--script PATH    方式一脚本（默认 scripts/perf_cow_watch.sh）
--warmup MS      方式一等待 perf attach 的时间（默认 500ms）
--max-print N    方式二最多打印的样本数（默认 3）
--max-frames N   每条用户态调用栈最多打印的帧数（默认 24）
--verbose        诊断输出
--help           帮助
```

## 方式一

主程序在 `fork()` 后启动 `scripts/perf_cow_watch.sh`：

```bash
perf probe --add do_wp_page
perf record --user-callchains -e probe:do_wp_page -g -p <child-pid>
```

`--user-callchains` 设置 `exclude_callchain_kernel=1`，`perf.data` 不包含内核调用链。
子进程写完但尚未退出时，父进程向 perf 发送 `SIGINT`，随后脚本用 `perf script`
输出用户态调用栈并按以下公式汇总：

```text
COW 内存 = do_wp_page 事件数 × 页面大小
```

脚本也可单独使用：

```bash
scripts/perf_cow_watch.sh record --pid <PID> --out <DIR>
scripts/perf_cow_watch.sh report --out <DIR> --pages <N>
```

## 方式二

方式二不调用 `perf` 命令：

1. 向 `<tracefs>/kprobe_events` 注册 `p:cowdemo/do_wp_page do_wp_page`；
2. 读取动态事件 id；
3. 使用 `perf_event_open(PERF_TYPE_TRACEPOINT)` 订阅目标子进程；
4. 设置 `PERF_SAMPLE_CALLCHAIN` 和 `exclude_callchain_kernel=1`；
5. 从 mmap 环形缓冲持续读取样本；
6. 仅用 `/proc/<pid>/maps` 和用户态 ELF 符号表解析子进程调用栈；
7. 输出文本调用栈、事件数及 COW 内存汇总。

输出中的用户态调用链类似：

```text
cow_touch_pages → child_main → main → __libc_start_main → _start
```

### `swfault` 回退

内核没有开放 tracefs/kprobe 时：

```bash
./cow_demo --watch method2 --backend swfault --size 16
```

回退后端订阅 `PERF_COUNT_SW_PAGE_FAULTS`，仍使用相同的环形缓冲与用户态调用栈
采集逻辑。它统计所有子进程缺页，因此可能比实际 COW 页数略多。默认 `auto` 会先尝试
kprobe，失败后自动回退。

## 非 root 最小权限

以下是运行 kprobe watcher 的最小条件，不要求 `kptr_restrict=0`。

### 两种方式的共同条件

1. watcher 与目标进程属于同一 uid（本 demo 的父子进程天然满足）；
2. tracefs 已挂载；
3. 当前用户可写 `<tracefs>/kprobe_events`，并可遍历、读取
   `<tracefs>/events/kprobes/.../id`；
4. 满足下列 perf_event 权限之一：
   - `kernel.perf_event_paranoid = -1`；或
   - watcher 可执行文件具有 `CAP_PERFMON`。

`perf_event_paranoid >= 0` 会禁止无 `CAP_PERFMON` 用户使用 ftrace/kprobe
tracepoint，因此仅设置为 `0` 不够。

### 方式一额外条件

- 安装与当前内核匹配的 `perf`；
- 使用 capability 方案时，`perf` 需要 `CAP_PERFMON`；
- `perf probe` 进程必须具备上述 tracefs ACL。

### 方式二额外条件

- 不需要安装 `perf`；
- 使用 capability 方案时，`cow_demo` 需要 `CAP_PERFMON`；
- `cow_demo` 自身必须具备上述 tracefs ACL。

### 方式二 `swfault` 回退

- 不需要 tracefs 或 kprobe 写权限；
- 因只采用户态调用栈，同 uid 子进程通常在 `perf_event_paranoid <= 2` 时可用。

ACL 示例（由管理员执行，路径按机器实际 tracefs 挂载点调整）：

```bash
sudo setfacl -m u:$USER:rx /sys/kernel/tracing /sys/kernel/tracing/events
sudo setfacl -m u:$USER:rw /sys/kernel/tracing/kprobe_events
```

能力示例（授予可执行文件能力具有安全影响，请由管理员评估）：

```bash
sudo setcap cap_perfmon+ep "$(command -v perf)"  # 方式一
sudo setcap cap_perfmon+ep ./cow_demo            # 方式二
```

## 已知限制

- `do_wp_page` 表示处理写保护缺页；本 demo 通过“fork + 私有匿名内存 + 逐页首次
  写入”的受控场景，使每次事件对应一次 COW。
- kprobe 本身不直接提供复制字节数，内存量按事件数乘页面大小估算。
- 程序使用 `MADV_NOHUGEPAGE`，避免透明大页影响按基页统计。
- 受限容器可能完全禁用 tracefs/kprobe；此时方式一和方式二 kprobe 后端不可用，
  可使用方式二 `swfault` 回退。
