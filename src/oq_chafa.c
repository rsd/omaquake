/* chafa presentation backend.
 *
 * chafa exposes the two dials independently: the symbol map picks the glyph
 * repertoire, the canvas mode picks the colour depth.  That is why we can
 * have a pure-ASCII picture in 24-bit colour, which libcaca cannot do.
 */
#include "oq_present.h"

#include <chafa.h>
#include <stdio.h>
#include <string.h>

static ChafaCanvas *canvas;
static ChafaTermInfo *term_info;
static oq_present_config conf;

static ChafaSymbolTags tags_for(oq_symbols s)
{
    switch (s) {
    case OQ_SYMBOLS_ASCII:
        return CHAFA_SYMBOL_TAG_ASCII | CHAFA_SYMBOL_TAG_SPACE;
    case OQ_SYMBOLS_BLOCK:
        return CHAFA_SYMBOL_TAG_BLOCK | CHAFA_SYMBOL_TAG_SPACE;
    case OQ_SYMBOLS_FINE:
        /* Sub-cell glyphs: 2x4 effective resolution per character cell.
         * Octants need chafa 1.16; older builds stop at sextants. */
        return CHAFA_SYMBOL_TAG_BLOCK | CHAFA_SYMBOL_TAG_SPACE
             | CHAFA_SYMBOL_TAG_HALF | CHAFA_SYMBOL_TAG_QUAD
#if CHAFA_CHECK_VERSION(1, 16, 0)
             | CHAFA_SYMBOL_TAG_OCTANT
#endif
             | CHAFA_SYMBOL_TAG_SEXTANT;
    }
    return CHAFA_SYMBOL_TAG_ASCII;
}

static ChafaCanvasMode mode_for(oq_color c)
{
    switch (c) {
    case OQ_COLOR_MONO: return CHAFA_CANVAS_MODE_FGBG;
    case OQ_COLOR_16:   return CHAFA_CANVAS_MODE_INDEXED_16;
    case OQ_COLOR_256:  return CHAFA_CANVAS_MODE_INDEXED_240;
    case OQ_COLOR_TRUE: return CHAFA_CANVAS_MODE_TRUECOLOR;
    }
    return CHAFA_CANVAS_MODE_TRUECOLOR;
}

static int build_canvas(const oq_present_config *cfg)
{
    ChafaSymbolMap *map;
    ChafaCanvasConfig *cc;

    if (canvas) {
        chafa_canvas_unref(canvas);
        canvas = NULL;
    }

    map = chafa_symbol_map_new();
    chafa_symbol_map_add_by_tags(map, tags_for(cfg->symbols));

    cc = chafa_canvas_config_new();
    chafa_canvas_config_set_geometry(cc, cfg->cols, cfg->rows);
    chafa_canvas_config_set_canvas_mode(cc, mode_for(cfg->color));
    chafa_canvas_config_set_pixel_mode(cc, CHAFA_PIXEL_MODE_SYMBOLS);
    chafa_canvas_config_set_symbol_map(cc, map);
    /* The frame is already the right aspect for the cell grid; stretching
     * keeps the engine's own 4:3 handling authoritative. */
    chafa_canvas_config_set_horiz_align(cc, CHAFA_ALIGN_CENTER);
    chafa_canvas_config_set_vert_align(cc, CHAFA_ALIGN_CENTER);

    canvas = chafa_canvas_new(cc);

    chafa_canvas_config_unref(cc);
    chafa_symbol_map_unref(map);
    conf = *cfg;
    return canvas ? 0 : -1;
}

static int chafa_be_init(const oq_present_config *cfg)
{
    ChafaTermDb *db = chafa_term_db_get_default();

    term_info = chafa_term_db_detect(db, (gchar **)environ);
    return build_canvas(cfg);
}

static void chafa_be_shutdown(void)
{
    if (canvas) {
        chafa_canvas_unref(canvas);
        canvas = NULL;
    }
    if (term_info) {
        chafa_term_info_unref(term_info);
        term_info = NULL;
    }
}

static int chafa_be_resize(const oq_present_config *cfg)
{
    return build_canvas(cfg);
}

static void chafa_be_frame(const uint8_t *src, int w, int h, int stride)
{
    GString *gs;

    if (!canvas)
        return;

    chafa_canvas_draw_all_pixels(canvas, CHAFA_PIXEL_RGB8,
                                 (const guint8 *)src, w, h, stride);
    gs = chafa_canvas_print(canvas, term_info);
    if (!gs)
        return;

    /* Same reason as the caca backend: a trailing newline on the bottom row
     * scrolls the terminal. */
    while (gs->len && (gs->str[gs->len - 1] == '\n' ||
                       gs->str[gs->len - 1] == '\r'))
        g_string_truncate(gs, gs->len - 1);

    fputs("\033[H", stdout);
    fwrite(gs->str, 1, gs->len, stdout);
    fflush(stdout);
    g_string_free(gs, TRUE);
}

const oq_present_backend oq_backend_chafa = {
    "chafa", chafa_be_init, chafa_be_shutdown, chafa_be_resize, chafa_be_frame
};
