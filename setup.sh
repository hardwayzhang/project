#!/bin/sh
# COW watcher 运行环境与非 root 最小权限检测
#
# ./setup.sh          只检测
# ./setup.sh --fix    尝试安装 perf、挂载 tracefs，并把 perf_event_paranoid 调为 -1
#
# 不读取或解析内核地址/内核调用栈，因此不要求 kptr_restrict=0。

set -u

FIX=0
[ "${1:-}" = "--fix" ] && FIX=1

GREEN=$(printf '\033[32m'); RED=$(printf '\033[31m')
YELLOW=$(printf '\033[33m'); BOLD=$(printf '\033[1m'); RST=$(printf '\033[0m')
pass() { printf '  %s[PASS]%s %s\n' "$GREEN" "$RST" "$1"; }
warn() { printf '  %s[WARN]%s %s\n' "$YELLOW" "$RST" "$1"; }
fail() { printf '  %s[FAIL]%s %s\n' "$RED" "$RST" "$1"; }
section() { printf '\n%s== %s ==%s\n' "$BOLD" "$1" "$RST"; }

IS_ROOT=0
[ "$(id -u)" -eq 0 ] && IS_ROOT=1
SUDO=""
if [ "$IS_ROOT" -eq 0 ] && command -v sudo >/dev/null 2>&1 &&
   sudo -n true 2>/dev/null; then
    SUDO="sudo -n"
fi

run_root() {
    if [ "$IS_ROOT" -eq 1 ]; then
        sh -c "$1"
    elif [ -n "$SUDO" ]; then
        $SUDO sh -c "$1"
    else
        return 1
    fi
}

find_tracefs() {
    awk '$3=="tracefs"{print $2; exit}' /proc/mounts 2>/dev/null
    for d in /sys/kernel/tracing /sys/kernel/debug/tracing; do
        [ -e "$d/kprobe_events" ] && { echo "$d"; return; }
        $SUDO test -e "$d/kprobe_events" 2>/dev/null &&
            { echo "$d"; return; }
    done
}

section "基本信息"
echo "  内核版本 : $(uname -r)"
echo "  当前用户 : $(id -un) (uid=$(id -u))"
echo "  页大小   : $(getconf PAGESIZE) 字节"
if [ "$IS_ROOT" -eq 1 ]; then
    pass "当前为 root"
elif [ -n "$SUDO" ]; then
    pass "当前为非 root，可免密 sudo（仅 --fix 使用）"
else
    warn "当前为非 root，且无免密 sudo"
fi

if [ "$FIX" -eq 1 ]; then
    run_root "echo -1 > /proc/sys/kernel/perf_event_paranoid" 2>/dev/null || true
fi
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "?")

section "非 root 最小权限（请明确区分两种方式）"
cat <<EOF
  共同前提:
    1. watcher 与目标子进程同一 uid（本 demo 天然满足）；
    2. tracefs 已挂载；
    3. 非 root 可写 <tracefs>/kprobe_events，并可遍历/读取
       <tracefs>/events/kprobes/.../id（建议管理员用 ACL 只授权该用户）；
    4. 以下二选一:
       A. kernel.perf_event_paranoid = -1；或
       B. 相关可执行文件具有 CAP_PERFMON（仍需上面的 tracefs ACL）。

  方式一 (--watch method1):
    - 需要 perf；
    - 若选能力方案 B，应给 perf CAP_PERFMON；
    - 需要 perf probe 对 kprobe_events 的写权限。

  方式二 (--watch method2, kprobe 后端):
    - 不需要 perf 命令；
    - 若选能力方案 B，应给 cow_demo CAP_PERFMON；
    - 需要 cow_demo 对 kprobe_events 的写权限和事件 id 的读权限。

  方式二 swfault 回退:
    - 不需要 tracefs/kprobe；
    - 仅采用户态调用栈，同 uid 子进程通常在 perf_event_paranoid <= 2 时可用。

  本项目不采集内核调用栈、不解析内核地址，因此无需调整 kptr_restrict。
EOF

section "perf_event 权限"
echo "  perf_event_paranoid = $PARANOID"
if [ "$IS_ROOT" -eq 1 ]; then
    pass "root 可绕过 perf_event_paranoid"
elif [ "$PARANOID" = "-1" ]; then
    pass "方式一和方式二 kprobe 后端满足 perf_event 最小 sysctl 条件"
else
    warn "kprobe watcher 的非 root 最小值是 -1；也可用 CAP_PERFMON 代替"
fi

section "perf 工具（仅方式一需要）"
METHOD1_OK=1
if command -v perf >/dev/null 2>&1; then
    pass "perf 已安装: $(perf --version 2>/dev/null)"
else
    METHOD1_OK=0
    fail "未安装 perf"
    echo "  安装建议: sudo apt-get install linux-tools-\$(uname -r) linux-tools-generic"
    if [ "$FIX" -eq 1 ]; then
        echo "  尝试安装 perf ..."
        run_root "apt-get update -y >/dev/null 2>&1 &&
                  apt-get install -y linux-tools-common linux-tools-generic linux-tools-\$(uname -r) >/dev/null 2>&1" || true
        if command -v perf >/dev/null 2>&1; then
            pass "perf 安装成功"
            METHOD1_OK=1
        fi
    fi
fi

TRACEFS=$(find_tracefs | sed -n '1p')
if [ -z "$TRACEFS" ] && [ "$FIX" -eq 1 ]; then
    run_root "mkdir -p /sys/kernel/tracing &&
              mount -t tracefs nodev /sys/kernel/tracing" 2>/dev/null || true
    TRACEFS=$(find_tracefs | sed -n '1p')
fi

section "tracefs / do_wp_page kprobe（两种 kprobe watcher 共用）"
KPROBE_OK=0
if [ -z "$TRACEFS" ]; then
    fail "未找到 tracefs；方式一和方式二 kprobe 后端不可用"
else
    pass "tracefs: $TRACEFS"
    KPE="$TRACEFS/kprobe_events"
    if [ -w "$KPE" ]; then
        pass "当前非 root 用户可写 $KPE"
    elif [ "$IS_ROOT" -eq 1 ]; then
        pass "root 可写 $KPE"
    else
        fail "当前非 root 用户不可写 $KPE"
        echo "  最小授权示例（由管理员执行，按实际 tracefs 路径调整）:"
        echo "    sudo setfacl -m u:$(id -un):rx $TRACEFS $TRACEFS/events"
        echo "    sudo setfacl -m u:$(id -un):rw $KPE"
    fi

    # 直接实测按名字注册探针，不读取 /proc/kallsyms，也不解析内核地址。
    if [ -w "$KPE" ]; then
        if printf '%s\n' 'p:cowdemo_setup_check do_wp_page' >> "$KPE" 2>/dev/null; then
            pass "do_wp_page kprobe 注册实测通过（未解析内核符号地址）"
            printf '%s\n' '-:cowdemo_setup_check' >> "$KPE" 2>/dev/null || true
            KPROBE_OK=1
        else
            fail "do_wp_page kprobe 注册失败"
        fi
    elif [ "$IS_ROOT" -eq 1 ] || [ -n "$SUDO" ]; then
        if run_root "echo 'p:cowdemo_setup_check do_wp_page' >> '$KPE'" 2>/dev/null; then
            pass "do_wp_page kprobe 注册实测通过（未解析内核符号地址）"
            run_root "echo '-:cowdemo_setup_check' >> '$KPE'" 2>/dev/null || true
            KPROBE_OK=1
        else
            fail "do_wp_page kprobe 注册失败"
        fi
    fi
fi

section "结论"
if [ "$METHOD1_OK" -eq 1 ] && [ "$KPROBE_OK" -eq 1 ]; then
    pass "方式一依赖满足（仍需满足上面的 perf_event 非 root 条件）"
else
    warn "方式一依赖不完整"
fi
if [ "$KPROBE_OK" -eq 1 ]; then
    pass "方式二 kprobe 后端依赖满足（仍需满足 perf_event 条件）"
else
    warn "方式二 kprobe 后端不可用；可选 --backend swfault"
fi
if [ "$IS_ROOT" -eq 1 ] || [ "$PARANOID" != "?" ] &&
   [ "$PARANOID" -le 2 ] 2>/dev/null; then
    pass "方式二 swfault 回退预期可用"
else
    warn "方式二 swfault 回退可能受 perf_event_paranoid 限制"
fi

echo ""
echo "提示: --fix 只处理系统级依赖/sysctl；tracefs 的非 root ACL 应由管理员审慎配置。"
