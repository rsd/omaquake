#include "oq_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATHCAP 1024

static int is_file(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void note(char *tried, size_t cap, const char *p)
{
    size_t len = strlen(tried);

    if (len + strlen(p) + 8 < cap)
        snprintf(tried + len, cap - len, "    %s\n", p);
}

/* The shareware archive ships ID1/PAK0.PAK; a hand-made directory is usually
 * lowercase.  Try both rather than making the player rename anything. */
static int pak_in(const char *dir, char *out, size_t cap)
{
    static const char *names[] = { "pak0.pak", "PAK0.PAK" };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(out, cap, "%s/%s", dir, names[i]);
        if (is_file(out))
            return 1;
    }
    out[0] = '\0';
    return 0;
}

/* The engine only walks up from the pak to find its base directory when the
 * path contains "id1" (see tyrquake's retro_load_game).  A pak somewhere else
 * would load and then fail to find progs.dat, so insist on the id1 layout and
 * say so plainly rather than letting it fail deeper down. */
static int has_id1(const char *path)
{
    const char *p;

    for (p = path; *p; p++)
        if (!strncasecmp(p, "id1", 3))
            return 1;
    return 0;
}

/* dir, or dir/id1 -- accepts both "the id1 directory" and "the directory
 * containing id1", because people reasonably mean either. */
static int try_dir(const char *dir, char *out, size_t cap, char *tried,
                   size_t triedcap)
{
    char sub[PATHCAP];

    if (tried)
        note(tried, triedcap, dir);
    if (is_dir(dir) && pak_in(dir, out, cap))
        return 1;
    snprintf(sub, sizeof(sub), "%s/id1", dir);
    if (is_dir(sub) && pak_in(sub, out, cap))
        return 1;
    return 0;
}

static void exe_dir(char *out, size_t cap)
{
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    char *slash;

    if (n <= 0) {
        out[0] = '\0';
        return;
    }
    out[n] = '\0';
    slash = strrchr(out, '/');
    if (slash)
        *slash = '\0';
}

int oq_find_pak(const char *want, char *out, size_t outcap,
                char *tried, size_t triedcap)
{
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_DATA_HOME");
    char buf[PATHCAP], exe[PATHCAP];
    size_t i;

    if (tried && triedcap)
        tried[0] = '\0';

    if (!want || !*want)
        want = getenv("OMAQUAKE_PAK");

    if (want && *want) {
        /* An explicit choice is honoured or refused, never silently ignored
         * in favour of a search -- being sent somewhere unexpected is worse
         * than a clear failure. */
        if (is_file(want)) {
            snprintf(out, outcap, "%s", want);
            return has_id1(out) ? 0 : -1;
        }
        if (try_dir(want, out, outcap, tried, triedcap))
            return has_id1(out) ? 0 : -1;
        if (tried && !tried[0])
            note(tried, triedcap, want);
        return -1;
    }

    /* Alongside the current directory and the binary first: a checkout with
     * id1/ next to it is the common development case. */
    if (try_dir(".", out, outcap, tried, triedcap))
        return 0;

    exe_dir(exe, sizeof(exe));
    if (exe[0]) {
        if (try_dir(exe, out, outcap, tried, triedcap))
            return 0;
        if (strlen(exe) + 4 < sizeof(buf)) {
            /* build/omaquake -> the checkout above it */
            snprintf(buf, sizeof(buf), "%s/..", exe);
            if (try_dir(buf, out, outcap, tried, triedcap))
                return 0;
        }
    }

    if (xdg && *xdg) {
        snprintf(buf, sizeof(buf), "%s/omaquake", xdg);
        if (try_dir(buf, out, outcap, tried, triedcap))
            return 0;
    }
    if (home && *home) {
        static const char *rel[] = {
            ".local/share/omaquake",
            ".omaquake",
            ".quakespasm",
            ".quake",
            ".local/share/Steam/steamapps/common/Quake",
            ".steam/steam/steamapps/common/Quake",
        };

        for (i = 0; i < sizeof(rel) / sizeof(rel[0]); i++) {
            snprintf(buf, sizeof(buf), "%s/%s", home, rel[i]);
            if (try_dir(buf, out, outcap, tried, triedcap))
                return 0;
        }
    }
    {
        static const char *sys[] = {
            "/usr/local/share/omaquake",
            "/usr/share/omaquake",
            "/usr/local/share/quake",
            "/usr/share/quake",
            "/usr/share/games/quake",
            "/opt/quake",
        };

        for (i = 0; i < sizeof(sys) / sizeof(sys[0]); i++)
            if (try_dir(sys[i], out, outcap, tried, triedcap))
                return 0;
    }
    out[0] = '\0';
    return -1;
}
