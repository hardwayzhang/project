/*
 * 父进程以 start_deactivated 启动（ASan 先不激活），fork 子进程后
 * 在子进程里【手动激活】ASan，再触发堆越界，被 ASan 捕获。
 *
 * 演示流程：
 *   1) 通过 __asan_default_options() 设置 start_deactivated=1。
 *   2) 父进程启动后显式调用 asan_deactivate() 确保处于“未激活”状态
 *      （原因见下方说明）。此时做越界访问不会被检查。
 *   3) fork 出子进程，子进程继承“未激活”状态：
 *        - 先演示一次越界：未激活 -> 不被拦截；
 *        - 调用 asan_activate() 手动激活；
 *        - 再做一次越界：已激活 -> 被 ASan 捕获并报告。
 *   4) 父进程 waitpid 读取子进程异常退出状态，自身正常退出。
 *
 * 关于 start_deactivated 的一个关键点：
 *   start_deactivated=1 主要面向“主程序未插桩、运行时随后被加载”的场景
 *   （如 Android）。当主程序本身就用 -fsanitize=address 插桩时，运行时在
 *   启动阶段会因检测到已插桩模块而【自动激活】，于是 start_deactivated 看起来
 *   “没生效”。因此这里在 main 入口再显式 asan_deactivate() 一次，
 *   以真正进入未激活状态来演示。
 *
 * 编译要求：必须静态链接 ASan 运行时（gcc 加 -static-libasan / clang 默认静态），
 * 否则 asan_activate/asan_deactivate 依赖的内部符号无法解析。详见 common/asan_activation.h。
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "asan_activation.h"

/* 本示例自带默认选项：以未激活方式启动，并开启泄漏检测。 */
const char *__asan_default_options(void) {
    return "start_deactivated=1"  /* 运行时以“未激活”状态启动 */
           ":detect_leaks=1"
           ":halt_on_error=1"
           ":abort_on_error=0";   /* 用退出码结束，方便父进程读取子进程状态 */
}

/* 触发一次堆越界写入；是否被拦截取决于 ASan 当前是否激活。 */
static void do_overflow(const char *tag) {
    int *arr = (int *)malloc(sizeof(int) * 4);
    if (!arr) {
        _exit(2);
    }
    volatile int idx = 8; /* 合法下标 0~3，这里写下标 8 -> 越界 */
    arr[idx] = 0x1234;
    printf("    [%s] 写入 arr[8]=%d 成功（说明本次未被 ASan 拦截）\n",
           tag, arr[idx]);
    fflush(stdout);
    free(arr);
}

static void child_work(void) {
    printf("[child  pid=%d] 继承父进程的“未激活”状态\n", (int)getpid());
    fflush(stdout);

    printf("[child  pid=%d] 越界#1（激活前，预期不被拦截）：\n", (int)getpid());
    fflush(stdout);
    do_overflow("deactivated");

    printf("[child  pid=%d] 手动调用 asan_activate() 激活 ASan 检查\n",
           (int)getpid());
    fflush(stdout);
    asan_activate();

    printf("[child  pid=%d] 越界#2（激活后，预期被 ASan 捕获）：\n", (int)getpid());
    fflush(stdout);
    do_overflow("activated"); /* 这里会触发 ASan 报告并结束子进程 */

    /* 正常情况下不会到达这里 */
    _exit(0);
}

int main(void) {
    printf("[parent pid=%d] 以 start_deactivated=1 启动\n", (int)getpid());

    /* 因主程序已插桩、运行时会自动激活，这里显式反激活以真正进入未激活状态。 */
    asan_deactivate();
    printf("[parent pid=%d] 已显式 asan_deactivate()，当前未激活\n", (int)getpid());
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        child_work();
        /* child_work 不会返回 */
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        printf("[parent pid=%d] 子进程(%d) 以退出码 %d 结束%s\n",
               (int)getpid(), (int)pid, code,
               code != 0 ? "（非 0，说明激活后被 ASan 终止）" : "");
    } else if (WIFSIGNALED(status)) {
        printf("[parent pid=%d] 子进程(%d) 被信号 %d 终止\n",
               (int)getpid(), (int)pid, WTERMSIG(status));
    }

    printf("[parent pid=%d] 父进程自身没有触发检查，正常退出\n", (int)getpid());
    return 0;
}
