#!/bin/sh
# ============================================================================
# perf_cow_watch.sh - 方式一: 用 perf record 追踪子进程的 do_wp_page(COW) 事件
# ============================================================================
#
# 由 cow_demo (--method 1) 调用, 分两个子命令:
#
#   record --pid <PID> --out <DIR>
#       注册 do_wp_page 探针(perf probe --add), 然后前台运行
#         perf record -e probe:do_wp_page -g -p <PID> -o <DIR>/perf.data
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

log()  { printf '%s\n' "$*" >&2; }
die()  { log "[方式一][错误] $*"; exit 1; }

usage() {
    cat >&2 <<EOF
用法:
  $PROG record --pid <PID> --out <DIR>
  $PROG report --out <DIR> [--pages <N>]
  $PROG add-probe        # 仅注册 do_wp_page 探针
  $PROG del-probe        # 仅删除 do_wp_page 探针
EOF
}

need_perf() {
    command -v perf >/dev/null 2>&1 || die "未找到 perf, 请先安装 (linux-tools)。"
}

# 确保 do_wp_page 探针存在(幂等)
add_probe() {
    need_perf
    if perf probe -l 2>/dev/null | grep -q "do_wp_page"; then
        log "[方式一] 探针 $PROBE_NAME 已存在。"
        return 0
    fi
    if perf probe --add do_wp_page >/dev/null 2>&1; then
        log "[方式一] 已注册探针 $PROBE_NAME。"
        return 0
    fi
    die "注册探针失败, 请以 root 运行并确认内核支持 kprobe。"
}

del_probe() {
    command -v perf >/dev/null 2>&1 || return 0
    perf probe --del "do_wp_page" >/dev/null 2>&1 || \
        perf probe --del "$PROBE_NAME" >/dev/null 2>&1 || true
    log "[方式一] 已删除探针(若存在)。"
}

cmd_record() {
    PID=""
    OUT=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --pid) PID="$2"; shift 2;;
            --out) OUT="$2"; shift 2;;
            *) die "record: 未知参数 $1";;
        esac
    done
    [ -n "$PID" ] || die "record 需要 --pid"
    [ -n "$OUT" ] || die "record 需要 --out"
    mkdir -p "$OUT"

    need_perf
    add_probe

    log "[方式一] 开始记录: perf record -e $PROBE_NAME -g -p $PID -o $OUT/perf.data"
    # 前台运行; 收到 SIGINT 时 perf 会停止并写出 perf.data。
    # 用 exec 让 SIGINT 直达 perf。
    exec perf record -e "$PROBE_NAME" -g -p "$PID" -o "$OUT/perf.data"
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

    echo "----------------------- 调用栈 (perf script) -----------------------"
    # 输出前若干条样本的完整调用栈(内核态 + 子进程用户态)
    perf script -i "$DATA" 2>/dev/null > "$OUT/script.txt" || true
    if [ -s "$OUT/script.txt" ]; then
        # 打印前 3 个样本块(以空行分隔)
        awk 'BEGIN{n=0} /^$/{blank=1; print; next}
             {print; if(blank){blank=0}}
             /probe:do_wp_page/{ } ' "$OUT/script.txt" | head -n 60
    else
        log "[方式一] perf script 无输出(可能未采集到事件)。"
    fi

    echo ""
    echo "----------------------- 聚合报告 (perf report) ---------------------"
    perf report -i "$DATA" --stdio -g --percent-limit 1 2>/dev/null | head -n 40 || true

    # 统计事件数: perf script 中每个样本含一行事件名 probe:do_wp_page
    EVENTS=$(grep -c "probe:do_wp_page" "$OUT/script.txt" 2>/dev/null || echo 0)
    if [ "$EVENTS" -eq 0 ]; then
        # 退而求其次, 用 perf report 头部的 samples 数
        EVENTS=$(perf report -i "$DATA" --stdio 2>/dev/null | \
                 sed -n 's/.*of event .*Samples: \([0-9]*\).*/\1/p' | head -n1)
        [ -n "$EVENTS" ] || EVENTS=0
    fi

    COW_BYTES=$((EVENTS * PAGE_SIZE))
    COW_MIB=$(awk "BEGIN{printf \"%.2f\", $COW_BYTES/1048576}")

    echo ""
    echo "--------------------- COW 内存汇总 ---------------------"
    echo "  采集事件源     : $PROBE_NAME"
    echo "  页大小         : $PAGE_SIZE 字节"
    [ -n "$PAGES" ] && echo "  子进程写入页数 : $PAGES"
    echo "  采集到事件数   : $EVENTS"
    echo "  估算 COW 内存  : $COW_BYTES 字节 ($COW_MIB MiB) = 事件数 x 页大小"
    echo "-------------------------------------------------------"
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
