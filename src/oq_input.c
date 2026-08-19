/* Terminal keyboard -> libretro RETROK_* events.
 *
 * Two paths:
 *
 *  - kitty keyboard protocol: every key arrives as CSI ... u carrying an
 *    event type (1 press, 2 repeat, 3 release), so we get true key-up and
 *    "hold W to walk forward" works properly.
 *
 *  - legacy tty: presses only.  We emit a press and arm a timer; the
 *    terminal's own auto-repeat keeps re-arming it while the key is held,
 *    and the release fires once the repeats stop.  This is the best a plain
 *    terminal can do, and it feels like it.
 */
#include "oq_input.h"

#include <libretro.h>

#include <stdio.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Must comfortably exceed the terminal's auto-repeat interval (~30ms) so a
 * held key is not released between repeats. */
#define HOLD_MS 260
#define MAX_HOLDS 16

static int kitty_mode;
static FILE *trace;
static int64_t esc_since;
static int quit_requested;
static unsigned char buf[1024];
static size_t buflen;

struct hold {
    unsigned keycode;
    int64_t deadline_ms;
};
static struct hold holds[MAX_HOLDS];

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void oq_input_set_trace(FILE *fp)
{
    trace = fp;
}

static const char *key_name(unsigned kc)
{
    static char buf[32];

    switch (kc) {
    case RETROK_UP:     return "UP";
    case RETROK_DOWN:   return "DOWN";
    case RETROK_LEFT:   return "LEFT";
    case RETROK_RIGHT:  return "RIGHT";
    case RETROK_RETURN: return "ENTER";
    case RETROK_ESCAPE: return "ESCAPE";
    case RETROK_SPACE:  return "SPACE";
    case RETROK_TAB:    return "TAB";
    case RETROK_LCTRL:  return "LCTRL";
    case RETROK_LSHIFT: return "LSHIFT";
    case RETROK_LALT:   return "LALT";
    }
    if (kc >= RETROK_a && kc <= RETROK_z) {
        snprintf(buf, sizeof(buf), "%c", 'a' + (int)(kc - RETROK_a));
        return buf;
    }
    snprintf(buf, sizeof(buf), "keycode:%u", kc);
    return buf;
}

void oq_input_init(int has_key_release)
{
    kitty_mode = has_key_release;
    buflen = 0;
    quit_requested = 0;
    memset(holds, 0, sizeof(holds));
}

int oq_input_quit(void)
{
    return quit_requested;
}

/* ---- keycode mapping ----------------------------------------------- */

static unsigned map_unicode(unsigned cp)
{
    if (cp >= 'A' && cp <= 'Z')
        return RETROK_a + (cp - 'A');
    if (cp >= 'a' && cp <= 'z')
        return RETROK_a + (cp - 'a');
    if (cp >= '0' && cp <= '9')
        return RETROK_0 + (cp - '0');

    switch (cp) {
    case 27:  return RETROK_ESCAPE;
    case 13:  return RETROK_RETURN;
    case 9:   return RETROK_TAB;
    case 127: return RETROK_BACKSPACE;
    case 32:  return RETROK_SPACE;
    case 96:  return RETROK_BACKQUOTE;
    /* kitty's private range for modifier keys */
    case 57441: return RETROK_LSHIFT;
    case 57442: return RETROK_LCTRL;
    case 57443: return RETROK_LALT;
    case 57447: return RETROK_RSHIFT;
    case 57448: return RETROK_RCTRL;
    case 57449: return RETROK_RALT;
    }
    /* Remaining printable ASCII maps 1:1 onto RETROK_*. */
    if (cp < 127)
        return cp;
    return RETROK_UNKNOWN;
}

static unsigned map_final(int final)
{
    switch (final) {
    case 'A': return RETROK_UP;
    case 'B': return RETROK_DOWN;
    case 'C': return RETROK_RIGHT;
    case 'D': return RETROK_LEFT;
    case 'H': return RETROK_HOME;
    case 'F': return RETROK_END;
    case 'P': return RETROK_F1;
    case 'Q': return RETROK_F2;
    case 'R': return RETROK_F3;
    case 'S': return RETROK_F4;
    }
    return RETROK_UNKNOWN;
}

static unsigned map_tilde(unsigned n)
{
    switch (n) {
    case 1:  return RETROK_HOME;
    case 2:  return RETROK_INSERT;
    case 3:  return RETROK_DELETE;
    case 4:  return RETROK_END;
    case 5:  return RETROK_PAGEUP;
    case 6:  return RETROK_PAGEDOWN;
    case 15: return RETROK_F5;
    case 17: return RETROK_F6;
    case 18: return RETROK_F7;
    case 19: return RETROK_F8;
    case 20: return RETROK_F9;
    case 21: return RETROK_F10;
    case 23: return RETROK_F11;
    case 24: return RETROK_F12;
    }
    return RETROK_UNKNOWN;
}

/* libretro modifier bits from a kitty modifier field (which is 1-based). */
static uint16_t map_mods(unsigned m)
{
    uint16_t out = 0;

    if (!m)
        return 0;
    m -= 1;
    if (m & 1) out |= RETROKMOD_SHIFT;
    if (m & 2) out |= RETROKMOD_ALT;
    if (m & 4) out |= RETROKMOD_CTRL;
    if (m & 8) out |= RETROKMOD_META;
    return out;
}

/* ---- synthesised holds (legacy path) -------------------------------- */

static void hold_press(unsigned keycode, uint16_t mods, oq_key_fn fn, void *ud)
{
    int i, free_slot = -1;
    int64_t deadline = now_ms() + HOLD_MS;

    for (i = 0; i < MAX_HOLDS; i++) {
        if (holds[i].keycode == keycode) {
            holds[i].deadline_ms = deadline;   /* auto-repeat: extend */
            return;
        }
        if (!holds[i].keycode && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0) {
        holds[free_slot].keycode = keycode;
        holds[free_slot].deadline_ms = deadline;
    }
    fn(keycode, 1, mods, ud);
}

void oq_input_expire(oq_key_fn fn, void *ud)
{
    int64_t t = now_ms();
    int i;

    if (kitty_mode)
        return;
    for (i = 0; i < MAX_HOLDS; i++) {
        if (holds[i].keycode && t >= holds[i].deadline_ms) {
            fn(holds[i].keycode, 0, 0, ud);
            holds[i].keycode = 0;
        }
    }
}

static void emit(unsigned keycode, int down, uint16_t mods,
                 oq_key_fn fn, void *ud)
{
    if (trace)
        fprintf(trace, "  -> %-12s %-5s mods=0x%04x\r\n",
                key_name(keycode), down ? "DOWN" : "UP", mods);
    if (keycode == RETROK_UNKNOWN)
        return;

    /* Quit has to be recognised from the DECODED event, not from a raw
     * control byte.  Under the kitty protocol every key arrives as an escape
     * sequence -- Ctrl-\ comes through as CSI 92;5u and the 0x1c byte never
     * appears -- so a raw-byte test silently stops working the moment the
     * protocol is active. */
    if (down && (keycode == RETROK_F10 ||
                 ((mods & RETROKMOD_CTRL) &&
                  (keycode == RETROK_BACKSLASH || keycode == RETROK_q)))) {
        quit_requested = 1;
        return;
    }
    if (kitty_mode)
        fn(keycode, down, mods, ud);
    else if (down)
        hold_press(keycode, mods, fn, ud);
}

/* ---- escape sequence parsing ---------------------------------------- */

/* Parse one CSI sequence starting at buf[i] (which is ESC).  Returns bytes
 * consumed, or 0 if the sequence has not fully arrived yet. */
static size_t parse_csi(size_t i, oq_key_fn fn, void *ud)
{
    size_t j = i + 2;               /* skip ESC [ */
    unsigned params[8] = {0};
    unsigned subs[8] = {0};
    int nparams = 0;
    int in_sub = 0;
    int final;

    while (j < buflen) {
        unsigned char c = buf[j];

        if (c >= '0' && c <= '9') {
            if (nparams < 8) {
                if (in_sub)
                    subs[nparams] = subs[nparams] * 10 + (c - '0');
                else
                    params[nparams] = params[nparams] * 10 + (c - '0');
            }
            j++;
        } else if (c == ':') {
            in_sub = 1;
            j++;
        } else if (c == ';') {
            if (nparams < 7)
                nparams++;
            in_sub = 0;
            j++;
        } else if (c >= 0x40 && c <= 0x7e) {
            goto have_final;
        } else {
            j++;                    /* private markers such as '?' and '>' */
        }
    }
    return 0;                       /* incomplete */

have_final:
    final = buf[j];

    {
        /* The event type is a sub-parameter of the modifier field:
         * CSI key ; mods : event u.  Absent means press. */
        unsigned ev = subs[1] ? subs[1] : 1;
        uint16_t mods = map_mods(params[1]);
        int down = (ev != 3);
        unsigned kc;

        if (final == 'u')
            kc = map_unicode(params[0]);
        else if (final == '~')
            kc = map_tilde(params[0]);
        else
            kc = map_final(final);

        /* Repeats would re-assert a press the engine already holds; it
         * tracks its own repeat state. */
        if (ev != 2)
            emit(kc, down, mods, fn, ud);
    }
    return j - i + 1;
}

void oq_input_poll(oq_key_fn fn, void *ud)
{
    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
    size_t i;

    while (buflen < sizeof(buf) && poll(&pfd, 1, 0) > 0 &&
           (pfd.revents & POLLIN)) {
        ssize_t n = read(STDIN_FILENO, buf + buflen, sizeof(buf) - buflen);

        if (n <= 0)
            break;
        buflen += (size_t)n;
    }

    if (trace && buflen) {
        size_t k;

        fprintf(trace, "raw:");
        for (k = 0; k < buflen; k++)
            fprintf(trace, " %02x(%c)", buf[k],
                    buf[k] >= 32 && buf[k] < 127 ? buf[k] : '.');
        fprintf(trace, "\r\n");
        fflush(trace);
    }

    i = 0;
    while (i < buflen) {
        unsigned char c = buf[i];

        if (c == 0x1b && i + 1 < buflen && buf[i + 1] == '[') {
            size_t used = parse_csi(i, fn, ud);

            if (!used)
                break;              /* wait for the rest of the sequence */
            i += used;
            continue;
        }
        if (c == 0x1b && i + 1 >= buflen) {
            /* A bare ESC is nearly always the head of a sequence that has
             * not fully arrived.  Under the kitty protocol Escape itself
             * comes through as CSI 27 u, so a lone ESC is NEVER the Escape
             * key there -- treating it as one fires a spurious menu-back on
             * every split read.
             *
             * But never wait forever: if the terminal turns out not to be
             * reporting in the enhanced format after all, a lone ESC would
             * sit here unconsumed and block every key behind it.  Give the
             * rest of the sequence a grace period, then take it at face
             * value. */
            if (kitty_mode) {
                int64_t t = now_ms();

                if (!esc_since) {
                    esc_since = t;
                    break;
                }
                if (t - esc_since < 50)
                    break;
                esc_since = 0;
                emit(RETROK_ESCAPE, 1, 0, fn, ud);
                emit(RETROK_ESCAPE, 0, 0, fn, ud);
                i++;
                continue;
            }
            emit(RETROK_ESCAPE, 1, 0, fn, ud);
            i++;
            continue;
        }
        if (c == 0x1b && i + 2 < buflen && buf[i + 1] == 'O') {
            /* SS3: what a legacy terminal sends for the arrows and F1-F4
             * when it is in application cursor mode.  Without this the
             * arrows fall through and get misread as Escape. */
            emit(map_final(buf[i + 2]), 1, 0, fn, ud);
            i += 3;
            continue;
        }
        if (c == 0x1b && i + 1 < buflen && buf[i + 1] == 'O')
            break;                  /* incomplete SS3 */
        if (c == 0x1c) {            /* Ctrl-\ as a raw byte: legacy path only */
            quit_requested = 1;
            i++;
            continue;
        }
        if (c < 32 && c != 9 && c != 13 && c != 27) {
            /* Ctrl+letter arrives as the bare control code. */
            emit(RETROK_a + (c - 1), 1, RETROKMOD_CTRL, fn, ud);
            i++;
            continue;
        }
        emit(map_unicode(c), 1, 0, fn, ud);
        i++;
    }

    if (buflen && buf[0] != 0x1b)
        esc_since = 0;

    if (i >= buflen)
        buflen = 0;
    else if (i) {
        memmove(buf, buf + i, buflen - i);
        buflen -= i;
    }
}
