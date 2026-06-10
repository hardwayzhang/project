/*
 * 释放后使用 (heap-use-after-free)
 *
 * 内存被 free 之后继续读写，属于典型的悬垂指针错误。
 * 运行后 AddressSanitizer 会报告 heap-use-after-free。
 */
#include <stdlib.h>
#include <stdio.h>

int main(void) {
    int *p = (int *)malloc(sizeof(int) * 4);
    if (!p) {
        return 1;
    }

    p[0] = 100;
    free(p);

    /* 内存已经被释放，这里再次读取属于 use-after-free */
    printf("p[0] = %d\n", p[0]);

    return 0;
}
