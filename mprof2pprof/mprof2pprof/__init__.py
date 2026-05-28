"""mprof2pprof: convert UE4 MallocProfiler (.mprof) dumps to pprof JSON.

The package is split into three pieces that mirror the conversion pipeline:

* ``mprof_format``  -- pure parser for the UE4 ``.mprof`` binary container.
* ``symbolizer``    -- resolves raw ProgramCounter values to function/file/line
                       information by shelling out to ``addr2line`` /
                       ``llvm-symbolizer`` against the matching dedicated-server
                       executable.
* ``pprof_writer``  -- builds an in-memory ``profile.proto`` style model and
                       serialises it as JSON (the textual form pprof understands
                       via ``pprof -proto -json`` or any custom tooling).

The top-level CLI entry point lives in :mod:`mprof2pprof.cli`.
"""

from .mprof_format import (
    MPROF_MAGIC,
    MprofFile,
    MprofHeader,
    CallStackAddressInfo,
    CallStackInfo,
    AllocToken,
    parse_mprof,
)
from .symbolizer import Symbolizer, Symbol, ResolvedFrame
from .pprof_writer import PprofProfile, build_pprof_from_mprof

__all__ = [
    "MPROF_MAGIC",
    "MprofFile",
    "MprofHeader",
    "CallStackAddressInfo",
    "CallStackInfo",
    "AllocToken",
    "parse_mprof",
    "Symbolizer",
    "Symbol",
    "ResolvedFrame",
    "PprofProfile",
    "build_pprof_from_mprof",
]
