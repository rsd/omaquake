/* OmaQuake -- terminal presentation backend interface.
 *
 * Every backend converts a packed RGB888 frame into characters and writes
 * them to stdout.  Backends are selected at runtime (--video=NAME) so the
 * same frame can be A/B'd between renderers.
 */
#ifndef OQ_PRESENT_H
#define OQ_PRESENT_H

#include <stdint.h>

/* Glyph repertoire.  This is the "which characters" dial, independent of
 * the colour dial below.  Backends that cannot honour a mode fall back to
 * the closest they support. */
typedef enum {
    OQ_SYMBOLS_ASCII,   /* letters, digits, punctuation only */
    OQ_SYMBOLS_BLOCK,   /* + half blocks and shade characters */
    OQ_SYMBOLS_FINE,    /* + quadrants/sextants/octants: 2x4 subcell detail */
} oq_symbols;

/* Colour depth.  The terminal caps this; we request, the backend clamps. */
typedef enum {
    OQ_COLOR_MONO,
    OQ_COLOR_16,
    OQ_COLOR_256,
    OQ_COLOR_TRUE,
} oq_color;

typedef struct {
    oq_symbols symbols;
    oq_color   color;
    int        cols;      /* terminal size in cells, updated on resize */
    int        rows;
    /* Pixel dimensions of one character cell.  Terminal cells are roughly
     * 1:2, so without this Quake's 4:3 picture comes out squashed. */
    int        cell_w;
    int        cell_h;
} oq_present_config;

typedef struct oq_present_backend {
    const char *name;
    /* Returns 0 on success.  May be called again after shutdown. */
    int  (*init)(const oq_present_config *cfg);
    void (*shutdown)(void);
    /* Re-target the canvas after a terminal resize. */
    int  (*resize)(const oq_present_config *cfg);
    /* Convert and emit one frame.  src is packed RGB888, stride in bytes. */
    void (*frame)(const uint8_t *src, int w, int h, int stride);
} oq_present_backend;

extern const oq_present_backend oq_backend_chafa;
extern const oq_present_backend oq_backend_caca;

/* NULL if no backend by that name was compiled in. */
const oq_present_backend *oq_present_lookup(const char *name);
/* Comma-separated list of compiled-in backend names, for --help. */
const char *oq_present_available(void);

#endif /* OQ_PRESENT_H */
