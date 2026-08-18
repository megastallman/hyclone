#include <cstddef>
#include <cstring>
#include <errno.h>

#include "BeDefs.h"
#include "haiku_errors.h"
#include "linux_syscall.h"

// From Haiku's headers/private/system/random_defs.h
#define RANDOM_SYSCALLS "random"
#define RANDOM_GET_ENTROPY 1

struct random_get_entropy_args
{
    void* buffer;
    size_t length;
};

extern "C"
{

extern void _moni_debug_output(const char* userString);

status_t _moni_generic_syscall(const char* subsystem, uint32 function,
    void* buffer, size_t bufferSize)
{
    if (strcmp(subsystem, RANDOM_SYSCALLS) == 0)
    {
        switch (function)
        {
            case RANDOM_GET_ENTROPY:
            {
                if (bufferSize != sizeof(random_get_entropy_args) || buffer == NULL)
                {
                    return B_BAD_VALUE;
                }

                auto args = (random_get_entropy_args*)buffer;
                uint8* out = (uint8*)args->buffer;
                size_t filled = 0;

                while (filled < args->length)
                {
                    long result = LINUX_SYSCALL3(__NR_getrandom,
                        out + filled, args->length - filled, 0);
                    if (result < 0)
                    {
                        if (result == -EINTR)
                        {
                            continue;
                        }
                        return B_ERROR;
                    }
                    filled += result;
                }

                args->length = filled;
                return B_OK;
            }
            default:
                return B_BAD_VALUE;
        }
    }

    // Other subsystems remain non-fatal stubs for now.
    // In the future, these should be emulated in hyclone_server, see
    // https://github.com/trungnt2910/hyclone/pull/17#discussion_r1931997407

    _moni_debug_output("stub: _moni_generic_syscall");
    _moni_debug_output(subsystem);

    return B_NAME_NOT_FOUND;
}

}
