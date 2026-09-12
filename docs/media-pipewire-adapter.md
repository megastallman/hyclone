# Audio for HyClone: media_kit → PipeWire adapter

Goal: play sound in guest media apps (cmus, ocp, and any BSoundPlayer app) by
routing Haiku's media_kit output to the host's PipeWire.

## Key finding: where the adapter belongs

Do NOT reimplement media_server / media_addon_server. They are ordinary Haiku
server binaries and already RUN under HyClone (they pair up once
`_kern_estimate_max_scheduling_latency` exists and they start in the right
order). Both target players need the full media_kit:

- cmus → libao, whose only backend here is `libhaiku.so` (BSoundPlayer → media_kit).
- ocp → SDL3 (`35-devpsdl3.so`, SDL Haiku audio = BSoundPlayer) or software mixers.

So the correct, minimal adapter is a **virtual `hmulti_audio` hardware device**
inside hyclone_server, bridged to PipeWire. The unmodified Haiku stack runs on
top: BSoundPlayer → System Mixer (mixer.media_addon) → hmulti_audio.media_addon
→ our virtual `/dev/audio/hmulti/...` device → PipeWire playback stream.

This reuses Haiku's mixer, format negotiation, node graph and time source. The
device is the audio clock: `B_MULTI_BUFFER_EXCHANGE` must BLOCK until the host
sink drains one buffer.

## HyClone integration points (verified)

- Driver ioctls (opcode > `B_DEVICE_OP_CODES_END`) already forward from the guest
  (`monika/linux/ioctl.cpp` default case) to `GET_SERVERCALLS()->ioctl`, handled
  by `server_hserver_call_ioctl` (hyclone_server/server_filesystem.cpp) which
  looks up the fd's path and calls `VfsService::Ioctl(path, op, addr, buf, size)`
  → `VfsDevice::Ioctl`. multi_audio moves PCM via ioctl + shared buffers, NOT
  read/write, so ioctl forwarding is sufficient for control + exchange.
- `/dev` is a pure host passthrough (`fs/devfs.cpp` = HostfsDevice /dev→/dev), so
  a virtual node under `/dev/audio/hmulti/` has no host file. Two sub-tasks:
  (a) make the guest `open()` of the virtual leaf succeed and yield a usable fd
  whose ioctls route to the server, and (b) route that fd's ioctls to the new
  device handler. Simplest: give devfs a small virtual-node table; back the open
  with a host fd we control (e.g. an eventfd/memfd) registered to the virtual
  path so ioctls dispatch by path in VfsService::Ioctl.
- PCM buffers: allocate a HyClone shared area (HYCLONE_SHM_NAME mechanism, see
  server_memory.cpp / loader area cloning), map into the guest, hand its guest
  addresses back from `B_MULTI_GET_BUFFERS`; the server's PipeWire thread reads
  the same memory host-side.

## Driver contract (headers/private/audio/hmulti_audio.h)

Opcodes are a sequential enum from `B_MULTI_DRIVER_BASE = B_DEVICE_OP_CODES_END
+ 10000 + 20`. The add-on (src/add-ons/media/media-add-ons/multi_audio) calls
`ioctl(fd, op, data, sizeof(struct))` — the 4th arg is the struct size.

Minimum viable set to implement:
- GET_DESCRIPTION (multi_description): advertise 2 out + 2 in channels,
  output_formats = B_FMT_16BIT only, output_rates = one bit (B_SR_48000),
  interface_version 0x4502, interface_flags = B_MULTI_INTERFACE_PLAYBACK. The
  add-on's select_format skips B_FMT_FLOAT and picks the highest int depth; its
  select_sample_rate picks the highest advertised rate — so advertise exactly
  what you want.
- GET/SET_ENABLED_CHANNELS (multi_channel_enable): echo.
- SET/GET_GLOBAL_FORMAT (multi_format_info): accept and echo back, fill latencies.
- GET_BUFFERS (multi_buffer_list): request is buffers=32, channels=out_count,
  size=0 (driver picks frames/buffer). Return e.g. 2 buffers; fill
  playback_buffers[cycle][channel] = {base, stride}. Interleaved 16-bit stereo:
  base = area + cycle*bufBytes + channel*2, stride = channels*2.
- LIST_MIX_CONTROLS (multi_mix_control_info): return 0 controls, B_OK (init fails
  otherwise).
- BUFFER_EXCHANGE (multi_buffer_info): BLOCK until PipeWire consumed one buffer,
  then set playback_buffer_cycle (just-completed index), played_real_time
  (system_time()), played_frames_count (running total). The add-on fills the
  buffer one behind the reported cycle. No snooze in its loop — the driver paces.
- SET_START_TIME / BUFFER_FORCE_STOP: no-op.

Discovery: MultiAudioAddOn scans /dev/audio/hmulti/ recursively; any non-dir leaf
that passes open+probe is used. Leaf name is cosmetic.

## Phased plan

0. Media servers run (DONE in part): `_kern_estimate_max_scheduling_latency`
   implemented in monika/linux/threading.cpp. Still to do: make media_server
   auto-start — its launch job (data/launch/system, x-vnd.Haiku-media_server)
   is gated on `initial_volumes_mounted`, which never fires under HyClone; either
   emit that event at boot or change the trigger. media_addon_server is spawned
   by media_server. Start order matters (media_server creates __media_server_port
   before media_addon_server's find_port).
1. Virtual devfs node infrastructure: publish /dev/audio/hmulti/hyclone/0, make
   open succeed, route its ioctls to a new HmultiAudioDevice handler.
2. HmultiAudioDevice in hyclone_server: implement the ioctls above with a shared
   buffer area; first ship a NULL sink that paces via a monotonic timer and
   discards PCM. Milestone: cmus/ocp play end-to-end (silently). Fully testable.
3. Swap the null sink for a PipeWire playback stream (link libpipewire-0.3 into
   hyclone_server; a pw_stream in a pw_thread_loop; on process, copy the next
   guest buffer; unblock BUFFER_EXCHANGE from the PipeWire on-process callback).
4. Format/rate handling, latency reporting, xrun handling; optional record.

## Related blockers (separate from audio)

- Guest AF_UNIX sockets return ENOSYS (socket family not wired through
  SocketFamilyBToLinux in monika/linux/socket_conversion.cpp even though the
  sockaddr_un conversion exists). cmus-remote and cmus's control socket need it;
  cmus can still run interactively without it. Fix is independent of audio.
- app_server remote-display backpressure (port 10900) can hang BApplications
  when no viewer drains it; keep a viewer connected during testing.
