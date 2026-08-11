#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct memory_stats {
    uint64_t rss_kb;
    uint64_t uss_kb;
};

static int parse_kb_line(const char *line, const char *name, uint64_t *value)
{
    size_t name_len = strlen(name);
    unsigned long long parsed;

    if (strncmp(line, name, name_len) != 0)
        return 0;
    if (sscanf(line + name_len, "%llu kB", &parsed) != 1)
        return -1;

    *value += (uint64_t)parsed;
    return 1;
}

static int read_smaps_stats(uintptr_t wanted_start, uintptr_t wanted_end,
                            struct memory_stats *stats)
{
    FILE *file = fopen("/proc/self/smaps", "re");
    char *line = NULL;
    size_t capacity = 0;
    bool selected = false;
    int result = -1;

    if (file == NULL) {
        perror("fopen(/proc/self/smaps)");
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    while (getline(&line, &capacity, file) != -1) {
        unsigned long start;
        unsigned long end;
        uint64_t value;
        int parsed;

        if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
            selected = (uintptr_t)start < wanted_end &&
                       (uintptr_t)end > wanted_start;
            continue;
        }
        if (!selected)
            continue;

        value = 0;
        parsed = parse_kb_line(line, "Rss:", &value);
        if (parsed < 0)
            goto out;
        if (parsed > 0) {
            stats->rss_kb += value;
            continue;
        }

        value = 0;
        parsed = parse_kb_line(line, "Private_Clean:", &value);
        if (parsed < 0)
            goto out;
        if (parsed > 0) {
            stats->uss_kb += value;
            continue;
        }

        value = 0;
        parsed = parse_kb_line(line, "Private_Dirty:", &value);
        if (parsed < 0)
            goto out;
        if (parsed > 0) {
            stats->uss_kb += value;
            continue;
        }

        value = 0;
        parsed = parse_kb_line(line, "Private_Hugetlb:", &value);
        if (parsed < 0)
            goto out;
        if (parsed > 0)
            stats->uss_kb += value;
    }

    if (ferror(file)) {
        perror("getline(/proc/self/smaps)");
        goto out;
    }
    result = 0;

out:
    free(line);
    fclose(file);
    return result;
}

static int read_process_stats(struct memory_stats *stats)
{
    FILE *file = fopen("/proc/self/smaps_rollup", "re");
    char *line = NULL;
    size_t capacity = 0;
    int result = -1;

    if (file == NULL) {
        perror("fopen(/proc/self/smaps_rollup)");
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    while (getline(&line, &capacity, file) != -1) {
        uint64_t value = 0;
        int parsed = parse_kb_line(line, "Rss:", &value);

        if (parsed < 0)
            goto out;
        if (parsed > 0) {
            stats->rss_kb += value;
            continue;
        }

        value = 0;
        parsed = parse_kb_line(line, "Private_Clean:", &value);
        if (parsed < 0)
            goto out;
        if (parsed > 0) {
            stats->uss_kb += value;
            continue;
        }

        value = 0;
        parsed = parse_kb_line(line, "Private_Dirty:", &value);
        if (parsed < 0)
            goto out;
        if (parsed > 0) {
            stats->uss_kb += value;
            continue;
        }

        value = 0;
        parsed = parse_kb_line(line, "Private_Hugetlb:", &value);
        if (parsed < 0)
            goto out;
        if (parsed > 0)
            stats->uss_kb += value;
    }

    if (ferror(file)) {
        perror("getline(/proc/self/smaps_rollup)");
        goto out;
    }
    result = 0;

out:
    free(line);
    fclose(file);
    return result;
}

static int count_resident_pages(void *address, size_t pages, size_t page_size,
                                size_t *resident)
{
    unsigned char *states = calloc(pages, sizeof(*states));
    size_t count = 0;

    if (states == NULL) {
        perror("calloc");
        return -1;
    }
    if (mincore(address, pages * page_size, states) != 0) {
        perror("mincore");
        free(states);
        return -1;
    }

    for (size_t i = 0; i < pages; ++i)
        count += (states[i] & 1U) != 0;

    free(states);
    *resident = count;
    return 0;
}

static int parse_pages(const char *text, const char *name, size_t *pages)
{
    char *end;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || value > SIZE_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        return -1;
    }

    *pages = (size_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    long page_size_long = sysconf(_SC_PAGESIZE);
    size_t total_pages = 1024;
    size_t reclaim_pages;
    size_t page_size;
    size_t mapping_size;
    size_t reclaim_offset_pages;
    void *reservation;
    unsigned char *mapping;
    struct memory_stats region_before;
    struct memory_stats region_after;
    struct memory_stats process_before;
    struct memory_stats process_after;
    size_t resident_before;
    size_t resident_after;
    int exit_code = EXIT_FAILURE;

    if (argc > 3) {
        fprintf(stderr, "usage: %s [total_pages [reclaim_pages]]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (page_size_long <= 0) {
        fprintf(stderr, "sysconf(_SC_PAGESIZE) failed\n");
        return EXIT_FAILURE;
    }
    page_size = (size_t)page_size_long;

    if (argc >= 2 && parse_pages(argv[1], "total_pages", &total_pages) != 0)
        return EXIT_FAILURE;
    reclaim_pages = total_pages / 2;
    if (argc == 3 &&
        parse_pages(argv[2], "reclaim_pages", &reclaim_pages) != 0)
        return EXIT_FAILURE;

    if (total_pages < 3 || reclaim_pages == 0 ||
        reclaim_pages >= total_pages) {
        fprintf(stderr,
                "require total_pages >= 3 and 0 < reclaim_pages < total_pages\n");
        return EXIT_FAILURE;
    }
    if (total_pages > SIZE_MAX / page_size - 2) {
        fprintf(stderr, "requested mapping is too large\n");
        return EXIT_FAILURE;
    }

    mapping_size = total_pages * page_size;
    reservation = mmap(NULL, mapping_size + 2 * page_size, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }
    mapping = (unsigned char *)reservation + page_size;
    if (mprotect(mapping, mapping_size, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect");
        goto unmap;
    }
#ifdef MADV_NOHUGEPAGE
    if (madvise(mapping, mapping_size, MADV_NOHUGEPAGE) != 0 &&
        errno != EINVAL) {
        perror("madvise(MADV_NOHUGEPAGE)");
        goto unmap;
    }
#endif

    for (size_t i = 0; i < total_pages; ++i)
        mapping[i * page_size] = (unsigned char)i;

    if (count_resident_pages(mapping, total_pages, page_size,
                             &resident_before) != 0 ||
        read_smaps_stats((uintptr_t)mapping,
                         (uintptr_t)mapping + mapping_size,
                         &region_before) != 0 ||
        read_process_stats(&process_before) != 0)
        goto unmap;

    reclaim_offset_pages = (total_pages - reclaim_pages) / 2;
    if (madvise(mapping + reclaim_offset_pages * page_size,
                reclaim_pages * page_size, MADV_DONTNEED) != 0) {
        perror("madvise(MADV_DONTNEED)");
        goto unmap;
    }

    if (count_resident_pages(mapping, total_pages, page_size,
                             &resident_after) != 0 ||
        read_smaps_stats((uintptr_t)mapping,
                         (uintptr_t)mapping + mapping_size,
                         &region_after) != 0 ||
        read_process_stats(&process_after) != 0)
        goto unmap;

    printf("page size:              %zu bytes\n", page_size);
    printf("mapping:                %zu pages (%zu KiB)\n", total_pages,
           mapping_size / 1024);
    printf("MADV_DONTNEED range:    pages %zu..%zu (%zu pages)\n",
           reclaim_offset_pages,
           reclaim_offset_pages + reclaim_pages - 1, reclaim_pages);
    printf("\n");
    printf("                         before       after       change\n");
    printf("mapping resident pages: %10zu  %10zu  %+11" PRId64 "\n",
           resident_before, resident_after,
           (int64_t)resident_after - (int64_t)resident_before);
    printf("mapping RSS (KiB):      %10" PRIu64 "  %10" PRIu64 "  %+11" PRId64 "\n",
           region_before.rss_kb, region_after.rss_kb,
           (int64_t)region_after.rss_kb - (int64_t)region_before.rss_kb);
    printf("mapping USS (KiB):      %10" PRIu64 "  %10" PRIu64 "  %+11" PRId64 "\n",
           region_before.uss_kb, region_after.uss_kb,
           (int64_t)region_after.uss_kb - (int64_t)region_before.uss_kb);
    printf("process USS (KiB):      %10" PRIu64 "  %10" PRIu64 "  %+11" PRId64 "\n",
           process_before.uss_kb, process_after.uss_kb,
           (int64_t)process_after.uss_kb - (int64_t)process_before.uss_kb);

    if (resident_after >= resident_before ||
        region_after.uss_kb >= region_before.uss_kb) {
        fprintf(stderr,
                "\nFAIL: MADV_DONTNEED did not reduce mapping residency and USS\n");
        goto unmap;
    }

    printf("\nPASS: MADV_DONTNEED reduced the mapping USS by %" PRIu64
           " KiB.\n",
           region_before.uss_kb - region_after.uss_kb);
    exit_code = EXIT_SUCCESS;

unmap:
    if (munmap(reservation, mapping_size + 2 * page_size) != 0) {
        perror("munmap");
        exit_code = EXIT_FAILURE;
    }
    return exit_code;
}
