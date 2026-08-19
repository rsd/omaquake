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

/* Current size in cells.  Returns 1 if it changed since the last call. */
int  oq_term_size(int *cols, int *rows);

/* 1 if the terminal acknowledged the kitty keyboard protocol, meaning we
 * get real key-release events.  0 means press-only input and the caller
 * must synthesise releases. */
int  oq_term_has_key_release(void);

/* Set by SIGINT/SIGTERM handlers. */
extern volatile int oq_term_quit_requested;

#endif /* OQ_TERM_H */
