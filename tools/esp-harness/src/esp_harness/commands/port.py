"""`esp-harness port` — list / detect ESP32 COM ports.

Subcommands:
    port list       all serial ports, with metadata + classification
    port detect     pick the single best ESP32 port (fail if ambiguous)
"""

from __future__ import annotations

import argparse

from esp_harness.core import ports as ports_mod
from esp_harness.exit_codes import AMBIGUOUS_DEVICE, NO_DEVICE, OK
from esp_harness.output import Output


def add_subparser(sub, add_common_flags) -> None:
    p = sub.add_parser("port", help="Find / inspect serial ports.")
    sp = p.add_subparsers(dest="port_action", metavar="<action>")
    sp.required = True

    p_list = sp.add_parser("list", help="List all serial ports, with metadata.")
    add_common_flags(p_list)
    p_list.add_argument(
        "--esp-only",
        action="store_true",
        help="Only show ports that look like ESP32 candidates.",
    )

    p_detect = sp.add_parser(
        "detect",
        help="Auto-detect the single ESP32 port. Fails (exit 10/12) if absent/ambiguous.",
    )
    add_common_flags(p_detect)


def run(args: argparse.Namespace, output: Output) -> int:
    if args.port_action == "list":
        return _run_list(args, output)
    if args.port_action == "detect":
        return _run_detect(args, output)
    return 0


def _run_list(args: argparse.Namespace, output: Output) -> int:
    all_ports = ports_mod.list_esp_ports() if args.esp_only else ports_mod.list_all_ports()
    payload = {
        "count": len(all_ports),
        "ports": [p.to_dict() for p in all_ports],
    }
    if output.json_mode:
        output.success(payload)
    else:
        if not all_ports:
            print("(no serial ports found)")
        else:
            print(f"{'PORT':<6}  {'TIER':<5}  {'VID':<8}  {'PID':<8}  {'CHIP':<22}  DESCRIPTION")
            print("-" * 90)
            for p in all_ports:
                vid = f"0x{p.vid:04X}" if p.vid is not None else "-"
                pid = f"0x{p.pid:04X}" if p.pid is not None else "-"
                chip = p.chip_guess or "-"
                desc = (p.description or "")[:40]
                print(f"{p.port:<6}  {p.tier:<5}  {vid:<8}  {pid:<8}  {chip:<22}  {desc}")
    return OK


def _run_detect(args: argparse.Namespace, output: Output) -> int:
    chosen, candidates = ports_mod.detect_one_esp_port()

    if chosen is not None:
        payload = {
            "port": chosen.port,
            "tier": chosen.tier,
            "chip_guess": chosen.chip_guess,
            "vid": f"0x{chosen.vid:04X}" if chosen.vid is not None else None,
            "pid": f"0x{chosen.pid:04X}" if chosen.pid is not None else None,
            "serial_number": chosen.serial_number,
            "candidates_considered": len(candidates),
        }
        if output.json_mode:
            output.success(payload)
        else:
            print(chosen.port)  # primary value for shell pipelines
            output.info(
                f"detected {chosen.port} ({chosen.chip_guess or '?'}, tier {chosen.tier})"
            )
        return OK

    if not candidates:
        output.failure(
            exit_code=NO_DEVICE,
            error="No ESP32-like serial port found.",
            details={"candidates": []},
            human="Plug in your board, check the USB cable (data, not power-only), "
            "and install the driver if it's a CH340/CP210x board.",
        )
        return NO_DEVICE

    output.failure(
        exit_code=AMBIGUOUS_DEVICE,
        error=f"Multiple ESP32 candidates ({len(candidates)}). Pass --port explicitly.",
        details={"candidates": [c.to_dict() for c in candidates]},
        human="Candidates:\n"
        + "\n".join(f"  {c.port}  {c.chip_guess or '?'}  (tier {c.tier})" for c in candidates),
    )
    return AMBIGUOUS_DEVICE
