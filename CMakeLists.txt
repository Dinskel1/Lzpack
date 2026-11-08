cmake_minimum_required(VERSION 3.16)
project(lzpack VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lzpack
    src/main.cpp
    src/cli.cpp
    src/codec.cpp
    src/crc32.cpp
    src/display.cpp
    src/filter.cpp
    src/fio.cpp
    src/format.cpp
    src/matchfinder.cpp
    src/rangecoder.cpp
)

target_include_directories(lzpack PRIVATE src)

if(WIN32)
    target_link_libraries(lzpack PRIVATE shell32)
endif()

if(MSVC)
    target_compile_options(lzpack PRIVATE /W4 /permissive-)
else()
    target_compile_options(lzpack PRIVATE -Wall -Wextra -Wpedantic)
endif()
