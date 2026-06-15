/*
 * 父进程与子进程把各自的 ASan 报告写到【不同的文件】。
 *
 * 思路：用公开 API __sanitizer_set_report_path() 在运行时设置报告输出路径，
 * ASan 会写到 "<路径>.<pid>"。父进程和子进程各自调用一次、传入不同前缀，
 * 于是两边的报告分别落到不同文件里，互不交错。
 *
 *   父进程 -> <logdir>/asan_parent.<父pid>
 *   子进程 -> <logdir>/asan_child.<子pid>
 *
 * 为了让进程在报告完一次错误后仍能继续运行（从而父进程能列出最终生成的文件），
 * 本示例：
 *   - 编译时加 -fsanitize-recover=address（使 ASan 错误可恢复）；
 *   - 运行时设 halt_on_error=0（报告后不中止）；
 *   - 故意使用“越界读”，被记录后继续也不会破坏堆。
 *
 * 提示：即使不调用 __sanitizer_set_report_path()，只要设置
 *   ASAN_OPTIONS=log_path=asan.log
 * ASan 也会按 "asan.log.<pid>" 给每个进程单独成文件；本示例展示的是
 * 用代码精确控制“父/子各写到带不同名字的文件”。
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>

#include <sanitizer/common_interface_defs.h>

/* 本示例自带默认选项：报告后不中止、可恢复继续。 */
const char *__asan_default_options(void) {
    return "halt_on_error=0"   /* 配合 -fsanitize-recover=address：报告后继续 */
           ":detect_leaks=0"
           ":abort_on_error=0";
}

#define LOG_DIR "asan_logs"

/* 触发一次堆越界【读】，被 ASan 记录到当前 report path 指向的文件。
 * 用读而非写：可恢复继续时不破坏堆内存。 */
static void trigger_overflow(const char *who) {
    int *arr = (int *)malloc(sizeof(int) * 4);
    if (!arr) {
        _exit(2);
    }
    for (int i = 0; i < 4; i++) {
        arr[i] = i;
    }
    volatile int idx = 8; /* 合法下标 0~3，这里读下标 8 -> 越界读 */
    int v = arr[idx];
    printf("[%s pid=%d] 越界读 arr[8]=%d，报告已写入: %s\n",
           who, (int)getpid(), v, __sanitizer_get_report_path());
    fflush(stdout);
    free(arr);
}

/* 父进程在最后列出日志目录里生成的报告文件。 */
static void list_log_files(void) {
    printf("\n[parent] %s/ 目录下生成的 ASan 报告文件：\n", LOG_DIR);
    DIR *d = opendir(LOG_DIR);
    if (!d) {
        perror("opendir");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "asan_", 5) == 0) {
            printf("    - %s/%s\n", LOG_DIR, e->d_name);
        }
    }
    closedir(d);
}

int main(void) {
    if (mkdir(LOG_DIR, 0755) != 0 && errno != EEXIST) {
        perror("mkdir");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* 子进程：报告写到 asan_child.<子pid> */
        __sanitizer_set_report_path(LOG_DIR "/asan_child");
        trigger_overflow("child");
        _exit(0);
    }

    /* 父进程：报告写到 asan_parent.<父pid> */
    __sanitizer_set_report_path(LOG_DIR "/asan_parent");
    trigger_overflow("parent");

    int status = 0;
    waitpid(pid, &status, 0);

    list_log_files();
    printf("\n[parent] 可用以下命令查看具体报告内容：\n");
    printf("    cat %s/asan_parent.<父pid>\n", LOG_DIR);
    printf("    cat %s/asan_child.<子pid>\n", LOG_DIR);
    return 0;
}
