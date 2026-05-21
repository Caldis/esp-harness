# AGENT.md — hello_minimal

> AI session onboarding for this project. The full conventions live in
> [esp-harness/AGENT.md](https://github.com/Caldis/esp-harness/blob/master/AGENT.md).
> This file is the project-specific addendum.

## Bootstrap

```bash
esp-harness manifest --json   # what does this project + harness expose?
esp-harness doctor            # env healthy?
```

## Where things go

| Adding | Goes in |
|---|---|
| New scene | `main/scenes/scene_<name>.c` + register in `hello_minimal_main.c` |
| New peripheral | `main/peripherals/<name>.{c,h}` + REQUIRES in main/CMakeLists.txt |
| New console command | inline in `hello_minimal_main.c` or a new `main/commands.c` |
| Generic reusable primitive | Open a PR against esp-harness upstream (`components/aurora-harness/`) |

## Don't break the manifest

Every `console_protocol_register()` auto-surfaces in `?help json`. Every
`scene_fw_register()` auto-surfaces in `scene list`. So if you add a
thing, just register it — no separate documentation step.

## Build / run loop

```bash
esp-harness build               # IDF build
esp-harness flash               # esptool flash
esp-harness run --until "X"     # build + flash + monitor with early-exit
esp-harness sim diff            # (if sim/ is set up) visual regression
```

See the root `README.md` for the longer "Quickstart" + the rest.
