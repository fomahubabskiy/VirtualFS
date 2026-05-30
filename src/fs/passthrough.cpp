/**
 * @file passthrough.c
 *
 * @copyright 2015-2025 Bill Zissimopoulos
 */
 /*
  * This file is part of WinFsp.
  *
  * You can redistribute it and/or modify it under the terms of the GNU
  * General Public License version 3 as published by the Free Software
  * Foundation.
  *
  * Licensees holding a valid commercial license may use this software
  * in accordance with the commercial license agreement provided in
  * conjunction with the software.  The terms and conditions of any such
  * commercial license agreement shall govern, supersede, and render
  * ineffective any application of the GPLv3 license to this software,
  * notwithstanding of any reference thereto in the software or
  * associated repository.
  */

#include <winfsp/winfsp.h>
#include <strsafe.h>
#include <windows.h>
#include "virtual_disk.h"
#include "../iso/IsoReader.h"
#include <string>
#include <set>

#define PROGNAME                        "passthrough"
#define ALLOCATION_UNIT                 4096
#define FULLPATH_SIZE                   (MAX_PATH + FSP_FSCTL_TRANSACT_PATH_SIZEMAX / sizeof(WCHAR))

#define info(format, ...)               FspServiceLog(EVENTLOG_INFORMATION_TYPE, format, __VA_ARGS__)
#define warn(format, ...)               FspServiceLog(EVENTLOG_WARNING_TYPE, format, __VA_ARGS__)
#define fail(format, ...)               FspServiceLog(EVENTLOG_ERROR_TYPE, format, __VA_ARGS__)

#define ConcatPath(Ptfs, FN, FP)        (0 == StringCbPrintfW(FP, sizeof FP, L"%s%s", Ptfs->Path, FN))

struct PTFS_FILE_CONTEXT
{
    std::string path;
    bool isDirectory = false;
    PVOID DirBuffer = nullptr;
};

static std::string NormalizePath(std::string path)
{
    for (char& c : path)
    {
        if (c == '\\')
            c = '/';

    }

    // remove leading slash
    while (!path.empty() &&
        (path[0] == '/' || path[0] == '\\'))
    {
        path.erase(path.begin());
    }

    // remove trailing slash
    while (!path.empty() &&
        (path.back() == '/' || path.back() == '\\'))
    {
        path.pop_back();
    }

    return path;
}


static std::string NormalizeComparePath(std::string path)
{
    path = NormalizePath(path);

    for (char& c : path)
    {
        c = (char)tolower((unsigned char)c);
    }

    return path;
}


typedef struct
{
    FSP_FILE_SYSTEM* FileSystem;
    PWSTR Path;
} PTFS;

static NTSTATUS GetFileInfoInternal(HANDLE Handle, FSP_FSCTL_FILE_INFO* FileInfo)
{
    BY_HANDLE_FILE_INFORMATION ByHandleFileInfo;

    if (!GetFileInformationByHandle(Handle, &ByHandleFileInfo))
        return FspNtStatusFromWin32(GetLastError());

    FileInfo->FileAttributes = ByHandleFileInfo.dwFileAttributes;
    FileInfo->ReparseTag = 0;
    FileInfo->FileSize =
        ((UINT64)ByHandleFileInfo.nFileSizeHigh << 32) | (UINT64)ByHandleFileInfo.nFileSizeLow;
    FileInfo->AllocationSize = (FileInfo->FileSize + ALLOCATION_UNIT - 1)
        / ALLOCATION_UNIT * ALLOCATION_UNIT;
    FileInfo->CreationTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftCreationTime)->QuadPart;
    FileInfo->LastAccessTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftLastAccessTime)->QuadPart;
    FileInfo->LastWriteTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftLastWriteTime)->QuadPart;
    FileInfo->ChangeTime = FileInfo->LastWriteTime;
    FileInfo->IndexNumber =
        ((UINT64)ByHandleFileInfo.nFileIndexHigh << 32) | (UINT64)ByHandleFileInfo.nFileIndexLow;
    FileInfo->HardLinks = 0;

    return STATUS_SUCCESS;
}

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM* FileSystem,
    FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
    PTFS* Ptfs = (PTFS*)FileSystem->UserContext;
    WCHAR Root[4096];
    ULARGE_INTEGER TotalSize, FreeSize;

    memset(VolumeInfo, 0, sizeof(*VolumeInfo));//не работает 

    if (!GetVolumePathName(Ptfs->Path, Root, MAX_PATH))
        return FspNtStatusFromWin32(GetLastError());

    if (!GetDiskFreeSpaceEx(Root, 0, &TotalSize, &FreeSize))
        return FspNtStatusFromWin32(GetLastError());

    VolumeInfo->TotalSize = TotalSize.QuadPart;
    VolumeInfo->FreeSize = FreeSize.QuadPart;

    wcscpy_s(VolumeInfo->VolumeLabel, 32, L"FS"); //не работает

    return STATUS_SUCCESS;
}

static NTSTATUS SetVolumeLabel_(FSP_FILE_SYSTEM* FileSystem,
    PWSTR VolumeLabel,
    FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
    /* we do not support changing the volume label */
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS GetSecurityByName(
    FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName,
    PUINT32 PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    SIZE_T* PSecurityDescriptorSize)
{
    UNREFERENCED_PARAMETER(FileSystem);

    std::wstring ws(FileName);
    std::string path(ws.begin(), ws.end());

    path = NormalizePath(path);

    if (PSecurityDescriptorSize)
        *PSecurityDescriptorSize = 0;

    // ROOT
    if (path.empty())
    {
        if (PFileAttributes)
            *PFileAttributes = FILE_ATTRIBUTE_DIRECTORY;

         if (PSecurityDescriptorSize)
            *PSecurityDescriptorSize = 0;

        return STATUS_SUCCESS;
    }

    for (auto& file : g_VFS.files)
    {
        std::string fp =
            NormalizeComparePath(file.path);

        std::string rp =
            NormalizeComparePath(path);

        if (fp == rp)
        {
            if (PFileAttributes)
            {
                *PFileAttributes =
                    file.isDirectory
                    ? FILE_ATTRIBUTE_DIRECTORY
                    : FILE_ATTRIBUTE_NORMAL;
            }

            if (PFileAttributes)
                *PFileAttributes = FILE_ATTRIBUTE_DIRECTORY;

            if (PSecurityDescriptorSize)
                *PSecurityDescriptorSize = 0;

            return STATUS_SUCCESS;
        }
    }

    // virtual directories
    for (auto& file : g_VFS.files)
    {
        std::string fp =
            NormalizePath(file.path);

        std::string prefix =
            path + "/";

        if (fp.rfind(prefix, 0) == 0)
        {
            if (PFileAttributes)
                *PFileAttributes =
                FILE_ATTRIBUTE_DIRECTORY;

            return STATUS_SUCCESS;
        }
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}


static NTSTATUS Open(
    FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName,
    UINT32 CreateOptions,
    UINT32 GrantedAccess,
    PVOID* PFileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(CreateOptions);
    UNREFERENCED_PARAMETER(GrantedAccess);

    std::wstring ws(FileName);

    std::string path(
        ws.begin(),
        ws.end());

    path = NormalizePath(path);

    OutputDebugStringA(
        ("OPEN PATH = [" + path + "]\n").c_str());

    PTFS_FILE_CONTEXT* ctx =
        new PTFS_FILE_CONTEXT();

    ctx->path = path;
    ctx->DirBuffer = nullptr;

    if (path.empty())
    {
        ctx->isDirectory = true;

        *PFileContext = ctx;

        memset(FileInfo,0,sizeof(FSP_FSCTL_FILE_INFO));

        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);

        UINT64 t =
            ((UINT64)ft.dwHighDateTime << 32) |
            ft.dwLowDateTime;

        FileInfo->CreationTime = t;
        FileInfo->LastAccessTime = t;
        FileInfo->LastWriteTime = t;
        FileInfo->ChangeTime = t;
        FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;

        return STATUS_SUCCESS;
    }

    for (auto& file : g_VFS.files)
    {
        
        OutputDebugStringA(
            ("VFS FILE = [" +
                NormalizePath(file.path) +
                "]\n").c_str());
        
        std::string fp =
            NormalizePath(file.path);

        if (_stricmp(
            fp.c_str(),
            path.c_str()) == 0)
        {
            ctx->isDirectory = file.isDirectory;

            OutputDebugStringA(
                (std::string("ISDIR = ")
                    + (ctx->isDirectory ? "YES" : "NO")
                    + "\n").c_str());

            *PFileContext = ctx;

            memset(FileInfo,0,sizeof(FSP_FSCTL_FILE_INFO));

            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);

            UINT64 t =
                ((UINT64)ft.dwHighDateTime << 32) |
                ft.dwLowDateTime;

            FileInfo->CreationTime = t;
            FileInfo->LastAccessTime = t;
            FileInfo->LastWriteTime = t;
            FileInfo->ChangeTime = t;
            FileInfo->FileAttributes = file.isDirectory
                ? FILE_ATTRIBUTE_DIRECTORY
                : FILE_ATTRIBUTE_NORMAL;

            if (file.isDirectory)
            {
                FileInfo->FileSize = 0;
                FileInfo->AllocationSize = 0;
            }
            else
            {
                FileInfo->FileSize =
                    (UINT64)file.isStreamed ? file.size : file.data.size();

                FileInfo->AllocationSize =
                    FileInfo->FileSize;
            }

            FileInfo->IndexNumber = 1;

            return STATUS_SUCCESS;
        }
    }

    delete ctx;

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS Create(
    FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName,
    UINT32 CreateOptions,
    UINT32 GrantedAccess,
    UINT32 FileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    UINT64 AllocationSize,
    PVOID* PFileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(SecurityDescriptor);
    UNREFERENCED_PARAMETER(AllocationSize);

    return Open(
        FileSystem,
        FileName,
        CreateOptions,
        GrantedAccess,
        PFileContext,
        FileInfo);
}
            

static NTSTATUS Overwrite(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    UINT32 FileAttributes,
    BOOLEAN ReplaceFileAttributes,
    UINT64 AllocationSize,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(FileAttributes);
    UNREFERENCED_PARAMETER(ReplaceFileAttributes);
    UNREFERENCED_PARAMETER(AllocationSize);
    UNREFERENCED_PARAMETER(FileInfo);

    return STATUS_SUCCESS;
}

static VOID Cleanup(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PWSTR FileName,
    ULONG Flags)
{}

static VOID Close(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext0)
{
    UNREFERENCED_PARAMETER(FileSystem);

    PTFS_FILE_CONTEXT* ctx =
        (PTFS_FILE_CONTEXT*)FileContext0;

    if (!ctx)
        return;

    if (ctx->DirBuffer)
    {
        FspFileSystemDeleteDirectoryBuffer(
            &ctx->DirBuffer);
    }

    delete ctx;
}
static NTSTATUS Read(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext0,
    PVOID Buffer,
    UINT64 Offset,
    ULONG Length,
    PULONG PBytesTransferred)
{
    UNREFERENCED_PARAMETER(FileSystem);

    PTFS_FILE_CONTEXT* ctx =
        (PTFS_FILE_CONTEXT*)FileContext0;

    if (!ctx)
        return STATUS_INVALID_HANDLE;

    std::string path =
        NormalizePath(ctx->path);

    for (auto& file : g_VFS.files)
    {
        std::string fp =
            NormalizePath(file.path);

        if (_stricmp(fp.c_str(), path.c_str()) == 0)
        {
            if (file.isDirectory)
            {
                *PBytesTransferred = 0;
                return STATUS_SUCCESS;
            }

            // STREAMED ФАЙЛ
            if (file.isStreamed)
            {
                std::vector<char> temp;

                if (!g_IsoReader.ReadFileData(
                    file.path,
                    Offset,
                    Length,
                    temp))
                {
                    return STATUS_UNSUCCESSFUL;
                }

                memcpy(Buffer, temp.data(), temp.size());
                *PBytesTransferred = (ULONG)temp.size();
                return STATUS_SUCCESS;
            }

            // RAM ФАЙЛ
            UINT64 fileSize =
                (UINT64)file.data.size();

            if (Offset >= fileSize)
            {
                *PBytesTransferred = 0;
                return STATUS_END_OF_FILE;
            }

            ULONG bytesToRead =
                (ULONG)min(
                    (UINT64)Length,
                    fileSize - Offset);

            memcpy(
                Buffer,
                file.data.data() + Offset,
                bytesToRead);

            *PBytesTransferred = bytesToRead;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS Write(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS Flush(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    return STATUS_SUCCESS;
}

static NTSTATUS GetFileInfo(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext0,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);

    PTFS_FILE_CONTEXT* ctx =
        (PTFS_FILE_CONTEXT*)FileContext0;

    if (!ctx)
        return STATUS_INVALID_HANDLE;

    std::string path =
        NormalizePath(ctx->path);

    memset(FileInfo,0,sizeof(FSP_FSCTL_FILE_INFO));

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    UINT64 t =
        ((UINT64)ft.dwHighDateTime << 32) |
        ft.dwLowDateTime;

    FileInfo->CreationTime = t;
    FileInfo->LastAccessTime = t;
    FileInfo->LastWriteTime = t;
    FileInfo->ChangeTime = t;

    // ROOT

    if (path.empty())
    {
        FileInfo->FileAttributes =
            FILE_ATTRIBUTE_DIRECTORY;

        return STATUS_SUCCESS;
    }

    for (auto& file : g_VFS.files)
    {
        std::string fp =
            NormalizePath(file.path);

        if (_stricmp(
            fp.c_str(),
            path.c_str()) == 0)
        {
            FileInfo->FileAttributes =
                file.isDirectory
                ? FILE_ATTRIBUTE_DIRECTORY
                : FILE_ATTRIBUTE_NORMAL;

            if (file.isDirectory)
            {
                FileInfo->FileSize = 0;
                FileInfo->AllocationSize = 0;
            }
            else
            {
                FileInfo->FileSize =
                    (UINT64)file.isStreamed ? file.size : file.data.size();

                FileInfo->AllocationSize =
                    FileInfo->FileSize;
            }

            FileInfo->IndexNumber = 1;

            return STATUS_SUCCESS;
        }
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT32 FileAttributes,
    UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime, UINT64 ChangeTime,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS SetFileSize(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
        /*
         * This file system does not maintain AllocationSize, although NTFS clearly can.
         * However it must always be FileSize <= AllocationSize and NTFS will make sure
         * to truncate the FileSize if it sees an AllocationSize < FileSize.
         *
         * If OTOH a very large AllocationSize is passed, the call below will increase
         * the AllocationSize of the underlying file, although our file system does not
         * expose this fact. This AllocationSize is only temporary as NTFS will reset
         * the AllocationSize of the underlying file when it is closed.
         */

        
}

static NTSTATUS Rename(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PWSTR FileName, PWSTR NewFileName, BOOLEAN ReplaceIfExists)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS GetSecurity(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    SIZE_T* PSecurityDescriptorSize)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);

    static SECURITY_DESCRIPTOR sd;
    static bool initialized = false;

    if (!initialized)
    {
        InitializeSecurityDescriptor(
            &sd,
            SECURITY_DESCRIPTOR_REVISION);

        SetSecurityDescriptorDacl(
            &sd,
            TRUE,
            NULL,
            FALSE);

        initialized = true;
    }

    ULONG len = GetSecurityDescriptorLength(&sd);

    if (!SecurityDescriptor ||
        *PSecurityDescriptorSize < len)
    {
        *PSecurityDescriptorSize = len;
        return STATUS_BUFFER_TOO_SMALL;
    }

    memcpy(SecurityDescriptor, &sd, len);

    *PSecurityDescriptorSize = len;

    return STATUS_SUCCESS;
}
static NTSTATUS SetSecurity(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    SECURITY_INFORMATION SecurityInformation, PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS ReadDirectory(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext0,
    PWSTR Pattern,
    PWSTR Marker,
    PVOID Buffer,
    ULONG BufferLength,
    PULONG PBytesTransferred)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(Pattern);

    PTFS_FILE_CONTEXT* ctx =
        (PTFS_FILE_CONTEXT*)FileContext0;

    static PVOID RootDirBuffer = nullptr;

    PVOID* DirBufferPtr =
        ctx ? &ctx->DirBuffer : &RootDirBuffer;

    union
    {
        UINT8 B[
            FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf)
                + MAX_PATH * sizeof(WCHAR)];
        FSP_FSCTL_DIR_INFO D;
    } DirInfoBuf;

    FSP_FSCTL_DIR_INFO* DirInfo =
        &DirInfoBuf.D;

    NTSTATUS Result = STATUS_SUCCESS;

    if (FspFileSystemAcquireDirectoryBuffer(
        DirBufferPtr,
        Marker == NULL,
        &Result))
    {
        // FIXIK: set вместо vector
        std::set<std::string> added;

        std::string currentPath =
            ctx ? NormalizePath(ctx->path) : "";

        int safety = 0;
        const int MAX_ENTRIES = 10000;

        for (auto& file : g_VFS.files)
        {
            if (++safety > MAX_ENTRIES)
                break;

            std::string full =
                NormalizePath(file.path);

            std::string relative;

            if (currentPath.empty())
            {
                relative = full;
            }
            else
            {
                if (_stricmp(
                    full.c_str(),
                    currentPath.c_str()) == 0)
                    continue;

                std::string prefix =
                    currentPath + "/";

                // FIXIK: строгий фильтр
                if (full.rfind(prefix, 0) != 0)
                    continue;

                relative =
                    full.substr(prefix.size());
            }

            if (relative.empty())
                continue;

            size_t slash =
                relative.find('/');

            std::string name;
            bool isDir = false;

            if (slash != std::string::npos)
            {
                name = relative.substr(0, slash);
                isDir = true;
            }
            else
            {
                name = relative;
                isDir = file.isDirectory;
            }

            // FIXIK: set проверка
            if (!added.insert(name).second)
                continue;

            memset(DirInfo, 0, sizeof(*DirInfo));

            DirInfo->FileInfo.IndexNumber = 1;

            std::wstring wname;
            wname.assign(name.begin(), name.end());

            ULONG Length =
                (ULONG)wname.size();

            DirInfo->Size =
                (UINT16)(
                    FIELD_OFFSET(
                        FSP_FSCTL_DIR_INFO,
                        FileNameBuf)
                    + Length * sizeof(WCHAR));

            memcpy(
                DirInfo->FileNameBuf,
                wname.c_str(),
                Length * sizeof(WCHAR));

            DirInfo->FileInfo.FileAttributes =
                isDir
                ? FILE_ATTRIBUTE_DIRECTORY
                : FILE_ATTRIBUTE_NORMAL;

            if (!isDir)
            {
                UINT64 size =
                    file.isStreamed
                    ? file.size
                    : (UINT64)file.data.size();

                DirInfo->FileInfo.FileSize = size;
                DirInfo->FileInfo.AllocationSize = size;
            }

            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);

            UINT64 t =
                ((UINT64)ft.dwHighDateTime << 32)
                | ft.dwLowDateTime;

            DirInfo->FileInfo.CreationTime = t;
            DirInfo->FileInfo.LastAccessTime = t;
            DirInfo->FileInfo.LastWriteTime = t;
            DirInfo->FileInfo.ChangeTime = t;

            if (!FspFileSystemFillDirectoryBuffer(
                DirBufferPtr,
                DirInfo,
                &Result))
            {
                break;
            }
        }

        FspFileSystemReleaseDirectoryBuffer(
            DirBufferPtr);
    }

    if (!NT_SUCCESS(Result))
        return Result;

    FspFileSystemReadDirectoryBuffer(
        DirBufferPtr,
        Marker,
        Buffer,
        BufferLength,
        PBytesTransferred);

    return STATUS_SUCCESS;
}

static NTSTATUS SetDelete(
    FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext,
    PWSTR FileName,
    BOOLEAN DeleteFile)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(FileContext);
    UNREFERENCED_PARAMETER(FileName);
    UNREFERENCED_PARAMETER(DeleteFile);

    return STATUS_MEDIA_WRITE_PROTECTED;
}

static FSP_FILE_SYSTEM_INTERFACE PtfsInterface =
{
    .GetVolumeInfo = GetVolumeInfo,
    .SetVolumeLabel = SetVolumeLabel_,
    .GetSecurityByName = nullptr,
    .Create = Create,
    .Open = Open,
    .Overwrite = Overwrite,
    .Cleanup = Cleanup,
    .Close = Close,
    .Read = Read,
    .Write = Write,
    .Flush = Flush,
    .GetFileInfo = GetFileInfo,
    .SetBasicInfo = SetBasicInfo,
    .SetFileSize = SetFileSize,
    .Rename = Rename,
    .GetSecurity = GetSecurity,
    .SetSecurity = SetSecurity,
    .ReadDirectory = ReadDirectory,
    .SetDelete = SetDelete,
};

static VOID PtfsDelete(PTFS* Ptfs);

static NTSTATUS PtfsCreate(PWSTR Path, PWSTR VolumePrefix, PWSTR MountPoint, UINT32 DebugFlags,
    PTFS** PPtfs)
{
    WCHAR FullPath[MAX_PATH];
    ULONG Length;
    HANDLE Handle;
    FILETIME CreationTime;
    DWORD LastError;
    FSP_FSCTL_VOLUME_PARAMS VolumeParams;
    PTFS* Ptfs = 0;
    NTSTATUS Result;
    PWSTR DevicePath = NULL;

    *PPtfs = 0;

    Handle = CreateFileW(
        Path, FILE_READ_ATTRIBUTES, 0, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (INVALID_HANDLE_VALUE == Handle)
        return FspNtStatusFromWin32(GetLastError());

    Length = GetFinalPathNameByHandleW(Handle, FullPath, FULLPATH_SIZE - 1, 0);
    if (0 == Length)
    {
        LastError = GetLastError();
        CloseHandle(Handle);
        return FspNtStatusFromWin32(LastError);
    }
    if (L'\\' == FullPath[Length - 1])
        FullPath[--Length] = L'\0';

    if (!GetFileTime(Handle, &CreationTime, 0, 0))
    {
        LastError = GetLastError();
        CloseHandle(Handle);
        return FspNtStatusFromWin32(LastError);
    }

    CloseHandle(Handle);

    /* from now on we must goto exit on failure */

    Ptfs = (PTFS*)malloc(sizeof(*Ptfs));
    if (0 == Ptfs)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    memset(Ptfs, 0, sizeof * Ptfs);

    Length = (Length + 1) * sizeof(WCHAR);
    Ptfs->Path = (PWSTR)malloc(Length);
    if (0 == Ptfs->Path)
    {
        Result = STATUS_INSUFFICIENT_RESOURCES;
        goto exit;
    }
    memcpy(Ptfs->Path, FullPath, Length);

    memset(&VolumeParams, 0, sizeof VolumeParams);
    VolumeParams.SectorSize = ALLOCATION_UNIT;
    VolumeParams.SectorsPerAllocationUnit = 1;
    VolumeParams.VolumeCreationTime = ((PLARGE_INTEGER)&CreationTime)->QuadPart;
    VolumeParams.VolumeSerialNumber = 0;
    VolumeParams.FileInfoTimeout = 1000;
    VolumeParams.CaseSensitiveSearch = 0;
    VolumeParams.CasePreservedNames = 1;
    VolumeParams.UnicodeOnDisk = 1;
    VolumeParams.PersistentAcls = 0;
    VolumeParams.PostCleanupWhenModifiedOnly = 1;
    VolumeParams.PassQueryDirectoryPattern = 1;
    VolumeParams.FlushAndPurgeOnCleanup = 1;
    VolumeParams.UmFileContextIsUserContext2 = 1;
    if (0 != VolumePrefix)
        wcscpy_s(VolumeParams.Prefix, sizeof VolumeParams.Prefix / sizeof(WCHAR), VolumePrefix);
    wcscpy_s(VolumeParams.FileSystemName, sizeof VolumeParams.FileSystemName / sizeof(WCHAR),
        L"" PROGNAME);

    DevicePath =
        VolumeParams.Prefix[0]
        ? (PWSTR)L"" FSP_FSCTL_NET_DEVICE_NAME
        : (PWSTR)L"" FSP_FSCTL_DISK_DEVICE_NAME;

    Result = FspFileSystemCreate(
        DevicePath,
        &VolumeParams,
        &PtfsInterface,
        &Ptfs->FileSystem);
    if (!NT_SUCCESS(Result))
        goto exit;
    Ptfs->FileSystem->UserContext = Ptfs;

    Result = FspFileSystemSetMountPoint(Ptfs->FileSystem, MountPoint);
    if (!NT_SUCCESS(Result))
        goto exit;

    FspFileSystemSetDebugLog(Ptfs->FileSystem, DebugFlags);

    Result = STATUS_SUCCESS;

exit:
    if (NT_SUCCESS(Result))
        *PPtfs = Ptfs;
    else if (0 != Ptfs)
        PtfsDelete(Ptfs);

    return Result;
}

static VOID PtfsDelete(PTFS* Ptfs)
{
    if (0 != Ptfs->FileSystem)
        FspFileSystemDelete(Ptfs->FileSystem);

    if (0 != Ptfs->Path)
        free(Ptfs->Path);

    free(Ptfs);
}

static NTSTATUS EnableBackupRestorePrivileges(VOID)
{
    union
    {
        TOKEN_PRIVILEGES P;
        UINT8 B[sizeof(TOKEN_PRIVILEGES) + sizeof(LUID_AND_ATTRIBUTES)];
    } Privileges;
    HANDLE Token;

    Privileges.P.PrivilegeCount = 2;
    Privileges.P.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Privileges.P.Privileges[1].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValueW(0, SE_BACKUP_NAME, &Privileges.P.Privileges[0].Luid) ||
        !LookupPrivilegeValueW(0, SE_RESTORE_NAME, &Privileges.P.Privileges[1].Luid))
        return FspNtStatusFromWin32(GetLastError());

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &Token))
        return FspNtStatusFromWin32(GetLastError());

    if (!AdjustTokenPrivileges(Token, FALSE, &Privileges.P, 0, 0, 0))
    {
        CloseHandle(Token);

        return FspNtStatusFromWin32(GetLastError());
    }

    CloseHandle(Token);

    return STATUS_SUCCESS;
}

static ULONG wcstol_deflt(wchar_t* w, ULONG deflt)
{
    wchar_t* endp;
    ULONG ul = wcstol(w, &endp, 0);
    return L'\0' != w[0] && L'\0' == *endp ? ul : deflt;
}

static NTSTATUS SvcStart(FSP_SERVICE* Service, ULONG argc, PWSTR* argv)
{
#define argtos(v)                       if (arge > ++argp) v = *argp; else goto usage
#define argtol(v)                       if (arge > ++argp) v = wcstol_deflt(*argp, v); else goto usage

    wchar_t** argp, ** arge;
    PWSTR DebugLogFile = 0;
    ULONG DebugFlags = 0;
    PWSTR VolumePrefix = 0;
    PWSTR PassThrough = 0;
    PWSTR MountPoint = 0;
    HANDLE DebugLogHandle = INVALID_HANDLE_VALUE;
    WCHAR PassThroughBuf[MAX_PATH];
    PTFS* Ptfs = 0;
    NTSTATUS Result;

    for (argp = argv + 1, arge = argv + argc; arge > argp; argp++)
    {
        if (L'-' != argp[0][0])
            break;
        switch (argp[0][1])
        {
        case L'?':
            goto usage;
        case L'd':
            argtol(DebugFlags);
            break;
        case L'D':
            argtos(DebugLogFile);
            break;
        case L'm':
            argtos(MountPoint);
            break;
        case L'p':
            argtos(PassThrough);
            break;
        case L'u':
            argtos(VolumePrefix);
            break;
        default:
            goto usage;
        }
    }

    if (arge > argp)
        goto usage;

    if (0 == PassThrough && 0 != VolumePrefix)
    {
        PWSTR P;

        P = wcschr(VolumePrefix, L'\\');
        if (0 != P && L'\\' != P[1])
        {
            P = wcschr(P + 1, L'\\');
            if (0 != P &&
                (
                    (L'A' <= P[1] && P[1] <= L'Z') ||
                    (L'a' <= P[1] && P[1] <= L'z')
                    ) &&
                L'$' == P[2])
            {
                StringCbPrintf(PassThroughBuf, sizeof PassThroughBuf, L"%c:%s", P[1], P + 3);
                PassThrough = PassThroughBuf;
            }
        }
    }

    if (0 == PassThrough || 0 == MountPoint)
        goto usage;

    EnableBackupRestorePrivileges();

    if (0 != DebugLogFile)
    {
        if (0 == wcscmp(L"-", DebugLogFile))
            DebugLogHandle = GetStdHandle(STD_ERROR_HANDLE);
        else
            DebugLogHandle = CreateFileW(
                DebugLogFile,
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                0,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                0);
        if (INVALID_HANDLE_VALUE == DebugLogHandle)
        {
            fail((PWSTR)L"cannot open debug log file");
            goto usage;
        }

        FspDebugLogSetHandle(DebugLogHandle);
    }

    Result = PtfsCreate(PassThrough, VolumePrefix, MountPoint, DebugFlags, &Ptfs);
    if (!NT_SUCCESS(Result))
    {
        fail((PWSTR)L"cannot create file system");
        goto exit;
    }

    Result = FspFileSystemStartDispatcher(Ptfs->FileSystem, 0);
    if (!NT_SUCCESS(Result))
    {
        fail((PWSTR)L"cannot create file system");
        goto exit;
    }

    MountPoint = FspFileSystemMountPoint(Ptfs->FileSystem);

    info((PWSTR)L"%s%s%s -p %s -m %s",
        L"" PROGNAME,
        0 != VolumePrefix && L'\0' != VolumePrefix[0] ? L" -u " : L"",
        0 != VolumePrefix && L'\0' != VolumePrefix[0] ? VolumePrefix : L"",
        PassThrough,
        MountPoint);

    Service->UserContext = Ptfs;
    Result = STATUS_SUCCESS;

exit:
    if (!NT_SUCCESS(Result) && 0 != Ptfs)
        PtfsDelete(Ptfs);

    return Result;

    static wchar_t usage[] = L""
        "usage: %s OPTIONS\n"
        "\n"
        "options:\n"
        "    -d DebugFlags       [-1: enable all debug logs]\n"
        "    -D DebugLogFile     [file path; use - for stderr]\n"
        "    -u \\Server\\Share    [UNC prefix (single backslash)]\n"
        "    -p Directory        [directory to expose as pass through file system]\n"
        "    -m MountPoint       [X:|*|directory]\n";
usage:
    fail(usage, (PWSTR)L"" PROGNAME);

    return STATUS_UNSUCCESSFUL;

#undef argtos
#undef argtol
}

static NTSTATUS SvcStop(FSP_SERVICE* Service)
{
    PTFS* Ptfs = (PTFS*)Service->UserContext;

    FspFileSystemStopDispatcher(Ptfs->FileSystem);
    PtfsDelete(Ptfs);

    return STATUS_SUCCESS;
}

static PTFS* g_Ptfs = NULL;

extern "C" bool StartFileSystem(const wchar_t* path)
{
    NTSTATUS Result;

    if (!NT_SUCCESS(FspLoad(0)))
        return false;

    wchar_t mountPoint[4];

    char buf[32];
    sprintf_s(buf, "PTFS = %c", g_MountLetter);

    swprintf_s(mountPoint, L"%c:", (wchar_t)toupper(g_MountLetter));

    Result = PtfsCreate(
        (PWSTR)path,
        NULL,
        mountPoint,
        0,
        &g_Ptfs);

    if (!NT_SUCCESS(Result))
    {
        char buffer[128];

        sprintf_s(
            buffer,
            "PtfsCreate failed\nNTSTATUS = 0x%X",
            Result);

        MessageBoxA(
            NULL,
            buffer,
            "VirtualFS",
            MB_ICONERROR);

        return false;
    }

    Result = FspFileSystemStartDispatcher(
        g_Ptfs->FileSystem,
        0);

    if (!NT_SUCCESS(Result))
    {
        char buffer[128];

        sprintf_s(
            buffer,
            "Dispatcher failed\nNTSTATUS = 0x%X",
            Result);

        MessageBoxA(
            NULL,
            buffer,
            "VirtualFS",
            MB_ICONERROR);

        return false;
    }

    return true;
}

extern "C" void StopFileSystem()
{
    if (g_Ptfs)
    {
        FspFileSystemStopDispatcher(
            g_Ptfs->FileSystem);

        PtfsDelete(g_Ptfs);

        g_Ptfs = NULL;
    }
}

