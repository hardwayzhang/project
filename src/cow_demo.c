/*
 * cow_demo.c - Linux 写时复制(Copy-On-Write) do_wp_page 事件观测 Demo
 * ============================================================================
 *
 * 场景:
 *   父进程申请一段私有匿名内存并写入(使其驻留物理页)。fork 之后, 父子共享
 *   这些物理页且被标记为只读。子进程首次写入任意一页时, 内核触发写保护缺页,
 *   进入 do_wp_page(), 为子进程复制出一份私有物理页 —— 这就是一次 COW。
 *
 * 本程序提供两种方式来"观测"子进程的 COW 事件:
 *
 *   方式一 (--method 1):
 *     在 fork 出子进程后, 启动外部脚本 scripts/perf_cow_watch.sh, 该脚本用
 *       perf probe --add do_wp_page
 *       perf record -e probe:do_wp_page -g -p <子进程pid>
 *     追踪子进程的 COW 事件调用栈; 在子进程真正退出之前, 本程序停止该脚本,
 *     再由脚本用 perf script / perf report 输出堆栈, 并按 事件数 x 页大小
 *     估算 COW 内存大小。
 *
 *   方式二 (--method 2):
 *     由本程序自身用 perf_event_open() 订阅 do_wp_page 事件(原理与
 *     perf record -e probe:do_wp_page 相同): 先经由 tracefs 注册 kprobe
 *     得到 tracepoint id, 再 perf_event_open(PERF_TYPE_TRACEPOINT) 挂到
 *     子进程上, 通过 mmap 环形缓冲区读取每次事件的调用栈(内核态 + 子进程
 *     用户态), 统计事件数并汇总 COW 内存大小, 输出为文本。
 *
 *     若运行环境的内核未开放 kprobe/tracefs(例如受限容器), 方式二支持一个
 *     可移植回退后端 (--backend swfault): 改用 perf_event_open 订阅
 *     子进程的"软件缺页(page-fault)"事件。它复用完全相同的 perf 环形缓冲 +
 *     调用栈采集通路, 依然能采到子进程用户态调用栈并按缺页数汇总 COW 内存,
 *     只是事件源不是 do_wp_page 内核探针。该模式会明确打印告警。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/perf_event.h>

#include "symbolize.h"

/* ------------------------------ 配置与常量 ------------------------------ */

#define KPROBE_GROUP "cowdemo"
#define KPROBE_EVENT "do_wp_page"
#define KPROBE_SYMBOL "do_wp_page"

enum backend {
    BK_AUTO = 0,   /* 先试 kprobe, 不行则回退 swfault */
    BK_KPROBE,     /* 仅使用 do_wp_page kprobe tracepoint */
    BK_SWFAULT,    /* 软件缺页事件(可移植回退) */
};

struct config {
    int method;            /* 1 或 2 */
    size_t size_mib;       /* 申请内存大小(MiB) */
    enum backend backend;  /* 方式二后端 */
    int max_print;         /* 打印多少条样本调用栈 */
    int max_frames;        /* 每条调用栈最多打印多少帧 */
    const char *script;    /* 方式一脚本路径 */
    int warmup_ms;         /* 方式一: 启动脚本后等待多久再放行子进程 */
    int verbose;
};

static long page_size;

/* ------------------------------ 子/父同步管道 ------------------------------ */

struct sync_pipes {
    int go[2];    /* 父 -> 子: 允许开始写入 */
    int wrote[2]; /* 子 -> 父: 已完成写入 */
    int quit[2];  /* 父 -> 子: 允许退出 */
};

static void notify(int fd) { char c = 'x'; if (write(fd, &c, 1) != 1) { /* ignore */ } }
static void waitfor(int fd) { char c; if (read(fd, &c, 1) != 1) { /* ignore */ } }

/* ------------------------------ COW 工作负载 ------------------------------ */

/*
 * 逐页写入以触发写时复制。刻意做成 noinline 且分层调用,
 * 让采集到的子进程用户态调用栈更有辨识度:
 *   main -> child_main -> cow_touch_pages
 */
static size_t __attribute__((noinline))
cow_touch_pages(volatile char *buf, size_t bytes)
{
    size_t touched = 0;
    for (size_t off = 0; off < bytes; off += (size_t)page_size) {
        buf[off] ^= 0xA5; /* 写操作 -> 触发 do_wp_page */
        touched++;
    }
    return touched;
}

static void __attribute__((noinline))
child_main(volatile char *buf, size_t bytes, struct sync_pipes *sp)
{
    waitfor(sp->go[0]);                 /* 等待观测器就绪 */
    size_t n = cow_touch_pages(buf, bytes);
    /* 用 write 直接输出, 避免 stdio 缓冲干扰 */
    dprintf(STDERR_FILENO, "[child ] 完成写入 %zu 页 (pid=%d)\n", n, (int)getpid());
    notify(sp->wrote[1]);               /* 告知父进程写入完成 */
    waitfor(sp->quit[0]);               /* 等待父进程允许退出 */
    _exit(0);
}

/* ------------------------------ perf 辅助 ------------------------------ */

static long perf_event_open(struct perf_event_attr *attr, pid_t pid,
                            int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/* ------------------------------ tracefs / kprobe ------------------------------ */

/* 查找可用的 tracefs 路径(其下应有 kprobe_events)。返回静态缓冲或 NULL。*/
static const char *find_tracefs(void)
{
    static char path[256];
    const char *cands[] = {
        "/sys/kernel/tracing",
        "/sys/kernel/debug/tracing",
        NULL
    };
    /* 先看 /proc/mounts 里已挂载的 tracefs */
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char dev[128], mnt[256], type[64];
        while (fscanf(fp, "%127s %255s %63s %*[^\n]", dev, mnt, type) == 3) {
            if (strcmp(type, "tracefs") == 0) {
                snprintf(path, sizeof(path), "%s", mnt);
                fclose(fp);
                return path;
            }
        }
        fclose(fp);
    }
    for (int i = 0; cands[i]; i++) {
        char kp[300];
        snprintf(kp, sizeof(kp), "%s/kprobe_events", cands[i]);
        if (access(kp, W_OK) == 0) {
            snprintf(path, sizeof(path), "%s", cands[i]);
            return path;
        }
    }
    return NULL;
}

/* 注册 do_wp_page kprobe, 返回其 tracepoint id; 失败返回 -1。*/
static int kprobe_register(const char *tracefs, int verbose)
{
    char kp[300];
    snprintf(kp, sizeof(kp), "%s/kprobe_events", tracefs);

    int fd = open(kp, O_WRONLY | O_APPEND);
    if (fd < 0) {
        if (verbose)
            fprintf(stderr, "[方式二] 打开 %s 失败: %s\n", kp, strerror(errno));
        return -1;
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "p:%s/%s %s\n", KPROBE_GROUP, KPROBE_EVENT, KPROBE_SYMBOL);
    ssize_t w = write(fd, cmd, strlen(cmd));
    close(fd);
    if (w < 0) {
        if (verbose)
            fprintf(stderr, "[方式二] 注册 kprobe 失败: %s\n", strerror(errno));
        return -1;
    }

    char idp[300];
    snprintf(idp, sizeof(idp), "%s/events/%s/%s/id", tracefs, KPROBE_GROUP, KPROBE_EVENT);
    FILE *f = fopen(idp, "r");
    if (!f) return -1;
    int id = -1;
    if (fscanf(f, "%d", &id) != 1) id = -1;
    fclose(f);
    return id;
}

static void kprobe_unregister(const char *tracefs)
{
    char kp[300];
    snprintf(kp, sizeof(kp), "%s/kprobe_events", tracefs);
    int fd = open(kp, O_WRONLY | O_APPEND);
    if (fd < 0) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "-:%s/%s\n", KPROBE_GROUP, KPROBE_EVENT);
    if (write(fd, cmd, strlen(cmd)) < 0) { /* ignore */ }
    close(fd);
}

/* ------------------------------ 环形缓冲采集 ------------------------------ */

#define RING_DATA_PAGES 64  /* 数据区页数(2 的幂) */

struct sample {
    uint64_t nr;
    uint64_t ips[128];
};

struct collector {
    int fd;
    struct perf_event_mmap_page *meta;
    uint8_t *data;
    size_t data_size;
    void *mmap_base;
    size_t mmap_size;

    unsigned long long n_samples;   /* 采集到的事件(样本)数 */
    unsigned long long n_lost;      /* 内核报告丢失的样本数 */
    struct sample *keep;            /* 保留若干条样本用于打印 */
    int keep_cap;
    int keep_n;
};

static int collector_init(struct collector *c, int fd, int keep_cap)
{
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->mmap_size = (1 + RING_DATA_PAGES) * (size_t)page_size;
    c->mmap_base = mmap(NULL, c->mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c->mmap_base == MAP_FAILED)
        return -1;
    c->meta = c->mmap_base;
    c->data = (uint8_t *)c->mmap_base + page_size;
    c->data_size = RING_DATA_PAGES * (size_t)page_size;
    c->keep_cap = keep_cap;
    c->keep = calloc(keep_cap, sizeof(struct sample));
    return 0;
}

/* 从环形缓冲区读取一段(可能跨界回绕), 复制到线性缓冲 */
static void ring_copy(struct collector *c, uint64_t off, void *dst, size_t len)
{
    uint8_t *d = dst;
    size_t start = off % c->data_size;
    size_t first = c->data_size - start;
    if (first >= len) {
        memcpy(d, c->data + start, len);
    } else {
        memcpy(d, c->data + start, first);
        memcpy(d + first, c->data, len - first);
    }
}

/*
 * 消费环形缓冲区中的所有记录。
 * 样本布局(sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN):
 *   struct { struct perf_event_header header;
 *            u32 pid, tid;              // PERF_SAMPLE_TID
 *            u64 nr; u64 ips[nr]; }     // PERF_SAMPLE_CALLCHAIN
 */
static void collector_drain(struct collector *c)
{
    uint64_t head = __atomic_load_n(&c->meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = c->meta->data_tail;

    while (tail < head) {
        struct perf_event_header hdr;
        ring_copy(c, tail, &hdr, sizeof(hdr));
        if (hdr.size == 0) break; /* 防御 */

        if (hdr.type == PERF_RECORD_SAMPLE) {
            uint8_t buf[sizeof(struct perf_event_header) + 8 + 8 + 128 * 8];
            size_t copy = hdr.size <= sizeof(buf) ? hdr.size : sizeof(buf);
            ring_copy(c, tail, buf, copy);

            uint8_t *p = buf + sizeof(struct perf_event_header);
            /* PERF_SAMPLE_TID */
            p += sizeof(uint32_t) * 2;
            /* PERF_SAMPLE_CALLCHAIN */
            uint64_t nr;
            memcpy(&nr, p, sizeof(nr));
            p += sizeof(uint64_t);

            c->n_samples++;
            if (c->keep_n < c->keep_cap) {
                struct sample *s = &c->keep[c->keep_n++];
                s->nr = nr > 128 ? 128 : nr;
                memcpy(s->ips, p, s->nr * sizeof(uint64_t));
            }
        } else if (hdr.type == PERF_RECORD_LOST) {
            uint8_t buf[sizeof(struct perf_event_header) + 16];
            ring_copy(c, tail, buf, sizeof(buf));
            uint64_t lost;
            memcpy(&lost, buf + sizeof(struct perf_event_header) + 8, sizeof(lost));
            c->n_lost += lost;
        }
        tail += hdr.size;
    }
    __atomic_store_n(&c->meta->data_tail, head, __ATOMIC_RELEASE);
}

static void collector_free(struct collector *c)
{
    if (c->mmap_base && c->mmap_base != MAP_FAILED)
        munmap(c->mmap_base, c->mmap_size);
    free(c->keep);
}

/* perf_event 上下文标记(用于区分内核/用户帧) */
#define CTX_HV      ((uint64_t)-32)   /* PERF_CONTEXT_HV */
#define CTX_KERNEL  ((uint64_t)-128)  /* PERF_CONTEXT_KERNEL */
#define CTX_USER    ((uint64_t)-512)  /* PERF_CONTEXT_USER */
#define CTX_MAX     ((uint64_t)-4095) /* 约定: >= 该值视为上下文标记 */

static int is_ctx_marker(uint64_t ip) { return ip >= CTX_MAX; }

/* ------------------------------ 方式二实现 ------------------------------ */

/* 打开针对子进程 child 的 perf_event。返回 fd, 失败 -1; *used_backend 回填实际后端 */
static int method2_open(pid_t child, enum backend want, const char *tracefs,
                        int tp_id, enum backend *used, int verbose)
{
    struct perf_event_attr attr;

    /* 尝试 kprobe tracepoint */
    if ((want == BK_AUTO || want == BK_KPROBE) && tp_id >= 0) {
        memset(&attr, 0, sizeof(attr));
        attr.type = PERF_TYPE_TRACEPOINT;
        attr.size = sizeof(attr);
        attr.config = tp_id;
        attr.sample_period = 1;
        attr.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN;
        attr.disabled = 1;
        attr.exclude_callchain_kernel = 0;
        attr.exclude_callchain_user = 0;
        attr.wakeup_events = 1;

        int fd = perf_event_open(&attr, child, -1, -1, 0);
        if (fd >= 0) {
            *used = BK_KPROBE;
            return fd;
        }
        if (verbose)
            fprintf(stderr, "[方式二] kprobe tracepoint perf_event_open 失败: %s\n",
                    strerror(errno));
        if (want == BK_KPROBE)
            return -1;
    }
    (void)tracefs;

    /* 回退: 软件缺页事件 */
    if (want == BK_AUTO || want == BK_SWFAULT) {
        memset(&attr, 0, sizeof(attr));
        attr.type = PERF_TYPE_SOFTWARE;
        attr.size = sizeof(attr);
        attr.config = PERF_COUNT_SW_PAGE_FAULTS;
        attr.sample_period = 1;
        attr.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN;
        attr.disabled = 1;
        attr.exclude_callchain_kernel = 0;
        attr.exclude_callchain_user = 0;
        attr.wakeup_events = 1;

        int fd = perf_event_open(&attr, child, -1, -1, 0);
        if (fd >= 0) {
            *used = BK_SWFAULT;
            return fd;
        }
        if (verbose)
            fprintf(stderr, "[方式二] swfault perf_event_open 失败: %s\n",
                    strerror(errno));
    }
    return -1;
}

static void print_stack(struct sample *s, symtab_t *ks, int have_kaddr,
                        usym_ctx_t *us, int max_frames)
{
    int shown = 0;
    int in_kernel = 0; /* 首帧默认视为内核(perf 惯例), 直到遇到 CTX_USER */
    for (uint64_t i = 0; i < s->nr && shown < max_frames; i++) {
        uint64_t ip = s->ips[i];
        if (ip == CTX_KERNEL || ip == CTX_HV) { in_kernel = 1; continue; }
        if (ip == CTX_USER) { in_kernel = 0; continue; }
        if (is_ctx_marker(ip)) continue;

        unsigned long off = 0;
        if (in_kernel) {
            const char *nm = have_kaddr ? ksym_resolve(ks, ip, &off) : NULL;
            if (nm)
                printf("        #%-2d [k] %s+0x%lx\n", shown, nm, off);
            else
                printf("        #%-2d [k] 0x%llx\n", shown, (unsigned long long)ip);
        } else {
            const char *fn = NULL, *mod = NULL;
            usym_resolve(us, ip, &fn, &off, &mod);
            if (fn)
                printf("        #%-2d [u] %s+0x%lx (%s)\n", shown, fn, off,
                       mod ? mod : "?");
            else if (mod)
                printf("        #%-2d [u] %s+0x%lx\n", shown, mod, off);
            else
                printf("        #%-2d [u] 0x%llx\n", shown, (unsigned long long)ip);
        }
        shown++;
    }
}

static int run_method2(struct config *cfg, volatile char *buf, size_t bytes,
                       pid_t child, struct sync_pipes *sp)
{
    (void)buf;
    const char *tracefs = find_tracefs();
    int tp_id = -1;
    int kprobe_ok = 0;

    if (cfg->backend != BK_SWFAULT) {
        if (tracefs) {
            tp_id = kprobe_register(tracefs, cfg->verbose);
            if (tp_id >= 0) kprobe_ok = 1;
        }
        if (!kprobe_ok && cfg->backend == BK_KPROBE) {
            fprintf(stderr,
                    "[方式二] 无法注册 do_wp_page kprobe (tracefs=%s)。\n"
                    "         请以 root 运行, 并确认内核支持 kprobe/tracefs, "
                    "或改用 --backend swfault。\n",
                    tracefs ? tracefs : "未找到");
            /* 让子进程退出, 避免卡死 */
            notify(sp->go[1]); waitfor(sp->wrote[0]); notify(sp->quit[1]);
            return 1;
        }
    }

    enum backend used = BK_SWFAULT;
    int fd = method2_open(child, cfg->backend, tracefs, tp_id, &used, cfg->verbose);
    if (fd < 0) {
        fprintf(stderr, "[方式二] perf_event_open 失败, 无法采集。\n");
        if (kprobe_ok) kprobe_unregister(tracefs);
        notify(sp->go[1]); waitfor(sp->wrote[0]); notify(sp->quit[1]);
        return 1;
    }

    printf("========================================================\n");
    printf(" 方式二: perf_event_open 订阅 %s\n",
           used == BK_KPROBE ? "do_wp_page (kprobe tracepoint)"
                             : "软件缺页事件 (PERF_COUNT_SW_PAGE_FAULTS)");
    if (used == BK_SWFAULT) {
        printf(" [告警] 当前环境未启用 do_wp_page kprobe, 已回退到软件缺页后端。\n");
        printf("        采集通路(perf 环形缓冲 + 调用栈)与 kprobe 完全一致,\n");
        printf("        子进程逐页写入 COW 页, 故缺页数≈COW 事件数。\n");
    }
    printf("========================================================\n");

    struct collector col;
    if (collector_init(&col, fd, cfg->max_print) < 0) {
        fprintf(stderr, "[方式二] mmap 环形缓冲失败: %s\n", strerror(errno));
        close(fd);
        if (kprobe_ok) kprobe_unregister(tracefs);
        return 1;
    }

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    notify(sp->go[1]);            /* 放行子进程开始 COW 写入 */

    /*
     * 边写边收: 在子进程写入期间持续 poll + 排空环形缓冲, 避免缓冲溢出丢样本。
     * 同时监听 wrote 管道: 子进程写完后再做最后几次排空。
     */
    struct pollfd pfds[2];
    pfds[0].fd = fd;            pfds[0].events = POLLIN;
    pfds[1].fd = sp->wrote[0];  pfds[1].events = POLLIN;
    usym_ctx_t *us = NULL;
    int child_wrote = 0;
    while (1) {
        int pr = poll(pfds, 2, 100);
        if (pr < 0 && errno == EINTR) continue;
        collector_drain(&col);
        if (!child_wrote && (pfds[1].revents & POLLIN)) {
            waitfor(sp->wrote[0]);          /* 消费通知 */
            child_wrote = 1;
            /* 子进程仍存活(等待 quit), 抓取其地址空间用于用户态符号化 */
            us = usym_capture(child);
        }
        if (child_wrote) {
            /* 再多排空几轮, 确保收尾样本落袋 */
            collector_drain(&col);
            uint64_t head = __atomic_load_n(&col.meta->data_head, __ATOMIC_ACQUIRE);
            if (head == col.meta->data_tail)
                break;
        }
    }

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    collector_drain(&col);

    /* 读取硬件/软件计数器累计值 */
    long long counter = 0;
    if (read(fd, &counter, sizeof(counter)) != sizeof(counter))
        counter = -1;

    /* 内核符号表 */
    symtab_t ks; int have_kaddr = 0;
    ksyms_load(&ks, &have_kaddr);

    /* -------- 打印样本调用栈 -------- */
    int nprint = col.keep_n < cfg->max_print ? col.keep_n : cfg->max_print;
    for (int i = 0; i < nprint; i++) {
        printf("\n  [样本 #%d] 调用栈 (自顶向下):\n", i + 1);
        print_stack(&col.keep[i], &ks, have_kaddr, us, cfg->max_frames);
    }

    /* -------- COW 汇总 -------- */
    unsigned long long events = col.n_samples;
    unsigned long long cow_bytes = events * (unsigned long long)page_size;
    printf("\n--------------------- COW 内存汇总 ---------------------\n");
    printf("  目标子进程 PID     : %d\n", (int)child);
    printf("  采集事件源         : %s\n",
           used == BK_KPROBE ? "probe:do_wp_page" : "sw:page-faults(回退)");
    printf("  申请/写入内存      : %zu MiB (%zu 页, 页大小 %ld 字节)\n",
           cfg->size_mib, bytes / page_size, page_size);
    printf("  采集到事件数       : %llu\n", events);
    printf("  计数器累计值       : %lld\n", counter);
    if (col.n_lost)
        printf("  丢失样本(缓冲溢出) : %llu\n", col.n_lost);
    printf("  估算 COW 内存      : %llu 字节 (%.2f MiB) = 事件数 x 页大小\n",
           cow_bytes, cow_bytes / (1024.0 * 1024.0));
    if (used == BK_SWFAULT && events > (unsigned long long)(bytes / page_size))
        printf("  (注: 事件数略多于写入页数, 因回退后端统计的是全部缺页,\n"
               "       含子进程首次执行 libc 等代码路径产生的零星缺页)\n");
    printf("--------------------------------------------------------\n");

    symtab_free(&ks);
    usym_free(us);
    collector_free(&col);
    close(fd);
    if (kprobe_ok) kprobe_unregister(tracefs);

    /* 放行子进程退出 */
    notify(sp->quit[1]);
    return 0;
}

/* ------------------------------ 方式一实现 ------------------------------ */

static int run_method1(struct config *cfg, volatile char *buf, size_t bytes,
                       pid_t child, struct sync_pipes *sp)
{
    (void)buf;
    char outdir[] = "/tmp/cow_demo_perfXXXXXX";
    if (!mkdtemp(outdir)) {
        perror("mkdtemp");
        notify(sp->go[1]); waitfor(sp->wrote[0]); notify(sp->quit[1]);
        return 1;
    }

    char pidstr[32], sizestr[32];
    snprintf(pidstr, sizeof(pidstr), "%d", (int)child);
    snprintf(sizestr, sizeof(sizestr), "%zu", bytes / (size_t)page_size);

    printf("========================================================\n");
    printf(" 方式一: perf record -e probe:do_wp_page -g -p %d\n", (int)child);
    printf(" 记录脚本: %s\n", cfg->script);
    printf(" 输出目录: %s\n", outdir);
    printf("========================================================\n");

    /* 启动记录: 子进程组内 exec 脚本的 record 模式(前台运行 perf record) */
    pid_t rec = fork();
    if (rec == 0) {
        setpgid(0, 0); /* 独立进程组, 便于定向发送 SIGINT */
        execl("/bin/sh", "sh", cfg->script, "record",
              "--pid", pidstr, "--out", outdir, (char *)NULL);
        _exit(127);
    }
    if (rec < 0) {
        perror("fork(record)");
        notify(sp->go[1]); waitfor(sp->wrote[0]); notify(sp->quit[1]);
        return 1;
    }
    setpgid(rec, rec); /* 与子分支竞争消除 */

    /* 等待 perf 完成 attach 的热身时间 */
    usleep((useconds_t)cfg->warmup_ms * 1000);

    /* 放行子进程执行 COW 写入 */
    notify(sp->go[1]);
    waitfor(sp->wrote[0]); /* 子进程写完(仍存活) */

    /* 在子进程退出前停止记录: 向记录进程组发送 SIGINT, perf 收尾写出 perf.data */
    kill(-rec, SIGINT);
    int rst = 0;
    waitpid(rec, &rst, 0);

    /* 允许子进程退出(记录已停止) */
    notify(sp->quit[1]);

    /* 分析: 运行脚本 report 模式, 输出堆栈与 COW 汇总 */
    pid_t rep = fork();
    if (rep == 0) {
        execl("/bin/sh", "sh", cfg->script, "report",
              "--out", outdir, "--pages", sizestr, (char *)NULL);
        _exit(127);
    }
    if (rep > 0) {
        int st = 0;
        waitpid(rep, &st, 0);
    }
    return 0;
}

/* ------------------------------ 主程序 ------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "  --method N       观测方式: 1=perf record 脚本, 2=perf_event_open (默认 2)\n"
        "  --size M         申请内存大小, 单位 MiB (默认 16)\n"
        "  --backend B      方式二后端: auto|kprobe|swfault (默认 auto)\n"
        "  --script PATH    方式一脚本路径 (默认 scripts/perf_cow_watch.sh)\n"
        "  --warmup MS      方式一启动记录后的热身毫秒数 (默认 500)\n"
        "  --max-print N    最多打印多少条样本调用栈 (默认 3)\n"
        "  --max-frames N   每条调用栈最多打印多少帧 (默认 24)\n"
        "  --verbose        打印更多诊断信息\n"
        "  --help           显示本帮助\n",
        prog);
}

int main(int argc, char **argv)
{
    page_size = sysconf(_SC_PAGESIZE);

    struct config cfg = {
        .method = 2,
        .size_mib = 16,
        .backend = BK_AUTO,
        .max_print = 3,
        .max_frames = 24,
        .script = "scripts/perf_cow_watch.sh",
        .warmup_ms = 500,
        .verbose = 0,
    };

    static struct option opts[] = {
        {"method", required_argument, 0, 'm'},
        {"size", required_argument, 0, 's'},
        {"backend", required_argument, 0, 'b'},
        {"script", required_argument, 0, 'c'},
        {"warmup", required_argument, 0, 'w'},
        {"max-print", required_argument, 0, 'p'},
        {"max-frames", required_argument, 0, 'f'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "m:s:b:c:w:p:f:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'm': cfg.method = atoi(optarg); break;
        case 's': cfg.size_mib = strtoul(optarg, NULL, 10); break;
        case 'b':
            if (!strcmp(optarg, "auto")) cfg.backend = BK_AUTO;
            else if (!strcmp(optarg, "kprobe")) cfg.backend = BK_KPROBE;
            else if (!strcmp(optarg, "swfault")) cfg.backend = BK_SWFAULT;
            else { fprintf(stderr, "未知后端: %s\n", optarg); return 2; }
            break;
        case 'c': cfg.script = optarg; break;
        case 'w': cfg.warmup_ms = atoi(optarg); break;
        case 'p': cfg.max_print = atoi(optarg); break;
        case 'f': cfg.max_frames = atoi(optarg); break;
        case 'v': cfg.verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }

    if (cfg.method != 1 && cfg.method != 2) {
        fprintf(stderr, "--method 只能为 1 或 2\n");
        return 2;
    }
    if (cfg.size_mib == 0) cfg.size_mib = 1;

    size_t bytes = cfg.size_mib * (size_t)1024 * 1024;
    bytes = (bytes + page_size - 1) & ~((size_t)page_size - 1);

    /* 申请私有匿名内存并写入使其驻留(在 fork 之前) */
    volatile char *buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* 禁用透明大页, 保证按 4K 基页计数 COW */
#ifdef MADV_NOHUGEPAGE
    madvise((void *)buf, bytes, MADV_NOHUGEPAGE);
#endif
    memset((void *)buf, 0x5A, bytes); /* 预先写入 -> 物理页驻留 */

    printf("[父进程] pid=%d, 申请并写入 %zu MiB (%zu 页)\n",
           (int)getpid(), cfg.size_mib, bytes / (size_t)page_size);

    struct sync_pipes sp;
    if (pipe(sp.go) || pipe(sp.wrote) || pipe(sp.quit)) {
        perror("pipe");
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        child_main(buf, bytes, &sp);
        _exit(0); /* 不会到达 */
    }

    int rc;
    if (cfg.method == 2)
        rc = run_method2(&cfg, buf, bytes, child, &sp);
    else
        rc = run_method1(&cfg, buf, bytes, child, &sp);

    int st = 0;
    waitpid(child, &st, 0);
    printf("[父进程] 子进程已退出。\n");
    return rc;
}
