#include "VirtualDiskManager.h"
#include <windows.h>
#include <iostream>

bool VirtualDiskManager::MountFolder(const std::string& folderPath)
{
    mountedPath = folderPath;

    std::cout << "[INFO] Folder mounted: "
        << folderPath << std::endl;

    return true;
}

bool VirtualDiskManager::MountISO(const std::string& isoPath)
{
    mountedPath = isoPath;

    std::cout << "[INFO] ISO mounted: "
        << isoPath << std::endl;

    return true;
}

void VirtualDiskManager::Unmount()
{
    mountedPath.clear();

    std::cout << "[INFO] Disk unmounted" << std::endl;
}

bool VirtualDiskManager::IsMounted() const
{
    return !mountedPath.empty();
}

std::string VirtualDiskManager::GetMountPoint() const
{
    return mountPoint;
}