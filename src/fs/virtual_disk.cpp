#include "virtual_disk.h"

char g_MountLetter = 'X';

VirtualFileSystem g_VFS;

void VirtualFileSystem::AddFile(
    const VirtualFile& file)
{
    files.push_back(file);
}

void VirtualFileSystem::Clear()
{
    files.clear();
}

