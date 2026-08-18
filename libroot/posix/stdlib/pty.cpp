#include <cstring>

#include "extended_commpage.h"
#include "haiku_errors.h"

extern "C"
{

int
posix_openpt(int openFlags)
{
    return GET_HOSTCALLS()->posix_openpt(openFlags);
}

int
grantpt(int masterFD)
{
    return GET_HOSTCALLS()->grantpt(masterFD);
}

int
ptsname_r(int masterFD, char* name, size_t namesize)
{
    // Haiku semantics: returns 0 on success, an errno value on failure.
    if (name == NULL)
    {
        return HAIKU_POSIX_EINVAL;
    }

    char buffer[32];
    if (GET_HOSTCALLS()->ptsname(masterFD, buffer, sizeof(buffer)) != 0)
    {
        return HAIKU_POSIX_EINVAL;
    }

    size_t length = strlen(buffer) + 1;
    if (length > namesize)
    {
        return HAIKU_POSIX_ERANGE;
    }

    memcpy(name, buffer, length);
    return 0;
}

char*
ptsname(int masterFD)
{
    static char buffer[32];

    if (GET_HOSTCALLS()->ptsname(masterFD, buffer, sizeof(buffer)) != 0)
    {
        return NULL;
    }

    return buffer;
}

int
unlockpt(int masterFD)
{
    return GET_HOSTCALLS()->unlockpt(masterFD);
}

}