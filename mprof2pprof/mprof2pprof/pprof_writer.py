"""Build a ``profile.proto``-shaped object and serialise it as JSON.

pprof reference: https://github.com/google/pprof/blob/main/proto/profile.proto

The JSON we emit follows the protobuf-to-JSON mapping (proto3 ``json_name``
semantics) which is what ``pprof -proto -json`` produces and what every other
custom pprof consumer expects::

    {
      "stringTable": [...],
      "sampleType":  [ {"type": idx, "unit": idx}, ... ],
      "sample":      [ {"locationId":[...], "value":[...], "label":[...]}, ...],
      "mapping":     [ {"id":1, "memoryStart":..., "filename": idx, ...} ],
      "location":    [ {"id":1, "mappingId":1, "address":..., "line":[...] } ],
      "function":    [ {"id":1, "name": idx, "systemName": idx,
                        "filename": idx, "startLine":0 } ],
      "periodType":  {"type": idx, "unit": idx},
      "period":      1,
      "timeNanos":   ...,
      "defaultSampleType": idx
    }

Index 0 of ``stringTable`` must be the empty string per the proto contract.

Mapping mprof -> pprof
======================

* Sample types: we emit two values per sample matching ``go tool pprof``'s
  default heap profile schema::

      sampleType[0] = (alloc_objects, count)
      sampleType[1] = (alloc_space,   bytes)

  Each unique callstack in the mprof contributes one ``Sample`` whose ``value``
  is ``[num_allocations, sum_of_sizes]``. Frees are netted into a separate
  ``inuse_objects/inuse_space`` pair so users can flip between perspectives in
  the pprof UI.

* Locations: one per ``CallStackAddressInfo``. ``address`` is the raw PC,
  ``mappingId`` points at the owning module, ``line[]`` carries one entry per
  inlined frame (leaf first, as required by pprof).

* Functions: deduplicated by ``(name, file)``. ``name`` is the demangled C++
  symbol; ``systemName`` keeps the mangled form when available.

* Mappings: one per ``ModuleInfo``. We set ``memoryStart`` / ``memoryLimit``
  to the runtime address range (so pprof can still relate raw PCs to a module
  even when locations carry symbolised ``line[]``), and ``filename`` to the
  module's basename which is what pprof will display.
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Tuple

from .mprof_format import MprofFile, PayloadType
from .symbolizer import ResolvedFrame, Symbolizer


# ---------------------------------------------------------------------------
# Lightweight in-memory pprof representation.
# ---------------------------------------------------------------------------

@dataclass
class PprofProfile:
    string_table: List[str] = field(default_factory=lambda: [""])
    sample_types: List[Tuple[int, int]] = field(default_factory=list)
    samples: List[dict] = field(default_factory=list)
    mappings: List[dict] = field(default_factory=list)
    locations: List[dict] = field(default_factory=list)
    functions: List[dict] = field(default_factory=list)
    period_type: Tuple[int, int] = (0, 0)
    period: int = 1
    time_nanos: int = 0
    default_sample_type: int = 0

    def intern(self, s: str) -> int:
        # Linear scan via dict cache to keep the string table compact.
        idx = self._str_index.get(s)
        if idx is None:
            idx = len(self.string_table)
            self.string_table.append(s)
            self._str_index[s] = idx
        return idx

    def __post_init__(self):
        self._str_index: Dict[str, int] = {s: i
                                           for i, s in enumerate(self.string_table)}

    def to_json(self, *, indent: int = 2) -> str:
        doc = {
            "stringTable": self.string_table,
            "sampleType": [{"type": t, "unit": u} for t, u in self.sample_types],
            "sample": self.samples,
            "mapping": self.mappings,
            "location": self.locations,
            "function": self.functions,
            "periodType": {"type": self.period_type[0],
                           "unit": self.period_type[1]},
            "period": self.period,
            "timeNanos": self.time_nanos,
            "defaultSampleType": self.default_sample_type,
        }
        return json.dumps(doc, indent=indent, ensure_ascii=False)


# ---------------------------------------------------------------------------
# Conversion pipeline.
# ---------------------------------------------------------------------------

def build_pprof_from_mprof(mprof: MprofFile,
                           symbolizer: Symbolizer,
                           ) -> PprofProfile:
    """Materialise a :class:`PprofProfile` from a parsed mprof."""
    p = PprofProfile()
    p.time_nanos = int(time.time() * 1_000_000_000)

    # ---- Sample types: heap profile, four channels --------------------
    p.sample_types = [
        (p.intern("alloc_objects"), p.intern("count")),
        (p.intern("alloc_space"),   p.intern("bytes")),
        (p.intern("inuse_objects"), p.intern("count")),
        (p.intern("inuse_space"),   p.intern("bytes")),
    ]
    p.period_type = (p.intern("space"), p.intern("bytes"))
    p.default_sample_type = p.sample_types[1][0]  # "alloc_space"

    # ---- Mappings: one per ModuleInfo (1-based ids) -------------------
    module_id_for_index: Dict[int, int] = {}
    for i, mod in enumerate(mprof.modules):
        mid = len(p.mappings) + 1
        module_id_for_index[i] = mid
        p.mappings.append({
            "id": mid,
            "memoryStart": mod.base_address,
            "memoryLimit": mod.base_address + mod.size,
            "fileOffset": 0,
            "filename": p.intern(mod.name),
            "buildId":  p.intern(mod.build_id) if mod.build_id else 0,
            "hasFunctions": True,
            "hasFilenames": True,
            "hasLineNumbers": True,
            "hasInlineFrames": True,
        })

    # ---- Resolve every unique PC in one batch -------------------------
    resolve_requests: List[Tuple[int, str, int]] = []
    for addr in mprof.callstack_addresses:
        mod = mprof.modules[addr.module_index] \
            if 0 <= addr.module_index < len(mprof.modules) else None
        mod_name = mod.name if mod else ""
        if mod is not None:
            offset = addr.program_counter - mod.base_address \
                     + mod.preferred_base_address
        else:
            offset = addr.program_counter
        resolve_requests.append((addr.program_counter, mod_name, offset))
    resolved: List[ResolvedFrame] = symbolizer.resolve_batch(resolve_requests)

    # ---- Functions + Locations ----------------------------------------
    function_id_by_key: Dict[Tuple[str, str], int] = {}
    def function_id(name: str, filename: str) -> int:
        key = (name, filename)
        fid = function_id_by_key.get(key)
        if fid is None:
            fid = len(p.functions) + 1
            function_id_by_key[key] = fid
            p.functions.append({
                "id": fid,
                "name": p.intern(name),
                "systemName": p.intern(name),
                "filename": p.intern(filename),
                "startLine": 0,
            })
        return fid

    location_ids: List[int] = []
    for addr_idx, addr in enumerate(mprof.callstack_addresses):
        frame = resolved[addr_idx]
        lines = []
        for sym in frame.symbols:
            lines.append({
                "functionId": function_id(sym.function, sym.file),
                "line": sym.line,
            })
        loc_id = len(p.locations) + 1
        location_ids.append(loc_id)
        mid = module_id_for_index.get(addr.module_index, 0)
        loc = {
            "id": loc_id,
            "address": addr.program_counter,
            "line": lines,
            "isFolded": False,
        }
        if mid:
            loc["mappingId"] = mid
        p.locations.append(loc)

    # ---- Samples: one per unique callstack ----------------------------
    # Pre-aggregate the token stream into per-callstack tallies.
    @dataclass
    class _Tally:
        alloc_objects: int = 0
        alloc_space:   int = 0
        inuse_objects: int = 0
        inuse_space:   int = 0

    tallies: Dict[int, _Tally] = {}
    live: Dict[int, Tuple[int, int]] = {}  # ptr -> (cs_index, size)

    for tok in mprof.tokens:
        if tok.op == PayloadType.MALLOC:
            t = tallies.setdefault(tok.callstack_index, _Tally())
            t.alloc_objects += 1
            t.alloc_space += tok.size
            t.inuse_objects += 1
            t.inuse_space += tok.size
            live[tok.pointer] = (tok.callstack_index, tok.size)
        elif tok.op == PayloadType.FREE:
            origin = live.pop(tok.pointer, None)
            if origin is not None:
                cs_idx, sz = origin
                t = tallies.get(cs_idx)
                if t is not None:
                    t.inuse_objects -= 1
                    t.inuse_space -= sz
        elif tok.op == PayloadType.REALLOC:
            origin = live.pop(tok.old_pointer, None)
            if origin is not None:
                old_cs, old_sz = origin
                t = tallies.get(old_cs)
                if t is not None:
                    t.inuse_objects -= 1
                    t.inuse_space -= old_sz
            t = tallies.setdefault(tok.callstack_index, _Tally())
            t.alloc_objects += 1
            t.alloc_space += tok.size
            t.inuse_objects += 1
            t.inuse_space += tok.size
            live[tok.new_pointer] = (tok.callstack_index, tok.size)

    for cs_idx, cs in enumerate(mprof.callstacks):
        t = tallies.get(cs_idx)
        if t is None:
            continue
        # pprof requires location_id ordered leaf -> root. UE4 stores the
        # callstack root-most-first (frame[0] = entry point), so reverse.
        loc_ids = [location_ids[i] for i in cs.address_indices][::-1]
        p.samples.append({
            "locationId": loc_ids,
            "value": [t.alloc_objects, t.alloc_space,
                      t.inuse_objects, t.inuse_space],
            "label": [],
        })

    # ---- Stash mprof metadata as pprof Labels on a synthetic sample ----
    if mprof.metadata:
        meta_labels = [{
            "key": p.intern(k),
            "str": p.intern(v),
            "num": 0,
            "numUnit": 0,
        } for k, v in mprof.metadata]
        p.samples.append({
            "locationId": [],
            "value": [0, 0, 0, 0],
            "label": meta_labels,
        })

    return p
