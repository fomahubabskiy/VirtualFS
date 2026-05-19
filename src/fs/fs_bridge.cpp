#pragma once
#include "fs_bridge.h"

extern "C"
{
    bool StartFileSystem(const wchar_t* path);

    void StopFileSystem();
}