# Pyroscope server (v1.14.0)

This directory holds the runtime files for the embedded Pyroscope server used
by this dev environment.

* `config.yaml` -- the single-node, filesystem-backed configuration. Tracked in git.
* `bin/pyroscope` -- the v1.14.0 linux/amd64 server binary. **Not tracked in git**
  (download with the script below or grab the tarball from the
  [release page](https://github.com/grafana/pyroscope/releases/tag/v1.14.0)).
* `data/` -- per-ingester local data and the long-term filesystem bucket store.
  Created on first run, ignored by git.
* `server.log` -- captured stdout when started in the background, ignored by git.

## Download the binary

The easiest way is to run the top-level bootstrap script — it picks the right
OS/arch tarball, extracts it, makes `pyroscope/data/{local,shared}` and (if
`go` is on `PATH`) also builds the Go demo:

```bash
./setup.sh                          # from the repository root
PYROSCOPE_VERSION=1.14.0 ./setup.sh # pin a specific version
PYROSCOPE_ARCH=arm64 ./setup.sh     # cross-arch override
```

Equivalent manual steps:

```bash
mkdir -p pyroscope/bin pyroscope/data/local pyroscope/data/shared
curl -fsSL -o /tmp/pyroscope.tar.gz \
  https://github.com/grafana/pyroscope/releases/download/v1.14.0/pyroscope_1.14.0_linux_amd64.tar.gz
tar -xzf /tmp/pyroscope.tar.gz -C /tmp pyroscope
install -m 0755 /tmp/pyroscope pyroscope/bin/pyroscope
pyroscope/bin/pyroscope -version
```

## Start the server

```bash
pyroscope/bin/pyroscope -config.file=pyroscope/config.yaml
```

See the top-level [`README.md`](../README.md) for the full walkthrough.
