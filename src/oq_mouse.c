/* The hybrid pointer-look model.  See oq_mouse.h for why it is hybrid. */
#include "oq_mouse.h"

#include <math.h>
#include <time.h>

/* What one mouse count is worth to the engine: IN_Move multiplies by
 * sensitivity (3) and then by m_yaw / m_pitch (0.022), both stock values.
 * We only need it to express the steering rate in degrees per second --
 * the count itself stays the unit we hand over, because the core does
 * `mx *= sensitivity.value` with mx an int, so any scaling we tried to do
 * through the cvar below 1.0 would truncate straight to zero. */
#define DEG_PER_COUNT 0.066

/* A pointer that has been still for this long is not mid-gesture, so the
 * next report starts a fresh measurement rather than a delta.  Without it,
 * leaving the window on one side and coming back on the other reads as one
 * enormous flick. */
#define RESYNC_MS 150

/* No real hand movement covers this many pixels between two reports under
 * any-motion tracking; anything larger is a report we mis-associated. */
#define MAX_STEP 500

static oq_mouse_config conf;
static int extent_w, extent_h;
static int have_pos;
static int pos_x, pos_y;

static double accum_x, accum_y;
static long long last_track_ms;
static long long last_step_ms;

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void oq_mouse_init(const oq_mouse_config *cfg)
{
    conf = *cfg;
    extent_w = extent_h = 0;
    have_pos = 0;
    pos_x = pos_y = 0;
    accum_x = accum_y = 0.0;
    last_track_ms = 0;
    last_step_ms = now_ms();
}

void oq_mouse_set_extent(int w, int h)
{
    if (w > 0 && h > 0) {
        extent_w = w;
        extent_h = h;
    }
}

void oq_mouse_set_active(int active)
{
    if (!active) {
        /* Drop the position, not just the steering: whatever the pointer
         * did while we were not being told about it must not arrive as one
         * delta when reports resume. */
        have_pos = 0;
        accum_x = accum_y = 0.0;
    }
}

void oq_mouse_track(int x, int y)
{
    long long t = now_ms();

    if (have_pos && t - last_track_ms <= RESYNC_MS) {
        int dx = x - pos_x;
        int dy = y - pos_y;

        if (dx > MAX_STEP)  dx = MAX_STEP;
        if (dx < -MAX_STEP) dx = -MAX_STEP;
        if (dy > MAX_STEP)  dy = MAX_STEP;
        if (dy < -MAX_STEP) dy = -MAX_STEP;

        accum_x += dx * conf.sens;
        accum_y += dy * conf.sens * (conf.invert ? -1.0 : 1.0);
    }
    pos_x = x;
    pos_y = y;
    have_pos = 1;
    last_track_ms = t;
}

/* How hard this axis steers, -1..1.  Zero through the central region, then
 * a squared ramp across the band so entering it is a nudge rather than a
 * step change and full rate is only reached at the very edge. */
static double band(int p, int extent)
{
    double width = extent * conf.edge;
    double t;

    if (conf.edge <= 0.0 || extent <= 0)
        return 0.0;
    if (width < 1.0)
        width = 1.0;

    if (p > extent - width)
        t = (p - (extent - width)) / width;
    else if (p < width)
        t = (p - width) / width;
    else
        return 0.0;

    if (t > 1.0)  t = 1.0;
    if (t < -1.0) t = -1.0;
    return t * fabs(t);
}

/* Hand over whole counts and keep the remainder: the steering term is a
 * fraction of a count per frame at low rates, and truncating it every
 * frame would mean it never moved the view at all. */
static int take(double *accum)
{
    double whole = (*accum < 0.0) ? ceil(*accum) : floor(*accum);

    *accum -= whole;
    if (whole > 30000.0)  whole = 30000.0;
    if (whole < -30000.0) whole = -30000.0;
    return (int)whole;
}

void oq_mouse_step(int *dx, int *dy)
{
    long long t = now_ms();
    double dt = (t - last_step_ms) / 1000.0;

    last_step_ms = t;
    /* A stall -- a long disk read, a resize -- must not be paid back as one
     * huge lurch of accumulated steering. */
    if (dt < 0.0)   dt = 0.0;
    if (dt > 0.1)   dt = 0.1;

    if (have_pos) {
        double rate = conf.turn / DEG_PER_COUNT * dt;

        accum_x += band(pos_x, extent_w) * rate;
        accum_y += band(pos_y, extent_h) * rate *
                   (conf.invert ? -1.0 : 1.0);
    }

    *dx = take(&accum_x);
    *dy = take(&accum_y);
}

void oq_mouse_debug(int *x, int *y, double *ex, double *ey)
{
    *x = pos_x;
    *y = pos_y;
    *ex = have_pos ? band(pos_x, extent_w) : 0.0;
    *ey = have_pos ? band(pos_y, extent_h) : 0.0;
}
