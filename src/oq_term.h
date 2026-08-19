#ifndef OQ_TERM_H
#define OQ_TERM_H

#include <stddef.h>

/* Enter raw mode + alternate screen.  Returns 0 on success. */
int  oq_term_init(void);
/* Restore the terminal.  Idempotent; also runs from atexit and signals. */
void oq_term_shutdown(void);

/* Write one finished frame: homes the cursor, translates the backends' bare
 * newlines to CRLF, and emits it in a single write.
 *
 * The translation is not optional.  Raw mode clears OPOST, which disables
 * ONLCR, so a bare LF moves down a line WITHOUT returning to column 0 --
 * every row then starts where the last one ended, runs off the right edge,
 * wraps, and scrolls the screen like a mistuned CRT. */
void oq_term_present(const char *s, size_t len);

/* As above, but places the first row at (row, col), 1-based, and positions
 * every subsequent row explicitly. Used when the canvas is smaller than the
 * terminal. */
void oq_term_present_at(const char *s, size_t len, int row, int col);

/* Clear the screen; call when the canvas geometry changes so no stale cells
 * survive around a newly shrunk canvas. */
void oq_term_clear(void);

/* Current size in cells.  Returns 1 if it changed since the last call. */
int  oq_term_size(int *cols, int *rows);

/* 1 if the terminal acknowledged the kitty keyboard protocol, meaning we
 * get real key-release events.  0 means press-only input and the caller
 * must synthesise releases. */
int  oq_term_has_key_release(void);

/* Start/stop any-motion pointer reporting in pixel coordinates.
 *
 * Both write to the terminal, so they may only be called while the render
 * thread is not running -- i.e. around the game loop, never inside it.
 * Disable is idempotent and also runs from oq_term_shutdown(). */
void oq_term_mouse_enable(void);
void oq_term_mouse_disable(void);

/* Pixel size of the text area, as reported by CSI 14 t at init.  Returns 0
 * on success, -1 if the terminal never answered.
 *
 * Cached deliberately: a re-query is a write to the terminal plus a read
 * from stdin, and once the game loop is up the render thread owns stdout
 * and the input parser owns stdin.  Callers that need the size after a
 * resize scale these numbers by the change in cell count instead. */
int  oq_term_text_pixels(int *w, int *h);

/* Set by SIGINT/SIGTERM handlers. */
extern volatile int oq_term_quit_requested;

#endif /* OQ_TERM_H */
