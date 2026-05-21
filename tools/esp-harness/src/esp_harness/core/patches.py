"""Known managed_components/ post-fetch patches.

ESP-IDF's component manager fetches third-party deps to a gitignored
`managed_components/` dir on first build. A few of those deps have
upstream bugs we work around by patching the fetched files after they
land. This module centralises those patches so the build wrapper can
apply them as a retry step.

Each patch is **idempotent** (signature-checks before re-writing) so
it's safe to apply repeatedly.

Today's patches:

- `waveshare__qmi8658` v1.0.0 — its CMakeLists.txt only declares
  `REQUIRES "driver"` (the v5 umbrella). IDF v6 split out i2c_master
  into `esp_driver_i2c`; without that dependency, `qmi8658.c` can't
  find `driver/i2c_master.h`. We rewrite CMakeLists.txt to add the
  missing requirement.
- `waveshare__qmi8658` header `qmi8658.h` — defines `M_PI` without an
  `#ifndef` guard, which gcc 15+ flags as `builtin-macro-redefined`.
  We wrap the existing `#define` with a guard.

If a new upstream component needs patching, add a `_patch_<name>`
function below and append it to `KNOWN_PATCHES`.
"""

from __future__ import annotations

from pathlib import Path


def _patch_qmi8658_cmakelists(project_dir: Path) -> tuple[bool, str]:
    """Add esp_driver_i2c to qmi8658 component's REQUIRES if absent."""
    target = project_dir / "managed_components" / "waveshare__qmi8658" / "CMakeLists.txt"
    if not target.exists():
        return False, "qmi8658 component not fetched yet"
    content = target.read_text(encoding="utf-8")
    if "esp_driver_i2c" in content:
        return False, "already patched"
    target.write_text(
        '# Auto-patched by esp-harness — see core/patches.py.\n'
        'idf_component_register(\n'
        '    SRCS "qmi8658.c"\n'
        '    INCLUDE_DIRS "include"\n'
        '    REQUIRES "driver" "esp_driver_i2c"\n'
        ')\n',
        encoding="utf-8",
        newline="\n",
    )
    return True, str(target)


def _patch_qmi8658_header(project_dir: Path) -> tuple[bool, str]:
    """Wrap qmi8658.h's M_PI define in an ifndef guard."""
    target = (
        project_dir
        / "managed_components"
        / "waveshare__qmi8658"
        / "include"
        / "qmi8658.h"
    )
    if not target.exists():
        return False, "qmi8658 header not fetched yet"
    content = target.read_text(encoding="utf-8")
    if "#ifndef M_PI" in content:
        return False, "already patched"
    if "#define M_PI" not in content:
        return False, "no M_PI define to patch"
    patched = content.replace(
        "#define M_PI (3.14159265358979323846f)",
        "#ifndef M_PI\n#define M_PI (3.14159265358979323846f)\n#endif",
    )
    target.write_text(patched, encoding="utf-8", newline="\n")
    return True, str(target)


KNOWN_PATCHES = [
    ("qmi8658_cmakelists", _patch_qmi8658_cmakelists),
    ("qmi8658_header",     _patch_qmi8658_header),
]


def apply_all(project_dir: Path) -> list[dict]:
    """Apply every known patch. Returns a list of dicts describing each
    attempt: name, applied (bool), detail."""
    results: list[dict] = []
    for name, fn in KNOWN_PATCHES:
        try:
            applied, detail = fn(project_dir)
        except Exception as e:
            results.append({"name": name, "applied": False, "detail": f"exception: {e}"})
            continue
        results.append({"name": name, "applied": applied, "detail": detail})
    return results


# Common error-string signatures from idf.py output. The build wrapper
# uses these to decide whether to retry after applying patches.
RETRY_SIGNATURES = (
    "qmi8658.h includes driver/i2c_master.h",
    "qmi8658.h:18: error: 'M_PI' redefined",
    "'M_PI' redefined",
    "is not in the requirements list of \"waveshare__qmi8658\"",
)


def stderr_suggests_retry(stderr: str) -> bool:
    """Return True if the build stderr matches a signature for a
    known patchable issue."""
    return any(sig in stderr for sig in RETRY_SIGNATURES)
