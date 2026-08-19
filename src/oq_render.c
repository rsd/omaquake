#include "oq_render.h"
#include "oq_term.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static const oq_present_backend *backend;
static oq_present_config config;
static int config_dirty;

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int running;
static int have_pending;
static unsigned long dropped;

/* Two buffers: the engine fills `pending` while the renderer reads `front`.
 * They are swapped under the lock, so the renderer never touches memory the
 * engine is writing and neither ever waits on the other. */
static uint8_t *pending, *front;
static size_t buf_cap;
static int pend_w, pend_h, pend_stride;
static int front_w, front_h, front_stride;

static void *render_loop(void *arg)
{
    (void)arg;

    for (;;) {
        oq_present_config cfg;
        int reconf;

        pthread_mutex_lock(&lock);
        while (running && !have_pending)
            pthread_cond_wait(&wake, &lock);
        if (!running && !have_pending) {
            pthread_mutex_unlock(&lock);
            break;
        }
        {
            uint8_t *tmp = front;

            front = pending;
            pending = tmp;
        }
        front_w = pend_w;
        front_h = pend_h;
        front_stride = pend_stride;
        have_pending = 0;
        cfg = config;
        reconf = config_dirty;
        config_dirty = 0;
        pthread_mutex_unlock(&lock);

        if (reconf) {
            /* Clear here, not on the engine thread: all terminal writes must
             * come from one thread or escape sequences interleave. */
            oq_term_clear();
            backend->resize(&cfg);
        }
        backend->frame(front, front_w, front_h, front_stride);
    }
    return NULL;
}

int oq_render_start(const oq_present_backend *be, const oq_present_config *cfg)
{
    backend = be;
    config = *cfg;
    running = 1;
    if (pthread_create(&thread, NULL, render_loop, NULL)) {
        running = 0;
        return -1;
    }
    return 0;
}

void oq_render_reconfigure(const oq_present_config *cfg)
{
    pthread_mutex_lock(&lock);
    config = *cfg;
    config_dirty = 1;
    pthread_mutex_unlock(&lock);
}

void oq_render_submit(const uint8_t *rgb, int w, int h, int stride)
{
    size_t need = (size_t)h * stride;

    pthread_mutex_lock(&lock);
    if (buf_cap < need) {
        uint8_t *a = realloc(pending, need);
        uint8_t *b = realloc(front, need);

        if (a) pending = a;
        if (b) front = b;
        if (!a || !b) {
            pthread_mutex_unlock(&lock);
            return;
        }
        buf_cap = need;
    }
    if (have_pending)
        dropped++;              /* renderer still busy; newest wins */
    memcpy(pending, rgb, need);
    pend_w = w;
    pend_h = h;
    pend_stride = stride;
    have_pending = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

void oq_render_stop(void)
{
    if (!running)
        return;
    pthread_mutex_lock(&lock);
    running = 0;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    free(pending);
    free(front);
    pending = front = NULL;
    buf_cap = 0;
}

unsigned long oq_render_dropped(void)
{
    return dropped;
}
