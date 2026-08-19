/* OmaQuake -- Quake rendered as characters in a terminal. */
#include "oq_input.h"
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
        "  --res=WxH        engine render resolution (default 320x200)\n"
        "  --fps=N          presentation rate cap, 0 = every frame (default 30)\n"
        "  --cells=WxH      cap the canvas; 0x0 fills the terminal\n"
        "  --log=PATH       write the engine log here (never to stdout)\n"
        "  --no-sound       do not open the audio device\n"
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



/* Prints what the terminal actually sends and what we decode it to.  When a
 * key does not work in game, this says whether the bytes never arrived, or
 * arrived and were decoded wrong. */
static void keytest_sink(unsigned keycode, int down, uint16_t mods, void *ud)
{
    (void)keycode; (void)down; (void)mods; (void)ud;
}

static int run_keytest(void)
{
    char hdr[256];
    int n;

    oq_input_init(oq_term_has_key_release());
    oq_input_set_trace(stdout);

    n = snprintf(hdr, sizeof(hdr),
                 "key-release: %s\r\n"
                 "press keys (arrows, Enter, W, Ctrl); Ctrl-Q, Ctrl-\\ or F10 quits\r\n\r\n",
                 oq_term_has_key_release()
                     ? "YES - kitty keyboard protocol active"
                     : "NO - press-only fallback, holds are synthesised");
    fwrite(hdr, 1, (size_t)n, stdout);
    fflush(stdout);

    while (!oq_term_quit_requested && !oq_input_quit()) {
        struct timespec ts = { 0, 20 * 1000 * 1000 };

        oq_input_poll(keytest_sink, NULL);
        oq_input_expire(keytest_sink, NULL);
        fflush(stdout);
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* ---- game loop ------------------------------------------------------ */

struct sink_ctx {
    const oq_present_backend *be;
    oq_present_config *cfg;
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
                    int nframes, int sound, int fps, int cap_cols, int cap_rows)
{
    struct sink_ctx ctx;
    oq_retro_config rc;
    int rw = 0, rh = 0;
    int64_t period, next;
    int frame = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.be = be;
    ctx.cfg = cfg;
    ctx.period_ns = fps > 0 ? (int64_t)(1000000000.0 / fps) : 0;

    /* Default the cap to the source's own detail limit: one cell resolves
     * 2x4 pixels with sub-cell glyphs. */
    if (cap_cols < 0 || cap_rows < 0) {
        if (sscanf(res, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
            cap_cols = rw / 2;
            cap_rows = rh / 4;
        } else {
            cap_cols = cap_rows = 0;
        }
    }
    ctx.cap_cols = cap_cols;
    ctx.cap_rows = cap_rows;

    {
        int tc, tr;

        oq_term_size(&tc, &tr);
        fit_canvas(&ctx, tc, tr);
        oq_term_clear();
        be->resize(cfg);
    }

    if (oq_render_start(be, cfg)) {
        oq_term_shutdown();
        fprintf(stderr, "omaquake: could not start the render thread\n");
        return 1;
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
    period = (int64_t)(1000000000.0 / oq_retro_fps());
    next = now_ns() + period;

    while (!oq_term_quit_requested && !oq_input_quit()) {
        int64_t remain;

        oq_input_poll(key_sink, NULL);
        oq_input_expire(key_sink, NULL);
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
    oq_retro_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    const char *video = "chafa";
    const char *game = NULL;
    const char *res = "320x200";
    const char *logpath = NULL;
    const oq_present_backend *be;
    oq_present_config cfg;
    int demo = 0, keytest = 0, nframes = 0, sound = 1, i, rc;
    int fps = 30, cap_cols = -1, cap_rows = -1;

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

    if (oq_term_init())
        return 1;

    if (keytest) {
        rc = run_keytest();
        oq_term_shutdown();
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
                      fps, cap_cols, cap_rows);
    }

    be->shutdown();
    oq_term_shutdown();

    fprintf(stderr, "omaquake: backend=%s key-release=%s\n",
            video, oq_term_has_key_release() ? "yes (kitty kbd)" : "no");
    return rc;
}
