cmake_minimum_required(VERSION 3.21)
project(DriveMount LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(DriveMount
    src/main.cpp
    src/virtual_fs.cpp
    src/virtual_fs.h
    src/google_drive_client.cpp
    src/google_drive_client.h
    src/cache_manager.cpp
    src/cache_manager.h
)

set(WINFSP_DIR "C:/Program Files (x86)/WinFsp")

target_include_directories(DriveMount PRIVATE
    "${WINFSP_DIR}/inc"
    "src"
)

target_link_libraries(DriveMount PRIVATE
    "${WINFSP_DIR}/lib/winfsp-x64.lib"
    advapi32
    winhttp
        delayimp
)

target_compile_definitions(DriveMount PRIVATE UNICODE _UNICODE NOMINMAX)

target_link_options(DriveMount PRIVATE
    "/DELAYLOAD:winfsp-x64.dll"
)

