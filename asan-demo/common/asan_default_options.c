/*
 * 用 __asan_default_options 把默认的 ASAN_OPTIONS 直接“编进”程序里。
 *
 * AddressSanitizer 在启动时会调用这个弱符号函数（weak symbol）来获取默认配置，
 * 因此无需再依赖外部环境变量 ASAN_OPTIONS。
 *
 * 优先级（后者覆盖前者）：
 *   编译内置默认  <  __asan_default_options()  <  运行时 ASAN_OPTIONS 环境变量
 * 也就是说：这里设置的是“默认值”，使用者仍可在运行时用环境变量临时覆盖。
 *
 * 把本文件链接进每个示例后，运行时就不必再写
 *   ASAN_OPTIONS=detect_leaks=1:... ./prog
 * 直接运行 ./prog 即可生效。
 */

/* 用 const char * 返回一个以冒号分隔的选项字符串。
 * 函数名是固定的，ASan 运行时按名字查找并调用。 */
const char *__asan_default_options(void) {
    return
        "detect_leaks=1"      /* 开启内存泄漏检测（LeakSanitizer） */
        ":halt_on_error=1"    /* 遇到第一个错误就停止该进程（默认行为） */
        ":abort_on_error=0";  /* 用退出码结束而非 abort 信号，方便父进程读取子进程状态 */
}
