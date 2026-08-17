#include <errno.h>
#include <unistd.h>

#include "BeDefs.h"
#include "errno_conversion.h"
#include "extended_commpage.h"
#include "haiku_errors.h"
#include "linux_syscall.h"

extern "C"
{

// Haiku replaced the _kern_get[ug]id/_kern_setre[ug]id syscalls with
// _kern_getres[ug]id/_kern_setres[ug]id (hrev57917).
// The HyClone server does not track saved IDs yet, so the saved ID is
// approximated with the effective one.
status_t _moni_getresgid(haiku_gid_t* rgid, haiku_gid_t* egid, haiku_gid_t* sgid)
{
    if (rgid != NULL)
    {
        *rgid = GET_SERVERCALLS()->getgid(false);
    }
    if (egid != NULL)
    {
        *egid = GET_SERVERCALLS()->getgid(true);
    }
    if (sgid != NULL)
    {
        *sgid = GET_SERVERCALLS()->getgid(true);
    }
    return B_OK;
}

status_t _moni_getresuid(haiku_uid_t* ruid, haiku_uid_t* euid, haiku_uid_t* suid)
{
    if (ruid != NULL)
    {
        *ruid = GET_SERVERCALLS()->getuid(false);
    }
    if (euid != NULL)
    {
        *euid = GET_SERVERCALLS()->getuid(true);
    }
    if (suid != NULL)
    {
        *suid = GET_SERVERCALLS()->getuid(true);
    }
    return B_OK;
}

static status_t _moni_setregid(haiku_gid_t rgid, haiku_gid_t egid,
    bool setAllIfPrivileged)
{
    intptr_t hostrgid = -1;
    intptr_t hostegid = -1;
    status_t status = GET_SERVERCALLS()->setregid(rgid, egid, setAllIfPrivileged, &hostrgid, &hostegid);

    if (status < 0)
    {
        return status;
    }

    if (hostrgid != -1 || hostegid != -1)
    {
        long result = LINUX_SYSCALL2(__NR_setregid, hostrgid, hostegid);
        if (result < 0)
        {
            return LinuxToB(-result);
        }

        // Retry
        status = GET_SERVERCALLS()->setregid(rgid, egid, setAllIfPrivileged, NULL, NULL);
    }

    if (status < 0)
    {
        return status;
    }

    return B_OK;
}

static status_t _moni_setreuid(haiku_uid_t ruid, haiku_uid_t euid,
    bool setAllIfPrivileged)
{
    intptr_t hostruid = -1;
    intptr_t hosteuid = -1;
    status_t status = GET_SERVERCALLS()->setreuid(ruid, euid, setAllIfPrivileged, &hostruid, &hosteuid);

    if (status < 0)
    {
        return status;
    }

    if (hostruid != -1 || hosteuid != -1)
    {
        long result = LINUX_SYSCALL2(__NR_setreuid, hostruid, hosteuid);
        if (result < 0)
        {
            return LinuxToB(-result);
        }

        // Retry
        status = GET_SERVERCALLS()->setreuid(ruid, euid, setAllIfPrivileged, NULL, NULL);
    }

    if (status < 0)
    {
        return status;
    }

    return B_OK;
}

status_t _moni_setresgid(haiku_gid_t rgid, haiku_gid_t egid, haiku_gid_t sgid,
    bool setAllIfPrivileged)
{
    // The saved ID is not tracked by the HyClone server yet.
    (void)sgid;
    if (rgid == (haiku_gid_t)-1 && egid == (haiku_gid_t)-1)
    {
        return B_OK;
    }
    return _moni_setregid(rgid, egid, setAllIfPrivileged);
}

status_t _moni_setresuid(haiku_uid_t ruid, haiku_uid_t euid, haiku_uid_t suid,
    bool setAllIfPrivileged)
{
    // The saved ID is not tracked by the HyClone server yet.
    (void)suid;
    if (ruid == (haiku_uid_t)-1 && euid == (haiku_uid_t)-1)
    {
        return B_OK;
    }
    return _moni_setreuid(ruid, euid, setAllIfPrivileged);
}

haiku_ssize_t _moni_getgroups(int groupCount, haiku_gid_t* groupList)
{
    static_assert(sizeof(haiku_gid_t) == sizeof(int), "gid_t is not 32-bit.");
    return GET_SERVERCALLS()->getgroups(groupCount, (int*)groupList);
}

status_t _moni_setgroups(int groupCount, haiku_gid_t* groupList)
{
    static_assert(sizeof(haiku_gid_t) == sizeof(int), "gid_t is not 32-bit.");

    if (groupCount > 0)
    {
        intptr_t* hostGroupList = (intptr_t*)__builtin_alloca(groupCount * sizeof(intptr_t));
        long hostGroupCount = GET_SERVERCALLS()->setgroups(groupCount, (int*)groupList, hostGroupList);

        if (hostGroupCount < 0)
        {
            return hostGroupCount;
        }

        if (hostGroupCount > 0)
        {
            gid_t* hostGroupList32 = (gid_t*)__builtin_alloca(hostGroupCount * sizeof(gid_t));
            for (long i = 0; i < hostGroupCount; i++)
            {
                hostGroupList32[i] = hostGroupList[i];
            }

            long status = LINUX_SYSCALL2(__NR_setgroups, hostGroupCount, hostGroupList32);
            if (status < 0)
            {
                return LinuxToB(-status);
            }
        }
    }

    return GET_SERVERCALLS()->setgroups(groupCount, (int*)groupList, NULL);
}

}