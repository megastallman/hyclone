#ifndef __LOADER_SIGNAL_H__
#define __LOADER_SIGNAL_H__

#include <cstdint>

int loader_get_sigrtmin();
int loader_get_sigrtmax();
void loader_notify_guest_signal();
uint64_t loader_guest_signal_count();

#endif // __LOADER_SIGNAL_H__
