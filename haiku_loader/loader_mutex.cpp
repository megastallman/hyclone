#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "BeDefs.h"
#include "haiku_errors.h"
#include "loader_mutex.h"
#include "user_mutex_defs.h"

// Logic based on Haiku's src/system/kernel/locks/user_mutex.cpp,
// which rewrote the user mutex protocol in hrev57113 and later.
//
// Note: B_USER_MUTEX_SHARED (mutexes in shared memory, keyed by physical
// address in the kernel) is not supported by this per-process emulation.
// Such mutexes only work within a single process here.

namespace
{

struct Waiter
{
    std::condition_variable condition;
    bool notified = false;
};

struct UserMutexEntry
{
    std::deque<Waiter *> waiters;

    size_t EntriesCount() const { return waiters.size(); }

    void Add(Waiter *waiter) { waiters.push_back(waiter); }

    bool NotifyOne()
    {
        if (waiters.empty())
        {
            return false;
        }
        Waiter *waiter = waiters.front();
        waiters.pop_front();
        waiter->notified = true;
        waiter->condition.notify_one();
        return true;
    }

    void NotifyAll()
    {
        while (NotifyOne())
            ;
    }

    void Remove(Waiter *waiter)
    {
        for (auto it = waiters.begin(); it != waiters.end(); ++it)
        {
            if (*it == waiter)
            {
                waiters.erase(it);
                return;
            }
        }
    }
};

} // namespace

static std::mutex sMutexTableLock;
static std::unordered_map<int32_t *, UserMutexEntry> sMutexTable;

static int32_t atomic_or(int32_t *a, int32_t b)
{
    static_assert(sizeof(std::atomic<int32_t>) == sizeof(int32_t), "Can't use this hack on this platform.");
    auto atomic_a = (std::atomic<int32_t> *)a;
    return atomic_a->fetch_or(b);
}

static int32_t atomic_and(int32_t *a, int32_t b)
{
    static_assert(sizeof(std::atomic<int32_t>) == sizeof(int32_t), "Can't use this hack on this platform.");
    auto atomic_a = (std::atomic<int32_t> *)a;
    return atomic_a->fetch_and(b);
}

static int32_t atomic_get(int32_t *a)
{
    auto atomic_a = (std::atomic<int32_t> *)a;
    return atomic_a->load();
}

// Returns the previous value (like Haiku's atomic_test_and_set).
static int32_t atomic_test_and_set(int32_t *a, int32_t newValue, int32_t testAgainst)
{
    auto atomic_a = (std::atomic<int32_t> *)a;
    atomic_a->compare_exchange_strong(testAgainst, newValue);
    return testAgainst;
}

// sMutexTableLock must be held.
static UserMutexEntry *get_entry(int32_t *address, bool noCreate = false)
{
    auto it = sMutexTable.find(address);
    if (it == sMutexTable.end())
    {
        if (noCreate)
        {
            return nullptr;
        }
        it = sMutexTable.emplace(address, UserMutexEntry()).first;
    }
    return &it->second;
}

// sMutexTableLock must be held.
static void put_entry(int32_t *address)
{
    auto it = sMutexTable.find(address);
    if (it != sMutexTable.end() && it->second.waiters.empty())
    {
        sMutexTable.erase(it);
    }
}

// Waits until notified or until the timeout expires.
// Returns B_OK if notified, B_TIMED_OUT otherwise.
// sMutexTableLock must be held through `lock`; it is released while waiting.
static int wait_for_notification(Waiter &waiter, std::unique_lock<std::mutex> &lock,
    uint32_t flags, int64_t timeout)
{
    bool absolute = flags & B_ABSOLUTE_TIMEOUT;
    bool relative = flags & B_RELATIVE_TIMEOUT;

    auto notified = [&waiter] { return waiter.notified; };

    if (timeout == B_INFINITE_TIMEOUT || (!absolute && !relative))
    {
        waiter.condition.wait(lock, notified);
        return B_OK;
    }

    bool success;
    if (absolute)
    {
        // Haiku absolute timeouts are in microseconds of system_time(),
        // which HyClone bases on CLOCK_MONOTONIC, like std::steady_clock.
        success = waiter.condition.wait_until(lock,
            std::chrono::steady_clock::time_point(std::chrono::microseconds(timeout)), notified);
    }
    else
    {
        success = waiter.condition.wait_for(lock, std::chrono::microseconds(timeout), notified);
    }

    return success ? B_OK : B_TIMED_OUT;
}

// Attempts to acquire the mutex. Returns true if successfully locked.
// sMutexTableLock must be held.
static bool loader_mutex_prepare_to_lock(UserMutexEntry *entry, int32_t *mutex)
{
    int32_t oldValue = atomic_or(mutex, B_USER_MUTEX_LOCKED | B_USER_MUTEX_WAITING);
    if ((oldValue & B_USER_MUTEX_LOCKED) == 0 || (oldValue & B_USER_MUTEX_DISABLED) != 0)
    {
        // possibly unset the waiting flag
        if ((oldValue & B_USER_MUTEX_WAITING) == 0 && entry->EntriesCount() == 0)
        {
            atomic_and(mutex, ~(int32_t)B_USER_MUTEX_WAITING);
        }
        return true;
    }

    return false;
}

// sMutexTableLock must be held through `lock`.
static int loader_mutex_lock_locked(UserMutexEntry *entry, int32_t *mutex,
    uint32_t flags, int64_t timeout, std::unique_lock<std::mutex> &lock)
{
    if (loader_mutex_prepare_to_lock(entry, mutex))
    {
        return B_OK;
    }

    Waiter waiter;
    entry->Add(&waiter);

    int error = wait_for_notification(waiter, lock, flags, timeout);

    if (error != B_OK && !waiter.notified)
    {
        entry->Remove(&waiter);
        if (entry->EntriesCount() == 0)
        {
            atomic_and(mutex, ~(int32_t)B_USER_MUTEX_WAITING);
        }
        return error;
    }

    // The unblocking thread has handed the lock over to us.
    return B_OK;
}

// sMutexTableLock must be held.
static void loader_mutex_unblock_locked(UserMutexEntry *entry, int32_t *mutex, uint32_t flags)
{
    if (entry->EntriesCount() == 0)
    {
        // Nobody is actually waiting at present.
        atomic_and(mutex, ~(int32_t)B_USER_MUTEX_WAITING);
        return;
    }

    int32_t oldValue = 0;
    if ((flags & B_USER_MUTEX_UNBLOCK_ALL) == 0)
    {
        // This is not merely an unblock, but a hand-off.
        oldValue = atomic_or(mutex, B_USER_MUTEX_LOCKED);
        if ((oldValue & B_USER_MUTEX_LOCKED) != 0)
        {
            return;
        }
    }

    if ((flags & B_USER_MUTEX_UNBLOCK_ALL) != 0 || (oldValue & B_USER_MUTEX_DISABLED) != 0)
    {
        // unblock all waiting threads
        entry->NotifyAll();
    }
    else
    {
        if (!entry->NotifyOne())
        {
            atomic_and(mutex, ~(int32_t)B_USER_MUTEX_LOCKED);
        }
    }

    if (entry->EntriesCount() == 0)
    {
        atomic_and(mutex, ~(int32_t)B_USER_MUTEX_WAITING);
    }
}

int loader_mutex_lock(int32_t *mutex, const char *name, uint32_t flags, int64_t timeout)
{
    if (mutex == NULL || ((intptr_t)mutex) % 4 != 0)
    {
        return B_BAD_ADDRESS;
    }

    std::unique_lock<std::mutex> lock(sMutexTableLock);

    UserMutexEntry *entry = get_entry(mutex);
    int error = loader_mutex_lock_locked(entry, mutex, flags, timeout, lock);
    put_entry(mutex);

    return error;
}

int loader_mutex_unblock(int32_t *mutex, uint32_t flags)
{
    if (mutex == NULL || ((intptr_t)mutex) % 4 != 0)
    {
        return B_BAD_ADDRESS;
    }

    std::unique_lock<std::mutex> lock(sMutexTableLock);

    UserMutexEntry *entry = get_entry(mutex, true);
    if (entry == nullptr)
    {
        atomic_and(mutex, ~(int32_t)B_USER_MUTEX_WAITING);
    }
    else
    {
        loader_mutex_unblock_locked(entry, mutex, flags);
        put_entry(mutex);
    }

    return B_OK;
}

int loader_mutex_switch_lock(int32_t *fromMutex, uint32_t fromFlags,
    int32_t *toMutex, const char *name, uint32_t toFlags, int64_t timeout)
{
    if (fromMutex == NULL || ((intptr_t)fromMutex) % 4 != 0
        || toMutex == NULL || ((intptr_t)toMutex) % 4 != 0)
    {
        return B_BAD_ADDRESS;
    }

    std::unique_lock<std::mutex> lock(sMutexTableLock);

    UserMutexEntry *toEntry = get_entry(toMutex);

    Waiter waiter;
    bool alreadyLocked = loader_mutex_prepare_to_lock(toEntry, toMutex);
    if (!alreadyLocked)
    {
        toEntry->Add(&waiter);
    }

    // unlock the first mutex and unblock its waiters, if any
    int32_t oldValue = atomic_and(fromMutex, ~(int32_t)B_USER_MUTEX_LOCKED);
    if ((oldValue & B_USER_MUTEX_WAITING) != 0)
    {
        UserMutexEntry *fromEntry = get_entry(fromMutex, true);
        if (fromEntry != nullptr)
        {
            loader_mutex_unblock_locked(fromEntry, fromMutex, fromFlags);
            put_entry(fromMutex);
        }
    }

    int error = B_OK;
    if (!alreadyLocked)
    {
        error = wait_for_notification(waiter, lock, toFlags, timeout);

        if (error != B_OK && !waiter.notified)
        {
            toEntry->Remove(&waiter);
            if (toEntry->EntriesCount() == 0)
            {
                atomic_and(toMutex, ~(int32_t)B_USER_MUTEX_WAITING);
            }
        }
        else
        {
            error = B_OK;
        }
    }

    put_entry(toMutex);

    return error;
}

int loader_mutex_sem_acquire(int32_t *sem, const char *name, uint32_t flags, int64_t timeout)
{
    if (sem == NULL || ((intptr_t)sem) % 4 != 0)
    {
        return B_BAD_ADDRESS;
    }

    std::unique_lock<std::mutex> lock(sMutexTableLock);

    UserMutexEntry *entry = get_entry(sem);

    // The semaphore may have been released in the meantime, and we also
    // need to mark it as contended if it isn't already.
    int error = B_OK;
    bool acquired = false;
    int32_t oldValue = atomic_get(sem);
    while (oldValue > -1)
    {
        int32_t value = atomic_test_and_set(sem, oldValue - 1, oldValue);
        if (value == oldValue && value > 0)
        {
            acquired = true;
            break;
        }
        oldValue = value;
    }

    if (!acquired)
    {
        Waiter waiter;
        entry->Add(&waiter);

        error = wait_for_notification(waiter, lock, flags, timeout);

        if (error != B_OK && !waiter.notified)
        {
            entry->Remove(&waiter);
        }
        else
        {
            error = B_OK;
        }
    }

    put_entry(sem);

    return error;
}

int loader_mutex_sem_release(int32_t *sem, uint32_t flags)
{
    if (sem == NULL || ((intptr_t)sem) % 4 != 0)
    {
        return B_BAD_ADDRESS;
    }

    std::unique_lock<std::mutex> lock(sMutexTableLock);

    UserMutexEntry *entry = get_entry(sem);

    if (!entry->NotifyOne())
    {
        // no waiters - mark as uncontended and release
        int32_t oldValue = atomic_get(sem);
        while (true)
        {
            int32_t inc = oldValue < 0 ? 2 : 1;
            int32_t value = atomic_test_and_set(sem, oldValue + inc, oldValue);
            if (value == oldValue)
            {
                break;
            }
            oldValue = value;
        }
    }
    else if (entry->EntriesCount() == 0)
    {
        // mark the semaphore uncontended
        atomic_test_and_set(sem, 0, -1);
    }

    put_entry(sem);

    return B_OK;
}
