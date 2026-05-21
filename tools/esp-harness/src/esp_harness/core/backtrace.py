"""Backtrace decoder — turn raw `0x40083a91` addresses into source locations.

ESP-IDF panic backtraces look like::

    Backtrace: 0x40083a91:0x3ffb47e0 0x4008b7f1:0x3ffb4810 0x4008f3c9:0x3ffb4830

Each pair is ``<code_addr>:<stack_ptr>``. We feed the code addresses to
`xtensa-esp32-elf-addr2line` (resolved from the ESP-IDF toolchain that
`idf_runner` puts on PATH) along with the project's ELF file, and parse the
output back into structured frames.

This is the moral equivalent of `idf.py monitor` decoding addresses inline,
except it works on saved log text, runs offline, and emits JSON for an AI to
branch on.

Usage::

    from esp_harness.core import backtrace
    decoded = backtrace.decode_text(captured_text, project_dir=Path("..."))
    # → [ {"raw": "Backtrace: 0x...", "frames": [{...}, ...]}, ... ]
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable

from esp_harness.core import idf_runner

# A "Backtrace:" line followed by one or more 0xADDR:0xADDR pairs (space sep).
# The leading colon may or may not have a space after it. Tolerate both.
_BT_LINE_RE = re.compile(
    r"Backtrace:?\s*((?:0x[0-9a-fA-F]+(?::0x[0-9a-fA-F]+)?\s*)+)",
    re.IGNORECASE,
)

# Address tokens *inside* the backtrace line.
_ADDR_RE = re.compile(r"0x[0-9a-fA-F]+")

# A single addr2line `-pfiaC` output line. Examples we handle::
#
#   0x40083a91: my_function at C:/path/file.c:42
#   0x4008b7f1: other_fn at ??:0
#    (inlined by) inner_fn at /path/file.c:35
#   0x4008b7f1: ?? ??:0
_FRAME_RE = re.compile(
    r"^(?P<addr>0x[0-9a-fA-F]+)?:?\s*"
    r"(?:\(inlined by\)\s*)?"
    r"(?P<fn>[^\s][^\n]*?)\s+at\s+"
    r"(?P<file>[^\s:]+):(?P<line>\d+|\?)",
)


@dataclass
class Frame:
    addr: str | None
    function: str
    file: str
    line: int | None
    inlined: bool = False

    def to_dict(self) -> dict:
        return {
            "addr": self.addr,
            "function": self.function,
            "file": self.file,
            "line": self.line,
            "inlined": self.inlined,
        }


@dataclass
class Decoded:
    raw: str
    frames: list[Frame]

    def to_dict(self) -> dict:
        return {"raw": self.raw, "frames": [f.to_dict() for f in self.frames]}


def find_elf(project_dir: Path | None) -> Path | None:
    """Return the first `.elf` in `<project>/build/` if one exists."""
    if project_dir is None:
        return None
    build_dir = project_dir / "build"
    if not build_dir.is_dir():
        return None
    elfs = sorted(build_dir.glob("*.elf"))
    return elfs[0] if elfs else None


def _addr2line_binary(env: dict[str, str]) -> str | None:
    """Resolve the full path to `xtensa-esp32-elf-addr2line` using the
    PATH from `env` (rather than the parent process's PATH).

    On Windows, `subprocess.run(['xtensa-...'], env=custom_env)` does NOT
    consult `custom_env['PATH']` to find the executable — it uses the
    parent process's PATH, which won't have the ESP-IDF toolchain. So
    we resolve manually before calling subprocess.

    Returns None if not found. Xtensa is the only path supported today;
    RISC-V boards (C3/C6/H2/P4) need `riscv32-esp-elf-addr2line` — TODO
    once we have a non-Xtensa project to test against.
    """
    name = "xtensa-esp32-elf-addr2line"
    # Windows quirk: a subprocess env dict will often have BOTH `Path`
    # (the live Windows env) and `PATH` (passed-through from POSIX-style
    # callers / Git Bash). The Windows-style `Path` is the one with the
    # ESP-IDF toolchain prepended; `PATH` is typically the stale parent
    # process's POSIX-style copy. Try both, and take whichever resolves.
    candidates = [
        env.get("Path"),
        env.get("PATH"),
        os.environ.get("Path"),
        os.environ.get("PATH"),
    ]
    for p in candidates:
        if not p:
            continue
        found = shutil.which(name, path=p)
        if found:
            return found
        if not name.endswith(".exe"):
            found = shutil.which(name + ".exe", path=p)
            if found:
                return found
    return None


def _parse_addr2line(raw_lines: list[str], code_addrs: list[str]) -> list[Frame]:
    """addr2line -pfiaC emits one or more lines per requested address; the
    first line carries the address prefix and any subsequent (inlined by)
    lines lack it. We walk the output sequentially, attaching each line to
    the most recent address."""
    frames: list[Frame] = []
    current_addr: str | None = None
    addr_idx = 0
    for ln in raw_lines:
        ln = ln.rstrip()
        if not ln:
            continue
        m = _FRAME_RE.match(ln)
        if not m:
            continue
        addr_in_line = m.group("addr")
        if addr_in_line:
            current_addr = addr_in_line
            inlined = False
            if addr_idx < len(code_addrs):
                addr_idx += 1
        else:
            inlined = True  # subsequent (inlined by) line
        fn = m.group("fn").strip()
        file = m.group("file")
        line_str = m.group("line")
        line_num: int | None = int(line_str) if line_str.isdigit() else None
        # addr2line returns "??" for unknown function/file; mark and keep.
        if fn == "??":
            fn = "(unknown)"
        frames.append(
            Frame(
                addr=current_addr,
                function=fn,
                file=file,
                line=line_num,
                inlined=inlined,
            )
        )
    return frames


def decode_text(
    text: str,
    project_dir: Path | None = None,
    elf_path: Path | None = None,
    timeout: float = 10.0,
) -> list[Decoded]:
    """Find every `Backtrace:` line in `text` and return decoded frames.

    Returns an empty list if no backtraces are found, or if we can't find
    an ELF, or if the toolchain isn't available — never raises for the
    common cases.
    """
    if elf_path is None:
        elf_path = find_elf(project_dir)
    if elf_path is None or not elf_path.exists():
        return []

    matches = list(_BT_LINE_RE.finditer(text))
    if not matches:
        return []

    # Reuse the IDF env so addr2line is on PATH.
    try:
        env = idf_runner.build_subprocess_env()
    except idf_runner.EnvError:
        return []

    a2l = _addr2line_binary(env)
    if a2l is None:
        return []

    out: list[Decoded] = []
    for m in matches:
        addrs = _ADDR_RE.findall(m.group(0))
        if not addrs:
            continue
        # ESP-IDF backtraces interleave code_addr / stack_ptr; the code
        # addresses are the EVEN-INDEXED ones (0, 2, 4, …). If the input
        # is a flat list of addresses without the `:sp` pairs, we just
        # use all of them.
        if ":" in m.group(0):
            code_addrs = addrs[::2]
        else:
            code_addrs = addrs
        if not code_addrs:
            continue

        cmd = [a2l, "-pfiaC", "-e", str(elf_path)] + code_addrs
        try:
            result = subprocess.run(
                cmd, env=env, capture_output=True, text=True, timeout=timeout
            )
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue
        if result.returncode != 0:
            continue
        frames = _parse_addr2line(result.stdout.splitlines(), code_addrs)
        if frames:
            out.append(Decoded(raw=m.group(0), frames=frames))

    return out


def decoded_to_jsonable(decoded: Iterable[Decoded]) -> list[dict]:
    return [d.to_dict() for d in decoded]
