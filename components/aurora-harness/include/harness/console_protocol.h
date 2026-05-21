/*
 * console_protocol — line-based bidirectional console for the AI loop.
 *
 *   host → device:  "<command> [arg1] [arg2] ...\n"
 *   device → host:  "OK: <text>\n" or "ERR: <reason>\n" or "EVT: <text>\n"
 *                   plus multi-line payloads delimited by BEGIN/END markers
 *
 * Each command is a registered handler. A built-in `?help` lists everything.
 *
 * Built-in commands provided by console_protocol_init():
 *   ?ping                 → OK: pong
 *   ?help                 → list of registered commands, one per line
 *   ?reset                → soft reset the device (esp_restart)
 *
 * Application code registers more (e.g. tap, ?dump, ?stat) with
 * console_protocol_register().
 */

#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONSOLE_MAX_ARGS       8
/* Max bytes per OK:/ERR: line. Bumped from 256 to 1024 so the richer
 * JSON-returning commands (`?sys`, `wifi scan top-5`, etc.) fit a single
 * reply without needing payload framing. The buffer is a single static
 * in console_reply_ok; ~700 extra bytes is rounding error. */
#define CONSOLE_MAX_LINE       1024
#define CONSOLE_MAX_COMMANDS   32

typedef struct {
    int          argc;
    const char  *argv[CONSOLE_MAX_ARGS];
} console_args_t;

typedef int (*console_handler_fn)(const console_args_t *args);

typedef struct {
    const char *name;
    console_handler_fn fn;
    const char *help;
} console_cmd_t;

/* Start serial input listener task + register built-in commands.
 * Call once at startup, after esp_log is set up. */
void console_protocol_init(void);

/* Register an application command. The pointer must remain valid for the
 * lifetime of the program (use static storage). */
void console_protocol_register(const console_cmd_t *cmd);

/* Reply helpers — called from a command handler to acknowledge. */
void console_reply_ok(const char *fmt, ...);
void console_reply_err(const char *fmt, ...);

/* Asynchronous event line — not in response to a command. Used by e.g.
 * touch hit reporting or scene transitions for AI observability. */
void console_send_evt(const char *fmt, ...);

/* Multi-line payload framing for binary-ish data (e.g. base64 framebuffer).
 *   console_begin_payload("DUMP", "w=128 h=128 fmt=RGB565 bytes=32768");
 *   console_write_raw(buf, len);    // call many times
 *   console_end_payload("DUMP");
 * Host parser knows BEGIN/END markers as transport boundaries. */
void console_begin_payload(const char *tag, const char *meta);
void console_write_raw(const char *data, size_t len);
void console_end_payload(const char *tag);

#ifdef __cplusplus
}
#endif
