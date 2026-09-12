// Guest-side virtual "hmulti_audio" sound driver for HyClone.
//
// Haiku's media stack (BSoundPlayer -> System Mixer -> hmulti_audio.media_addon)
// drives an audio device under /dev/audio/hmulti/ purely through B_MULTI_* ioctls
// plus shared playback buffers. HyClone has no such device, so this file
// implements the driver contract in-process: the ioctls arrive in the add-on's
// own address space (media_addon_server), where monika can service them directly.
//
// The multi_audio ioctl opcodes (B_MULTI_DRIVER_BASE = B_AUDIO_DRIVER_BASE + 20
// = 8020) sit BELOW B_DEVICE_OP_CODES_END (9999), so monika's ioctl dispatcher
// does not forward them to hyclone_server; they are handled here instead.
//
// This is Phase 2 of docs/media-pipewire-adapter.md: a NULL SINK. Playback is
// paced against the monotonic clock (so the media time source advances at real
// time and apps play at the correct speed) but the PCM is discarded. Phase 3
// replaces the discard with a hand-off of each filled buffer to a PipeWire
// stream in hyclone_server.

#include <cstddef>
#include <cstring>
#include <sys/mman.h>
#include <ctime>
#include <time.h>

#include "BeDefs.h"
#include "extended_commpage.h"
#include "haiku_errors.h"
#include "hmulti_audio.h"
#include "linux_debug.h"
#include "linux_syscall.h"
#include "stringutils.h"

// ---------------------------------------------------------------------------
// Device configuration (what we advertise). Keep it minimal and robust: stereo
// 16-bit at a single rate. The add-on's select_format() skips float and picks
// the highest integer depth advertised; select_sample_rate() picks the highest
// rate advertised -- so advertise exactly what we want.
// ---------------------------------------------------------------------------
static const uint32 kChannels = 2;
static const uint32 kSampleSize = 2;      // bytes, B_FMT_16BIT
static const uint32 kFramesPerBuffer = 2048;
static const int32 kBufferCount = 2;
static const uint32 kRateHz = 48000;

struct AudioDevice
{
    int fd;                 // -1 == free slot
    bool running;
    void* buffers;          // mmap'd: kBufferCount * kChannels * kFramesPerBuffer * kSampleSize
    size_t buffersSize;
    bigtime_t startTime;    // system_time() when playback began
    int64 framesPlayed;
    int32 cycle;
};

static const int kMaxDevices = 8;
static AudioDevice sDevices[kMaxDevices];
static volatile int sLock = 0;
static bool sInitialized = false;

namespace {

// Minimal spinlock via compiler atomics -- avoids pulling libstdc++ runtime
// (std::atomic_flag) into monika. Contention is negligible: the add-on's init
// thread and its single real-time output thread touch one device sequentially.
class SpinLock
{
public:
    SpinLock() { while (__atomic_exchange_n(&sLock, 1, __ATOMIC_ACQUIRE)) {} }
    ~SpinLock() { __atomic_store_n(&sLock, 0, __ATOMIC_RELEASE); }
};

void EnsureInit()
{
    if (sInitialized)
        return;
    for (int i = 0; i < kMaxDevices; ++i)
        sDevices[i].fd = -1;
    sInitialized = true;
}

AudioDevice* Find(int fd)
{
    for (int i = 0; i < kMaxDevices; ++i)
        if (sDevices[i].fd == fd)
            return &sDevices[i];
    return NULL;
}

AudioDevice* FindOrCreate(int fd)
{
    AudioDevice* dev = Find(fd);
    if (dev != NULL)
        return dev;
    for (int i = 0; i < kMaxDevices; ++i)
    {
        if (sDevices[i].fd == -1)
        {
            AudioDevice* d = &sDevices[i];
            memset(d, 0, sizeof(*d));
            d->fd = fd;
            return d;
        }
    }
    return NULL;
}

// Is this fd one of our virtual audio devices? Decided once, on the first
// B_MULTI_GET_DESCRIPTION, by asking the server for the fd's guest path.
bool IsAudioDevice(int fd)
{
    char path[256];
    long len = GET_HOSTCALLS()->vchroot_unexpandat(fd, "", path, sizeof(path));
    if (len <= 0 || (size_t)len >= sizeof(path))
        return false;
    // vchroot_unexpandat yields a host-prefix-relative path, so the mount over
    // /dev/audio/hmulti shows up as its backing dir. Accept either the mount
    // path or the private backing marker (nothing else uses ".hyclone.audio").
    return strncmp(path, "/dev/audio/hmulti/", 18) == 0
        || strstr(path, ".hyclone.audio") != NULL;
}

void SleepUntil(bigtime_t targetSystemTime)
{
    bigtime_t now = GET_HOSTCALLS()->system_time();
    bigtime_t delta = targetSystemTime - now;
    if (delta <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = delta / 1000000;
    ts.tv_nsec = (delta % 1000000) * 1000;
    // Relative sleep on the monotonic clock (same domain as system_time()).
    LINUX_SYSCALL4(__NR_clock_nanosleep, CLOCK_MONOTONIC, 0, &ts, NULL);
}

} // namespace

extern "C" bool _moni_hmulti_audio_ioctl(int fd, uint32 op, void* buffer, size_t length,
    status_t* result)
{
    // Fast reject: only opcodes in the multi_audio range are ours.
    if (op < (uint32)B_MULTI_GET_DESCRIPTION || op > (uint32)B_MULTI_SET_MODE)
        return false;

    SpinLock guard;
    EnsureInit();

    AudioDevice* dev = Find(fd);
    if (dev == NULL)
    {
        // First multi_audio ioctl on this fd. Only claim it if it really is one
        // of our /dev/audio/hmulti/ nodes; otherwise let the normal path deal
        // with it (returns "unknown ioctl").
        if (op != (uint32)B_MULTI_GET_DESCRIPTION || !IsAudioDevice(fd))
            return false;
        dev = FindOrCreate(fd);
        if (dev == NULL)
        {
            *result = B_NO_MEMORY;
            return true;
        }
    }

    switch (op)
    {
        case B_MULTI_GET_DESCRIPTION:
        {
            multi_description* desc = (multi_description*)buffer;
            desc->interface_version = B_CURRENT_INTERFACE_VERSION;
            desc->interface_minimum = B_MINIMUM_INTERFACE_VERSION;
            strncpy(desc->friendly_name, "HyClone Virtual Audio", sizeof(desc->friendly_name) - 1);
            desc->friendly_name[sizeof(desc->friendly_name) - 1] = '\0';
            strncpy(desc->vendor_info, "HyClone", sizeof(desc->vendor_info) - 1);
            desc->vendor_info[sizeof(desc->vendor_info) - 1] = '\0';
            desc->output_channel_count = kChannels;
            desc->input_channel_count = 0;
            desc->output_bus_channel_count = 0;
            desc->input_bus_channel_count = 0;
            desc->aux_bus_channel_count = 0;
            desc->output_rates = B_SR_48000;
            desc->input_rates = 0;
            desc->min_cvsr_rate = 0;
            desc->max_cvsr_rate = 0;
            desc->output_formats = B_FMT_16BIT;
            desc->input_formats = 0;
            desc->lock_sources = B_MULTI_LOCK_INTERNAL;
            desc->timecode_sources = 0;
            desc->interface_flags = B_MULTI_INTERFACE_PLAYBACK;
            desc->start_latency = 0;

            int32 want = desc->request_channel_count;
            if (want > (int32)kChannels && desc->channels != NULL)
            {
                for (uint32 i = 0; i < kChannels; ++i)
                {
                    desc->channels[i].channel_id = i;
                    desc->channels[i].kind = B_MULTI_OUTPUT_CHANNEL;
                    desc->channels[i].designations = (i == 0) ? B_CHANNEL_LEFT : B_CHANNEL_RIGHT;
                    desc->channels[i].connectors = 0;
                }
            }
            *result = B_OK;
            return true;
        }
        case B_MULTI_GET_ENABLED_CHANNELS:
        {
            multi_channel_enable* mce = (multi_channel_enable*)buffer;
            if (mce->enable_bits != NULL)
            {
                // All output channels enabled: low kChannels bits set.
                mce->enable_bits[0] = (uchar)((1 << kChannels) - 1);
            }
            mce->lock_source = B_MULTI_LOCK_INTERNAL;
            *result = B_OK;
            return true;
        }
        case B_MULTI_SET_ENABLED_CHANNELS:
            // Accept whatever the add-on asks for.
            *result = B_OK;
            return true;
        case B_MULTI_GET_GLOBAL_FORMAT:
        case B_MULTI_SET_GLOBAL_FORMAT:
        {
            multi_format_info* mfi = (multi_format_info*)buffer;
            // We only support one format; report/accept it regardless of request.
            mfi->output.rate = B_SR_48000;
            mfi->output.cvsr = 0;
            mfi->output.format = B_FMT_16BIT;
            mfi->input.rate = 0;
            mfi->input.cvsr = 0;
            mfi->input.format = 0;
            // One buffer of latency is a reasonable estimate.
            mfi->output_latency = (bigtime_t)kFramesPerBuffer * 1000000 / kRateHz;
            mfi->input_latency = 0;
            mfi->timecode_kind = 0;
            *result = B_OK;
            return true;
        }
        case B_MULTI_GET_BUFFERS:
        {
            multi_buffer_list* list = (multi_buffer_list*)buffer;

            if (dev->buffers == NULL)
            {
                dev->buffersSize = (size_t)kBufferCount * kChannels * kFramesPerBuffer * kSampleSize;
                long mapped = LINUX_SYSCALL6(__NR_mmap, NULL, dev->buffersSize,
                    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (mapped < 0)
                {
                    *result = B_NO_MEMORY;
                    return true;
                }
                dev->buffers = (void*)mapped;
                memset(dev->buffers, 0, dev->buffersSize);
            }

            list->return_playback_buffers = kBufferCount;
            list->return_playback_channels = kChannels;
            list->return_playback_buffer_size = kFramesPerBuffer;
            list->return_record_buffers = 0;
            list->return_record_channels = 0;
            list->return_record_buffer_size = 0;

            // Interleaved 16-bit stereo. For buffer b, channel c:
            //   base   = buffers + b*(frames*channels*sampleSize) + c*sampleSize
            //   stride = channels*sampleSize
            const size_t bufferBytes = (size_t)kFramesPerBuffer * kChannels * kSampleSize;
            const size_t stride = (size_t)kChannels * kSampleSize;
            char* audioBase = (char*)dev->buffers;
            if (list->playback_buffers != NULL)
            {
                for (int32 b = 0; b < kBufferCount; ++b)
                {
                    for (uint32 c = 0; c < kChannels; ++c)
                    {
                        list->playback_buffers[b][c].base =
                            audioBase + b * bufferBytes + c * kSampleSize;
                        list->playback_buffers[b][c].stride = stride;
                    }
                }
            }
            *result = B_OK;
            return true;
        }
        case B_MULTI_LIST_MIX_CONTROLS:
        {
            multi_mix_control_info* mmci = (multi_mix_control_info*)buffer;
            mmci->control_count = 0;
            *result = B_OK;
            return true;
        }
        case B_MULTI_BUFFER_EXCHANGE:
        {
            multi_buffer_info* info = (multi_buffer_info*)buffer;

            if (!dev->running)
            {
                dev->running = true;
                dev->startTime = GET_HOSTCALLS()->system_time();
                dev->framesPlayed = 0;
                dev->cycle = 0;
            }

            // Advance one buffer worth of time and pace to it (drift-free:
            // deadline is anchored to startTime, not to "now").
            dev->framesPlayed += kFramesPerBuffer;
            bigtime_t deadline = dev->startTime
                + (bigtime_t)dev->framesPlayed * 1000000 / kRateHz;
            SleepUntil(deadline);

            info->played_real_time = GET_HOSTCALLS()->system_time();
            info->played_frames_count = dev->framesPlayed;
            info->playback_buffer_cycle = dev->cycle;
            info->recorded_real_time = 0;
            info->recorded_frames_count = 0;
            info->record_buffer_cycle = 0;

            dev->cycle = (dev->cycle + 1) % kBufferCount;
            *result = B_OK;
            return true;
        }
        case B_MULTI_BUFFER_FORCE_STOP:
            dev->running = false;
            *result = B_OK;
            return true;
        case B_MULTI_SET_START_TIME:
            *result = B_OK;
            return true;
        default:
            // Optional ioctls the add-on's minimal path does not require
            // (channel formats, mix get/set, extensions, modes...). Accept them
            // as harmless no-ops so device init does not fail.
            *result = B_OK;
            return true;
    }
}

extern "C" void _moni_hmulti_audio_close(int fd)
{
    SpinLock guard;
    EnsureInit();
    AudioDevice* dev = Find(fd);
    if (dev == NULL)
        return;
    if (dev->buffers != NULL)
        LINUX_SYSCALL2(__NR_munmap, dev->buffers, dev->buffersSize);
    dev->fd = -1;
    dev->buffers = NULL;
}
