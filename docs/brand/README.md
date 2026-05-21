# brand

Visual identity assets for esp-harness. Use these instead of recreating
the mark in slides / blog posts / talks.

## Mark concept

The logo is the letter **H** reframed as a control harness:

- Two black vertical strokes — the device side (firmware vs host, two endpoints under control).
- One rust-orange cross-bar — the dev-loop signal, the harness itself, the wire that ties the two endpoints together.
- A small paper-coloured dot at center — the manifest discovery point, the "one place that tells you what's wired in".
- A dashed outer rectangle — the device boundary, low-emphasis so the eye lands on the **H** first.

Read literally it's *just an H*. That's intentional — the secondary "harness as control structure" reading is for anyone who pauses.

## Files

| File | Use |
|---|---|
| `logo.svg` | 120×120 mark, light-background variant. Default. |
| `logo-dark.svg` | Inverted for dark backgrounds. |
| `wordmark.svg` | Mark + "esp-harness" wordmark in Fraunces serif. Use in headers. |
| `favicon.svg` | 32×32 simplified mark for browser tabs. |
| `social-card.svg` | 1280×640 Open Graph / Twitter card. The image attached to the GitHub repo. |

## Colors

| Name | Hex | Use |
|---|---|---|
| Ink | `#1c1814` | Body text, the vertical strokes of the H |
| Paper | `#f3eee2` | Background, centre dot |
| Rust | `#b8431a` | Accent, the cross-bar, links / emphasis |
| Moss | `#344a36` | "Done / passing" status (CI green, sim diff PASS) |
| Gold | `#b89020` | "Warning / blocked" status |
| Ink-mute | `#5a514a` | Secondary text |
| Ink-fade | `#8a807a` | Tertiary text, captions |

## Typography

| Role | Family | Notes |
|---|---|---|
| Display / wordmark | **Fraunces** | Variable serif. `opsz` axis for size-appropriate optical sizing; italic `WONK` for accents. |
| Body | **Geist** | Variable sans. Substitutes: system UI stack. |
| Mono | **IBM Plex Mono** | Code blocks, the monospace voice. |

## Don'ts

- **Don't stretch the mark.** Always keep 1:1 aspect.
- **Don't recolor the cross-bar.** The rust is load-bearing — it's the only accent in the whole brand.
- **Don't add a tagline next to the mark below 200 px wide.** Use the wordmark instead.
- **Don't put the light-background variant on photos / busy backgrounds.** Use the social card layout, which has a deliberate clean field.

If you need a variant that doesn't exist here (e.g. all-white for a dark sticker, monochrome for a thermal printer), open an issue rather than improvising — naming consistency matters at this layer.
