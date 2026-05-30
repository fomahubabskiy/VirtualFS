#include "Extractor.h"
#include <archive.h>
#include <archive_entry.h>
#include <windows.h>
#include <vector>
#include <atomic>

bool ExtractAll(const std::string& archivePath,
    const std::string& outDir,
    std::atomic<float>& progress)
{
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archivePath.c_str(), 10240) != ARCHIVE_OK)
    {
        archive_read_free(a);
        return false;
    }

    // считаем сколько файлов
    std::vector<std::string> entries;
    struct archive_entry* entry;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
    {
        const char* name = archive_entry_pathname(entry);
        if (name)
            entries.push_back(name);

        archive_read_data_skip(a);
    }

    archive_read_free(a);

    // открываем заново для реального извлечения
    a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archivePath.c_str(), 10240) != ARCHIVE_OK)
    {
        archive_read_free(a);
        return false;
    }

    size_t total = entries.size();
    size_t current = 0;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
    {
        const char* name = archive_entry_pathname(entry);
        if (!name) continue;

        std::string fullPath = outDir + "\\" + name;

        for (auto& c : fullPath)
            if (c == '/') c = '\\';

        // папки
        size_t pos = 0;
        while ((pos = fullPath.find('\\', pos)) != std::string::npos)
        {
            std::string dir = fullPath.substr(0, pos);
            CreateDirectoryA(dir.c_str(), NULL);
            pos++;
        }

        if (archive_entry_filetype(entry) == AE_IFDIR)
        {
            CreateDirectoryA(fullPath.c_str(), NULL);
        }
        else
        {
            HANDLE hFile = CreateFileA(
                fullPath.c_str(),
                GENERIC_WRITE,
                0,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL
            );

            if (hFile != INVALID_HANDLE_VALUE)
            {
                char buffer[8192];
                la_ssize_t size;

                while ((size = archive_read_data(a, buffer, sizeof(buffer))) > 0)
                {
                    DWORD written;
                    WriteFile(hFile, buffer, (DWORD)size, &written, NULL);
                }

                CloseHandle(hFile);
            }
        }

        archive_read_data_skip(a);

        // рефреш прогресс
        current++;
        progress = (float)current / (float)total;
    }

    archive_read_free(a);
    progress = 1.0f;
    return true;
}