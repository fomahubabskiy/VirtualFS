#pragma once

#include <string>

class VirtualDiskManager
{
public:
    bool MountFolder(const std::string& folderPath);
    bool MountISO(const std::string& isoPath);

    void Unmount();

    bool IsMounted() const;

    std::string GetMountPoint() const;

private:
    std::string mountedPath;
    std::string mountPoint = "X:";
};