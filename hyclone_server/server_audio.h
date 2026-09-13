#ifndef __SERVER_AUDIO_H__
#define __SERVER_AUDIO_H__

// Release any host audio stream owned by the given guest process. Safe to call
// for a process that never opened one. Invoked from the connection-teardown
// path so a guest that dies without calling audio_close does not leak its host
// stream. Unlike an explicit audio_close it does not drain (the guest is gone).
void server_audio_cleanup(int pid);

#endif // __SERVER_AUDIO_H__
