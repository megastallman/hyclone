#ifndef __LOADER_AUDIO_H__
#define __LOADER_AUDIO_H__

#include <cstddef>
#include <cstdint>

// Host-side audio sink for HyClone's virtual hmulti_audio driver.
// Backed by PulseAudio's simple API (libpulse-simple, loaded lazily via
// dlopen so the loader has no hard dependency on it). Under a PipeWire
// desktop this reaches the pipewire-pulse server transparently.
//
// audio_open() is idempotent (a process has a single playback stream).
// audio_write() blocks until the sink accepts the data, which paces guest
// playback. All return B_OK (0) or a negative Haiku error.
int loader_audio_open(uint32_t rate, uint32_t channels, uint32_t sampleBits);
int loader_audio_write(const void* data, size_t size);
void loader_audio_close();

#endif // __LOADER_AUDIO_H__
