"""Semantic exit codes for esp-harness commands.

Agents rely on exit codes to branch logic. Stdout is for humans/JSON payload;
exit code is the authoritative success/failure signal.

Ranges:
    0       success
    1-2     generic / CLI misuse
    10-19   device / port problems
    20-29   build / configuration problems
    30-39   flash problems
    40-49   monitor / serial-capture problems
    50-59   simulator (Phase 2)
    60-69   visual capture (Phase 2)
    100+    environment / installation problems
"""

# generic
OK = 0
GENERIC_ERROR = 1
CLI_MISUSE = 2

# device / port
NO_DEVICE = 10
DEVICE_BUSY = 11
AMBIGUOUS_DEVICE = 12

# build
BUILD_FAILED = 20
PROJECT_NOT_FOUND = 21
SDKCONFIG_ERROR = 22

# flash
FLASH_FAILED = 30
FLASH_VERIFY_FAILED = 31

# monitor
MONITOR_TIMEOUT = 40
MONITOR_DISCONNECTED = 41

# environment
ENV_NOT_CONFIGURED = 100
EIM_NOT_FOUND = 101
