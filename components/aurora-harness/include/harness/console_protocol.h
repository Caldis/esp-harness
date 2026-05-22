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
 *   console_reply_ok("dump start tag=DUMP w=128 h=128");
 *   console_begin_payload("DUMP", "w=128 h=128 fmt=RGB565 bytes=32768");
 *   console_write_raw(buf, len);    // call many times
 *   console_end_payload("DUMP");
 *
 * --- THE EXPLICIT-TAG CONVENTION (post-G-4) -------------------------
 *
 * Every OK: line that precedes a multi-line payload block MUST embed
 * `tag=<NAME>` somewhere in the body. The host-side parser
 * (esp_harness.core.parser.PayloadFollowsReader) matches the regex
 * `\btag=([A-Z][A-Z0-9_]*)\b` on the OK body to learn which tag is
 * about to arrive, so it can route the inner body to the right
 * consumer. Before this convention existed (gap G-4 in the
 * agent-dashboard project), consumers had to grep firmware source to
 * find out that `?help json` emitted HELP_BEGIN — the tag name was
 * implicit.
 *
 * Canonical forms used by built-ins and known consumers:
 *
 *   OK: manifest follows tag=HELP        (?help json)
 *   OK: scene manifest follows tag=SCENES (scene list)
 *   OK: dump start tag=DUMP w=N h=N ...  (?dump)
 *   OK: payload follows tag=HEALTH       (dash health, agent-dashboard)
 *
 * Free-form key=val pairs after `tag=` are encouraged — the host can
 * record them in the reply metadata for diagnostics (e.g. w_actual /
 * w_requested for ?dump when the requested size was downgraded). The
 * inner `<TAG>_BEGIN` line carries `fmt=` / `bytes=` etc. as a second
 * key=val payload; both are merged in the host's parse output.
 *
 * Legacy form `OK: payload follows` (no `tag=`) is still parsed by
 * the host helper for backward compatibility, but is discouraged.
 * -----------------------------------------------------------------*/
void console_begin_payload(const char *tag, const char *meta);
void console_write_raw(const char *data, size_t len);
void console_end_payload(const char *tag);

#ifdef __cplusplus
}
#endif
