// Server-side audio sink for HyClone's virtual hmulti_audio driver.
//
// The guest driver (monika/linux/hmulti_audio.cpp) hands each filled playback
// buffer to the server via the audio_open/write/close servercalls. Here we read
// that buffer out of the guest with process_vm_readv (as every servercall does)
// and push it to the host through PulseAudio's simple API. libpulse-simple is
// loaded lazily with dlopen(), so the server keeps no build/link dependency on
// it; under a PipeWire desktop it reaches pipewire-pulse. If it is unavailable,
// audio_open() fails and the guest falls back to its clock-paced null sink.
//
// One playback stream per guest process (keyed by pid): only media_addon_server
// drives audio, but keying by pid keeps it correct and self-cleaning.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <unistd.h>
#include <vector>

#include <pulse/simple.h>
#include <pulse/error.h>

#include "haiku_errors.h"
#include "process.h"
#include "server_servercalls.h"

namespace {

typedef pa_simple* (*pa_simple_new_t)(const char*, const char*,
    pa_stream_direction_t, const char*, const char*, const pa_sample_spec*,
    const pa_channel_map*, const pa_buffer_attr*, int*);
typedef int (*pa_simple_write_t)(pa_simple*, const void*, size_t, int*);
typedef int (*pa_simple_drain_t)(pa_simple*, int*);
typedef void (*pa_simple_free_t)(pa_simple*);

std::mutex sLock;
bool sTried = false;
bool sAvailable = false;
pa_simple_new_t sNew = nullptr;
pa_simple_write_t sWrite = nullptr;
pa_simple_drain_t sDrain = nullptr;
pa_simple_free_t sFree = nullptr;

std::map<int, pa_simple*> sStreams;

bool LoadSymbols()
{
    if (sTried)
        return sAvailable;
    sTried = true;

    void* handle = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr)
    {
        return false;
    }

    sNew = (pa_simple_new_t)dlsym(handle, "pa_simple_new");
    sWrite = (pa_simple_write_t)dlsym(handle, "pa_simple_write");
    sDrain = (pa_simple_drain_t)dlsym(handle, "pa_simple_drain");
    sFree = (pa_simple_free_t)dlsym(handle, "pa_simple_free");

    sAvailable = sNew != nullptr && sWrite != nullptr && sDrain != nullptr
        && sFree != nullptr;
    return sAvailable;
}

void CloseLocked(int pid)
{
    auto it = sStreams.find(pid);
    if (it == sStreams.end())
        return;
    if (it->second != nullptr)
    {
        int error = 0;
        sDrain(it->second, &error);
        sFree(it->second);
    }
    sStreams.erase(it);
}

} // namespace

intptr_t server_hserver_call_audio_open(hserver_context& context,
    unsigned int rate, unsigned int channels, unsigned int sampleBits)
{
    std::lock_guard<std::mutex> guard(sLock);


    if (sStreams.find(context.pid) != sStreams.end())
        return B_OK;
    if (!LoadSymbols())
    {
        return B_NOT_SUPPORTED;
    }

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;   // guest advertises 16-bit only
    ss.rate = rate;
    ss.channels = (uint8_t)channels;

    // Keep the server-side buffer modest so pa_simple_write() blocks and paces
    // the guest near real time instead of buffering seconds ahead.
    pa_buffer_attr attr;
    unsigned int frameSize = channels * (sampleBits / 8);
    attr.maxlength = (uint32_t)-1;
    attr.tlength = frameSize * (rate / 8);   // ~125 ms target latency
    attr.prebuf = (uint32_t)-1;
    attr.minreq = (uint32_t)-1;
    attr.fragsize = (uint32_t)-1;

    // libpulse locates the server via the PULSE_SERVER / XDG_RUNTIME_DIR environment,
    // but hyclone_server is typically forked by haiku_loader without a desktop session's
    // environment. When neither variable is set, fall back to the standard systemd
    // per-user runtime socket so a PipeWire/PulseAudio session is reachable out of the box.
    // A NULL server keeps libpulse's own discovery when the environment does provide a hint.
    char serverPath[64];
    const char* server = nullptr;
    if (getenv("PULSE_SERVER") == nullptr && getenv("XDG_RUNTIME_DIR") == nullptr)
    {
        snprintf(serverPath, sizeof(serverPath),
            "unix:/run/user/%u/pulse/native", (unsigned int)getuid());
        server = serverPath;
    }

    int error = 0;
    pa_simple* stream = sNew(server, "HyClone", PA_STREAM_PLAYBACK, nullptr,
        "Haiku audio", &ss, nullptr, &attr, &error);
    if (stream == nullptr)
    {
        return B_NOT_SUPPORTED;
    }

    sStreams[context.pid] = stream;
    return B_OK;
}

intptr_t server_hserver_call_audio_write(hserver_context& context,
    const void* buffer, size_t size)
{
    pa_simple* stream;
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto it = sStreams.find(context.pid);
        if (it == sStreams.end() || it->second == nullptr)
            return B_ERROR;
        stream = it->second;
    }


    if (size == 0)
        return B_OK;
    if (size > (1u << 20))
        return B_BAD_VALUE;

    std::vector<uint8_t> data(size);
    if (context.process->ReadMemory((void*)buffer, data.data(), size) != size)
        return B_BAD_ADDRESS;

    // pa_simple_write() blocks until the sink accepts the data (pacing). Done
    // without sLock held so it never stalls this process's other servercalls.
    int error = 0;
    if (sWrite(stream, data.data(), size, &error) < 0)
    {
        std::lock_guard<std::mutex> guard(sLock);
        CloseLocked(context.pid);
        return B_IO_ERROR;
    }
    return B_OK;
}

intptr_t server_hserver_call_audio_close(hserver_context& context)
{
    std::lock_guard<std::mutex> guard(sLock);
    CloseLocked(context.pid);
    return B_OK;
}

// Called from the connection-teardown path so a guest that dies without
// closing its device does not leak its stream.
void server_audio_cleanup(int pid)
{
    std::lock_guard<std::mutex> guard(sLock);
    if (sAvailable)
        CloseLocked(pid);
}
