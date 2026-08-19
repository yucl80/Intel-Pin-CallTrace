/* 一个用于演示 calltrace Pin 工具的小程序。
 * 包含递归（fib）和 libc 调用（printf/puts/malloc/free），
 * 便于观察多级调用链。 */
#include <stdio.h>
#include <stdlib.h>

static int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

static void do_work(int n) {
    int r = fib(n);
    char *p = (char *)malloc((size_t)(r + 1));
    if (p) {
        p[0] = (char)('0' + r);
        p[1] = '\0';
        puts(p);
        free(p);
    } else {
        puts("malloc failed");
    }
}

int main(void) {
    for (int i = 1; i <= 5; i++)
        do_work(i);
    return 0;
}
