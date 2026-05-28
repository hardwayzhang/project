"""Resolve raw ``ProgramCounter`` values into ``function/file:line`` symbols.

Why this is non-trivial
=======================

The dedicated-server captures call stacks with
``FPlatformStackWalk::CaptureStackBackTrace`` which returns absolute *runtime*
addresses. Those addresses depend on where the OS loader actually placed each
binary at startup, which on modern systems is randomised (ASLR / PIE). The
binary itself however has a stable *link-time* layout described by its
ELF/PE/DWARF/PDB metadata.

To go from ``PC`` to ``"MyClass::Foo()  Engine/Core/Foo.cpp:42"`` we need both
inputs:

1. The unsymbolised ``PC`` and the ``ModuleIndex`` (the mprof file gives us
   these).
2. The matching binary that was running on the server when the dump was made
   plus its runtime ``BaseAddress`` (mprof's ``ModulesTable`` records the
   runtime ``BaseAddress`` and ``PreferredBaseAddress``).

The arithmetic is then::

    binary_offset = PC - Module.BaseAddress
    file_vma      = binary_offset + Module.PreferredBaseAddress

For a non-PIE Linux ELF ``PreferredBaseAddress`` matches the link-time
``p_vaddr`` of the first ``PT_LOAD`` segment, so ``file_vma`` is the value
``addr2line`` / ``llvm-symbolizer`` expect for that binary.

Symbolizer backends
-------------------
The class wraps either ``llvm-symbolizer`` (preferred -- handles inlining and
PE/COFF + ELF) or GNU ``addr2line`` (fallback on systems without LLVM). In
batch mode both tools read addresses one-per-line on stdin and stream the
``function\nfile:line`` pairs back on stdout, which keeps process startup cost
amortised across thousands of PCs. We pass ``-a`` so each result is prefixed
with the originating address -- that lets us chunk the output reliably even
when no inlining occurred and there are no blank-line terminators.

If no debug info is available the resolver gracefully degrades to ``module +
0xoffset`` -- the resulting pprof file is still useful (you can flame-graph by
module / PC bucket) and can be re-symbolised later by anyone holding the
matching binary.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Tuple


@dataclass(frozen=True)
class Symbol:
    function: str
    file: str
    line: int


@dataclass
class ResolvedFrame:
    """One stack frame, possibly inlined.

    A single PC can expand to multiple frames when the compiler inlined call
    sites. ``symbols`` lists them in *outer-first* order, matching the order
    that pprof's ``Location.line`` expects.
    """

    pc: int
    module: str
    module_offset: int
    symbols: List[Symbol]


class Symbolizer:
    """Address resolver shelling out to ``llvm-symbolizer`` / ``addr2line``."""

    def __init__(
        self,
        binary_path: Optional[str] = None,
        module_overrides: Optional[Dict[str, str]] = None,
        tool: Optional[str] = None,
    ) -> None:
        """Args:

        ``binary_path``: path to the dedicated-server executable (used for the
        primary module, i.e. ``ModuleIndex == 0``).

        ``module_overrides``: optional ``{module_name: path_to_binary}`` map
        for resolving secondary modules (shared libraries, plugins). The names
        should match the ``ModuleInfo.name`` field recorded in the mprof.

        ``tool``: explicit path/name of the symbolizer tool. If ``None`` we
        auto-detect ``llvm-symbolizer`` and fall back to ``addr2line``.
        """
        self._module_overrides = dict(module_overrides or {})
        if binary_path:
            self._module_overrides.setdefault(
                os.path.basename(binary_path), binary_path
            )
            self._module_overrides.setdefault("", binary_path)

        self._tool, self._is_llvm = self._pick_tool(tool)
        self._sym_cache: Dict[Tuple[str, int], List[Symbol]] = {}

    # ----- public API -----------------------------------------------------

    def _binary_for(self, module_name: str) -> Optional[str]:
        # Match either by exact ModuleInfo.name or by basename to be friendly
        # with absolute vs. relative path differences between capture and
        # symbolize machines.
        b = self._module_overrides.get(module_name)
        if b is None:
            b = self._module_overrides.get(os.path.basename(module_name))
        if b is None:
            b = self._module_overrides.get("")
        return b

    def resolve_pc(self, pc: int, module_name: str,
                   module_offset: int) -> ResolvedFrame:
        binary = self._binary_for(module_name)
        if binary is None or not os.path.exists(binary):
            return self._unresolved(pc, module_name, module_offset)

        key = (binary, module_offset)
        cached = self._sym_cache.get(key)
        if cached is None:
            cached = self._run_tool(binary, [module_offset])[0]
            self._sym_cache[key] = cached
        return ResolvedFrame(
            pc=pc,
            module=module_name,
            module_offset=module_offset,
            symbols=cached,
        )

    def resolve_batch(self, requests: Iterable[Tuple[int, str, int]]
                      ) -> List[ResolvedFrame]:
        """Batch variant: feeds all unresolved PCs to one subprocess per binary."""
        order: List[Tuple[int, str, int]] = list(requests)

        # Bucket by binary, skipping anything already cached or unresolvable.
        per_binary: Dict[str, List[int]] = {}
        for _, mod_name, off in order:
            binary = self._binary_for(mod_name)
            if binary is None or not os.path.exists(binary):
                continue
            if (binary, off) in self._sym_cache:
                continue
            per_binary.setdefault(binary, []).append(off)

        for binary, offs in per_binary.items():
            uniq = sorted(set(offs))
            syms_list = self._run_tool(binary, uniq)
            for off, syms in zip(uniq, syms_list):
                self._sym_cache[(binary, off)] = syms

        return [self.resolve_pc(pc, mod, off) for pc, mod, off in order]

    # ----- internal -------------------------------------------------------

    def _unresolved(self, pc: int, module: str, off: int) -> ResolvedFrame:
        return ResolvedFrame(
            pc=pc,
            module=module,
            module_offset=off,
            symbols=[Symbol(
                function=f"{os.path.basename(module) or '<unknown>'}+0x{off:x}",
                file="",
                line=0,
            )],
        )

    def _pick_tool(self, tool: Optional[str]) -> Tuple[str, bool]:
        if tool:
            return tool, "llvm-symbolizer" in os.path.basename(tool)
        llvm = shutil.which("llvm-symbolizer")
        if llvm:
            return llvm, True
        addr2line = shutil.which("addr2line")
        if addr2line:
            return addr2line, False
        raise RuntimeError(
            "neither llvm-symbolizer nor addr2line is available on PATH; "
            "install one or pass --symbolizer-tool to mprof2pprof"
        )

    def _run_tool(self, binary: str, offsets: List[int]) -> List[List[Symbol]]:
        if not offsets:
            return []
        # ``-a`` makes both tools prefix each result with the address that
        # produced it, which is what we use to chunk the output back into one
        # entry per input PC. ``-i`` enables inline-frame expansion; ``-C``
        # demangles C++; ``-f`` adds the function name above each location.
        if self._is_llvm:
            args = [self._tool, "-e", binary, "-f", "-C", "-i",
                    "-a", "--output-style=GNU"]
        else:
            args = [self._tool, "-e", binary, "-f", "-C", "-i", "-a"]
        stdin = "\n".join(f"0x{off:x}" for off in offsets) + "\n"
        try:
            proc = subprocess.run(
                args, input=stdin, capture_output=True, text=True,
                check=True, timeout=120,
            )
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                f"symbolizer {self._tool} failed for {binary}: {exc.stderr}"
            ) from exc
        return _parse_symbolizer_output(proc.stdout, len(offsets))


# ---------------------------------------------------------------------------
# Output parser
# ---------------------------------------------------------------------------

_ADDR_LINE_PREFIX = "0x"


def _parse_symbolizer_output(stdout: str, expected_addrs: int
                             ) -> List[List[Symbol]]:
    """Parse ``addr2line``/``llvm-symbolizer`` ``-a -f -i`` output.

    Layout, given one or more input addresses::

        0xADDR1
        function_outer
        file:line
        function_inlined         <- present only if compiler inlined a call site
        file:line
        0xADDR2
        function_only
        file:line

    Lines starting with ``0x`` always mark the start of a new address's frame
    group; everything between two ``0x`` headers (or between the last header
    and EOF) is the inline-expanded chain for that address, in *outer-first*
    order (matching the way pprof wants ``Location.line`` to be ordered).
    """
    results: List[List[Symbol]] = []
    current: List[Symbol] = []
    have_header = False

    def flush() -> None:
        nonlocal current
        if have_header:
            results.append(current)
        current = []

    lines = stdout.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith(_ADDR_LINE_PREFIX):
            flush()
            have_header = True
            i += 1
            continue
        if line == "":
            i += 1
            continue
        fn = line
        loc = lines[i + 1] if i + 1 < len(lines) else "??:0"
        i += 2
        file, _, line_str = loc.rpartition(":")
        # Strip trailing ``(discriminator N)`` / column suffix.
        line_str = line_str.split(" ", 1)[0].split(":", 1)[0]
        try:
            line_num = int(line_str) if line_str not in ("?", "") else 0
        except ValueError:
            line_num = 0
        current.append(Symbol(function=fn, file=file or "??", line=line_num))
    flush()

    while len(results) < expected_addrs:
        results.append([Symbol(function="??", file="??", line=0)])
    return results[:expected_addrs]
