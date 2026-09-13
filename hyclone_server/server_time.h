#ifndef __SERVER_TIME_H__
#define __SERVER_TIME_H__

#include <chrono>

#include "BeDefs.h"

constexpr auto kMaxTimePoint =
    std::chrono::time_point_cast<std::chrono::microseconds, std::chrono::steady_clock>
        (std::chrono::steady_clock::time_point::max()).time_since_epoch().count();

inline bool server_is_infinite_timeout(bigtime_t timeout, uint32 flags = B_RELATIVE_TIMEOUT)
{
    return timeout == B_INFINITE_TIMEOUT || timeout > kMaxTimePoint
        || ((flags & B_RELATIVE_TIMEOUT) &&
            timeout > (bigtime_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now()).count());
}

// Convert a Haiku (flags, timeout) pair into a relative timeout in microseconds, suitable for
// the wait primitives (such as Port::Read/Write) that only understand a relative wait or
// B_INFINITE_TIMEOUT. Returns B_INFINITE_TIMEOUT when the caller requested a blocking
// (no-timeout) operation. A B_ABSOLUTE_TIMEOUT deadline that has already elapsed yields 0,
// which those primitives treat as a non-blocking poll.
//
// This exists because the guest passes B_ABSOLUTE_TIMEOUT (0x10) for timed waits (e.g. the
// media kit's BMediaEventLooper::ControlLoop), but B_TIMEOUT (0x8) only names the relative
// flag; testing "flags & B_TIMEOUT" silently mishandled every absolute-timeout read as an
// infinite block. The absolute deadline shares the guest system_time() base (CLOCK_MONOTONIC
// == std::chrono::steady_clock); with B_TIMEOUT_REAL_TIME_BASE it uses the real-time clock
// (CLOCK_REALTIME == std::chrono::system_clock).
inline bigtime_t server_relative_timeout(uint32 flags, bigtime_t timeout)
{
    // No timeout flag set: block indefinitely.
    if (!(flags & (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT)))
        return B_INFINITE_TIMEOUT;

    if (server_is_infinite_timeout(timeout, flags))
        return B_INFINITE_TIMEOUT;

    if (flags & B_ABSOLUTE_TIMEOUT)
    {
        bigtime_t now;
        if (flags & B_TIMEOUT_REAL_TIME_BASE)
        {
            now = (bigtime_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        else
        {
            now = (bigtime_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        // bigtime_t is unsigned: compare before subtracting to avoid wrap-around.
        // A deadline at or before now has already elapsed -> don't wait (relative 0).
        if (timeout <= now)
            return 0;
        return timeout - now;
    }

    // B_RELATIVE_TIMEOUT (already a non-negative relative value).
    return timeout;
}

#endif // __SERVER_TIME_H__
