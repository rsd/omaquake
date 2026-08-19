/* Terminal setup: raw mode, alternate screen, and -- the part that actually
 * matters for a game -- negotiating key-release events.
 *
 * A plain tty delivers key PRESSES only.  There is no way to know the player
 * let go of W, so "hold to walk" is impossible and you are left synthesising
 * releases from a timer, which feels awful.  The kitty keyboard protocol
 * fixes this properly: flag 0b10 makes the terminal report event types
 * (press / repeat / release).  We probe for it and fall back if absent.
 */
#include "oq_term.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* disambiguate(1) | report event types(2) | report all keys as escapes(8) */
#define KITTY_KBD_FLAGS 11

static struct termios saved_termios;
static int termios_saved;
static int alt_screen;
static int kbd_pushed;
static int has_key_release;
static int mouse_on;
static int text_px_w, text_px_h;
static int last_cols, last_rows;

volatile int oq_term_quit_requested;

static void on_signal(int sig)
{
    (void)sig;
    oq_term_quit_requested = 1;
}

static void write_all_n(const char *s, size_t len)
{
    ssize_t n;

    while (len) {
        n = write(STDOUT_FILENO, s, len);
        if (n <= 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        s += n;
        len -= (size_t)n;
    }
}

static void write_all(const char *s)
{
    write_all_n(s, strlen(s));
}

void oq_term_clear(void)
{
    write_all("\033[2J");
}

void oq_term_present_at(const char *s, size_t len, int row, int col)
{
    static char *out;
    static size_t cap;
    size_t need = len * 2 + 64 + (size_t)len / 8 * 16;
    size_t i, o = 0;
    int line = row;

    if (col <= 1 && row <= 1) {
        oq_term_present(s, len);
        return;
    }
    if (cap < need) {
        char *p = realloc(out, need);

        if (!p)
            return;
        out = p;
        cap = need;
    }

    o += (size_t)snprintf(out + o, cap - o, "\033[%d;%dH", line, col);
    for (i = 0; i < len; i++) {
        if (s[i] == '\n') {
            line++;
            o += (size_t)snprintf(out + o, cap - o, "\033[%d;%dH", line, col);
            continue;
        }
        out[o++] = s[i];
    }
    write_all_n(out, o);
}

void oq_term_present(const char *s, size_t len)
{
    static char *out;
    static size_t cap;
    size_t need = len * 2 + 8;
    size_t i, o = 0;

    if (cap < need) {
        char *p = realloc(out, need);

        if (!p)
            return;
        out = p;
        cap = need;
    }

    out[o++] = '\033';
    out[o++] = '[';
    out[o++] = 'H';
    for (i = 0; i < len; i++) {
        if (s[i] == '\n')
            out[o++] = '\r';
        out[o++] = s[i];
    }
    write_all_n(out, o);
}

/* Send a query and collect the terminal's reply into buf (NUL-terminated).
 *
 * A Primary Device Attributes request is appended as a sentinel: every
 * terminal answers DA, so once the DA reply arrives we know the reply we
 * actually wanted is either already in hand or never coming. */
static void probe(const char *req, char *buf, size_t cap)
{
    size_t used = 0;

    write_all(req);
    write_all("\033[c");

    for (;;) {
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        ssize_t n;

        if (poll(&pfd, 1, 200) <= 0)
            break;
        n = read(STDIN_FILENO, buf + used, cap - 1 - used);
        if (n <= 0)
            break;
        used += (size_t)n;
        if (memchr(buf, 'c', used))
            break;
        if (used >= cap - 1)
            break;
    }
    buf[used] = '\0';
}

/* Query the terminal's current kitty-keyboard flags.  Returns the flag word,
 * or -1 if the terminal did not answer (protocol unsupported). */
static int query_kitty_flags(void)
{
    char buf[256];
    const char *p;

    probe("\033[?u", buf, sizeof(buf));

    /* A flags reply is CSI ? <digits> u.  The DA reply also starts CSI ?
     * but ends in 'c', so require the 'u' terminator. */
    p = buf;
    while ((p = strstr(p, "\033[?")) != NULL) {
        const char *q = p + 3;
        int value = 0;

        while (*q >= '0' && *q <= '9') {
            value = value * 10 + (*q - '0');
            q++;
        }
        if (*q == 'u')
            return value;
        p = q;
    }
    return -1;
}

/* Ask for the text area in pixels (CSI 14 t -> CSI 4 ; height ; width t).
 *
 * This is what makes pointer look possible: SGR-pixel mouse reports are in
 * exactly this coordinate space (it excludes the window padding), so the
 * reply gives us the centre to measure deltas against and the edges to
 * place the steering bands at. */
static void query_text_pixels(void)
{
    char buf[256];
    const char *p = buf;

    probe("\033[14t", buf, sizeof(buf));

    while ((p = strstr(p, "\033[4;")) != NULL) {
        int h = 0, w = 0;

        if (sscanf(p + 4, "%d;%dt", &h, &w) == 2 && w > 0 && h > 0) {
            text_px_w = w;
            text_px_h = h;
            return;
        }
        p += 4;
    }
}

int oq_term_text_pixels(int *w, int *h)
{
    if (text_px_w <= 0 || text_px_h <= 0)
        return -1;
    *w = text_px_w;
    *h = text_px_h;
    return 0;
}

void oq_term_mouse_enable(void)
{
    if (mouse_on)
        return;
    /* 1003 reports every pointer motion, not just drags, so looking around
     * needs no button held.  1016 is not a nicety: motion reports are
     * coalesced to one per cell crossing in every other encoding, which is
     * an aiming resolution of one character -- and it is only in SGR-pixel
     * mode that the terminal skips that coalescing and reports pixels. */
    write_all("\033[?1003h"        /* report every motion, not just drags */
              "\033[?1016h"        /* ...in pixels, uncoalesced           */
              "\033[?1004h");      /* tell us when we lose focus          */
    mouse_on = 1;
}

void oq_term_mouse_disable(void)
{
    if (!mouse_on)
        return;
    write_all("\033[?1004l\033[?1016l\033[?1003l");
    mouse_on = 0;
}

int oq_term_init(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "omaquake: stdin/stdout must be a terminal\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &saved_termios) < 0)
        return -1;
    termios_saved = 1;

    raw = saved_termios;
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
        return -1;

    atexit(oq_term_shutdown);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGWINCH, SIG_IGN);   /* we poll TIOCGWINSZ instead */

    write_all("\033[?1049h"     /* alternate screen         */
              "\033[?25l"       /* hide cursor              */
              "\033[?7l"        /* no autowrap: a glyph in  */
                                 /* the last column must not */
                                 /* wrap and scroll          */
              "\033[2J");       /* clear                    */
    alt_screen = 1;

    /* The keyboard-mode stack is PER SCREEN, so this must happen after the
     * switch to the alternate screen -- pushing first looks like it works
     * and is then silently discarded by \033[?1049h, leaving us believing
     * we have key releases while the terminal sends legacy bytes.  The
     * engine then sees presses that never end and every key sticks down. */
    if (query_kitty_flags() >= 0) {
        char seq[32];
        int got;

        snprintf(seq, sizeof(seq), "\033[>%du", KITTY_KBD_FLAGS);
        write_all(seq);
        kbd_pushed = 1;

        /* Verify rather than assume: we need bit 1 (report event types) or
         * there are no releases and the fallback is the honest choice. */
        got = query_kitty_flags();
        has_key_release = (got >= 0 && (got & 2));
        if (!has_key_release) {
            write_all("\033[<u");
            kbd_pushed = 0;
        }
    }

    /* Probe now, while stdin is still ours: once the game loop starts the
     * input parser owns it and a reply arriving mid-game would be decoded
     * as keystrokes. */
    query_text_pixels();
    return 0;
}

void oq_term_shutdown(void)
{
    oq_term_mouse_disable();
    if (kbd_pushed) {
        write_all("\033[<u");   /* pop keyboard flags */
        kbd_pushed = 0;
    }
    if (alt_screen) {
        write_all("\033[?7h\033[?25h\033[0m\033[?1049l");
        alt_screen = 0;
    }
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
        termios_saved = 0;
    }
}

int oq_term_size(int *cols, int *rows)
{
    struct winsize ws;
    int changed;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || !ws.ws_col || !ws.ws_row) {
        ws.ws_col = 80;
        ws.ws_row = 24;
    }
    changed = (ws.ws_col != last_cols || ws.ws_row != last_rows);
    last_cols = ws.ws_col;
    last_rows = ws.ws_row;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return changed;
}

int oq_term_has_key_release(void)
{
    return has_key_release;
}
