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

*** RESOLVED (2026-09-13) *** The media_addon_server crash-on-connect is FIXED.

Root cause (found by an instrumented mixer add-on, checkpoint by checkpoint):
media_addon_server was SIGSEGV'ing -- not exit()/debugger() as earlier guessed; it
only looked "silent" because /proc/sys/kernel/print-fatal-signals is 0 so segfaults
are not logged to dmesg. The fault chain, on the FIRST BSoundPlayer connect:
  AudioMixer::Connected()
    -> MixerCore::AddInput -> new MixerInput(...)
      -> MixerInput::SetMixBufferFormat(48000, 2048)
        -> rtm_create_pool(&pool, size)            [src/kits/media/RealtimeAlloc.cpp]
          -> create_area(name == NULL, ...)        [rtm_create_pool passes NULL name]
            -> _moni_create_area()                 [hyclone monika/linux/mman.cpp]
              -> strlcpy(info.name, NULL, ...)      -> NULL deref -> SIGSEGV
HyClone's strlcpy (monika/linux/stringutils.cpp) reads *src with no NULL check, and
create_area()/clone_area()/reserve on Haiku legitimately accept a NULL (unnamed)
area name -- rtm_create_pool relies on that. So ANY guest that creates an unnamed
area crashed here, not just the mixer.

Fix: NULL-guard the three `strlcpy(info.name, name, ...)` sites in _moni_create_area
(monika/linux/mman.cpp) -> `name != NULL ? name : ""`. Rebuild libroot
(`make root`), deploy to build/lib/libroot.so and $HPREFIX/boot/system/lib/libroot.so
(the boot restores libroot from build/lib, so the fix must live there too).

Verified: with the fix, media_addon_server SURVIVES the connect, AudioMixer::Connected()
runs to completion, and sptest reports "BSoundPlayer InitCheck: No error" and keeps
playing (buffers flow to the hmulti_audio null sink). media_server + media_addon_server
stay healthy across repeated connects. This unblocks Phase 3 (PipeWire): the media_kit
pipeline now connects and runs end-to-end (silently, into the null sink).

Debug scaffolding used to find this (now reverted / removable): an instrumented
mixer.media_addon with raw-write checkpoints, deployable from
$HPREFIX/boot/home/config/non-packaged/add-ons/media/ (build kit kept in
~/mixer_dbg_build/), and the SIGUSR1-armed hyclone_server port trace.

--- historical analysis below (superseded in part by the RESOLVED section above) ---

ROOT CAUSE (2026-09-12, refined via instrumented media_server AND media_addon_server):
the media stack fully recognizes the device -- our MultiAudioNode is created,
discovered, probed, and is the default audio output (verified with a BMediaRoster
probe: GetAudioOutput returns "HyClone Virtual Audio", GetLiveNodes lists it). The
blocker is NOT the audio driver.

The primary observable failure is that **media_addon_server's team dies during a
client's BSoundPlayer connect**. media_addon_server owns three nodes -- the System
clock (time source), the Audio Mixer, and our HyClone Virtual Audio output -- so when
its team goes away, NodeManager::CleanupTeam removes ALL THREE, and from that point
every audio client's GetAudioMixer returns B_ERROR ("can't open audio"). A probe app
seeing "nodes=0" afterwards is downstream of this collapse. The death reproduces
reliably: right after the client (sptest, a minimal BSoundPlayer) does GetClone
AUDIO_MIXER and registers its producer node, media_addon_server tears down.

What was ruled OUT by instrumentation (do not re-chase these):
- NOT media_server quitting it. A traced media_server (fprintf markers in ReadyToRun,
  QuitRequested, _QuitAddOnServer) shows _QuitAddOnServer runs ONCE at startup
  (running=0) and is NOT called at the moment of death. media_server only reacts to
  the death afterwards: it receives B_SOME_APP_QUIT ('BRAQ' = 1112686929) from the
  registrar and runs CleanupTeam. media_server is the only code in the whole tree
  that sends B_QUIT_REQUESTED to the addon server, and it does not send it here.
- NOT a catchable signal to media_addon_server. A rebuilt media_addon_server that
  installs handlers for signals 1..31 (except KILL/STOP) and logs to
  /boot/home/mas_debug.log recorded NO signal and NO QuitRequested at death.
- NOT the launch method. Controlled test: the STOCK media_addon_server dies whether
  media_server launches it by signature (be_roster->Launch(SIGNATURE)) OR by path
  (Launch(entry_ref of /boot/system/servers/media_addon_server)). An earlier
  "by-path fixes it" reading was a deploy-confound artifact and is WRONG.

What it IS: a timing-sensitive death in the media/audio connect path -- and, per a
full port trace of the connect (2026-09-13, below), NOT a port-layer race. The
decisive control: the STOCK addon server dies reliably during the connect, but a
rebuilt media_addon_server (same libmedia.so, only different -O/instrumentation)
reliably SURVIVES the same connect across many iterations. So instrumentation perturbs
timing enough to win the race. The stock build exits on its own down a path its
timing hits and the slower build does not.

PORT TRACE OF THE CONNECT (definitive, supersedes the "port race" guess above).
Captured with hyclone_server's signal-armed port trace (SIGUSR1 arm / SIGUSR2 stop,
env HYCLONE_PORT_TRACE=<file>) around one sptest BSoundPlayer connect that killed the
addon (team went zombie). 19k port ops in the window. What it shows:
  - NO B_QUIT_REQUESTED (code 1599165041) is written to ANY port in the whole trace.
    Nobody tells the addon to quit. Its BApplication looper (main thread, its looper
    port) sits blocked in port-read the entire time and only unblocks with
    B_BAD_PORT_ID at teardown -- it never dispatches a quit. This ends the "who quits
    it" line for good.
  - EVERY connect port op SUCCEEDS (status=0). The only non-teardown errors are benign
    timed-out polls on media_server's own port. There is NO B_BAD_PORT_ID / failed
    cross-team write during the connect. So it is NOT the launch_roster port-race
    family; the port layer is fine. (Correcting the earlier hypothesis.)
  - The death is localized: the system mixer's consumer control port receives the
    CONSUMER_* connect handshake in order -- CONSUMER_GET_NEXT_INPUT (0x301),
    CONSUMER_DISPOSE_INPUT_COOKIE (0x302), CONSUMER_ACCEPT_FORMAT (0x303),
    CONSUMER_CONNECTED (0x304) -- and the addon dies while handling CONSUMER_CONNECTED.
    The connect-handling thread reads 0x304 and then goes silent for ~411 ms (no port
    ops -- so it is doing non-port work: in-process hmulti_audio ioctls and/or
    buffer/format setup), after which the whole team is torn down.
  - No debug_server is invoked at the death (the 160+ debug_server "Failed to create
    BApplication: Already running" lines are all earlier), which points to a clean
    exit_group() rather than a routed SIGSEGV. monika/hmulti_audio.cpp contains no
    exit()/abort()/assert path, so the exit originates in the media_kit/mixer connect
    path itself, not in our adapter's syscall code.

So the real target is the system mixer's BBufferConsumer::Connected() / node-start
path inside media_addon_server (which owns both the AudioMixer and our "HyClone
Virtual Audio" MultiAudioNode), NOT hyclone_server's port code. Next step: instrument
media_addon_server / the mixer add-on (or add tracing to the MultiAudioNode's
hmulti_audio buffer/format calls) to catch the exit inside CONSUMER_CONNECTED
handling -- accepting that instrumentation perturbs the race, so pair it with a way to
still trigger the death (e.g. a busy-loop delay in the connect thread of the rebuilt
binary to reproduce stock timing).

SOURCE-LEVEL LOCALIZATION (2026-09-13). Read AudioMixer::Connected()
(src/add-ons/media/media-add-ons/mixer/AudioMixer.cpp:390). On the FIRST input only
(`if (fAutoStop && fCore->CountInputs() == 1)`) -- which is exactly the first client
connecting -- it starts the destination node and blocks waiting for it:
    roster->StartNode(output=HyClone Virtual Audio / MultiAudioNode, startLatency);
    // then a up-to-1-second wait loop on TimeSource()->GetTime(), snooze(100)
    fCore->Start();
That StartNode + wait loop is the ~411 ms silent gap the port trace showed. So the
death is inside starting our MultiAudioNode and/or fCore->Start(), on the first
connect.

Death mode nailed down: it is a SILENT clean exit_group. Ruled out, each with
evidence: not a SIGSEGV (no dmesg segfault; HyClone installs no SIGSEGV handler that
could swallow one -- only SIGREQUEST); not a debugger()/assert (HyClone's
_moni_debugger writes "_kern_debugger: <msg>" via raw write(2) before exit_group and
no such line appears); not a C++ abort/terminate/pure-virtual/stack-smash (none of
those messages appear on the addon's stderr, which is captured -- the addon inherits
media_server's fd 1/2); not B_QUIT. So some code on the StartNode/Start path calls
exit()/_exit() (or exit_group directly) with no output.

Corroboration: after a few of these deaths, media_server itself stops staying up
across restarts -- consistent with DefaultManager auto-connecting the mixer to the
output on startup (from saved Media state) and hitting the same Connected() path. So
the crash fires on ANY connect into the mixer, not only sptest's.

Precise next step (blocked only by env, see below): checkpoint AudioMixer::Connected()
around StartNode / the wait loop / fCore->Start() -- either build an instrumented
mixer add-on and load it from the writable user add-on dir
(/boot/home/config/non-packaged/add-ons/media/), or sudo strace -f the addon host
process across the connect (ptrace works; strace 6.19 present) to see the exact
exit_group and the last syscall before it. Instrumentation perturbs the race (the
addon then survives), so pair it with a small delay in the connect path to keep stock
timing.

ENV NOTE: after many repeated boots+crashes in one session the prefix degrades --
media_server begins exiting on startup even with Media settings cleared, and the
per-guest server topology gets unstable. A clean VM reboot (which also resets the two
AppArmor sysctls to 1; HyClone only needs the file caps, so that is fine) gives the
cleanest slate; remember hyclone_server loses its file caps on every rebuild and needs
`sudo setcap cap_sys_ptrace,cap_sys_nice,cap_sys_admin+eip build/bin/hyclone_server`.

Second, independent observation: on the runs where the addon SURVIVES (rebuilt
binary), sptest still hangs in the BSoundPlayer constructor -- the connect handshake
does not complete. Whether that is the same underlying bug seen from the other side,
or a separate one, is open.

Tooling recipe (reusable): the media servers can be built standalone with the Haiku
cross-compiler at generated.x86_64/cross-tools-x86_64/bin/x86_64-unknown-haiku-g++.
Compile with `-I src/servers/media[_addon] -I src/kits/media -I headers` + every
headers/os subdir + the headers/private/* set, `-idirafter headers/posix
headers/glibc`, `-include BeBuild.h`. Link with `-B<glue>/` (crti/crtn/start_dyn/
init_term_dyn.o) and `-L$HPREFIX/boot/system/lib -lbe -lmedia [-lgame] -lroot`.
Deploy to /boot/home (writable; /boot/system is read-only packagefs) and have
media_server launch it from there. Note: HyClone process/proc bookkeeping is noisy
-- enumerate real guests by argv[0] endswith bin/haiku_loader, not substring match.

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
3. DONE (via PulseAudio simple API, not libpipewire directly): the null sink is
   replaced by a per-guest pa_simple playback stream in hyclone_server
   (server_audio.cpp), fed by audio_open/audio_write/audio_close servercalls the
   guest's B_MULTI_BUFFER_EXCHANGE drives. pa_simple_write() blocks, which paces
   the guest, so BUFFER_EXCHANGE needs no separate clock. Reaches pipewire-pulse
   under a PipeWire desktop. Unblocked once the B_ABSOLUTE_TIMEOUT port bug was
   fixed (see the RESOLVED section above).
4. Format/rate handling, latency reporting, xrun handling; optional record.
   Currently the device advertises a single fixed format (48 kHz / 2ch / 16-bit)
   and the mixer resamples everything to it; per-app rate negotiation and record
   are still open.

## Related blockers (separate from audio)

- Guest AF_UNIX sockets return ENOSYS (socket family not wired through
  SocketFamilyBToLinux in monika/linux/socket_conversion.cpp even though the
  sockaddr_un conversion exists). cmus-remote and cmus's control socket need it;
  cmus can still run interactively without it. Fix is independent of audio.
- app_server remote-display backpressure (port 10900) can hang BApplications
  when no viewer drains it; keep a viewer connected during testing.

## BUFFER_EXCHANGE gap (2026-09-13): output node never pumps

Even with the connect crash fixed and the sink (servercall) in place, no audio
flows. Traced it end to end (per-ioctl logging in monika hmulti_audio; loader
spawn_thread logging; an instrumented mixer and multi_audio add-on in
$HPREFIX/boot/home/config/non-packaged/add-ons/media/):

- The virtual device IS created and probed: monika sees GET_DESCRIPTION,
  GET/SET_ENABLED_CHANNELS, GET/SET_GLOBAL_FORMAT, LIST_MIX_CONTROLS,
  GET_BUFFERS (opcodes 8020..8035) -- all at MultiAudioNode/MultiAudioDevice
  *creation* time. The node advertises kinds 0x2000f, i.e. B_PHYSICAL_OUTPUT.
- But B_MULTI_BUFFER_EXCHANGE (8038) is NEVER issued, and the loader NEVER
  spawns a "multi_audio audio output" thread. That thread is created only in
  MultiAudioNode::_StartOutputThreadIfNeeded(), which is called only from
  MultiAudioNode::Connected() (src/add-ons/media/media-add-ons/multi_audio/
  MultiAudioNode.cpp). GET_BUFFERS comes from MultiAudioDevice::_GetBuffers()
  at init, NOT from Connected().

Conclusion: MultiAudioNode::Connected() never runs -> the system mixer is never
connected to our MultiAudioNode (the physical output) -> the output node's
BufferExchange loop never starts -> no buffer ever reaches the sink. A client's
BSoundPlayer still connects to the mixer (InitCheck: No error), but the mixer's
output goes nowhere.

So the missing link is the mixer->MultiAudioNode connection, established by
media_server's DefaultManager (_RescanThread -> _FindPhysical /
_ConnectMixerToOutput). Older traces (a TRACE media_server_dbg) once logged
"Default physical audio output ... created!", so it can work; it is not
connecting in the current post-reboot environment. NEXT STEP: build a TRACE
media_server_dbg and watch DefaultManager decide whether it finds the physical
output and whether _ConnectMixerToOutput succeeds or fails silently. The
servercall sink is ready and will produce sound as soon as buffers flow.

## *** RESOLVED (2026-09-13): audio flows end-to-end to PipeWire ***

Background: DefaultManager does connect the mixer to our node (fMixerConnected=1)
and the consumer Connected() runs, but the MultiAudioNode's output thread was
never spawned and B_MULTI_BUFFER_EXCHANGE (opcode 8038) never fired -- the node
received NODE_START but never ran _HandleStart, so it never actually started.
That was NOT a time-source / RunMode problem. Root cause, found by building an
instrumented libmedia.so (BMediaEventLooper::ControlLoop + BTimeSource logging)
and then a server-side trace of every port read:

  **hyclone_server's port servercalls ignored B_ABSOLUTE_TIMEOUT.**

`server_hserver_call_read_port_etc` (and write / buffer_size / message_info)
decided whether to wait with `useTimeout = flags & B_TIMEOUT`. B_TIMEOUT (0x8)
is only the *relative*-timeout flag; B_ABSOLUTE_TIMEOUT is 0x10. So any port op
issued with an absolute deadline was treated as having no timeout and passed
B_INFINITE_TIMEOUT to Port::Read -> it blocked forever instead of returning
B_TIMED_OUT at the deadline.

BMediaEventLooper::ControlLoop dispatches a node's timed events (B_START,
buffer handling, ...) precisely by reading its control port with
B_ABSOLUTE_TIMEOUT and treating B_TIMED_OUT as "the event's time has come".
Because that read never timed out, the node received NODE_START, queued the
B_START event, and then blocked forever without ever dispatching it. Every
media node (AudioMixer, MultiAudioNode) hung the same way -- so no BUFFER_EXCHANGE
ever ran and no buffers reached the sink. sptest connected ("InitCheck: No
error") but nothing played.

Fix (now on master): `server_relative_timeout(flags, timeout)` in
hyclone_server/server_time.h converts an absolute deadline to the relative wait
Port::Read/Write expect (or infinite when no timeout flag is set); all four port
servercalls route through it. Because bigtime_t is UNSIGNED in HyClone, the
conversion compares before subtracting (`if (timeout <= now) return 0;`) so an
already-elapsed deadline yields a non-blocking poll rather than wrapping to a
near-infinite value; an elapsed absolute deadline is normalized to B_TIMED_OUT.
Committed separately as "fix: honor B_ABSOLUTE_TIMEOUT in port read/write
servercalls" -- it is a general correctness fix, not audio-specific.

With that fix the whole chain runs:
  sptest (BSoundPlayer) -> System Mixer -> MultiAudioNode (NODE_START now
  dispatches -> _HandleStart -> output thread) -> B_MULTI_BUFFER_EXCHANGE (8038)
  -> audio_open/audio_write servercalls -> pa_simple -> PipeWire.

Verified: a live PipeWire sink-input (s16le 2ch 48000Hz, client "hyclone_server")
appears during playback. 300+ ControlLoop dispatches and 100+ buffer exchanges
per short clip.

Two supporting pieces landed on the audio-sink branch:
- server_audio.cpp reaches pipewire-pulse out of the box: when neither
  PULSE_SERVER nor XDG_RUNTIME_DIR is in the (haiku_loader-forked) server's
  environment, it constructs unix:/run/user/<uid>/pulse/native itself.
- Streams are released on teardown: server_audio_cleanup(pid) is called from the
  connection-teardown path (system.cpp) so a media_addon_server that dies without
  audio_close does not leak its host stream. An orderly audio_close() drains; a
  dead sink or dead guest frees without draining (draining would block).

Debug scaffolding used to find this (all reverted afterwards): a full instrumented
libmedia.so build (build script + notes were in the session scratchpad), a
fixed-file server-side read trace in port.cpp, and pa_strerror logging in
server_audio.cpp. The stock libmedia.so is restored in the prefix.

## *** RESOLVED (2026-09-15): real apps (cmus, ocp) play end-to-end ***

The sink above worked for sptest, but cmus and ocp still hung. Root cause: a
close_port() bug in hyclone_server's Port. BSoundPlayer's destructor calls
BMediaEventLooper::Quit(), which does close_port(ControlPort()) and then
wait_for_thread() to join the node's control-loop thread. That thread sits in
read_port() on its control port. Port::Close() only woke writers (never
_readCondVar) and the read/get-message-info predicate ignored _closed, so the
close never woke the reader -- it never returned B_BAD_PORT_ID, never exited,
and the join hung forever, wedging the whole media stack. It hit cmus/ocp during
audio-output setup (a BSoundPlayer probe/teardown) and sptest in its teardown.

Found with guest-symbol gdb: host gdb only symbolizes the loader frames, so the
BMediaRoster frames showed as ??. Loading the guest images at their runtime
bases (add-symbol-file libroot.so/libmedia.so/libbe.so -o <base from
/proc/PID/maps>) resolved the stuck main-thread stack to
  BSoundPlayer::~BSoundPlayer() -> BMediaNode::Release()
    -> BMediaEventLooper::DeleteHook() -> BMediaEventLooper::Quit()
      -> wait_for_thread() [blocked join].

Fix: Port::Close() now also does _readCondVar.notify_all(); Read/GetMessageInfo
wake on _closed and return B_BAD_PORT_ID on a closed-and-empty port -- matching
Haiku's close_port() semantics. Committed as "fix: close_port() must unblock
threads reading the port".

Verified end-to-end, each producing a live PipeWire sink-input (s16le 2ch 48000Hz):
- cmus  (libao -> libhaiku -> media_kit): status playing, position advances.
- ocp   (SDL3 -> Haiku audio -> media_kit): auto-plays on open.
- sptest now tears down cleanly instead of hanging.

Two related fixes landed while getting cmus this far:
- AF_UNIX bind rejected a not-yet-existing socket path -- vchroot_expandat
  returns B_ENTRY_NOT_FOUND for a path bind() is about to create, and monika
  treated that as failure (ENOSYS). cmus could not create its control socket
  ("bind: Function not implemented") and would not start. Committed as
  "fix: allow AF_UNIX bind to a not-yet-existing socket path".
- ocp config note: its stock ocp.ini `playerdevices` list predates SDL3 (it lists
  devpSDL2/devpSDL, not devpSDL3), so ocp falls through to devpNone (silent).
  Set `playerdevices=devpSDL3` in ~/config/settings/ocp/ocp.ini to use the SDL3
  output that reaches BSoundPlayer.
