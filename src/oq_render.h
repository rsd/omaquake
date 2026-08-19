#ifndef OQ_RENDER_H
#define OQ_RENDER_H

#include "oq_present.h"

/* Presentation on its own thread.
 *
 * The engine needs ~16.9ms per frame against a 16.7ms budget at its 60fps
 * target, so it already saturates one core on its own. Any conversion work
 * done on the engine thread therefore comes straight out of the frame rate --
 * and because the core emits sample_rate/fps audio samples per retro_run,
 * a slower engine loop starves the sound card and the audio breaks up.
 *
 * So the engine thread only memcpy's the frame (~192KB, tens of microseconds)
 * and moves on. Everything expensive happens here.
 */

/* Returns 0 on success. */
int  oq_render_start(const oq_present_backend *be, const oq_present_config *cfg);

/* Hand over a frame. Non-blocking: if the renderer is still busy with the
 * previous one this overwrites it, because a stale frame is worth nothing. */
void oq_render_submit(const uint8_t *rgb, int w, int h, int stride);

/* Push new geometry; picked up before the next frame is drawn. */
void oq_render_reconfigure(const oq_present_config *cfg);

/* Drain and stop. */
void oq_render_stop(void);

/* Frames dropped because the renderer could not keep up. */
unsigned long oq_render_dropped(void);

#endif /* OQ_RENDER_H */
