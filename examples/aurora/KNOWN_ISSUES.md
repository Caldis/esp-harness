# Aurora — Known issues, gotchas, future-AI handoffs

Notes to a future session (probably me) on what hurt and where the bodies
are buried. Pair with `README.md` and the toolkit's `AGENT.md`.

## 1. ~~CO5300 QSPI DMA TX underflow at boot (multi-scene)~~  **RESOLVED in v0.2**

**TL;DR fix.** In `components/esp32_s3_touch_amoled_2_16/esp32_s3_touch_amoled_2_16.c::bsp_display_lcd_init`, set `.use_psram = false` on the `esp_lv_adapter_display_config_t.profile`. One-line patch; everything else stays the same.

The diagnosis below is kept for posterity — useful when porting to a new board / a new BSP / a new LVGL version.

---

**Symptom.** When `aurora_main.c` registers more than one scene at startup,
the boot log shows:

```
I (xxxx) aurora: bsp_display_start ok
I (xxxx) console: console protocol ready · 3 builtin + 0 app cmds
E spi_master: DMA TX underflow detected
E lcd_panel.io.spi: panel_io_spi_tx_param: recycle spi transactions failed
E co5300_spi: panel_co5300_draw_bitmap: send command failed
E esp_lvgl:bridge_v9: Draw bitmap failed: ESP_ERR_INVALID_STATE
```

…and then the LVGL adapter task gets stuck in retry, holding the mutex
forever. `app_main` is then permanently parked at `bsp_display_lock(-1)`
on the very next line. Console keeps working (it's its own task), but
`harness_commands_register()` is never reached, so `?stat` / `?dump` /
`tap` / `scene` come back as `ERR: unknown command`.

**Workaround in v0.1.** Only `scene_halo` is `scene_fw_register`-ed. The
other two scenes are referenced via `(void)&scene_grid;` so the linker
keeps them, but they don't run at boot.

**What I think is happening.** Three widget-heavy scene containers, all
parented to `lv_screen_active()`, all created back-to-back, generate too
many initial-render invalidations for the CO5300 QSPI flush task to
keep up with. Once one bitmap-draw fails it cascades; the BSP's LVGL
adapter (`esp_lv_adapter`) hangs in its retry loop.

**Investigation log (all four hypotheses tested before the fix landed).**

1. ~~Render each scene to an `lv_canvas` instead of nested LVGL objects.~~
   **Tried and disproved.** A single 466×466 RGB565 canvas widget triggers
   exactly the same DMA underflow on its initial render. The bottleneck
   is NOT widget count or per-frame invalidation count — it's the bus's
   ability to ship a full-screen rectangle from PSRAM in one DMA burst.

2. ~~Stagger scene registration with `vTaskDelay`s.~~ Never needed —
   solving the underlying buffer-in-PSRAM problem made registration order
   irrelevant.

3. **BSP-level fix.** ✅ This was it. The bundled BSP (Waveshare's
   `esp32_s3_touch_amoled_2_16`) constructs the LVGL adapter display
   profile with `.use_psram = true`. ESP32-S3 GDMA can't pull from PSRAM
   fast enough to feed the SPI3 controller's FIFO on a full-screen
   render — the FIFO empties, `spi_master` reports `DMA TX underflow
   detected`, `panel_io_spi` fails to recycle transactions, the LVGL
   flush task wedges in its retry loop, and the LVGL adapter mutex
   stays held forever. `app_main` then blocks on `bsp_display_lock(-1)`
   and `harness_commands_register()` never runs. With `.use_psram =
   false` and `buffer_height = 50` (the BSP's default), the double
   buffer is `480 × 50 × 2 × 2 = 96 KB` in internal SRAM — fits easily
   in the S3's 512 KB and feeds the SPI controller without underflow.

   Credit: the [esp-bsp performance guide](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/docs/performance.md)
   recommends "buffer in SRAM" + "10–25 % of screen height" precisely
   for this case. Espressif's own docs note "SPI DMA does not directly
   support PSRAM" for LCD framebuffers.

4. ~~`CONFIG_SPIRAM_XIP_FROM_PSRAM=n`.~~ Not needed once (3) was fixed.
   May still be worth disabling if you're squeezing FPS further.

**Forensics that pointed at #3 specifically.**

* ESP-IDF GitHub issues [#8085 (IDFGH-6426)](https://github.com/espressif/esp-idf/issues/8085) (LCD DMA buffer in PSRAM doesn't work properly) and [#18177 (IDFGH-17166)](https://github.com/espressif/esp-idf/issues/18177) (ESP32-S3 LVGL QSPI screen freeze) both end with "buffer must be in SRAM" as the resolution.
* LVGL forum [#21062](https://forum.lvgl.io/t/help-with-double-buff-qspi-dma-on-esp32-s3/21062) ("Help with double buff QSPI DMA on ESP32-S3") and [#21242](https://forum.lvgl.io/t/buggy-homebrew-display-firmware-help-me-fix-the-co5300-sh8601-driver-lvgl9-setup/21242) (CO5300/SH8601 LVGL 9 setup) both arrive at the same answer.
* The error chain `DMA TX underflow → recycle spi transactions failed → panel_co5300_draw_bitmap send command failed → esp_lvgl:bridge_v9 Draw bitmap failed: ESP_ERR_INVALID_STATE` is a smoking gun for "SPI3 FIFO underflowing before transaction completion" — i.e. the source memory isn't being read fast enough.

**Why the v0.1 single-scene workaround worked.** With one scene the
initial render burst was just barely under the threshold; subsequent
flushes were small partial-redraws that PSRAM-DMA could handle. Three
scenes' initial renders pushed it over.

## 2. Animations on full-screen widgets break the same way

**Symptom.** A previous `scene_pulse` (preserved in git history under
the rename to `scene_halo`) had 4 concentric rings each driven by
`lv_anim` exec callbacks updating `lv_obj_set_size()` per LVGL refresh.
Same DMA underflow within a few seconds of boot.

**Lesson.** On this board, any scene that re-invalidates a large region
every refresh tick will eventually stall. Even with `CONFIG_LV_DEF_REFR_PERIOD=33`
(30 Hz) the QSPI doesn't keep up with full-screen repaints.

Static compositions are the safe path until the canvas-based renderer
lands.

## 3. `bsp_display_lock(timeout_ms)` semantics

The BSP header documents `0 = block indefinitely`. In practice, **passing
a non-zero `timeout_ms` does not yield a useful early-return** when the
LVGL task is hot — the lock starves on us at any reasonable timeout
(we tried 200 ms, 3000 ms). We worked around this in `harness_commands.c::
cmd_dump` by **scheduling the snapshot via `lv_async_call`**, which makes
it run inside the LVGL task context where the lock is already held.

Pattern worth copying for any other "I want to do something to LVGL state
from another task" need:

```c
static SemaphoreHandle_t s_done;
static lv_result_t       s_result;

static void my_lvgl_op(void *user) {
    // runs on LVGL task, already has the lock implicitly
    s_result = ...
    xSemaphoreGive(s_done);
}

int my_cmd_from_console_task(...) {
    if (!s_done) s_done = xSemaphoreCreateBinary();
    xSemaphoreTake(s_done, 0);            // drain stale give
    lv_async_call(my_lvgl_op, NULL);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
        // LVGL task is wedged — bail
    }
    // s_result is now valid
}
```

## 4. `pyserial` opens with DTR asserted on Windows → ESP32-S3 resets

ESP32-S3's native USB-Serial/JTAG controller treats DTR transitions as a
soft-reset trigger. pyserial's default behaviour on `open()` asserts DTR.
Result: every fresh `monitor` session would reboot the device, ruining
state continuity across commands.

The fix lives in `esp-harness-toolkit/src/esp_harness/core/serial_io.py`:

```python
ser = serial.Serial()
ser.port = port
ser.baudrate = baud
ser.dtr = False              # set BEFORE open
ser.rts = False
ser.open()
ser.dtr = False              # belt-and-suspenders — some drivers re-assert
ser.rts = False
```

Without this, multi-tap testing across captures was impossible — each
new session started from a fresh boot.

## 5. `?dump` — the four-layer screenshot pipeline post-mortem

The current screenshot path (`?dump` → toolkit `esp-harness screenshot`)
went through **four distinct failure modes** before stabilising. Worth
recording because each was misdiagnosed as the previous and the lessons
are independent.

### 5.1. Chromatic speckles on anti-aliased shapes  *(quality)*

**Symptom.** The first working screenshots showed rainbow speckles
along the rings — most visibly on the outer (low-opacity) ring. The
physical display was clean; the artefact lived in the dump.

**Cause.** Firmware was nearest-neighbour downsampling 466 × 466 → 128 × 128.
LVGL renders the rings with anti-aliasing, so the edge has many partially
blended pixels. RGB565 in 5/6/5 doesn't quantise hue uniformly — adjacent
blended pixels round to different colours. Each output pixel picks one
source pixel at random of the AA edge, so the colour jumps unpredictably.

**Fix (`harness_commands.c::cmd_dump`).** Box-filter average instead of
point sample: each output pixel is the mean of every source pixel in its
source-space bin. Average in 8-bit RGB (after bit-rep expand from 5/6),
then re-quantise to RGB565 for the wire.

```c
for (int sy = sy_lo; sy < sy_hi; ++sy) {
    for (int sx = sx_lo; sx < sx_hi; ++sx) {
        uint16_t v = srow[sx];
        rs += (((v >> 11) & 0x1F) << 3) | (((v >> 11) & 0x1F) >> 2);
        gs += (((v >>  5) & 0x3F) << 2) | (((v >>  5) & 0x3F) >> 4);
        bs += (( v        & 0x1F) << 3) | (( v        & 0x1F) >> 2);
        n++;
    }
}
```

### 5.2. ESP_LOG lines bleed into the base64 stream  *(protocol noise)*

**Symptom.** At 192 px (~9 s on the wire), about 70 % of dumps would
fail host-side with `Incorrect padding` or `4n+1 mod 4` errors. Smaller
dumps (~4 s) almost never failed. Easy to misdiagnose as USB-CDC drops.

**Cause.** The firmware emits base64 lines into stdout — but **every
other task that calls `ESP_LOGI` writes into the same stdout**. A
heartbeat ESP_LOG happening during a 9 s dump appears as a line
like `I (12345) aurora: heartbeat #5` in the middle of the base64
stream. The host parser, in `in_payload` mode, treats it as another
payload line. The naive "filter to base64 alphabet" then keeps the
alphanumeric tokens (`I12345auroraheartbeat5`), silently inflating the
data count and breaking 4-char alignment.

The signal that gave it away: failures cluster around the 10-second
heartbeat boundary, not at byte-count thresholds.

**Fix (`screenshot.py`).** Validate **per-line**, not per-character.
A valid base64 line is *purely* alphabet bytes. ESP_LOG lines contain
spaces, colons, parens, hashes — drop the whole line if any non-base64
char appears.

```python
B64_LINE_CHARS = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")
valid_lines = [ln for ln in resp.payload.split(b"\n")
                if (ln := ln.strip()) and all(c in B64_LINE_CHARS for c in ln)]
clean = b"".join(valid_lines)
```

**Future hardening:** in firmware, `esp_log_level_set("*", ESP_LOG_NONE)`
around the dump emission would prevent the interleave at source. We
chose to do host-side defence first because it works for any firmware
that uses console_protocol — but firmware-side silence is cleaner and
should land in v0.2.

### 5.3. `lv_layer_top` content missing from snapshots  *(rendering scope)*

**Symptom.** After 5.1 and 5.2 were fixed, the rings were clean and
sharp, but the **UI shell chrome** ("I. Halo" header label, the bottom
indicator dot) was absent from the dump even though it was clearly
visible on the physical screen.

**Cause.** `lv_snapshot_take(lv_screen_active(), ...)` traverses the
active screen subtree only. `lv_layer_top()` is a parallel root that
LVGL composites *over* the screen at flush time — semantically it's
on the same display but it's not a descendant of the screen.

**Fix (`harness_commands.c::aurora_do_snapshot`).** After the screen
snapshot, also snapshot the top layer as **ARGB8888** (so we get an
alpha channel), then alpha-blend it over the RGB565 screen buffer
in-place. ~870 KB transient ARGB buffer goes to PSRAM; failure is
non-fatal (the chrome just won't appear in that one dump).

### 5.4. Fixed 8-second timeout truncates 192 px captures  *(timing)*

**Symptom.** 192 px dumps occasionally returned with `ok=False` and
empty error text, even though the raw_tail showed a complete payload
ending in `DUMP_END`.

**Cause.** Default toolkit timeout was 8.0 s. 192 px at ESP32-S3 USB-CDC
throughput takes ~9.1 s end-to-end. The session would close mid-stream;
`DUMP_END` was in the OS buffer but never read.

**Fix (`screenshot.py`).** Auto-scale timeout with the requested size:
`max(4.0, 2.0 + raw_bytes * 0.00012)`. 192 px → ~10.8 s budget, 256 px →
~18 s. Users can override with `--timeout N`.

### Outcome

After all four layers, captures at 96 / 128 / 192 px are stable. **128 is
the daily sweet spot** (4 s, fully reliable, comfortably below the heartbeat
window). 192 is the "look closely" option at 9 s.

256 px still fails reliably — diagnosing it would require either:

* binary framing with CRC32 + retry on the wire (research-agent-recommended
  for v0.2),
* or chunked emit with `vTaskDelay(1)` between 16-line chunks to let USB
  CDC TX drain.

Either is multi-hour; deferred until someone actually needs higher
resolution than 192.

## 6. Custom partition layout

`partitions.csv` uses `factory 8 MB + storage 7 MB spiffs`. The original
ESP-IDF default is much smaller (1 MB factory). Don't reset to defaults
unless you also remove `CONFIG_PARTITION_TABLE_CUSTOM=y` from sdkconfig
or the build will fail to fit.

## 7. ESP-IDF v6.0 vs Waveshare BSP (v5.5 era)

Three patches were necessary to make the bundled `components/esp32_s3_touch_amoled_2_16/`
build on IDF v6.0. They're all in `CMakeLists.txt` of that component:

* drop `usb` from `PRIV_REQUIRES` (unused; removed from IDF umbrella)
* add `esp_driver_i2s esp_driver_sdmmc` to `REQUIRES` (drivers split out)
* remove the unused `#include "driver/ledc.h"` from `esp32_s3_touch_amoled_2_16.c`

If you upgrade ESP-IDF, check whether these have rotted further.

## 8. `LV_USE_PERF_MONITOR` / `LV_USE_SYSMON`

Both `=n` in our sdkconfig — they add an always-on opaque overlay widget
that destroys the aesthetic and adds load. Don't re-enable unless you're
benchmarking. We have `?stat` for that.

## 11. ~~BLE host stack fails silently on internal SRAM~~  **RESOLVED**

**TL;DR fix.** Set `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` in
`sdkconfig.defaults` (and the same for WiFi+BLE coexistence:
`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`). NimBLE buffers move out of
internal SRAM into PSRAM, and the stack actually initialises.

---

The diagnosis below is kept for posterity — useful when porting to a
new board / heavier UI / additional peripherals that compete for the
~500 KB of internal SRAM.

**Symptom progression** (each new symptom appeared after fixing the
previous one):

1. **`hci inits failed` → `nimble_port_init: ESP_FAIL`** at boot.
   Fixed by `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`. ESP32-S3 needs
   software coexistence even for BLE-alone use (the controller and
   host need to arbitrate radio access, and on this chip that requires
   the coex library to be linked).

2. **`controller init -2 → ESP_ERR_NO_MEM`** when BLE init runs
   AFTER WiFi init. WiFi grabs ~80 KB of internal SRAM for static
   buffers; the BT controller alloc that needs ~32 KB then fails.
   Workaround: eagerly initialise BLE at boot, BEFORE the first
   WiFi-using command runs.

3. **`xTaskCreate(host) failed (-1)` while heap free 7 MB.**
   The 7 MB is PSRAM. FreeRTOS stacks live in internal SRAM, which by
   this point in boot is down to 26 KB total / 18 KB largest contiguous
   block (LVGL takes ~96 KB for its double buffer, audio DMA takes
   another ~30 KB, plus assorted task TCBs). Reducing the host task
   stack from 8 KB → 4 KB didn't help — the problem is NimBLE
   *itself* allocating its HCI buffer pools from internal SRAM via
   `esp_nimble_init`.

4. **The actual fix:** `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`.
   Routes NimBLE pool allocations to PSRAM. After this change,
   `host task entered` and `host sync'd` both fire within ~10 ms of
   `nimble_port_init`, and `ble_gap_disc` returns valid advertising
   data.

**Validation:** `ble scan 2500 5` returns `count=5, adv_events=536`
in 3006 ms. The adv_events figure is the AI's smoking-gun: every
advertising packet received from any device increments it, so 536/3 s
≈ 180 packets/sec which is a healthy ambient density for an urban
environment and *cannot* be faked by a stalled stack.

**Internal SRAM diagnostic log we added during the hunt:**
```c
heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
```
…before BLE init prints `internal SRAM free=26255 largest_block=18432`.
If the largest internal block ever drops below ~32 KB and you're seeing
mysterious WiFi/BLE/audio alloc failures, internal-SRAM contention is
almost certainly the cause — move the offending stack to PSRAM via the
appropriate `_MEM_ALLOC_MODE_EXTERNAL` Kconfig.

**What we kept for the diagnostic value, even after the fix:**
* The pre-host SRAM size log in `peripherals/ble.c` — confirms PSRAM
  routing worked and provides early warning if a future change
  re-tightens internal SRAM budget.
* Our custom `xTaskCreate` (replacing `nimble_port_freertos_init`)
  with explicit return-value check — the upstream wrapper silently
  drops `xTaskCreate` errors, which was the symptom that made #3 so
  hard to chase.

## 12. WiFi + BLE mutual exclusion (Phase I, partial)

Once BLE has eagerly initialised at boot, the first `wifi scan` fails
with `wifi:create wifi task: failed to create task` →
`esp_wifi_init: ESP_ERR_NO_MEM`. The WiFi driver task's TCB+stack live
in internal SRAM, which is already squeezed by LVGL (96 KB double
buffer) + audio I²S DMA + BLE controller pool (BLE host pool moved to
PSRAM via §11, but the controller side is still ~25 KB internal).

Earlier boots in this repo's history showed WiFi-first-then-BLE works
fine (the boot log even ran a successful `wifi scan` returning 5 real
APs), so both stacks individually function — they just can't coexist
in the residual internal SRAM the rest of Aurora leaves behind.

**What we ship.** WiFi scanning works on the FIRST boot after a flash,
provided you don't trigger BLE. BLE scanning works after eager-init
at boot, and is the path used by Aurora today. Phase I demo loop
should call `ble scan` and document `wifi scan` as a "reset-required"
operation, OR rebuild with BLE disabled and treat the firmware as
WiFi-mode.

**Future paths** (not pursued — out of scope for the "touch every
peripheral" milestone):

* Add a `?radio mode=wifi|ble` command that does
  `esp_bt_controller_disable + esp_bt_controller_deinit` before WiFi
  init, and the reverse on demand. This is the canonical WiFi+BLE
  coexistence pattern when both can't simultaneously fit.
* Cut LVGL draw buffer to 25 rows (would free ~50 KB internal SRAM)
  and accept lower fps.
* Move audio I²S DMA buffers to a smaller config or to PSRAM (PSRAM
  DMA on ESP32-S3 is slower but works for 22 kHz audio).

The infrastructure (`peripherals/wifi.c`, `peripherals/ble.c`, both
console commands) is in place. Re-enable WiFi by either path above
when needed.

## 10. AXP2101 PMIC — Waveshare BSP doesn't expose it (Phase F)

The board has an AXP2101 PMIC on the shared I²C bus (SDA=15 SCL=14,
7-bit address 0x34) but the BSP makes no mention of it — no
`bsp_battery_*`, no power-rail helpers, no GPIO defines for an interrupt
line. Confirmed via the Waveshare wiki and the AXP2101 STATUS1 probe in
firmware (chip ACKs the read).

We did NOT pull in XPowersLib: it's C++, brings its own I²C bus
configuration (not the BSP's shared bus), and needs menuconfig switches.
For a four-register read loop (battery present / VBUS in / charge state
/ percent / voltage), `peripherals/pmic.c` is a 130-line direct driver
that hooks into the BSP I²C bus via `bsp_i2c_get_handle()` — same
pattern as IMU.

**Register subset we touch:**

| Reg  | Name              | Notes |
|------|-------------------|-------|
| 0x00 | STATUS1           | bit 5 = VBUS good, bit 3 = battery present |
| 0x01 | STATUS2           | low 3 bits = charge phase; bits 5–6 direction |
| 0x34 | VBAT_H            | high 5 bits of battery voltage (mV, direct) |
| 0x35 | VBAT_L            | low 8 bits (combine: `((vh & 0x1F) << 8) | vl`) |
| 0xA4 | BAT_PERCENT       | fuel-gauge SoC 0..100 |

**No battery attached, just USB?** Then `battery=false` and we
report `percent=-1, voltage_mv=0, charge=off`. Scene V (Pulse) reads
this and shows the warm-amber "discharging on USB / no battery"
palette with an invisible core (opacity gated on `percent > 0`).

**Bit semantics inferred from XPowersAXP2101.hpp** in lewisxhe/XPowersLib.
The full AXP2101 datasheet has a much wider register map (DC1–DC5,
LDO1–LDO5, ALDO1–4, BLDO1–2, fault registers, IRQ control, ADC channel
selection, watchdog, on-key, etc.) — leaving all that for future phases
to enable as needed.

## 9. QMI8658 IMU — three traps stacked on top of each other (Phase E)

Adding IMU support hit three independent bugs that all masked each other.
Captured here so the next peripheral integration doesn't repeat them.

### 9a. Don't `set_accel_range/odr` after `qmi8658_init`

The Waveshare `qmi8658_init()` is the **final** init path — full reset
sequence + CTRL1/2/3/7 + sensor enable. Calling
`qmi8658_set_accel_range(4G)` or `qmi8658_set_accel_odr(125HZ)` afterwards
puts the chip into a stuck state where the data registers stay pegged at
`0x7FFF / 0x8000` and `STATUS0` never goes high (data-ready bit never
sets). WHO_AM_I still reads 0x05; CTRL1/2/7 still read back the expected
values — but no fresh samples ever land.

**Rule:** read the vendor init first, then either accept its defaults or
follow exactly the disable-reconfig-enable dance it implements. Don't
poke individual config registers after init.

### 9b. Output unit is milli-g, not g

With `accel_unit_mps2 = false` (the lib default), `qmi8658_read_accel`
returns **milli-g**. 1 g of gravity reads as ~1000 — and the divisor
math line is `(raw * 1000.0f) / accel_lsb_div`. We convert mg → g in
`imu_get_accel` so the rest of the firmware sees clean ±1.0 g values.
This trap is invisible until something else goes wrong, because the
chip's output range looks "reasonable" either way.

### 9c. Non-LVGL tasks calling LVGL APIs deadlock once a scene has a timer

This was the gnarly one. `cmd_scene` (running on the console task) called
`scene_fw_show` + `ui_shell_set_active` → `lv_label_set_text` →
`lv_obj_invalidate` without holding the LVGL lock. Worked fine for the
first 3 scenes (static or self-animating via `lv_anim` which runs on the
LVGL task and thus is naturally serialised). Tilt scene introduced a
20 Hz `lv_timer` (running on the LVGL task) that mutates obj position
via `lv_obj_set_pos`. Now there are two writers to LVGL's internal
invalidate-area list. The list corrupts, `lv_inv_area` chases a bad
pointer indefinitely, IDLE1 starves, task watchdog fires.

Backtrace pointed at `lv_inv_area at lv_refr.c:282` — the giveaway:
ridiculously deep stack into LVGL internals from a console command.

**Fix:** wrap LVGL mutations in `cmd_scene` (and by extension any
console command, sampling task, network callback, etc.) with
`bsp_display_lock(0)` / `bsp_display_unlock()`.

```c
bsp_display_lock(0);
scene_fw_show(idx);
ui_shell_set_active(idx, c->display_name);
bsp_display_unlock();
```

The physical-tap handler doesn't need locking because it's registered as
an LVGL event callback and is dispatched from the LVGL task (the task
already holds the lock on its own behalf).

**General rule for this codebase:** *any* LVGL call from a task other
than the LVGL adapter task must be lock-wrapped. Pre-existing places
that already follow this pattern: `?dump`'s snapshot path (uses
`lv_async_call` instead, which is equivalent — defers to the LVGL task).

### 9d. Calibration: don't assume "flat on the table"

Earlier code did `s_off_z = avg_z - 1.0f` — assuming the board is flat,
Z = +1 g at rest. That's wrong if the board lives on a dev-station with
USB-C facing down (gravity sits on X instead). New scheme: capture
whatever pose the board is in at boot as the "neutral pose"; readings
are deltas from neutral. Cost: we lose absolute-tilt info; gain: setup
is orientation-free. Good enough for spirit-level visuals; revisit if a
scene actually needs absolute pitch/roll.

### 9e. `M_PI` collision in `managed_components/waveshare__qmi8658/qmi8658.h`

The upstream header does an unconditional `#define M_PI (3.14159…)`.
That collides with newlib's `<math.h>`, which also defines M_PI when
`_USE_MATH_DEFINES` is implied (it is, on the IDF toolchain). Local
patch adds `#ifndef M_PI` guard. Lives in
`managed_components/waveshare__qmi8658/include/qmi8658.h` and will be
clobbered on any `idf.py reconfigure` that re-downloads the component —
keep an eye on it after every dependency refresh.

Same component also needs `esp_driver_i2c` added to its `REQUIRES`
(already patched in its CMakeLists.txt for the same reason as §7).
