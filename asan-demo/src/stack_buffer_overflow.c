/*
 * 栈缓冲区溢出 (stack-buffer-overflow)
 *
 * 局部数组只有 8 个元素，却访问下标 8，越界写到栈上。
 * 运行后 AddressSanitizer 会报告 stack-buffer-overflow。
 */
#include <stdio.h>

int main(void) {
    int buf[8];

    for (int i = 0; i < 8; i++) {
        buf[i] = i;
    }

    /* volatile 防止编译器把越界访问优化掉 */
    volatile int idx = 8;

    /* 越界访问：合法下标是 0~7，这里写入下标 8 */
    buf[idx] = 1234;

    printf("buf[8] = %d\n", buf[idx]);
    return 0;
}
