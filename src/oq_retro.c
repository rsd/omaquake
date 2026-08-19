/* libretro host for the statically linked tyrquake core.
 *
 * The core is linked in directly (STATIC_LINKING=1), so we call the retro_*
 * entry points rather than dlopen'ing anything.  Our job is to satisfy the
 * environment queries it makes, take RGB565 frames off video_refresh, and
 * push key events into the keyboard callback it registers.
 */
#include "oq_retro.h"

#include "oq_audio.h"
#include "oq_input.h"

#include <libretro.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static oq_retro_config conf;
static retro_keyboard_event_t kbd_cb;
static uint8_t *rgb_buf;
static size_t rgb_cap;
static double core_fps = 60.0;
static int audio_on;
static FILE *logfp;

/* Pending relative motion, in the counts IN_Move expects.  Reading clears
 * it, which is the libretro contract for a relative device. */
static int mouse_dx, mouse_dy;
static int mouse_btn[3];        /* left, middle, right -- held */
static int mouse_wheel[4];      /* up, down, left, right -- latched pulses */

/* The engine polls the arrow keys through input_state every frame and calls
 * Key_Event(K_UPARROW, 0) whenever we answer 0.  That poll runs on the same
 * key numbers the keyboard callback delivers, so unless we mirror what the
 * terminal told us is held, it releases the arrow key the callback just
 * pressed and turning stops dead. */
static int arrow_down[4];       /* up, down, left, right */

static int arrow_slot(unsigned keycode)
{
    switch (keycode) {
    case RETROK_UP:    return 0;
    case RETROK_DOWN:  return 1;
    case RETROK_LEFT:  return 2;
    case RETROK_RIGHT: return 3;
    }
    return -1;
}

/* Anything the core prints must not reach stdout -- stdout is the picture.
 * Without this the first log line shreds the frame. */
static void RETRO_CALLCONV log_printf(enum retro_log_level level,
                                      const char *fmt, ...)
{
    va_list ap;

    if (!logfp)
        return;
    fprintf(logfp, "[%d] ", (int)level);
    va_start(ap, fmt);
    vfprintf(logfp, fmt, ap);
    va_end(ap);
    fflush(logfp);
}

static bool RETRO_CALLCONV env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const enum retro_pixel_format *fmt = data;
        /* The core is built with FRONTEND_SUPPORTS_RGB565 and asks for it;
         * anything else means our expansion below is wrong. */
        return *fmt == RETRO_PIXEL_FORMAT_RGB565;
    }
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
        const struct retro_keyboard_callback *cb = data;
        kbd_cb = cb->callback;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        struct retro_log_callback *cb = data;
        cb->log = log_printf;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = data;

        if (!strcmp(var->key, "tyrquake_resolution"))
            var->value = conf.resolution;
        else if (!strcmp(var->key, "tyrquake_framerate"))
            var->value = conf.framerate;
        else if (!strcmp(var->key, "tyrquake_sound_samplerate"))
            var->value = conf.samplerate;
        else if (!strcmp(var->key, "tyrquake_compute_rendering"))
            var->value = "disabled";   /* force the software rasterizer */
        else if (!strcmp(var->key, "tyrquake_rumble"))
            var->value = "disabled";
        else if (!strcmp(var->key, "tyrquake_invert_y_axis"))
            var->value = "disabled";
        else if (!strcmp(var->key, "tyrquake_analog_deadzone"))
            var->value = "15";
        else
            return false;
        return var->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
        bool *upd = data;
        *upd = false;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
        const char **dir = data;
        *dir = conf.save_dir;
        return conf.save_dir != NULL;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
        bool *dupe = data;
        *dupe = true;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
        int *bits = data;
        *bits = 1 | 2;              /* video and audio both live */
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const struct retro_message *msg = data;
        if (logfp && msg && msg->msg)
            fprintf(logfp, "message: %s\n", msg->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        return true;
    default:
        /* Everything else -- rumble, VFS, bitmasks, float audio, memory
         * status -- is optional; declining makes the core take its
         * fallback path. */
        return false;
    }
}

static void ensure_rgb(size_t need)
{
    if (rgb_cap >= need)
        return;
    free(rgb_buf);
    rgb_buf = malloc(need);
    rgb_cap = rgb_buf ? need : 0;
}

static void RETRO_CALLCONV video_refresh(const void *data, unsigned width,
                                         unsigned height, size_t pitch)
{
    const uint8_t *row = data;
    unsigned x, y;

    /* A NULL frame means "same as last time" (we advertised CAN_DUPE). */
    if (!data || !conf.sink)
        return;
    if (conf.want_frame && !conf.want_frame(conf.sink_ud))
        return;

    ensure_rgb((size_t)width * height * 3);
    if (!rgb_buf)
        return;

    for (y = 0; y < height; y++) {
        const uint16_t *src = (const uint16_t *)(const void *)row;
        uint8_t *dst = rgb_buf + (size_t)y * width * 3;

        for (x = 0; x < width; x++) {
            uint16_t p = src[x];
            unsigned r = (p >> 11) & 0x1f;
            unsigned g = (p >> 5) & 0x3f;
            unsigned b = p & 0x1f;

            /* Replicate the high bits into the low ones so full-scale
             * inputs reach 255 rather than 248. */
            *dst++ = (uint8_t)((r << 3) | (r >> 2));
            *dst++ = (uint8_t)((g << 2) | (g >> 4));
            *dst++ = (uint8_t)((b << 3) | (b >> 2));
        }
        row += pitch;
    }

    conf.sink(rgb_buf, (int)width, (int)height, (int)width * 3, conf.sink_ud);
}

/* tyrquake uses the batch callback, but the single-sample one has to work
 * too: the core is free to mix the two and a missing sink means silence. */
static void RETRO_CALLCONV audio_sample(int16_t l, int16_t r)
{
    int16_t frame[2];

    if (!audio_on)
        return;
    frame[0] = l;
    frame[1] = r;
    oq_audio_write(frame, 1);
}

/* Signed 16-bit stereo interleaved, so "frames" is half the sample count.
 * We always claim to have consumed all of them -- the core has no useful
 * response to a short write and we never block to make one true. */
static size_t RETRO_CALLCONV audio_sample_batch(const int16_t *data,
                                                size_t frames)
{
    if (audio_on)
        oq_audio_write(data, frames);
    return frames;
}

static void RETRO_CALLCONV input_poll(void) { }

/* Keys reach the engine through the keyboard callback; the pointer cannot,
 * because the callback carries no axes.  It comes through here instead --
 * IN_Move reads MOUSE_X/Y as RELATIVE deltas and applies them straight to
 * cl.viewangles, which is exactly the free-look path we want. */
static int16_t RETRO_CALLCONV input_state(unsigned port, unsigned device,
                                          unsigned index, unsigned id)
{
    (void)index;

    if (port != 0)
        return 0;

    if (device == RETRO_DEVICE_MOUSE) {
        int v;

        switch (id) {
        case RETRO_DEVICE_ID_MOUSE_X:
            v = mouse_dx;
            mouse_dx = 0;               /* relative: cleared on read */
            return (int16_t)v;
        case RETRO_DEVICE_ID_MOUSE_Y:
            v = mouse_dy;
            mouse_dy = 0;
            return (int16_t)v;
        case RETRO_DEVICE_ID_MOUSE_LEFT:   return (int16_t)mouse_btn[0];
        case RETRO_DEVICE_ID_MOUSE_MIDDLE: return (int16_t)mouse_btn[1];
        case RETRO_DEVICE_ID_MOUSE_RIGHT:  return (int16_t)mouse_btn[2];
        case RETRO_DEVICE_ID_MOUSE_WHEELUP:
        case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
        case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP:
        case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN: {
            /* A wheel notch is one event with no release.  Report it for
             * exactly one poll so the engine sees a press and then the
             * release that ends the bound command. */
            int slot = (id == RETRO_DEVICE_ID_MOUSE_WHEELUP)   ? 0 :
                       (id == RETRO_DEVICE_ID_MOUSE_WHEELDOWN) ? 1 :
                       (id == RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP) ? 2 : 3;

            v = mouse_wheel[slot];
            mouse_wheel[slot] = 0;
            return (int16_t)v;
        }
        }
        return 0;
    }

    if (device == RETRO_DEVICE_KEYBOARD) {
        int slot = arrow_slot(id);

        return slot >= 0 ? (int16_t)arrow_down[slot] : 0;
    }
    return 0;
}

void oq_retro_log(const char *fmt, ...)
{
    va_list ap;

    if (!logfp)
        return;
    va_start(ap, fmt);
    vfprintf(logfp, fmt, ap);
    va_end(ap);
    fflush(logfp);
}

void oq_retro_mouse_move(int dx, int dy)
{
    mouse_dx += dx;
    mouse_dy += dy;
}

void oq_retro_mouse_button(int index, int down)
{
    switch (index) {
    case OQ_MB_LEFT:   mouse_btn[0] = down; break;
    case OQ_MB_MIDDLE: mouse_btn[1] = down; break;
    case OQ_MB_RIGHT:  mouse_btn[2] = down; break;
    case OQ_MB_WHEEL_UP:    if (down) mouse_wheel[0] = 1; break;
    case OQ_MB_WHEEL_DOWN:  if (down) mouse_wheel[1] = 1; break;
    case OQ_MB_WHEEL_LEFT:  if (down) mouse_wheel[2] = 1; break;
    case OQ_MB_WHEEL_RIGHT: if (down) mouse_wheel[3] = 1; break;
    }
}

int oq_retro_init(const oq_retro_config *cfg, const char *pak_path)
{
    struct retro_game_info game;
    struct retro_system_av_info av;

    conf = *cfg;
    if (conf.log_path) {
        logfp = fopen(conf.log_path, "w");
        if (!logfp)
            return -1;
    }

    /* Order matters: the core reads core options and registers its keyboard
     * callback from inside retro_load_game, so the environment callback has
     * to be in place well before that. */
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_refresh);
    retro_set_audio_sample(audio_sample);
    retro_set_audio_sample_batch(audio_sample_batch);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);

    retro_init();

    memset(&game, 0, sizeof(game));
    game.path = pak_path;
    if (!retro_load_game(&game)) {
        retro_deinit();
        return -1;
    }

    /* IN_Init leaves port 0 on RETRO_DEVICE_JOYPAD, and on that setting the
     * core never reads the mouse at all: both the pointer deltas in IN_Move
     * and the mouse buttons in Sys_SendKeyEvents sit behind a test for the
     * keyboard device.  Nothing else selects it -- the frontend has to. */
    retro_set_controller_port_device(0, RETRO_DEVICE_KEYBOARD);

    memset(&av, 0, sizeof(av));
    retro_get_system_av_info(&av);
    if (av.timing.fps > 1.0)
        core_fps = av.timing.fps;
    if (logfp)
        fprintf(logfp, "core: %.2f fps, %.0f Hz audio\n",
                av.timing.fps, av.timing.sample_rate);

    /* The core decides the rate (tyrquake_sound_samplerate), so the device
     * can only be opened once av_info is in.  Losing sound is not fatal --
     * a machine with no card still gets to play. */
    if (conf.sound) {
        if (oq_audio_init((int)av.timing.sample_rate) == 0) {
            audio_on = 1;
            if (logfp)
                fprintf(logfp, "audio: %d Hz on '%s'\n",
                        (int)av.timing.sample_rate, oq_audio_device());
        } else if (logfp) {
            fprintf(logfp, "audio: disabled (%s)\n", oq_audio_error());
            fflush(logfp);
        }
    }

    return 0;
}

void oq_retro_run(void)
{
    retro_run();
}

double oq_retro_fps(void)
{
    return core_fps;
}

void oq_retro_key(unsigned keycode, int down, uint16_t mods)
{
    int slot = arrow_slot(keycode);

    if (slot >= 0)
        arrow_down[slot] = down ? 1 : 0;
    if (kbd_cb)
        kbd_cb(down ? true : false, keycode, 0, mods);
}

void oq_retro_shutdown(void)
{
    retro_unload_game();
    retro_deinit();
    if (audio_on) {
        if (logfp)
            fprintf(logfp, "audio: %lu frames dropped\n", oq_audio_dropped());
        oq_audio_shutdown();
        audio_on = 0;
    }
    free(rgb_buf);
    rgb_buf = NULL;
    rgb_cap = 0;
    if (logfp) {
        fclose(logfp);
        logfp = NULL;
    }
}
