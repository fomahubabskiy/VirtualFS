#include "../fs/virtual_disk.h"
#include "IsoReader.h"
#include <set>
#include <archive.h>
#include <archive_entry.h>

bool IsoReader::Open(const std::string& path)
{
	isoPath = path;

	struct archive* a =
		archive_read_new();

	archive_read_support_format_all(a);
	archive_read_support_filter_all(a);

	if (archive_read_open_filename(
		a,
		path.c_str(),
		10240) != ARCHIVE_OK)
	{
		archive_read_free(a);
		return false;
	}

	archive_read_free(a);
	return true;
}

bool IsoReader::ReadFileData(
	const std::string& path,
	uint64_t offset,
	uint32_t size,
	std::vector<char>& out)
{
	struct archive* a = archive_read_new();

	archive_read_support_format_all(a);
	archive_read_support_filter_none(a);

	if (archive_read_open_filename(a, isoPath.c_str(), 10240) != ARCHIVE_OK)
	{
		archive_read_free(a);
		return false;
	}

	struct archive_entry* entry = nullptr;

	while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
	{
		const char* name = archive_entry_pathname(entry);
		if (!name) continue;

		std::string entryPath = name;

		for (auto& c : entryPath)
			if (c == '\\') c = '/';

		while (!entryPath.empty() && entryPath[0] == '/')
			entryPath.erase(0, 1);

		if (_stricmp(entryPath.c_str(), path.c_str()) == 0)
		{
			// читаем файл
			std::vector<char> full;

			const size_t CHUNK = 64 * 1024;
			char buffer[CHUNK];

			while (true)
			{
				la_ssize_t r = archive_read_data(a, buffer, CHUNK);
				if (r <= 0) break;

				full.insert(full.end(), buffer, buffer + r);
			}

			// offset вручную
			if (offset >= full.size())
			{
				out.clear();
			}
			else
			{
				size_t toRead = std::min<size_t>(
					size,
					full.size() - offset);

				out.assign(
					full.begin() + offset,
					full.begin() + offset + toRead);
			}

			archive_read_free(a);
			return true;
		}

		archive_read_data_skip(a);
	}

	archive_read_free(a);
	return false;
}

std::vector<std::string> IsoReader::GetFiles()
{
	entries.clear();

	std::vector<std::string> files;

	struct archive* a =
		archive_read_new();

	archive_read_support_format_all(a);
	archive_read_support_filter_all(a);

	if (archive_read_open_filename(
		a,
		isoPath.c_str(),
		10240) != ARCHIVE_OK)
	{
		archive_read_free(a);
		return files;
	}

	struct archive_entry* entryPtr =
		nullptr;

	while (archive_read_next_header(
		a,
		&entryPtr) == ARCHIVE_OK)
	{
		if (!entryPtr)
			continue;

		const char* name =
			archive_entry_pathname(entryPtr);

		if (!name)
			continue;

		files.push_back(name);

		IsoEntry entry;

		entry.path = name;

		la_int64_t size =
			archive_entry_size(entryPtr);

		if (size < 0)
			size = 0;

		entry.size =
			(uint64_t)size;

		entry.isDirectory =
			archive_entry_filetype(entryPtr) == AE_IFDIR;

		entries.push_back(entry);

		archive_read_data_skip(a);
	}

	archive_read_free(a);

	return files;
}

std::vector<IsoEntry> IsoReader::GetEntries()
{
	return entries;
}

bool IsoReader::LoadToVFS(VirtualFileSystem& vfs)
{
	vfs.Clear();

	struct archive* a = archive_read_new();
	archive_read_support_format_all(a);
	archive_read_support_filter_none(a);

	if (archive_read_open_filename(a, isoPath.c_str(), 10240) != ARCHIVE_OK)
	{
		archive_read_free(a);
		return false;
	}

	const size_t MAX_RAM_FILE = 50 * 1024 * 1024; // 50MB

	std::set<std::string> addedDirs;
	struct archive_entry* entryPtr = nullptr;

	while (archive_read_next_header(a, &entryPtr) == ARCHIVE_OK)
	{
		if (!entryPtr) continue;

		// пропускаем hardlinks
		if (archive_entry_hardlink(entryPtr))
		{
			archive_read_data_skip(a);
			continue;
		}

		const char* name = archive_entry_pathname(entryPtr);
		if (!name) continue;

		std::string path = name;

		// нормализация пути
		for (auto& c : path)
			if (c == '\\') c = '/';

		while (!path.empty() && path[0] == '/')
			path.erase(0, 1);

		// СОЗДАНИЕ ПАПОК
		size_t pos = 0;
		while ((pos = path.find('/', pos)) != std::string::npos)
		{
			std::string dir = path.substr(0, pos);

			if (!dir.empty() && addedDirs.insert(dir).second)
			{
				VirtualFile dirFile;
				dirFile.path = dir;
				dirFile.isDirectory = true;
				vfs.AddFile(dirFile);
			}

			pos++;
		}

		// ФАЙЛ
		VirtualFile file;
		file.path = path;
		file.isDirectory = archive_entry_filetype(entryPtr) == AE_IFDIR;

		if (!file.isDirectory)
		{
			la_int64_t size = archive_entry_size(entryPtr);
			if (size < 0) size = 0;

			file.size = (uint64_t)size;

			if (size > 0 && size <= MAX_RAM_FILE)
			{
				file.data.resize((size_t)size);

				size_t total = 0;
				while (total < (size_t)size)
				{
					la_ssize_t r = archive_read_data(
						a,
						file.data.data() + total,
						(size_t)(size - total));

					if (r <= 0) break;
					total += r;
				}

				file.data.resize(total);
			}
			else
			{
				file.isStreamed = true;

				archive_read_data_skip(a);
			}

			vfs.AddFile(file);
		}
	}

	archive_read_free(a);
	return true;
}
IsoReader g_IsoReader;