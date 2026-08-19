#ifndef OQ_RETRO_H
#define OQ_RETRO_H

#include <stdint.h>

/* Receives each finished frame already expanded to packed RGB888. */
typedef void (*oq_video_sink)(const uint8_t *rgb, int w, int h, int stride,
                              void *ud);

typedef struct {
    const char   *resolution;   /* core option, e.g. "320x200" */
    const char   *framerate;    /* "auto" or a number of fps */
    const char   *samplerate;   /* "auto" or a rate in Hz */
    const char   *save_dir;
    const char   *log_path;     /* core log destination, or NULL to discard */
    int           sound;        /* 0 disables audio output entirely */
    /* Asked before a frame is expanded to RGB. Returning 0 skips the
     * conversion entirely -- there is no point paying for 64k pixels of
     * colour expansion on a frame the presenter is going to drop. */
    int         (*want_frame)(void *ud);
    oq_video_sink sink;
    void         *sink_ud;
} oq_retro_config;

int    oq_retro_init(const oq_retro_config *cfg, const char *pak_path);

/* Write a host-side line to the same --log file the core logs to.  The
 * only diagnostic channel that exists once the game is up: stdout is the
 * picture and stderr shares the terminal with it.  A no-op before
 * oq_retro_init() has opened the log, and when no --log was given. */
void   oq_retro_log(const char *fmt, ...);
void   oq_retro_run(void);              /* advance exactly one frame */
double oq_retro_fps(void);              /* from retro_get_system_av_info */
void   oq_retro_key(unsigned keycode, int down, uint16_t mods);

/* Relative mouse motion in engine counts, added to whatever has not been
 * read yet.  The core clears it by reading, once per frame, in IN_Move. */
void   oq_retro_mouse_move(int dx, int dy);

/* index is an OQ_MB_* button; wheel "buttons" have no release, so they are
 * latched and cleared by the read that reports them. */
void   oq_retro_mouse_button(int index, int down);
void   oq_retro_shutdown(void);

#endif /* OQ_RETRO_H */
