#include "oq_present.h"
#include <string.h>

static const oq_present_backend *const backends[] = {
#ifdef OQ_HAVE_CHAFA
    &oq_backend_chafa,
#endif
#ifdef OQ_HAVE_CACA
    &oq_backend_caca,
#endif
    NULL
};

const oq_present_backend *oq_present_lookup(const char *name)
{
    int i;

    for (i = 0; backends[i]; i++)
        if (!strcmp(backends[i]->name, name))
            return backends[i];
    return NULL;
}

const char *oq_present_available(void)
{
    static char buf[128];
    int i;

    buf[0] = '\0';
    for (i = 0; backends[i]; i++) {
        if (i)
            strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, backends[i]->name, sizeof(buf) - strlen(buf) - 1);
    }
    return buf[0] ? buf : "(none)";
}
