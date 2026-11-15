#!/usr/bin/env python3
"""Turns the addresses in an RFAB Online crash record into names.

The client writes every crash into RFAB_Online_Crash.log (and one line into tp_client.log): the
exception code, the registers and up to 16 unwound frames as `<module>+0x<rva>`. This script reads
such a log and explains each address:

  * an address inside our own binary (the launcher that carries the client, or STServer.dll) is
    looked up in the linker map file of the SAME build (PDB_Symbols zip of the CI artifact) and
    becomes `Class::Method +0x<offset>  [object file]`;
  * an address inside the game image is looked up in docs/se-1597-id-map.csv, the list of game
    functions the client hooks or calls, and becomes `Address Library id N (symbol)`, with the
    source line that refers to it. An address between two known functions is reported as "after
    id N": the game has no symbols, so that is the nearest thing we touch, not a certainty.

The launcher is built as SkyrimSE.exe and the game image is loaded inside it, so one module name
covers both. The map file decides: an address that falls into one of its functions is ours.

    python3 Tools/rfab/symbolize_crash.py <unpacked report folder>      # finds logs and symbols itself
    python3 Tools/rfab/symbolize_crash.py RFAB_Online_Crash.log --map SkyrimSE.map
    python3 Tools/rfab/symbolize_crash.py tp_client.log --map SkyrimSE.map --map STServer.map
    python3 Tools/rfab/symbolize_crash.py --address SkyrimSE.exe+0x2F3A10 --map SkyrimSE.map
    python3 Tools/rfab/symbolize_crash.py --export-symbols SkyrimSE.map --out RFAB_Online_Symbols.gz
    python3 Tools/rfab/symbolize_crash.py --self-test

A map file is tens of megabytes. `--export-symbols` writes the part this script needs (functions
only, gzip) and the build installs that file next to the client, so that the report collector
(Tools/rfab/Collect-RfabReport.ps1) can put the symbols of the exact binary into every report.
`--map` accepts either form.

Exit code: 0 when the input was read (crash records or not), 2 on a usage error.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import gzip
import io
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ID_MAP = REPO_ROOT / "docs" / "se-1597-id-map.csv"

# The game's own code ends well below this; the crash logger labels launcher addresses above
# 64 MB as trampolines, so the same boundary is used here when the map has no answer.
GAME_IMAGE_LIMIT = 0x04000000
# A game function we know by id is assumed to be at most this long when an address has to be
# attributed to "the function that starts before it".
NEAR_GAME_FUNCTION = 0x600
# Same for our own functions, where the map gives every start: the gap to the next symbol bounds it.
MAX_OWN_FUNCTION = 0x40000

EXCEPTION_CODES = {
    0xC0000005: "access violation",
    0xC00000FD: "stack overflow",
    0xC0000409: "fast fail (std::terminate, abort, a failed /GS or noexcept check)",
    0xC0000374: "heap corruption",
    0xC000001D: "illegal instruction",
    0xC0000094: "integer division by zero",
    0xC0000096: "privileged instruction",
    0xC000013A: "process terminated from the console",
    0xE06D7363: "C++ exception that nothing caught",
    0x80000003: "breakpoint",
    0x80000001: "guard page",
}


# --------------------------------------------------------------------------------------------
# Linker map (link.exe /MAP)
# --------------------------------------------------------------------------------------------
@dataclass
class MapSymbol:
    rva: int
    name: str
    is_function: bool
    object_file: str


@dataclass
class LinkerMap:
    module: str  # "SkyrimSE", "STServer"
    preferred_base: int
    timestamp: str = ""  # PE TimeDateStamp of the binary the map belongs to, lower-case hex
    symbols: list[MapSymbol] = field(default_factory=list)
    code_ranges: list[tuple[int, int]] = field(default_factory=list)
    _starts: list[int] = field(default_factory=list)

    def finish(self) -> None:
        self.symbols.sort(key=lambda s: s.rva)
        self._starts = [s.rva for s in self.symbols]

    def in_code(self, rva: int) -> bool:
        return any(start <= rva < end for start, end in self.code_ranges)

    def lookup(self, rva: int) -> tuple[MapSymbol, int] | None:
        """The function that contains `rva`, with the offset into it."""
        index = bisect.bisect_right(self._starts, rva) - 1
        while index >= 0 and not self.symbols[index].is_function:
            index -= 1
        if index < 0:
            return None
        symbol = self.symbols[index]
        offset = rva - symbol.rva
        if offset > MAX_OWN_FUNCTION:
            return None
        if self.code_ranges and not self.in_code(rva):
            return None
        return symbol, offset


_SECTION_LINE = re.compile(r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+([0-9a-fA-F]+)H\s+(\S+)\s+(\S+)\s*$")
_SYMBOL_LINE = re.compile(
    r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{16})\s+(?:(f)\s+)?(?:(i)\s+)?(.*)$"
)
_BASE_LINE = re.compile(r"Preferred load address is\s+([0-9a-fA-F]+)")
_TIMESTAMP_LINE = re.compile(r"Timestamp is\s+([0-9a-fA-F]+)")
COMPACT_MAGIC = "RFABSYM 1"
MAX_COMPACT_NAME = 200


_SPECIAL_NAMES = {
    "0": "{ctor}", "1": "{dtor}", "2": "operator new", "3": "operator delete", "4": "operator=", "8": "operator==",
    "9": "operator!=", "A": "operator[]", "R": "operator()", "B": "operator <cast>",
    "_G": "{deleting dtor}", "_E": "{vector deleting dtor}", "_7": "{vftable}",
}
MAX_RAW_NAME = 110


def demangle(name: str) -> str:
    """A readable form of an MSVC decorated name.

    `?Method@Class@Namespace@@...` becomes `Namespace::Class::Method`. Templates and lambdas have
    no simple grammar: for those the plain part of the name is shown and the decorated name is kept
    next to it, which together with the object file is enough to find the function.
    """
    if not name.startswith("?"):
        return name
    raw = name if len(name) <= MAX_RAW_NAME else name[:MAX_RAW_NAME] + "..."
    body = name[1:]

    if body.startswith("?$"):
        # A function template instantiation: ??$Name@<arguments>@<scope>@@...
        end = body.find("@")
        return f"{body[2:end] if end > 2 else '?'}<...>  ({raw})"

    special = ""
    if body.startswith("?"):
        code = body[1:3] if body[1:2] == "_" else body[1:2]
        special = _SPECIAL_NAMES.get(code, f"{{special {code}}}")
        body = body[1 + len(code):]

    scope_end = body.find("@@")
    if scope_end < 0:
        return name
    scope = body[:scope_end]
    if "?" in scope or "$" in scope or "<" in scope:
        plain = scope.split("@", 1)[0] if not special else special
        return f"{plain if plain and '?' not in plain else '?'}  ({raw})"
    parts = [part for part in scope.split("@") if part]
    parts.reverse()
    readable = "::".join(parts)
    if special:
        readable = f"{readable}::{special}" if readable else special
    return readable or name


def read_symbol_file(path: Path) -> str:
    raw = path.read_bytes()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    return raw.decode("utf-8", errors="replace")


def parse_compact(text: str) -> LinkerMap:
    """The form written by export_compact: a header, then one function per line."""
    linker_map = LinkerMap(module="?", preferred_base=0x140000000)
    for line in text.splitlines():
        kind, _, rest = line.partition(" ")
        if kind == "f":
            fields = rest.split("\t")
            if len(fields) >= 2:
                try:
                    linker_map.symbols.append(MapSymbol(int(fields[0], 16), fields[1], True, fields[2] if len(fields) > 2 else ""))
                except ValueError:
                    continue
        elif kind == "module":
            linker_map.module = rest.strip()
        elif kind == "timestamp":
            linker_map.timestamp = rest.strip().lower()
        elif kind == "base":
            linker_map.preferred_base = int(rest.strip(), 16)
        elif kind == "code":
            start, _, end = rest.strip().partition(" ")
            linker_map.code_ranges.append((int(start, 16), int(end, 16)))
    linker_map.finish()
    return linker_map


def export_compact(linker_map: LinkerMap) -> bytes:
    out = io.StringIO()
    out.write(f"{COMPACT_MAGIC}\n")
    out.write(f"module {linker_map.module}\n")
    out.write(f"timestamp {linker_map.timestamp}\n")
    out.write(f"base {linker_map.preferred_base:x}\n")
    for start, end in linker_map.code_ranges:
        out.write(f"code {start:x} {end:x}\n")
    for symbol in linker_map.symbols:
        if symbol.is_function:
            name = symbol.name if len(symbol.name) <= MAX_COMPACT_NAME else symbol.name[:MAX_COMPACT_NAME]
            out.write(f"f {symbol.rva:x}\t{name}\t{symbol.object_file}\n")
    # mtime=0: the same map always gives the same bytes.
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode="wb", compresslevel=9, mtime=0) as handle:
        handle.write(out.getvalue().encode("utf-8"))
    return buffer.getvalue()


def parse_map(path: Path) -> LinkerMap:
    text = read_symbol_file(path)
    if text.startswith(COMPACT_MAGIC):
        return parse_compact(text)
    lines = text.splitlines()
    module = next((line.strip() for line in lines if line.strip()), path.stem)
    base_match = _BASE_LINE.search(text)
    base = int(base_match.group(1), 16) if base_match else 0x140000000
    linker_map = LinkerMap(module=module, preferred_base=base)
    stamp_match = _TIMESTAMP_LINE.search(text)
    if stamp_match:
        linker_map.timestamp = stamp_match.group(1).lower()

    section_starts: dict[int, int] = {}
    pending_sections: list[tuple[int, int, int, str]] = []
    for line in lines:
        section = _SECTION_LINE.match(line)
        if section:
            pending_sections.append((int(section.group(1), 16), int(section.group(2), 16), int(section.group(3), 16), section.group(5)))
            continue
        symbol = _SYMBOL_LINE.match(line)
        if not symbol:
            continue
        seg, offset = int(symbol.group(1), 16), int(symbol.group(2), 16)
        absolute = int(symbol.group(4), 16)
        if seg == 0 or absolute < base:
            continue  # absolute symbols such as __guard_flags
        rva = absolute - base
        section_starts.setdefault(seg, rva - offset)
        linker_map.symbols.append(MapSymbol(rva, symbol.group(3), symbol.group(5) == "f", symbol.group(7).strip()))

    for seg, offset, length, section_class in pending_sections:
        if section_class.upper() == "CODE" and seg in section_starts:
            start = section_starts[seg] + offset
            linker_map.code_ranges.append((start, start + length))
    linker_map.finish()
    return linker_map


# --------------------------------------------------------------------------------------------
# Game functions the client refers to (docs/se-1597-id-map.csv)
# --------------------------------------------------------------------------------------------
@dataclass
class GameSite:
    rva: int
    se_id: str
    kind: str
    symbol: str
    first_reference: str


class GameIdMap:
    def __init__(self, sites: list[GameSite]) -> None:
        self.sites = sorted(sites, key=lambda s: s.rva)
        self._starts = [s.rva for s in self.sites]

    @classmethod
    def load(cls, path: Path) -> "GameIdMap":
        sites: list[GameSite] = []
        if path.is_file():
            with path.open(encoding="utf-8", errors="replace", newline="") as handle:
                for row in csv.DictReader(handle):
                    try:
                        rva = int(row.get("se_rva_hex", "") or "0", 16)
                    except ValueError:
                        continue
                    if rva:
                        sites.append(GameSite(rva, row.get("se_1_5_97_id", ""), row.get("kind", ""), row.get("symbol", ""), row.get("first_reference", "")))
        return cls(sites)

    def lookup(self, rva: int) -> tuple[GameSite, int] | None:
        index = bisect.bisect_right(self._starts, rva) - 1
        if index < 0:
            return None
        site = self.sites[index]
        offset = rva - site.rva
        if offset > NEAR_GAME_FUNCTION:
            return None
        return site, offset


# --------------------------------------------------------------------------------------------
# Explaining one address
# --------------------------------------------------------------------------------------------
LAUNCHER_NAMES = {"skyrimse.exe", "skyrimtogether.exe"}


def explain(module: str, rva: int, maps: list[LinkerMap], game: GameIdMap) -> str:
    name = module.lower().rsplit("\\", 1)[-1].rsplit("/", 1)[-1]
    stem = name.rsplit(".", 1)[0]
    is_launcher = name in LAUNCHER_NAMES

    for linker_map in maps:
        map_stem = linker_map.module.lower().rsplit(".", 1)[0]
        if map_stem != stem and not (is_launcher and map_stem in ("skyrimse", "skyrimtogether")):
            continue
        found = linker_map.lookup(rva)
        if found:
            symbol, offset = found
            where = f"  [{symbol.object_file}]" if symbol.object_file else ""
            return f"OURS  {demangle(symbol.name)} +0x{offset:X}{where}"

    if is_launcher:
        if rva >= GAME_IMAGE_LIMIT:
            hint = "" if maps else " (pass --map SkyrimSE.map: without it our own code cannot be told from trampolines)"
            return f"OURS? above the game image: a hook trampoline or our own code{hint}"
        found = game.lookup(rva)
        if found:
            site, offset = found
            label = f"Address Library id {site.se_id}" + (f" ({site.symbol})" if site.symbol else "")
            ref = f"  referred to at {site.first_reference}" if site.first_reference else ""
            if offset == 0:
                return f"GAME  {label}{ref}"
            return f"GAME  +0x{offset:X} after {label}: inside it or in the next function{ref}"
        return "GAME  a function the client neither hooks nor calls"

    if stem in ("stserver", "skyrimtogetherserver"):
        return "OURS  server binary (pass --map STServer.map)"
    return "other module"


# --------------------------------------------------------------------------------------------
# Reading crash records
# --------------------------------------------------------------------------------------------
_RECORD = re.compile(
    r"\[UNHANDLED_CRASH\].*?Code=0x(?P<code>[0-9A-Fa-f]+)\s+RIP=0x(?P<rip>[0-9A-Fa-f]+)\s+FaultAddr=0x(?P<fault>[0-9A-Fa-f]+)\s+"
    r"Module='(?P<module>[^']*)'\s+Base=0x(?P<base>[0-9A-Fa-f]+)\s+RVA=\+0x(?P<rva>[0-9A-Fa-f]+)"
)
_SHORT_RECORD = re.compile(
    r"Unhandled crash: code=0x(?P<code>[0-9A-Fa-f]+)\s+RIP=0x(?P<rip>[0-9A-Fa-f]+)\s+module='(?P<module>[^']*)'\s+\+0x(?P<rva>[0-9A-Fa-f]+)"
)
# "[unwind #n]" is a frame of a real unwind; "[stack #n]" is a return address found by scanning the
# stack (a crash without unwind data, and every [HANG] record: the stuck thread is not unwound).
_FRAME = re.compile(r"\[(?P<how>unwind|stack) #(?P<n>\d+)\]\s+(?P<module>[^\s\[+]+)(?P<tramp>\[TiltedTrampoline\])?\+0x(?P<rva>[0-9A-Fa-f]+)")
# The client's records of an ending that raised no exception (Code/client/RfabDiag.cpp, 4.1.2).
_EVENT = re.compile(r"\[(?P<kind>HANG|TERMINATE|PURECALL|INVALID_PARAMETER|ABORT)\]\s+(?P<text>.*)$")
_EVENT_TITLES = {
    "HANG": "hang: the game's main loop stopped turning",
    "TERMINATE": "std::terminate: an exception left a thread or a noexcept function",
    "PURECALL": "pure virtual call",
    "INVALID_PARAMETER": "the C runtime rejected an argument",
    "ABORT": "abort() was called",
}
_CPP = re.compile(r"Type=\[(?P<type>[^\]]*)\]\s+what\(\)=\[\"(?P<what>.*?)\"\]")
_TIMESTAMP = re.compile(r"^\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)\]")
# The server's second kind of record (Code/server_runner/CrashRecord.cpp): std::terminate was called.
_TERMINATE = re.compile(r"^(?:\[(?P<stamp>[^\]]+)\]\s+)?\[TERMINATE\]\s+(?P<text>.*?)(?:;\s*what\(\)=\[\"(?P<what>.*)\"\])?\s*$")
_ADDRESS = re.compile(r"^(?P<module>[^+]+)\+(?:0x)?(?P<rva>[0-9A-Fa-f]+)$")
# Breadcrumbs (Code/common/Diag/Breadcrumbs.h): what the client did just before the fault.
_CRUMB = re.compile(
    r"\[crumb (?P<kind>\S+) -(?P<age>\d+)ms T(?P<thread>\d+)\]\s+(?P<name>\S+)\s+a=0x(?P<a>[0-9A-Fa-f]+)\s+b=0x(?P<b>[0-9A-Fa-f]+)\s+x(?P<count>\d+)"
)
_THREAD = re.compile(r"\bTID=(\d+)")
OPCODES_HEADER = REPO_ROOT / "Code" / "encoding" / "Opcodes.h"


def describe_code(code: int, fault: int | None) -> str:
    text = EXCEPTION_CODES.get(code, "unknown exception code")
    if code == 0xC0000005 and fault is not None:
        if fault < 0x10000:
            text += f" at 0x{fault:X}: a null pointer (or a small offset from one)"
        elif fault == 0xFFFFFFFFFFFFFFFF:
            text += ": a non-canonical address, usually a pointer that was already freed"
        else:
            text += f" at 0x{fault:X}"
    return text


@dataclass
class Crumb:
    kind: str      # "event" (session, world, remote actors), "packet" or "hook"
    age_ms: int    # before the fault
    thread: int
    name: str
    a: int
    b: int
    count: int


def load_opcode_names(header: Path = OPCODES_HEADER) -> dict[str, list[str]]:
    """Enumerator names of ClientOpcode and ServerOpcode, by value. Read from the checkout this
    script runs in: the numbers are only right for a crash of the same protocol version."""
    names: dict[str, list[str]] = {}
    try:
        text = header.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return names
    for enum in re.finditer(r"enum\s+(ClientOpcode|ServerOpcode)\b[^{]*\{(.*?)\};", text, re.S):
        body = re.sub(r"//[^\n]*", "", enum.group(2))
        names[enum.group(1)] = [item.split("=")[0].strip() for item in body.split(",") if item.strip()]
    return names


# What the two values of an event crumb mean (the RFAB_EVENT_CRUMB call sites in Code/client).
EVENT_CRUMBS = {
    "session-begin": "session-begin",
    "session-end": "session-end",
    "world-load-begin": "world-load-begin ({online})",
    "world-load-end": "world-load-end ({online})",
    "cell-change-exterior": "cell-change-exterior to cell {a:X}, worldspace {b:X}",
    "cell-change-interior": "cell-change-interior to cell {a:X}",
    "neighbour-cell-containers": "neighbour-cell-containers: registering containers of the neighbouring cell {a:X} (Gameplay:bEnableNeighbourCellContainers)",
    "early-container-change": "early-container-change: container {a:X} changed before the server answered its registration, in the sent list: {b} (Gameplay:bEnableEarlyTakeReplay)",
    "remove-character": "remove-character: server id {a:X}",
    "ownership-transfer": "ownership-transfer: server id {a:X}, epoch {b}",
    "remote-actor-materialize": "remote-actor-materialize: server id {a:X} into cell {b:X}",
    "remote-actor-dematerialize": "remote-actor-dematerialize: form {a:X}, server id {b:X}",
    "blocking-screen": "blocking-screen: a loading (1) or black (2) screen, kind {a}, has been up for {b} s",
}


def describe_crumb(crumb: Crumb, opcodes: dict[str, list[str]]) -> str:
    def opcode(table: str) -> str:
        values = opcodes.get(table, [])
        return values[crumb.a] if crumb.a < len(values) else f"opcode {crumb.a}"

    if crumb.name in EVENT_CRUMBS:
        what = EVENT_CRUMBS[crumb.name].format(a=crumb.a, b=crumb.b, online="in a session" if crumb.a else "offline")
    elif crumb.name in ("packet-in", "packet-in-deferred"):
        what = f"{crumb.name} {opcode('ServerOpcode')}"
        if crumb.name == "packet-in":
            what += f", {crumb.b} bytes"
    elif crumb.name == "packet-out":
        what = f"packet-out {opcode('ClientOpcode')}"
    elif crumb.kind == "hook":
        subject = crumb.a or crumb.b
        what = crumb.name + (f" form {subject:X}" if subject else "")
        if crumb.a and crumb.b:
            what += f" (other {crumb.b:X})"
    else:
        what = crumb.name + (f" {crumb.a:X}" if crumb.a or crumb.b else "") + (f" / {crumb.b:X}" if crumb.b else "")
    if crumb.count > 1:
        what += f"  x{crumb.count}"
    return what


@dataclass
class CrashRecord:
    stamp: str
    code: int
    fault: int | None
    module: str
    rva: int
    cpp_type: str = ""
    cpp_what: str = ""
    thread: int = 0
    frames: list[tuple[int, str, int, bool]] = field(default_factory=list)  # (index, module, rva, found by a stack scan)
    crumbs: list["Crumb"] = field(default_factory=list)
    kind: str = "crash"  # or one of _EVENT_TITLES
    text: str = ""       # the sentence of an event record

    def key(self) -> tuple:
        return (self.stamp, self.kind, self.text, self.code, self.module.lower(), self.rva, tuple(self.frames))


def parse_records(text: str) -> list[CrashRecord]:
    records: list[CrashRecord] = []
    for line in text.replace("\r\n", "\n").split("\n"):
        record = _RECORD.search(line)
        short = None if record else _SHORT_RECORD.search(line)
        if record or short:
            match = record or short
            stamp = _TIMESTAMP.match(line)
            cpp = _CPP.search(line)
            thread = _THREAD.search(line)
            records.append(
                CrashRecord(
                    stamp=stamp.group(1) if stamp else "",
                    code=int(match.group("code"), 16),
                    fault=int(match.group("fault"), 16) if record else None,
                    module=match.group("module"),
                    rva=int(match.group("rva"), 16),
                    cpp_type=cpp.group("type") if cpp else "",
                    cpp_what=cpp.group("what") if cpp else "",
                    thread=int(thread.group(1)) if thread else 0,
                )
            )
        event = None if (record or short) else _EVENT.search(line)
        if event:
            stamp = _TIMESTAMP.match(line)
            thread = _THREAD.search(line)
            records.append(CrashRecord(stamp=stamp.group(1) if stamp else "", code=0, fault=None, module="", rva=0,
                                       thread=int(thread.group(1)) if thread else 0, kind=event.group("kind"), text=event.group("text").strip()))
        if records:
            for frame in _FRAME.finditer(line):
                records[-1].frames.append((int(frame.group("n")), frame.group("module"), int(frame.group("rva"), 16), frame.group("how") == "stack"))
            for crumb in _CRUMB.finditer(line):
                records[-1].crumbs.append(
                    Crumb(
                        kind=crumb.group("kind"),
                        age_ms=int(crumb.group("age")),
                        thread=int(crumb.group("thread")),
                        name=crumb.group("name"),
                        a=int(crumb.group("a"), 16),
                        b=int(crumb.group("b"), 16),
                        count=int(crumb.group("count")),
                    )
                )
    # A [TERMINATE] line with no call stack is the server's (render_terminations shows those).
    return [r for r in records if not (r.kind == "TERMINATE" and not r.frames)]


def render_record(number: int, record: CrashRecord, maps: list[LinkerMap], game: GameIdMap) -> list[str]:
    if record.kind == "crash":
        out = ["", f"=== crash {number}" + (f"  {record.stamp}" if record.stamp else "")]
        out.append(f"    0x{record.code:08X}  {describe_code(record.code, record.fault)}")
        if record.cpp_type or record.cpp_what:
            out.append(f"    C++ type {record.cpp_type}  what(): {record.cpp_what}")
        short_name = record.module.replace(chr(92), "/").rsplit("/", 1)[-1] or "?"
        out.append(f"    at      {short_name}+0x{record.rva:X}")
        out.append(f"            {explain(record.module, record.rva, maps, game)}")
    else:
        out = ["", f"=== record {number}" + (f"  {record.stamp}" if record.stamp else "") + f"  {_EVENT_TITLES.get(record.kind, record.kind)}"]
        out.append(f"    {record.text}")
        if record.kind == "HANG":
            out.append("    The frames marked '~' are return addresses found on the stuck thread's stack, innermost first;")
            out.append("    a few may be left over from earlier calls. The first OURS or GAME frames say where it stood.")
    for index, module, rva, scanned in record.frames:
        out.append(f"    {'~' if scanned else '#'}{index:<2}     {module}+0x{rva:X}")
        out.append(f"            {explain(module, rva, maps, game)}")
    if record.crumbs:
        opcodes = load_opcode_names()
        out.append("    before the fault (oldest first; '*' = on the thread that crashed):" if record.kind == "crash"
                   else "    before the record (oldest first; '*' = on the thread the record names):")
        # Both rings are written oldest first; one timeline is easier to read than two.
        for crumb in sorted(record.crumbs, key=lambda c: -c.age_ms):
            mark = "*" if record.thread and crumb.thread == record.thread else " "
            out.append(f"      {mark} -{crumb.age_ms:>6} ms  {crumb.kind:<6}  {describe_crumb(crumb, opcodes)}")
    return out


def render_terminations(text: str, last: int = 0) -> list[str]:
    """The [TERMINATE] lines of a server crash log: the process ended itself, with the exception's text."""
    found = []
    for line in text.replace("\r\n", "\n").split("\n"):
        match = _TERMINATE.match(line)
        if match:
            found.append(match)
    if not found:
        return []
    first = max(0, len(found) - last) if last > 0 else 0
    out = ["", f"=== std::terminate: {len(found)} record(s)" + (f", showing the last {len(found) - first}" if first else "")]
    for match in found[first:]:
        stamp = match.group("stamp") or "?"
        what = match.group("what")
        out.append(f"    {stamp}  {match.group('text')}")
        if what is not None:
            out.append(f"            what(): {what if what else '(empty)'}")
    out.append("    An exception left main, a thread or a noexcept function. On Windows the record above it")
    out.append("    (code 0xE06D7363, same second) carries the call stack of the throw.")
    return out


def symbolize_text(text: str, maps: list[LinkerMap], game: GameIdMap, last: int = 0) -> list[str]:
    records = parse_records(text)
    terminations = render_terminations(text, last)
    if not records:
        if terminations:
            return terminations
        return ["no crash record found (looked for [UNHANDLED_CRASH], [HANG], [TERMINATE], [PURECALL], [INVALID_PARAMETER], [ABORT] and 'Unhandled crash:' lines)"]
    out: list[str] = []
    first = max(0, len(records) - last) if last > 0 else 0
    if first:
        out.append(f"{len(records)} crash records, showing the last {len(records) - first}")
    for number, record in enumerate(records[first:], first + 1):
        out.extend(render_record(number, record, maps, game))
    out.extend(terminations)
    return out


def read_input(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8", errors="replace")


# --------------------------------------------------------------------------------------------
# A whole report folder (what Collect-RfabReport.ps1 produces, unpacked)
# --------------------------------------------------------------------------------------------
_REPORT_BINARY = re.compile(r"^(?P<path>[^|]+?)\s*\|.*?\bpe_timestamp=(?P<stamp>[0-9A-Fa-f]+)", re.M)


_BOOT_START = re.compile(r"\[BOOT\] Checkpoint \|.*GamePathSet=1")
_BOOT_BUILD = re.compile(r"Build='(?P<build>[^']*)'")
_START_MARKS = (
    ("[UNHANDLED_CRASH]", "CRASHED (see the crash records)"),
    ("[TERMINATE]", "ended by std::terminate (see the records)"),
    ("[PURECALL]", "ended by a pure virtual call (see the records)"),
    ("[INVALID_PARAMETER]", "ended by the C runtime (see the records)"),
    ("[ABORT]", "ended by abort() (see the records)"),
    ("[HANG-END]", None),  # a stall that ended is not an ending
    ("[HANG]", "HUNG (see the records)"),
    ("[EXIT]", "ended normally"),
)


def summarize_starts(text: str, last: int = 12) -> list[str]:
    """One line per start of the game from RFAB_Online_Startup.log: when, how far it got, how it ended.

    A start with no ending at all is the case the 2026-11-15 playtest was full of: the process
    disappeared (closed from the task manager, killed by Windows or by a driver reset, a fast-fail
    that no handler sees). Windows may know more: windows/application_events.txt of the report.
    """
    starts: list[dict] = []
    for line in text.replace("\r\n", "\n").split("\n"):
        if _BOOT_START.search(line):
            stamp = _TIMESTAMP.match(line)
            build = _BOOT_BUILD.search(line)
            starts.append({"stamp": stamp.group(1) if stamp else "(no time: written by 4.1.1 or older)", "build": build.group("build") if build else "",
                           "reached": "launcher", "ending": "", "stalls": 0})
            continue
        if not starts:
            continue
        current = starts[-1]
        if "[BOOT OK]" in line:
            current["reached"] = "game start"
        elif "main loop is turning" in line:
            current["reached"] = "main menu"
        for mark, ending in _START_MARKS:
            if mark in line:
                if mark == "[HANG-END]":
                    current["stalls"] += 1
                    if current["ending"].startswith("HUNG"):
                        current["ending"] = ""
                elif mark == "[HANG]":
                    current["ending"] = current["ending"] or ending
                elif ending and (not current["ending"] or mark != "[EXIT]"):
                    current["ending"] = ending
                break
    if not starts:
        return []
    first = max(0, len(starts) - last) if last > 0 else 0
    out = ["", f"=== starts of the game: {len(starts)}" + (f", showing the last {len(starts) - first}" if first else "")]
    for start in starts[first:]:
        timed = not start["stamp"].startswith("(")
        if start["ending"]:
            ending = start["ending"]
        elif not timed:
            ending = "unknown (this version wrote no ending)"
        else:
            ending = "NO RECORD OF AN ENDING: the process disappeared (killed, closed from outside, or a failure no handler sees)"
        stalls = f", {start['stalls']} long stall(s) that ended" if start["stalls"] else ""
        build = f" build {start['build']}" if start["build"] else ""
        out.append(f"    {start['stamp']}{build}: reached {start['reached']}{stalls}; {ending}")
    return out


def symbolize_report(folder: Path, extra_maps: list[LinkerMap], game: GameIdMap, last: int) -> list[str]:
    out: list[str] = [f"report folder: {folder}"]
    maps = list(extra_maps)
    for symbol_file in sorted(list(folder.rglob("*.gz")) + list(folder.rglob("*.map"))):
        try:
            linker_map = parse_map(symbol_file)
        except (OSError, ValueError, EOFError) as error:
            out.append(f"  cannot read symbols {symbol_file.name}: {error}")
            continue
        if linker_map.symbols:
            maps.append(linker_map)
            out.append(f"  symbols: {symbol_file.relative_to(folder)}  module {linker_map.module}, {len(linker_map.symbols)} functions, timestamp {linker_map.timestamp or '?'}")
    if not maps:
        out.append("  NO SYMBOLS in the report: our own frames stay unnamed (pass --map with the map of the same build)")

    # Do the symbols belong to the binary that was installed when the report was made?
    report_txt = next(iter(sorted(folder.rglob("REPORT.txt"))), None)
    if report_txt is not None:
        # The same file name can be listed more than once: client\\SkyrimSE.exe is our launcher under
        # the game's name, game\\SkyrimSE.exe is the game itself. The symbols are right when ANY
        # binary of that name carries their timestamp (until 4.1.2 the last one listed won, and
        # every report claimed that our own function names were wrong).
        installed: dict[str, list[str]] = {}
        for match in _REPORT_BINARY.finditer(report_txt.read_text(encoding="utf-8", errors="replace")):
            binary = match.group("path").strip().replace(chr(92), "/").rsplit("/", 1)[-1].lower()
            installed.setdefault(binary, []).append(match.group("stamp").lower())
        for linker_map in maps:
            stem = linker_map.module.lower().rsplit(".", 1)[0]
            for binary, stamps in installed.items():
                if binary.rsplit(".", 1)[0] != stem or not linker_map.timestamp:
                    continue
                wanted = int(linker_map.timestamp, 16)
                matching = [stamp for stamp in stamps if int(stamp, 16) == wanted]
                if matching:
                    out.append(f"  {binary}: the symbols belong to the installed binary (timestamp {matching[0]})")
                else:
                    out.append(f"  WARNING {binary}: installed binary has timestamp {', '.join(stamps)}, the symbols are for {linker_map.timestamp}. "
                               f"Names of OUR functions below are WRONG; game addresses are still right.")

    crash_logs = sorted(folder.rglob("RFAB_Online_Crash.log"))
    sources = crash_logs if crash_logs else sorted(folder.rglob("tp_client*.log*"))
    # The server writes its own log (logs/STServer_Crash.log): crash records in the client's layout
    # and [TERMINATE] lines.
    server_logs = sorted(folder.rglob("STServer_Crash.log"))
    sources = sources + server_logs
    records: list[CrashRecord] = []
    seen: set[tuple] = set()
    terminations: list[str] = []
    for source in sources:
        text = source.read_text(encoding="utf-8", errors="replace")
        for record in parse_records(text):
            if record.key() not in seen:
                seen.add(record.key())
                records.append(record)
        if source in server_logs:
            terminations.extend(render_terminations(text, last))
    records.sort(key=lambda r: r.stamp)
    # How each start of the game ended. The copy in the game folder keeps every start.
    starts: list[str] = []
    startup_logs = sorted(folder.rglob("RFAB_Online_Startup.log"), key=lambda p: (0 if "game" in p.parts else 1, str(p)))
    if startup_logs:
        starts = summarize_starts(startup_logs[0].read_text(encoding="utf-8", errors="replace"))
    if not records:
        if terminations:
            out.append("  no crash record with a call stack; the server ended itself:")
            out.extend(terminations)
            out.extend(starts)
            return out
        out.append("  no crash record in " + (", ".join(str(s.relative_to(folder)) for s in sources) if sources else "the folder (no RFAB_Online_Crash.log, no STServer_Crash.log, no tp_client log)"))
        out.extend(starts)
        return out
    first = max(0, len(records) - last) if last > 0 else 0
    out.append(f"  {len(records)} crash record(s)" + (f", showing the last {len(records) - first}" if first else ""))
    for number, record in enumerate(records[first:], first + 1):
        out.extend(render_record(number, record, maps, game))
    out.extend(terminations)
    out.extend(starts)
    return out


# --------------------------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------------------------
_SAMPLE_MAP = """ SkyrimSE

 Timestamp is 6736a5c1 (Thu Nov 14 11:20:01 2026)

 Preferred load address is 0000000140000000

 Start         Length     Name                   Class
 0001:00000000 03f00000H .seg01                  DATA
 0002:00000000 00300000H .zdata                  CODE
 0003:00000000 00010000H .rdata                  DATA

  Address         Publics by Value              Rva+Base               Lib:Object

 0000:00000000       __guard_flags              0000000000000100     <absolute>
 0002:00001000       ?OnUpdate@CharacterService@@QEAAXAEBUUpdateEvent@@@Z 0000000144001000 f   SkyrimTogetherClient.lib:CharacterService.obj
 0002:00001400       ??0World@@QEAA@XZ          0000000144001400 f   SkyrimTogetherClient.lib:World.obj
 0002:00002000       ?HookDamageActor@@YA_NPEAVActor@@M0_N@Z 0000000144002000 f i SkyrimTogetherClient.lib:Actor.obj
 0003:00000040       ?kTable@@3QBHB             0000000144300040     SkyrimTogetherClient.lib:Tables.obj

 entry point at        0002:00000100

 Static symbols

 0002:00003000       ?Helper@?A0x1234@@YAXXZ    0000000144003000 f   SkyrimTogetherClient.lib:Helper.obj
"""

_SAMPLE_IDS = """ae_id,se_1_5_97_id,se_rva_hex,kind,ref_count,first_reference,source,symbol
37673,36345,0x5E4C70,hook,1,Code/client/Games/Skyrim/Actor.cpp:2590,se_ae.csv,Actor::DamageActor
13620,13620,0x16D230,pointer,1,Code/client/Games/ModManager.cpp:89,se_ae.csv,
"""

_SAMPLE_LOG = (
    "[2026-11-14 21:03:11.512] [UNHANDLED_CRASH] TID=4242 Code=0xC0000005 RIP=0x1405E4C9A FaultAddr=0x38 "
    "Module='D:\\Games\\RFAB\\SkyrimSE.exe' Base=0x140000000 RVA=+0x5E4C9A RSP=0x1 RBP=0x2 Bytes=[48 8B 01 ]\r\n"
    "    [unwind #0] SkyrimSE.exe+0x5E4C9A (0x1405E4C9A)\r\n"
    "    [unwind #1] SkyrimSE.exe[TiltedTrampoline]+0x4002040 (0x144002040)\r\n"
    "    [unwind #2] SkyrimSE.exe[TiltedTrampoline]+0x4001010 (0x144001010)\r\n"
    "    [unwind #3] SkyrimSE.exe+0x900000 (0x140900000)\r\n"
    "    [unwind #4] KERNEL32.DLL+0x17614 (0x7FF8AAAA7614)\r\n"
    "    [crumbs] events=812 packets=40311 hooks=90211 dropped=0 (newest last; a/b: see the RFAB_*_CRUMB call sites)\r\n"
    "    [crumb event -5210ms T4242] world-load-end a=0x1 b=0x0 x1\r\n"
    "    [crumb event -38ms T4242] remove-character a=0x1A3 b=0x0 x1\r\n"
    "    [crumb packet -40ms T4242] packet-in a=0x1 b=0x58 x1\r\n"
    "    [crumb packet -12ms T9000] packet-out a=0x2 b=0x0 x4\r\n"
    "    [crumb hook -3ms T4242] HookActorProcess a=0x0 b=0xFF000A12 x57\r\n"
    "    [crumb hook -0ms T4242] HookDamageActor a=0xFF000A12 b=0x14 x1\r\n"
    "[2026-11-14 21:40:00.001] [UNHANDLED_CRASH] TID=7 Code=0xE06D7363 RIP=0x7FF8BBBB0000 FaultAddr=0x0 "
    "Module='C:\\Windows\\System32\\KERNELBASE.dll' Base=0x7FF8BBB00000 RVA=+0xB0000 Bytes=[] | CppThrowOrigin=[x] Magic=0x19930520 "
    "Type=[.?AVsystem_error@std@@] what()=[\"No mapping for the Unicode character exists in the target multi-byte code page.\"]\r\n"
    "    [unwind #0] KERNELBASE.dll+0xB0000 (0x7FF8BBBB0000)\r\n"
    "    [unwind #1] SkyrimSE.exe[TiltedTrampoline]+0x4003008 (0x144003008)\r\n"
)


def self_test() -> int:
    failures: list[str] = []

    def expect(condition: bool, what: str) -> None:
        if not condition:
            failures.append(what)

    expect(demangle("?OnUpdate@CharacterService@@QEAAXAEBUUpdateEvent@@@Z") == "CharacterService::OnUpdate", "demangle: member function")
    expect(demangle("??0World@@QEAA@XZ") == "World::{ctor}", "demangle: constructor")
    expect(demangle("?HookDamageActor@@YA_NPEAVActor@@M0_N@Z") == "HookDamageActor", "demangle: free function")
    expect(demangle("?Get@Service@Inner@Outer@@QEAAXXZ") == "Outer::Inner::Service::Get", "demangle: nested scope")
    expect(demangle("main") == "main", "demangle: undecorated name")
    templated = demangle("?Method5@?$Template@V?$basic_string@DU?$char_traits@D@std@@@std@@@Class7@Namespace@@QEAAXXZ")
    expect(templated.startswith("Method5  (?Method5@?$Template"), f"demangle: template scope keeps the plain name and the raw one: {templated}")
    expect(demangle("??$Make@H@Factory@@YAHXZ").startswith("Make<...>  ("), "demangle: function template")
    expect(demangle("??1World@@QEAA@XZ") == "World::{dtor}", "demangle: destructor")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "SkyrimSE.map").write_text(_SAMPLE_MAP, encoding="utf-8")
        (root / "ids.csv").write_text(_SAMPLE_IDS, encoding="utf-8")
        linker_map = parse_map(root / "SkyrimSE.map")
        game = GameIdMap.load(root / "ids.csv")

    expect(linker_map.module == "SkyrimSE" and linker_map.preferred_base == 0x140000000, "map: header")
    expect(len(linker_map.symbols) == 5, f"map: {len(linker_map.symbols)} symbols (absolute ones are skipped, static ones kept)")
    expect(linker_map.code_ranges == [(0x4000000, 0x4300000)], f"map: code range {linker_map.code_ranges}")
    found = linker_map.lookup(0x4002040)
    expect(bool(found) and found[0].name.startswith("?HookDamageActor") and found[1] == 0x40, "map: address inside a function")
    expect(linker_map.lookup(0x4300050) is None, "map: an address in a data section is not a function")
    expect(linker_map.lookup(0x5E4C9A) is None, "map: an address in the game image is not ours")

    report = "\n".join(symbolize_text(_SAMPLE_LOG, [linker_map], game))
    expect("=== crash 1  2026-11-14 21:03:11.512" in report, "report: first record and its time")
    expect("access violation at 0x38: a null pointer" in report, "report: the fault address is explained")
    expect("GAME  +0x2A after Address Library id 36345 (Actor::DamageActor)" in report, "report: game address near a hooked function")
    expect("referred to at Code/client/Games/Skyrim/Actor.cpp:2590" in report, "report: where the client refers to that function")
    expect("OURS  HookDamageActor +0x40  [SkyrimTogetherClient.lib:Actor.obj]" in report, "report: our hook, although the logger called it a trampoline")
    expect("OURS  CharacterService::OnUpdate +0x10" in report, "report: our member function")
    expect("GAME  a function the client neither hooks nor calls" in report, "report: unknown game function")
    expect("other module" in report, "report: a system module")
    expect("=== crash 2" in report and "C++ exception that nothing caught" in report, "report: second record")
    expect("C++ type .?AVsystem_error@std@@  what(): No mapping for the Unicode character" in report, "report: C++ exception type and message")
    expect("Helper  (?Helper@?A0x1234@@YAXXZ) +0x8" in report, "report: a static function (anonymous namespace) from the map")

    expect("before the fault (oldest first" in report, "crumbs: the section is there")
    expect("      * -    40 ms  packet  packet-in " in report and ", 88 bytes" in report, "crumbs: an inbound packet with its size, on the crashing thread")
    expect("        -    12 ms  packet  packet-out " in report and "  x4" in report, "crumbs: another thread is not starred, repeats are counted")
    expect("hook    HookDamageActor form FF000A12 (other 14)" in report, "crumbs: a hook with its subject and the other party")
    expect("hook    HookActorProcess form FF000A12  x57" in report, "crumbs: a per-frame hook collapses into one row")
    expect("event   world-load-end (in a session)" in report and "event   remove-character: server id 1A3" in report, "crumbs: event values are named")
    for line in (REPO_ROOT / "Code" / "client").rglob("*.cpp") if (REPO_ROOT / "Code" / "client").is_dir() else []:
        for name in re.findall(r'RFAB_EVENT_CRUMB\("([^"]+)"', line.read_text(encoding="utf-8", errors="replace")):
            expect(name in EVENT_CRUMBS, f"crumbs: event '{name}' ({line.name}) has no description in EVENT_CRUMBS")
    expect(report.index("world-load-end") < report.index("packet-in ") < report.index("remove-character: server id 1A3") < report.index("packet-out ") < report.index("HookDamageActor form"),
           "crumbs: the three rings become one timeline, oldest first")
    opcodes = load_opcode_names()
    if opcodes:
        expect(opcodes["ClientOpcode"][0] == "kAuthenticationRequest" and "kClientOpcodeMax" in opcodes["ClientOpcode"], "crumbs: client opcode names come from Opcodes.h")
        expect(describe_crumb(Crumb("packet", 1, 1, "packet-out", 0, 0, 1), opcodes) == "packet-out kAuthenticationRequest", "crumbs: opcode number becomes a name")
        expect(describe_crumb(Crumb("packet", 1, 1, "packet-in", 9999, 12, 1), opcodes) == "packet-in opcode 9999, 12 bytes", "crumbs: an opcode outside the table stays a number")
    second = [r for r in parse_records(_SAMPLE_LOG)][1]
    expect(not second.crumbs, "crumbs: they belong to the record they follow")

    without_map = "\n".join(symbolize_text(_SAMPLE_LOG, [], game))
    expect("pass --map SkyrimSE.map" in without_map, "report: says what is missing when no map is given")
    expect("GAME  +0x2A after Address Library id 36345" in without_map, "report: game addresses need no map")

    short = "\n".join(symbolize_text("[x] Unhandled crash: code=0xC0000409 RIP=0x144001400 module='D:\\G\\SkyrimSE.exe' +0x4001400", [linker_map], game))
    expect("fast fail" in short and "OURS  World::{ctor} +0x0" in short, "report: the one-line record of tp_client.log")

    expect(symbolize_text("nothing here", [], game)[-1].startswith("no crash record found"), "report: a log without crashes")
    expect("showing the last 1" in "\n".join(symbolize_text(_SAMPLE_LOG, [linker_map], game, last=1)), "report: --last")

    # The compact form: what the build installs next to the client.
    compact = export_compact(linker_map)
    expect(compact[:2] == b"\x1f\x8b" and compact == export_compact(linker_map), "compact: gzip, and the same bytes every time")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "client").mkdir()
        (root / "client" / "RFAB_Online_Symbols.gz").write_bytes(compact)
        reread = parse_map(root / "client" / "RFAB_Online_Symbols.gz")
        expect(reread.module == "SkyrimSE" and reread.timestamp == "6736a5c1" and reread.code_ranges == linker_map.code_ranges, "compact: header survives")
        expect(len(reread.symbols) == 4, f"compact: {len(reread.symbols)} symbols (functions only)")
        found = reread.lookup(0x4002040)
        expect(bool(found) and found[0].name.startswith("?HookDamageActor") and found[1] == 0x40, "compact: lookup")

        # A whole report folder: the crash log twice (client folder and game root), symbols, REPORT.txt.
        (root / "client" / "RFAB_Online_Crash.log").write_text(_SAMPLE_LOG, encoding="utf-8")
        (root / "game").mkdir()
        (root / "game" / "RFAB_Online_Crash.log").write_text(_SAMPLE_LOG, encoding="utf-8")
        (root / "REPORT.txt").write_text(
            "[binaries]\nclient\\SkyrimSE.exe | size=1 | version=4.1.1 | pe_timestamp=6736A5C1 | sha256=00\n", encoding="utf-8")
        whole = "\n".join(symbolize_report(root, [], game, 10))
        expect("2 crash record(s)" in whole, "folder: the two copies of the crash log are one set of records")
        expect("the symbols belong to the installed binary" in whole, "folder: matching timestamp is confirmed")
        expect("OURS  HookDamageActor +0x40" in whole, "folder: symbols from the report are used")
        (root / "REPORT.txt").write_text("client\\SkyrimSE.exe | size=1 | pe_timestamp=11111111 | sha256=00\n", encoding="utf-8")
        expect("WARNING skyrimse.exe: installed binary has timestamp 11111111" in "\n".join(symbolize_report(root, [], game, 10)),
               "folder: a binary that does not match the symbols is called out")
        # The game's own SkyrimSE.exe is listed too; it must not hide the client binary of the same name.
        (root / "REPORT.txt").write_text(
            "client\\SkyrimSE.exe | size=1 | pe_timestamp=6736A5C1 | sha256=00\ngame\\SkyrimSE.exe | size=2 | pe_timestamp=5DCAE282 | sha256=11\n", encoding="utf-8")
        both = "\n".join(symbolize_report(root, [], game, 10))
        expect("the symbols belong to the installed binary" in both and "WARNING" not in both,
               "folder: the game's SkyrimSE.exe next to the client's does not make the symbols look wrong")
        (root / "client" / "RFAB_Online_Symbols.gz").unlink()
        expect("NO SYMBOLS in the report" in "\n".join(symbolize_report(root, [], game, 10)), "folder: missing symbols are called out")

    # The server's crash log: a record in the client's layout inside STServer.dll, and a [TERMINATE] line.
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "STServer.map").write_text(
            " STServer\n\n Timestamp is 6736b000 (Thu Nov 14 21:00:00 2026)\n\n Preferred load address is 0000000180000000\n\n"
            " Start         Length     Name                   Class\n"
            " 0001:00000000 00100000H .text                   CODE\n\n"
            "  Address         Publics by Value              Rva+Base               Lib:Object\n\n"
            " 0001:00002000       ?RfabServerCrashSelfTestFault@@YAXXZ 0000000180003000 f   STServer.lib:main.obj\n"
            " 0001:00004000       ?Update@GameServer@@QEAAXXZ 0000000180005000 f   STServer.lib:GameServer.obj\n",
            encoding="utf-8")
        server_map = parse_map(Path(tmp) / "STServer.map")
    expect(server_map.module == "STServer" and len(server_map.symbols) == 2, f"server: the server's map is read ({server_map.module}, {len(server_map.symbols)} symbols)")
    server_log = (
        "[2026-11-14 22:00:01.250] [UNHANDLED_CRASH] TID=11 Code=0xC0000005 RIP=0x7FFA00003010 FaultAddr=0x0 "
        "Module='C:\\RFAB\\RFAB_Online_Server\\STServer.dll' Base=0x7FFA00000000 RVA=+0x3010 RSP=0x1 RBP=0x2 RAX=0x0 RBX=0x0 RCX=0x0 RDX=0x0 "
        "PeTimestamp=0x6736B000 Process=[server]\r\n"
        "    [unwind #0] STServer.dll+0x3010 (0x7FFA00003010)\r\n"
        "    [unwind #1] SkyrimTogetherServer.exe+0x1234 (0x7FF600001234)\r\n"
        "[2026-11-14 22:05:00.000] [UNHANDLED_CRASH] TID=11 Code=0xE06D7363 RIP=0x7FF8BBBB0000 FaultAddr=0x0 "
        "Module='C:\\Windows\\System32\\KERNELBASE.dll' Base=0x7FF8BBB00000 RVA=+0xB0000 RSP=0x1 RBP=0x2 RAX=0x0 RBX=0x0 RCX=0x0 RDX=0x0 "
        "PeTimestamp=0x11111111 Process=[server] | Type=[.?AVruntime_error@std@@] what()=[\"bad file name\"]\r\n"
        "    [unwind #0] KERNELBASE.dll+0xB0000 (0x7FF8BBBB0000)\r\n"
        "    [unwind #1] STServer.dll+0x5020 (0x7FFA00005020)\r\n"
        "[2026-11-14 22:05:00.010] [TERMINATE] the server stopped itself: std::terminate was called; what()=[\"bad file name\"]\r\n")
    server_report = "\n".join(symbolize_text(server_log, [server_map], game))
    expect("OURS  RfabServerCrashSelfTestFault +0x10" in server_report, "server: a fault inside STServer.dll is named by the server's symbols")
    expect("OURS  GameServer::Update +0x20" in server_report, "server: the frame that threw is named")
    expect("C++ type" in server_report and "bad file name" in server_report, "server: the thrown type and text are shown")
    expect("=== std::terminate: 1 record(s)" in server_report and "what(): bad file name" in server_report, "server: the terminate record is shown")
    only_terminate = "\n".join(symbolize_text("[2026-11-14 22:05:00.010] [TERMINATE] the server stopped itself: std::terminate was called; what()=[\"\"]\n", [], game))
    expect("std::terminate: 1 record(s)" in only_terminate and "what(): (empty)" in only_terminate, "server: a terminate record alone is still reported")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "server" / "logs").mkdir(parents=True)
        (root / "server" / "logs" / "STServer_Crash.log").write_text(server_log, encoding="utf-8")
        (root / "server" / "RFAB_Online_Server_Symbols.gz").write_bytes(export_compact(server_map))
        folder_report = "\n".join(symbolize_report(root, [], game, 10))
        expect("2 crash record(s)" in folder_report and "RfabServerCrashSelfTestFault" in folder_report and "std::terminate: 1 record(s)" in folder_report,
               "folder: the server's crash log is read and named with the server's symbols")

    # 4.1.2: the endings that raise no exception, and how each start of the game ended.
    hang_log = (
        "[2026-11-16 20:10:05.000] [UNHANDLED_CRASH] TID=4242 Code=0xC0000005 RIP=0x1405E4C9A FaultAddr=0x38 "
        "Module='D:\\Games\\Skyrim\\SkyrimSE.exe' Base=0x140000000 RVA=+0x5E4C9A RSP=0x1 RBP=0x2\r\n"
        "    [unwind #0] SkyrimSE.exe+0x5E4C9A (0x1405E4C9A)\r\n"
        "[2026-11-16 21:03:11.512] [HANG] the game's main loop has not turned for 20 s (running; window does not answer; main thread TID=4242; 5000 turns before; record 1). "
        "If the game was closed after this line, this is the reason and there is no crash record.\r\n"
        "    [unwind #0] ntdll.dll+0x9F8A4 (0x7FF8CCCCF8A4)\r\n"
        "    [stack #1] SkyrimSE.exe[TiltedTrampoline]+0x4001010 (0x144001010)\r\n"
        "    [stack #2] SkyrimSE.exe+0x5E4C9A (0x1405E4C9A)\r\n"
        "    [crumbs] events=1 packets=0 hooks=1 dropped=0 (newest last; a/b: see the RFAB_*_CRUMB call sites)\r\n"
        "    [crumb event -61000ms T4242] blocking-screen a=0x2 b=0x3C x1\r\n"
        "    [crumb hook -20010ms T4242] HookSaveImpl a=0x0 b=0x0 x1\r\n"
        "[2026-11-16 21:20:00.000] [TERMINATE] TID=77 the game was stopped by std::terminate: an exception left a thread or a noexcept function "
        "(the stack below names it); what()=[\"vector too long\"]\r\n"
        "    [unwind #0] ucrtbase.dll+0x1234 (0x7FF8AAAA1234)\r\n"
        "    [unwind #1] SkyrimSE.exe[TiltedTrampoline]+0x4001410 (0x144001410)\r\n"
        "[2026-11-16 21:30:00.000] [PURECALL] TID=78 a pure virtual function was called: an object was used while it was being built or after it was destroyed\r\n"
        "    [unwind #0] SkyrimSE.exe[TiltedTrampoline]+0x4002040 (0x144002040)\r\n")
    hang_records = parse_records(hang_log)
    expect([r.kind for r in hang_records] == ["crash", "HANG", "TERMINATE", "PURECALL"], f"events: four records in order ({[r.kind for r in hang_records]})")
    expect(len(hang_records[0].frames) == 1 and not hang_records[0].crumbs, "events: a hang's frames and crumbs do not land on the crash before it")
    expect(len(hang_records[1].frames) == 3 and [f[3] for f in hang_records[1].frames] == [False, True, True] and len(hang_records[1].crumbs) == 2,
           "events: the hang keeps its unwind frame, its scanned frames and its crumbs")
    expect(hang_records[1].thread == 4242, "events: the hang names the main thread")
    hang_report = "\n".join(symbolize_text(hang_log, [linker_map], game))
    expect("=== record 2  2026-11-16 21:03:11.512  hang: the game's main loop stopped turning" in hang_report, "events: the hang's heading")
    expect("~1      SkyrimSE.exe+0x4001010" in hang_report and "OURS  CharacterService::OnUpdate +0x10" in hang_report, "events: a scanned frame is marked and named")
    expect("blocking-screen: a loading (1) or black (2) screen, kind 2, has been up for 60 s" in hang_report, "events: the stuck screen crumb is explained")
    expect("=== record 3" in hang_report and "what()=[\"vector too long\"]" in hang_report and "OURS  World::{ctor} +0x10" in hang_report,
           "events: the client's terminate record has its text and its call stack")
    expect("=== record 4" in hang_report and "pure virtual call" in hang_report and "OURS  HookDamageActor +0x40" in hang_report, "events: pure call")
    expect(len(parse_records("[2026-11-14 22:05:00.010] [TERMINATE] the server stopped itself: std::terminate was called; what()=[\"x\"]\n")) == 0,
           "events: the server's one-line terminate record is not a call-stack record")

    startup_log = (
        "[BOOT] Checkpoint | ExeVersion='1.5.97.0' | GamePathSet=1\r\n"
        "[BOOT OK] BeginMain() + StartSKSE() completed; entering WinMain!\r\n"
        "[2026-11-16 20:00:00.000] [BOOT] Checkpoint | Build='4.1.2-abc' | PID=100 | ExeVersion='1.5.97.0' | GamePathSet=1\r\n"
        "[2026-11-16 20:00:05.000] [BOOT OK] BeginMain() + StartSKSE() completed; entering WinMain!\r\n"
        "[2026-11-16 20:00:40.000] [BOOT] the game's main loop is turning (first turn 40 s after start, TID=1)\r\n"
        "[2026-11-16 20:30:00.000] [EXIT] the game is quitting (main loop saw the quit flag) (ran for 1800 s, 99 main-loop turns)\r\n"
        "[2026-11-16 20:30:01.000] [EXIT] the process is ending the normal way\r\n"
        "[2026-11-16 20:31:00.000] [BOOT] Checkpoint | Build='4.1.2-abc' | PID=101 | ExeVersion='1.5.97.0' | GamePathSet=1\r\n"
        "[2026-11-16 20:31:05.000] [BOOT OK] BeginMain() + StartSKSE() completed; entering WinMain!\r\n"
        "[2026-11-16 20:36:05.000] [HANG] the game was started 300 s ago and its main loop has not turned once (no game window; main thread TID=1; record 1). x\r\n"
        "[2026-11-16 20:40:00.000] [BOOT] Checkpoint | Build='4.1.2-abc' | PID=102 | ExeVersion='1.5.97.0' | GamePathSet=1\r\n"
        "[2026-11-16 20:40:05.000] [BOOT OK] BeginMain() + StartSKSE() completed; entering WinMain!\r\n"
        "[2026-11-16 20:40:40.000] [BOOT] the game's main loop is turning (first turn 40 s after start, TID=1)\r\n"
        "[2026-11-16 20:50:00.000] [HANG] the game's main loop has not turned for 20 s (running; window does not answer; main thread TID=1; 5 turns before; record 1). x\r\n"
        "[2026-11-16 20:50:15.000] [HANG-END] the main loop turns again; it stood for 35 s\r\n"
        "[2026-11-16 21:00:00.000] [BOOT] Checkpoint | Build='4.1.2-abc' | PID=103 | ExeVersion='1.5.97.0' | GamePathSet=1\r\n"
        "[2026-11-16 21:00:05.000] [BOOT OK] BeginMain() + StartSKSE() completed; entering WinMain!\r\n"
        "[2026-11-16 21:00:40.000] [BOOT] the game's main loop is turning (first turn 40 s after start, TID=1)\r\n"
        "[2026-11-16 21:10:00.000] [UNHANDLED_CRASH] TID=1 Code=0xC0000005 RIP=0x1 FaultAddr=0x0 Module='x' Base=0x0 RVA=+0x1\r\n")
    starts = summarize_starts(startup_log)
    expect(len(starts) == 7 and starts[1].startswith("=== starts of the game: 5"), f"starts: five starts are listed ({len(starts)} lines)")
    if len(starts) == 7:
        expect("no time" in starts[2] and "unknown (this version wrote no ending)" in starts[2], "starts: a start written by an old version claims nothing")
        expect("2026-11-16 20:00:00.000 build 4.1.2-abc: reached main menu; ended normally" in starts[3], f"starts: a normal session ({starts[3].strip()})")
        expect("reached game start; HUNG" in starts[4], f"starts: a start that never reached the main loop ({starts[4].strip()})")
        expect("reached main menu, 1 long stall(s) that ended; NO RECORD OF AN ENDING" in starts[5], f"starts: a stall that ended, then the process disappeared ({starts[5].strip()})")
        expect("reached main menu; CRASHED" in starts[6], f"starts: a crash ({starts[6].strip()})")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "game").mkdir()
        (root / "game" / "RFAB_Online_Startup.log").write_text(startup_log, encoding="utf-8")
        (root / "game" / "RFAB_Online_Crash.log").write_text(hang_log, encoding="utf-8")
        folder_report = "\n".join(symbolize_report(root, [], game, 10))
        expect("4 crash record(s)" in folder_report and "=== starts of the game: 5" in folder_report and "hang: the game's main loop stopped turning" in folder_report,
               "folder: hang records and the list of starts are part of the report")

    if failures:
        print("symbolize_crash self-test FAILED:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("symbolize_crash self-test: map parsing, game ids, both record formats, C++ exceptions, breadcrumbs, the server's crash log, "
          "hang and terminate records, the list of starts")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("log", nargs="?", help="an unpacked report folder, RFAB_Online_Crash.log, tp_client.log, or - for stdin")
    parser.add_argument("--last", type=int, default=0, help="only the last N crash records (default: all; 10 for a report folder)")
    parser.add_argument("--export-symbols", metavar="MAP", help="write the compact symbol table of a linker map (needs --out)")
    parser.add_argument("--out", help="output file of --export-symbols")
    parser.add_argument("--map", action="append", default=[], help="linker map of the same build (SkyrimSE.map, STServer.map); repeatable")
    parser.add_argument("--id-map", default=str(DEFAULT_ID_MAP), help="game function list (default: docs/se-1597-id-map.csv)")
    parser.add_argument("--address", action="append", default=[], help="explain one address, e.g. SkyrimSE.exe+0x2F3A10; repeatable")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.export_symbols:
        if not args.out:
            print("--export-symbols needs --out", file=sys.stderr)
            return 2
        source = Path(args.export_symbols)
        if not source.is_file():
            print(f"map file not found: {source}", file=sys.stderr)
            return 2
        linker_map = parse_map(source)
        functions = sum(1 for symbol in linker_map.symbols if symbol.is_function)
        if functions == 0:
            print(f"{source}: no function symbols found - not a linker map?", file=sys.stderr)
            return 2
        data = export_compact(linker_map)
        Path(args.out).write_bytes(data)
        # One line, picked up by ci_diagnostics.py as a result line of the CI step.
        print(f"[RFAB_RESULT] symbols {linker_map.module}: {functions} functions, timestamp {linker_map.timestamp or '?'}, "
              f"{source.stat().st_size / 1048576:.1f} MB map -> {len(data) / 1048576:.2f} MB {Path(args.out).name}")
        return 0
    if not args.log and not args.address:
        parser.print_usage(sys.stderr)
        return 2

    maps = []
    for map_path in args.map:
        path = Path(map_path)
        if not path.is_file():
            print(f"map file not found: {map_path}", file=sys.stderr)
            return 2
        maps.append(parse_map(path))
    game = GameIdMap.load(Path(args.id_map))

    for address in args.address:
        match = _ADDRESS.match(address.strip())
        if not match:
            print(f"cannot read address {address!r}: expected <module>+0x<rva>", file=sys.stderr)
            return 2
        print(f"{address}\n    {explain(match.group('module'), int(match.group('rva'), 16), maps, game)}")
    if args.log:
        if args.log != "-" and Path(args.log).is_dir():
            lines = symbolize_report(Path(args.log), maps, game, args.last if args.last > 0 else 10)
        else:
            lines = symbolize_text(read_input(args.log), maps, game, args.last)
        for line in lines:
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
