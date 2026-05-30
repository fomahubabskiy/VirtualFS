# VirtualFS
VirtualFS — это инструмент для монтирования ISO, ZIP и других архивов как виртуального диска (например X:) в Windows.

Программа позволяет работать с содержимым архивов так, как будто это обычный диск — без полной распаковки.

# Возможности

- Монтирование ISO файлов
- Поддержка ZIP и других архивов (через libarchive)
- Виртуальный диск через WinFsp
- Мгновенный доступ к файлам без извлечения
- Полное извлечение (Extract All)
- Фоновый сервис (работает отдельно от GUI)
- Автоматический mount через IPC (файл-команды)

# Архитектура (как это работает)

VirtualFS состоит из 2 частей:

## 1. GUI (основное приложение)

Отвечает за:
- интерфейс (ImGui)
- выбор файлов
- отображение прогресса
- отправку команд

GUI **НЕ монтирует сам**

## 2. Сервис (background process)

Запускается с параметром: VirtualFS.exe --service

Отвечает за:
- чтение архивов
- построение виртуальной файловой системы
- запуск WinFsp
- управление диском

## Связь GUI <-> сервис

Общение происходит через файлы в: C:\VirtualFSRoot\

### last_mount.txt
GUI пишет: <ID>|ANY|C:\path\to\file.iso


Сервис читает и выполняет команду.

### progress.txt
Сервис пишет прогресс:
    0.1 → загрузка ISO; 
    0.6 → построение VFS; 
    1.0 → готово;

GUI читает и отображает прогрессбар.

### status.txt

Сервис пишет текущий mounted файл: C:\file.iso
GUI использует это для отображения состояния.

# Полный процесс:


## Шаг 1 — пользователь нажимает Mount

GUI:
- сбрасывает прогресс
- генерирует ID (GetTickCount)
- пишет команду в *last_mount.txt*


## Шаг 2 — сервис ловит команду

Сервис:
- читает файл
- проверяет ID (чтобы не повторять)
- останавливает предыдущий mount


## Шаг 3 — загрузка архива

- IsoReader.Open()
- IsoReader.LoadToVFS()
- все файлы загружаются в виртуальную структуру (`g_VFS`)


## Шаг 4 — запуск файловой системы 

- StartFileSystem("C:\VirtualFSRoot")

WinFsp:
- создает виртуальный диск
- проксирует файловые операции в код


## Шаг 5 — пользователь открывает X:

Windows думает, что это обычный диск, но на самом деле:
- все запросы идут в `virtual_disk.cpp`


## Шаг 6 — чтение файлов

- Когда пользователь открывает файл:  Explorer → WinFsp → твой FS → g_VFS → данные

# Извлечение (Extract ALL)

Работает отдельно:
- запускается поток
- читает архив через libarchive
- записывает на диск
- обновляет `extractProgress`

# Почему это быстро?

- не нужно распаковывать архив  
- читаются только нужные файлы  
- минимум IO операций  

# Зависимости

- **WinFsp** — виртуальная файловая система
- **ImGui** — UI
- **GLFW** — окно
- **OpenGL** — рендеринг
- **libarchive** — работа с архивами

# Установка

Используйте установщик: VirtualFS_Installer.exe
Он:
- копирует exe и dll
- устанавливает WinFsp
- добавляет автозапуск сервиса
ДЛЯ ОТКЛЮЧЕНИЯ АВТОЗАПУСКА СЕРВИСА:
- можно удалить VirtualFS service из реестра: - HKCU\Software\Microsoft\Windows\CurrentVersion\Run
- при удалении программы *unins000.exe* - сервис удаляется автоматически.



# О том как создать VirtualFS.

## 1. Описание проекта
В качестве основы использовался пример `passthrough.c` из официального набора примеров WinFsp. Исходный пример был существенно переработан и расширен.
---

## 2. Используемые технологии

* C++17
* WinFsp
* libarchive
* WinAPI
* ImGui
* GLFW
* OpenGL

---

## 3. Исходное ядро WinFsp

За основу был взят пример `passthrough.c` из WinFsp. Данный пример реализует passthrough-файловую систему, которая отображает содержимое реальной папки в виде отдельного диска.

Создание файловой системы выполняется через:

```cpp
Result = FspFileSystemCreate(
    VolumeParams.Prefix[0]
        ? L"" FSP_FSCTL_NET_DEVICE_NAME
        : L"" FSP_FSCTL_DISK_DEVICE_NAME,
    &VolumeParams,
    &PtfsInterface,
    &Ptfs->FileSystem);
```

В оригинальном варианте все операции чтения и записи происходят через WinAPI и реальные файлы на диске.

Например, чтение файла выполняется следующим кодом:

```cpp
if (!ReadFile(
        Handle,
        Buffer,
        Length,
        PBytesTransferred,
        &Overlapped))
{
    return FspNtStatusFromWin32(
        GetLastError());
}
```

Недостатком такого подхода является невозможность работы с ISO, ZIP, RAR и другими - образами напрямую.

Из оригинального файла `passthrough.c` были использованы:

* механизм создания файловой системы
* интерфейс `FSP_FILE_SYSTEM_INTERFACE`
* диспетчер WinFsp
* базовые файловые операции
* механизм монтирования виртуального диска
* интеграция с Проводником Windows

Основные функции исходного проекта:

```cpp
Create()
Open()
Read()
Write()
ReadDirectory()
GetFileInfo()
Rename()
SetDelete()
```

## 4. Изменение ядра WinFsp

### 4.1 Изменение механизма чтения файлов

#### Было

В оригинальном проекте чтение происходило напрямую через WinAPI:

```cpp
ReadFile(...)
```

#### Стало

Алгоритм был изменён:

```text
Поиск файла в VirtualFileSystem
          │
          ▼
 Проверка способа хранения
          │
 ┌────────┴────────┐
 ▼                 ▼
 RAM          Потоковое чтение
 ▼                 ▼
data[]      IsoReader::ReadFileData()
```

### 4.2 Изменение функции Read

В оригинальном WinFsp использовался вызов:

```cpp
if (!ReadFile(
        Handle,
        Buffer,
        Length,
        PBytesTransferred,
        &Overlapped))
{
    return FspNtStatusFromWin32(
        GetLastError());
}
```

После внедрения VirtualFileSystem чтение было перенесено на работу с объектами VFS.

Поиск файла выполняется через:

```cpp
for (auto& file : g_VFS.files)
{
    if (file.path == requestedPath)
    {
        ...
    }
}
```

Если файл хранится в памяти:

```cpp
memcpy(
    Buffer,
    file.data.data() + Offset,
    Length);
```

Если файл большой:

```cpp
g_IsoReader.ReadFileData(
    file.path,
    Offset,
    Length,
    outData);
```

### 4.3 Изменение ReadDirectory

#### Было

Оригинальный код использовал:

```cpp
FindFirstFileW()
FindNextFileW()
```

для чтения содержимого физической папки.

#### Стало

Содержимое формируется из объектов VFS:

```cpp
for (auto& file : g_VFS.files)
{
    ...
}
```

Схема работы:

```text
ReadDirectory()
      │
      ▼
g_VFS.files
      │
      ▼
FSP_FSCTL_DIR_INFO
      │
      ▼
Explorer.exe
```

Для каждого файла создаётся структура:

```cpp
FSP_FSCTL_DIR_INFO dirInfo;
```

После чего она добавляется в буфер WinFsp:

```cpp
FspFileSystemFillDirectoryBuffer(
    &FileContext->DirBuffer,
    &dirInfo,
    &Result);
```

### 4.4 Добавление в буфер:

```cpp
FspFileSystemFillDirectoryBuffer(
DirBufferPtr,
DirInfo,
&Result))
```
### 4.5 Изменение Open

#### Было

```cpp
CreateFileW(...)
```

#### Стало

Открывается виртуальный объект:

```text
Поиск VirtualFile
        │
        ▼
Создание контекста
        │
        ▼
Возврат виртуального дескриптора
```

### 4.6 Изменение GetFileInfo

#### Было

```cpp
GetFileInformationByHandle(...)
```

#### Стало

Информация берётся из структуры:

```cpp
VirtualFile
```

## 5. Создание VirtualFileSystem

### Файлы

```text
virtual_disk.h
virtual_disk.cpp
```

### Структура VirtualFile

```cpp
struct VirtualFile
{
    std::string path;
    std::vector<char> data;

    bool isDirectory = false;
    uint64_t size = 0;

    bool isStreamed = false;
    uint64_t archiveOffset = 0;
};
```

### Описание полей

| Поле          | Назначение                     |
| ------------- | ------------------------------ |
| path          | путь внутри виртуального диска |
| data          | содержимое файла               |
| isDirectory   | является ли объект каталогом   |
| size          | размер файла                   |
| isStreamed    | потоковый режим чтения         |
| archiveOffset | смещение в архиве              |

### Класс VirtualFileSystem

```cpp
class VirtualFileSystem
{
public:
    std::vector<VirtualFile> files;

    void AddFile(const VirtualFile& file);
    void Clear();
};
```

### Реализация методов

```cpp
void VirtualFileSystem::AddFile(
    const VirtualFile& file)
{
    files.push_back(file);
}
```

```cpp
void VirtualFileSystem::Clear()
{
    files.clear();
}
```

Глобальный экземпляр системы:

```cpp
VirtualFileSystem g_VFS;
```

## 6. Создание IsoReader

### Файлы

```text
IsoReader.h
IsoReader.cpp
```

### Назначение

Модуль предназначен для чтения ISO-образов и архивов с использованием библиотеки libarchive.

### Подключение библиотек

```cpp
#include <archive.h>
#include <archive_entry.h>
```

### Структура IsoEntry

```cpp
struct IsoEntry
{
    std::string path;
    uint64_t size = 0;
    bool isDirectory = false;
};
```

### Открытие ISO

```cpp
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
```

### Основные методы

| Метод                                 | Назначение                                              |
| ------------------------------------- | ------------------------------------------------------- |
| Open(path)                            | Открывает ISO-образ                                     |
| GetFiles()                            | Возвращает список файлов                                |
| GetEntries()                          | Возвращает расширенную информацию о содержимом          |
| LoadToVFS(vfs)                        | Загружает содержимое ISO в виртуальную файловую систему |
| ReadFileData(path, offset, size, out) | Читает данные непосредственно из ISO                    |

### Алгоритм работы LoadToVFS

```text
Открытие ISO
      │
      ▼
Чтение записей
      │
      ▼
Создание директорий
      │
      ▼
Создание VirtualFile
      │
      ▼
Добавление в VFS
```

```cpp
vfs.AddFile(file);
```

### Чтение данных из ISO

```cpp
bool IsoReader::ReadFileData(
    const std::string& path,
    uint64_t offset,
    uint32_t size,
    std::vector<char>& out);
```

## 7. Оптимизация памяти (двухуровневое хранение)

В проекте реализована схема двухуровневого хранения данных.

```cpp
const size_t MAX_RAM_FILE =
    50 * 1024 * 1024;
```

### Малые файлы

Если размер файла меньше 50 МБ:

```text
Полная загрузка в память
```

### Большие файлы

Если размер файла больше 50 МБ:

```cpp
file.isStreamed = true;
```

Файл читается напрямую из ISO без полной загрузки в RAM.

Схема потокового чтения:

```text
ISO
 │
 ▼
ReadFileData()
 │
 ▼
Буфер
 │
 ▼
Explorer
```

## 8. Создание FSBridge

### Файлы

```text
fs_bridge.h
fs_bridge.cpp
```

### Интерфейс

```cpp
extern "C"
{
    bool StartFileSystem(
        const wchar_t* path);

    void StopFileSystem();
}
```

### Назначение

Связующий слой между графическим интерфейсом и WinFsp.

```text
GUI
 │
 ▼
FSBridge
 │
 ▼
WinFsp
```

## 9. Создание VirtualDiskManager

### Файлы

```text
VirtualDiskManager.h
VirtualDiskManager.cpp
```

### Назначение

Управление жизненным циклом виртуального диска.

### Интерфейс

```cpp
class VirtualDiskManager
{
public:
    bool MountFolder(
        const std::string& folderPath);

    bool MountISO(
        const std::string& isoPath);

    void Unmount();

    bool IsMounted() const;

    std::string GetMountPoint() const;

private:
    std::string mountedPath;
    std::string mountPoint = "X:";
};
```

### Реализация монтирования ISO

```cpp
bool VirtualDiskManager::MountISO(
    const std::string& isoPath)
{
    mountedPath = isoPath;

    std::cout
        << "[INFO] ISO mounted: "
        << isoPath
        << std::endl;

    return true;
}
```

### Возможности

* подключение ISO
* подключение директорий
* размонтирование
* получение точки монтирования
* проверка состояния подключения

## 10. Создание Extractor

### Файлы

```text
Extractor.h
Extractor.cpp
```

### Назначение

Извлечение содержимого архивов и ISO на физический диск.

### Создание файла

```cpp
HANDLE hFile = CreateFileA(
    fullPath.c_str(),
    GENERIC_WRITE,
    0,
    NULL,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    NULL);
```

### Запись данных

```cpp
WriteFile(
    hFile,
    buffer,
    (DWORD)size,
    &written,
    NULL);
```

### Отслеживание прогресса

```cpp
progress =
    (float)current /
    (float)total;
```

### Интерфейс

```cpp
bool ExtractAll(
    const std::string& archivePath,
    const std::string& outDir,
    std::atomic<float>& progress);
```

### Возможности

* создание директорий
* создание файлов
* запись данных
* отслеживание прогресса
* поддержка различных форматов архивов через libarchive

## 11. Создание main.cpp (GUI)

### Назначение

Главное окно приложения.

Отвечает за:

* выбор ISO-файла
* выбор директории
* запуск монтирования
* отключение виртуального диска
* отображение прогресса
* запуск извлечения архива

### Подключение ImGui

Для интерфейса использовалась библиотека ImGui.

### Выбор файла

Кнопка выбора ISO:

```cpp
if (ImGui::Button("Open ISO"))
{
    selectedFile =
        OpenFileDialog();
}
```

После выбора путь сохраняется:

```cpp
selectedFile = fileName;
```

### Монтирование ISO

Кнопка монтирования:

```cpp
if (ImGui::Button("Mount"))
{
    g_IsoReader.Open(
        selectedFile);

    g_IsoReader.LoadToVFS(
        g_VFS);

    StartFileSystem(
        Utf8ToWString(
            selectedFile).c_str());
}
```

### Размонтирование

Кнопка размонтирования:

```cpp
if (ImGui::Button("Unmount"))
{
    StopFileSystem();

    g_VFS.Clear();
}
```

### Отображение прогресса

```cpp
ImGui::ProgressBar(
    displayProgress,
    ImVec2(300, 20));
```

### Структура GUI

```text
+------------------------+
| Select ISO             |
+------------------------+

[ Open ISO ]

archive.iso

[ Mount ]
[ Unmount ]

Progress:
██████████
```

## 12. Архитектура проекта

### Исходная архитектура WinFsp

```text
Windows
   │
   ▼
WinFsp Driver
   │
   ▼
passthrough.c
   │
   ▼
Реальная папка на диске
```

### Новая архитектура проекта

```text
ISO
 │
 ▼
IsoReader
 │
 ▼
VirtualFileSystem
 │
 ▼
WinFsp Core
 │
 ▼
Проводник Windows
 │
 ▼
Виртуальный диск X:
```

### Структура проекта

```text
src/
│
├── fs/
│   ├── virtual_disk.h
│   ├── virtual_disk.cpp
│   ├── fs_bridge.h
│   |── fs_bridge.cpp
│   └── passthrough.cpp
│
├── iso/
│   ├── IsoReader.h
│   |── IsoReader.cpp
│   ├── Extractor.h
│   └── Extractor.cpp
│
├── core/
│   ├── VirtualDiskManager.h
│   └── VirtualDiskManager.cpp
|
├── ui/
│   ├── Mainwindow.h
│   └── Mainwindow.cpp
│
├── main.cpp
│
└── CMakeLists.txt
```

## 13. Алгоритм работы

### Полная цепочка монтирования ISO

```text
Пользователь выбирает ISO
            │
            ▼
      OpenFileDialog()
            │
            ▼
      IsoReader::Open()
            │
            ▼
      LoadToVFS()
            │
            ▼
     StartFileSystem()
            │
            ▼
       Создание диска
            │
            ▼
 Проводник Windows получает доступ
 к содержимому ISO
```

### Последовательность действий при нажатии Mount

```cpp
g_IsoReader.Open(selectedFile);
g_IsoReader.LoadToVFS(g_VFS);
StartFileSystem(isoPath.c_str());
```

Полная цепочка:

```text
Нажатие Mount
       │
       ▼
Открытие ISO
       │
       ▼
Заполнение VFS
       │
       ▼
Запуск WinFsp
       │
       ▼
Появление диска
```

### Последовательность действий при размонтировании

```text
Unmount
   │
   ▼
StopFileSystem()
   │
   ▼
Clear()
   │
   ▼
Удаление содержимого VFS
```

## 14. Результат

В ходе выполнения проекта демонстрационный пример WinFsp был преобразован в полноценную виртуальную файловую систему с поддержкой ISO-образов.

Были реализованы:

* собственная виртуальная файловая система (VFS)
* поддержка ISO через libarchive
* потоковое чтение больших файлов
* извлечение архивов
* управление виртуальными дисками
* графический интерфейс на ImGui
* интеграция с Проводником Windows через WinFsp

### Сравнительная таблица

| Компонент        | Исходный проект   | После доработки      |
| ---------------- | ----------------- | -------------------- |
| Источник данных  | Папка Windows     | ISO-файл             |
| Чтение файлов    | ReadFile()        | VirtualFileSystem    |
| Каталоги         | FindFirstFileW()  | g_VFS.files          |
| Интерфейс        | Консоль           | ImGui GUI            |
| Большие файлы    | Не поддерживались | Потоковое чтение     |
| Виртуальный диск | Passthrough       | Полноценный ISO-диск |

Проект представляет собой самостоятельную файловую систему пользовательского режима, построенную на основе WinFsp и расширенную собственными модулями для работы с ISO-образами и виртуальными дисками.






