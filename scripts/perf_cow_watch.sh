#!/bin/sh
# ============================================================================
# perf_cow_watch.sh - 方式一: 用 perf record 追踪子进程的 do_wp_page(COW) 事件
# ============================================================================
#
# 由 cow_demo (--watch method1/both) 调用, 分两个子命令:
#
#   record --pid <PID> --out <DIR>
#       注册 do_wp_page 探针(perf probe --add), 然后前台运行
#         perf record --user-callchains -e probe:do_wp_page -g -p <PID>
#       调用方(cow_demo)在子进程写完 COW、退出之前, 向本进程发送 SIGINT,
#       perf 收尾并写出 perf.data。
#
#   report --out <DIR> --pages <N>
#       用 perf script / perf report 输出调用栈, 并按 事件数 x 页大小
#       估算 COW 内存大小。
#
# 也可单独手动调用本脚本(见 --help)。
# ============================================================================

set -u

PROG=$(basename "$0")
PAGE_SIZE=$(getconf PAGESIZE 2>/dev/null || echo 4096)
PROBE_NAME="probe:do_wp_page"
USE_SUDO=0
PERF_MODE="未初始化"

log()  { printf '%s\n' "$*" >&2; }
die()  { log "[方式一][错误] $*"; exit 1; }

usage() {
    cat >&2 <<EOF
用法:
  $PROG record --pid <PID> --out <DIR> [--skip-probe]
  $PROG report --out <DIR> [--pages <N>]
  $PROG add-probe        # 仅注册 do_wp_page 探针
  $PROG del-probe        # 仅删除 do_wp_page 探针
EOF
}

need_perf() {
    if [ "$(id -u)" -eq 0 ]; then
        command -v perf >/dev/null 2>&1 ||
            die "root 的 PATH 中未找到 perf, 请安装匹配内核的 linux-tools。"
        USE_SUDO=0
        PERF_MODE="perf (cow_demo 已是 root)"
        return
    fi

    command -v sudo >/dev/null 2>&1 ||
        die "当前是非 root, 但未安装 sudo。方式一要求通过 sudo perf 执行。"
    if ! sudo -n perf --version >/dev/null 2>&1; then
        die "sudo -n perf 不可用。请先执行 sudo -v 缓存凭据，确认 sudoers 允许 perf，并确认 sudo 的 secure_path 中能找到 perf。"
    fi
    USE_SUDO=1
    PERF_MODE="sudo -n perf (cow_demo 以 uid=$(id -u) 运行)"
}

run_perf() {
    if [ "$USE_SUDO" -eq 1 ]; then
        sudo -n perf "$@"
    else
        perf "$@"
    fi
}

# 分析阶段(perf script/report)专用: 只读 perf.data, 身份由 cmd_report 决定。
ANALYZE_SUDO=1
run_analyze() {
    if [ "$ANALYZE_SUDO" -eq 1 ]; then
        sudo -n perf "$@"
    else
        perf "$@"
    fi
}

# 确保 do_wp_page 探针存在(幂等)。
# 这里不屏蔽 perf 的输出: record 模式下调用方会把 stdout/stderr 收集到
# <out>/record.log, 失败时需要这些信息来定位原因。
add_probe() {
    need_perf
    log "[方式一] 权限模式: $PERF_MODE"
    log "[方式一] perf 版本: $(run_perf --version 2>&1)"
    log "[方式一] cow_demo 用户: $(id -un) (uid=$(id -u)), perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo '?')"

    if run_perf probe -l 2>&1 | grep -q "do_wp_page"; then
        log "[方式一] 探针 $PROBE_NAME 已存在。"
        return 0
    fi

    # perf probe 要解析 kallsyms/debuginfo, 在大内核上可能耗时数秒甚至更久,
    # 所以调用方会在放行子进程之前同步执行它, 而不是塞进热身等待。
    log "[方式一] 执行(可能耗时较久): perf probe --add do_wp_page"
    if run_perf probe --add do_wp_page 2>&1; then
        log "[方式一] 已注册探针 $PROBE_NAME。"
        return 0
    fi
    die "注册探针失败(上方为 perf 原始输出)。需 root/CAP_PERFMON 且内核支持 kprobe。"
}

del_probe() {
    need_perf || return 0
    run_perf probe --del "do_wp_page" >/dev/null 2>&1 || \
        run_perf probe --del "$PROBE_NAME" >/dev/null 2>&1 || true
    log "[方式一] 已删除探针(若存在)。"
}

cmd_record() {
    PID=""
    OUT=""
    SKIP_PROBE=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --pid) PID="$2"; shift 2;;
            --out) OUT="$2"; shift 2;;
            --skip-probe) SKIP_PROBE=1; shift;;
            *) die "record: 未知参数 $1";;
        esac
    done
    [ -n "$PID" ] || die "record 需要 --pid"
    [ -n "$OUT" ] || die "record 需要 --out"
    mkdir -p "$OUT"

    need_perf
    # --skip-probe: 探针已由调用方提前同步注册好, 这里直接进入 perf record,
    # 让"启动记录进程"到"真正开始采样"之间的延迟尽量小。
    if [ "$SKIP_PROBE" -eq 1 ]; then
        log "[方式一] 探针由调用方预先注册, 跳过注册。权限模式: $PERF_MODE"
    else
        add_probe
    fi

    if ! kill -0 "$PID" 2>/dev/null; then
        die "目标进程 $PID 不存在或不可见, 无法 attach。"
    fi

    log "[方式一] 开始记录子进程用户态调用栈:"
    log "         perf record --user-callchains -e $PROBE_NAME -g -p $PID -o $OUT/perf.data"
    # 用 exec 让调用方发来的 SIGINT 直达 perf, 由 perf 自己收尾写出 perf.data
    # (这是最可靠的停止路径)。若经由 sudo 运行, perf 以 root 身份创建的
    # perf.data 会是 root:0600, 其属主由 report 阶段负责改回调用用户。
    if [ "$USE_SUDO" -eq 1 ]; then
        exec sudo -n perf record --user-callchains -e "$PROBE_NAME" -g -p "$PID" \
            -o "$OUT/perf.data"
    else
        exec perf record --user-callchains -e "$PROBE_NAME" -g -p "$PID" \
            -o "$OUT/perf.data"
    fi
}

cmd_report() {
    OUT=""
    PAGES=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --out) OUT="$2"; shift 2;;
            --pages) PAGES="$2"; shift 2;;
            *) die "report: 未知参数 $1";;
        esac
    done
    [ -n "$OUT" ] || die "report 需要 --out"
    DATA="$OUT/perf.data"
    [ -f "$DATA" ] || die "找不到 $DATA (记录阶段可能失败)。"

    need_perf

    # perf.data 可能由 sudo 下的 perf 以 root:0600 生成, 非 root 的调用方读不到。
    # 这里把属主改回调用用户(即运行本脚本的 cow_demo 用户), 使其可直接读取/留存;
    # chown 失败时退化为 chmod a+r。已可读则跳过。
    if [ ! -r "$DATA" ] && [ "$USE_SUDO" -eq 1 ]; then
        if sudo -n chown "$(id -u):$(id -g)" "$DATA" 2>/dev/null; then
            log "[方式一] 已将 $DATA 属主改为 uid=$(id -u)。"
        elif sudo -n chmod a+r "$DATA" 2>/dev/null; then
            log "[方式一] 已放宽 $DATA 为全体可读。"
        else
            log "[方式一] 无法修正 $DATA 权限, 将继续用 sudo perf 读取分析。"
        fi
    fi

    # 只保留一行十进制数字, 其余情况一律归零。
    # grep -c 在无匹配时会打印 0 并返回 1, 因此不能写成 "|| echo 0"。
    sanitize_count() {
        case "$1" in
            ''|*[!0-9]*) echo 0 ;;
            *) echo "$1" ;;
        esac
    }

    # 选择分析身份。分析只是读文件, 不需要特权, 而 perf 有属主安全检查:
    #   util/data.c: if (!force && st.st_uid && (st.st_uid != geteuid())) -> 拒绝
    # 即"文件属主非 root 且不等于 perf 进程 euid"时报
    #   File ... not owned by current user or root (use -f to override)
    # 上面已把 perf.data 归还给调用用户, 此时若再用 sudo(root) 分析,
    # root 眼中文件属于 uid=1000, 正好命中该检查。所以优先用当前用户身份分析;
    # 只有当前用户读不到(chown 失败, 文件仍是 root:0600)时才退回 sudo,
    # 那种情况下 st_uid==0, 检查同样会放行。
    if [ -r "$DATA" ] && command -v perf >/dev/null 2>&1; then
        ANALYZE_SUDO=0
        ANALYZE_MODE="perf (当前用户身份, uid=$(id -u))"
    else
        ANALYZE_SUDO=1
        ANALYZE_MODE="sudo -n perf (当前用户读不到 perf.data)"
    fi
    log "[方式一] 分析身份: $ANALYZE_MODE"

    echo "---------------- 子进程用户态调用栈 (perf script) ----------------"
    # 记录阶段使用 --user-callchains, perf.data 中不包含内核调用链。
    # perf script 的 stderr 不能丢弃, 它是"没有输出"时唯一的线索。
    run_analyze script -i "$DATA" >"$OUT/script.txt" 2>"$OUT/script.err" || true

    # 兜底: 若仍撞上属主检查(例如属主被第三方改动), 用 --force 重试一次。
    if [ ! -s "$OUT/script.txt" ] &&
       grep -q "not owned by current user" "$OUT/script.err" 2>/dev/null; then
        log "[方式一] 命中 perf 属主检查, 改用 --force 重试。"
        run_analyze script --force -i "$DATA" \
            >"$OUT/script.txt" 2>"$OUT/script.err" || true
    fi
    if [ -s "$OUT/script.txt" ]; then
        head -n 60 "$OUT/script.txt"
    else
        log "[方式一] perf script 没有输出。"
        if [ -s "$OUT/script.err" ]; then
            log "[方式一] perf script 报错如下:"
            sed 's/^/    /' "$OUT/script.err" >&2
        fi
    fi

    EVENTS=$(sanitize_count "$(grep -c 'probe:do_wp_page' "$OUT/script.txt" 2>/dev/null || true)")

    # perf record 自己在结束时会打印 "(N samples)", 它记录在 record.log 中,
    # 是判断"到底有没有采到事件"最直接的依据。
    # 找不到该行时保持为空, 以便和 "确实采到 0 个样本" 区分开
    RECORDED=""
    if [ -f "$OUT/record.log" ]; then
        RECORDED=$(sed -n 's/.*(\([0-9][0-9]*\) samples).*/\1/p' \
                   "$OUT/record.log" 2>/dev/null | tail -n1)
        case "$RECORDED" in
            ''|*[!0-9]*) RECORDED="" ;;
        esac
    fi

    SOURCE="perf script"
    if [ "$EVENTS" -eq 0 ] && [ -n "$RECORDED" ] && [ "$RECORDED" -gt 0 ]; then
        EVENTS="$RECORDED"
        SOURCE="perf record 自报样本数"
    fi

    COW_BYTES=$((EVENTS * PAGE_SIZE))
    COW_MIB=$(awk "BEGIN{printf \"%.2f\", $COW_BYTES/1048576}")
    # 用 stat 而非 "wc -c < $DATA": 后者要由 shell 打开文件, 当 perf.data 仍是
    # root:0600 时会直接报 Permission denied; stat 只需目录搜索权限。
    DATA_SIZE=$(stat -c %s "$DATA" 2>/dev/null || echo 0)

    echo ""
    echo "--------------------- COW 内存汇总 ---------------------"
    echo "  采集事件源     : $PROBE_NAME"
    echo "  页大小         : $PAGE_SIZE 字节"
    [ -n "$PAGES" ] && echo "  子进程写入页数 : $PAGES"
    echo "  perf.data 大小 : $DATA_SIZE 字节"
    [ -n "$RECORDED" ] && echo "  perf 自报样本数 : $RECORDED"
    echo "  采集到事件数   : $EVENTS (来源: $SOURCE)"
    echo "  估算 COW 内存  : $COW_BYTES 字节 ($COW_MIB MiB) = 事件数 x 页大小"
    echo "-------------------------------------------------------"

    if [ "$EVENTS" -eq 0 ]; then
        echo ""
        echo "[方式一][告警] 采集到 0 个 $PROBE_NAME 事件。可能原因:"
        echo "  1) perf record 实际开始采样晚于子进程写入 ——"
        echo "     用 --settle 加大就绪后的静置时间(如 --settle 1000)重试;"
        echo "  2) do_wp_page 被内联或探针挂在了不会命中的位置 ——"
        echo "     用 sudo perf probe -l 确认探针位置;"
        echo "  3) 子进程写入的页并未触发写保护缺页(例如内存并未真正预先驻留)。"
        echo "  记录阶段完整日志: $OUT/record.log"
        return 1
    fi
    return 0
}

[ $# -ge 1 ] || { usage; exit 2; }
SUB="$1"; shift
case "$SUB" in
    record)    cmd_record "$@";;
    report)    cmd_report "$@";;
    add-probe) add_probe;;
    del-probe) del_probe;;
    -h|--help) usage;;
    *) usage; exit 2;;
esac
