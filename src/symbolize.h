/*
 * symbolize.h - 用户态地址符号化辅助
 *
 * 用于把 perf 采集到的调用栈地址（指令指针）翻译成可读的
 * "函数名+偏移" 形式：
 *   - 用户帧：解析目标进程 /proc/<pid>/maps + 对应 ELF 文件的符号表
 */
#ifndef COW_SYMBOLIZE_H
#define COW_SYMBOLIZE_H

#include <stddef.h>
#include <sys/types.h>

/* 单个符号：地址、大小、名字 */
typedef struct {
    unsigned long addr;
    unsigned long size;
    char *name;
} sym_t;

/* 已排序的符号表 */
typedef struct {
    sym_t *syms;
    size_t n;
    char *strpool; /* 名字使用的字符串池，统一释放 */
} symtab_t;

/* ---------------- 用户符号 ---------------- */

/* /proc/<pid>/maps 中的一条映射 */
typedef struct {
    unsigned long start;
    unsigned long end;
    unsigned long pgoff;
    int exec;
    char *path;
} umap_t;

/* 目标进程的用户态地址空间快照 + 已解析 ELF 文件缓存 */
typedef struct usym_ctx usym_ctx_t;

/* 抓取 pid 的 /proc/<pid>/maps 快照。必须在目标进程仍存活时调用。*/
usym_ctx_t *usym_capture(pid_t pid);

/* 解析用户态地址。成功时写入 func（函数名或模块名）、*off（偏移）、
 * 以及 module（所属映射文件 basename）。返回 0。
 * func/module 指向内部缓冲，调用者不要释放。*/
int usym_resolve(usym_ctx_t *ctx, unsigned long ip,
                 const char **func, unsigned long *off, const char **module);

void usym_free(usym_ctx_t *ctx);
void symtab_free(symtab_t *tab);

#endif /* COW_SYMBOLIZE_H */
