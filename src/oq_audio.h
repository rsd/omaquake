/* OmaQuake -- audio output.
 *
 * The core hands us signed 16-bit stereo interleaved frames.  This is a
 * single sink, not a set of selectable backends, so it self-stubs when ALSA
 * is missing: every call below is a no-op and the game runs silently.
 */
#ifndef OQ_AUDIO_H
#define OQ_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/* Opens the device at the core's rate.  Returns 0 on success; on failure
 * the caller should log oq_audio_error() and carry on without sound. */
int oq_audio_init(int sample_rate);
void oq_audio_shutdown(void);

/* One frame is one left/right pair, so nframes is half the sample count.
 * Never blocks: samples the device has no room for are dropped. */
void oq_audio_write(const int16_t *frames, size_t nframes);

/* Reason the last call failed, for the caller's log file.  Never NULL. */
const char *oq_audio_error(void);
/* Frames dropped since init.  A steadily climbing count means the loop is
 * feeding faster than the device drains -- worth having in the log. */
unsigned long oq_audio_dropped(void);

#endif /* OQ_AUDIO_H */
