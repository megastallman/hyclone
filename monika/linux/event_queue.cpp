#include <cstdint>
#include <cstring>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/mman.h>

#include "BeDefs.h"
#include "errno_conversion.h"
#include "extended_commpage.h"
#include "haiku_errors.h"
#include "haiku_fcntl.h"
#include "linux_debug.h"
#include "linux_subsystemlock.h"
#include "linux_syscall.h"
#include "syscall_restart.h"

// Haiku kernel event queues (headers/private/system/event_queue_defs.h),
// added in 2023 and used by libbsd's kqueue()/kevent() (and through that by
// e.g. python's select.kqueue). Emulated on top of Linux epoll: the queue
// IS the epoll fd (on Haiku, too, the queue is an fd that is closed with
// close()).
//
// Only B_OBJECT_TYPE_FD is supported for now; semaphores, ports and threads
// return B_UNSUPPORTED.

// From headers/os/kernel/OS.h
#define HAIKU_B_OBJECT_TYPE_FD              0
#define HAIKU_B_EVENT_READ                  0x0001
#define HAIKU_B_EVENT_WRITE                 0x0002
#define HAIKU_B_EVENT_ERROR                 0x0004
#define HAIKU_B_EVENT_PRIORITY_READ         0x0008
#define HAIKU_B_EVENT_PRIORITY_WRITE        0x0010
#define HAIKU_B_EVENT_HIGH_PRIORITY_READ    0x0020
#define HAIKU_B_EVENT_HIGH_PRIORITY_WRITE   0x0040
#define HAIKU_B_EVENT_DISCONNECTED          0x0080
#define HAIKU_B_EVENT_INVALID               0x1000

// From headers/private/system/event_queue_defs.h
#define HAIKU_B_EVENT_LEVEL_TRIGGERED       (1 << 26)
#define HAIKU_B_EVENT_ONE_SHOT              (1 << 27)

struct haiku_event_wait_info
{
    int32 object;
    uint16_t type;
    int32 events;
    void* user_data;
};

// Per-registration bookkeeping. epoll's per-fd data word points at one of
// these so that _moni_event_queue_wait can report the Haiku object ID and
// the user_data. Records live in mmap'ed arena pages that are never
// unmapped; freed records go to a free list.
struct EventQueueRegistration
{
    int queueFd;
    int objectFd;
    int32 selectedEvents;
    void* userData;
    EventQueueRegistration* next;
};

static int sEventQueueLock = HYCLONE_SUBSYSTEM_LOCK_UNINITIALIZED;
static EventQueueRegistration* sActiveList = NULL;
static EventQueueRegistration* sFreeList = NULL;

static EventQueueRegistration* AllocRegistrationLocked()
{
    if (sFreeList == NULL)
    {
        long result = LINUX_SYSCALL6(__NR_mmap, NULL, B_PAGE_SIZE,
            PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (result < 0)
        {
            return NULL;
        }

        auto records = (EventQueueRegistration*)result;
        size_t count = B_PAGE_SIZE / sizeof(EventQueueRegistration);
        for (size_t i = 0; i < count; ++i)
        {
            records[i].next = sFreeList;
            sFreeList = &records[i];
        }
    }

    EventQueueRegistration* reg = sFreeList;
    sFreeList = reg->next;
    reg->next = sActiveList;
    sActiveList = reg;
    return reg;
}

static void FreeRegistrationLocked(EventQueueRegistration* reg)
{
    EventQueueRegistration** link = &sActiveList;
    while (*link != NULL && *link != reg)
    {
        link = &(*link)->next;
    }
    if (*link == NULL)
    {
        return;
    }
    *link = reg->next;

    reg->queueFd = -1;
    reg->objectFd = -1;
    reg->next = sFreeList;
    sFreeList = reg;
}

static EventQueueRegistration* FindRegistrationLocked(int queueFd, int objectFd)
{
    for (EventQueueRegistration* reg = sActiveList; reg != NULL; reg = reg->next)
    {
        if (reg->queueFd == queueFd && reg->objectFd == objectFd)
        {
            return reg;
        }
    }
    return NULL;
}

static bool IsActiveRegistrationLocked(EventQueueRegistration* reg)
{
    for (EventQueueRegistration* it = sActiveList; it != NULL; it = it->next)
    {
        if (it == reg)
        {
            return true;
        }
    }
    return false;
}

static uint32_t EventsBToEpoll(int32 events)
{
    uint32_t result = 0;
    if (events & (HAIKU_B_EVENT_READ | HAIKU_B_EVENT_HIGH_PRIORITY_READ))
        result |= EPOLLIN;
    if (events & HAIKU_B_EVENT_PRIORITY_READ)
        result |= EPOLLPRI;
    if (events & (HAIKU_B_EVENT_WRITE | HAIKU_B_EVENT_PRIORITY_WRITE
            | HAIKU_B_EVENT_HIGH_PRIORITY_WRITE))
        result |= EPOLLOUT;
    if (events & HAIKU_B_EVENT_DISCONNECTED)
        result |= EPOLLRDHUP;
    if (!(events & HAIKU_B_EVENT_LEVEL_TRIGGERED))
        result |= EPOLLET;
    if (events & HAIKU_B_EVENT_ONE_SHOT)
        result |= EPOLLONESHOT;
    return result;
}

static int32 EventsEpollToB(uint32_t events)
{
    int32 result = 0;
    if (events & EPOLLIN)
        result |= HAIKU_B_EVENT_READ;
    if (events & EPOLLPRI)
        result |= HAIKU_B_EVENT_PRIORITY_READ;
    if (events & EPOLLOUT)
        result |= HAIKU_B_EVENT_WRITE;
    if (events & EPOLLERR)
        result |= HAIKU_B_EVENT_ERROR;
    if (events & (EPOLLHUP | EPOLLRDHUP))
        result |= HAIKU_B_EVENT_DISCONNECTED;
    return result;
}

extern "C"
{

int _moni_event_queue_create(int openFlags)
{
    int linuxFlags = 0;
    if (openFlags & HAIKU_O_CLOEXEC)
    {
        linuxFlags |= EPOLL_CLOEXEC;
    }
    // Haiku's O_CLOFORK has no epoll equivalent on Linux; ignored.

    long fd = LINUX_SYSCALL1(__NR_epoll_create1, linuxFlags);
    if (fd < 0)
    {
        return LinuxToB(-fd);
    }
    return fd;
}

status_t _moni_event_queue_select(int queue, haiku_event_wait_info* infos,
    int numInfos)
{
    if (numInfos < 0 || (infos == NULL && numInfos != 0))
    {
        return B_BAD_VALUE;
    }

    auto lock = SubsystemLock(sEventQueueLock);

    for (int i = 0; i < numInfos; ++i)
    {
        haiku_event_wait_info& info = infos[i];

        if (info.type != HAIKU_B_OBJECT_TYPE_FD)
        {
            trace("event_queue_select: unsupported object type.");
            return B_UNSUPPORTED;
        }

        EventQueueRegistration* reg
            = FindRegistrationLocked(queue, info.object);

        if (info.events == -1)
        {
            // Query the current selection.
            info.events = (reg != NULL) ? reg->selectedEvents : 0;
            continue;
        }

        if (info.events == 0)
        {
            // Deselect.
            if (reg == NULL)
            {
                return B_ENTRY_NOT_FOUND;
            }
            LINUX_SYSCALL4(__NR_epoll_ctl, queue, EPOLL_CTL_DEL,
                info.object, NULL);
            FreeRegistrationLocked(reg);
            continue;
        }

        // Select (or replace an existing selection).
        bool isNew = (reg == NULL);
        if (isNew)
        {
            reg = AllocRegistrationLocked();
            if (reg == NULL)
            {
                return B_NO_MEMORY;
            }
            reg->queueFd = queue;
            reg->objectFd = info.object;
        }
        reg->selectedEvents = info.events;
        reg->userData = info.user_data;

        struct epoll_event event;
        memset(&event, 0, sizeof(event));
        event.events = EventsBToEpoll(info.events);
        event.data.ptr = reg;

        long result = LINUX_SYSCALL4(__NR_epoll_ctl, queue,
            isNew ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, info.object, &event);
        if (result == -EEXIST)
        {
            result = LINUX_SYSCALL4(__NR_epoll_ctl, queue, EPOLL_CTL_MOD,
                info.object, &event);
        }
        else if (result == -ENOENT)
        {
            result = LINUX_SYSCALL4(__NR_epoll_ctl, queue, EPOLL_CTL_ADD,
                info.object, &event);
        }

        if (result < 0)
        {
            FreeRegistrationLocked(reg);
            return LinuxToB(-result);
        }
    }

    return B_OK;
}

ssize_t _moni_event_queue_wait(int queue, haiku_event_wait_info* infos,
    int numInfos, uint32 flags, int64 timeout)
{
    if (numInfos <= 0 || infos == NULL)
    {
        return B_BAD_VALUE;
    }

    const int kMaxEvents = 64;
    struct epoll_event events[kMaxEvents];
    int maxEvents = (numInfos < kMaxEvents) ? numInfos : kMaxEvents;

    int timeoutMs;
    if (flags & B_RELATIVE_TIMEOUT)
    {
        if (timeout <= 0)
        {
            timeoutMs = 0;
        }
        else
        {
            int64 ms = (timeout + 999) / 1000;
            timeoutMs = (ms > INT32_MAX) ? -1 : (int)ms;
        }
    }
    else if (flags & B_ABSOLUTE_TIMEOUT)
    {
        int64 delta = timeout - GET_HOSTCALLS()->system_time();
        if (delta <= 0)
        {
            timeoutMs = 0;
        }
        else
        {
            int64 ms = (delta + 999) / 1000;
            timeoutMs = (ms > INT32_MAX) ? -1 : (int)ms;
        }
    }
    else
    {
        timeoutMs = -1;
    }

    long result = restartable_syscall([&] {
        return (long)LINUX_SYSCALL4(__NR_epoll_wait, queue, events,
            maxEvents, timeoutMs);
    });

    if (result < 0)
    {
        return LinuxToB(-result);
    }

    if (result == 0)
    {
        if (timeoutMs == 0)
        {
            return B_WOULD_BLOCK;
        }
        return B_TIMED_OUT;
    }

    auto lock = SubsystemLock(sEventQueueLock);

    ssize_t count = 0;
    for (long i = 0; i < result; ++i)
    {
        auto reg = (EventQueueRegistration*)events[i].data.ptr;

        // The registration may have been deselected (and recycled) between
        // epoll_wait() returning and us taking the lock.
        if (reg == NULL || !IsActiveRegistrationLocked(reg)
            || reg->queueFd != queue)
        {
            continue;
        }

        infos[count].object = reg->objectFd;
        infos[count].type = HAIKU_B_OBJECT_TYPE_FD;
        infos[count].events = EventsEpollToB(events[i].events);
        infos[count].user_data = reg->userData;
        count++;

        if (reg->selectedEvents & HAIKU_B_EVENT_ONE_SHOT)
        {
            LINUX_SYSCALL4(__NR_epoll_ctl, queue, EPOLL_CTL_DEL,
                reg->objectFd, NULL);
            FreeRegistrationLocked(reg);
        }
    }

    return count;
}

}
