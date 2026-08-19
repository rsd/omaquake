#ifndef OQ_INPUT_H
#define OQ_INPUT_H

#include <stdint.h>
#include <stdio.h>

/* Emitted for every key transition.  keycode is a libretro RETROK_* value. */
typedef void (*oq_key_fn)(unsigned keycode, int down, uint16_t mods, void *ud);

/* has_key_release comes from oq_term_has_key_release().  When it is 0 we
 * are on a terminal that only reports presses, and releases get synthesised
 * from a timeout -- which is why holding a key feels stuttery there. */
void oq_input_init(int has_key_release);

/* Drain stdin and emit events.  Non-blocking. */
void oq_input_poll(oq_key_fn fn, void *ud);

/* Expire synthesised holds in the no-key-release fallback.  Harmless to
 * call when the kitty protocol is active. */
void oq_input_expire(oq_key_fn fn, void *ud);

/* Log raw bytes and decoded events here; NULL disables. */
void oq_input_set_trace(FILE *fp);

/* Non-zero once the user asked to quit (Ctrl-\). */
int  oq_input_quit(void);

#endif /* OQ_INPUT_H */
