#!/bin/sh
# ============================================================================
# setup.sh - 运行环境检测 (方式一/方式二 的权限与内核能力)
# ============================================================================
#
#   ./setup.sh          仅检测并给出 PASS/WARN/FAIL 报告
#   ./setup.sh --fix    在检测的同时尝试用 sudo 修复(放宽 sysctl / 挂载 tracefs /
#                       安装 perf)
#
# 检测项:
#   - 是否 root / 是否有 sudo
#   - perf_event_paranoid / kptr_restrict
#   - perf 是否安装 (方式一 需要)
#   - tracefs 是否可用, kprobe_events 是否可写 (方式二 kprobe 后端 需要)
#   - do_wp_page 是否在 /proc/kallsyms, 是否可被探测
#   - 软件缺页后端 (方式二 回退) 是否可用
# ============================================================================

FIX=0
[ "${1:-}" = "--fix" ] && FIX=1

GREEN=$(printf '\033[32m'); RED=$(printf '\033[31m')
YELLOW=$(printf '\033[33m'); BOLD=$(printf '\033[1m'); RST=$(printf '\033[0m')

pass() { printf '  %s[PASS]%s %s\n' "$GREEN" "$RST" "$1"; }
warn() { printf '  %s[WARN]%s %s\n' "$YELLOW" "$RST" "$1"; }
fail() { printf '  %s[FAIL]%s %s\n' "$RED" "$RST" "$1"; }
head2(){ printf '\n%s== %s ==%s\n' "$BOLD" "$1" "$RST"; }

IS_ROOT=0
[ "$(id -u)" -eq 0 ] && IS_ROOT=1
SUDO=""
if [ "$IS_ROOT" -eq 0 ] && command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    SUDO="sudo -n"
fi
# 能否以特权执行(用于 --fix)
PRIV=""
[ "$IS_ROOT" -eq 1 ] && PRIV="sh -c"
[ -n "$SUDO" ] && PRIV="$SUDO sh -c"

run_priv() {  # run_priv "<shell command>"
    [ -n "$PRIV" ] || { warn "无特权, 跳过: $1"; return 1; }
    $PRIV "$1"
}

METHOD1_OK=1
METHOD2_KP_OK=1
METHOD2_SW_OK=1

# ---------------------------------------------------------------- 基本信息
head2 "基本信息"
echo "  内核版本 : $(uname -r)"
echo "  架构     : $(uname -m)"
echo "  页大小   : $(getconf PAGESIZE) 字节"
if [ "$IS_ROOT" -eq 1 ]; then
    pass "以 root 运行"
elif [ -n "$SUDO" ]; then
    pass "非 root, 但可免密 sudo"
else
    warn "非 root 且无免密 sudo, 部分能力可能受限"
fi

# ---------------------------------------------------------------- sysctl
head2 "perf 相关 sysctl"
if [ "$FIX" -eq 1 ]; then
    run_priv "echo -1 > /proc/sys/kernel/perf_event_paranoid" >/dev/null 2>&1
    run_priv "echo 0  > /proc/sys/kernel/kptr_restrict" >/dev/null 2>&1
fi
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "?")
KPTR=$(cat /proc/sys/kernel/kptr_restrict 2>/dev/null || echo "?")
echo "  perf_event_paranoid = $PARANOID (<=0 才能观测其它进程; <=1 才能采集内核调用栈)"
echo "  kptr_restrict       = $KPTR (0 才能解析内核符号地址)"

if [ "$PARANOID" = "?" ]; then
    warn "无法读取 perf_event_paranoid"
elif [ "$IS_ROOT" -eq 1 ] || [ "$PARANOID" -le 1 ] 2>/dev/null; then
    pass "perf_event_paranoid 满足内核栈采集 (=$PARANOID)"
else
    warn "perf_event_paranoid=$PARANOID 偏严, 建议以 root 运行或 ./setup.sh --fix"
fi
if [ "$KPTR" = "0" ] || [ "$IS_ROOT" -eq 1 ]; then
    pass "可解析内核符号地址 (kptr_restrict=$KPTR)"
else
    warn "kptr_restrict=$KPTR, 内核帧可能显示为地址, 建议 --fix"
fi

# ---------------------------------------------------------------- do_wp_page 符号
head2 "do_wp_page 内核符号"
if grep -qw do_wp_page /proc/kallsyms 2>/dev/null; then
    ADDR=$( ( $SUDO cat /proc/kallsyms 2>/dev/null || cat /proc/kallsyms ) | \
            awk '$3=="do_wp_page"{print $1; exit}')
    pass "在 /proc/kallsyms 中找到 do_wp_page (addr=$ADDR)"
else
    fail "未找到 do_wp_page 符号 (该内核可能无法探测)"
    METHOD1_OK=0; METHOD2_KP_OK=0
fi

# ---------------------------------------------------------------- perf (方式一)
head2 "perf 工具 (方式一 需要)"
if command -v perf >/dev/null 2>&1; then
    pass "perf 已安装: $(perf --version 2>/dev/null)"
else
    fail "未安装 perf"
    METHOD1_OK=0
    if [ "$FIX" -eq 1 ]; then
        echo "  尝试安装 perf ..."
        run_priv "apt-get update -y >/dev/null 2>&1 && \
                  apt-get install -y linux-tools-common linux-tools-generic \
                  linux-tools-\$(uname -r) >/dev/null 2>&1" && \
            command -v perf >/dev/null 2>&1 && { pass "perf 安装成功"; METHOD1_OK=1; } || \
            warn "自动安装 perf 失败, 请手动安装匹配当前内核的 linux-tools"
    else
        echo "  安装建议: sudo apt-get install linux-tools-\$(uname -r) linux-tools-generic"
    fi
fi

# ---------------------------------------------------------------- tracefs / kprobe (方式二 kprobe)
head2 "tracefs / kprobe (方式二 kprobe 后端 需要)"
find_tracefs() {
    # 已挂载的 tracefs
    awk '$3=="tracefs"{print $2; exit}' /proc/mounts 2>/dev/null && return 0
    for d in /sys/kernel/tracing /sys/kernel/debug/tracing; do
        if $SUDO test -e "$d/kprobe_events" 2>/dev/null || [ -e "$d/kprobe_events" ]; then
            echo "$d"; return 0
        fi
    done
    return 1
}
TRACEFS=$(find_tracefs)

if [ -z "$TRACEFS" ] && [ "$FIX" -eq 1 ]; then
    echo "  未发现 tracefs, 尝试挂载 ..."
    run_priv "mkdir -p /sys/kernel/tracing && mount -t tracefs nodev /sys/kernel/tracing" 2>/dev/null
    run_priv "mount -t tracefs nodev /sys/kernel/debug/tracing" 2>/dev/null
    TRACEFS=$(find_tracefs)
fi

if [ -n "$TRACEFS" ]; then
    pass "tracefs 可用: $TRACEFS"
    KPE="$TRACEFS/kprobe_events"
    if [ -w "$KPE" ] || [ "$IS_ROOT" -eq 1 ] || [ -n "$SUDO" ]; then
        # 实测: 注册并删除一个临时 do_wp_page 探针
        if run_priv "echo 'p:cowdemo_probe do_wp_page' > $KPE" 2>/dev/null; then
            if $SUDO test -e "$TRACEFS/events/kprobes/cowdemo_probe/id" 2>/dev/null; then
                pass "成功注册 do_wp_page kprobe (实测通过)"
            else
                warn "写入 kprobe_events 成功但未见事件 id"
            fi
            run_priv "echo '-:cowdemo_probe' > $KPE" 2>/dev/null
        else
            fail "无法写入 $KPE 注册 kprobe"
            METHOD2_KP_OK=0
        fi
    else
        fail "$KPE 不可写 (需 root)"
        METHOD2_KP_OK=0
    fi
else
    fail "未找到 tracefs (内核可能未启用 kprobe/ftrace)"
    METHOD2_KP_OK=0
    echo "  说明: 方式二可用 --backend swfault 回退(不依赖 tracefs)。"
fi

# ---------------------------------------------------------------- 软件缺页后端 (方式二回退)
head2 "软件缺页后端 (方式二 回退, 无需 tracefs)"
# 软件事件对本进程通常允许(paranoid<=2)
if [ "$PARANOID" = "?" ]; then
    warn "无法判断, 但通常可用"
elif [ "$IS_ROOT" -eq 1 ] || [ "$PARANOID" -le 2 ] 2>/dev/null; then
    pass "可使用 PERF_COUNT_SW_PAGE_FAULTS (paranoid=$PARANOID)"
else
    warn "paranoid=$PARANOID 可能限制采集"
    METHOD2_SW_OK=0
fi

# ---------------------------------------------------------------- 汇总
head2 "结论"
if [ "$METHOD1_OK" -eq 1 ]; then
    pass "方式一 (perf record -e probe:do_wp_page) 预期可用"
else
    fail "方式一 不满足 (缺 perf 或 kprobe/符号)"
fi
if [ "$METHOD2_KP_OK" -eq 1 ]; then
    pass "方式二 kprobe 后端 (perf_event_open + do_wp_page) 预期可用"
else
    warn "方式二 kprobe 后端 不满足, 请改用 --backend swfault"
fi
if [ "$METHOD2_SW_OK" -eq 1 ]; then
    pass "方式二 swfault 回退后端 预期可用"
else
    warn "方式二 swfault 回退后端 可能受限"
fi

echo ""
echo "提示: 若权限不足, 可尝试:  ./setup.sh --fix   或以 root 运行 demo。"
