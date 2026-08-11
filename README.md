# MADV_DONTNEED 对 USS 的影响验证

这个 Linux 小程序使用匿名私有 `mmap` 创建一块内存，逐页写入以确保页面实际分配，
然后对映射中间的一部分页面调用 `madvise(MADV_DONTNEED)`。程序在调用前后读取：

- `/proc/self/smaps`：统计目标映射的 RSS 和 USS；
- `/proc/self/smaps_rollup`：统计整个进程的 USS；
- `mincore(2)`：统计目标映射的驻留页数。

USS 按 `Private_Clean + Private_Dirty + Private_Hugetlb` 计算。目标映射两端设置了保护页，
避免它与相邻 VMA 合并，从而让映射自身的统计不受其他内存区域干扰。

## 编译和运行

```sh
make
./madvise_uss
```

默认分配 1024 页，并归还中间的 512 页。也可指定总页数和归还页数：

```sh
./madvise_uss 4096 2048
```

参数要求：总页数至少为 3，归还页数大于 0 且小于总页数。

典型输出如下（具体数值取决于系统页大小）：

```text
page size:              4096 bytes
mapping:                1024 pages (4096 KiB)
MADV_DONTNEED range:    pages 256..767 (512 pages)

                         before       after       change
mapping resident pages:       1024         512         -512
mapping RSS (KiB):            4096        2048        -2048
mapping USS (KiB):            4096        2048        -2048
process USS (KiB):            4204        2156        -2048

PASS: MADV_DONTNEED reduced the mapping USS by 2048 KiB.
```

运行 `make test` 可执行默认参数和非整比例参数两组验证。该程序依赖 Linux 的
`/proc`、`mincore(2)` 和 `madvise(2)`。
