/*
 * sim/main.c — host entry for the Aurora simulator.
 *
 * Brings up LVGL + the SDL2 display/indev driver, registers the same
 * scene framework as the target, wires up the pure-render scenes, and
 * runs an event loop with these bindings:
 *
 *   click / tap     → scene next (LVGL short-click on root)
 *   long-press      → scene action (callback)
 *   key 1           → BOOT button (passed to scene via keys mock)
 *   key 2           → USER button
 *   Escape          → quit
 *
 * Scenes that touch peripherals other than display/touch/keys read from
 * mock_peripherals.c — see that file for what fake state they see.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"

#include "harness/scene_framework.h"
#include "scenes/scenes.h"
#include "ui_shell.h"
#include "mock_bsp.h"
#include "mock_peripherals.h"

#define SCREEN_W 466
#define SCREEN_H 466

/* CLI options — see usage block in main(). */
typedef struct {
    int          start_scene;     /* index, default 0 */
    int          exit_after_ms;   /* 0 = run forever */
    const char  *snapshot_path;   /* NULL = no snapshot */
} sim_opts_t;

static void parse_args(int argc, char **argv, sim_opts_t *o)
{
    o->start_scene   = 0;
    o->exit_after_ms = 0;
    o->snapshot_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--scene") && i + 1 < argc) {
            o->start_scene = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--exit-after-ms") && i + 1 < argc) {
            o->exit_after_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) {
            o->snapshot_path = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("aurora_sim — host LVGL build\n"
                   "  --scene N            start on scene index N (default 0)\n"
                   "  --exit-after-ms MS   auto-quit after MS ms (default: run forever)\n"
                   "  --snapshot PATH      write SDL window contents as BMP at exit\n");
            exit(0);
        }
    }
}

/* Mirror Aurora's chrome-sync listener pattern (v1.4): one source of
 * truth for "scene changed" side-effects, fires both for tap-driven
 * navigation and any external scene_fw_show. */
static void on_sim_scene_changed(int idx, const scene_t *current)
{
    if (current) ui_shell_set_active(idx, current->display_name);
}

static void on_short_click(lv_event_t *e)
{
    (void)e;
    scene_fw_next();
}

static void on_long_press(lv_event_t *e)
{
    (void)e;
    const scene_t *c = scene_fw_current();
    if (c && c->on_long_press) c->on_long_press((scene_t *)c);
}

/* Snapshot the SDL window's current contents to BMP. Called at exit
 * if --snapshot was supplied. Uses SDL_RenderReadPixels from LVGL's
 * SDL backend renderer — works because LVGL has already presented the
 * current frame by the time we get here. */
static int save_window_snapshot(lv_display_t *disp, const char *path)
{
    SDL_Window *win = lv_sdl_window_get_window(disp);
    if (!win) { fprintf(stderr, "snapshot: no SDL window\n"); return -1; }
    SDL_Renderer *r = SDL_GetRenderer(win);
    if (!r) { fprintf(stderr, "snapshot: no renderer\n"); return -1; }
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) { fprintf(stderr, "snapshot: surface alloc: %s\n", SDL_GetError()); return -1; }
    if (SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888,
                              s->pixels, s->pitch) != 0) {
        fprintf(stderr, "snapshot: ReadPixels: %s\n", SDL_GetError());
        SDL_FreeSurface(s);
        return -1;
    }
    if (SDL_SaveBMP(s, path) != 0) {
        fprintf(stderr, "snapshot: SaveBMP %s: %s\n", path, SDL_GetError());
        SDL_FreeSurface(s);
        return -1;
    }
    SDL_FreeSurface(s);
    printf("snapshot saved %dx%d -> %s\n", w, h, path);
    return 0;
}

int main(int argc, char **argv)
{
    sim_opts_t opts;
    parse_args(argc, argv, &opts);
    printf("Aurora simulator starting (host LVGL + SDL2)\n");

    lv_init();

    /* SDL display + mouse + keyboard inputs registered via LVGL's
     * built-in drivers (enabled by LV_USE_SDL=1 in lv_conf.h). */
    lv_display_t *disp = lv_sdl_window_create(SCREEN_W, SCREEN_H);
    if (!disp) { fprintf(stderr, "lv_sdl_window_create failed\n"); return 1; }
    lv_sdl_mouse_create();
    lv_sdl_keyboard_create();

    /* Mock peripherals + settings + system init mirror the order in
     * aurora_main.c so any scene that depends on it sees ready state. */
    settings_init();
    system_init();
    imu_init();
    pmic_init();
    audio_init();
    keys_init();

    /* Build UI on the active screen, then register host-portable scenes.
     * See CMakeLists.txt::AURORA_SCENES for what's wired in. */
    const int kSceneCount = 13;
    ui_shell_init(kSceneCount);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_short_click, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, on_long_press,  LV_EVENT_LONG_PRESSED,  NULL);

    scene_fw_init(scr);
    scene_fw_set_change_listener(on_sim_scene_changed);
    scene_fw_register(&scene_halo);     /* 0  I    Halo */
    scene_fw_register(&scene_grid);     /* 1  II   Grid */
    scene_fw_register(&scene_bloom);    /* 2  III  Bloom */
    scene_fw_register(&scene_tilt);     /* 3  IV   Tilt — mouse-driven */
    scene_fw_register(&scene_pulse);    /* 4  V    Pulse — fake charge */
    scene_fw_register(&scene_cell);     /* 5  X    Cell — pmic dashboard */
    scene_fw_register(&scene_keys);     /* 6  XI   Keys — SDL 1/2 keys */
    scene_fw_register(&scene_tone);     /* 7  XIII Tone — audio no-op */
    scene_fw_register(&scene_system);   /* 8  XIV  System — static info */
    scene_fw_register(&scene_glow);     /* 9  XV   Glow — brightness log */
    scene_fw_register(&scene_spin);     /* 10 XVI  Spin — mouse gyro */
    scene_fw_register(&scene_notify);   /* 11 XIX  Notify — toast demo */
    scene_fw_register(&scene_track);    /* 12 XX   Track  — progress demo */

    if (opts.start_scene > 0 && opts.start_scene < kSceneCount) {
        scene_fw_show(opts.start_scene);
    }
    /* Listener handled chrome sync for the auto-show and any restore above. */

    Uint32 deadline_ticks = 0;
    if (opts.exit_after_ms > 0) {
        deadline_ticks = SDL_GetTicks() + (Uint32)opts.exit_after_ms;
    }

    /* Event loop. SDL events get dispatched by the LVGL SDL driver; we
     * just step the tick + handler and check for our own keyboard
     * shortcuts ourselves. */
    uint32_t last_tick = SDL_GetTicks();
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_1: mock_keys_set_boot(true); break;
                    case SDLK_2: mock_keys_set_user(true); break;
                    default: break;
                }
            }
            if (ev.type == SDL_KEYUP) {
                switch (ev.key.keysym.sym) {
                    case SDLK_1: mock_keys_set_boot(false); break;
                    case SDLK_2: mock_keys_set_user(false); break;
                    default: break;
                }
            }
        }

        uint32_t now = SDL_GetTicks();
        lv_tick_inc(now - last_tick);
        last_tick = now;
        lv_timer_handler();

        if (deadline_ticks && SDL_GetTicks() >= deadline_ticks) running = false;

        SDL_Delay(5);
    }

    int snap_rc = 0;
    if (opts.snapshot_path) {
        /* One more handler tick so the final frame is presented. */
        lv_timer_handler();
        snap_rc = save_window_snapshot(disp, opts.snapshot_path);
    }
    printf("Aurora simulator exit\n");
    return snap_rc;
}
