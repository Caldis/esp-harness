"""`esp-harness screenshot` — capture the device's framebuffer as a PNG.

Pairs with the firmware-side `?dump` command, implemented in
`components/aurora-harness/src/screenshot.c` and registered automatically
by `harness_default_register()`. The device base64-encodes a downsampled
RGB565 framebuffer; we decode + reassemble + save.

Usage:
    esp-harness screenshot --out aurora.png --size 128
    esp-harness screenshot --port COM9 --out s.png --size 256 --json
"""

from __future__ import annotations

import argparse
import base64
import re
import time
from pathlib import Path

try:
    from PIL import Image  # type: ignore[import-untyped]
except ImportError as e:
    raise ImportError("Pillow is required. pip install Pillow") from e

from esp_harness.core import console_session
from esp_harness.core import ports as ports_mod
from esp_harness.exit_codes import (
    AMBIGUOUS_DEVICE,
    DEVICE_BUSY,
    GENERIC_ERROR,
    NO_DEVICE,
    OK,
)
from esp_harness.output import Output


_META_RE = re.compile(r"w=(?P<w>\d+)\s+h=(?P<h>\d+)\s+fmt=(?P<fmt>\S+)\s+bytes=(?P<bytes>\d+)")


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "screenshot",
        help="Capture device framebuffer to a PNG (requires firmware-side ?dump).",
    )
    p.add_argument("--port", default=None, help="COM port (auto-detect if omitted).")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument(
        "--out",
        type=Path,
        default=Path("screenshot.png"),
        help="Output PNG path (default: screenshot.png).",
    )
    p.add_argument(
        "--size",
        type=int,
        default=128,
        help="Downsample size sent to firmware via ?dump w=N (default 128). "
             "Firmware accepts 32..2048 but additionally caps at panel "
             "dimensions; the actual emitted size is in the OK line's "
             "`w_actual=` field (gap G-F1b).",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=None,
        help="Seconds to wait for the full reply. Default scales with size: "
             "~0.0003s/byte → 96px=3s, 128px=4s, 192px=10s, 256px=18s.",
    )
    add_common_flags(p)


def _rgb565_to_rgb888(data: bytes, w: int, h: int) -> bytes:
    """Convert little-endian RGB565 to 24-bit RGB8 bytes.

    Tolerates short input: missing tail is treated as black pixels so the
    image at least decodes. (USB CDC occasionally drops bytes near the end
    of a long burst — we'd rather see a slightly-truncated frame than fail.)
    """
    n = w * h
    needed = n * 2
    if len(data) < needed:
        data = data + b"\x00" * (needed - len(data))
    out = bytearray(n * 3)
    for i in range(n):
        lo = data[i * 2]
        hi = data[i * 2 + 1]
        v = (hi << 8) | lo
        r5 = (v >> 11) & 0x1F
        g6 = (v >> 5) & 0x3F
        b5 = v & 0x1F
        # 5/6-bit → 8-bit with lower bits replicated for smoother gradient
        r8 = (r5 << 3) | (r5 >> 2)
        g8 = (g6 << 2) | (g6 >> 4)
        b8 = (b5 << 3) | (b5 >> 2)
        out[i * 3]     = r8
        out[i * 3 + 1] = g8
        out[i * 3 + 2] = b8
    return bytes(out)


def _resolve_port(requested: str | None, output: Output) -> tuple[str | None, int]:
    if requested:
        return requested, OK
    chosen, candidates = ports_mod.detect_one_esp_port()
    if chosen is not None:
        return chosen.port, OK
    if not candidates:
        output.failure(exit_code=NO_DEVICE, error="No ESP32 port found.")
        return None, NO_DEVICE
    output.failure(
        exit_code=AMBIGUOUS_DEVICE,
        error=f"Ambiguous: {len(candidates)} candidates. Pass --port.",
        details={"candidates": [c.to_dict() for c in candidates]},
    )
    return None, AMBIGUOUS_DEVICE


def run(args: argparse.Namespace, output: Output) -> int:
    port, code = _resolve_port(args.port, output)
    if port is None:
        return code

    out_path: Path = args.out.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # Auto-scale timeout if user didn't override. Raw payload bytes =
    # size*size*2; base64 inflates to ~4/3; USB-CDC throughput ≈ 1 Mbps
    # → ~0.011 ms/byte raw → 0.003 s/raw-byte after framing+printf overhead.
    # Plus snapshot + downsample compute. Floor at 4 s for tiny dumps.
    if args.timeout is None:
        raw_bytes = args.size * args.size * 2
        timeout = max(4.0, 2.0 + raw_bytes * 0.00012)
    else:
        timeout = args.timeout

    output.info(f"screenshot from {port} at {args.size}x{args.size} (timeout {timeout:.1f}s)")

    started = time.monotonic()
    try:
        with console_session.ConsoleSession(port, baud=args.baud) as s:
            resp = s.send(
                f"?dump w={args.size}",
                timeout=timeout,
                expect_payload="DUMP",
            )
    except Exception as e:
        msg = str(e)
        if "access" in msg.lower() or "permission" in msg.lower() or "busy" in msg.lower():
            output.failure(
                exit_code=DEVICE_BUSY,
                error=f"Port {port} busy: {e}",
                details={"port": port},
            )
            return DEVICE_BUSY
        output.failure(
            exit_code=GENERIC_ERROR,
            error=f"Connection / I/O error: {e}",
            details={"port": port},
        )
        return GENERIC_ERROR

    elapsed_ms = int((time.monotonic() - started) * 1000)

    if not resp.ok:
        # Diagnostic — what did we actually observe?
        head = resp.raw[:500] if resp.raw else "(empty)"
        output.failure(
            exit_code=GENERIC_ERROR,
            error=f"?dump failed: {resp.text}",
            details={
                "port": port,
                "raw_head": head,
                "raw_tail": resp.raw[-400:],
                "raw_len": len(resp.raw),
                "payload_len": len(resp.payload),
                "payload_meta": resp.payload_meta,
            },
        )
        return GENERIC_ERROR

    m = _META_RE.search(resp.payload_meta)
    if not m:
        output.failure(
            exit_code=GENERIC_ERROR,
            error="DUMP payload meta missing w/h/fmt/bytes",
            details={"meta": resp.payload_meta, "tail": resp.raw[-400:]},
        )
        return GENERIC_ERROR

    w, h = int(m["w"]), int(m["h"])
    expected_bytes = int(m["bytes"])
    fmt = m["fmt"]

    # Decode base64 — but defensive against interleaved ESP_LOG lines.
    #
    # While the firmware is emitting the base64 payload, any other task
    # that calls ESP_LOGI prints into the SAME stdout (USB-Serial/JTAG).
    # The host-side parser is currently in_payload, so those log lines
    # get appended to resp.payload as if they were base64 lines. A naive
    # "filter to base64 alphabet" then keeps the alphanumeric tokens from
    # the log line (e.g. "I12345auroraheartbeat5") and silently inflates
    # the data count, breaking padding alignment.
    #
    # Fix: split into lines and keep only lines that are PURELY base64.
    # An ESP_LOG line contains spaces / colons / parens / hashes — none
    # of those are base64 alphabet — so the whole line gets dropped.
    # Pure base64 lines (64 chars from the firmware's 48-byte chunks, or
    # a shorter final chunk with '=' padding) survive intact.
    B64_LINE_CHARS = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")
    valid_lines: list[bytes] = []
    rejected = 0
    for raw_line in resp.payload.split(b"\n"):
        line = raw_line.strip(b"\r\n\t ")
        if not line:
            continue
        if all(c in B64_LINE_CHARS for c in line):
            valid_lines.append(line)
        else:
            rejected += 1
    if rejected:
        output.debug(f"dropped {rejected} non-base64 lines from payload "
                     "(probably ESP_LOG interleaved with the dump)")
    clean = b"".join(valid_lines)

    # Final realignment: '=' at end of stream only, never in middle.
    # If a USB-CDC drop left us at 4n+1 (impossible base64), drop the
    # orphan byte. Then pad to multiple of 4.
    clean = clean.rstrip(b"=")
    if len(clean) % 4 == 1:
        clean = clean[:-1]
        output.warn("base64 stream had a 4n+1 tail (likely 1 USB-CDC drop); "
                    "dropped 1 char to realign")
    while len(clean) % 4 != 0:
        clean += b"="
    try:
        raw = base64.b64decode(clean, validate=False)
    except Exception as e:
        output.failure(
            exit_code=GENERIC_ERROR,
            error=f"base64 decode failed: {e}",
            details={
                "payload_first_120": clean[:120].decode("ascii", "replace"),
                "clean_len": len(clean),
            },
        )
        return GENERIC_ERROR

    if len(raw) != expected_bytes:
        output.warn(
            f"byte count mismatch: got {len(raw)}, expected {expected_bytes} "
            f"(payload may be incomplete)"
        )

    if fmt != "RGB565LE":
        output.failure(
            exit_code=GENERIC_ERROR,
            error=f"Unsupported pixel format: {fmt}",
            details={"meta": resp.payload_meta},
        )
        return GENERIC_ERROR

    rgb = _rgb565_to_rgb888(raw, w, h)
    img = Image.frombytes("RGB", (w, h), rgb)
    img.save(str(out_path), "PNG")

    output.success(
        {
            "port": port,
            "out": str(out_path),
            "w": w,
            "h": h,
            "bytes": len(raw),
            "elapsed_ms": elapsed_ms,
        },
        human=f"saved {w}x{h} → {out_path} ({elapsed_ms} ms)",
    )
    return OK
