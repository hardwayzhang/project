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
 *   方式一 (--watch method1):
 *     在 fork 出子进程后, 启动外部脚本 scripts/perf_cow_watch.sh, 该脚本用
 *       perf probe --add do_wp_page
 *       perf record -e probe:do_wp_page -g -p <子进程pid>
 *     仅追踪子进程的用户态调用栈; 在子进程真正退出之前, 本程序停止该脚本,
 *     再由脚本用 perf script / perf report 输出堆栈, 并按 事件数 x 页大小
 *     估算 COW 内存大小。
 *
 *   方式二 (--watch method2):
 *     由本程序自身用 perf_event_open() 订阅 do_wp_page 事件(原理与
 *     perf record -e probe:do_wp_page 相同): 先经由 tracefs 注册 kprobe
 *     得到 tracepoint id, 再 perf_event_open(PERF_TYPE_TRACEPOINT) 挂到
 *     子进程上, 通过 mmap 环形缓冲区读取每次事件的子进程用户态调用栈,
 *     统计事件数并汇总 COW 内存大小, 输出为文本。
 *
 *   未指定 --watch 时, 两种方式会针对同一个子进程同时启动。
 *   方式二不提供 page-fault 回退: 任一步失败都会记录诊断并退出。
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

enum watch_mode {
    WATCH_BOTH = 0,
    WATCH_METHOD1,
    WATCH_METHOD2,
};

struct config {
    enum watch_mode watch; /* 默认同时启动方式一和方式二 */
    size_t size_mib;       /* 申请内存大小(MiB) */
    int max_print;         /* 打印多少条样本调用栈 */
    int max_frames;        /* 每条调用栈最多打印多少帧 */
    const char *script;    /* 方式一脚本路径 */
    int probe_timeout_ms;  /* 方式一: 同步注册 do_wp_page 探针的最长等待 */
    int ready_timeout_ms;  /* 方式一: 等待 perf record 真正开始采样的最长时间 */
    int settle_ms;         /* 方式一: 判定就绪后额外静置多久再放行子进程 */
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
    /* 只要求存在: 不可写的情况留给后续 open() 报出精确的 errno */
    for (int i = 0; cands[i]; i++) {
        char kp[300];
        snprintf(kp, sizeof(kp), "%s/kprobe_events", cands[i]);
        if (access(kp, F_OK) == 0) {
            snprintf(path, sizeof(path), "%s", cands[i]);
            return path;
        }
    }
    return NULL;
}

/*
 * 注册 do_wp_page kprobe, 返回其 tracepoint id; 失败返回 -1 并把失败原因
 * (含 errno 与可操作建议) 写入 reason, 供上层记录后退出。
 */
static int kprobe_register(const char *tracefs, char *reason, size_t rlen)
{
    char kp[300];
    snprintf(kp, sizeof(kp), "%s/kprobe_events", tracefs);

    int fd = open(kp, O_WRONLY | O_APPEND);
    if (fd < 0) {
        int e = errno;
        snprintf(reason, rlen,
                 "打开 %s 失败: %s%s", kp, strerror(e),
                 e == EACCES ? " (需要 root, 或由管理员对该文件授予 ACL 写权限)"
                             : "");
        return -1;
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "p:%s/%s %s\n", KPROBE_GROUP, KPROBE_EVENT, KPROBE_SYMBOL);
    ssize_t w = write(fd, cmd, strlen(cmd));
    int werr = errno;
    close(fd);
    if (w < 0) {
        const char *hint = "";
        if (werr == EACCES || werr == EPERM)
            hint = " (需要 root 或 CAP_PERFMON)";
        else if (werr == ENOENT || werr == EINVAL)
            hint = " (内核未导出 do_wp_page, 或该符号在 kprobe 黑名单中/已被内联)";
        snprintf(reason, rlen, "向 %s 写入探针定义失败: %s%s",
                 kp, strerror(werr), hint);
        return -1;
    }

    char idp[300];
    snprintf(idp, sizeof(idp), "%s/events/%s/%s/id", tracefs, KPROBE_GROUP, KPROBE_EVENT);
    FILE *f = fopen(idp, "r");
    if (!f) {
        int e = errno;
        snprintf(reason, rlen, "探针已写入但读取事件 id 失败 (%s): %s%s",
                 idp, strerror(e),
                 e == EACCES ? " (需要对 events 目录的遍历/读取权限)" : "");
        return -1;
    }
    int id = -1;
    if (fscanf(f, "%d", &id) != 1) {
        snprintf(reason, rlen, "事件 id 文件 %s 内容无法解析", idp);
        id = -1;
    }
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

/* perf_event 上下文标记(这里只保留用户态调用栈) */
#define CTX_USER    ((uint64_t)-512)  /* PERF_CONTEXT_USER */
#define CTX_MAX     ((uint64_t)-4095) /* 约定: >= 该值视为上下文标记 */

static int is_ctx_marker(uint64_t ip) { return ip >= CTX_MAX; }

/* ------------------------------ 方式二实现 ------------------------------ */

/* 打开子进程的 do_wp_page tracepoint；失败原因写入 reason。 */
static int method2_open(pid_t child, int tp_id, char *reason, size_t rlen)
{
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_TRACEPOINT;
    attr.size = sizeof(attr);
    attr.config = tp_id;
    attr.sample_period = 1;
    attr.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN;
    attr.disabled = 1;
    attr.exclude_callchain_kernel = 1;
    attr.exclude_callchain_user = 0;
    attr.wakeup_events = 1;

    int fd = perf_event_open(&attr, child, -1, -1, 0);
    if (fd >= 0)
        return fd;

    int e = errno;
    const char *hint = "";
    if (e == EACCES || e == EPERM)
        hint = " (请执行 sudo ./cow_demo，或配置 tracefs ACL + "
               "perf_event_paranoid=-1/CAP_PERFMON)";
    snprintf(reason, rlen,
             "kprobe tracepoint(id=%d) perf_event_open 失败: %s%s",
             tp_id, strerror(e), hint);
    return -1;
}

static void print_stack(struct sample *s, usym_ctx_t *us, int max_frames)
{
    int shown = 0;
    int in_user = 0;
    for (uint64_t i = 0; i < s->nr && shown < max_frames; i++) {
        uint64_t ip = s->ips[i];
        if (ip == CTX_USER) { in_user = 1; continue; }
        if (is_ctx_marker(ip)) continue;
        if (!in_user) continue;

        unsigned long off = 0;
        const char *fn = NULL, *mod = NULL;
        usym_resolve(us, ip, &fn, &off, &mod);
        if (fn)
            printf("        #%-2d [u] %s+0x%lx (%s)\n", shown, fn, off,
                   mod ? mod : "?");
        else if (mod)
            printf("        #%-2d [u] %s+0x%lx\n", shown, mod, off);
        else
            printf("        #%-2d [u] 0x%llx\n", shown, (unsigned long long)ip);
        shown++;
    }
}

struct method1_watch {
    pid_t recorder;
    char outdir[64];
    char logpath[96];
    char datapath[96];
    char pages[32];
    int active;
    int recorded;
    int status;      /* 记录进程的 waitpid 状态 */
};

/* 打印记录脚本/perf 的输出, 便于定位方式一的失败原因 */
static void dump_log(const char *path, const char *title)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  (无法打开日志 %s: %s)\n", path, strerror(errno));
        return;
    }
    printf("  ---- %s (%s) ----\n", title, path);
    char line[512];
    int empty = 1;
    while (fgets(line, sizeof(line), f)) {
        empty = 0;
        printf("  | %s", line);
        if (!strchr(line, '\n')) printf("\n");
    }
    if (empty) printf("  | (空)\n");
    printf("  ---- 日志结束 ----\n");
    fclose(f);
}

/* 把 waitpid 状态翻译成可读文本 */
static void describe_status(int status, char *out, size_t len)
{
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        snprintf(out, len, "正常退出, 退出码=%d%s", code,
                 code == 127 ? " (127 通常表示 execl 找不到脚本或解释器)" : "");
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        snprintf(out, len, "被信号终止, signal=%d%s", sig,
                 sig == SIGINT ? " (SIGINT, 即本程序发出的正常停止信号)" : "");
    } else {
        snprintf(out, len, "未知状态 0x%x", (unsigned)status);
    }
}

struct method2_watch {
    int fd;
    int active;
    int kprobe_ok;
    const char *tracefs;
    struct collector col;
    usym_ctx_t *us;
    long long counter;
    char reason[512];
    char logpath[96];
};

/* 方式二不自动提权；失败时把执行上下文与原因同时写终端和独立日志。 */
static void method2_failure_log(struct method2_watch *w, const char *stage)
{
    FILE *f = fopen(w->logpath, "a");
    FILE *outs[2] = { stderr, f };
    char paranoid[32] = "?";
    FILE *pf = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (pf) {
        if (fscanf(pf, "%31s", paranoid) != 1)
            snprintf(paranoid, sizeof(paranoid), "?");
        fclose(pf);
    }

    for (int i = 0; i < 2; i++) {
        FILE *o = outs[i];
        if (!o) continue;
        fprintf(o, "[方式二][失败] 阶段: %s\n", stage);
        fprintf(o, "  uid=%d, euid=%d, perf_event_paranoid=%s\n",
                (int)getuid(), (int)geteuid(), paranoid);
        fprintf(o, "  tracefs=%s\n", w->tracefs ? w->tracefs : "未找到");
        fprintf(o, "  原因: %s\n", w->reason[0] ? w->reason : "未知");
        fprintf(o, "  日志: %s\n", w->logpath);
        if (geteuid() != 0) {
            fprintf(o, "  建议: 直接执行 sudo ./cow_demo；如果坚持非 root，需同时配置\n");
            fprintf(o, "        tracefs ACL 与 perf_event_paranoid=-1/CAP_PERFMON。\n");
        }
        fflush(o);
    }
    if (f) fclose(f);
}

/*
 * 同步注册 do_wp_page 探针。这一步可能很慢, 但它必须在放行子进程之前完成,
 * 否则 perf 还没开始采样, COW 就已经全部发生完了。
 */
static int method1_add_probe(struct method1_watch *w, struct config *cfg)
{
    pid_t p = fork();
    if (p == 0) {
        int fd = open(w->logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
        }
        execl("/bin/sh", "sh", cfg->script, "add-probe", (char *)NULL);
        fprintf(stderr, "execl(%s) 失败: %s\n", cfg->script, strerror(errno));
        _exit(127);
    }
    if (p < 0) {
        perror("fork(add-probe)");
        return -1;
    }

    int waited = 0, st = 0;
    for (;;) {
        pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p) break;
        if (r < 0) { st = 0; break; }
        if (waited >= cfg->probe_timeout_ms) {
            kill(p, SIGKILL);
            waitpid(p, &st, 0);
            printf("[方式一][失败] 注册 do_wp_page 探针超时 (>%d ms), 已终止。\n",
                   cfg->probe_timeout_ms);
            dump_log(w->logpath, "探针注册阶段输出");
            printf("  提示: perf probe 解析 debuginfo 可能很慢, 可用 --probe-timeout "
                   "调大; 也可先手动执行 sudo perf probe --add do_wp_page。\n");
            return -1;
        }
        usleep(50 * 1000);
        waited += 50;
    }

    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        char desc[160];
        describe_status(st, desc, sizeof(desc));
        printf("[方式一][失败] 注册 do_wp_page 探针失败: %s\n", desc);
        dump_log(w->logpath, "探针注册阶段输出");
        printf("  提示: 非 root 时经由 sudo -n perf 执行, 请先运行 sudo -v;\n"
               "        并确认 tracefs 已挂载且内核支持 kprobe。\n");
        return -1;
    }
    if (cfg->verbose)
        printf("[方式一] 探针注册完成, 耗时约 %d ms。\n", waited);
    return 0;
}

/*
 * 等待 perf record 真正开始采样。
 * perf 会在 perf_event_open + mmap 成功之后才写出 perf.data 头部, 因此
 * "perf.data 出现且非空" 是一个比固定 sleep 可靠得多的就绪信号。
 */
static int method1_wait_ready(struct method1_watch *w, struct config *cfg)
{
    int waited = 0;
    for (;;) {
        int st = 0;
        if (waitpid(w->recorder, &st, WNOHANG) == w->recorder) {
            char desc[160];
            describe_status(st, desc, sizeof(desc));
            printf("[方式一][失败] 记录进程在开始采样前就退出了: %s\n", desc);
            dump_log(w->logpath, "记录阶段输出");
            printf("  提示: 非 root 模式通过 sudo -n perf 执行。请先运行 sudo -v，\n"
                   "        并确认 sudoers/secure_path 允许 perf；也可执行 ./setup.sh 检查。\n");
            return -1;
        }

        struct stat sb;
        if (stat(w->datapath, &sb) == 0 && sb.st_size > 0) {
            /* 再静置一小会儿, 抹平不同 perf 版本写头部与开始采样的次序差异 */
            usleep((useconds_t)cfg->settle_ms * 1000);
            if (cfg->verbose)
                printf("[方式一] perf record 已开始采样 (等待 %d ms)。\n", waited);
            return 0;
        }

        if (waited >= cfg->ready_timeout_ms) {
            /*
             * 记录进程还活着, 只是没等到就绪信号。继续往下走仍有可能采到数据,
             * 所以这里只告警, 由最终的事件数来体现真实结果。
             */
            printf("[方式一][告警] 等待 %d ms 仍未确认 perf record 已开始采样,\n"
                   "             继续执行; 若最终事件数为 0, 请用 --ready-timeout 调大。\n",
                   cfg->ready_timeout_ms);
            return 0;
        }
        usleep(50 * 1000);
        waited += 50;
    }
}

static int method1_start(struct method1_watch *w, struct config *cfg,
                         pid_t child, size_t bytes)
{
    memset(w, 0, sizeof(*w));
    snprintf(w->outdir, sizeof(w->outdir), "/tmp/cow_demo_perfXXXXXX");
    if (!mkdtemp(w->outdir)) {
        perror("mkdtemp");
        return -1;
    }

    char pidstr[32];
    snprintf(pidstr, sizeof(pidstr), "%d", (int)child);
    snprintf(w->pages, sizeof(w->pages), "%zu", bytes / (size_t)page_size);
    snprintf(w->logpath, sizeof(w->logpath), "%s/record.log", w->outdir);
    snprintf(w->datapath, sizeof(w->datapath), "%s/perf.data", w->outdir);

    printf("========================================================\n");
    printf(" 方式一: perf record --user-callchains -e probe:do_wp_page -g -p %d\n",
           (int)child);
    printf(" 记录脚本: %s\n", cfg->script);
    printf(" 输出目录: %s\n", w->outdir);
    printf(" 记录日志: %s\n", w->logpath);
    printf("========================================================\n");

    /* 每次运行重建日志: 注册阶段与记录阶段都追加到同一个文件 */
    int lfd = open(w->logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd >= 0) close(lfd);

    /*
     * 第一步: 同步注册探针。
     * perf probe 需要解析 kallsyms/debuginfo, 常常要数秒。以前它和 perf record
     * 一起塞在固定热身 sleep 里, 一旦超过热身时间, 子进程就会在 perf 尚未开始
     * 采样时跑完全部 COW 写入, 最终连 perf.data 都没有。
     */
    if (method1_add_probe(w, cfg) < 0)
        return -1;

    /* 第二步: 启动记录进程, 探针已就绪, 它可以直接进入 perf record */
    w->recorder = fork();
    if (w->recorder == 0) {
        setpgid(0, 0);
        /* 把脚本与 perf 的 stdout/stderr 全部落盘, 失败时回放给用户 */
        int fd = open(w->logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO) close(fd);
        }
        execl("/bin/sh", "sh", cfg->script, "record",
              "--pid", pidstr, "--out", w->outdir, "--skip-probe", (char *)NULL);
        fprintf(stderr, "execl(%s) 失败: %s\n", cfg->script, strerror(errno));
        _exit(127);
    }
    if (w->recorder < 0) {
        perror("fork(record)");
        return -1;
    }
    setpgid(w->recorder, w->recorder);

    /* 第三步: 等 perf record 真正开始采样, 而不是盲等一个固定时长 */
    if (method1_wait_ready(w, cfg) < 0)
        return -1;

    w->active = 1;
    return 0;
}

static void method1_stop(struct method1_watch *w)
{
    if (!w->active) return;
    kill(-w->recorder, SIGINT);
    waitpid(w->recorder, &w->status, 0);
    w->active = 0;
    w->recorded = 1;
}

/* 返回 0 表示记录与分析正常; -1 表示方式一失败(已打印诊断信息) */
static int method1_report(struct method1_watch *w, struct config *cfg)
{
    if (!w->recorded) return 0;

    char desc[160];
    describe_status(w->status, desc, sizeof(desc));

    /*
     * perf record 收到 SIGINT 后应当写出 perf.data。这里用 stat() 判断"存在且
     * 非空", 而不是 access(R_OK): 经由 sudo 录制时 perf.data 可能一度是 root:0600,
     * 属主由脚本改回调用用户; 即便属主修正失败, 后续分析仍走 sudo perf script,
     * 因此只要文件已生成就继续, 不因当前进程读不到而误判为失败。
     */
    struct stat sb;
    if (stat(w->datapath, &sb) != 0 || sb.st_size == 0) {
        printf("\n[方式一][失败] 未生成有效的 %s (%s)\n", w->datapath,
               stat(w->datapath, &sb) != 0 ? strerror(errno) : "文件为空");
        printf("  记录进程结束情况: %s\n", desc);
        dump_log(w->logpath, "记录阶段输出");
        printf("  常见原因:\n");
        printf("    1) sudo -n perf 不可用 (未 sudo -v、sudoers 拒绝或 secure_path 找不到 perf);\n");
        printf("    2) perf probe --add do_wp_page 失败 (tracefs/kprobe 不可用);\n");
        printf("    3) 内核未导出 do_wp_page, 或该符号被内联/在 kprobe 黑名单中;\n");
        printf("    4) perf record 无权 attach 目标进程 "
               "(perf_event_paranoid 过高);\n");
        printf("    5) perf 版本与内核不匹配。\n");
        printf("  可执行 ./setup.sh 逐项确认, 或手动运行:\n");
        printf("    %s record --pid <PID> --out <DIR>\n", cfg->script);
        return -1;
    }

    /* 文件已生成但当前用户读不到: 属主修正可能失败, 交给 sudo perf script 处理 */
    if (access(w->datapath, R_OK) != 0)
        printf("\n[方式一] 提示: 当前用户暂不能直接读取 %s, 将通过 perf 读取分析。\n",
               w->datapath);

    if (cfg->verbose) {
        printf("\n[方式一] 记录进程结束情况: %s\n", desc);
        dump_log(w->logpath, "记录阶段输出");
    }

    pid_t rep = fork();
    if (rep == 0) {
        execl("/bin/sh", "sh", cfg->script, "report",
              "--out", w->outdir, "--pages", w->pages, (char *)NULL);
        _exit(127);
    }
    if (rep > 0) {
        int st = 0;
        waitpid(rep, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            char rdesc[160];
            describe_status(st, rdesc, sizeof(rdesc));
            printf("\n[方式一][失败] 分析阶段未得到有效结果: %s\n", rdesc);
            /* 记录阶段日志里有 perf 自己打印的 "(N samples)", 是关键佐证 */
            dump_log(w->logpath, "记录阶段输出");
            return -1;
        }
    }
    return 0;
}

static int method2_start(struct method2_watch *w, struct config *cfg, pid_t child)
{
    memset(w, 0, sizeof(*w));
    w->fd = -1;
    snprintf(w->logpath, sizeof(w->logpath),
             "/tmp/cow_demo_method2_%d.log", (int)child);
    unlink(w->logpath);
    w->tracefs = find_tracefs();
    int tp_id = -1;

    if (!w->tracefs) {
        snprintf(w->reason, sizeof(w->reason),
                 "未找到 tracefs (已尝试 /proc/mounts、/sys/kernel/tracing、"
                 "/sys/kernel/debug/tracing); 内核可能未启用 ftrace/kprobe, "
                 "或容器未挂载 tracefs");
        method2_failure_log(w, "查找 tracefs");
        return -1;
    }

    tp_id = kprobe_register(w->tracefs, w->reason, sizeof(w->reason));
    if (tp_id < 0) {
        method2_failure_log(w, "注册 do_wp_page kprobe / 读取动态事件 id");
        return -1;
    }
    w->kprobe_ok = 1;

    w->fd = method2_open(child, tp_id, w->reason, sizeof(w->reason));
    if (w->fd < 0) {
        method2_failure_log(w, "perf_event_open(PERF_TYPE_TRACEPOINT)");
        kprobe_unregister(w->tracefs);
        w->kprobe_ok = 0;
        return -1;
    }
    if (collector_init(&w->col, w->fd, cfg->max_print) < 0) {
        snprintf(w->reason, sizeof(w->reason),
                 "mmap perf 环形缓冲失败: %s", strerror(errno));
        method2_failure_log(w, "mmap perf 环形缓冲");
        close(w->fd);
        w->fd = -1;
        if (w->kprobe_ok) {
            kprobe_unregister(w->tracefs);
            w->kprobe_ok = 0;
        }
        return -1;
    }

    printf("========================================================\n");
    printf(" 方式二: perf_event_open 订阅 do_wp_page (kprobe tracepoint)\n");
    printf(" 事件 id: %d, tracefs: %s\n", tp_id, w->tracefs);
    printf(" 仅采集子进程用户态调用栈；无 page-fault fallback。\n");
    printf("========================================================\n");

    ioctl(w->fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(w->fd, PERF_EVENT_IOC_ENABLE, 0);
    w->active = 1;
    return 0;
}

static void wait_for_child_write(struct method2_watch *w, int wrote_fd,
                                 pid_t child)
{
    if (!w->active) {
        waitfor(wrote_fd);
        return;
    }

    struct pollfd pfds[2] = {
        { .fd = w->fd, .events = POLLIN },
        { .fd = wrote_fd, .events = POLLIN },
    };
    for (;;) {
        int pr = poll(pfds, 2, 100);
        if (pr < 0 && errno == EINTR) continue;
        collector_drain(&w->col);
        if (pfds[1].revents & POLLIN) {
            waitfor(wrote_fd);
            w->us = usym_capture(child);
            collector_drain(&w->col);
            break;
        }
    }
}

static void method2_stop(struct method2_watch *w)
{
    if (!w->active) return;
    ioctl(w->fd, PERF_EVENT_IOC_DISABLE, 0);
    collector_drain(&w->col);
    if (read(w->fd, &w->counter, sizeof(w->counter)) != sizeof(w->counter))
        w->counter = -1;
    w->active = 0;
}

static void method2_report(struct method2_watch *w, struct config *cfg,
                           pid_t child, size_t bytes)
{
    if (w->fd < 0) return;
    int nprint = w->col.keep_n < cfg->max_print ? w->col.keep_n : cfg->max_print;
    for (int i = 0; i < nprint; i++) {
        printf("\n  [方式二样本 #%d] 子进程用户态调用栈:\n", i + 1);
        print_stack(&w->col.keep[i], w->us, cfg->max_frames);
    }

    unsigned long long events = w->col.n_samples;
    unsigned long long cow_bytes = events * (unsigned long long)page_size;
    printf("\n---------------- 方式二 COW 内存汇总 ----------------\n");
    printf("  目标子进程 PID     : %d\n", (int)child);
    printf("  采集事件源         : probe:do_wp_page\n");
    printf("  申请/写入内存      : %zu MiB (%zu 页, 页大小 %ld 字节)\n",
           cfg->size_mib, bytes / page_size, page_size);
    printf("  采集到事件数       : %llu\n", events);
    printf("  计数器累计值       : %lld\n", w->counter);
    if (w->col.n_lost)
        printf("  丢失样本(缓冲溢出) : %llu\n", w->col.n_lost);
    printf("  估算 COW 内存      : %llu 字节 (%.2f MiB) = 事件数 x 页大小\n",
           cow_bytes, cow_bytes / (1024.0 * 1024.0));
    printf("------------------------------------------------------\n");
}

static void method2_cleanup(struct method2_watch *w)
{
    if (w->fd < 0) return;
    usym_free(w->us);
    collector_free(&w->col);
    close(w->fd);
    if (w->kprobe_ok) kprobe_unregister(w->tracefs);
}

static int run_watchers(struct config *cfg, size_t bytes, pid_t child,
                        struct sync_pipes *sp)
{
    int want1 = cfg->watch == WATCH_BOTH || cfg->watch == WATCH_METHOD1;
    int want2 = cfg->watch == WATCH_BOTH || cfg->watch == WATCH_METHOD2;
    struct method1_watch m1;
    struct method2_watch m2;
    memset(&m1, 0, sizeof(m1));
    memset(&m2, 0, sizeof(m2));
    m2.fd = -1;

    int failures = 0;
    if (want1 && method1_start(&m1, cfg, child, bytes) < 0) failures++;
    if (want2 && method2_start(&m2, cfg, child) < 0) failures++;

    printf("[父进程] watcher 启动结果: 方式一=%s, 方式二=%s\n",
           m1.active ? "已启动" : (want1 ? "失败" : "未选择"),
           m2.active ? "已启动" : (want2 ? "失败" : "未选择"));

    if (!m1.active && !m2.active) {
        printf("[父进程][失败] 没有任何 watcher 成功启动，终止等待中的子进程，"
               "不执行 COW 工作负载。\n");
        kill(child, SIGTERM);
        method2_cleanup(&m2);
        return 1;
    }

    printf("[父进程] 至少一个 watcher 已启动，放行子进程。\n");
    notify(sp->go[1]);
    wait_for_child_write(&m2, sp->wrote[0], child);

    /* 两个 watcher 都必须在子进程退出之前停止。 */
    method2_stop(&m2);
    method1_stop(&m1);
    notify(sp->quit[1]);

    method2_report(&m2, cfg, child, bytes);
    if (method1_report(&m1, cfg) < 0) failures++;
    method2_cleanup(&m2);
    return failures ? 1 : 0;
}

/* ------------------------------ 主程序 ------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "  --watch W        watcher: method1|method2|both (默认 both)\n"
        "  --size M         申请内存大小, 单位 MiB (默认 16)\n"
        "  --script PATH    方式一脚本路径 (默认 scripts/perf_cow_watch.sh)\n"
        "  --probe-timeout MS  方式一注册 do_wp_page 探针的超时 (默认 60000)\n"
        "  --ready-timeout MS  方式一等待 perf record 开始采样的超时 (默认 15000)\n"
        "  --settle MS      确认就绪后额外静置毫秒数 (默认 200)\n"
        "  --max-print N    最多打印多少条样本调用栈 (默认 3)\n"
        "  --max-frames N   每条调用栈最多打印多少帧 (默认 24)\n"
        "  --verbose        打印更多诊断信息\n"
        "  --help           显示本帮助\n",
        prog);
}

int main(int argc, char **argv)
{
    /* 行缓冲: 输出重定向到文件时, 父子进程的日志仍按真实发生顺序排列 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    page_size = sysconf(_SC_PAGESIZE);

    struct config cfg = {
        .watch = WATCH_BOTH,
        .size_mib = 16,
        .max_print = 3,
        .max_frames = 24,
        .script = "scripts/perf_cow_watch.sh",
        .probe_timeout_ms = 60000,
        .ready_timeout_ms = 15000,
        .settle_ms = 200,
        .verbose = 0,
    };

    static struct option opts[] = {
        {"watch", required_argument, 0, 'W'},
        {"size", required_argument, 0, 's'},
        {"script", required_argument, 0, 'c'},
        {"probe-timeout", required_argument, 0, 'T'},
        {"ready-timeout", required_argument, 0, 'R'},
        {"settle", required_argument, 0, 'S'},
        {"max-print", required_argument, 0, 'p'},
        {"max-frames", required_argument, 0, 'f'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "W:s:c:T:R:S:p:f:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'W':
            if (!strcmp(optarg, "both")) cfg.watch = WATCH_BOTH;
            else if (!strcmp(optarg, "method1")) cfg.watch = WATCH_METHOD1;
            else if (!strcmp(optarg, "method2")) cfg.watch = WATCH_METHOD2;
            else {
                fprintf(stderr, "--watch 只能为 method1、method2 或 both\n");
                return 2;
            }
            break;
        case 's': cfg.size_mib = strtoul(optarg, NULL, 10); break;
        case 'c': cfg.script = optarg; break;
        case 'T': cfg.probe_timeout_ms = atoi(optarg); break;
        case 'R': cfg.ready_timeout_ms = atoi(optarg); break;
        case 'S': cfg.settle_ms = atoi(optarg); break;
        case 'p': cfg.max_print = atoi(optarg); break;
        case 'f': cfg.max_frames = atoi(optarg); break;
        case 'v': cfg.verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
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

    int rc = run_watchers(&cfg, bytes, child, &sp);

    int st = 0;
    waitpid(child, &st, 0);
    printf("[父进程] 子进程已退出。\n");
    return rc;
}
