# Minimal Pyroscope dev environment (Linux, local-filesystem)

This project sets up a self-contained Pyroscope (Grafana Pyroscope v1.14.0) server
on a single Linux host with local-filesystem storage, plus a tiny Go program that
generates a heap pprof file and a `curl`-based uploader that pushes it to the
server. After running the uploader you can browse to <http://localhost:4040> and
inspect the `memory:inuse_space` flame graph for the demo app.

```
┌──────────────────────┐    heap.pprof         POST /ingest
│  ./memdemo/memdemo   │  ───────────────▶  ./memdemo/ingest.sh
│  (Go test program)   │                       │
└──────────────────────┘                       ▼
                                       ┌────────────────────────┐
                                       │ pyroscope (all-in-one) │
                                       │  http://0.0.0.0:4040   │
                                       │  storage: filesystem    │
                                       └────────────────────────┘
```

## Layout

What's in git (tracked):

```
.
├── README.md            # this file
├── setup.sh             # one-shot bootstrap: downloads binary, makes dirs, builds memdemo
├── memdemo/
│   ├── go.mod           # Go module (no third-party deps)
│   ├── main.go          # allocates ~150 MiB and writes heap.pprof
│   └── ingest.sh        # uploads heap.pprof to Pyroscope with curl
└── pyroscope/
    ├── README.md
    └── config.yaml      # single-node, filesystem-backed Pyroscope config
```

What's created at runtime (gitignored, see `.gitignore`):

```
pyroscope/bin/pyroscope  # v1.14.0 server binary, downloaded by setup.sh
pyroscope/data/          # local pyroscopedb + filesystem-bucket blocks
pyroscope/server.log     # captured stdout when started in the background
memdemo/memdemo          # compiled Go binary
memdemo/heap.pprof       # heap profile produced by running ./memdemo
```

## 0. One-shot bootstrap

After cloning the repo, just run:

```bash
./setup.sh
```

This will (idempotently):

1. Download the Grafana Pyroscope OSS **v1.14.0** server binary that matches
   your OS/arch into `pyroscope/bin/pyroscope` (skipped if the right version is
   already there).
2. Create the local storage directories `pyroscope/data/local/` and
   `pyroscope/data/shared/`.
3. Build `memdemo/memdemo` with the Go toolchain (skipped with
   `SKIP_GO_BUILD=1`, or if `go` is not in `PATH`).

Environment overrides: `PYROSCOPE_VERSION`, `PYROSCOPE_OS`, `PYROSCOPE_ARCH`.

If you want to install the binary by hand instead, see
[`pyroscope/README.md`](pyroscope/README.md) for the equivalent `curl + tar`
commands.

## 1. Start the Pyroscope server

The server is a single static binary. With the bundled config it listens on
`0.0.0.0:4040` and stores everything under `pyroscope/data/`.

```bash
# Start in the foreground (Ctrl-C to stop)
./pyroscope/bin/pyroscope -config.file=./pyroscope/config.yaml

# Or in the background, logging to pyroscope/server.log
nohup ./pyroscope/bin/pyroscope -config.file=./pyroscope/config.yaml \
  > pyroscope/server.log 2>&1 &

# Smoke test
curl -fsS http://127.0.0.1:4040/ready    # -> "ready"
curl -fsS -o /dev/null -w "%{http_code}\n" http://127.0.0.1:4040/   # -> 200
```

Open <http://localhost:4040> (or `http://<host-ip>:4040` from another machine)
in a browser to see the Pyroscope UI.

### Key config knobs

`pyroscope/config.yaml` keeps the dev setup as small as possible:

* `target: all` runs every Pyroscope component (distributor, ingester, querier,
  store-gateway, compactor, query-scheduler) inside one process.
* `storage.backend: filesystem` + `storage.filesystem.dir` keeps the long-term
  block store on the local disk -- no S3/GCS/Azure required.
* `pyroscopedb.data_path` is the per-ingester local data directory.
* `multitenancy_enabled: false` so requests work without an `X-Scope-OrgId`
  header (the implicit tenant is `anonymous`).
* `self_profiling.disable_push: true` avoids the server profiling itself.
* `analytics.reporting_enabled: false` disables the usage telemetry beacon.

## 2. Build & run the Go memory demo

`setup.sh` already builds `memdemo/memdemo`. If you skipped that step (or
changed `main.go`), you can rebuild manually:

```bash
cd memdemo
go build -o memdemo .
./memdemo ./heap.pprof
```

`memdemo` deliberately allocates from four different call paths so that the
flame graph has interesting structure:

| Function                      | What it allocates                                |
|-------------------------------|--------------------------------------------------|
| `allocateBigBuffers`          | 20 x 4 MiB byte slices (~80 MiB)                 |
| `allocateManySmallBuffers`    | 50 000 x 1 KiB byte slices (~50 MiB)             |
| `buildStringIndex`            | a 20 000-entry `map[string][]byte` (~10 MiB)     |
| `deepCaller -> allocateBigBuffers` | 5 x 2 MiB through 8 levels of recursion (~10 MiB) |

After running, `heap.pprof` is the standard Go heap profile (i.e. the same
content as `pprof.Lookup("heap").WriteTo(...)`). It contains all four memory
sample types: `alloc_objects`, `alloc_space`, `inuse_objects`, `inuse_space`.

Local inspection:

```bash
go tool pprof -top -unit=bytes -inuse_space ./heap.pprof
```

## 3. Push the pprof to Pyroscope with `curl`

```bash
cd memdemo
./ingest.sh                     # defaults: memdemo, ./heap.pprof, http://127.0.0.1:4040
./ingest.sh myapp ./heap.pprof http://127.0.0.1:4040
```

`ingest.sh` uses the classic Pyroscope `/ingest` endpoint. The trailing
component of the `name` query parameter (after the last `.`) is interpreted by
the server as the pprof sample type to extract. We therefore POST the same
file once per memory sample type:

```bash
NOW=$(date +%s)
curl -sS -X POST \
  "http://127.0.0.1:4040/ingest?name=memdemo.inuse_space&from=$((NOW-10))&until=${NOW}&spyName=gospy&format=pprof" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @heap.pprof
```

Repeat with `memdemo.alloc_space`, `memdemo.inuse_objects`,
`memdemo.alloc_objects`. The server then exposes four series under the same
application name. You can verify with:

```bash
curl -sS http://127.0.0.1:4040/querier.v1.QuerierService/ProfileTypes \
  -H "Content-Type: application/json" -d '{}'
```

Expected output (abridged):

```json
{"profileTypes":[
  {"ID":"memory:alloc_objects:count:space:bytes", ... },
  {"ID":"memory:alloc_space:bytes:space:bytes", ... },
  {"ID":"memory:inuse_objects:count:space:bytes", ... },
  {"ID":"memory:inuse_space:bytes:space:bytes", ... }
]}
```

## 4. View the flame graph in the browser

1. Open <http://localhost:4040>.
2. In the top bar pick **Service**: `memdemo`.
3. **Profile type**: `memory - inuse_space` (or any of the other three).
4. Make sure the time range covers the `from/until` window used by `ingest.sh`
   (the script uses `now-10s ... now`; "Last 5 minutes" is a safe default).

The flame graph should be dominated by `main.allocateBigBuffers`,
`main.allocateManySmallBuffers`, `main.buildStringIndex` and the `deepCaller`
chain, mirroring the `pprof -top` output above.
