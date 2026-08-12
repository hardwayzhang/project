/*
 * symbolize.c - 见 symbolize.h
 */
#define _GNU_SOURCE
#include "symbolize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* ============================ 通用符号表辅助 ============================ */

static int sym_cmp(const void *a, const void *b)
{
    const sym_t *x = a, *y = b;
    if (x->addr < y->addr) return -1;
    if (x->addr > y->addr) return 1;
    return 0;
}

static const char *tab_resolve(const symtab_t *tab, unsigned long ip, unsigned long *off)
{
    if (!tab->n) return NULL;
    size_t lo = 0, hi = tab->n; /* 找最大的 addr <= ip */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (tab->syms[mid].addr <= ip)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0) return NULL;
    const sym_t *s = &tab->syms[lo - 1];
    if (s->size && ip >= s->addr + s->size)
        return NULL; /* 落在两个符号之间的空洞 */
    if (off) *off = ip - s->addr;
    return s->name;
}

void symtab_free(symtab_t *tab)
{
    if (!tab) return;
    free(tab->syms);
    free(tab->strpool);
    memset(tab, 0, sizeof(*tab));
}

/* ============================ 用户符号 ============================ */

/* 一个 ELF 文件里的 PT_LOAD 段，用于把运行时地址还原成 ELF 虚拟地址 */
typedef struct {
    unsigned long vaddr;
    unsigned long offset;
    unsigned long filesz;
} seg_t;

typedef struct elf_cache {
    char *path;
    symtab_t tab;
    seg_t *segs;
    size_t nseg;
    int failed;
    struct elf_cache *next;
} elf_cache_t;

struct usym_ctx {
    umap_t *maps;
    size_t nmap;
    elf_cache_t *files;
};

static void add_sym(sym_t **arr, size_t *n, size_t *cap,
                    char **pool, size_t *plen, size_t *pcap,
                    unsigned long addr, unsigned long size, const char *name)
{
    size_t nl = strlen(name) + 1;
    if (*plen + nl > *pcap) {
        while (*plen + nl > *pcap) *pcap *= 2;
        *pool = realloc(*pool, *pcap);
    }
    if (*n == *cap) {
        *cap *= 2;
        *arr = realloc(*arr, *cap * sizeof(**arr));
    }
    memcpy(*pool + *plen, name, nl);
    (*arr)[*n].addr = addr;
    (*arr)[*n].size = size;
    (*arr)[*n].name = (char *)(uintptr_t)(*plen); /* 暂存偏移 */
    (*n)++;
    *plen += nl;
}

/* 解析一个 ELF 文件的符号表与 PT_LOAD 段 */
static elf_cache_t *elf_load(const char *path)
{
    elf_cache_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->path = strdup(path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) { c->failed = 1; return c; }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        close(fd); c->failed = 1; return c;
    }
    size_t fsz = st.st_size;
    uint8_t *base = mmap(NULL, fsz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { c->failed = 1; return c; }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64) {
        munmap(base, fsz); c->failed = 1; return c;
    }

    /* PT_LOAD 段 */
    size_t segcap = 8;
    c->segs = malloc(segcap * sizeof(seg_t));
    c->nseg = 0;
    Elf64_Phdr *ph = (Elf64_Phdr *)(base + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (c->nseg == segcap) {
            segcap *= 2;
            c->segs = realloc(c->segs, segcap * sizeof(seg_t));
        }
        c->segs[c->nseg].vaddr = ph[i].p_vaddr;
        c->segs[c->nseg].offset = ph[i].p_offset;
        c->segs[c->nseg].filesz = ph[i].p_filesz;
        c->nseg++;
    }

    /* 符号表：优先 .symtab，再补 .dynsym */
    size_t cap = 256, n = 0, pcap = 1 << 16, plen = 0;
    sym_t *arr = malloc(cap * sizeof(sym_t));
    char *pool = malloc(pcap);

    Elf64_Shdr *sh = (Elf64_Shdr *)(base + eh->e_shoff);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM)
            continue;
        Elf64_Sym *syms = (Elf64_Sym *)(base + sh[i].sh_offset);
        size_t cnt = sh[i].sh_size / sizeof(Elf64_Sym);
        const char *strs = (const char *)(base + sh[sh[i].sh_link].sh_offset);
        for (size_t j = 0; j < cnt; j++) {
            int t = ELF64_ST_TYPE(syms[j].st_info);
            if (t != STT_FUNC && t != STT_GNU_IFUNC)
                continue;
            if (syms[j].st_shndx == SHN_UNDEF || syms[j].st_value == 0)
                continue;
            const char *nm = strs + syms[j].st_name;
            if (!nm || !nm[0]) continue;
            add_sym(&arr, &n, &cap, &pool, &plen, &pcap,
                    syms[j].st_value, syms[j].st_size, nm);
        }
    }
    munmap(base, fsz);

    for (size_t i = 0; i < n; i++)
        arr[i].name = pool + (uintptr_t)arr[i].name;
    qsort(arr, n, sizeof(sym_t), sym_cmp);

    c->tab.syms = arr;
    c->tab.n = n;
    c->tab.strpool = pool;
    return c;
}

static elf_cache_t *ctx_get_file(usym_ctx_t *ctx, const char *path)
{
    for (elf_cache_t *c = ctx->files; c; c = c->next)
        if (strcmp(c->path, path) == 0)
            return c;
    elf_cache_t *c = elf_load(path);
    if (!c) return NULL;
    c->next = ctx->files;
    ctx->files = c;
    return c;
}

usym_ctx_t *usym_capture(pid_t pid)
{
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/maps", (int)pid);
    FILE *fp = fopen(p, "r");
    if (!fp) return NULL;

    usym_ctx_t *ctx = calloc(1, sizeof(*ctx));
    size_t cap = 64;
    ctx->maps = malloc(cap * sizeof(umap_t));

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start, end, off;
        char perms[8], path[256];
        path[0] = '\0';
        int m = sscanf(line, "%lx-%lx %7s %lx %*s %*s %255[^\n]",
                       &start, &end, perms, &off, path);
        if (m < 4) continue;
        /* 去掉路径前导空格 */
        char *ps = path;
        while (*ps == ' ') ps++;
        if (!*ps) continue;                 /* 匿名映射不含符号 */
        if (ps[0] == '[') continue;         /* [stack]/[heap] 等 */
        if (ctx->nmap == cap) {
            cap *= 2;
            ctx->maps = realloc(ctx->maps, cap * sizeof(umap_t));
        }
        ctx->maps[ctx->nmap].start = start;
        ctx->maps[ctx->nmap].end = end;
        ctx->maps[ctx->nmap].pgoff = off;
        ctx->maps[ctx->nmap].exec = (perms[2] == 'x');
        ctx->maps[ctx->nmap].path = strdup(ps);
        ctx->nmap++;
    }
    fclose(fp);
    return ctx;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int usym_resolve(usym_ctx_t *ctx, unsigned long ip,
                 const char **func, unsigned long *off, const char **module)
{
    if (func) *func = NULL;
    if (module) *module = NULL;
    if (off) *off = 0;
    if (!ctx) return -1;

    umap_t *m = NULL;
    for (size_t i = 0; i < ctx->nmap; i++) {
        if (ip >= ctx->maps[i].start && ip < ctx->maps[i].end) {
            m = &ctx->maps[i];
            break;
        }
    }
    if (!m) return -1;
    if (module) *module = base_name(m->path);

    elf_cache_t *c = ctx_get_file(ctx, m->path);
    if (!c || c->failed) {
        /* 无法解析符号，退化为 模块+文件偏移 */
        if (off) *off = ip - m->start + m->pgoff;
        return 0;
    }

    /* 运行时地址 -> 文件偏移 -> ELF 虚拟地址 */
    unsigned long file_off = ip - m->start + m->pgoff;
    unsigned long vaddr = 0;
    int found_seg = 0;
    for (size_t i = 0; i < c->nseg; i++) {
        if (file_off >= c->segs[i].offset &&
            file_off < c->segs[i].offset + c->segs[i].filesz) {
            vaddr = c->segs[i].vaddr + (file_off - c->segs[i].offset);
            found_seg = 1;
            break;
        }
    }
    if (!found_seg) {
        if (off) *off = file_off;
        return 0;
    }

    unsigned long o = 0;
    const char *nm = tab_resolve(&c->tab, vaddr, &o);
    if (nm) {
        if (func) *func = nm;
        if (off) *off = o;
    } else {
        if (off) *off = vaddr;
    }
    return 0;
}

void usym_free(usym_ctx_t *ctx)
{
    if (!ctx) return;
    for (size_t i = 0; i < ctx->nmap; i++)
        free(ctx->maps[i].path);
    free(ctx->maps);
    elf_cache_t *c = ctx->files;
    while (c) {
        elf_cache_t *nx = c->next;
        free(c->path);
        free(c->segs);
        symtab_free(&c->tab);
        free(c);
        c = nx;
    }
    free(ctx);
}
