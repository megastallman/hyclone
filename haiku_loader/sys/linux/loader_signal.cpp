#include <signal.h>

#include "loader_signal.h"

int loader_get_sigrtmin()
{
    return SIGRTMIN;
}

int loader_get_sigrtmax()
{
    return SIGRTMAX;
}

// Number of guest-installed signal handlers that have run on this thread.
// Incremented from a signal handler context: initial-exec TLS in the main
// executable is a plain fs-relative access, which is async-signal-safe.
static thread_local uint64_t sGuestSignalCount = 0;

void loader_notify_guest_signal()
{
    ++sGuestSignalCount;
}

uint64_t loader_guest_signal_count()
{
    return sGuestSignalCount;
}
