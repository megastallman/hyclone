#include <filesystem>
#include <fstream>
#include <iostream>

#include "server_prefix.h"

bool server_setup_prefix()
{
    std::filesystem::path systemPath = std::filesystem::path(gHaikuPrefix) / "boot" / "system";
    std::filesystem::path settingsPath = systemPath / "settings";
    std::filesystem::path networkSettingsPath = settingsPath / "network";

    std::filesystem::create_directories(networkSettingsPath);

    std::filesystem::copy_file("/etc/hostname", networkSettingsPath / "hostname",
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file("/etc/hosts", networkSettingsPath / "hosts",
        std::filesystem::copy_options::overwrite_existing);
    // On systemd-resolved hosts, /etc/resolv.conf points at a local stub
    // resolver (127.0.0.53) that Haiku's netresolv fails to query, breaking
    // all guest DNS resolution. Prefer the real upstream nameserver list
    // maintained by systemd-resolved when it is available.
    std::filesystem::path hostResolvConf = "/run/systemd/resolve/resolv.conf";
    if (!std::filesystem::exists(hostResolvConf))
    {
        hostResolvConf = "/etc/resolv.conf";
    }
    std::filesystem::copy_file(hostResolvConf, networkSettingsPath / "resolv.conf",
        std::filesystem::copy_options::overwrite_existing);

    std::filesystem::path etcPath = systemPath / "etc";
    auto oldSystemPerms = std::filesystem::status(systemPath).permissions();
    std::filesystem::permissions(systemPath, oldSystemPerms | std::filesystem::perms::owner_write);
    std::filesystem::create_directories(etcPath);
    std::filesystem::permissions(systemPath, oldSystemPerms);

    std::fstream shadow((etcPath / "shadow").c_str(), std::ios::out | std::ios::app);

    return true;
}

void server_replace_libroot(const std::filesystem::path& target)
{
    auto oldPermissions = std::filesystem::status(target).permissions();

    std::filesystem::path hostServerPath = std::filesystem::canonical("/proc/self/exe");
    std::filesystem::path hostInstallPrefix = hostServerPath.parent_path().parent_path();
    std::filesystem::path hostLibrootPath = hostInstallPrefix / "lib" / "libroot.so";

    // TODO: Check libroot.so hcrev.
    if (!std::filesystem::exists(hostLibrootPath))
    {
        std::cerr << "Could not find libroot.so at " << hostLibrootPath << std::endl;
        std::cerr << "HyClone may not work correctly without the correct custom libroot.so" << std::endl;
    }

    // The target may be mmap'ed by running guest processes (every one of
    // them maps libroot.so). copy_file(overwrite_existing) truncates and
    // rewrites the file in place, which yanks the pages out from under
    // those mappings and makes running processes crash on garbage reads
    // (this fired on every package activation). Copy to a temporary file
    // and atomically rename() it over the target instead: existing
    // mappings keep the old inode, new processes get the new file.
    std::filesystem::path tempPath = target;
    tempPath += ".hyclone.tmp";

    // The extracted lib directory is read-only; make it writable while we
    // create the temporary file and rename it into place.
    auto dirPermissions = std::filesystem::status(target.parent_path()).permissions();
    std::filesystem::permissions(target.parent_path(),
        std::filesystem::perms::owner_write, std::filesystem::perm_options::add);

    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    std::filesystem::copy_file(hostLibrootPath, tempPath, ec);
    if (ec)
    {
        std::cerr << "Failed to stage libroot.so replacement: " << ec.message() << std::endl;
        std::filesystem::permissions(target.parent_path(), dirPermissions);
        return;
    }
    std::filesystem::permissions(tempPath, oldPermissions);
    std::filesystem::rename(tempPath, target, ec);
    if (ec)
    {
        std::cerr << "Failed to replace libroot.so: " << ec.message() << std::endl;
        std::filesystem::remove(tempPath, ec);
    }

    std::filesystem::permissions(target.parent_path(), dirPermissions);
}