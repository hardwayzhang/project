#!/usr/bin/env bash
#
# 依次运行所有 ASan 示例，并打印每个程序的退出码与报告。
#
# 由于这些示例（除 correct_example 外）都会触发内存错误，
# AddressSanitizer 默认会在检测到错误后让进程以非 0 退出码结束，
# 因此本脚本不开启 set -e，而是逐个运行并继续。

set -u

CC="${CC:-gcc}"
BIN_DIR="bin"

# 开启内存泄漏检测（部分平台/编译器默认未开启）。
# halt_on_error=1 表示遇到第一个错误就停止该进程（默认行为）。
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}"

if [ ! -d "$BIN_DIR" ]; then
    echo "未找到 $BIN_DIR 目录，请先执行: make"
    exit 1
fi

run_one() {
    local name="$1"
    local bin="$BIN_DIR/$name"
    echo "=================================================================="
    echo ">>> 运行示例: $name"
    echo "------------------------------------------------------------------"
    if [ ! -x "$bin" ]; then
        echo "(跳过) 未找到可执行文件 $bin"
        return
    fi
    "$bin"
    local rc=$?
    echo "------------------------------------------------------------------"
    echo "<<< $name 退出码: $rc"
    echo
}

echo "使用编译器: $CC"
echo "ASAN_OPTIONS=$ASAN_OPTIONS"
echo

run_one correct_example
run_one heap_buffer_overflow
run_one heap_use_after_free
run_one stack_buffer_overflow
run_one global_buffer_overflow
run_one double_free
run_one memory_leak

echo "=================================================================="
echo "全部示例运行结束。"
echo "提示: 除 correct_example 外，其余示例都应触发 ASan 报告。"
