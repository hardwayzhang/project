"""UE4 ``MallocProfiler`` (.mprof) binary format -- parser & data model.

Background
==========

``Engine/Source/Runtime/Core/Private/ProfilingDebugging/MallocProfiler.cpp``
serialises the dump using ``FArchive`` in little-endian order. The on-disk
layout looks like::

    +-----------------+
    | Header (fixed   |  written at file offset 0; some offset fields point
    | + FString fields|  forward to tables that are appended at the *end*
    | + table offsets)|  after the live token stream finishes.
    +-----------------+
    | Token stream    |  variable length, ends with SUBTYPE_EndOfStreamMarker
    +-----------------+
    | NameTable       |  TArray<FString>  (one string per name index)
    +-----------------+
    | CallStackAddress|  TArray<FCallStackAddressInfo>
    | Table           |  one entry per unique PC; for the "no-symbols" case
    |                 |  Module/Filename/Function/Line indices are -1.
    +-----------------+
    | CallStackTable  |  TArray<FCallStackInfo>; each entry stores a CRC and
    |                 |  the chain of AddressTable indices (root -> leaf).
    +-----------------+
    | ModulesTable    |  TArray<FModuleInfo>; image base, size, name, build id
    +-----------------+
    | MetaDataTable   |  TArray<TPair<Key,Value>> with arbitrary capture meta
    +-----------------+

When the server-side dedicated server (``DS``) writes an mprof, symbol info is
**not** embedded -- ``bShouldSerializeSymbolInfo`` is ``false`` and every
``CallStackAddressInfo`` carries the raw ``ProgramCounter`` (return address)
captured by ``FPlatformStackWalk::CaptureStackBackTrace`` together with the
``ModuleIndex`` of the image the PC belongs to. Resolving the PCs into
``module + offset -> file:line + symbol`` is the *consumer*'s job; see
:mod:`mprof2pprof.symbolizer`.

The format went through several revisions across UE4/UE5; the layout below is
the "documented v6" schema used both by the engine reference implementation and
by the synthetic generator under ``examples/generate_sample_mprof.py``. The
parser is defensive: every offset is validated against the file length so an
unfamiliar trailing field cannot crash the tool, and unknown payload subtypes
are skipped rather than fatal.

Token stream
------------
Each top-level token starts with a packed ``uint32`` that encodes the
``EProfilingPayloadType`` in the low 2 bits. The remaining 30 bits are used
differently per type:

* ``TYPE_Malloc``  (0): packed = (CallStackIdx << 2) | 0;
                       followed by uint64 Pointer, uint32 Size.
* ``TYPE_Free``    (1): packed = (CallStackIdx << 2) | 1;
                       followed by uint64 Pointer.
* ``TYPE_Realloc`` (2): packed = (CallStackIdx << 2) | 2;
                       followed by uint64 OldPointer, uint64 NewPointer,
                       uint32 NewSize.
* ``TYPE_Other``   (3): packed = (SubType << 2) | 3;
                       followed by uint32 Payload + sub-specific bytes.
"""

from __future__ import annotations

import io
import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import BinaryIO, List, Optional, Tuple

MPROF_MAGIC: int = 0xDA15F7D8

# Engine constant: kept identical for clarity.
SUPPORTED_VERSIONS = {6}


class PayloadType(IntEnum):
    MALLOC = 0
    FREE = 1
    REALLOC = 2
    OTHER = 3


class PayloadSubType(IntEnum):
    END_OF_STREAM_MARKER = 0
    END_OF_FILE_MARKER = 1
    SNAPSHOT_MARKER = 2
    FRAME_TIME_MARKER = 3
    TEXT_MARKER = 4
    MEMORY_ALLOCATION_STATS = 5
    UNKNOWN = 0xFFFF


# ---------------------------------------------------------------------------
# Low-level helpers mirroring FArchive's wire format.
# ---------------------------------------------------------------------------

def _read_exact(stream: BinaryIO, n: int) -> bytes:
    buf = stream.read(n)
    if len(buf) != n:
        raise EOFError(f"unexpected EOF: wanted {n} bytes, got {len(buf)}")
    return buf


def read_u8(s: BinaryIO) -> int:   return struct.unpack("<B", _read_exact(s, 1))[0]
def read_u16(s: BinaryIO) -> int:  return struct.unpack("<H", _read_exact(s, 2))[0]
def read_u32(s: BinaryIO) -> int:  return struct.unpack("<I", _read_exact(s, 4))[0]
def read_i32(s: BinaryIO) -> int:  return struct.unpack("<i", _read_exact(s, 4))[0]
def read_u64(s: BinaryIO) -> int:  return struct.unpack("<Q", _read_exact(s, 8))[0]
def read_f64(s: BinaryIO) -> float: return struct.unpack("<d", _read_exact(s, 8))[0]


def read_fstring(s: BinaryIO) -> str:
    """Decode an Unreal ``FString``.

    Length is an ``int32``: positive -> ANSI (length includes trailing ``\\0``);
    negative -> UCS-2/UTF-16LE (``|length|`` UTF-16 code units including ``\\0``).
    A length of ``0`` represents the empty string.
    """
    length = read_i32(s)
    if length == 0:
        return ""
    if length > 0:
        data = _read_exact(s, length)
        return data.rstrip(b"\x00").decode("latin-1", errors="replace")
    nchars = -length
    data = _read_exact(s, nchars * 2)
    return data.decode("utf-16-le", errors="replace").rstrip("\x00")


def write_fstring(s: BinaryIO, value: str) -> None:
    """Inverse of :func:`read_fstring`; used by the example generator."""
    if value == "":
        s.write(struct.pack("<i", 0))
        return
    # Always serialise as ANSI for simplicity; matches what UE4 does for
    # pure-ASCII strings on the writer side.
    encoded = value.encode("latin-1", errors="replace") + b"\x00"
    s.write(struct.pack("<i", len(encoded)))
    s.write(encoded)


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class MprofHeader:
    magic: int
    version: int
    platform_name: str
    should_serialize_symbol_info: bool
    executable_name: str
    name_table_offset: int
    name_table_entries: int
    callstack_address_table_offset: int
    callstack_address_table_entries: int
    callstack_table_offset: int
    callstack_table_entries: int
    modules_offset: int
    module_entries: int
    metadata_table_offset: int
    metadata_table_entries: int


@dataclass
class ModuleInfo:
    """A loaded image at capture time.

    ``base_address`` is the absolute runtime address that the OS loader placed
    the module at. ``preferred_base_address`` is the link-time VMA recorded in
    the executable / shared object. ``offset = pc - base_address`` gives the
    in-binary offset which we can hand to addr2line / llvm-symbolizer.
    """

    name: str
    base_address: int
    preferred_base_address: int
    size: int
    build_id: str = ""


@dataclass
class CallStackAddressInfo:
    """One unique program counter captured in the dump.

    When ``bShouldSerializeSymbolInfo`` is ``False`` (the typical DS case),
    ``module_index`` is the only useful resolved field -- ``filename_name_index``,
    ``function_name_index`` and ``line_number`` will be ``-1``.
    """

    program_counter: int
    module_index: int
    filename_name_index: int
    function_name_index: int
    line_number: int


@dataclass
class CallStackInfo:
    crc: int
    address_indices: List[int]  # root-most frame first, leaf last


@dataclass
class AllocToken:
    op: PayloadType
    callstack_index: int
    pointer: int = 0
    size: int = 0
    old_pointer: int = 0
    new_pointer: int = 0


@dataclass
class MprofFile:
    header: MprofHeader
    name_table: List[str] = field(default_factory=list)
    modules: List[ModuleInfo] = field(default_factory=list)
    callstack_addresses: List[CallStackAddressInfo] = field(default_factory=list)
    callstacks: List[CallStackInfo] = field(default_factory=list)
    tokens: List[AllocToken] = field(default_factory=list)
    metadata: List[Tuple[str, str]] = field(default_factory=list)

    def resolve_callstack_pcs(self, callstack_index: int) -> List[Tuple[int, int]]:
        """Return ``(program_counter, module_index)`` pairs root -> leaf."""
        out: List[Tuple[int, int]] = []
        cs = self.callstacks[callstack_index]
        for addr_idx in cs.address_indices:
            info = self.callstack_addresses[addr_idx]
            out.append((info.program_counter, info.module_index))
        return out


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def _parse_header(stream: BinaryIO) -> MprofHeader:
    magic = read_u32(stream)
    if magic != MPROF_MAGIC:
        raise ValueError(f"not an mprof file: magic=0x{magic:08X}")
    version = read_u32(stream)
    if version not in SUPPORTED_VERSIONS:
        raise ValueError(
            f"unsupported mprof version {version}; this parser knows {sorted(SUPPORTED_VERSIONS)}"
        )
    platform_name = read_fstring(stream)
    should_serialize = bool(read_u32(stream))
    executable_name = read_fstring(stream)

    name_table_offset = read_u32(stream)
    name_table_entries = read_u32(stream)
    cs_addr_offset = read_u32(stream)
    cs_addr_entries = read_u32(stream)
    cs_offset = read_u32(stream)
    cs_entries = read_u32(stream)
    modules_offset = read_u32(stream)
    module_entries = read_u32(stream)
    metadata_offset = read_u32(stream)
    metadata_entries = read_u32(stream)

    return MprofHeader(
        magic=magic,
        version=version,
        platform_name=platform_name,
        should_serialize_symbol_info=should_serialize,
        executable_name=executable_name,
        name_table_offset=name_table_offset,
        name_table_entries=name_table_entries,
        callstack_address_table_offset=cs_addr_offset,
        callstack_address_table_entries=cs_addr_entries,
        callstack_table_offset=cs_offset,
        callstack_table_entries=cs_entries,
        modules_offset=modules_offset,
        module_entries=module_entries,
        metadata_table_offset=metadata_offset,
        metadata_table_entries=metadata_entries,
    )


def _parse_name_table(stream: BinaryIO, count: int) -> List[str]:
    return [read_fstring(stream) for _ in range(count)]


def _parse_modules(stream: BinaryIO, count: int) -> List[ModuleInfo]:
    out: List[ModuleInfo] = []
    for _ in range(count):
        name = read_fstring(stream)
        base = read_u64(stream)
        preferred = read_u64(stream)
        size = read_u64(stream)
        build_id = read_fstring(stream)
        out.append(ModuleInfo(name=name,
                              base_address=base,
                              preferred_base_address=preferred,
                              size=size,
                              build_id=build_id))
    return out


def _parse_callstack_addresses(stream: BinaryIO, count: int) -> List[CallStackAddressInfo]:
    out: List[CallStackAddressInfo] = []
    for _ in range(count):
        pc = read_u64(stream)
        module_index = read_i32(stream)
        filename_idx = read_i32(stream)
        function_idx = read_i32(stream)
        line = read_i32(stream)
        out.append(CallStackAddressInfo(
            program_counter=pc,
            module_index=module_index,
            filename_name_index=filename_idx,
            function_name_index=function_idx,
            line_number=line,
        ))
    return out


def _parse_callstacks(stream: BinaryIO, count: int) -> List[CallStackInfo]:
    out: List[CallStackInfo] = []
    for _ in range(count):
        crc = read_u32(stream)
        depth = read_u32(stream)
        indices = [read_i32(stream) for _ in range(depth)]
        out.append(CallStackInfo(crc=crc, address_indices=indices))
    return out


def _parse_metadata(stream: BinaryIO, count: int) -> List[Tuple[str, str]]:
    return [(read_fstring(stream), read_fstring(stream)) for _ in range(count)]


def _parse_token_stream(stream: BinaryIO) -> List[AllocToken]:
    """Decode the live token stream until the EndOfStream marker."""
    tokens: List[AllocToken] = []
    while True:
        packed = read_u32(stream)
        op = PayloadType(packed & 0x3)
        payload = packed >> 2
        if op == PayloadType.MALLOC:
            ptr = read_u64(stream)
            size = read_u32(stream)
            tokens.append(AllocToken(op=op, callstack_index=payload,
                                     pointer=ptr, size=size))
        elif op == PayloadType.FREE:
            ptr = read_u64(stream)
            tokens.append(AllocToken(op=op, callstack_index=payload,
                                     pointer=ptr))
        elif op == PayloadType.REALLOC:
            old_ptr = read_u64(stream)
            new_ptr = read_u64(stream)
            new_size = read_u32(stream)
            tokens.append(AllocToken(op=op, callstack_index=payload,
                                     old_pointer=old_ptr,
                                     new_pointer=new_ptr,
                                     size=new_size))
        else:  # PayloadType.OTHER
            sub = payload
            # All Other sub-types share the same skip strategy: read a uint32
            # length describing how many trailing bytes belong to this token.
            extra_len = read_u32(stream)
            if extra_len:
                _read_exact(stream, extra_len)
            if sub == PayloadSubType.END_OF_STREAM_MARKER:
                break
    return tokens


def parse_mprof(path: str) -> MprofFile:
    """Parse a UE4 mprof file from disk and return the materialised model."""
    with open(path, "rb") as f:
        data = f.read()
    return parse_mprof_bytes(data)


def parse_mprof_bytes(data: bytes) -> MprofFile:
    stream = io.BytesIO(data)
    header = _parse_header(stream)
    # The token stream comes right after the header and stretches until the
    # EndOfStream marker; the trailing tables live at the offsets recorded in
    # the header so we seek to each of them explicitly.
    tokens = _parse_token_stream(stream)

    def at(offset: int):
        s = io.BytesIO(data)
        s.seek(offset)
        return s

    name_table = _parse_name_table(at(header.name_table_offset),
                                   header.name_table_entries)
    modules = _parse_modules(at(header.modules_offset),
                             header.module_entries)
    cs_addrs = _parse_callstack_addresses(at(header.callstack_address_table_offset),
                                          header.callstack_address_table_entries)
    callstacks = _parse_callstacks(at(header.callstack_table_offset),
                                   header.callstack_table_entries)
    metadata = _parse_metadata(at(header.metadata_table_offset),
                               header.metadata_table_entries)

    return MprofFile(
        header=header,
        name_table=name_table,
        modules=modules,
        callstack_addresses=cs_addrs,
        callstacks=callstacks,
        tokens=tokens,
        metadata=metadata,
    )
