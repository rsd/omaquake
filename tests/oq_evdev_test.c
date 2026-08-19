/* Tests for the evdev pointer detector.
 *
 * The one that matters is the keyboard rejection: get it wrong and OmaQuake
 * reads a keyboard event device, which is a keylogger.  Two checks cover it.
 *
 *  - Against the machine's real devices: nothing that classifies as a
 *    pointer may have KEY_A/KEY_Z/KEY_ENTER in its bitmap, re-read here
 *    independently of the module rather than trusting its own verdict.
 *  - Against a synthetic uinput device that is deliberately BOTH a complete
 *    pointer and a complete keyboard.  A real machine rarely offers one
 *    (the mouse this was written against exposes a keyboard node with no
 *    relative axes, so the pointer test alone would already reject it),
 *    and without one the rejection rule is never actually exercised.
 *
 * The uinput half is skipped, not failed, where /dev/uinput is not
 * writable.  It grabs only the virtual device it created itself: no test
 * here ever grabs the user's real mouse, because a held grab is a frozen
 * desktop pointer.
 */
#include "oq_evdev.h"
#include "oq_input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define LONG_BITS (8 * (int)sizeof(unsigned long))
#define NLONGS(n) (((n) + LONG_BITS - 1) / LONG_BITS)

static int failures;
static int skipped;

static void ok(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        failures++;
}

static void skip(const char *what)
{
    printf("skip %s\n", what);
    skipped++;
}

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };

    nanosleep(&ts, NULL);
}

/* Read the EV_KEY bitmap ourselves, so the check below does not depend on
 * the same code it is checking. */
static int has_keyboard_keys(const char *path, int *readable)
{
    unsigned long keybit[NLONGS(KEY_MAX + 1)];
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    int i, hit = 0;
    static const int keys[] = { KEY_A, KEY_Z, KEY_ENTER };

    *readable = (fd >= 0);
    if (fd < 0)
        return 0;
    memset(keybit, 0, sizeof(keybit));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
        for (i = 0; i < 3; i++)
            hit |= (keybit[keys[i] / LONG_BITS] >> (keys[i] % LONG_BITS)) & 1UL;
    }
    close(fd);
    return hit;
}

/* ---- the machine's own devices --------------------------------------- */

static void test_real_devices(void)
{
    int i, pointers = 0, keyboards = 0;

    for (i = 0; i < 64; i++) {
        char path[64], name[128];
        oq_evdev_verdict v;
        int readable;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (access(path, F_OK))
            continue;
        v = oq_evdev_classify(path, name, sizeof(name));
        if (v == OQ_EVDEV_KEYBOARD)
            keyboards++;
        if (v != OQ_EVDEV_POINTER)
            continue;
        pointers++;
        if (has_keyboard_keys(path, &readable) && readable) {
            printf("     %s (%s) was accepted as a pointer but reports"
                   " keyboard keys\n", path, name);
            failures++;
        }
    }
    printf("     scanned: %d pointer(s), %d keyboard(s) rejected\n",
           pointers, keyboards);
    ok(1, "no accepted pointer reports keyboard keys");
}

/* ---- synthetic devices ------------------------------------------------ */

struct uidev {
    int  fd;
    /* /dev/input/eventN, sized for the longest name readdir can hand back
     * rather than the longest one that makes sense. */
    char path[sizeof("/dev/input/") + 256];
};

/* The event node a uinput device ended up as: ask for its sysfs name and
 * look for the event child underneath it. */
static int find_node(int uifd, char *out, size_t cap)
{
    char sysname[64], dirpath[128];
    struct dirent *de;
    DIR *d;
    int found = 0;

    if (ioctl(uifd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0)
        return -1;
    snprintf(dirpath, sizeof(dirpath), "/sys/class/input/%s", sysname);
    d = opendir(dirpath);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        if (!strncmp(de->d_name, "event", 5)) {
            snprintf(out, cap, "/dev/input/%s", de->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found ? 0 : -1;
}

static int uidev_create(struct uidev *u, const char *name, int keyboard)
{
    struct uinput_user_dev dev;
    int i;

    u->fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (u->fd < 0)
        return -1;

    ioctl(u->fd, UI_SET_EVBIT, EV_REL);
    ioctl(u->fd, UI_SET_RELBIT, REL_X);
    ioctl(u->fd, UI_SET_RELBIT, REL_Y);
    ioctl(u->fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(u->fd, UI_SET_RELBIT, REL_HWHEEL);
    ioctl(u->fd, UI_SET_EVBIT, EV_KEY);
    ioctl(u->fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(u->fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(u->fd, UI_SET_KEYBIT, BTN_MIDDLE);
    if (keyboard) {
        /* A device that is a perfectly good pointer AND a keyboard: the
         * only thing that can reject it is the keyboard rule itself. */
        for (i = KEY_ESC; i <= KEY_SLASH; i++)
            ioctl(u->fd, UI_SET_KEYBIT, i);
    }

    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "%s", name);
    dev.id.bustype = BUS_USB;
    dev.id.vendor = 0x6f71;         /* "oq" */
    dev.id.product = keyboard ? 2 : 1;
    dev.id.version = 1;
    if (write(u->fd, &dev, sizeof(dev)) != (ssize_t)sizeof(dev) ||
        ioctl(u->fd, UI_DEV_CREATE) < 0) {
        close(u->fd);
        u->fd = -1;
        return -1;
    }

    /* udev has to relabel the new node before we can open it. */
    for (i = 0; i < 200; i++) {
        if (find_node(u->fd, u->path, sizeof(u->path)) == 0 &&
            access(u->path, R_OK) == 0)
            return 0;
        msleep(10);
    }
    ioctl(u->fd, UI_DEV_DESTROY);
    close(u->fd);
    u->fd = -1;
    return -1;
}

static void uidev_destroy(struct uidev *u)
{
    if (u->fd >= 0) {
        ioctl(u->fd, UI_DEV_DESTROY);
        close(u->fd);
        u->fd = -1;
    }
}

static void emit(int fd, int type, int code, int value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        printf("     warning: injection write failed: %s\n", strerror(errno));
}

static void syn(int fd)
{
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* What the button callback saw, in order. */
static int  btn_log[32][2];
static int  btn_n;

static void btn_sink(int button, int down, void *ud)
{
    (void)ud;
    if (btn_n < 32) {
        btn_log[btn_n][0] = button;
        btn_log[btn_n][1] = down;
        btn_n++;
    }
}

/* Poll until the deltas stop changing: the kernel delivers asynchronously,
 * so a single poll right after injection can legitimately come back empty. */
static void settle(int *dx, int *dy)
{
    int i;

    *dx = *dy = 0;
    for (i = 0; i < 50; i++) {
        int x, y;

        oq_evdev_poll(&x, &y, btn_sink, NULL);
        *dx += x;
        *dy += y;
        msleep(5);
    }
}

static void test_synthetic_pointer(void)
{
    struct uidev u;
    int dx = 0, dy = 0, fd2;

    if (uidev_create(&u, "OmaQuake test pointer", 0)) {
        skip("synthetic pointer (no writable /dev/uinput?)");
        return;
    }
    ok(oq_evdev_classify(u.path, NULL, 0) == OQ_EVDEV_POINTER,
       "synthetic mouse classifies as a pointer");
    ok(oq_evdev_open(u.path) == 0, "opens the named device");
    ok(oq_evdev_is_open() && !strcmp(oq_evdev_path(), u.path),
       "reports the path it opened");
    ok(!strcmp(oq_evdev_name(), "OmaQuake test pointer"),
       "reports the device name");

    /* Grab before injecting anything, so no synthetic motion can reach the
     * desktop pointer.  Safe to hold here: this device is ours. */
    ok(oq_evdev_grab() == 0, "grabs the device");
    fd2 = open(u.path, O_RDONLY | O_NONBLOCK);
    ok(fd2 >= 0 && ioctl(fd2, EVIOCGRAB, 1) < 0 && errno == EBUSY,
       "the grab is exclusive: a second EVIOCGRAB gets EBUSY");

    btn_n = 0;
    emit(u.fd, EV_REL, REL_X, 7);
    emit(u.fd, EV_REL, REL_Y, -3);
    syn(u.fd);
    emit(u.fd, EV_REL, REL_X, 2);
    emit(u.fd, EV_KEY, BTN_LEFT, 1);
    syn(u.fd);
    emit(u.fd, EV_KEY, BTN_LEFT, 0);
    emit(u.fd, EV_KEY, BTN_RIGHT, 1);
    syn(u.fd);
    emit(u.fd, EV_REL, REL_WHEEL, 1);
    syn(u.fd);
    emit(u.fd, EV_REL, REL_WHEEL, -1);
    syn(u.fd);
    emit(u.fd, EV_REL, REL_HWHEEL, 1);
    syn(u.fd);
    settle(&dx, &dy);

    printf("     deltas: dx=%d dy=%d, %d button event(s)\n", dx, dy, btn_n);
    ok(dx == 9, "REL_X accumulates across polls (7 + 2 = 9)");
    ok(dy == -3, "REL_Y keeps its sign");
    ok(btn_n == 6, "one callback per button transition and per wheel notch");
    ok(btn_n > 0 && btn_log[0][0] == OQ_MB_LEFT && btn_log[0][1] == 1,
       "BTN_LEFT press -> OQ_MB_LEFT down");
    ok(btn_n > 1 && btn_log[1][0] == OQ_MB_LEFT && btn_log[1][1] == 0,
       "BTN_LEFT release -> OQ_MB_LEFT up");
    ok(btn_n > 2 && btn_log[2][0] == OQ_MB_RIGHT && btn_log[2][1] == 1,
       "BTN_RIGHT press -> OQ_MB_RIGHT down");
    ok(btn_n > 3 && btn_log[3][0] == OQ_MB_WHEEL_UP && btn_log[3][1] == 1,
       "REL_WHEEL +1 -> OQ_MB_WHEEL_UP");
    ok(btn_n > 4 && btn_log[4][0] == OQ_MB_WHEEL_DOWN,
       "REL_WHEEL -1 -> OQ_MB_WHEEL_DOWN");
    ok(btn_n > 5 && btn_log[5][0] == OQ_MB_WHEEL_RIGHT,
       "REL_HWHEEL +1 -> OQ_MB_WHEEL_RIGHT");

    oq_evdev_ungrab();
    ok(fd2 >= 0 && ioctl(fd2, EVIOCGRAB, 1) == 0,
       "ungrab releases it: the same EVIOCGRAB now succeeds");
    if (fd2 >= 0) {
        ioctl(fd2, EVIOCGRAB, 0);
        close(fd2);
    }
    oq_evdev_close();
    ok(!oq_evdev_is_open(), "close leaves nothing open");
    uidev_destroy(&u);
}

static void test_synthetic_keyboard(void)
{
    struct uidev u;

    if (uidev_create(&u, "OmaQuake test mouse-keyboard", 1)) {
        skip("synthetic keyboard/pointer hybrid (no writable /dev/uinput?)");
        return;
    }
    ok(oq_evdev_classify(u.path, NULL, 0) == OQ_EVDEV_KEYBOARD,
       "a device that is both a pointer and a keyboard is a KEYBOARD");
    ok(oq_evdev_open(u.path) != 0,
       "--mouse-dev refuses it rather than opening it");
    ok(!oq_evdev_is_open(), "nothing was left open by the refusal");
    printf("     refusal said: %s\n", oq_evdev_error());
    uidev_destroy(&u);
}

/* Auto-detection must never land on the hybrid either, even though it is a
 * complete pointer -- and it must still find a real one if there is one. */
static void test_autodetect_skips_keyboards(void)
{
    struct uidev u;

    if (uidev_create(&u, "OmaQuake test mouse-keyboard", 1)) {
        skip("auto-detection against a hybrid (no writable /dev/uinput?)");
        return;
    }
    if (oq_evdev_open(NULL) == 0) {
        int readable;

        ok(strcmp(oq_evdev_path(), u.path) != 0,
           "auto-detection did not pick the keyboard hybrid");
        ok(!has_keyboard_keys(oq_evdev_path(), &readable),
           "auto-detection picked a device with no keyboard keys");
        printf("     picked: %s (%s)\n", oq_evdev_path(), oq_evdev_name());
        oq_evdev_close();
    } else {
        /* No real pointer of our own to find is still a pass for the rule
         * under test: what must not happen is picking the hybrid. */
        ok(1, "auto-detection found nothing rather than the hybrid");
    }
    uidev_destroy(&u);
}

int main(void)
{
    printf("-- real devices --\n");
    test_real_devices();
    printf("-- synthetic pointer --\n");
    test_synthetic_pointer();
    printf("-- synthetic keyboard/pointer hybrid --\n");
    test_synthetic_keyboard();
    printf("-- auto-detection --\n");
    test_autodetect_skips_keyboards();

    printf("\n%d failure(s), %d skipped\n", failures, skipped);
    return failures ? 1 : 0;
}
