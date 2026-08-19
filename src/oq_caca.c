/* libcaca presentation backend.
 *
 * We deliberately do NOT use caca_create_display(): its ncurses/slang
 * drivers take over the tty, which would fight our own raw-mode and
 * keyboard-protocol handling in oq_term.c.  Instead we dither into a
 * detached canvas and export it as an ANSI/UTF-8 string that we write
 * ourselves -- the same shape as the chafa backend.
 */
#include "oq_present.h"

#include <caca.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static caca_canvas_t *canvas;
static caca_dither_t *dither;
static int dither_w, dither_h, dither_stride;
static oq_present_config conf;

static void free_dither(void)
{
    if (dither) {
        caca_free_dither(dither);
        dither = NULL;
    }
    dither_w = dither_h = dither_stride = 0;
}

static const char *charset_for(oq_symbols s)
{
    switch (s) {
    case OQ_SYMBOLS_ASCII: return "ascii";
    case OQ_SYMBOLS_BLOCK: return "shades";
    /* libcaca has no sub-cell glyphs; "blocks" is as fine as it gets. */
    case OQ_SYMBOLS_FINE:  return "blocks";
    }
    return "ascii";
}

/* libcaca caps out at 16 ANSI colours in a terminal regardless of what the
 * terminal can do -- that is its defining limitation next to chafa. */
static const char *color_for(oq_color c)
{
    switch (c) {
    case OQ_COLOR_MONO: return "mono";
    case OQ_COLOR_16:   return "16";
    case OQ_COLOR_256:
    case OQ_COLOR_TRUE: return "full16";
    }
    return "full16";
}

static int caca_init(const oq_present_config *cfg)
{
    conf = *cfg;
    canvas = caca_create_canvas(cfg->cols, cfg->rows);
    if (!canvas)
        return -1;
    return 0;
}

static void caca_shutdown(void)
{
    free_dither();
    if (canvas) {
        caca_free_canvas(canvas);
        canvas = NULL;
    }
}

static int caca_resize(const oq_present_config *cfg)
{
    conf = *cfg;
    if (!canvas)
        return -1;
    return caca_set_canvas_size(canvas, cfg->cols, cfg->rows);
}

static void caca_frame(const uint8_t *src, int w, int h, int stride)
{
    char *out;
    size_t len;

    if (!canvas)
        return;

    if (!dither || w != dither_w || h != dither_h || stride != dither_stride) {
        free_dither();
        /* libcaca reads bpp/8 bytes little-endian into a word, so for the
         * in-memory byte order R,G,B the word is B<<16 | G<<8 | R -- hence
         * the red and blue masks look swapped compared to an ARGB literal. */
        dither = caca_create_dither(24, w, h, stride,
                                    0x0000ff, 0x00ff00, 0xff0000, 0x000000);
        if (!dither)
            return;
        dither_w = w;
        dither_h = h;
        dither_stride = stride;
        caca_set_dither_charset(dither, charset_for(conf.symbols));
        caca_set_dither_color(dither, color_for(conf.color));
        caca_set_dither_algorithm(dither, "fstein");
        caca_set_dither_antialias(dither, "prefilter");
    }

    caca_dither_bitmap(canvas, 0, 0,
                       caca_get_canvas_width(canvas),
                       caca_get_canvas_height(canvas),
                       dither, src);

    out = caca_export_canvas_to_memory(canvas, "utf8", &len);
    if (!out)
        return;
    /* Trim the trailing newline: emitting it on the bottom row scrolls the
     * screen and the image walks upward one line per frame. */
    while (len && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        len--;

    fputs("\033[H", stdout);
    fwrite(out, 1, len, stdout);
    fflush(stdout);
    free(out);
}

const oq_present_backend oq_backend_caca = {
    "caca", caca_init, caca_shutdown, caca_resize, caca_frame
};
