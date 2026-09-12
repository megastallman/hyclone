// Self-contained subset of Haiku's <private/audio/hmulti_audio.h>, sufficient to
// implement the virtual playback driver in hmulti_audio.cpp. monika builds
// against shared_headers/BeDefs.h (not the Haiku tree), so the structs are
// reproduced here with the exact field layout/order of the Haiku ABI. Keep in
// sync with headers/private/audio/hmulti_audio.h.
#ifndef __HYCLONE_MONIKA_HMULTI_AUDIO_H__
#define __HYCLONE_MONIKA_HMULTI_AUDIO_H__

#include "BeDefs.h"

// B_AUDIO_DRIVER_BASE (Drivers.h) = 8000; B_MULTI_DRIVER_BASE = +20.
#define B_MULTI_DRIVER_BASE (8000 + 20)

enum
{
    B_MULTI_GET_DESCRIPTION = B_MULTI_DRIVER_BASE,  // 8020
    B_MULTI_GET_EVENT_INFO,
    B_MULTI_SET_EVENT_INFO,
    B_MULTI_GET_EVENT,
    B_MULTI_GET_ENABLED_CHANNELS,
    B_MULTI_SET_ENABLED_CHANNELS,
    B_MULTI_GET_GLOBAL_FORMAT,
    B_MULTI_SET_GLOBAL_FORMAT,
    B_MULTI_GET_CHANNEL_FORMATS,
    B_MULTI_SET_CHANNEL_FORMATS,
    B_MULTI_GET_MIX,
    B_MULTI_SET_MIX,
    B_MULTI_LIST_MIX_CHANNELS,
    B_MULTI_LIST_MIX_CONTROLS,
    B_MULTI_LIST_MIX_CONNECTIONS,
    B_MULTI_GET_BUFFERS,
    B_MULTI_SET_BUFFERS,
    B_MULTI_SET_START_TIME,
    B_MULTI_BUFFER_EXCHANGE,
    B_MULTI_BUFFER_FORCE_STOP,
    B_MULTI_LIST_EXTENSIONS,
    B_MULTI_GET_EXTENSION,
    B_MULTI_SET_EXTENSION,
    B_MULTI_LIST_MODES,
    B_MULTI_GET_MODE,
    B_MULTI_SET_MODE
};

#define B_SR_48000 0x100
#define B_FMT_16BIT 0x10

#define B_MULTI_LOCK_INTERNAL 0x1
#define B_MULTI_INTERFACE_PLAYBACK 0x1

#define B_CURRENT_INTERFACE_VERSION 0x4502
#define B_MINIMUM_INTERFACE_VERSION 0x4502

#define B_CHANNEL_LEFT 0x1
#define B_CHANNEL_RIGHT 0x2

typedef enum
{
    B_MULTI_NO_CHANNEL_KIND,
    B_MULTI_OUTPUT_CHANNEL = 0x1,
    B_MULTI_INPUT_CHANNEL = 0x2,
    B_MULTI_OUTPUT_BUS = 0x4,
    B_MULTI_INPUT_BUS = 0x8,
    B_MULTI_AUX_BUS = 0x10
} channel_kind;

struct multi_channel_info
{
    int32 channel_id;
    channel_kind kind;
    uint32 designations;
    uint32 connectors;
    uint32 _reserved_[4];
};

struct multi_description
{
    size_t info_size;
    uint32 interface_version;
    uint32 interface_minimum;
    char friendly_name[32];
    char vendor_info[32];
    int32 output_channel_count;
    int32 input_channel_count;
    int32 output_bus_channel_count;
    int32 input_bus_channel_count;
    int32 aux_bus_channel_count;
    int32 request_channel_count;
    multi_channel_info* channels;
    uint32 output_rates;
    uint32 input_rates;
    float min_cvsr_rate;
    float max_cvsr_rate;
    uint32 output_formats;
    uint32 input_formats;
    uint32 lock_sources;
    uint32 timecode_sources;
    uint32 interface_flags;
    bigtime_t start_latency;
    uint32 _reserved_[11];
    char control_panel[64];
};

struct multi_channel_enable
{
    size_t info_size;
    uchar* enable_bits;
    uint32 lock_source;
    int32 lock_data;
    uint32 timecode_source;
    uint32* connectors;
};

struct _multi_format
{
    uint32 rate;
    float cvsr;
    uint32 format;
    uint32 _reserved_[3];
};

struct multi_format_info
{
    size_t info_size;
    bigtime_t output_latency;
    bigtime_t input_latency;
    int32 timecode_kind;
    uint32 _reserved_[7];
    _multi_format input;
    _multi_format output;
};

struct buffer_desc
{
    char* base;
    size_t stride;
    uint32 _reserved_[2];
};

struct multi_buffer_list
{
    size_t info_size;
    uint32 flags;
    int32 request_playback_buffers;
    int32 request_playback_channels;
    uint32 request_playback_buffer_size;
    int32 return_playback_buffers;
    int32 return_playback_channels;
    uint32 return_playback_buffer_size;
    buffer_desc** playback_buffers;
    void* _reserved_1;
    int32 request_record_buffers;
    int32 request_record_channels;
    uint32 request_record_buffer_size;
    int32 return_record_buffers;
    int32 return_record_channels;
    uint32 return_record_buffer_size;
    buffer_desc** record_buffers;
    void* _reserved_2;
};

struct multi_buffer_info
{
    size_t info_size;
    uint32 flags;
    bigtime_t played_real_time;
    bigtime_t played_frames_count;
    int32 _reserved_0;
    int32 playback_buffer_cycle;
    bigtime_t recorded_real_time;
    bigtime_t recorded_frames_count;
    int32 _reserved_1;
    int32 record_buffer_cycle;
    int32 meter_channel_count;
    char* meters_peak;
    char* meters_average;
    int32 hours;
    int32 minutes;
    int32 seconds;
    int32 tc_frames;
    int32 at_frame_delta;
};

struct multi_mix_control_info
{
    size_t info_size;
    int32 control_count;
    void* controls;
};

#endif // __HYCLONE_MONIKA_HMULTI_AUDIO_H__
