#!/usr/bin/env bash
# Bootstrap the minimal Pyroscope dev environment from a fresh git checkout.
#
# What this script does (idempotent):
#   1. Downloads the Grafana Pyroscope OSS v1.14.0 server binary into
#      ./pyroscope/bin/pyroscope (skips the download if it is already there
#      and reports the right version).
#   2. Creates the local-filesystem storage directories under ./pyroscope/data/.
#   3. Builds the Go memory demo into ./memdemo/memdemo (requires `go`).
#
# After running this script:
#   ./pyroscope/bin/pyroscope -config.file=./pyroscope/config.yaml   # start server
#   ./memdemo/memdemo ./memdemo/heap.pprof                           # produce pprof
#   ./memdemo/ingest.sh                                              # upload via curl
#
# Environment overrides:
#   PYROSCOPE_VERSION   default: 1.14.0
#   PYROSCOPE_OS        default: linux
#   PYROSCOPE_ARCH      default: auto-detected from `uname -m` (amd64 / arm64)
#   SKIP_GO_BUILD       set to 1 to skip building the Go demo
set -euo pipefail

PYROSCOPE_VERSION="${PYROSCOPE_VERSION:-1.14.0}"
PYROSCOPE_OS="${PYROSCOPE_OS:-linux}"

if [[ -z "${PYROSCOPE_ARCH:-}" ]]; then
  case "$(uname -m)" in
    x86_64|amd64) PYROSCOPE_ARCH="amd64" ;;
    aarch64|arm64) PYROSCOPE_ARCH="arm64" ;;
    *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
  esac
fi

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PYRO_BIN_DIR="${ROOT_DIR}/pyroscope/bin"
PYRO_BIN="${PYRO_BIN_DIR}/pyroscope"
PYRO_DATA_DIR="${ROOT_DIR}/pyroscope/data"

mkdir -p "$PYRO_BIN_DIR" "$PYRO_DATA_DIR/local" "$PYRO_DATA_DIR/shared"

need_download=1
if [[ -x "$PYRO_BIN" ]]; then
  installed_version="$("$PYRO_BIN" -version 2>&1 | awk '/pyroscope, version/ {print $3; exit}')"
  if [[ "$installed_version" == "$PYROSCOPE_VERSION" ]]; then
    echo "[setup] pyroscope ${PYROSCOPE_VERSION} already installed at $PYRO_BIN"
    need_download=0
  else
    echo "[setup] replacing existing pyroscope binary (was '${installed_version}', want '${PYROSCOPE_VERSION}')"
  fi
fi

if [[ "$need_download" == "1" ]]; then
  tarball="pyroscope_${PYROSCOPE_VERSION}_${PYROSCOPE_OS}_${PYROSCOPE_ARCH}.tar.gz"
  url="https://github.com/grafana/pyroscope/releases/download/v${PYROSCOPE_VERSION}/${tarball}"
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' EXIT

  echo "[setup] downloading ${url}"
  curl -fsSL -o "${tmpdir}/${tarball}" "$url"

  echo "[setup] extracting"
  tar -xzf "${tmpdir}/${tarball}" -C "$tmpdir" pyroscope
  install -m 0755 "${tmpdir}/pyroscope" "$PYRO_BIN"
  echo "[setup] installed -> $PYRO_BIN"
fi

"$PYRO_BIN" -version 2>&1 | awk '/^pyroscope, version/ {print; exit}' || true

if [[ "${SKIP_GO_BUILD:-0}" != "1" ]]; then
  if command -v go >/dev/null 2>&1; then
    echo "[setup] building memdemo"
    (cd "${ROOT_DIR}/memdemo" && go build -o memdemo .)
    echo "[setup] built -> ${ROOT_DIR}/memdemo/memdemo"
  else
    echo "[setup] WARNING: 'go' not found in PATH; skipping memdemo build" >&2
  fi
fi

cat <<EOF

[setup] done.

Next steps:
  1. Start the server:
       ${PYRO_BIN} -config.file=${ROOT_DIR}/pyroscope/config.yaml
  2. In another shell, generate and upload a heap pprof:
       ${ROOT_DIR}/memdemo/memdemo ${ROOT_DIR}/memdemo/heap.pprof
       ${ROOT_DIR}/memdemo/ingest.sh
  3. Open http://127.0.0.1:4040 and switch to
     Single View -> memdemo.inuse_space -> memory:inuse_space.
EOF
