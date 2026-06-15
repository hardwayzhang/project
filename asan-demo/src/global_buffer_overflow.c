/*
 * 全局缓冲区溢出 (global-buffer-overflow)
 *
 * 全局数组只有 5 个元素，却访问下标 5，越界读取全局区。
 * 运行后 AddressSanitizer 会报告 global-buffer-overflow。
 */
#include <stdio.h>

int g_arr[5] = {0, 1, 2, 3, 4};

int main(void) {
    volatile int idx = 5; /* volatile 防止编译器把越界访问优化掉 */

    /* 越界访问：合法下标是 0~4，这里读取下标 5 */
    printf("g_arr[5] = %d\n", g_arr[idx]);

    return 0;
}
