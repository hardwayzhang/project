#!/usr/bin/env bash
# run_benchmarks.sh
#
# Build disk_write_test, run it, and also compare its numbers against the
# standard `dd` utility (which essentially does the same thing as our
# "buffered" and "fsync" modes).
#
# Usage:
#   ./run_benchmarks.sh [size_in_MiB] [test_dir]
#
#   size_in_MiB - total bytes to write per run (default: 512)
#   test_dir    - directory where the temporary file is created
#                 (default: current directory).  Use this to test a
#                 different disk / mount point.
set -euo pipefail

SIZE_MIB=${1:-512}
TEST_DIR=${2:-.}
TEST_FILE="${TEST_DIR%/}/disk_write_test.dat"

cd "$(dirname "$0")"

echo "=========================================="
echo " Disk write benchmark"
echo "=========================================="
echo " size per run : ${SIZE_MIB} MiB"
echo " test file    : ${TEST_FILE}"
echo " test dir df  :"
df -hT "${TEST_DIR}" | sed 's/^/   /'
if command -v lsblk >/dev/null 2>&1; then
  echo " block devices:"
  lsblk -d -o NAME,SIZE,ROTA,TYPE,MODEL 2>/dev/null | sed 's/^/   /' || true
fi
echo

echo "--- Building ---"
make -s all
echo

echo "--- disk_write_test ---"
./disk_write_test -f "${TEST_FILE}" -s "${SIZE_MIB}"
echo

if command -v dd >/dev/null 2>&1; then
  BS_BYTES=$((1024 * 1024))           # 1 MiB block
  COUNT=$SIZE_MIB

  echo "--- dd: buffered (no fsync) ---"
  dd if=/dev/zero of="${TEST_FILE}" bs=${BS_BYTES} count=${COUNT} \
     conv=notrunc 2>&1 | sed 's/^/   /'
  rm -f "${TEST_FILE}"
  echo

  echo "--- dd: with conv=fdatasync ---"
  dd if=/dev/zero of="${TEST_FILE}" bs=${BS_BYTES} count=${COUNT} \
     conv=fdatasync 2>&1 | sed 's/^/   /'
  rm -f "${TEST_FILE}"
  echo

  echo "--- dd: with oflag=direct (bypass page cache) ---"
  if dd if=/dev/zero of="${TEST_FILE}" bs=${BS_BYTES} count=${COUNT} \
       oflag=direct 2>&1 | sed 's/^/   /'; then
    :
  else
    echo "   (O_DIRECT not supported on this filesystem)"
  fi
  rm -f "${TEST_FILE}"
  echo
else
  echo "dd not found, skipping cross check"
fi

echo "Done."
