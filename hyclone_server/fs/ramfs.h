#ifndef __HYCLONE_RAMFS_H__
#define __HYCLONE_RAMFS_H__

#include "fs/hostfs.h"

// Emulates Haiku's ramfs with a host-directory-backed passthrough.
// A fresh backing directory under the HyClone prefix is created (and
// emptied) for every mount, so the volume starts out empty like a real
// RAM filesystem. The contents are not actually RAM-backed, which is an
// acceptable approximation for the ways ramfs is used on Haiku
// (/var/shared_memory, user scratch mounts).
class RamfsDevice : public HostfsDevice
{
public:
    RamfsDevice(const std::filesystem::path& root,
        const std::filesystem::path& hostRoot, uint32 mountFlags = 0);

    static status_t Mount(const std::filesystem::path& path,
        const std::filesystem::path& device, uint32 flags,
        const std::string& args, std::shared_ptr<VfsDevice>& output);
};

#endif // __HYCLONE_RAMFS_H__
