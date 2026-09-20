#!/usr/bin/env python3
"""Inspect the ToolBook 4 OpenScript dispatcher in MTB40RUN.EXE.

The runtime is a 16-bit Windows NE executable.  Its OpenScript interpreter
lives in segment 34; its fetch continuations index a near jump table in the
automatic data segment.  This tool derives the segment file offsets from the
NE header, verifies the observed dispatcher and table invariants, and prints
the complete opcode-to-handler map without external dependencies.

Capstone is optional.  When installed, ``--disasm`` also prints the first
instructions of each selected handler::

    tools/runtime_opcodes.py games/bashnya/RUNTIME/MTB40RUN.EXE
    tools/runtime_opcodes.py --disasm --opcode 0x23 --opcode 0x6c \
        games/bashnya/RUNTIME/MTB40RUN.EXE
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


KNOWN_SHA256 = "93070576acac60dfa81c47bb1a31e29e481d2df996c5048c49a2b24f6416a9a9"

CODE_SEGMENT = 34
TABLE_OFFSET = 0x0B88
OPCODE_COUNT = 0x79  # inclusive range 0x00..0x78

# lodsb; xor ah,ah; add ax,ax; mov bx,ax; jmp word ptr ss:[bx+0x0b88]
DISPATCH_SIGNATURE = bytes.fromhex("ac32e403c08bd836ffa7880b")
EXPECTED_DISPATCH_OFFSETS = (0x33EC, 0x3448)
TOP_LEVEL_ENTRY_OFFSET = 0x0A33
TOP_LEVEL_FETCH_OFFSET = 0x0B45
RESUME_FETCH_OFFSET = 0x0C50

EXPECTED_FIRST = (0x0DA6, 0x0DD2, 0x0DFB, 0x0E27, 0x0E8E, 0x0EB6, 0x0EDC, 0x0F38)
EXPECTED_LAST = (0x2EC1, 0x3F35, 0x396F, 0x39A3, 0x38CC, 0x4943, 0x3352, 0x295B)


class FormatError(Exception):
    """The input is not the validated ToolBook runtime layout."""


@dataclass(frozen=True)
class Segment:
    number: int
    file_offset: int
    size: int
    flags: int
    min_alloc: int


@dataclass(frozen=True)
class NEImage:
    data: bytes
    ne_offset: int
    sector_shift: int
    auto_data_segment: int
    segments: tuple[Segment, ...]

    def segment(self, number: int) -> Segment:
        if number < 1 or number > len(self.segments):
            raise FormatError(f"NE segment {number} is outside 1..{len(self.segments)}")
        return self.segments[number - 1]


def _u16(data: bytes, offset: int, what: str) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise FormatError(f"truncated while reading {what} at file offset {offset:#x}")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int, what: str) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise FormatError(f"truncated while reading {what} at file offset {offset:#x}")
    return struct.unpack_from("<I", data, offset)[0]


def parse_ne(data: bytes) -> NEImage:
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise FormatError("missing DOS MZ header")

    ne_offset = _u32(data, 0x3C, "e_lfanew")
    if ne_offset + 0x40 > len(data) or data[ne_offset:ne_offset + 2] != b"NE":
        raise FormatError(f"missing NE header at {ne_offset:#x}")

    segment_count = _u16(data, ne_offset + 0x1C, "segment count")
    auto_data = _u16(data, ne_offset + 0x0E, "automatic data segment")
    segment_table = ne_offset + _u16(data, ne_offset + 0x22, "segment table offset")
    sector_shift = _u16(data, ne_offset + 0x32, "sector alignment shift")
    if not segment_count or segment_count > 4096:
        raise FormatError(f"implausible NE segment count {segment_count}")
    if sector_shift > 31:
        raise FormatError(f"implausible NE sector shift {sector_shift}")
    if segment_table + segment_count * 8 > len(data):
        raise FormatError("truncated NE segment table")

    segments = []
    for index in range(segment_count):
        sector, raw_size, flags, min_alloc = struct.unpack_from(
            "<4H", data, segment_table + index * 8
        )
        size = raw_size or 0x10000
        file_offset = sector << sector_shift if sector else 0
        if sector and file_offset + size > len(data):
            raise FormatError(
                f"segment {index + 1} exceeds file: {file_offset:#x}+{size:#x}"
            )
        segments.append(Segment(index + 1, file_offset, size, flags, min_alloc))

    if not auto_data or auto_data > segment_count:
        raise FormatError(f"invalid automatic data segment {auto_data}")
    return NEImage(data, ne_offset, sector_shift, auto_data, tuple(segments))


def find_all(haystack: bytes, needle: bytes) -> list[int]:
    offsets = []
    start = 0
    while True:
        found = haystack.find(needle, start)
        if found < 0:
            return offsets
        offsets.append(found)
        start = found + 1


def inspect_runtime(image: NEImage) -> tuple[Segment, Segment, list[int], list[int]]:
    code = image.segment(CODE_SEGMENT)
    auto_data = image.segment(image.auto_data_segment)
    if not code.file_offset or not auto_data.file_offset:
        raise FormatError("interpreter code or automatic data segment has no file data")

    code_bytes = image.data[code.file_offset:code.file_offset + code.size]
    # The compact dispatch tail is shared by internal continuations: 0x33ec is
    # inside opcode 0x77 and 0x3448 is one branch of opcode 0x29. They are not
    # top-level entry paths (those are reported separately below), but their
    # exact signatures still provide a strong table-layout invariant.
    signature_matches = find_all(code_bytes, DISPATCH_SIGNATURE)
    missing = [offset for offset in EXPECTED_DISPATCH_OFFSETS if offset not in signature_matches]
    if missing:
        got = ", ".join(f"{offset:#x}" for offset in signature_matches) or "none"
        wanted = ", ".join(f"{offset:#x}" for offset in missing)
        raise FormatError(f"dispatcher signature missing at {wanted}; matches are {got}")
    dispatchers = list(EXPECTED_DISPATCH_OFFSETS)

    # The final disp16 in both signatures is the offset in the automatic data
    # segment used by ``jmp word ptr ss:[bx+disp16]``.
    displacement = struct.unpack_from("<H", DISPATCH_SIGNATURE, len(DISPATCH_SIGNATURE) - 2)[0]
    if displacement != TABLE_OFFSET:
        raise FormatError(f"dispatcher table displacement is {displacement:#x}")

    table_size = OPCODE_COUNT * 2
    if TABLE_OFFSET + table_size > auto_data.size:
        raise FormatError("opcode table exceeds automatic data segment")
    table_file_offset = auto_data.file_offset + TABLE_OFFSET
    handlers = list(struct.unpack_from(
        f"<{OPCODE_COUNT}H", image.data, table_file_offset
    ))

    if tuple(handlers[:8]) != EXPECTED_FIRST or tuple(handlers[-8:]) != EXPECTED_LAST:
        raise FormatError("opcode table bookends do not match the validated runtime")
    invalid = [(opcode, target) for opcode, target in enumerate(handlers) if target >= code.size]
    if invalid:
        opcode, target = invalid[0]
        raise FormatError(
            f"opcode {opcode:#04x} target {target:#06x} exceeds segment 34 size {code.size:#x}"
        )
    if len(set(handlers)) != len(handlers):
        raise FormatError("opcode table unexpectedly contains duplicate handler offsets")

    return code, auto_data, dispatchers, handlers


def parse_opcode(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid opcode {text!r}") from exc
    if not 0 <= value < OPCODE_COUNT:
        raise argparse.ArgumentTypeError("opcode must be in range 0x00..0x78")
    return value


def print_disassembly(
    image: NEImage,
    code: Segment,
    handlers: list[int],
    opcodes: list[int],
    instruction_limit: int,
) -> None:
    try:
        from capstone import CS_ARCH_X86, CS_MODE_16, Cs
    except ImportError as exc:
        raise FormatError(
            "--disasm requires the optional capstone package; the base table does not"
        ) from exc

    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    segment_data = image.data[code.file_offset:code.file_offset + code.size]
    for opcode in opcodes:
        target = handlers[opcode]
        sample = segment_data[target:min(code.size, target + 0x100)]
        print(f"\nopcode 0x{opcode:02x}, seg34:{target:04x}")
        shown = 0
        for instruction in decoder.disasm(sample, target):
            raw = instruction.bytes.hex()
            print(
                f"  {instruction.address:04x}: {raw:<20} "
                f"{instruction.mnemonic:<8} {instruction.op_str}".rstrip()
            )
            shown += 1
            if shown >= instruction_limit or instruction.mnemonic in {"ret", "retf", "iret"}:
                break
        if not shown:
            print("  <capstone decoded no instruction>")


def main() -> int:
    project_root = Path(__file__).resolve().parent.parent
    default_runtime = project_root / "games/bashnya/RUNTIME/MTB40RUN.EXE"

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "runtime", nargs="?", type=Path, default=default_runtime,
        help="path to MTB40RUN.EXE (defaults to the project runtime copy)",
    )
    parser.add_argument(
        "--disasm", action="store_true",
        help="disassemble handler prefixes using optional capstone",
    )
    parser.add_argument(
        "--opcode", action="append", type=parse_opcode, default=[],
        help="opcode to disassemble; repeatable (default: all)",
    )
    parser.add_argument(
        "--instructions", type=int, default=12,
        help="maximum instructions per handler with --disasm (default: 12)",
    )
    args = parser.parse_args()
    if args.instructions < 1:
        parser.error("--instructions must be positive")

    try:
        data = args.runtime.read_bytes()
        image = parse_ne(data)
        code, auto_data, dispatchers, handlers = inspect_runtime(image)
    except OSError as exc:
        print(f"runtime_opcodes.py: {exc}", file=sys.stderr)
        return 2
    except (FormatError, struct.error) as exc:
        print(f"runtime_opcodes.py: invalid runtime: {exc}", file=sys.stderr)
        return 2

    digest = hashlib.sha256(data).hexdigest()
    print(f"runtime: {args.runtime}")
    print(f"sha256: {digest} ({'known' if digest == KNOWN_SHA256 else 'unrecognized'})")
    print(
        f"NE: header={image.ne_offset:#x}, segments={len(image.segments)}, "
        f"sector_shift={image.sector_shift}, autodata=seg{image.auto_data_segment}"
    )
    print(
        f"seg34: file={code.file_offset:#x}, size={code.size:#x}, flags={code.flags:#06x}"
    )
    print(
        f"seg{auto_data.number}: file={auto_data.file_offset:#x}, "
        f"size={auto_data.size:#x}, flags={auto_data.flags:#06x}"
    )
    print(
        "validated fetch continuations: " + ", ".join(f"seg34:{offset:04x}" for offset in dispatchers)
    )
    print(f"top-level entry: seg34:{TOP_LEVEL_ENTRY_OFFSET:04x}")
    print(f"top-level first fetch: seg34:{TOP_LEVEL_FETCH_OFFSET:04x}")
    print(f"resume/debug fetch: seg34:{RESUME_FETCH_OFFSET:04x}")
    print(
        f"opcode table: seg{auto_data.number}:{TABLE_OFFSET:04x}, "
        f"file={auto_data.file_offset + TABLE_OFFSET:#x}, entries={len(handlers)}"
    )
    print("opcode  handler          file")
    for opcode, target in enumerate(handlers):
        print(f"  0x{opcode:02x}  seg34:0x{target:04x}  {code.file_offset + target:#08x}")

    if args.disasm:
        selected = args.opcode or list(range(OPCODE_COUNT))
        try:
            print_disassembly(image, code, handlers, selected, args.instructions)
        except FormatError as exc:
            print(f"runtime_opcodes.py: {exc}", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
