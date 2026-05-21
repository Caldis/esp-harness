# sim-base — reusable host LVGL build base

> The board-agnostic half of "how to run aurora-harness on your laptop".
> Per-project pieces (scene list, peripheral mocks, project name) live
> in `examples/<your-thing>/sim/`. This directory only holds what's
> identical across every host build.

What's here:

| File | Role |
|---|---|
| `include/` | ESP-IDF stub headers — `esp_err.h`, `esp_log.h`, `esp_timer.h`, `esp_heap_caps.h`, `esp_system.h`, `bsp/*`, `freertos/*` — let firmware source compile on the host with zero changes. |
| `mock_bsp.{c,h}` | No-op `bsp_display_lock` / `bsp_display_unlock` + brightness logger. The component's BSP interface (see `components/aurora-harness/include/harness/bsp_iface.h`) implemented for host. |
| `tools/setup.ps1` | Probe for cmake / mingw / SDL2 / LVGL prerequisites. |

What's **not** here (lives in the consuming example):

| File | Why per-example |
|---|---|
| `main.c` | Registers the scenes — list is project-specific. |
| `lv_conf.h` | Enables only the LVGL widgets / font sizes this project uses. |
| `mock_peripherals.{c,h}` | Signatures match the project's `peripherals/*.h` — those headers are project-specific. |
| `CMakeLists.txt` | References this dir + the project's main + the project's scenes. |
| `golden/*.bmp` | Visual regression baseline for this project's scenes. |

See [INTEGRATION.md](./INTEGRATION.md) for the full "adopt this in my
own project" walkthrough.

For the Aurora demo's sim build (the reference consumer), look at
[../examples/aurora/sim/](../examples/aurora/sim/) — it's the smallest
real example.
