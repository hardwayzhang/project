# Linux Disk Write Speed Benchmark

A small, self-contained C program that measures how fast a Linux machine
can write to an ordinary disk under several different I/O modes, plus a
shell wrapper that cross-checks the numbers against the standard `dd`
utility.

## Why more than one number?

"Disk write speed" is not a single number — the answer depends on
*how* you write:

| Mode       | Flags                       | What it really measures                                                                 |
| ---------- | --------------------------- | --------------------------------------------------------------------------------------- |
| `buffered` | plain `write()`             | Throughput to the kernel page cache. Bursty writes often look very fast here.           |
| `fsync`    | `write()` + final `fsync()` | Time to *durably* write all data to the device.                                         |
| `osync`    | `O_SYNC`                    | Every `write()` blocks until durable. Shows the worst case for many small writes.       |
| `odirect`  | `O_DIRECT`                  | Bypasses the page cache. The closest userspace estimate of raw device throughput.       |

Each mode is run for a sweep of block sizes (4 KiB, 64 KiB, 1 MiB,
4 MiB) so you can see how throughput scales with I/O granularity.

## Files

- `disk_write_test.c` — the benchmark itself.
- `Makefile` — `make`, `make run`, `make run-small`, `make clean`.
- `run_benchmarks.sh` — builds the program, runs it, and also runs
  `dd` in three configurations for a sanity check.

## Build

```bash
make
```

Requires `gcc` and a POSIX/Linux system. No external libraries.

## Run

A short smoke run (writes 64 MiB per mode/block, finishes in a few
seconds):

```bash
make run-small
```

A more meaningful run (writes 512 MiB per mode/block, the default):

```bash
make run
# or
./disk_write_test -s 512
```

Custom path / size / modes:

```bash
./disk_write_test -f /mnt/data/scratch.dat -s 2048 -m buffered,fsync,odirect
```

Cross-check against `dd` and print extra environment info:

```bash
./run_benchmarks.sh 512 /tmp
```

## Output

Example output (4 columns: I/O mode, block size, throughput, wall time):

```
mode       block          throughput      time(s)
----       -----          ----------      -------
buffered   4.00 KiB       1234.56 MiB/s     0.041
buffered   64.00 KiB      4321.00 MiB/s     0.012
fsync      1.00 MiB        980.12 MiB/s     0.052
odirect    4.00 MiB       1500.34 MiB/s     0.034
...
```

Throughput is computed as `bytes_written / wall_time` and shown in
MiB/s (1 MiB = 1024 × 1024 bytes).

## Caveats

- Cloud / containerised filesystems (overlayfs, ext4 on a thin
  provisioned volume, …) usually report much higher *buffered* numbers
  than the raw device can sustain because data sits in RAM until the
  kernel flushes it later. The `fsync` and `odirect` modes are far
  more representative of the actual disk.
- Some filesystems (notably tmpfs, overlayfs in some kernels) do not
  support `O_DIRECT`. The benchmark reports `ERROR` for those rows; the
  remaining modes still produce valid numbers.
- This program writes to a real file in the target directory. Make
  sure you have at least `total_MiB` (default 512 MiB) of free space.
