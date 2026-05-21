/*
 * console_protocol implementation.
 *
 * Tokeniser: simple whitespace-split, no quoting, no escapes. Keep commands
 * cleanly tokenisable (no spaces in arg values).
 *
 * Output: goes through printf, which by ESP-IDF default routes to the chosen
 * console (USB-Serial/JTAG on this board). We do NOT install the usb_serial_jtag
 * driver for output — IDF console already handles it.
 *
 * Input: we DO install usb_serial_jtag driver, in receive-only mode, on the
 * same controller. They coexist because output uses VFS hooks above the
 * driver, while input we read via the driver directly.
 */

#include "harness/console_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "console";

static const console_cmd_t *s_registry[CONSOLE_MAX_COMMANDS];
static int s_registry_n = 0;

/* ── Reply helpers ──────────────────────────────────────────────────── */

static void vreply(const char *prefix, const char *fmt, va_list ap)
{
    char buf[CONSOLE_MAX_LINE];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) {
        n = 0;
    } else if ((size_t)n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    /* Single fwrite for atomicity vs interleaved log lines is best-effort
     * (ESP_LOG also uses stdout). The "OK:" prefix on its own line is the
     * reliable framing. */
    fputs(prefix, stdout);
    fwrite(buf, 1, n, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void console_reply_ok(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vreply("OK: ", fmt, ap); va_end(ap);
}
void console_reply_err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vreply("ERR: ", fmt, ap); va_end(ap);
}
void console_send_evt(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vreply("EVT: ", fmt, ap); va_end(ap);
}

void console_begin_payload(const char *tag, const char *meta)
{
    printf("%s_BEGIN %s\n", tag, meta ? meta : "");
    fflush(stdout);
}
void console_write_raw(const char *data, size_t len)
{
    fwrite(data, 1, len, stdout);
}
void console_end_payload(const char *tag)
{
    /* Ensure tag starts on its own line. */
    fputc('\n', stdout);
    printf("%s_END\n", tag);
    fflush(stdout);
}

/* ── Built-in commands ──────────────────────────────────────────────── */

static int cmd_ping(const console_args_t *a) { (void)a; console_reply_ok("pong"); return 0; }

/* JSON-escape a string for output between double quotes. Handles the
 * subset of characters our `help` strings actually contain — namely
 * `"` and `\`. Anything outside is passed through. We don't worry
 * about control characters because our help strings are author-curated
 * ASCII. */
static void emit_json_escaped(const char *s)
{
    if (s == NULL) return;
    for (const char *p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', stdout);
        }
        fputc(*p, stdout);
    }
}

static int cmd_help(const console_args_t *a)
{
    bool json_mode = (a->argc >= 2 &&
                      (strcmp(a->argv[1], "json") == 0 ||
                       strcmp(a->argv[1], "--json") == 0));
    if (!json_mode) {
        /* Text mode — readable in a serial monitor by humans. */
        console_reply_ok("%d commands", s_registry_n);
        for (int i = 0; i < s_registry_n; ++i) {
            printf("  %-12s  %s\n",
                   s_registry[i]->name,
                   s_registry[i]->help ? s_registry[i]->help : "");
        }
        fflush(stdout);
        return 0;
    }
    /* JSON mode — machine-readable manifest of every registered command.
     * The toolkit's `manifest` CLI calls this to enumerate firmware
     * capabilities without grepping source. Framed with HELP_BEGIN /
     * HELP_END so it can exceed CONSOLE_MAX_LINE. */
    console_reply_ok("manifest follows");
    console_begin_payload("HELP", "fmt=json");
    printf("{\"count\":%d,\"commands\":[", s_registry_n);
    for (int i = 0; i < s_registry_n; ++i) {
        printf("%s{\"name\":\"", i == 0 ? "" : ",");
        emit_json_escaped(s_registry[i]->name);
        printf("\",\"usage\":\"");
        emit_json_escaped(s_registry[i]->help ? s_registry[i]->help : "");
        printf("\"}");
    }
    printf("]}");
    fflush(stdout);
    console_end_payload("HELP");
    return 0;
}

static int cmd_reset(const console_args_t *a)
{
    (void)a;
    console_reply_ok("resetting");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
    return 0;
}

static const console_cmd_t s_builtin_ping  = { "?ping",  cmd_ping,  "liveness probe; returns 'pong'" };
static const console_cmd_t s_builtin_help  = { "?help",  cmd_help,  "list all commands" };
static const console_cmd_t s_builtin_reset = { "?reset", cmd_reset, "soft-reset (esp_restart)" };

/* ── Registration ──────────────────────────────────────────────────── */

void console_protocol_register(const console_cmd_t *cmd)
{
    if (cmd == NULL || cmd->name == NULL || cmd->fn == NULL) return;
    if (s_registry_n >= CONSOLE_MAX_COMMANDS) {
        ESP_LOGE(TAG, "registry full, dropping '%s'", cmd->name);
        return;
    }
    /* Replace if name already present, otherwise append. */
    for (int i = 0; i < s_registry_n; ++i) {
        if (strcmp(s_registry[i]->name, cmd->name) == 0) {
            s_registry[i] = cmd;
            return;
        }
    }
    s_registry[s_registry_n++] = cmd;
}

/* ── Parser ────────────────────────────────────────────────────────── */

static const console_cmd_t *lookup(const char *name)
{
    for (int i = 0; i < s_registry_n; ++i) {
        if (strcmp(s_registry[i]->name, name) == 0) return s_registry[i];
    }
    return NULL;
}

static void dispatch_line(char *line)
{
    /* Trim trailing CR / spaces */
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == ' ' || line[len-1] == '\t')) {
        line[--len] = '\0';
    }
    /* Skip leading whitespace */
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;  /* empty line: ignore silently */

    /* Tokenise — whitespace-separated, with double-quote support so
     * values containing spaces / apostrophes (e.g. wifi connect
     * ssid="My Wi-Fi") survive as a single argv entry. Quotes are
     * stripped in-place; backslash-escaping is intentionally NOT
     * supported (keeps the parser tiny — use a different separator if
     * you need a literal `"` in a value, which has never come up).
     */
    console_args_t args = { .argc = 0 };
    while (*p && args.argc < CONSOLE_MAX_ARGS) {
        char *out = p;
        args.argv[args.argc++] = out;
        bool in_quote = false;
        while (*p) {
            if (!in_quote && (*p == ' ' || *p == '\t')) break;
            if (*p == '"') {
                in_quote = !in_quote;
                p++;
                continue;
            }
            *out++ = *p++;
        }
        if (*p) {
            *p++ = '\0';
            while (*p == ' ' || *p == '\t') p++;
        }
        *out = '\0';
    }
    if (args.argc == 0) return;

    const console_cmd_t *cmd = lookup(args.argv[0]);
    if (cmd == NULL) {
        console_reply_err("unknown command: %s (use ?help)", args.argv[0]);
        return;
    }
    cmd->fn(&args);
}

static void console_task(void *arg)
{
    (void)arg;

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: 0x%x", err);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "console protocol ready · %d builtin + %d app cmds",
             3, s_registry_n - 3);

    static char line[CONSOLE_MAX_LINE];
    size_t pos = 0;
    uint8_t buf[64];

    while (1) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; ++i) {
            uint8_t c = buf[i];
            if (c == '\n') {
                line[pos] = '\0';
                if (pos > 0) {
                    dispatch_line(line);
                }
                pos = 0;
            } else if (c == '\r') {
                continue;  /* ignore CR; handled by trim too */
            } else if (pos + 1 < sizeof(line)) {
                line[pos++] = (char)c;
            } else {
                /* Overflow: drop the line and reset */
                pos = 0;
                console_reply_err("line too long, max %d bytes", (int)sizeof(line) - 1);
            }
        }
    }
}

void console_protocol_init(void)
{
    console_protocol_register(&s_builtin_ping);
    console_protocol_register(&s_builtin_help);
    console_protocol_register(&s_builtin_reset);

    xTaskCreate(console_task, "console", 8192, NULL, 5, NULL);
}
