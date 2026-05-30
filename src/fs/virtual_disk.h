#pragma once

#include <string>
#include <vector>

extern char g_MountLetter;

struct VirtualFile
{
    std::string path;
    std::vector<char> data;

    bool isDirectory = false;
    uint64_t size = 0;

    bool isStreamed = false;
    uint64_t archiveOffset = 0; 
};


class VirtualFileSystem
{
public:

    std::vector<VirtualFile> files;

    void AddFile(
        const VirtualFile& file);

    void Clear();
};

extern VirtualFileSystem g_VFS;