// Host-side PulseAudio sink for HyClone's virtual hmulti_audio driver.
//
// libpulse-simple is loaded lazily with dlopen(): the loader keeps no build- or
// link-time dependency on it, and if it (or a running sound server) is missing,
// audio_open() fails cleanly and the guest driver falls back to a clock-paced
// null sink. Under a PipeWire desktop, libpulse-simple talks to pipewire-pulse.

#include "loader_audio.h"

#include <dlfcn.h>
#include <mutex>

#include <pulse/simple.h>
#include <pulse/error.h>

#include "BeDefs.h"
#include "haiku_errors.h"

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
pa_simple* sStream = nullptr;

bool LoadSymbols()
{
    if (sTried)
        return sAvailable;
    sTried = true;

    void* handle = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr)
        return false;

    sNew = (pa_simple_new_t)dlsym(handle, "pa_simple_new");
    sWrite = (pa_simple_write_t)dlsym(handle, "pa_simple_write");
    sDrain = (pa_simple_drain_t)dlsym(handle, "pa_simple_drain");
    sFree = (pa_simple_free_t)dlsym(handle, "pa_simple_free");

    sAvailable = sNew && sWrite && sDrain && sFree;
    return sAvailable;
}

} // namespace

int loader_audio_open(uint32_t rate, uint32_t channels, uint32_t sampleBits)
{
    std::lock_guard<std::mutex> guard(sLock);

    if (sStream != nullptr)
        return B_OK;
    if (!LoadSymbols())
        return B_NOT_SUPPORTED;

    pa_sample_spec ss;
    ss.format = (sampleBits == 16) ? PA_SAMPLE_S16LE : PA_SAMPLE_S16LE;
    ss.rate = rate;
    ss.channels = (uint8_t)channels;

    // Keep the server-side buffer modest so pa_simple_write() blocks and paces
    // guest playback near real time instead of buffering seconds ahead.
    pa_buffer_attr attr;
    uint32_t frameSize = channels * (sampleBits / 8);
    attr.maxlength = (uint32_t)-1;
    attr.tlength = frameSize * (rate / 8);   // ~125 ms target latency
    attr.prebuf = (uint32_t)-1;
    attr.minreq = (uint32_t)-1;
    attr.fragsize = (uint32_t)-1;

    int error = 0;
    sStream = sNew(nullptr, "HyClone", PA_STREAM_PLAYBACK, nullptr,
        "Haiku audio", &ss, nullptr, &attr, &error);
    if (sStream == nullptr)
        return B_NOT_SUPPORTED;

    return B_OK;
}

int loader_audio_write(const void* data, size_t size)
{
    // No lock on the hot path: only the guest's single audio output thread
    // calls write(), and open()/close() bracket the whole playback session.
    if (sStream == nullptr || sWrite == nullptr)
        return B_ERROR;

    int error = 0;
    if (sWrite(sStream, data, size, &error) < 0)
        return B_IO_ERROR;
    return B_OK;
}

void loader_audio_close()
{
    std::lock_guard<std::mutex> guard(sLock);

    if (sStream == nullptr)
        return;
    int error = 0;
    if (sDrain != nullptr)
        sDrain(sStream, &error);
    if (sFree != nullptr)
        sFree(sStream);
    sStream = nullptr;
}
