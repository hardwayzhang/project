/*
 * 堆缓冲区溢出 (heap-buffer-overflow)
 *
 * 在堆上分配 10 个 int，却访问下标 10，越界写入。
 * 运行后 AddressSanitizer 会报告 heap-buffer-overflow。
 */
#include <stdlib.h>
#include <stdio.h>

int main(void) {
    int *arr = (int *)malloc(sizeof(int) * 10);
    if (!arr) {
        return 1;
    }

    for (int i = 0; i < 10; i++) {
        arr[i] = i;
    }

    /* volatile 防止编译器把越界访问优化掉 */
    volatile int idx = 10;

    /* 越界访问：合法下标是 0~9，这里写入下标 10 */
    arr[idx] = 42;

    printf("arr[10] = %d\n", arr[idx]);

    free(arr);
    return 0;
}
