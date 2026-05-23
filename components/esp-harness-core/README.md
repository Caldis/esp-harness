# esp-harness-core

[![component](https://img.shields.io/badge/component-aurora--harness-b8431a)](./idf_component.yml)
[![version](https://img.shields.io/badge/version-1.4.0-1c1814)](./idf_component.yml)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-≥5.0-1c1814)](https://docs.espressif.com/projects/esp-idf/)
[![LVGL](https://img.shields.io/badge/LVGL-9.x-1c1814)](https://lvgl.io/)
[![docs](https://img.shields.io/badge/docs-API_reference-b8431a)](./README.md)
[![porting](https://img.shields.io/badge/porting-PORTING.md-344a36)](./PORTING.md)

> Reusable ESP-IDF component: line-protocol console + LVGL scene framework.
> The pair that turns a one-off device into an AI-driveable dev loop.

This component is the **scaffold core** of the Aurora project. It's
intentionally generic — nothing here is Waveshare-specific or even
Aurora-specific. Drop it into any ESP-IDF + LVGL project and you get:

- A **console protocol** that gives the host (a human, an AI, or a CI
  job) a stable, parseable conversation with the device:
  `OK: …` / `ERR: …` / `EVT: …` lines, plus `<TAG>_BEGIN … <TAG>_END`
  multi-line payload framing for binary-ish data.
- A **scene framework** that organises the LVGL screen into a deck of
  full-screen "scenes", each with its own lifecycle hooks
  (`init` / `on_show` / `on_hide` / `frame` / `on_long_press` / `on_release`)
  and the ability to receive global IMU updates via a single fanout call.

Both modules are designed to **self-register their capabilities** so
that one round-trip — `?help json` for commands, `scene list` for
scenes — tells the host everything the device can do.

---

## Quickstart — five lines to a working console

```c
#include "harness/console_protocol.h"

static int cmd_ping(const console_args_t *a) {
    (void)a;
    console_reply_ok("pong");
    return 0;
}
static const console_cmd_t s_cmd_ping = { "?ping", cmd_ping, "liveness probe" };

void app_main(void) {
    console_protocol_init();
    console_protocol_register(&s_cmd_ping);
    /* ... your normal init ... */
}
```

That's it — over serial you can now send `?ping<LF>` and get back
`OK: pong<LF>`. Add `?help`, `?reset`, `?stat`, … the same way. Every
command you register shows up in `?help` (text) and `?help json`
(machine-readable manifest).

To add a scene:

```c
#include "harness/scene_framework.h"

static void halo_init(scene_t *s, lv_obj_t *parent) {
    /* build LVGL objects under `parent` */
}

scene_t scene_halo = {
    .id           = "halo",
    .display_name = "I. Halo",
    .accent       = LV_COLOR_MAKE(0x8F, 0xD9, 0xFF),
    .init         = halo_init,
};

void app_main(void) {
    /* after LVGL is up and you have a screen object */
    scene_fw_init(lv_screen_active());
    scene_fw_register(&scene_halo);
}
```

`scene list` over the console now returns a JSON manifest including
`{"idx":0,"id":"halo","name":"I. Halo", …}`.

---

## Installation

This component is not yet on the IDF Component Registry. Until then,
use `esp-harness new` (which wires this up for you), or vendor it in
manually:

```cmake
# In your project's top-level CMakeLists.txt, before `project(...)`:
list(APPEND EXTRA_COMPONENT_DIRS path/to/esp-harness/components)
```

Then add `esp-harness-core` to your `main` component's `REQUIRES`:

```cmake
idf_component_register(
    SRCS "..."
    REQUIRES
        lvgl__lvgl
        esp-harness-core
        # ...your other deps
)
```

`#include "harness/console_protocol.h"` and `#include "harness/scene_framework.h"`
now resolve.

### Why the `harness/` include prefix

`esp-harness-core` is the component name; `harness/` is the include
namespace. Two reasons:

1. Avoids collision with any other `console_protocol.h` your project
   (or other components) might already have.
2. Makes the dependency obvious at the include site — anyone reading
   the code immediately knows where this header lives.

---

## Console protocol — the contract

### Wire format

Everything is line-based at UART/USB-Serial-JTAG baud (typically 115200).

```
host → device   <command> <arg1> <arg2> ...\n
device → host   OK:  <result line>\n
                ERR: <reason>\n
                EVT: <unsolicited event>\n
```

Replies and events are always single lines unless wrapped in a payload
frame (see below). Lines end with `\n` (no `\r\n`). The first three
characters of any line classify it for an AI parser:

- `OK:` — successful reply to the most recent command.
- `ERR:` — failed reply, command-specific reason after the colon.
- `EVT:` — unsolicited event. Doesn't follow a command. The component
  emits these from `console_send_evt(fmt, ...)`.

### Multi-line payloads

When a command's reply doesn't fit in a single line — a JSON dump, a
base64 framebuffer, a CSV table — wrap it:

```
device → host   OK: payload follows tag=<TAG>
                <TAG>_BEGIN fmt=json bytes=2048
                {... raw payload ...}
                <TAG>_END
```

The OK line MUST embed `tag=<TAG>` somewhere in its body — this is the
post-G-4 contract that lets host parsers route the body without grepping
firmware source. Built-in callers use variations like
`OK: manifest follows tag=HELP`, `OK: scene manifest follows tag=SCENES`,
`OK: dump start tag=DUMP w=128 h=128 fmt=RGB565LE`. Host code matches
`\btag=([A-Z][A-Z0-9_]*)\b` in the OK body to learn the upcoming tag.

API:

```c
console_reply_ok("scene manifest follows tag=SCENES");    /* explicit tag */
console_begin_payload("SCENES", "fmt=json");              /* SCENES_BEGIN ... */
printf("{\"count\":%d, ...}", n);                         /* raw bytes */
fflush(stdout);
console_end_payload("SCENES");                            /* SCENES_END */
```

Tags are uppercase, short, scene/command-named. Used in production:
`HELP`, `SCENES`, `DUMP`, `HEALTH`. The meta string after the tag is
freeform — typically `fmt=json bytes=...` so the host parser knows
what's coming.

**Host-side helper**: the toolkit ships
`esp_harness.core.parser.PayloadFollowsReader`, a state machine that
consumes the wire line stream and yields one `ReplyEvent(kind="payload",
tag=..., blob=...)` per multi-line reply. Bridges that want to drain
ok/err/evt/payload events from one open session use
`esp_harness.client.open_persistent_session(port)` — both APIs were
added in v0.2.0 to close gaps G-1, G-3, G-H1, G-H3.

Legacy `OK: payload follows` without an explicit tag still works on the
host side for backward compatibility (the host infers the tag from the
following `<TAG>_BEGIN` line), but new firmware should always use the
explicit form.

### Built-in commands

`console_protocol_init()` registers three commands automatically:

| Command | Behaviour |
|---|---|
| `?ping` | `OK: pong` — liveness probe |
| `?help` | List of every registered command's name + help string |
| `?help json` | HELP_BEGIN/END JSON manifest of every command (for discovery) |
| `?reset` | `OK: resetting` then `esp_restart()` |

`?help json` is the **discovery anchor**. The host's manifest tool
should always call this first before doing anything else with the
device — it's the single source of truth about what's available.

### Reply helpers

```c
console_reply_ok("idx=%d id=%s", idx, name);   /* OK: idx=3 id=tilt */
console_reply_err("not found: %s", name);      /* ERR: not found: foo */
console_send_evt("scene_changed idx=%d", idx); /* EVT: scene_changed idx=3 */
```

`console_send_evt` is the asynchronous channel — use it for state
changes the host should know about (touch hit, scene transition,
sensor threshold crossed, etc.).

---

## Scene framework — the contract

A scene is "a self-contained generative visual that owns a full-screen
LVGL container". The framework holds a registry of scenes, shows one
at a time, and offers two navigation modes (`scene_fw_next` /
`scene_fw_show(idx_or_name)`).

### scene_t lifecycle

```c
struct scene {
    const char *id;            /* "halo" — stable, lowercase, used by `scene list` */
    const char *display_name;  /* "I. Halo" — shown in UI shell */
    lv_color_t  accent;        /* per-scene accent colour, used by ui_shell etc */

    void (*init)(scene_t *s, lv_obj_t *parent);        /* one-time setup */
    void (*on_show)(scene_t *s);                       /* becoming visible */
    void (*on_hide)(scene_t *s);                       /* about to be hidden */
    void (*frame)(scene_t *s, uint32_t t_ms);          /* per-frame tick */
    void (*on_tilt)(scene_t *s, float ax, float ay, float az);  /* IMU update */
    void (*on_long_press)(scene_t *s);                 /* held >= 400 ms */
    void (*on_release)(scene_t *s);                    /* finger lifted */

    void     *user_data;       /* scene-private state */
    lv_obj_t *container;       /* set by framework */
    bool      initialised;
};
```

Set only the hooks you need. `NULL` = no-op.

### Initialisation order

```c
bsp_display_start();             /* LVGL must be up */
bsp_display_lock(-1);            /* hold the LVGL mutex */
scene_fw_init(lv_screen_active());
scene_fw_register(&scene_halo);
scene_fw_register(&scene_grid);
/* ... */
bsp_display_unlock();
```

Registering must happen under the LVGL lock because each registration
creates a child container — that's an LVGL widget mutation.

### Navigation

```c
scene_fw_next();          /* wrap-around to next */
scene_fw_prev();
scene_fw_show(2);         /* by index */
const scene_t *cur = scene_fw_current();
int idx = scene_fw_current_index();
int n   = scene_fw_count();
```

Switching does a brief crossfade (current implementation: 280 ms via
`lv_obj_set_style_opa` animations). Both old and new scenes' `on_hide`
/ `on_show` fire at the right time.

### IMU fanout

```c
/* In your IMU task — typically 20 Hz: */
float ax, ay, az;
imu_get_accel(&ax, &ay, &az);
scene_fw_push_tilt(ax, ay, az);   /* hits the current scene's on_tilt */
```

Only the currently visible scene receives the call. Scenes that don't
care leave `on_tilt = NULL`.

### Per-frame tick

The framework owns a single `lv_timer` at 33 ms (30 Hz — verified
ceiling for the Waveshare AMOLED at full-screen redraw; faster ticks
hit DMA TX underflow). The current scene's `frame(s, t_ms)` is called
each tick. Scenes that need real animation work in there.

If you need a faster cadence, create your own `lv_timer` in the
scene's `init` and free it in a destructor (no scene destruction
today — scenes are static, but the pattern is portable).

### Cap

The component caps registered scenes at **24**. Exceeding this used to
silently drop scenes; now it logs `ESP_LOGE`. Bump `MAX_SCENES` in
`src/scene_framework.c` if you genuinely need more, and consider
whether you really want that many full-screen views.

### Discovery surface

```
host → device     scene list
device → host     OK: scene manifest follows tag=SCENES
                  SCENES_BEGIN fmt=json
                  {"count":18,"current":0,"scenes":[
                    {"idx":0,"id":"halo","name":"I. Halo","on_long_press":false},
                    ...]}
                  SCENES_END
```

Mirrors `?help json` at the scene-framework layer. The host's
manifest aggregator can enumerate every scene without grepping
firmware source.

---

## Default command set (`harness_default_register`)

One opt-in call after `console_protocol_init()` gives you the standard
console surface every esp-harness-core consumer should have:

```c
console_protocol_init();
harness_default_register();   // see include/harness/default_cmds.h
```

| Command | Use |
|---|---|
| `?stat` | JSON: fps + heap + scene + uptime. Tick `harness_record_frame()` each LVGL frame to feed fps. |
| `scene` | Navigation: `scene next` / `prev` / `<N>` / `<id>` / `name` / `action` / `list`. `list` emits the SCENES manifest. |
| `tap` | Synthetic LVGL touch (no args = screen centre). |
| `swipe` | `swipe X1 Y1 X2 Y2 [DUR_MS]` — synthetic gesture. |
| `?dump` | PSRAM-backed screenshot: snapshots the screen + top layer, downsamples to N×N (default 128), emits base64 RGB565 in a DUMP payload. |

Plus the always-on `?ping` / `?help` / `?reset` from `console_protocol_init()`.

## UI overlay primitives

Two thread-safe, drop-in widgets on `lv_layer_top()`. Both safe to call
from any FreeRTOS task — they dispatch LVGL mutations via `lv_async_call`.

```c
#include "harness/toast.h"
#include "harness/progress.h"

/* Fire-and-forget notification with auto-expire. */
harness_toast("captured", 1500);

/* Long-running task overlay (label + bar). */
harness_progress_show("Downloading 42%", 42);
/* ... loop updates ... */
harness_progress_dismiss();
```

Each has a corresponding reference scene in the Aurora showcase:
[scene_notify.c](../../examples/aurora/main/scenes/scene_notify.c) for toast,
[scene_track.c](../../examples/aurora/main/scenes/scene_track.c) for progress —
copy-paste from there when you need one.

## Threading model

All console commands run on the **console parser task**, which is not
the LVGL task. Any LVGL widget mutation from a command handler must:

- **Hold `bsp_display_lock(-1)` for the whole mutation**, or
- **Dispatch to LVGL task via `lv_async_call(...)`** when the mutation
  is "fire and forget" (it'll run later, after the handler returns).

The existing `scene` command shows both patterns:

```c
/* Pattern A: hold lock during the mutation */
bsp_display_lock(-1);
scene_fw_show(idx);
bsp_display_unlock();
console_reply_ok(...);

/* Pattern B: dispatch action to the LVGL task */
lv_async_call((lv_async_cb_t)c->on_long_press, (void *)c);
console_reply_ok("action queued");
```

**Don't use timeout=0** on `bsp_display_lock` from a console command.
On a busy system it silently fails to acquire, and subsequent
`lv_obj_*` calls trigger a task watchdog on the console task. Use
`-1` (wait forever) — console commands are user-initiated, the
extra few ms don't matter.

---

## Integration with the toolkit

The companion repo
[`esp32-harness-toolkit`](https://github.com/Caldis/esp32-harness-toolkit)
provides the host-side CLI. With this component on the device side
and the toolkit on the host side, you get:

| Toolkit command | Talks to this component via |
|---|---|
| `esp-harness manifest --json` | `?help json` + `scene list` |
| `esp-harness console --cmd "<x>"` | Raw `<x>\n` line |
| `esp-harness screenshot` | `?dump` (your `?dump` impl — payload framing) |
| `esp-harness bench` | `?stat` + your peripheral probes |
| `esp-harness backtrace --monitor` | reads `Backtrace:` from monitor stream |
| `esp-harness sim diff` | host build of selected scenes (no device) |

You don't need the toolkit to use the component — `picocom`, `screen`,
or a Python `pyserial` script all work. The toolkit just removes
boilerplate.

---

## Known limitations

- **Single console session.** `console_protocol_init` claims stdout/
  stdin via USB-Serial-JTAG (default) or UART. Two simultaneous
  connections clobber each other. Multi-stream support is not
  planned — embed device usually has one host.
- **LVGL 9.x only.** `scene_framework` uses `lv_obj_set_style_opa`
  animation primitives that exist in 9.x. Backporting to 8.x is
  about 50 lines.
- **No scene destruction.** Scenes are static — `scene_fw_register`
  expects a pointer that outlives the program. Dynamic add/remove is
  not implemented; you can simulate it by hiding (set opa=0) instead.
- **Crossfade is fixed at 280 ms.** Tweakable in
  `src/scene_framework.c::FADE_MS`. Not exposed as Kconfig yet.
- **Cap at 24 scenes.** Tunable; see "Cap" above.
- **BSP coupling.** `scene_framework.c` calls `bsp_display_lock` /
  `bsp_display_unlock` during transitions. On a board without an
  Espressif BSP, define these as no-ops (or wrappers around your own
  LVGL mutex) before including the component.

---

## File layout

```
components/esp-harness-core/
├── CMakeLists.txt            ESP-IDF component manifest
├── idf_component.yml         (Component Registry metadata, not published yet)
├── README.md                 you are here
├── include/harness/
│   ├── console_protocol.h    public API: protocol + reply helpers
│   └── scene_framework.h     public API: scene_t + framework calls
└── src/
    ├── console_protocol.c    parser task + line buffer + dispatch
    └── scene_framework.c     scene registry + crossfade + frame timer
```

`src/*.c` files are private — never `#include` them directly.

---

## Maintenance notes for future contributors

- **Public API breaks** require a major version bump in
  `idf_component.yml`. The current API surface has 18 functions across
  the two headers; touch them carefully.
- **Adding a new built-in command** to `console_protocol_init`: each
  built-in is a 5-line static `console_cmd_t` plus the handler. Keep
  the bar high — currently `?ping` / `?help` / `?reset` are the only
  ones, on the basis that they're useful in literally every project.
  Peripheral or app-specific commands belong in the consuming app, not
  here.
- **Sync `?help json` schema with the toolkit.** The fields are
  `{count, commands:[{name, usage}]}` today. If you change them,
  update `commands/manifest.py::_parse_help_payload` in the toolkit.
- **Test in both target and host builds.** The Aurora `sim/` build
  compiles `src/scene_framework.c` directly on the host — keep this
  module free of `freertos/` / `driver/` includes so the host stub
  shims are minimal. (`src/console_protocol.c` *is* allowed to be
  device-only since the host sim doesn't need a serial parser.)
