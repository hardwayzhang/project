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

# 注意：默认的 ASan 选项（如 detect_leaks=1）已通过
# common/asan_default_options.c 里的 __asan_default_options() 编进程序，
# 这里不再设置 ASAN_OPTIONS 环境变量。
# 如需临时覆盖，运行时仍可 `ASAN_OPTIONS=... ./run_demo.sh`。

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
echo "默认 ASan 选项来自 __asan_default_options() (见 common/asan_default_options.c)"
echo "ASAN_OPTIONS(环境变量)=${ASAN_OPTIONS:-<未设置, 使用程序内置默认>}"
echo

run_one correct_example
run_one heap_buffer_overflow
run_one heap_use_after_free
run_one stack_buffer_overflow
run_one global_buffer_overflow
run_one double_free
run_one memory_leak
run_one fork_child_overflow
run_one fork_log_split

echo "=================================================================="
echo "全部示例运行结束。"
echo "提示: 除 correct_example 外，其余示例都应触发 ASan 报告。"
