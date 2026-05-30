#pragma once

extern "C"
{
    bool StartFileSystem(const wchar_t* path);
    void StopFileSystem();
}