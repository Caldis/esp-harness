---
name: Bug report
about: Something broken, hung, or behaving unexpectedly
title: 'bug: '
labels: bug
assignees: ''
---

## What happened

A clear description of the bug.

## What you expected

A clear description of what should have happened instead.

## How to reproduce

Minimal steps from a fresh clone:

```bash
# e.g.
git clone https://github.com/Caldis/esp-harness && cd esp-harness
pip install -e tools/esp-harness/
esp-harness new bug-repro
cd bug-repro
esp-harness build
# → error here
```

## Output

```
<paste the full error + last 20-30 lines of context>
```

## Environment

Please run and paste:

```bash
esp-harness --version
esp-harness doctor --json
```

Plus:

- **OS** (and version): e.g. Windows 11 / Ubuntu 22.04 / macOS 14.2
- **ESP-IDF version**: `idf.py --version`
- **Board** (if device-side issue): e.g. Waveshare ESP32-S3-Touch-AMOLED-2.16

## Additional context

Anything else that might help — a stack trace, a partial workaround,
a related GitHub issue, etc.
