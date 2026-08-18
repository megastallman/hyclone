#ifndef __SYSCALL_RESTART_H__
#define __SYSCALL_RESTART_H__

#include <cstdint>
#include <errno.h>

#include "extended_commpage.h"

// HyClone uses Linux realtime signals internally (e.g. SIGREQUEST for
// server-to-process requests). These interrupt blocking syscalls that
// SA_RESTART does not cover (poll, select, timed recv, nanosleep, ...),
// making them fail with EINTR even though the guest never saw a signal.
// On Haiku, a syscall only returns B_INTERRUPTED when a userland-visible
// signal was delivered.
//
// This helper restarts a syscall that failed with EINTR unless a
// guest-installed signal handler ran in the meantime (tracked by monika's
// signal trampolines via notify_guest_signal()), in which case the EINTR
// is propagated like the Haiku kernel would.
template <typename Call>
static inline long restartable_syscall(Call&& call)
{
    while (true)
    {
        uint64_t guestSignalCount = GET_HOSTCALLS()->guest_signal_count();

        long result = call();

        if (result != -EINTR)
        {
            return result;
        }

        if (GET_HOSTCALLS()->guest_signal_count() != guestSignalCount)
        {
            // A guest signal handler ran; the interruption is guest-visible.
            return result;
        }

        // Interrupted by HyClone-internal machinery; restart transparently.
    }
}

#endif // __SYSCALL_RESTART_H__
