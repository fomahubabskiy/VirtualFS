cmake_minimum_required(VERSION 3.20)

project(VirtualFS)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_definitions(-DUNICODE -D_UNICODE)


# INCLUDE DIRECTORIES
include_directories(

    src

    # ImGui
    external/imgui
    external/imgui/backends

    # GLFW
    external/glfw/include

    # WinFsp
    "C:/Program Files (x86)/WinFsp/inc"

    # libarchive
    "${CMAKE_SOURCE_DIR}/include"
)


# LIB DIRECTORIES
link_directories(

    "C:/Program Files (x86)/WinFsp/lib"
    "${CMAKE_SOURCE_DIR}/lib"
)


# SOURCES
set(SOURCES

    # MAIN
    src/main.cpp

    # CORE
    src/core/VirtualDiskManager.cpp

    # ISO
    src/iso/IsoReader.cpp
    src/iso/Extractor.cpp

    # UI
    src/ui/MainWindow.cpp

    # FILE SYSTEM
    src/fs/passthrough.cpp
    src/fs/fs_bridge.cpp
    src/fs/virtual_disk.cpp

    # UTILS
    src/utils/FileUtils.cpp

    # IMGUI
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/imgui_demo.cpp

    # IMGUI BACKENDS
    external/imgui/backends/imgui_impl_glfw.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp
)


# EXECUTABLE
add_executable(VirtualFS ${SOURCES})


# LIBRARIES
target_link_libraries(VirtualFS

    # GLFW
    "${CMAKE_SOURCE_DIR}/external/glfw/lib-vc2022/glfw3.lib"

    # OpenGL
    opengl32

    # WinFsp
    winfsp-x64

    # libarchive
    archive
)

# DLLки
file(GLOB DLLS "${CMAKE_SOURCE_DIR}/dlls/*.dll")

add_custom_command(TARGET VirtualFS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${DLLS}
    $<TARGET_FILE_DIR:VirtualFS>
)

# OUTPUT
set_target_properties(VirtualFS PROPERTIES

    RUNTIME_OUTPUT_DIRECTORY
    ${CMAKE_BINARY_DIR}/bin
)

