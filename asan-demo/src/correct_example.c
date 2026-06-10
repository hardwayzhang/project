/*
 * 正确示例 (no error)
 *
 * 没有任何内存错误，开启 ASan 编译运行后不会有任何报告，
 * 用于对照 "干净" 程序的运行结果。
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    size_t n = 10;
    int *arr = (int *)malloc(sizeof(int) * n);
    if (!arr) {
        return 1;
    }

    for (size_t i = 0; i < n; i++) {
        arr[i] = (int)(i * i);
    }

    long sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += arr[i];
    }
    printf("sum of squares 0..9 = %ld\n", sum);

    free(arr); /* 正确释放 */
    return 0;
}
