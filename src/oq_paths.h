#ifndef OQ_PATHS_H
#define OQ_PATHS_H

#include <stddef.h>

/* Locate pak0.pak.
 *
 * want may be NULL, a path to a pak file, a directory holding one, or a
 * directory holding id1/.  When it is NULL the usual places are searched.
 *
 * Returns 0 and writes the pak path to out; on failure returns -1 and writes
 * a human-readable account of everywhere it looked to tried, so the error
 * message can tell the player where to put the file. */
int oq_find_pak(const char *want, char *out, size_t outcap,
                char *tried, size_t triedcap);

#endif /* OQ_PATHS_H */
