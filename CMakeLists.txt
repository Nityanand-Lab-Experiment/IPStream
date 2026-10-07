cmake_minimum_required(VERSION 3.16)
project(ixgIPStream VERSION 4.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)

set(SOURCES
    src/main.c
    src/pipeline.c
    src/utils.c
)

add_executable(ixgIPStream ${SOURCES})

target_include_directories(ixgIPStream PRIVATE src)

target_link_libraries(ixgIPStream PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    pthread
    m
)

install(TARGETS ixgIPStream DESTINATION bin)
