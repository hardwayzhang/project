/*
 * 重复释放 (double-free)
 *
 * 同一块内存被 free 两次，AddressSanitizer 会报告
 * "attempting double-free"。
 */
#include <stdlib.h>
#include <stdio.h>

int main(void) {
    int *p = (int *)malloc(sizeof(int) * 16);
    if (!p) {
        return 1;
    }

    p[0] = 7;
    printf("p[0] = %d\n", p[0]);

    free(p);
    /* 第二次释放同一指针 -> double-free */
    free(p);

    return 0;
}
