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

### 失败诊断

记录阶段的全部输出（包括 `perf probe` 与 `perf record` 的原始报错）会写入
`<输出目录>/record.log`。方式一失败时，程序会直接回放该日志，而不是只报告
“找不到 perf.data”，并给出记录进程的退出状态与常见原因：

```text
[方式一][失败] 未生成 /tmp/cow_demo_perfXXXX/perf.data (No such file or directory)
  记录进程结束情况: 被信号终止, signal=2 (SIGINT, 即本程序发出的正常停止信号)
  ---- 记录阶段输出 (/tmp/cow_demo_perfXXXX/record.log) ----
  | [方式一] perf probe --add do_wp_page
  | Permission denied
  ---- 日志结束 ----
  常见原因:
    1) perf probe --add do_wp_page 失败 (无 root/CAP_PERFMON, 或 tracefs kprobe_events 不可写);
    ...
```

失败分两种情况：记录进程在热身期内就退出（通常是 `perf` 缺失或 `perf probe` 失败），
以及记录进程正常运行但没有产出 `perf.data`。两种情况都会回放日志，并让 `cow_demo`
以非零状态退出。加 `--verbose` 可在成功时也打印该日志。

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
采集逻辑。默认 `auto` 会先尝试 kprobe，失败后自动回退。

发生回退时程序会打印具体原因，例如：

```text
 [告警] 未能使用 do_wp_page kprobe, 已回退到软件缺页后端。
 [回退原因] 未找到 tracefs (已尝试 /proc/mounts、/sys/kernel/tracing、
            /sys/kernel/debug/tracing); 内核可能未启用 ftrace/kprobe, 或容器未挂载 tracefs
 [tracefs]   未找到
```

可能的回退原因包括：tracefs 未挂载；`kprobe_events` 打开或写入失败（`EACCES`
表示缺少 root/ACL，`ENOENT`/`EINVAL` 表示符号不可探测）；事件 id 读取失败；
tracepoint 的 `perf_event_open` 被 `perf_event_paranoid` 拒绝。

#### 两种事件源的语义区别

| | `do_wp_page` kprobe | `PERF_COUNT_SW_PAGE_FAULTS` |
| --- | --- | --- |
| 触发条件 | 只在写入“已存在但只读”的页时触发 | 进程的任意缺页都触发 |
| 与 COW 的关系 | 一次事件对应一次真实页复制 | 包含 COW，也包含非复制类缺页 |
| 统计精度 | 精确 | 近似，偏大 |

`do_wp_page()` 是内核的写保护缺页处理函数，正是 `fork` 之后的 COW 路径，所以
`事件数 × 页大小` 就是 COW 内存量。而软件缺页事件还会统计首次访问匿名页
（`do_anonymous_page`）、文件页读入（`filemap_fault`）、栈扩展等并不发生页复制的缺页。

本 demo 中子进程只对 `fork` 前已驻留的私有页逐页写入，绝大多数缺页就是 COW，因此
缺页数约等于 COW 数；但仍会多出少量非 COW 缺页（例如子进程首次执行 libc 代码路径），
统计值因此略微偏大。

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
- 虽然只采用户态调用栈，但缺页事件由内核产生；同 uid 子进程仍要求
  `perf_event_paranoid <= 1`，或 `cow_demo` 具有 `CAP_PERFMON`。

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
