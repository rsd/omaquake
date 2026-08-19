/* OmaQuake -- raw pointer input from Linux evdev.
 *
 * The terminal can only ever say where the pointer IS (see oq_mouse.h for
 * what that costs us).  The kernel's own event devices say how far it
 * MOVED, unbounded, which is what a first-person game actually wants: no
 * window edge to wall out at, so no steering band to work around it.  This
 * is therefore the preferred source when a device can be opened, with the
 * terminal path kept as the fallback for machines where it cannot.
 *
 * SAFETY: an event device is a firehose of everything the hardware sends,
 * so reading the wrong one is keylogging.  Nothing here ever reads a device
 * that reports keyboard keys -- not the auto-detected one and not one named
 * explicitly with --mouse-dev; see oq_evdev_classify().
 */
#ifndef OQ_EVDEV_H
#define OQ_EVDEV_H

#include <stddef.h>
#include <stdio.h>

/* What the capability test made of a device. */
typedef enum {
    OQ_EVDEV_POINTER = 0,   /* EV_REL with REL_X+REL_Y, EV_KEY with BTN_LEFT */
    OQ_EVDEV_KEYBOARD,      /* reports keyboard keys: never opened for reading */
    OQ_EVDEV_NOT_POINTER,   /* lacks relative axes or buttons */
    OQ_EVDEV_UNREADABLE     /* no permission, or not an event device at all */
} oq_evdev_verdict;

const char *oq_evdev_verdict_str(oq_evdev_verdict v);

/* Test one device by its capability bitmaps.  Opens the node to run the
 * ioctls and closes it again without ever read()ing it -- the descriptor is
 * only kept for a device that came back OQ_EVDEV_POINTER.
 *
 * name receives EVIOCGNAME (NUL-terminated, may be NULL/0 to skip). */
oq_evdev_verdict oq_evdev_classify(const char *path, char *name, size_t cap);

/* Write the whole /dev/input scan and its verdicts to out; what
 * --mouse-list prints.  Diagnostic only, opens nothing for reading. */
void oq_evdev_list(FILE *out);

/* Open a pointer.  path NULL scans /dev/input/event* and takes the first
 * device that classifies as a pointer; an explicit path is subject to
 * exactly the same test and is refused, not opened, if it fails.
 *
 * Returns 0 on success, -1 with oq_evdev_error() set otherwise.  Does NOT
 * grab: the caller decides when the exclusive-access window opens. */
int oq_evdev_open(const char *path);

/* Exclusive access.  With the grab the compositor stops seeing the device
 * entirely, so the desktop pointer freezes where it is and every count
 * belongs to the game -- which is the whole reason this source can aim 1:1
 * with no re-centring trick.
 *
 * A held grab is a DEAD MOUSE for the whole session, so it has to come off
 * on every exit path: oq_term_shutdown() calls oq_evdev_close(), and that
 * runs from atexit() and from the SIGINT/SIGTERM handlers.  The kernel also
 * drops the grab when the descriptor is closed, so even a SIGKILL or a
 * segfault returns the mouse -- killing the process is always a way out. */
int  oq_evdev_grab(void);
void oq_evdev_ungrab(void);

/* Ungrabs and closes.  Idempotent, and safe to call having never opened. */
void oq_evdev_close(void);

int         oq_evdev_is_open(void);
const char *oq_evdev_path(void);   /* NULL when nothing is open */
const char *oq_evdev_name(void);   /* device name, "" when nothing is open */
/* Why the last call failed, for the caller's log/stderr.  Never NULL. */
const char *oq_evdev_error(void);

/* Button transitions, using the OQ_MB_* numbering from oq_input.h so both
 * pointer sources hand the engine the same button ids. */
typedef void (*oq_evdev_button_fn)(int button, int down, void *ud);

/* Drain whatever the device has queued.  dx and dy come back holding the
 * relative counts accumulated since the last call, buttons through fn
 * (which may be NULL).  Non-blocking: an idle device costs one read()
 * returning EAGAIN, so this can sit in the engine loop. */
void oq_evdev_poll(int *dx, int *dy, oq_evdev_button_fn fn, void *ud);

#endif /* OQ_EVDEV_H */
