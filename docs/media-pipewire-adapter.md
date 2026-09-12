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

## Status (2026-09-12)

Phase 0 DONE: `_kern_estimate_max_scheduling_latency` implemented; media_server +
media_addon_server pair up. Also implemented `_kern_find_area` (was a fatal stub
the media_kit's shared BBufferGroups need).

Phase 1 DONE: a virtual node is published at /dev/audio/hmulti/0 (SystemfsDevice
mount over the host passthrough, backed by $HPREFIX/.hyclone.audio). The guest
opens it and its B_MULTI_* ioctls (opcodes 8020+, which fall BELOW
B_DEVICE_OP_CODES_END so monika does not forward them to the server) are serviced
in-process by monika/linux/hmulti_audio.cpp.

Phase 2 SUBSTANTIALLY DONE (null sink): the guest driver implements the full
B_MULTI_* contract and paces B_MULTI_BUFFER_EXCHANGE against the monotonic clock
(verified: 5 exchanges of 2048 frames @ 48 kHz took 213 ms). Haiku's hmulti_audio
media add-on discovers and fully probes it -- the complete init sequence runs on
the real stack: GET_DESCRIPTION -> GET/SET_ENABLED_CHANNELS -> SET/GET_GLOBAL_FORMAT
-> GET_BUFFERS (2 x 2048 x 2ch) -> LIST_MIX_CONTROLS. A MultiAudioNode is created
in media_server.

ROOT CAUSE FOUND (2026-09-12, via a traced media_server): the media stack fully
recognizes the device -- our MultiAudioNode is created, discovered, probed, and is
the default audio output (verified with a BMediaRoster probe: GetAudioOutput
returns "HyClone Virtual Audio", GetLiveNodes lists it). The blocker is NOT the
audio driver. It is HyClone cross-team media PORT MESSAGING during node connection:
DefaultManager::_ConnectMixerToOutput (and BSoundPlayer::_Init) send connection
messages (write_port) to node control ports across teams, and these intermittently
return B_BAD_PORT_ID -- the target port is not in the server registry at that
instant -- or block. It is timing-dependent (a race): some runs BSoundPlayer::_Init
returns "General system error" (what cmus/ocp would surface as "can't open audio"),
some runs it hangs in the BSoundPlayer constructor. Port ids are globally unique
(System::_ports IdMap), so it is not an id collision; it is a registration/visibility
timing race, the same family as the launch_roster cross-team port hang. This is the
next thing to fix -- in HyClone's port layer, not the media code. Tooling: media_server
can be built standalone with tracing via the cross-compiler (see the recipe note in
memory); -DDEBUG=2 turns on the DefaultManager TRACE that pinpointed this.

Phase 3 (PipeWire) is unblocked only after the connection race is fixed, since no
buffers flow until the mixer connects to the node.

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
