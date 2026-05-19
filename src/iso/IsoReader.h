#pragma once

#include "../fs/virtual_disk.h"

#include <string>
#include <vector>

struct IsoEntry
{
    std::string path;
    uint64_t size = 0;
    bool isDirectory = false;
};

class IsoReader
{
public:
    bool Open(const std::string& path);

    std::vector<std::string> GetFiles();

    std::vector<IsoEntry> GetEntries();

    bool LoadToVFS(VirtualFileSystem& vfs);

    bool ReadFileData(
        const std::string& path,
        uint64_t offset,
        uint32_t size,
        std::vector<char>& out);

private:
    std::string isoPath;

    std::vector<IsoEntry> entries;
};

extern IsoReader g_IsoReader;