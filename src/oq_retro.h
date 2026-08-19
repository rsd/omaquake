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
    oq_video_sink sink;
    void         *sink_ud;
} oq_retro_config;

int    oq_retro_init(const oq_retro_config *cfg, const char *pak_path);
void   oq_retro_run(void);              /* advance exactly one frame */
double oq_retro_fps(void);              /* from retro_get_system_av_info */
void   oq_retro_key(unsigned keycode, int down, uint16_t mods);
void   oq_retro_shutdown(void);

#endif /* OQ_RETRO_H */
