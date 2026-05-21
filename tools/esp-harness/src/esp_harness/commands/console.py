"""`esp-harness console` — generic command channel.

One CLI to rule the device's whole command surface:

    esp-harness console --cmd "?stat"
    esp-harness console --cmd "audio tone 440 200 30"
    esp-harness console --cmd "scene 6"

Wraps `core.console_session.ConsoleSession` so it inherits the DTR/RTS
no-reset open, the line buffering, the OK:/ERR: parsing, and the
payload-block (DUMP_BEGIN/END) handling that we already get for the
`screenshot` command.

Why this exists: before this, every audio/SD/key/etc. test I wrote
hand-rolled a tiny pyserial dance to send the command and parse the
reply. Now it's one CLI, JSON output, and no shell-quoting traps.
"""

from __future__ import annotations

import argparse
import json
import time

from esp_harness.core import ports as ports_mod
from esp_harness.core.console_session import ConsoleSession
from esp_harness.exit_codes import (
    AMBIGUOUS_DEVICE,
    DEVICE_BUSY,
    GENERIC_ERROR,
    NO_DEVICE,
    OK,
)
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser(
        "console",
        help="Send one command (e.g. '?stat') and return the OK/ERR reply as JSON.",
        description=(
            "Generic console wrapper. Sends one line, reads until OK:/ERR:, "
            "returns the body as JSON (parsed if it looks like JSON, raw "
            "string otherwise). Use this for any command not covered by a "
            "dedicated subcommand."
        ),
    )
    p.add_argument("--port", default=None, help="COM port (auto-detect if omitted).")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument(
        "--cmd",
        default=None,
        help="The exact line to send (without trailing newline). Omit when "
             "using --repl.",
    )
    p.add_argument(
        "--repl",
        action="store_true",
        help="Interactive mode: read commands from stdin in a loop, pretty-"
             "print replies. Slash commands inside the REPL: /quit, /help, "
             "/clear, /payload TAG, /raw, /timeout SECS.",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="Seconds to wait for OK:/ERR: (default 5).",
    )
    p.add_argument(
        "--raw",
        action="store_true",
        help="Dump everything we observed (including ESP_LOG lines and EVTs) "
        "to stdout instead of just parsing the OK/ERR body.",
    )
    p.add_argument(
        "--payload",
        metavar="TAG",
        default=None,
        help="Expect a payload block framed by TAG_BEGIN/TAG_END after OK:. "
        "The bytes between are decoded as UTF-8 and parsed as JSON if "
        "possible. Used by manifest-style commands (`?help json` → TAG=HELP, "
        "`scene list` → TAG=SCENES).",
    )
    add_common_flags(p)


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


def _run_repl(session, output: Output) -> int:
    """Interactive command loop. Read lines from stdin, send each, pretty-
    print reply. Quit on EOF / `/quit`."""
    print("esp-harness console REPL — type /help for slash commands, /quit to exit")
    state = {"payload": None, "raw": False, "timeout": 5.0}
    while True:
        try:
            line = input("> ")
        except (EOFError, KeyboardInterrupt):
            print()
            return OK
        line = line.strip()
        if not line:
            continue
        if line.startswith("/"):
            parts = line[1:].split(maxsplit=1)
            slash = parts[0].lower()
            arg = parts[1] if len(parts) > 1 else None
            if slash in ("q", "quit", "exit"):
                return OK
            if slash in ("h", "help", "?"):
                print("  /quit         — exit REPL")
                print("  /help         — this list")
                print("  /clear        — clear screen")
                print("  /payload TAG  — next command expects TAG_BEGIN/TAG_END payload")
                print("  /payload off  — clear payload expectation")
                print("  /raw          — toggle raw-everything mode")
                print("  /timeout S    — set reply timeout (default 5s)")
                print("Any other line is sent verbatim to the device.")
                continue
            if slash == "clear":
                print("\033[H\033[J", end="")
                continue
            if slash == "payload":
                if arg is None or arg.lower() == "off":
                    state["payload"] = None
                    print("  payload tag cleared")
                else:
                    state["payload"] = arg.strip()
                    print(f"  payload tag = {state['payload']!r}")
                continue
            if slash == "raw":
                state["raw"] = not state["raw"]
                print(f"  raw = {state['raw']}")
                continue
            if slash == "timeout":
                try:
                    state["timeout"] = float(arg) if arg else 5.0
                    print(f"  timeout = {state['timeout']}s")
                except ValueError:
                    print(f"  /timeout: bad number {arg!r}")
                continue
            print(f"  unknown slash command: {slash}. Try /help.")
            continue

        # Send to device
        try:
            resp = session.send(line, timeout=state["timeout"],
                                expect_payload=state["payload"])
        except Exception as e:
            print(f"  send failed: {e}")
            continue

        if state["raw"]:
            print(resp.raw)
            continue

        mark = "OK " if resp.ok else "ERR"
        print(f"  {mark}  {resp.text}")
        if resp.payload:
            try:
                body = resp.payload.decode("utf-8", errors="replace").strip()
            except Exception:
                body = ""
            if body:
                # Truncate to a few hundred chars to keep the REPL tidy
                preview = body if len(body) <= 400 else body[:400] + " ..."
                print(f"  PAYLOAD: {preview}")
        if resp.events:
            for evt in resp.events:
                print(f"  EVT: {evt}")


def run(args: argparse.Namespace, output: Output) -> int:
    if not args.repl and not args.cmd:
        output.failure(exit_code=GENERIC_ERROR,
                       error="must pass --cmd or --repl")
        return GENERIC_ERROR

    port, code = _resolve_port(args.port, output)
    if port is None:
        return code

    if args.repl:
        try:
            with ConsoleSession(port, baud=args.baud) as session:
                return _run_repl(session, output)
        except Exception as e:
            output.failure(exit_code=GENERIC_ERROR, error=f"repl: {e}")
            return GENERIC_ERROR

    started = time.monotonic()
    try:
        with ConsoleSession(port, baud=args.baud) as session:
            resp = session.send(
                args.cmd,
                timeout=args.timeout,
                expect_payload=args.payload,
            )
    except Exception as e:
        msg = str(e)
        if any(k in msg.lower() for k in ("access", "permission", "busy")):
            output.failure(
                exit_code=DEVICE_BUSY,
                error=f"Port {port} busy: {e}",
                human="Close any open monitor on this port first.",
            )
            return DEVICE_BUSY
        output.failure(exit_code=GENERIC_ERROR, error=f"console: {e}")
        return GENERIC_ERROR
    elapsed_ms = int((time.monotonic() - started) * 1000)

    if args.raw:
        # `--raw` dumps everything verbatim. Useful when the reply isn't OK:/
        # ERR: at all (e.g., the user is sending a payload-producing command
        # like `?dump` and wants the wire data).
        print(resp.raw)
        return OK if resp.ok else GENERIC_ERROR

    # Try to parse the body as JSON — most of our firmware commands return
    # `OK: {...}` so this is the common case and AI agents prefer parsed
    # objects.
    parsed: object = resp.text
    try:
        parsed = json.loads(resp.text)
    except Exception:
        pass

    payload: dict[str, object] = {
        "port": port,
        "sent": args.cmd,
        "ok": resp.ok,
        "reply": parsed,
        "elapsed_ms": elapsed_ms,
    }
    if resp.events:
        payload["events"] = resp.events
    if resp.other_lines:
        # ESP_LOG lines interleaved with the reply — useful for diagnosing
        # commands that emit warnings but still return OK.
        payload["log_lines"] = resp.other_lines
    if resp.payload:
        # When `--payload TAG` is set, the device emitted a body block
        # framed by TAG_BEGIN/TAG_END. Decode to text and try JSON.
        try:
            body_text = resp.payload.decode("utf-8", errors="replace").strip()
        except Exception:
            body_text = ""
        try:
            payload["payload"] = json.loads(body_text)
        except Exception:
            payload["payload_text"] = body_text
        payload["payload_meta"] = resp.payload_meta

    if resp.ok:
        output.success(payload, human=f"{args.cmd} → OK ({elapsed_ms} ms)")
        return OK
    output.failure(
        exit_code=GENERIC_ERROR,
        error=f"firmware ERR: {resp.text}",
        details=payload,
    )
    return GENERIC_ERROR
