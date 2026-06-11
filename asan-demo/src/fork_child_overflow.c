/*
 * 父进程 fork 子进程，子进程发生堆缓冲区越界 (heap-buffer-overflow)
 *
 * 演示要点：
 *   1) ASan 在 fork 出来的子进程中同样有效，会捕获子进程里的内存错误。
 *   2) 子进程命中错误后（因 abort_on_error=0）以非 0 退出码结束，
 *      父进程通过 waitpid 能读取到子进程的异常退出状态。
 *   3) ASan 报告默认输出到 stderr；多进程场景下若想分文件，可用
 *      ASAN_OPTIONS=log_path=asan.log（会生成 asan.log.<pid>，按 pid 区分）。
 *
 * 注意：默认配置来自链接进来的 common/asan_default_options.c，
 *       无需设置 ASAN_OPTIONS 环境变量。
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

static void child_work(void) {
    printf("[child  pid=%d] 分配 4 个 int，准备越界写入下标 8\n", (int)getpid());
    fflush(stdout); /* fork 后及时刷新，避免缓冲区内容重复输出 */

    int *arr = (int *)malloc(sizeof(int) * 4);
    if (!arr) {
        _exit(2);
    }

    volatile int idx = 8; /* volatile 防止编译器优化掉越界访问 */
    arr[idx] = 123;       /* 越界写：合法下标 0~3，这里写下标 8 -> ASan 报错 */

    printf("[child  pid=%d] 不应到达这里: arr[8]=%d\n", (int)getpid(), arr[idx]);
    free(arr);
    _exit(0);
}

int main(void) {
    printf("[parent pid=%d] 即将 fork 子进程\n", (int)getpid());
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* 子进程 */
        child_work();
        /* child_work 内部已 _exit，不会返回 */
    }

    /* 父进程：等待子进程结束并解析其退出状态 */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        printf("[parent pid=%d] 子进程(%d) 以退出码 %d 结束%s\n",
               (int)getpid(), (int)pid, code,
               code != 0 ? "（非 0，说明子进程被 ASan 终止）" : "");
    } else if (WIFSIGNALED(status)) {
        printf("[parent pid=%d] 子进程(%d) 被信号 %d 终止\n",
               (int)getpid(), (int)pid, WTERMSIG(status));
    }

    printf("[parent pid=%d] 父进程自身没有内存错误，正常退出\n", (int)getpid());
    return 0;
}
