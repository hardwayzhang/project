"""Command-line entry point for the mprof -> pprof JSON converter.

Example::

    python -m mprof2pprof.cli capture.mprof \\
        --binary ./LinuxServer/MyGameServer \\
        --module-binary libUE4Engine.so=./libs/libUE4Engine.so \\
        --output capture.pprof.json
"""

from __future__ import annotations

import argparse
import sys
from typing import Dict, List

from .mprof_format import parse_mprof
from .pprof_writer import build_pprof_from_mprof
from .symbolizer import Symbolizer


def _parse_module_overrides(values: List[str]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for v in values or []:
        if "=" not in v:
            raise SystemExit(f"--module-binary requires NAME=PATH, got {v!r}")
        name, path = v.split("=", 1)
        out[name] = path
    return out


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Convert a UE4 mprof dump into pprof-compatible JSON.",
    )
    parser.add_argument("mprof", help="path to the .mprof file")
    parser.add_argument("-o", "--output", required=True,
                        help="output path for the pprof JSON file")
    parser.add_argument("--binary", help=(
        "path to the dedicated-server executable used to symbolize "
        "ModuleIndex 0 (default for unmatched modules)."))
    parser.add_argument(
        "--module-binary", action="append", default=[],
        metavar="NAME=PATH",
        help=("override the binary used for module NAME (matched against the "
              "ModuleInfo.name recorded in the mprof). Repeatable."))
    parser.add_argument("--symbolizer-tool", default=None,
                        help="explicit path to llvm-symbolizer or addr2line")
    parser.add_argument("--indent", type=int, default=2,
                        help="JSON indent (0 disables pretty printing)")
    parser.add_argument("--summary", action="store_true",
                        help="print a one-line summary of the converted profile")
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    mprof = parse_mprof(args.mprof)
    overrides = _parse_module_overrides(args.module_binary)
    symbolizer = Symbolizer(
        binary_path=args.binary,
        module_overrides=overrides,
        tool=args.symbolizer_tool,
    )
    profile = build_pprof_from_mprof(mprof, symbolizer)
    indent = args.indent if args.indent > 0 else None
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(profile.to_json(indent=indent))

    if args.summary:
        live_callstacks = len(mprof.callstacks)
        unique_pcs = len(mprof.callstack_addresses)
        tokens = len(mprof.tokens)
        print(
            f"wrote {args.output}: {tokens} tokens, {live_callstacks} callstacks, "
            f"{unique_pcs} unique PCs, {len(profile.functions)} symbolised functions",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
