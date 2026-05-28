"""Generate a self-contained mprof + matching binary for the tool's demos.

What this does
==============

1. Compiles a tiny C program with ``-O0 -g`` so we have a real ELF/DWARF binary
   whose internal addresses are known (we discover them with ``nm``).
2. Picks three "interesting" symbols from that binary and treats their VMAs as
   the ``ProgramCounter`` values that the running dedicated server would have
   captured.
3. Picks a synthetic runtime base for the module so the demo also exercises the
   ``PC - BaseAddress + PreferredBaseAddress`` arithmetic that
   :class:`Symbolizer` performs.
4. Writes a complete, format-faithful mprof file containing a name table, one
   module entry, one CallStackAddressTable entry per fake PC, two callstacks
   sharing a common prefix, and a small token stream of malloc/free/realloc
   operations.

The resulting ``.mprof`` and binary are written to ``examples/out/`` so the
tests (and the user) can run the converter against them.
"""

from __future__ import annotations

import io
import os
import struct
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Tuple

THIS_DIR = Path(__file__).resolve().parent
OUT_DIR = THIS_DIR / "out"
SOURCE_C = OUT_DIR / "fakeserver.c"
BINARY = OUT_DIR / "fakeserver"
MPROF = OUT_DIR / "capture.mprof"

# Adjust if you want to plug the package source path differently.
sys.path.insert(0, str(THIS_DIR.parent))

from mprof2pprof.mprof_format import (  # noqa: E402
    MPROF_MAGIC,
    write_fstring,
)

C_SOURCE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Leaf_AllocChunk(int sz) {
    volatile void* p = malloc(sz);
    if (!p) return;
    memset((void*)p, 0xAB, sz);
    free((void*)p);
}

void Middle_PrepareBuffer(int sz) {
    Leaf_AllocChunk(sz);
}

void Outer_HandleRequest(int sz) {
    Middle_PrepareBuffer(sz);
}

int main(int argc, char** argv) {
    for (int i = 0; i < 4; ++i) Outer_HandleRequest(64 << i);
    return 0;
}
"""


def _build_binary() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    SOURCE_C.write_text(C_SOURCE)
    subprocess.run(
        ["gcc", "-O0", "-g", "-no-pie", "-fno-omit-frame-pointer",
         "-o", str(BINARY), str(SOURCE_C)],
        check=True,
    )


def _symbol_vmas(binary: Path, names: List[str]) -> Dict[str, int]:
    """Return ``{symbol: link_time_VMA}`` using ``nm``."""
    out = subprocess.check_output(["nm", str(binary)], text=True)
    found: Dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] in names:
            found[parts[-1]] = int(parts[0], 16)
    missing = [n for n in names if n not in found]
    if missing:
        raise RuntimeError(f"symbols not found in {binary}: {missing}")
    return found


def _write_mprof(binary_path: Path,
                 fake_runtime_base: int,
                 pc_chains: List[List[int]],
                 module_size: int) -> bytes:
    """Render a complete v6-shaped mprof byte stream into a buffer.

    ``pc_chains`` is a list of callstacks, each callstack a list of *file-VMA*
    program counters (root frame first, leaf last).  We translate them to the
    fake runtime PCs (``vma - preferred_base + runtime_base``) before storing
    them in the file so the converter actually has to undo the loader offset.
    """
    # The format requires the table offsets to be present in the *header*, but
    # the actual table bytes live at the *end* of the file. We therefore
    # serialise the body first, remember each table's start offset, and patch
    # the header afterwards.
    body = io.BytesIO()

    # --- Token stream ----------------------------------------------------
    # We invent some tokens so per-callstack alloc/free aggregation has work
    # to do downstream.
    next_ptr = 0x1000
    tokens: List[Tuple[int, int, int]] = []  # (cs_idx, op, size)
    # 0 = MALLOC, 1 = FREE, 2 = REALLOC
    tokens.append((0, 0, 128))   # cs 0 mallocs 128 bytes
    tokens.append((0, 0, 256))   # cs 0 mallocs 256 bytes
    tokens.append((1, 0, 64))    # cs 1 mallocs 64 bytes
    tokens.append((1, 2, 96))    # cs 1 reallocs to 96 bytes
    tokens.append((0, 1, 0))     # cs 0 frees the first ptr (128)

    live_ptrs_by_cs: Dict[int, List[int]] = {}

    def alloc_ptr() -> int:
        nonlocal next_ptr
        v = next_ptr
        next_ptr += 0x40
        return v

    for cs_idx, op, sz in tokens:
        packed = (cs_idx << 2) | op
        body.write(struct.pack("<I", packed))
        if op == 0:  # MALLOC
            p = alloc_ptr()
            live_ptrs_by_cs.setdefault(cs_idx, []).append(p)
            body.write(struct.pack("<QI", p, sz))
        elif op == 1:  # FREE
            ptrs = live_ptrs_by_cs.get(cs_idx) or [0]
            p = ptrs.pop(0) if ptrs else 0
            body.write(struct.pack("<Q", p))
        elif op == 2:  # REALLOC
            old = (live_ptrs_by_cs.get(cs_idx) or [0]).pop(0) if live_ptrs_by_cs.get(cs_idx) else 0
            new = alloc_ptr()
            live_ptrs_by_cs.setdefault(cs_idx, []).append(new)
            body.write(struct.pack("<QQI", old, new, sz))

    # EndOfStream marker (TYPE_Other | SUBTYPE_EndOfStream)
    sub = 0  # SUBTYPE_END_OF_STREAM_MARKER
    body.write(struct.pack("<I", (sub << 2) | 3))
    body.write(struct.pack("<I", 0))  # extra_len = 0

    # --- Tables: NameTable, CallStackAddressTable, CallStackTable,
    #             Modules, Metadata ---------------------------------------
    # NameTable: keep it empty (the writer-side has nothing useful to put in
    # it for the no-symbols dedicated-server case, every name field on the
    # address rows is -1 already).
    name_table_offset = body.tell()
    name_table_entries = 0

    # Build CallStackAddressTable: dedup PCs across all chains, preserve order.
    pc_to_index: Dict[int, int] = {}
    addr_rows: List[int] = []   # store the runtime PCs
    for chain in pc_chains:
        for pc in chain:
            if pc not in pc_to_index:
                pc_to_index[pc] = len(addr_rows)
                addr_rows.append(pc)

    cs_addr_offset = body.tell()
    for pc in addr_rows:
        body.write(struct.pack("<Q", pc))         # ProgramCounter
        body.write(struct.pack("<i", 0))          # ModuleIndex = 0
        body.write(struct.pack("<i", -1))         # FilenameNameIndex
        body.write(struct.pack("<i", -1))         # FunctionNameIndex
        body.write(struct.pack("<i", -1))         # LineNumber
    cs_addr_entries = len(addr_rows)

    cs_offset = body.tell()
    for chain in pc_chains:
        crc = 0
        for pc in chain:
            crc = (crc * 2654435761 + pc) & 0xFFFFFFFF
        body.write(struct.pack("<I", crc))
        body.write(struct.pack("<I", len(chain)))
        for pc in chain:
            body.write(struct.pack("<i", pc_to_index[pc]))
    cs_entries = len(pc_chains)

    # Modules table: just our single fake server binary.
    modules_offset = body.tell()
    write_fstring(body, str(binary_path))
    body.write(struct.pack("<Q", fake_runtime_base))
    body.write(struct.pack("<Q", 0x400000))  # preferred base (matches -no-pie default)
    body.write(struct.pack("<Q", module_size))
    write_fstring(body, "deadbeefcafefeedbaadf00d")  # fake build id
    module_entries = 1

    # Metadata table: a couple of arbitrary capture-time annotations.
    metadata_offset = body.tell()
    write_fstring(body, "GameName"); write_fstring(body, "FakeGameDS")
    write_fstring(body, "MachineHost"); write_fstring(body, "server-42")
    metadata_entries = 2

    body_bytes = body.getvalue()
    body_start_in_file = 0  # we'll patch in the header offset shortly

    # --- Header ----------------------------------------------------------
    header = io.BytesIO()
    header.write(struct.pack("<I", MPROF_MAGIC))
    header.write(struct.pack("<I", 6))                # VersionNumber
    write_fstring(header, "Linux")
    header.write(struct.pack("<I", 0))                # bShouldSerializeSymbolInfo
    write_fstring(header, os.path.basename(str(binary_path)))
    # Placeholders for the ten offset/count fields; we'll overwrite once we know
    # how big the header itself is.
    placeholder_pos = header.tell()
    for _ in range(10):
        header.write(struct.pack("<I", 0))

    header_size = header.tell()
    body_start_in_file = header_size

    def fix(off: int) -> int:
        return off + body_start_in_file

    header.seek(placeholder_pos)
    header.write(struct.pack(
        "<IIIIIIIIII",
        fix(name_table_offset),         name_table_entries,
        fix(cs_addr_offset),            cs_addr_entries,
        fix(cs_offset),                 cs_entries,
        fix(modules_offset),            module_entries,
        fix(metadata_offset),           metadata_entries,
    ))

    return header.getvalue() + body_bytes


def main() -> int:
    _build_binary()
    sym_names = [
        "Leaf_AllocChunk",
        "Middle_PrepareBuffer",
        "Outer_HandleRequest",
        "main",
    ]
    vmas = _symbol_vmas(BINARY, sym_names)
    print("symbol VMAs:")
    for n, v in vmas.items():
        print(f"  {n} = 0x{v:x}")

    fake_runtime_base = 0x7F0000000000
    preferred_base = 0x400000
    module_size = 0x100000

    def runtime_pc(name: str, ins_off: int = 0) -> int:
        # ``ins_off`` is the offset *inside* the function used to simulate a
        # return address (so addr2line doesn't see the symbol boundary).
        return vmas[name] + ins_off - preferred_base + fake_runtime_base

    chain_a = [  # root -> leaf
        runtime_pc("main", 0x10),
        runtime_pc("Outer_HandleRequest", 0x10),
        runtime_pc("Middle_PrepareBuffer", 0x10),
        runtime_pc("Leaf_AllocChunk", 0x10),
    ]
    chain_b = [
        runtime_pc("main", 0x20),
        runtime_pc("Outer_HandleRequest", 0x14),
        runtime_pc("Leaf_AllocChunk", 0x14),
    ]
    data = _write_mprof(
        binary_path=BINARY,
        fake_runtime_base=fake_runtime_base,
        pc_chains=[chain_a, chain_b],
        module_size=module_size,
    )
    MPROF.write_bytes(data)
    print(f"wrote {MPROF} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
