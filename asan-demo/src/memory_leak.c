/*
 * 内存泄漏 (memory leak)
 *
 * 分配的内存在程序结束前没有被 free，
 * 由 LeakSanitizer (LSan，ASan 的一部分) 在进程退出时报告。
 *
 * 注意：若默认未开启泄漏检测，可通过环境变量
 *   ASAN_OPTIONS=detect_leaks=1
 * 显式开启。
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *make_buffer(size_t n) {
    char *buf = (char *)malloc(n);
    if (buf) {
        memset(buf, 'A', n);
    }
    return buf;
}

int main(void) {
    /* 分配后丢失了指针，从未释放 -> 内存泄漏 */
    char *leaked = make_buffer(128);
    printf("first byte = %c\n", leaked[0]);

    /* 故意不 free(leaked) */
    return 0;
}
