"""esp-harness: AI-driven dev-loop CLI for ESP-IDF + LVGL projects."""

from importlib.metadata import PackageNotFoundError, version as _pkg_version

try:
    __version__ = _pkg_version("esp-harness")
except PackageNotFoundError:
    # Not installed (e.g. running from source via PYTHONPATH).
    # Fall back to a sentinel; the README's release table is the
    # authoritative answer in that path.
    __version__ = "0.0.0+source"
