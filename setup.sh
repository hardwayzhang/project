#!/bin/sh
# 检查两种 COW watcher 的实际执行权限。
#   ./setup.sh          只检测
#   ./setup.sh --fix    尝试安装 perf、挂载 tracefs（不会自动修改 ACL/sudoers）

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
HAVE_SUDO=0
command -v sudo >/dev/null 2>&1 && HAVE_SUDO=1

run_root() {
    if [ "$IS_ROOT" -eq 1 ]; then
        sh -c "$1"
    elif [ "$HAVE_SUDO" -eq 1 ]; then
        sudo -n sh -c "$1"
    else
        return 1
    fi
}

find_tracefs() {
    awk '$3=="tracefs"{print $2; exit}' /proc/mounts 2>/dev/null
    for d in /sys/kernel/tracing /sys/kernel/debug/tracing; do
        [ -e "$d/kprobe_events" ] && { echo "$d"; return; }
        [ "$HAVE_SUDO" -eq 1 ] &&
            sudo -n test -e "$d/kprobe_events" 2>/dev/null &&
            { echo "$d"; return; }
    done
}

section "基本信息"
echo "  内核版本 : $(uname -r)"
echo "  当前用户 : $(id -un), uid=$(id -u), euid=$(id -u)"
echo "  页大小   : $(getconf PAGESIZE) 字节"
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "?")
echo "  perf_event_paranoid = $PARANOID"

if [ "$FIX" -eq 1 ]; then
    if ! command -v perf >/dev/null 2>&1; then
        echo "  尝试安装 perf ..."
        run_root "apt-get update -y >/dev/null 2>&1 &&
                  apt-get install -y linux-tools-common linux-tools-generic linux-tools-\$(uname -r) >/dev/null 2>&1" ||
            warn "自动安装 perf 失败"
    fi
    TRACEFS=$(find_tracefs | sed -n '1p')
    if [ -z "$TRACEFS" ]; then
        run_root "mkdir -p /sys/kernel/tracing &&
                  mount -t tracefs nodev /sys/kernel/tracing" 2>/dev/null || true
    fi
fi
TRACEFS=$(find_tracefs | sed -n '1p')

section "方式一：非 root cow_demo + sudo -n perf"
M1_OK=1
if [ "$IS_ROOT" -eq 1 ]; then
    if command -v perf >/dev/null 2>&1; then
        pass "当前已是 root，方式一直接执行 perf: $(perf --version 2>/dev/null)"
    else
        fail "root 的 PATH 中未找到 perf"
        M1_OK=0
    fi
else
    if [ "$HAVE_SUDO" -ne 1 ]; then
        fail "未安装 sudo；方式一非 root 模式无法执行 sudo perf"
        M1_OK=0
    elif sudo -n perf --version >/dev/null 2>&1; then
        pass "sudo -n perf 可用: $(sudo -n perf --version 2>/dev/null)"
        echo "  脚本会用 sudo -n perf 执行 probe/record/script/report。"
    else
        fail "sudo -n perf 不可用"
        echo "  请先执行: sudo -v"
        echo "  再确认 sudoers 允许 perf，且 sudo secure_path 中能找到 perf。"
        M1_OK=0
    fi
fi

if [ -z "$TRACEFS" ]; then
    fail "未找到 tracefs；即使 sudo perf 可用也无法注册 do_wp_page"
    M1_OK=0
else
    pass "tracefs 已挂载: $TRACEFS"
    # 方式一实际由 root perf 注册，所以这里也按同一权限模型实测。
    if run_root "echo 'p:cowdemo_setup_m1 do_wp_page' >> '$TRACEFS/kprobe_events'" 2>/dev/null; then
        pass "用方式一权限模型注册 do_wp_page 实测通过"
        run_root "echo '-:cowdemo_setup_m1' >> '$TRACEFS/kprobe_events'" 2>/dev/null || true
    else
        fail "sudo/root 写 kprobe_events 注册 do_wp_page 失败"
        M1_OK=0
    fi
fi

section "方式二：cow_demo 自身权限（不提权、不 fallback）"
M2_OK=1
echo "  方式二不会调用 sudo，也不会回退到 page-fault。"
if [ "$IS_ROOT" -eq 1 ]; then
    pass "当前以 root 运行 setup；sudo ./cow_demo 将使用同样权限"
else
    warn "当前是非 root；若下述直接注册失败，请由执行者改用 sudo ./cow_demo"
fi

if [ -z "$TRACEFS" ]; then
    fail "未找到 tracefs"
    M2_OK=0
else
    KPE="$TRACEFS/kprobe_events"
    # 必须直接写，刻意不通过 sudo：这才是非 root ./cow_demo 的真实权限。
    if printf '%s\n' 'p:cowdemo_setup_m2 do_wp_page' >> "$KPE" 2>/dev/null; then
        pass "当前 cow_demo 用户可直接注册 do_wp_page"
        ID="$TRACEFS/events/kprobes/cowdemo_setup_m2/id"
        if [ -r "$ID" ]; then
            pass "当前用户可读取动态事件 id: $(cat "$ID" 2>/dev/null)"
        else
            fail "已注册探针，但当前用户不能读取 $ID"
            M2_OK=0
        fi
        printf '%s\n' '-:cowdemo_setup_m2' >> "$KPE" 2>/dev/null || true
    else
        fail "当前用户不能直接写 $KPE"
        M2_OK=0
        echo "  推荐: sudo ./cow_demo"
        echo "  或由管理员配置 tracefs ACL，并设置 perf_event_paranoid=-1/CAP_PERFMON。"
    fi
fi

if [ "$IS_ROOT" -ne 1 ] && [ "$PARANOID" != "-1" ]; then
    warn "非 root tracepoint perf_event_open 通常还要求 perf_event_paranoid=-1 或 CAP_PERFMON"
    M2_OK=0
fi

section "结论"
if [ "$M1_OK" -eq 1 ]; then
    pass "方式一权限满足：可非 root 运行 cow_demo，内部通过 sudo -n perf"
else
    fail "方式一权限/依赖不满足"
fi
if [ "$M2_OK" -eq 1 ]; then
    pass "方式二可按当前用户直接运行"
else
    warn "方式二按当前用户不可用；请执行 sudo ./cow_demo"
fi

echo ""
echo "推荐命令:"
echo "  仅方式一（cow_demo 非 root）: sudo -v && ./cow_demo --watch method1"
echo "  方式二或默认 both          : sudo ./cow_demo"
