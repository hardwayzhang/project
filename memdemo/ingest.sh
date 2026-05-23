#!/usr/bin/env bash
# Upload a Go heap pprof file to a Pyroscope server using the /ingest HTTP API.
#
# Pyroscope's classic ingest convention extracts the profile type from the
# trailing component of the application name (after the last '.'). For example,
# `memdemo.inuse_space` is stored as application `memdemo`, metric `memory`,
# sample type `inuse_space`, unit `bytes`. We therefore POST the same pprof
# once per heap sample type so the four series become visible in the UI.
#
# Usage:
#   ./ingest.sh [APP_NAME] [PPROF_FILE] [SERVER_URL]
#
# Defaults:
#   APP_NAME    = memdemo
#   PPROF_FILE  = ./heap.pprof
#   SERVER_URL  = http://127.0.0.1:4040
set -euo pipefail

APP_NAME="${1:-memdemo}"
PPROF_FILE="${2:-$(dirname "$0")/heap.pprof}"
SERVER_URL="${3:-http://127.0.0.1:4040}"

if [[ ! -f "$PPROF_FILE" ]]; then
  echo "pprof file not found: $PPROF_FILE" >&2
  exit 1
fi

UNTIL="$(date +%s)"
FROM="$((UNTIL - 10))"

post_one() {
  local sample_type="$1"
  local full_name="${APP_NAME}.${sample_type}"
  local url="${SERVER_URL}/ingest?name=${full_name}&from=${FROM}&until=${UNTIL}&spyName=gospy&format=pprof"
  printf '%-15s -> POST %s ... ' "${sample_type}" "${full_name}"
  http_code=$(curl -sS -o /tmp/ingest_resp -w '%{http_code}' -X POST "$url" \
    -H 'Content-Type: application/octet-stream' \
    --data-binary "@${PPROF_FILE}")
  if [[ "$http_code" == "200" ]]; then
    echo "OK (HTTP 200)"
  else
    echo "FAIL (HTTP ${http_code})"
    cat /tmp/ingest_resp
    echo
    exit 1
  fi
}

post_one "inuse_space"
post_one "inuse_objects"
post_one "alloc_space"
post_one "alloc_objects"

echo
echo "All sample types ingested for application: ${APP_NAME}"
echo "Open the UI:    ${SERVER_URL}"
echo "Profile type:   memory : inuse_space (and alloc_space / inuse_objects / alloc_objects)"
echo "Application:    ${APP_NAME}"
