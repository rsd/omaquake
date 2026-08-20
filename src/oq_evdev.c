#define _GNU_SOURCE   /* strcasestr */
/* Raw pointer input from Linux evdev.  See oq_evdev.h for why it exists. */
#include "oq_evdev.h"

#include "oq_input.h"       /* OQ_MB_* button numbering */

#ifdef __linux__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EVDEV_DIR "/dev/input"

/* struct dirent::d_name is 256 bytes on Linux, and a path that would not
 * fit has to be built anyway rather than silently truncated into a
 * different device's name. */
#define PATH_CAP (sizeof(EVDEV_DIR) + 1 + 256)

/* A mouse at 1000 Hz sending x, y and SYN produces ~3 events per
 * millisecond, so a 16 ms frame owes us well under 64.  The cap only exists
 * so that a device stuck emitting cannot hold the engine loop inside this
 * function: dropping input is recoverable, stalling the frame is not. */
#define MAX_READS 32

#define LONG_BITS (8 * (int)sizeof(unsigned long))
#define NLONGS(n) (((n) + LONG_BITS - 1) / LONG_BITS)

static int   dev_fd = -1;
static int   grabbed;
static char  dev_path[PATH_CAP];
static char  dev_name[128];
static char  errbuf[PATH_CAP + 256];

static int bit_set(const unsigned long *bits, int b)
{
    return (bits[b / LONG_BITS] >> (b % LONG_BITS)) & 1UL;
}

const char *oq_evdev_verdict_str(oq_evdev_verdict v)
{
    switch (v) {
    case OQ_EVDEV_POINTER:     return "pointer";
    case OQ_EVDEV_KEYBOARD:    return "keyboard - never opened";
    case OQ_EVDEV_KBD_POINTER: return "keyboard's pointer - rejected";
    case OQ_EVDEV_NOT_POINTER: return "not a pointer";
    case OQ_EVDEV_UNREADABLE:  return "unreadable";
    }
    return "?";
}

static oq_evdev_verdict classify_fd(int f, char *name, size_t cap)
{
    unsigned long evbit[NLONGS(EV_MAX + 1)];
    unsigned long keybit[NLONGS(KEY_MAX + 1)];
    unsigned long relbit[NLONGS(REL_MAX + 1)];

    memset(evbit, 0, sizeof(evbit));
    memset(keybit, 0, sizeof(keybit));
    memset(relbit, 0, sizeof(relbit));

    if (name && cap) {
        if (ioctl(f, EVIOCGNAME(cap), name) < 0)
            snprintf(name, cap, "?");
        name[cap - 1] = '\0';
    }
    if (ioctl(f, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0)
        return OQ_EVDEV_UNREADABLE;

    if (bit_set(evbit, EV_KEY) &&
        ioctl(f, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0)
        return OQ_EVDEV_UNREADABLE;

    /* The keyboard test comes FIRST, and rejects outright rather than
     * falling through to the pointer test, because reading an event device
     * hands us every key the hardware sends: opening a keyboard here would
     * make this program a keylogger, whatever else the device can also do.
     * It is not hypothetical -- the mouse on the machine this was written
     * on exposes a second node, "E-Signal USB Gaming Mouse Keyboard", that
     * carries the full KEY_A..KEY_Z bitmap.  A composite device that really
     * does have both is the user's cue to name the pointer-only node with
     * --mouse-dev, not ours to guess at. */
    if (bit_set(keybit, KEY_A) || bit_set(keybit, KEY_Z) ||
        bit_set(keybit, KEY_ENTER))
        return OQ_EVDEV_KEYBOARD;

    if (!bit_set(evbit, EV_REL) || !bit_set(evbit, EV_KEY))
        return OQ_EVDEV_NOT_POINTER;
    if (ioctl(f, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit) < 0)
        return OQ_EVDEV_UNREADABLE;
    /* Both axes and a left button: a wheel-only or button-only node (this
     * mouse has one of each) is not something we can aim with. */
    if (!bit_set(relbit, REL_X) || !bit_set(relbit, REL_Y) ||
        !bit_set(keybit, BTN_LEFT))
        return OQ_EVDEV_NOT_POINTER;

    return OQ_EVDEV_POINTER;
}

/* Does the device report a horizontal wheel?  A keyboard's built-in pointer
 * interface generally does not; a real mouse usually does.  Weak on its own,
 * useful as a tie-breaker. */
static int has_hwheel(int f)
{
    unsigned long relbit[NLONGS(REL_MAX + 1)];

    memset(relbit, 0, sizeof(relbit));
    if (ioctl(f, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit) < 0)
        return 0;
    return bit_set(relbit, REL_HWHEEL) != 0;
}

/* Is this pointer really a keyboard's built-in pointer interface?
 *
 * A keyboard can expose a completely genuine one: a Razer BlackWidow presents
 * it on if02, udev names it -event-mouse and sets ID_INPUT_MOUSE=1, and it
 * advertises the same five buttons as a real mouse. No capability test can
 * separate them, so picking the first qualifying device lands on the keyboard
 * and aiming is dead.
 *
 * What does separate them is the name. A keyboard's pointer node carries the
 * keyboard's name EXACTLY, because it is the same physical device:
 *
 *     event7   keyboard   "Razer Razer BlackWidow X Chroma"
 *     event10  pointer    "Razer Razer BlackWidow X Chroma"   <- same
 *
 * whereas a mouse's companion key node is suffixed, so the names differ:
 *
 *     event11  pointer    "SINOWEALTH Game Mouse"
 *     event12  keyboard   "SINOWEALTH Game Mouse Keyboard"    <- differs
 *
 * Rejecting on an exact match is generic: it needs no vendor list and no
 * maintenance, and it catches every keyboard that does this rather than the
 * one we happened to hit. */
static int is_kbd_pointer(const char *name,
                          char kbd_names[][OQ_EVDEV_NAME_CAP], int nkbd)
{
    int i;

    for (i = 0; i < nkbd; i++)
        if (!strcmp(name, kbd_names[i]))
            return 1;
    return 0;
}

/* Rank one pointer candidate.  Higher wins; ties go to the lower event
 * number.
 *
 * The problem this solves: a keyboard can expose a perfectly genuine pointer
 * interface.  A Razer BlackWidow presents one on if02 -- udev even names it
 * -event-mouse, sets ID_INPUT_MOUSE=1, and it advertises the same five
 * buttons as a real mouse -- so capability tests alone cannot separate them
 * and picking the first match lands on the keyboard.
 *
 * Three signals, none decisive alone:
 *   - the device name says "mouse"; a keyboard's pointer node carries the
 *     keyboard's name.
 *   - a real mouse tends to have a horizontal wheel.
 *   - a keyboard's pointer node usually shares its name EXACTLY with that
 *     keyboard's key node, whereas a mouse's companion key node is suffixed
 *     ("SINOWEALTH Game Mouse" vs "SINOWEALTH Game Mouse Keyboard").
 */
static int score_pointer(int f, const char *name)
{
    int score = 0;

    if (strcasestr(name, "mouse") || strcasestr(name, "trackball"))
        score += 100;
    if (has_hwheel(f))
        score += 10;
    return score;
}

/* Names of every keyboard on the system, for is_kbd_pointer(). */
static int collect_kbd_names(struct dirent **list, int n,
                             char names[][OQ_EVDEV_NAME_CAP])
{
    int i, nkbd = 0;

    for (i = 0; i < n && nkbd < OQ_EVDEV_MAX_KBD; i++) {
        char cand[PATH_CAP], name[OQ_EVDEV_NAME_CAP];

        snprintf(cand, sizeof(cand), "%s/%s", EVDEV_DIR, list[i]->d_name);
        if (oq_evdev_classify(cand, name, sizeof(name)) == OQ_EVDEV_KEYBOARD)
            snprintf(names[nkbd++], OQ_EVDEV_NAME_CAP, "%s", name);
    }
    return nkbd;
}

oq_evdev_verdict oq_evdev_classify(const char *path, char *name, size_t cap)
{
    oq_evdev_verdict v;
    int f;

    if (name && cap)
        name[0] = '\0';
    /* Opening is not reading.  The capability bitmaps are only reachable
     * through ioctls on an open descriptor, so a device has to be opened to
     * be judged -- but nothing read()s it until it has been judged a
     * pointer, and this descriptor is closed either way. */
    f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (f < 0)
        return OQ_EVDEV_UNREADABLE;
    v = classify_fd(f, name, cap);
    close(f);
    return v;
}

static int is_event_node(const struct dirent *d)
{
    const char *p = d->d_name;

    if (strncmp(p, "event", 5))
        return 0;
    for (p += 5; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return p != d->d_name + 5;
}

/* event2 must not sort after event10: the first qualifying device wins, so
 * the order decides which mouse a two-mouse machine gets. */
static int by_number(const struct dirent **a, const struct dirent **b)
{
    return atoi((*a)->d_name + 5) - atoi((*b)->d_name + 5);
}

static int scan(struct dirent ***list)
{
    int n = scandir(EVDEV_DIR, list, is_event_node, by_number);

    if (n < 0)
        snprintf(errbuf, sizeof(errbuf), "%s: %s", EVDEV_DIR, strerror(errno));
    return n;
}

void oq_evdev_list(FILE *out)
{
    struct dirent **list;
    int n = scan(&list), i;

    if (n < 0) {
        fprintf(out, "%s\n", errbuf);
        return;
    }
    {
        char kbd_names[OQ_EVDEV_MAX_KBD][OQ_EVDEV_NAME_CAP];
        int nkbd = collect_kbd_names(list, n, kbd_names);

        for (i = 0; i < n; i++) {
            char path[PATH_CAP], name[OQ_EVDEV_NAME_CAP];
            oq_evdev_verdict v;

            snprintf(path, sizeof(path), "%s/%s", EVDEV_DIR, list[i]->d_name);
            v = oq_evdev_classify(path, name, sizeof(name));
            if (v == OQ_EVDEV_POINTER && is_kbd_pointer(name, kbd_names, nkbd))
                v = OQ_EVDEV_KBD_POINTER;
            fprintf(out, "%-22s %-30s %s\n", path, oq_evdev_verdict_str(v),
                    name[0] ? name : "(no name)");
            free(list[i]);
        }
    }
    free(list);
}

static int adopt(const char *path)
{
    char name[sizeof(dev_name)];
    oq_evdev_verdict v;
    int f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

    if (f < 0) {
        snprintf(errbuf, sizeof(errbuf), "%s: %s", path, strerror(errno));
        return -1;
    }
    v = classify_fd(f, name, sizeof(name));
    if (v != OQ_EVDEV_POINTER) {
        /* Same rule for a device the user named as for one we found: an
         * explicit --mouse-dev is not consent to read a keyboard. */
        snprintf(errbuf, sizeof(errbuf), "%s (%s): %s", path,
                 name[0] ? name : "?", oq_evdev_verdict_str(v));
        close(f);
        return -1;
    }
    dev_fd = f;
    snprintf(dev_path, sizeof(dev_path), "%s", path);
    snprintf(dev_name, sizeof(dev_name), "%s", name);
    return 0;
}

int oq_evdev_open(const char *path)
{
    struct dirent **list;
    int n, i;

    if (dev_fd >= 0)
        return 0;
    errbuf[0] = '\0';

    if (path && *path)
        return adopt(path);

    n = scan(&list);
    if (n < 0)
        return -1;

    {
        char kbd_names[OQ_EVDEV_MAX_KBD][OQ_EVDEV_NAME_CAP];
        char best_path[PATH_CAP];
        int nkbd = 0, best = 0, best_score = 0;

        /* First pass: every keyboard's name, so a pointer sharing one can be
         * recognised as that keyboard's own pointer interface and skipped. */
        nkbd = collect_kbd_names(list, n, kbd_names);

        /* Second pass: score the pointers and keep the best. */
        best_path[0] = '\0';
        for (i = 0; i < n; i++) {
            char cand[PATH_CAP], name[OQ_EVDEV_NAME_CAP];
            int f, sc;

            snprintf(cand, sizeof(cand), "%s/%s", EVDEV_DIR, list[i]->d_name);
            if (oq_evdev_classify(cand, name, sizeof(name)) != OQ_EVDEV_POINTER)
                continue;
            if (is_kbd_pointer(name, kbd_names, nkbd))
                continue;
            f = open(cand, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (f < 0)
                continue;
            sc = score_pointer(f, name);
            close(f);
            if (!best || sc > best_score) {
                best_score = sc;
                best = 1;
                snprintf(best_path, sizeof(best_path), "%s", cand);
            }
        }

        for (i = 0; i < n; i++)
            free(list[i]);
        free(list);

        if (!best) {
            snprintf(errbuf, sizeof(errbuf),
                     "no usable pointer among %d %s/event* devices",
                     n, EVDEV_DIR);
            return -1;
        }
        return adopt(best_path);
    }
}

int oq_evdev_grab(void)
{
    if (dev_fd < 0) {
        snprintf(errbuf, sizeof(errbuf), "no device open");
        return -1;
    }
    if (grabbed)
        return 0;
    if (ioctl(dev_fd, EVIOCGRAB, 1) < 0) {
        snprintf(errbuf, sizeof(errbuf), "EVIOCGRAB %s: %s", dev_path,
                 strerror(errno));
        return -1;
    }
    grabbed = 1;
    return 0;
}

void oq_evdev_ungrab(void)
{
    if (dev_fd >= 0 && grabbed)
        ioctl(dev_fd, EVIOCGRAB, 0);
    grabbed = 0;
}

void oq_evdev_close(void)
{
    /* Ungrab explicitly even though close() would do it: this runs from
     * oq_term_shutdown(), and leaving the user's mouse dead is the one
     * failure here that outlives the process. */
    oq_evdev_ungrab();
    if (dev_fd >= 0)
        close(dev_fd);
    dev_fd = -1;
    dev_path[0] = '\0';
    dev_name[0] = '\0';
}

int oq_evdev_is_open(void)
{
    return dev_fd >= 0;
}

const char *oq_evdev_path(void)
{
    return dev_path[0] ? dev_path : NULL;
}

const char *oq_evdev_name(void)
{
    return dev_name;
}

const char *oq_evdev_error(void)
{
    return errbuf[0] ? errbuf : "no error";
}

static void handle(const struct input_event *ev, int *dx, int *dy,
                   oq_evdev_button_fn fn, void *ud)
{
    if (ev->type == EV_REL) {
        switch (ev->code) {
        case REL_X: *dx += ev->value; break;
        case REL_Y: *dy += ev->value; break;
        /* A wheel notch has no release; oq_retro latches it and clears it
         * on the read, exactly as for the terminal source. */
        case REL_WHEEL:
            if (fn && ev->value)
                fn(ev->value > 0 ? OQ_MB_WHEEL_UP : OQ_MB_WHEEL_DOWN, 1, ud);
            break;
        case REL_HWHEEL:
            if (fn && ev->value)
                fn(ev->value > 0 ? OQ_MB_WHEEL_RIGHT : OQ_MB_WHEEL_LEFT, 1, ud);
            break;
        }
        return;
    }
    if (ev->type == EV_KEY && fn) {
        int btn = -1;

        switch (ev->code) {
        case BTN_LEFT:   btn = OQ_MB_LEFT;   break;
        case BTN_RIGHT:  btn = OQ_MB_RIGHT;  break;
        case BTN_MIDDLE: btn = OQ_MB_MIDDLE; break;
        }
        /* value 2 is auto-repeat, which a button never sends and which the
         * engine tracks itself anyway. */
        if (btn >= 0 && ev->value != 2)
            fn(btn, ev->value ? 1 : 0, ud);
    }
}

void oq_evdev_poll(int *dx, int *dy, oq_evdev_button_fn fn, void *ud)
{
    struct input_event evs[64];
    int reads;

    *dx = *dy = 0;
    for (reads = 0; dev_fd >= 0 && reads < MAX_READS; reads++) {
        ssize_t n = read(dev_fd, evs, sizeof(evs));
        size_t i;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            /* ENODEV: unplugged mid-game.  Close rather than spin on the
             * error every frame; aiming falls back to the keyboard, which
             * is survivable, and the grab goes with it. */
            snprintf(errbuf, sizeof(errbuf), "read %s: %s", dev_path,
                     strerror(errno));
            oq_evdev_close();
            break;
        }
        for (i = 0; i < (size_t)n / sizeof(evs[0]); i++)
            handle(&evs[i], dx, dy, fn, ud);
        if ((size_t)n < sizeof(evs))
            break;
    }
}

#else /* !__linux__ */

/* evdev is a Linux interface; elsewhere this stubs out the same way
 * oq_audio.c does without ALSA, and --mouse=auto lands on the terminal. */

const char *oq_evdev_verdict_str(oq_evdev_verdict v)
{
    (void)v;
    return "unsupported";
}

oq_evdev_verdict oq_evdev_classify(const char *path, char *name, size_t cap)
{
    (void)path;
    if (name && cap)
        name[0] = '\0';
    return OQ_EVDEV_UNREADABLE;
}

void oq_evdev_list(FILE *out)
{
    fprintf(out, "evdev is only available on Linux\n");
}

int  oq_evdev_open(const char *path) { (void)path; return -1; }
int  oq_evdev_grab(void)             { return -1; }
void oq_evdev_ungrab(void)           { }
void oq_evdev_close(void)            { }
int  oq_evdev_is_open(void)          { return 0; }
const char *oq_evdev_path(void)      { return NULL; }
const char *oq_evdev_name(void)      { return ""; }
const char *oq_evdev_error(void)     { return "evdev is only available on Linux"; }

void oq_evdev_poll(int *dx, int *dy, oq_evdev_button_fn fn, void *ud)
{
    (void)fn; (void)ud;
    *dx = *dy = 0;
}

#endif /* __linux__ */
