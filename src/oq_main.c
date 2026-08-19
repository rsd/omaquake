/* OmaQuake -- Quake rendered as characters in a terminal. */
#include "oq_evdev.h"
#include "oq_input.h"
#include "oq_mouse.h"
#include "oq_present.h"
#include "oq_render.h"
#include "oq_retro.h"
#include "oq_term.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEMO_W 320
#define DEMO_H 200

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options] [id1/pak0.pak]\n"
        "\n"
        "  --video=NAME     presentation backend: %s (default: chafa)\n"
        "  --symbols=SET    ascii | block | fine        (default: fine)\n"
        "  --color=DEPTH    mono | 16 | 256 | true      (default: true)\n"
        "  --demo           render a test pattern instead of the game\n"
        "  --keytest        show decoded key events; diagnoses input problems\n"
        "  --frames=N       stop after N frames (demo/benchmark)\n"
        "  --cell=WxH       character cell pixel size (default 10x20)\n"
        "  --res=WxH        engine render resolution, or auto to match the\n"
        "                   terminal (default auto)\n"
        "  --fps=N          presentation rate cap, 0 = every frame (default 30)\n"
        "  --cells=WxH      cap the canvas (default 0x0: fill the terminal)\n"
        "  --log=PATH       write the engine log here (never to stdout)\n"
        "  --no-sound       do not open the audio device\n"
        "  --mouse=SRC      evdev | term | none | auto  (default: auto --\n"
        "                   evdev when a pointer can be opened and grabbed,\n"
        "                   otherwise the terminal's own reporting)\n"
        "  --mouse-dev=PATH use this evdev node instead of auto-detecting\n"
        "  --mouse-list     list /dev/input/event* and why each was taken or\n"
        "                   rejected, then exit\n"
        "  --no-mouse       do not take over the pointer (same as --mouse=none)\n"
        "  --mouse-sens=F   pointer sensitivity (default 2.0)\n"
        "  --mouse-edge=F   steering band, fraction of each side (default 0.15,\n"
        "                   0 turns steering off and leaves plain 1:1 look).\n"
        "                   Terminal source only: evdev reports unbounded\n"
        "                   deltas, which have no edge to steer away from\n"
        "  --mouse-turn=F   steering rate at the edge, deg/sec (default 220);\n"
        "                   terminal source only, as above\n"
        "  --mouse-invert   invert pitch\n"
        "  --help\n",
        argv0, oq_present_available());
}

static int parse_symbols(const char *s, oq_symbols *out)
{
    if (!strcmp(s, "ascii")) { *out = OQ_SYMBOLS_ASCII; return 0; }
    if (!strcmp(s, "block")) { *out = OQ_SYMBOLS_BLOCK; return 0; }
    if (!strcmp(s, "fine"))  { *out = OQ_SYMBOLS_FINE;  return 0; }
    return -1;
}

static int parse_color(const char *s, oq_color *out)
{
    if (!strcmp(s, "mono")) { *out = OQ_COLOR_MONO; return 0; }
    if (!strcmp(s, "16"))   { *out = OQ_COLOR_16;   return 0; }
    if (!strcmp(s, "256"))  { *out = OQ_COLOR_256;  return 0; }
    if (!strcmp(s, "true")) { *out = OQ_COLOR_TRUE; return 0; }
    return -1;
}

/* Where pointer motion comes from.  auto prefers evdev and falls back to
 * the terminal, which is the only source that exists on a machine without
 * readable /dev/input nodes. */
enum { MOUSE_AUTO, MOUSE_EVDEV, MOUSE_TERM, MOUSE_NONE };

static int parse_mouse(const char *s, int *out)
{
    if (!strcmp(s, "auto"))  { *out = MOUSE_AUTO;  return 0; }
    if (!strcmp(s, "evdev")) { *out = MOUSE_EVDEV; return 0; }
    if (!strcmp(s, "term"))  { *out = MOUSE_TERM;  return 0; }
    if (!strcmp(s, "none"))  { *out = MOUSE_NONE;  return 0; }
    return -1;
}

/* Colour bars over a sweeping gradient.  Deliberately includes saturated
 * primaries so a channel-order mistake in a backend is obvious on sight. */
static void demo_fill(uint8_t *buf, int w, int h, int frame)
{
    static const uint8_t bars[8][3] = {
        {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
        {255,0,255},   {255,0,0},   {0,0,255},   {0,0,0},
    };
    int x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t *p = buf + (size_t)y * w * 3 + (size_t)x * 3;

            if (y < h * 2 / 3) {
                memcpy(p, bars[x * 8 / w], 3);
            } else {
                int v = (x * 255 / w + frame * 3) & 0xff;
                p[0] = (uint8_t)v;
                p[1] = (uint8_t)(255 - v);
                p[2] = (uint8_t)((y * 255 / h) & 0xff);
            }
        }
    }
}

static int run_demo(const oq_present_backend *be, oq_present_config *cfg,
                    int nframes)
{
    uint8_t *buf = malloc((size_t)DEMO_W * DEMO_H * 3);
    int frame;

    if (!buf)
        return 1;

    for (frame = 0; frame < nframes && !oq_term_quit_requested; frame++) {
        struct timespec ts = { 0, 33 * 1000 * 1000 };
        int cols, rows;

        if (oq_term_size(&cols, &rows)) {
            cfg->cols = cols;
            cfg->rows = rows;
            be->resize(cfg);
        }
        demo_fill(buf, DEMO_W, DEMO_H, frame);
        be->frame(buf, DEMO_W, DEMO_H, DEMO_W * 3);
        nanosleep(&ts, NULL);
    }

    free(buf);
    return 0;
}



/* ---- pointer ---------------------------------------------------------- */

/* Which source is live.  Decided before the terminal is touched and then
 * confirmed when the grab is taken, because either step can fail. */
enum { PSRC_NONE, PSRC_TERM, PSRC_EVDEV };
static int mouse_released;
static int pointer_src = PSRC_NONE;
/* The user asked for evdev by name, or named a device: failing over to the
 * terminal would then be a silent substitution of something they did not
 * ask for, so it is an error instead. */
static int pointer_required;
/* Reported to --log and, once the terminal is back, to stderr.  Neither
 * channel is available at the moment the choice is made: stdout is the
 * picture and the log file is not opened until the core starts. */
static char pointer_note[512];

/* Pixels per character cell, so the size of the text area can be recomputed
 * after a resize without asking the terminal again -- that would be a write
 * to the terminal from the wrong thread once the game is running. */
static int cell_px_w = 10, cell_px_h = 20;

static void mouse_calibrate(const oq_present_config *cfg, int cols, int rows)
{
    int pw, ph;

    if (oq_term_text_pixels(&pw, &ph) == 0 && cols > 0 && rows > 0) {
        cell_px_w = pw / cols;
        cell_px_h = ph / rows;
    }
    /* Either the terminal did not answer CSI 14 t, or it answered with an
     * area smaller than one cell per row.  Fall back to --cell, which is
     * the player's own estimate of their cell size. */
    if (cell_px_w < 1) cell_px_w = cfg->cell_w > 0 ? cfg->cell_w : 10;
    if (cell_px_h < 1) cell_px_h = cfg->cell_h > 0 ? cfg->cell_h : 20;

    oq_mouse_set_extent(cols * cell_px_w, rows * cell_px_h);
}

/* Pick the source.  Deliberately runs before oq_term_init(): opening an
 * evdev node can fail, and that message belongs on a plain stderr rather
 * than inside the alternate screen.  Nothing is grabbed here -- --demo and
 * --keytest without a mouse must not take the pointer at all. */
static int pointer_select(int mode, const char *dev)
{
    /* Said out loud rather than dropped: --mouse-dev only means anything to
     * the evdev source, and an option that looks accepted and does nothing
     * is the failure mode this codebase keeps running into. */
    if (dev && (mode == MOUSE_TERM || mode == MOUSE_NONE))
        fprintf(stderr, "omaquake: --mouse-dev names an evdev device and is"
                        " ignored by --mouse=%s\n",
                mode == MOUSE_TERM ? "term" : "none");

    if (mode == MOUSE_NONE) {
        pointer_src = PSRC_NONE;
        return 0;
    }
    if (mode != MOUSE_TERM && oq_evdev_open(dev) == 0) {
        pointer_src = PSRC_EVDEV;
        return 0;
    }
    if (mode != MOUSE_TERM && pointer_required) {
        fprintf(stderr, "omaquake: mouse: %s\n", oq_evdev_error());
        return -1;
    }
    pointer_src = PSRC_TERM;
    return 0;
}

/* Take the pointer for the duration of the loop: grab the device, or turn
 * on the terminal's own reporting.  Returns -1 only when evdev was asked
 * for by name and could not be grabbed; under --mouse=auto a failed grab
 * quietly becomes the terminal source, which is the whole point of auto.
 *
 * Everything here writes to the terminal or to the log, so it must run
 * before the render thread starts and takes stdout over. */
static int pointer_start(const oq_mouse_config *mc, oq_present_config *cfg,
                         int cols, int rows, int edge_given, int turn_given)
{
    oq_mouse_config c = *mc;

    if (pointer_src == PSRC_EVDEV && oq_evdev_grab()) {
        if (pointer_required) {
            snprintf(pointer_note, sizeof(pointer_note),
                     "could not grab the pointer: %s", oq_evdev_error());
            return -1;
        }
        oq_evdev_close();
        pointer_src = PSRC_TERM;
    }
    /* Unbounded deltas have no window edge to wall out at, so the steering
     * band -- the entire reason the terminal path is a hybrid -- is off. */
    c.relative = (pointer_src == PSRC_EVDEV);
    oq_mouse_init(&c);
    mouse_calibrate(cfg, cols, rows);

    if (pointer_src == PSRC_EVDEV) {
        /* Focus reporting only.  Having the terminal track and encode the
         * same pointer the kernel is already handing us would be work on
         * both ends for nothing; focus we still want, so that motion
         * arriving while the player is in another window is discarded
         * rather than turning the view. */
        oq_term_focus_enable();
        snprintf(pointer_note, sizeof(pointer_note),
                 "evdev %s \"%s\", grabbed, 1:1 (no edge band)%s",
                 oq_evdev_path(), oq_evdev_name(),
                 (edge_given || turn_given)
                     ? "; --mouse-edge/--mouse-turn do not apply to a"
                       " relative source and are ignored" : "");
    } else if (pointer_src == PSRC_TERM) {
        oq_term_mouse_enable();
        snprintf(pointer_note, sizeof(pointer_note),
                 "terminal SGR-pixels (modes 1003/1016),"
                 " edge band %.2f at %.0f deg/s", c.edge, c.turn);
    }
    return 0;
}

/* Buttons from the kernel take the same path as the terminal's: same
 * OQ_MB_* numbering, so a bind in autoexec.cfg works either way. */
static void evdev_button_sink(int button, int down, void *ud)
{
    (void)ud;
    oq_retro_mouse_button(button, down);
}

/* Motion drives the view; buttons go through as MOUSE1..MOUSE7 so they can
 * be bound in autoexec.cfg.  There is deliberately no "hold to look"
 * modifier: mode 1003 reports motion with nothing held, and a first-person
 * game wants the view on the pointer at all times -- reserving a button for
 * engagement would cost the one that should be firing. */
static void mouse_sink(const oq_mouse_event *ev, void *ud)
{
    (void)ud;
    oq_mouse_track(ev->x, ev->y);
    /* A drag repeats the held button on every motion report; only the
     * transitions are button events. */
    if (ev->button != OQ_MB_NONE && !ev->motion)
        oq_retro_mouse_button(ev->button, ev->down);
}

/* Per-frame pointer work, shared by the game loop and --keytest. */
static void mouse_frame(int *dx, int *dy)
{
    static int had_focus = 1;
    int focus = oq_input_focused();

    if (oq_input_take_mouse_toggle()) {
        mouse_released = !mouse_released;
        if (pointer_src == PSRC_EVDEV) {
            if (mouse_released)
                oq_evdev_ungrab();
            else
                oq_evdev_grab();
        }
    }

    if (pointer_src == PSRC_EVDEV) {
        int rx, ry;

        /* Hand the pointer back when the player alt-tabs away. The grab is
         * exclusive, so holding it in the background leaves the whole desktop
         * without a mouse. */
        if (focus != had_focus) {
            if (focus && !mouse_released)
                oq_evdev_grab();
            else
                oq_evdev_ungrab();
        }

        /* Drained even while in the background, so nothing queues up to
         * arrive as one lurch on the way back -- but discarded there: the
         * device is grabbed, so those events are still ours while the
         * player is in another window and must not aim or fire. */
        oq_evdev_poll(&rx, &ry, focus ? evdev_button_sink : NULL, NULL);
        if (focus && !mouse_released)
            oq_mouse_move(rx, ry);
    }
    if (focus != had_focus) {
        had_focus = focus;
        /* A button still held when the terminal loses focus never gets its
         * release: reporting stops while we are in the background, so the
         * engine would go on firing for as long as the player is away. */
        if (!focus) {
            oq_retro_mouse_button(OQ_MB_LEFT, 0);
            oq_retro_mouse_button(OQ_MB_MIDDLE, 0);
            oq_retro_mouse_button(OQ_MB_RIGHT, 0);
        }
    }
    oq_mouse_set_active(focus);
    oq_mouse_step(dx, dy);
}

/* Prints what the terminal actually sends and what we decode it to.  When a
 * key does not work in game, this says whether the bytes never arrived, or
 * arrived and were decoded wrong. */
static void keytest_sink(unsigned keycode, int down, uint16_t mods, void *ud)
{
    (void)keycode; (void)down; (void)mods; (void)ud;
}

static int run_keytest(oq_present_config *cfg, const oq_mouse_config *mc,
                       int edge_given, int turn_given)
{
    char hdr[768];
    int cols, rows, pw = 0, ph = 0;
    int n;

    oq_input_init(oq_term_has_key_release());
    oq_input_set_trace(stdout);
    oq_term_size(&cols, &rows);

    if (mc) {
        if (pointer_start(mc, cfg, cols, rows, edge_given, turn_given))
            return 1;           /* main prints pointer_note after restoring */
        pw = cols * cell_px_w;
        ph = rows * cell_px_h;
    }

    n = snprintf(hdr, sizeof(hdr),
                 "key-release: %s\r\n"
                 "pointer:     %s\r\n"
                 "text area:   %dx%d px, %dx%d per cell\r\n"
                 "press keys (arrows, Enter, W, Ctrl) or move the pointer;"
                 " Ctrl-Q, Ctrl-\\ or F10 quits\r\n\r\n",
                 oq_term_has_key_release()
                     ? "YES - kitty keyboard protocol active"
                     : "NO - press-only fallback, holds are synthesised",
                 mc ? pointer_note : "off (--mouse=none)", pw, ph,
                 cell_px_w, cell_px_h);
    fwrite(hdr, 1, (size_t)n, stdout);
    fflush(stdout);

    while (!oq_term_quit_requested && !oq_input_quit()) {
        struct timespec ts = { 0, 20 * 1000 * 1000 };

        oq_input_poll(keytest_sink,
                      pointer_src == PSRC_TERM ? mouse_sink : NULL, NULL);
        oq_input_expire(keytest_sink, NULL);
        if (mc) {
            int dx, dy, mx, my;
            double ex, ey;

            mouse_frame(&dx, &dy);
            oq_mouse_debug(&mx, &my, &ex, &ey);
            if (dx || dy)
                printf("     look dx=%+-6d dy=%+-6d at %d,%d"
                       " steer=%+.2f,%+.2f\r\n", dx, dy, mx, my, ex, ey);
        }
        fflush(stdout);
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* ---- game loop ------------------------------------------------------ */

struct sink_ctx {
    const oq_present_backend *be;
    oq_present_config *cfg;
    int      mouse;                /* pointer look is on */
    int      cap_cols, cap_rows;   /* 0 = fill the terminal */
    int64_t  period_ns;            /* minimum gap between presents */
    int64_t  next_present;
};

/* Work out the canvas size and where to put it.
 *
 * Filling a large terminal from a 320x200 source is pure waste: with
 * sub-cell glyphs one cell already resolves 2x4 pixels, so beyond
 * res_w/2 by res_h/4 cells chafa is upscaling detail that does not
 * exist -- at several times the conversion cost and, worse, several
 * times the escape-sequence volume the terminal has to parse. */
static void fit_canvas(struct sink_ctx *ctx, int term_cols, int term_rows)
{
    oq_present_config *cfg = ctx->cfg;
    int cols = term_cols, rows = term_rows;

    if (ctx->cap_cols > 0 && cols > ctx->cap_cols)
        cols = ctx->cap_cols;
    if (ctx->cap_rows > 0 && rows > ctx->cap_rows)
        rows = ctx->cap_rows;

    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    cfg->cols = cols;
    cfg->rows = rows;
    cfg->origin_col = 1 + (term_cols - cols) / 2;
    cfg->origin_row = 1 + (term_rows - rows) / 2;
}

static int64_t now_ns(void);

/* Presenting is far more expensive than simulating. Throttling presentation
 * rather than the whole loop keeps retro_run at the engine's own rate, which
 * is what the audio stream is generated from -- pace the loop instead and the
 * core produces fewer samples per second than the sound card consumes, and
 * the sound breaks up. */
static int want_frame(void *ud)
{
    struct sink_ctx *ctx = ud;
    int64_t now = now_ns();

    if (!ctx->period_ns)
        return 1;
    if (now < ctx->next_present)
        return 0;
    ctx->next_present = (now > ctx->next_present + ctx->period_ns)
                        ? now + ctx->period_ns
                        : ctx->next_present + ctx->period_ns;
    return 1;
}

static void video_sink(const uint8_t *rgb, int w, int h, int stride, void *ud)
{
    struct sink_ctx *ctx = ud;
    int cols, rows;

    if (oq_term_size(&cols, &rows)) {
        fit_canvas(ctx, cols, rows);
        /* The bands are placed against the text area, so a resize moves
         * them.  Scaling the cached cell size is deliberate: re-querying
         * means writing to the terminal, and stdout belongs to the render
         * thread from here on. */
        if (ctx->mouse)
            oq_mouse_set_extent(cols * cell_px_w, rows * cell_px_h);
        /* The clear happens on the render thread: two threads writing to the
         * same fd can interleave mid escape-sequence and corrupt the frame. */
        oq_render_reconfigure(ctx->cfg);
    }
    oq_render_submit(rgb, w, h, stride);
}

static void key_sink(unsigned keycode, int down, uint16_t mods, void *ud)
{
    (void)ud;
    oq_retro_key(keycode, down, mods);
}

static int64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int run_game(const oq_present_backend *be, oq_present_config *cfg,
                    const char *pak, const char *res, const char *logpath,
                    int nframes, int sound, int fps, int cap_cols, int cap_rows,
                    const oq_mouse_config *mc, int edge_given, int turn_given)
{
    struct sink_ctx ctx;
    oq_retro_config rc;
    char resbuf[32];
    int64_t period, next;
    int frame = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.be = be;
    ctx.cfg = cfg;
    ctx.period_ns = fps > 0 ? (int64_t)(1000000000.0 / fps) : 0;

    /* Fill the terminal by default. Capping only makes sense when the engine
     * resolution is pinned below what the cell grid can show, which --res=auto
     * avoids; --cells is there for when someone wants it anyway. */
    if (cap_cols < 0 || cap_rows < 0)
        cap_cols = cap_rows = 0;
    ctx.cap_cols = cap_cols;
    ctx.cap_rows = cap_rows;

    {
        int tc, tr;

        oq_term_size(&tc, &tr);
        fit_canvas(&ctx, tc, tr);
        oq_term_clear();
        be->resize(cfg);
        /* This writes to the terminal, so it has to happen before the
         * render thread starts and takes stdout over. */
        if (mc && pointer_start(mc, cfg, tc, tr, edge_given, turn_given))
            return 1;           /* main prints pointer_note after restoring */
    }
    /* After pointer_start, which is where a failed grab downgrades evdev to
     * the terminal.  Only the terminal source cares about the text area:
     * it is the space its reports and steering bands live in, whereas
     * evdev counts are not in any window's coordinates at all. */
    ctx.mouse = mc != NULL && pointer_src == PSRC_TERM;

    if (oq_render_start(be, cfg)) {
        oq_term_shutdown();
        fprintf(stderr, "omaquake: could not start the render thread\n");
        return 1;
    }

    /* --res=auto: render at exactly the detail the cell grid can display.
     * One cell resolves 2x4 pixels with sub-cell glyphs, so this is the point
     * where the engine is neither wasting work the terminal cannot show nor
     * making chafa upscale detail that was never rendered. Raising the engine
     * resolution turns out to be nearly free -- 1920x1200 still holds the
     * 60fps target -- because presentation happens on another thread. */
    if (!strcmp(res, "auto")) {
        int tc, tr, w, h;

        oq_term_size(&tc, &tr);
        w = tc * 2;
        h = tr * 4;
        if (w < 320)  w = 320;
        if (h < 200)  h = 200;
        if (w > 1920) w = 1920;
        if (h > 1200) h = 1200;
        snprintf(resbuf, sizeof(resbuf), "%dx%d", w, h);
        res = resbuf;
    }

    memset(&rc, 0, sizeof(rc));
    rc.resolution = res;
    rc.framerate = "auto";
    rc.samplerate = "auto";
    rc.save_dir = ".";
    rc.log_path = logpath;
    rc.sound = sound;
    rc.sound = sound;
    rc.want_frame = want_frame;
    rc.sink = video_sink;
    rc.sink_ud = &ctx;

    if (oq_retro_init(&rc, pak)) {
        oq_term_shutdown();
        fprintf(stderr, "omaquake: failed to load '%s'\n", pak);
        return 1;
    }

    oq_input_init(oq_term_has_key_release());
    /* The log is the only place this can go now: stdout is the picture and
     * stderr shares the terminal with it.  It opens with the core, which is
     * why the choice is reported here rather than where it was made. */
    if (mc)
        oq_retro_log("mouse: %s\n", pointer_note);
    period = (int64_t)(1000000000.0 / oq_retro_fps());
    next = now_ns() + period;

    while (!oq_term_quit_requested && !oq_input_quit()) {
        int64_t remain;

        oq_input_poll(key_sink,
                      pointer_src == PSRC_TERM ? mouse_sink : NULL, NULL);
        oq_input_expire(key_sink, NULL);
        if (mc) {
            int dx, dy;

            mouse_frame(&dx, &dy);
            if (dx || dy)
                oq_retro_mouse_move(dx, dy);
        }
        oq_retro_run();

        if (nframes && ++frame >= nframes)
            break;

        remain = next - now_ns();
        if (remain > 0) {
            struct timespec ts = { remain / 1000000000,
                                   remain % 1000000000 };
            nanosleep(&ts, NULL);
        } else {
            next = now_ns();   /* we fell behind; do not spiral */
        }
        next += period;
    }

    oq_render_stop();
    /* Give the pointer back before the slow parts of shutdown: the grab is
     * the user's whole desktop mouse, and unloading the core is no reason
     * to keep holding it. */
    oq_evdev_close();
    oq_term_mouse_disable();
    oq_retro_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    const char *video = "chafa";
    const char *game = NULL;
    const char *res = "auto";
    const char *logpath = NULL;
    const oq_present_backend *be;
    oq_present_config cfg;
    oq_mouse_config mc;
    const char *mouse_dev = NULL;
    int demo = 0, keytest = 0, nframes = 0, sound = 1, i, rc;
    int fps = 30, cap_cols = -1, cap_rows = -1;
    int mouse_mode = MOUSE_AUTO, edge_given = 0, turn_given = 0;

    /* 2.0 counts per pixel is 0.13 degrees per pixel with the engine's stock
     * cvars: about a quarter turn across the middle of a 1000px window,
     * which is close to how a real mouse on a desk feels. */
    mc.sens = 2.0;
    mc.edge = 0.15;
    mc.turn = 220.0;
    mc.invert = 0;
    mc.relative = 0;

    cfg.symbols = OQ_SYMBOLS_FINE;
    cfg.color = OQ_COLOR_TRUE;
    /* Typical terminal cell proportions; --cell= overrides. */
    cfg.cell_w = 10;
    cfg.cell_h = 20;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strncmp(a, "--video=", 8)) {
            video = a + 8;
        } else if (!strncmp(a, "--symbols=", 10)) {
            if (parse_symbols(a + 10, &cfg.symbols)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strncmp(a, "--color=", 8)) {
            if (parse_color(a + 8, &cfg.color)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strncmp(a, "--fps=", 6)) {
            fps = atoi(a + 6);
        } else if (!strncmp(a, "--cells=", 8)) {
            if (sscanf(a + 8, "%dx%d", &cap_cols, &cap_rows) != 2) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strncmp(a, "--res=", 6)) {
            res = a + 6;
        } else if (!strncmp(a, "--log=", 6)) {
            logpath = a + 6;
        } else if (!strncmp(a, "--cell=", 7)) {
            if (sscanf(a + 7, "%dx%d", &cfg.cell_w, &cfg.cell_h) != 2) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strncmp(a, "--frames=", 9)) {
            nframes = atoi(a + 9);
        } else if (!strncmp(a, "--mouse-sens=", 13)) {
            mc.sens = atof(a + 13);
        } else if (!strncmp(a, "--mouse-edge=", 13)) {
            mc.edge = atof(a + 13);
            edge_given = 1;
        } else if (!strncmp(a, "--mouse-turn=", 13)) {
            mc.turn = atof(a + 13);
            turn_given = 1;
        } else if (!strcmp(a, "--mouse-invert")) {
            mc.invert = 1;
        } else if (!strncmp(a, "--mouse-dev=", 12)) {
            mouse_dev = a + 12;
        } else if (!strcmp(a, "--mouse-list")) {
            /* Before any terminal setup, so this is a plain program writing
             * to a plain stdout -- the "stdout is the picture" rule binds
             * only once the alternate screen is up. */
            oq_evdev_list(stdout);
            return 0;
        } else if (!strncmp(a, "--mouse=", 8)) {
            if (parse_mouse(a + 8, &mouse_mode)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!strcmp(a, "--no-mouse")) {
            mouse_mode = MOUSE_NONE;    /* the older spelling of --mouse=none */
        } else if (!strcmp(a, "--no-sound")) {
            sound = 0;
        } else if (!strcmp(a, "--keytest")) {
            keytest = 1;
        } else if (!strcmp(a, "--demo")) {
            demo = 1;
        } else if (!strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else if (a[0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            game = a;
        }
    }

    be = oq_present_lookup(video);
    if (!be) {
        fprintf(stderr, "omaquake: no such video backend '%s' (have: %s)\n",
                video, oq_present_available());
        return 2;
    }
    if (!demo && !keytest && !game) {
        usage(argv[0]);
        return 2;
    }

    /* A device named by hand, or evdev asked for by name, must not silently
     * fall back to the terminal: the whole class of bug this project keeps
     * hitting is an option that looks accepted and is quietly dropped. */
    pointer_required = (mouse_mode == MOUSE_EVDEV) ||
                       (mouse_dev != NULL && mouse_mode != MOUSE_NONE &&
                        mouse_mode != MOUSE_TERM);
    if (demo)
        mouse_mode = MOUSE_NONE;        /* a test pattern has no view to aim */
    if (pointer_select(mouse_mode, mouse_dev))
        return 2;

    if (oq_term_init())
        return 1;

    if (keytest) {
        rc = run_keytest(&cfg, pointer_src != PSRC_NONE ? &mc : NULL,
                         edge_given, turn_given);
        oq_term_shutdown();
        if (pointer_note[0])
            fprintf(stderr, "omaquake: mouse: %s\n", pointer_note);
        return rc;
    }

    oq_term_size(&cfg.cols, &cfg.rows);

    if (be->init(&cfg)) {
        oq_term_shutdown();
        fprintf(stderr, "omaquake: backend '%s' failed to initialise\n", video);
        return 1;
    }

    if (demo) {
        rc = run_demo(be, &cfg, nframes ? nframes : 600);
    } else {
        rc = run_game(be, &cfg, game, res, logpath, nframes, sound,
                      fps, cap_cols, cap_rows,
                      pointer_src != PSRC_NONE ? &mc : NULL,
                      edge_given, turn_given);
    }

    be->shutdown();
    oq_term_shutdown();

    fprintf(stderr, "omaquake: backend=%s key-release=%s\n",
            video, oq_term_has_key_release() ? "yes (kitty kbd)" : "no");
    /* Repeated on stderr because the log file is optional, and "which
     * pointer source did I actually get" is the first question when aiming
     * misbehaves. */
    if (pointer_note[0])
        fprintf(stderr, "omaquake: mouse: %s\n", pointer_note);
    return rc;
}
