#ifndef OQ_MOUSE_H
#define OQ_MOUSE_H

/* Pointer position -> view movement.
 *
 * A terminal reports where the pointer IS, never how far it moved, and
 * there is no pointer lock and no warp: we cannot pull it back to the
 * centre the way a windowed game does.  So pure delta-from-absolute look
 * runs out of desk -- the pointer hits the edge of the window, motion
 * reports stop, and the turn stops with them, mid-corner.
 *
 * Hence the hybrid.  In the central region the pointer drives the view
 * 1:1, which is what makes aiming feel like a mouse.  Across an outer band
 * on each side a rate term fades in on top, so pushing to the edge and
 * holding keeps turning for as long as you hold it.  The two are added,
 * not switched between: the 1:1 term dies away on its own as the pointer
 * runs out of room, and the rate term is already carrying the turn by the
 * time it does.
 *
 * All of that is a workaround for absolute positions, and it applies only
 * to the terminal source.  Fed by evdev (oq_evdev.h) the deltas are already
 * relative and unbounded, config.relative switches the band off, and what
 * is left is plain 1:1 aiming.
 */
typedef struct {
    double sens;    /* pointer pixels -> mouse counts; 1.0 is the engine's
                     * own default feel */
    double edge;    /* fraction of each side that steers; 0 disables */
    double turn;    /* steering rate at the outermost edge, degrees/sec */
    int    invert;  /* invert pitch */
    /* The source reports relative motion (evdev), not a position.  That
     * motion is unbounded -- there is no window edge to run out of -- so
     * the steering band above has nothing to fix and is switched off
     * entirely: edge and turn are ignored, and aiming is plain 1:1. */
    int    relative;
} oq_mouse_config;

void oq_mouse_init(const oq_mouse_config *cfg);

/* Pixel size of the text area, i.e. the coordinate space the reports use. */
void oq_mouse_set_extent(int w, int h);

/* An absolute pointer position from the terminal.  May be outside the
 * extent; a terminal keeps reporting during a drag past the window edge.
 * Terminal source only. */
void oq_mouse_track(int x, int y);

/* Relative motion in device counts, already known to be a delta.  evdev
 * source only; nothing here needs the extent or the resync guard, because
 * a delta cannot be mis-associated with a previous position the way two
 * absolute reports either side of a gap can. */
void oq_mouse_move(int dx, int dy);

/* Stop and resync -- used when the terminal loses focus, where reports stop
 * arriving and the last known position would otherwise steer forever. */
void oq_mouse_set_active(int active);

/* Mouse counts to hand the engine for this frame: the 1:1 term accumulated
 * since the last call plus the steering term for the time that passed.
 * Fractions are carried over, so a slow steer is not rounded away. */
void oq_mouse_step(int *dx, int *dy);

/* --keytest diagnostics: pointer position and the two steering factors,
 * each -1..1 across its band. */
void oq_mouse_debug(int *x, int *y, double *ex, double *ey);

#endif /* OQ_MOUSE_H */
