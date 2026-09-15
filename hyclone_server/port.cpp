#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "BeDefs.h"
#include "haiku_errors.h"
#include "port.h"
#include "process.h"
#include "server_native.h"
#include "server_servercalls.h"
#include "server_time.h"
#include "server_workers.h"
#include "system.h"

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

// Env-gated port tracing (set HYCLONE_PORT_TRACE=<path> to enable).
// Logs the port lifecycle so a hang shows up as an "enter" with no
// matching "exit"; correlate WRITE(id) against BUFSIZE/MSGINFO/READ(id).
//
// Tracing every port op across the whole boot serializes all guests through
// one mutex + synchronous I/O and can deadlock the boot, so tracing is armed
// at runtime: it starts DISABLED even when the env var is set, and is toggled
// with signals -- SIGUSR1 to start, SIGUSR2 to stop -- so only the window of
// interest (e.g. a BSoundPlayer connect) is captured. The gate is a relaxed
// atomic checked before the lock, so when disabled the overhead is a single
// load.
static std::atomic<bool> gPortTraceEnabled{false};

static void PortTraceSignal(int sig)
{
    gPortTraceEnabled.store(sig == SIGUSR1, std::memory_order_relaxed);
}

// Install the arm/disarm handlers UNCONDITIONALLY in every hyclone_server
// process (even ones without the trace env), so a broadcast SIGUSR1/SIGUSR2
// can never fall through to the default action and kill a server. The
// topology can have several server processes; only the one(s) that opened
// the trace file below actually emit anything.
static bool gPortTraceHandlers = []()
{
    signal(SIGUSR1, PortTraceSignal);
    signal(SIGUSR2, PortTraceSignal);
    return true;
}();

static FILE* gPortTraceFile = []() -> FILE*
{
    const char* path = getenv("HYCLONE_PORT_TRACE");
    if (path == NULL || path[0] == '\0')
        return NULL;
    FILE* f = fopen(path, "w");
    if (f != NULL)
        setvbuf(f, NULL, _IONBF, 0);
    return f;
}();

static void PortTrace(const hserver_context& context, const char* fmt, ...)
{
    if (gPortTraceFile == NULL || !gPortTraceEnabled.load(std::memory_order_relaxed))
        return;
    static std::mutex sLock;
    std::lock_guard<std::mutex> guard(sLock);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(gPortTraceFile, "%ld.%06ld pid=%d tid=%d ",
        (long)ts.tv_sec, ts.tv_nsec / 1000, context.pid, context.tid);
    va_list args;
    va_start(args, fmt);
    vfprintf(gPortTraceFile, fmt, args);
    va_end(args);
    fputc('\n', gPortTraceFile);
}

const int kSleepTimeMicroseconds = 100 * 1000; // 100ms, in microseconds.

Port::Port(int pid, int capacity, const char* name)
{
    _info.capacity = capacity;
    _info.team = pid;
    strncpy(_info.name, name, sizeof(_info.name) - 1);
    _info.name[sizeof(_info.name) - 1] = '\0';
    _info.queue_count = 0;
    _info.total_count = 0;
    _info.port = 0;
}

status_t Port::Write(Message&& message, bigtime_t timeout)
{
    message.info.size = message.data.size();

    std::unique_lock<std::mutex> lock(_messagesLock);

    if (_info.queue_count == _info.capacity)
    {
        if (timeout == 0)
        {
            return B_WOULD_BLOCK;
        }

        server_worker_run_wait([&]()
        {
            const auto writableOrDead = [&]()
            {
                return !_registered || _closed || _info.queue_count < _info.capacity;
            };

            if (!server_is_infinite_timeout(timeout))
            {
                _writeCondVar.wait_for(lock, std::chrono::microseconds(timeout), writableOrDead);
                return;
            }
            else
            {
                _writeCondVar.wait(lock, writableOrDead);
            }
        });
    }

    if (!_registered || _closed)
    {
        return B_BAD_PORT_ID;
    }

    if (_info.queue_count == _info.capacity)
    {
        return B_TIMED_OUT;
    }

    _messages.emplace(std::move(message));
    ++_info.queue_count;

    _readCondVar.notify_one();

    return B_OK;
}

status_t Port::Read(Message& message, bigtime_t timeout)
{
    std::unique_lock<std::mutex> lock(_messagesLock);

    if (_info.queue_count == 0)
    {
        // A closed port with nothing left to drain fails immediately, and a
        // reader that blocks here must be woken by Close() (see readableOrDead
        // and Port::Close), otherwise close_port() cannot unblock it.
        if (_closed)
        {
            return B_BAD_PORT_ID;
        }
        if (timeout == 0)
        {
            return B_WOULD_BLOCK;
        }

        server_worker_run_wait([&]()
        {
            const auto readableOrDead = [&]()
            {
                return !_registered || _closed || _info.queue_count > 0;
            };

            if (!server_is_infinite_timeout(timeout))
            {
                _readCondVar.wait_for(lock, std::chrono::microseconds(timeout), readableOrDead);
                return;
            }
            else
            {
                _readCondVar.wait(lock, readableOrDead);
            }
        });
    }

    if (!_registered)
    {
        return B_BAD_PORT_ID;
    }

    if (_info.queue_count == 0)
    {
        return _closed ? B_BAD_PORT_ID : B_TIMED_OUT;
    }

    --_info.queue_count;
    ++_info.total_count;

    message = std::move(_messages.front());
    _messages.pop();

    _writeCondVar.notify_one();

    return B_OK;
}

status_t Port::GetMessageInfo(haiku_port_message_info& info, bigtime_t timeout)
{
    std::unique_lock<std::mutex> lock(_messagesLock);

    if (_info.queue_count == 0)
    {
        if (_closed)
        {
            return B_BAD_PORT_ID;
        }
        if (timeout == 0)
        {
            return B_WOULD_BLOCK;
        }

        server_worker_run_wait([&]()
        {
            const auto readableOrDead = [&]()
            {
                return !_registered || _closed || _info.queue_count > 0;
            };

            if (!server_is_infinite_timeout(timeout))
            {
                _readCondVar.wait_for(lock, std::chrono::microseconds(timeout), readableOrDead);
                return;
            }
            else
            {
                _readCondVar.wait(lock, readableOrDead);
            }
        });
    }

    if (!_registered)
    {
        return B_BAD_PORT_ID;
    }

    if (_info.queue_count == 0)
    {
        return _closed ? B_BAD_PORT_ID : B_TIMED_OUT;
    }

    info = _messages.front().info;

    // We haven't read anything, let someone else read it.
    _readCondVar.notify_one();

    return B_OK;
}

status_t Port::Close()
{
    if (_closed)
    {
        return B_BAD_PORT_ID;
    }

    _closed = true;
    // Wake writers (they fail) AND readers: a thread blocked in read_port /
    // get_port_message_info must return B_BAD_PORT_ID once the port is closed,
    // which is what lets close_port() unblock e.g. a BMediaEventLooper control
    // thread so BMediaEventLooper::Quit() can join it.
    _writeCondVar.notify_all();
    _readCondVar.notify_all();
    return B_OK;
}

intptr_t server_hserver_call_create_port(hserver_context& context, int32 queue_length, const char *name, size_t portNameLength)
{
    if (queue_length < 1 || queue_length > HAIKU_PORT_MAX_QUEUE_LENGTH)
    {
        return B_BAD_VALUE;
    }

    auto buffer = std::string(portNameLength, '\0');
    if (server_read_process_memory(context.pid, (void*)name, buffer.data(), portNameLength) != portNameLength)
    {
        return B_BAD_ADDRESS;
    }
    auto newPort = std::make_shared<Port>(context.pid, queue_length, buffer.c_str());

    int id;

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();

        id = system.RegisterPort(std::move(newPort));
    }

    {
        auto lock = context.process->Lock();
        context.process->AddOwningPort(id);
    }

    PortTrace(context, "CREATE id=%d name=%s", id, buffer.c_str());
    return id;
}

intptr_t server_hserver_call_close_port(hserver_context& context, int portId)
{
    auto& system = System::GetInstance();

    std::shared_ptr<Port> port;

    {
        auto lock = system.Lock();

        port = system.GetPort(portId).lock();
        if (!port)
        {
            return B_BAD_PORT_ID;
        }
    }

    {
        auto lock = port->Lock();
        return port->Close();
    }
}

intptr_t server_hserver_call_delete_port(hserver_context& context, int portId)
{
    auto& system = System::GetInstance();

    std::shared_ptr<Port> port;
    std::shared_ptr<Process> owner;

    {
        auto lock = system.Lock();

        port = system.GetPort(portId).lock();
        if (!port)
        {
            return B_BAD_PORT_ID;
        }

        owner = system.GetProcess(port->GetOwner()).lock();
        if (!owner)
        {
            std::cerr << "Port #" << port->GetId() << " (" << port->GetName() << ") owned by dead process " << port->GetInfo().team << std::endl;
        }
    }

    if (owner)
    {
        auto lock = owner->Lock();
        owner->RemoveOwningPort(portId);
    }

    {
        auto lock = system.Lock();
        system.UnregisterPort(portId);
    }
    PortTrace(context, "DELETE id=%d name=%s", portId, port->GetName().c_str());
    return B_OK;
}

intptr_t server_hserver_call_find_port(hserver_context& context, const char *port_name, size_t portNameLength)
{
    auto buffer = std::string(portNameLength, '\0');
    if (server_read_process_memory(context.pid, (void*)port_name, buffer.data(), portNameLength) != portNameLength)
    {
        return B_BAD_ADDRESS;
    }

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();

        int result = system.FindPort(buffer);

        if (result < 0)
        {
            PortTrace(context, "FIND name=%s -> NOT_FOUND", buffer.c_str());
            return B_NAME_NOT_FOUND;
        }

        PortTrace(context, "FIND name=%s -> id=%d", buffer.c_str(), result);
        return result;
    }
}

intptr_t server_hserver_call_get_port_info(hserver_context& context, port_id id, void *info)
{
    std::shared_ptr<Port> port;

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();

        port = system.GetPort(id).lock();
    }

    if (!port)
    {
        return B_BAD_PORT_ID;
    }

    {
        auto lock = context.process->Lock();
        if (server_write_process_memory(context.pid, info, &port->GetInfo(), sizeof(haiku_port_info))
            != sizeof(haiku_port_info))
        {
            return B_BAD_ADDRESS;
        }

        return B_OK;
    }
}

intptr_t server_hserver_call_get_next_port_info(hserver_context& context, int team, int* userCookie, void* info)
{
    std::shared_ptr<Process> targetProcess;

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();

        targetProcess = system.GetProcess(team).lock();
    }

    if (!targetProcess)
    {
        return B_BAD_TEAM_ID;
    }

    int cookie;

    {
        auto lock = context.process->Lock();

        if (context.process->ReadMemory(userCookie, &cookie, sizeof(int)) != sizeof(int))
        {
            return B_BAD_ADDRESS;
        }
    }

    if (cookie < 0)
    {
        return B_BAD_VALUE;
    }

    std::shared_ptr<Port> port;

    {
        auto lock = targetProcess->Lock();
        auto& ports = targetProcess->GetOwningPorts();
        if (ports.empty() || cookie >= *ports.rbegin())
        {
            cookie = -1;
        }

        while (!port && cookie != -1)
        {
            auto it = ports.lower_bound(cookie);
            if (it == ports.end())
            {
                cookie = -1;
                break;
            }

            {
                auto& system = System::GetInstance();
                auto sysLock = system.Lock();

                port = system.GetPort(*it).lock();
            }

            cookie = *it + 1;
        }
    }

    {
        auto lock = context.process->Lock();

        if (port)
        {
            haiku_port_info portInfo = port->GetInfo();
            if (context.process->WriteMemory(info, &portInfo, sizeof(haiku_port_info)) != sizeof(haiku_port_info))
            {
                return B_BAD_ADDRESS;
            }
        }

        if (context.process->WriteMemory(userCookie, &cookie, sizeof(int)) != sizeof(int))
        {
            return B_BAD_ADDRESS;
        }
    }

    if (cookie == -1)
    {
        return B_BAD_VALUE;
    }

    return B_OK;
}

intptr_t server_hserver_call_port_count(hserver_context& context, port_id id)
{
    std::shared_ptr<Port> port;

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();

        port = system.GetPort(id).lock();
    }

    if (!port)
    {
        return B_BAD_PORT_ID;
    }

    return port->GetInfo().queue_count;
}

intptr_t server_hserver_call_port_buffer_size_etc(hserver_context& context, port_id id,
    uint32 flags, unsigned long long timeout)
{
    std::shared_ptr<Port> port;

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();
        port = system.GetPort(id).lock();
    }

    if (!port)
    {
        return B_BAD_PORT_ID;
    }

    haiku_port_message_info messageInfo;
    bigtime_t relativeTimeout = server_relative_timeout(flags, timeout);

    PortTrace(context, "BUFSIZE-enter id=%d queue=%d timeout=%s", id,
        port->GetInfo().queue_count,
        relativeTimeout == B_INFINITE_TIMEOUT ? "INFINITE" : "yes");
    status_t status = port->GetMessageInfo(messageInfo, relativeTimeout);
    if (status == B_WOULD_BLOCK && (flags & B_ABSOLUTE_TIMEOUT))
        status = B_TIMED_OUT;
    PortTrace(context, "BUFSIZE-exit  id=%d -> status=%d size=%d", id, (int)status,
        status == B_OK ? (int)messageInfo.size : -1);

    if (status != B_OK)
    {
        return status;
    }

    return messageInfo.size;
}

intptr_t server_hserver_call_set_port_owner(hserver_context& context, port_id id, team_id team)
{
    std::shared_ptr<Port> port;
    std::shared_ptr<Process> oldOwner;
    std::shared_ptr<Process> newOwner;
    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();

        port = system.GetPort(id).lock();

        if (!port)
        {
            return B_BAD_PORT_ID;
        }

        oldOwner = system.GetProcess(port->GetInfo().team).lock();

        // Owner dead so port probably dead?
        if (!oldOwner)
        {
            std::cerr << "Port " << port->GetName() << " owned by dead process " << port->GetInfo().team << std::endl;
            return B_BAD_PORT_ID;
        }

        newOwner = system.GetProcess(team).lock();

        if (!newOwner)
        {
            return B_BAD_TEAM_ID;
        }
    }

    if (oldOwner != newOwner)
    {
        auto oldOwnerLock = oldOwner->Lock();
        auto newOwnerLock = newOwner->Lock();
        auto portLock = port->Lock();
        oldOwner->RemoveOwningPort(id);
        newOwner->AddOwningPort(id);
        port->SetOwner(team);
    }

    PortTrace(context, "SETOWNER id=%d team=%d", id, team);
    return B_OK;
}

intptr_t server_hserver_call_write_port_etc(hserver_context& context, port_id id, int32 messageCode, const void *msgBuffer,
    size_t bufferSize, uint32 flags, unsigned long long timeout)
{
    std::shared_ptr<Port> port;

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();
        port = system.GetPort(id).lock();
    }

    if (!port)
    {
        PortTrace(context, "WRITE id=%d code=%d -> BAD_PORT_ID (not found)", id, messageCode);
        return B_BAD_PORT_ID;
    }

    // Don't need to acquire a lock, a race here is harmless.
    if (port->IsClosed())
    {
        PortTrace(context, "WRITE id=%d code=%d -> BAD_PORT_ID (closed)", id, messageCode);
        return B_BAD_PORT_ID;
    }

    Port::Message message;
    message.code = messageCode;
    message.data.resize(bufferSize);
    memset(&message.info, 0, sizeof(message.info));
    message.info.sender_team = context.pid;
    message.info.size = bufferSize;

    if (server_read_process_memory(context.pid, (void*)msgBuffer, message.data.data(), bufferSize)
        != bufferSize)
    {
        return B_BAD_ADDRESS;
    }

    bigtime_t relativeTimeout = server_relative_timeout(flags, timeout);

    status_t writeStatus = port->Write(std::move(message), relativeTimeout);
    if (writeStatus == B_WOULD_BLOCK && (flags & B_ABSOLUTE_TIMEOUT))
        writeStatus = B_TIMED_OUT;
    PortTrace(context, "WRITE id=%d code=%d size=%zu -> status=%d queue=%d",
        id, messageCode, bufferSize, (int)writeStatus, port->GetInfo().queue_count);
    return writeStatus;
}

intptr_t server_hserver_call_read_port_etc(hserver_context& context,
    port_id id, int32* userMessageCode, void *msgBuffer,
    size_t bufferSize, uint32 flags, unsigned long long timeout)
{
    std::shared_ptr<Port> port;

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();
        port = system.GetPort(id).lock();
    }

    if (!port)
    {
        return B_BAD_PORT_ID;
    }

    Port::Message message;

    bigtime_t relativeTimeout = server_relative_timeout(flags, timeout);

    PortTrace(context, "READ-enter id=%d queue=%d", id, port->GetInfo().queue_count);
    status_t status = port->Read(message, relativeTimeout);
    // Haiku reports an elapsed absolute deadline as B_TIMED_OUT, not B_WOULD_BLOCK.
    if (status == B_WOULD_BLOCK && (flags & B_ABSOLUTE_TIMEOUT))
        status = B_TIMED_OUT;
    PortTrace(context, "READ-exit  id=%d code=%d -> status=%d", id, message.code, (int)status);

    if (status != B_OK)
    {
        return status;
    }

    if (context.process->WriteMemory(userMessageCode, &message.code, sizeof(message.code)) != sizeof(message.code))
    {
        return B_BAD_ADDRESS;
    }

    size_t writeSize = std::min(message.data.size(), bufferSize);
    if (context.process->WriteMemory(msgBuffer, message.data.data(), writeSize) != writeSize)
    {
        return B_BAD_ADDRESS;
    }

    return writeSize;
}

intptr_t server_hserver_call_get_port_message_info_etc(hserver_context& context, int id,
    void* userPortMessageInfo, size_t infoSize, unsigned int flags, unsigned long long timeout)
{
    if (infoSize != sizeof(haiku_port_message_info))
    {
        return B_BAD_VALUE;
    }

    std::shared_ptr<Port> port;

    {
        auto& system = System::GetInstance();
        auto lock = system.Lock();
        port = system.GetPort(id).lock();
    }

    if (!port)
    {
        return B_BAD_PORT_ID;
    }

    haiku_port_message_info messageInfo;
    bigtime_t relativeTimeout = server_relative_timeout(flags, timeout);

    PortTrace(context, "MSGINFO-enter id=%d queue=%d timeout=%s", id,
        port->GetInfo().queue_count,
        relativeTimeout == B_INFINITE_TIMEOUT ? "INFINITE" : "yes");
    status_t status = port->GetMessageInfo(messageInfo, relativeTimeout);
    if (status == B_WOULD_BLOCK && (flags & B_ABSOLUTE_TIMEOUT))
        status = B_TIMED_OUT;
    PortTrace(context, "MSGINFO-exit  id=%d -> status=%d size=%d", id, (int)status,
        status == B_OK ? (int)messageInfo.size : -1);

    if (status != B_OK)
    {
        return status;
    }

    if (context.process->WriteMemory(userPortMessageInfo, &messageInfo, infoSize) != infoSize)
    {
        return B_BAD_ADDRESS;
    }

    return B_OK;
}