cmake_minimum_required(VERSION 3.16)
project(multicast2rtmp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)

set(SOURCES
    src/main.c
    src/pipeline.c
    src/utils.c
)

add_executable(multicast2rtmp ${SOURCES})

target_include_directories(multicast2rtmp PRIVATE src)

target_link_libraries(multicast2rtmp PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    pthread
    m
)

install(TARGETS multicast2rtmp DESTINATION bin)
