/* ALSA audio output.
 *
 * The host loop paces frames itself, so this path must never wait: a
 * blocking snd_pcm_writei() would park the whole loop inside the sound card
 * driver and the picture would stutter in step with the audio.  The device
 * is therefore opened SND_PCM_NONBLOCK and a write that does not fit is
 * simply dropped.  Dropping is the right failure: the core produces exactly
 * one frame's worth of samples per retro_run, so the only way the buffer
 * fills is that we are ahead of the clock, and the next writes land again.
 *
 * A ring buffer plus a feeder thread would recover those samples, but it
 * buys a mutex and a thread to hide a glitch that is inaudible when it is
 * rare and inevitable when it is not.
 */
#include "oq_audio.h"

#ifdef OQ_HAVE_ALSA

#include <alsa/asoundlib.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Deep enough to ride out the loop's own jitter (the core hands us one video
 * frame of audio at a time -- 735 frames, ~17 ms, at 44.1 kHz / 60 fps),
 * shallow enough that gunfire is not noticeably late against the muzzle
 * flash.  Measured drop rates at 40 ms and 160 ms are the same, because what
 * actually overflows the buffer is a loop stall (a level load), not buffer
 * depth -- so this sits in the middle of a flat curve. */
#define OQ_AUDIO_LATENCY_US 80000

static snd_pcm_t *pcm;
static const char *pcm_name;
static unsigned long dropped;
static char errbuf[160];

static void set_error(const char *what, int err)
{
    snprintf(errbuf, sizeof(errbuf), "%s: %s", what, snd_strerror(err));
}

/* ALSA's default error handler writes to stderr, which is the same terminal
 * the picture is on -- one "underrun occurred" would shred the frame. */
static void oq_alsa_silent(const char *file, int line, const char *fn,
                           int err, const char *fmt, ...)
{
    (void)file; (void)line; (void)fn; (void)err; (void)fmt;
}

int oq_audio_init(int sample_rate)
{
    const char *dev;
    int err;

    if (pcm)
        return 0;
    if (sample_rate <= 0) {
        snprintf(errbuf, sizeof(errbuf), "bad sample rate %d", sample_rate);
        return -1;
    }

    snd_lib_error_set_handler(oq_alsa_silent);

    /* An automated run has to be able to exercise this path without making
     * noise on whatever machine it lands on: OMAQUAKE_ALSA_DEVICE=null
     * points at ALSA's built-in bit bucket. */
    dev = getenv("OMAQUAKE_ALSA_DEVICE");
    if (!dev || !*dev)
        dev = "default";

    err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        pcm = NULL;
        snprintf(errbuf, sizeof(errbuf), "snd_pcm_open(%s): %s", dev,
                 snd_strerror(err));
        return -1;
    }

    /* soft_resample=1: the core picks its own rate, and refusing to open
     * because the hardware wants 48000 would be a silly way to lose sound. */
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16,
                             SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                             (unsigned)sample_rate, 1, OQ_AUDIO_LATENCY_US);
    if (err < 0) {
        set_error("snd_pcm_set_params", err);
        snd_pcm_close(pcm);
        pcm = NULL;
        return -1;
    }

    pcm_name = dev;
    dropped = 0;
    return 0;
}

void oq_audio_write(const int16_t *frames, size_t nframes)
{
    int retries = 2;

    if (!pcm || !frames)
        return;

    while (nframes > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, frames, nframes);

        if (n > 0) {
            frames += (size_t)n * 2;   /* stereo interleaved */
            nframes -= (size_t)n;
            continue;
        }
        /* n == 0 is not documented for a non-blocking device, but treating
         * it as "no room" keeps this loop from spinning if it ever happens. */
        if (n == 0 || n == -EAGAIN) {
            dropped += nframes;        /* buffer full: never wait for it */
            return;
        }
        /* -EPIPE (we starved the card) and -ESTRPIPE (suspend) are both
         * recoverable; silent=1 keeps the recovery off stderr too. */
        if (retries-- <= 0 || snd_pcm_recover(pcm, (int)n, 1) < 0) {
            set_error("snd_pcm_writei", (int)n);
            dropped += nframes;
            return;
        }
    }
}

void oq_audio_shutdown(void)
{
    if (!pcm)
        return;
    /* Drop rather than drain: draining waits for the buffer to play out,
     * and the terminal is already being handed back to the shell. */
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = NULL;
    pcm_name = NULL;
}

#else /* !OQ_HAVE_ALSA */

int oq_audio_init(int sample_rate)
{
    (void)sample_rate;
    return -1;
}

void oq_audio_write(const int16_t *frames, size_t nframes)
{
    (void)frames; (void)nframes;
}

void oq_audio_shutdown(void)
{
}

#endif /* OQ_HAVE_ALSA */

const char *oq_audio_error(void)
{
#ifdef OQ_HAVE_ALSA
    return errbuf[0] ? errbuf : "no error";
#else
    return "built without ALSA support";
#endif
}

const char *oq_audio_device(void)
{
#ifdef OQ_HAVE_ALSA
    return pcm_name;
#else
    return NULL;
#endif
}

unsigned long oq_audio_dropped(void)
{
#ifdef OQ_HAVE_ALSA
    return dropped;
#else
    return 0;
#endif
}
