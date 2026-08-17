#include <cstring>

#include "fs/ramfs.h"
#include "server_filesystem.h"
#include "server_native.h"
#include "server_prefix.h"

RamfsDevice::RamfsDevice(const std::filesystem::path& root,
    const std::filesystem::path& hostRoot, uint32 mountFlags)
    : HostfsDevice(root, hostRoot, mountFlags)
{
    _info.flags = B_FS_HAS_ATTR;
    _info.block_size = B_PAGE_SIZE;
    _info.io_size = B_PAGE_SIZE;
    _info.total_blocks = 0;
    _info.free_blocks = 0;
    _info.total_nodes = 0;
    _info.free_nodes = 0;
    _info.device_name[0] = '\0';
    strncpy(_info.volume_name, root.filename().c_str(), sizeof(_info.volume_name));
    strncpy(_info.fsh_name, "ramfs", sizeof(_info.fsh_name));

    server_fill_fs_info(hostRoot, &_info);
}

status_t RamfsDevice::Mount(const std::filesystem::path& path,
    const std::filesystem::path& device, uint32 flags, const std::string& args,
    std::shared_ptr<VfsDevice>& output)
{
    // Derive a backing directory under the prefix from the mount path,
    // e.g. "/var/shared_memory" => $HPREFIX/.hyclone.ramfs/var.shared_memory
    std::string mangled;
    for (const auto& part : path.relative_path())
    {
        if (!mangled.empty())
        {
            mangled += '.';
        }
        mangled += part.string();
    }
    if (mangled.empty())
    {
        return B_BAD_VALUE;
    }

    std::filesystem::path hostRoot =
        std::filesystem::path(gHaikuPrefix) / ".hyclone.ramfs" / mangled;

    std::error_code ec;
    // A ramfs starts out empty on every mount.
    std::filesystem::remove_all(hostRoot, ec);
    std::filesystem::create_directories(hostRoot, ec);
    if (ec)
    {
        return B_ERROR;
    }
    std::filesystem::permissions(hostRoot, std::filesystem::perms::all, ec);

    output = std::make_shared<RamfsDevice>(path, hostRoot, flags);

    return B_OK;
}
