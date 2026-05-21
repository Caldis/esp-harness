"""ESP32 COM port detection on Windows.

Strategy (from research):

  Tier A — definite ESP32:
      VID 0x303A  (Espressif's USB-IF vendor ID — covers all native
                   USB-Serial/JTAG on S3/C3/C6/H2/P4, S2 OTG-CDC, esp-usb-bridge,
                   and customer-allocated PIDs like Waveshare/M5/LilyGo).

  Tier B — likely ESP32 (via USB-UART bridge IC):
      0x1A86  WCH  (CH340/CH341/CH343/CH9102)
      0x10C4  Silicon Labs CP210x
      0x0403  FTDI FT2xx series
      0x067B  Prolific PL2303

We reject anything not enumerated under USB\\ (filters Bluetooth SPP,
ACPI ghost COM1, LPT etc.). Multiple candidates are returned to the caller —
we never silently auto-pick when ambiguous.
"""

from __future__ import annotations

from dataclasses import dataclass, asdict
from typing import Any

try:
    from serial.tools import list_ports
except ImportError as e:
    raise ImportError("pyserial is required. pip install pyserial") from e


# Tier A: trust the VID alone.
ESPRESSIF_VID = 0x303A

# Tier B: USB-UART bridge ICs commonly used on ESP32 boards.
BRIDGE_VID_PIDS: dict[int, dict[int, str]] = {
    0x1A86: {  # WCH
        0x7522: "CH340",
        0x7523: "CH340",
        0x5523: "CH341",
        0x55D2: "CH343",
        0x55D3: "CH343",
        0x55D4: "CH9102",
        0x55D5: "CH9102",
    },
    0x10C4: {  # Silicon Labs
        0xEA60: "CP210x",
        0xEA70: "CP210x",
        0xEA71: "CP210x",
        0xEA80: "CP210x",
    },
    0x0403: {  # FTDI
        0x6001: "FT232",
        0x6010: "FT2232",
        0x6011: "FT4232",
        0x6014: "FT232H",
        0x6015: "FT231X",
    },
    0x067B: {  # Prolific
        0x2303: "PL2303",
        0x23A3: "PL2303",
        0x23B3: "PL2303",
    },
}

# Espressif native PIDs (informational — VID alone is sufficient to accept)
ESP_NATIVE_PIDS: dict[int, str] = {
    0x1001: "USB-Serial/JTAG",
    0x1002: "esp-usb-bridge",
    0x0002: "ESP32-S2 OTG CDC",
    0x0001: "ESP32-S2 ROM DFU",
}


@dataclass
class PortInfo:
    """Normalised serial port info, agent-friendly."""

    port: str  # e.g. "COM10"
    description: str
    vid: int | None
    pid: int | None
    serial_number: str | None
    manufacturer: str | None
    product: str | None
    location: str | None
    hwid: str
    tier: str  # "A" | "B" | "unknown"
    chip_guess: str | None

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        # render VID/PID as hex string for legibility
        d["vid"] = f"0x{self.vid:04X}" if self.vid is not None else None
        d["pid"] = f"0x{self.pid:04X}" if self.pid is not None else None
        return d


def _classify(vid: int | None, pid: int | None) -> tuple[str, str | None]:
    if vid is None:
        return "unknown", None
    if vid == ESPRESSIF_VID:
        return "A", ESP_NATIVE_PIDS.get(pid or -1, "Espressif (custom PID)")
    if vid in BRIDGE_VID_PIDS:
        chip = BRIDGE_VID_PIDS[vid].get(pid or -1)
        if chip is not None:
            return "B", chip
    return "unknown", None


def list_all_ports() -> list[PortInfo]:
    """All serial ports the OS can see (including Bluetooth, ACPI ghosts, etc).

    Useful for diagnostics. For ESP32 detection use `list_esp_ports`.
    """
    out: list[PortInfo] = []
    for p in list_ports.comports():
        tier, chip = _classify(p.vid, p.pid)
        out.append(
            PortInfo(
                port=p.device,
                description=p.description or "",
                vid=p.vid,
                pid=p.pid,
                serial_number=p.serial_number,
                manufacturer=p.manufacturer,
                product=p.product,
                location=p.location,
                hwid=p.hwid or "",
                tier=tier,
                chip_guess=chip,
            )
        )
    return out


_REJECT_HWID_PREFIXES = (
    "BTHENUM\\",  # Bluetooth SPP virtual COM
    "BTH\\",      # other Bluetooth enumerator
    "ACPI\\",     # ACPI ghost port (COM1 on most motherboards)
    "LPTENUM\\",  # legacy parallel port adapter
    "SWD\\",      # software-only virtual port
)


def list_esp_ports() -> list[PortInfo]:
    """Only ports we believe are ESP32-like.

    Primary signal is VID/PID classification (`tier` A or B). The hwid prefix
    filter is belt-and-suspenders: explicitly drops Bluetooth/ACPI enumerators
    so we don't surface them even if they had a stray matching VID.

    Note: pyserial on Windows renders USB hwid as
        'USB VID:PID=303A:1001 SER=AA:BB:CC...'      (space after USB)
    while PnP-style strings use 'USB\\VID_...'. We don't filter by accept-prefix
    because of this normalisation difference.
    """
    candidates: list[PortInfo] = []
    for p in list_all_ports():
        hwid_upper = p.hwid.upper()
        if any(hwid_upper.startswith(pfx) for pfx in _REJECT_HWID_PREFIXES):
            continue
        if p.tier in ("A", "B"):
            candidates.append(p)

    # Tier-A first, then Tier-B; within tier, prefer lower COM number for stability
    def sort_key(pi: PortInfo) -> tuple[int, int]:
        tier_rank = 0 if pi.tier == "A" else 1
        try:
            num = int("".join(c for c in pi.port if c.isdigit()))
        except ValueError:
            num = 999
        return (tier_rank, num)

    candidates.sort(key=sort_key)
    return candidates


def detect_one_esp_port() -> tuple[PortInfo | None, list[PortInfo]]:
    """Pick the single best ESP32 port.

    Returns (chosen, all_candidates).
    - chosen is None if no candidates.
    - chosen is None if multiple Tier-A candidates exist (ambiguous — caller decides).
    - chosen is the only Tier-A if exactly one.
    - chosen is the only Tier-B if no Tier-A and exactly one Tier-B.
    - chosen is None if multiple Tier-B exist (ambiguous).
    """
    candidates = list_esp_ports()
    tier_a = [p for p in candidates if p.tier == "A"]
    tier_b = [p for p in candidates if p.tier == "B"]

    if len(tier_a) == 1:
        return tier_a[0], candidates
    if len(tier_a) == 0 and len(tier_b) == 1:
        return tier_b[0], candidates
    return None, candidates
