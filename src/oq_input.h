#ifndef OQ_INPUT_H
#define OQ_INPUT_H

#include <stdint.h>
#include <stdio.h>

/* Emitted for every key transition.  keycode is a libretro RETROK_* value. */
typedef void (*oq_key_fn)(unsigned keycode, int down, uint16_t mods, void *ud);

/* Buttons as they arrive in an SGR mouse report, renumbered so the wheel
 * follows the three real buttons. */
enum {
    OQ_MB_NONE        = -1,     /* motion with nothing held */
    OQ_MB_LEFT        = 0,
    OQ_MB_MIDDLE      = 1,
    OQ_MB_RIGHT       = 2,
    OQ_MB_WHEEL_UP    = 3,
    OQ_MB_WHEEL_DOWN  = 4,
    OQ_MB_WHEEL_LEFT  = 5,
    OQ_MB_WHEEL_RIGHT = 6
};

/* One decoded pointer report.
 *
 * x and y are PIXELS within the terminal's text area and may be negative or
 * past the far edge: a terminal keeps reporting while a button is held and
 * the pointer is dragged outside the window. */
typedef struct {
    int x, y;
    int button;         /* OQ_MB_*, OQ_MB_NONE for plain motion */
    int down;           /* 1 press, 0 release; always 1 for motion */
    int motion;         /* 1 if the pointer moved rather than clicked */
} oq_mouse_event;

typedef void (*oq_mouse_fn)(const oq_mouse_event *ev, void *ud);

/* has_key_release comes from oq_term_has_key_release().  When it is 0 we
 * are on a terminal that only reports presses, and releases get synthesised
 * from a timeout -- which is why holding a key feels stuttery there. */
void oq_input_init(int has_key_release);

/* Drain stdin and emit events.  Non-blocking.  Either callback may be NULL;
 * a NULL mouse callback still consumes the reports so they cannot be
 * mistaken for keystrokes. */
void oq_input_poll(oq_key_fn fn, oq_mouse_fn mfn, void *ud);

/* Expire synthesised holds in the no-key-release fallback.  Harmless to
 * call when the kitty protocol is active. */
void oq_input_expire(oq_key_fn fn, void *ud);

/* Log raw bytes and decoded events here; NULL disables. */
void oq_input_set_trace(FILE *fp);

/* 0 while the terminal has told us it lost focus (mode 1004).  Terminals
 * that do not report focus leave this at 1 forever, which is the safe
 * answer.  Pointer steering has to stop here: no reports arrive while we
 * are in the background, so the last known position would keep turning the
 * view for as long as the player is away. */
int  oq_input_focused(void);

/* Returns 1 once, and clears, when the player asked to release or retake the
 * pointer (Ctrl-G). */
int  oq_input_take_mouse_toggle(void);

/* Non-zero once the user asked to quit (Ctrl-\). */
int  oq_input_quit(void);

#endif /* OQ_INPUT_H */
