# hello-minimal

The smallest possible esp-harness consumer. One scene, one console
command, no peripherals. **Use this as your starting point** when
adding a new example to the monorepo, or as a reference of "what the
floor looks like" when building your own project.

> **Note**: this example exists primarily for documentation. It doesn't
> target a specific board — it builds cleanly via ESP-IDF but the BSP
> resolution depends on what board you set as the IDF target. For
> hands-on hardware testing, use the Aurora demo at
> [`examples/aurora/`](../aurora/) instead.

## File tree

```
hello-minimal/
├── CMakeLists.txt              top-level ESP-IDF project file
├── partitions.csv              standard factory layout
├── sdkconfig.defaults          minimal Kconfig (PSRAM, USB-Serial-JTAG)
├── AGENT.md                    project-specific AI session notes
├── main/
│   ├── CMakeLists.txt
│   ├── hello_minimal_main.c    app_main + display + scene framework
│   └── scenes/
│       ├── scenes.h
│       └── scene_hello.c       one demo scene (centered label)
└── README.md
```

Total: ~150 lines of code (excluding the harness component).

## How it consumes aurora-harness

`CMakeLists.txt` uses `link` vendoring mode (the monorepo path is
relative):

```cmake
list(APPEND EXTRA_COMPONENT_DIRS
    ${ESP_HARNESS_ROOT}/components
    ${ESP_HARNESS_ROOT}/boards
)
```

where `ESP_HARNESS_ROOT` defaults to `../..` — the monorepo root.

## What you get from the component

By calling `harness_default_register()` once after
`console_protocol_init()`, this minimal app gains:

- `?ping` / `?reset` / `?help` (always on, from console_protocol_init)
- `?stat` — fps / heap / scene as JSON
- `scene next|prev|N|<id>|name|action|list` — navigation
- `tap [X Y]` — synthetic touch
- `swipe X1 Y1 X2 Y2 [DUR]` — synthetic gesture
- `?dump [w=N]` — screenshot as base64 RGB565 payload

All of these are usable from the host immediately:

```bash
esp-harness manifest --json     # see everything available
esp-harness console --cmd "?stat"
esp-harness console --cmd "scene list" --payload SCENES
```

## Build (if you have ESP-IDF set up)

```bash
cd examples/hello-minimal
idf.py set-target esp32s3        # or whatever your board is
esp-harness build
```

You'll likely need to:

1. Set the right IDF_TARGET for your board (or add it to
   `sdkconfig.defaults`).
2. Add your board's BSP component to `main/CMakeLists.txt` REQUIRES
   (the generated template has a comment placeholder).

For the Aurora demo board specifically (Waveshare ESP32-S3-Touch-AMOLED-2.16),
the BSP is already in this monorepo at
`boards/esp32_s3_touch_amoled_2_16/`.

## Why is this here?

Three reasons:

1. **Onboarding proof**: a brand-new user can `esp-harness new
   my-thing` and see this exact shape come out. Knowing the
   target looks like this reduces "what should my project look like"
   anxiety.

2. **Multi-example readiness**: the moment we have a second example
   in this directory, the "examples can be alternatives, not all
   subordinate to Aurora" message is real. hello-minimal is that
   second example.

3. **Template integrity check**: if `esp-harness new` produces
   something that doesn't even compile to a clean intermediate
   state, hello-minimal catches it.

## Where to look next

- Need more capability? → [`../aurora/`](../aurora/) — same scaffolding, full demo
- Want to learn the API? → [`../../components/aurora-harness/README.md`](../../components/aurora-harness/README.md)
- Want to bring up a new board? → [`../../components/aurora-harness/PORTING.md`](../../components/aurora-harness/PORTING.md)
