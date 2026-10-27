cmake_minimum_required(VERSION 3.16)
project(ixgIPStream VERSION 4.2 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -Wno-multichar")

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
pkg_check_modules(SRT srt)

set(SOURCES
    src/main.c
    src/pipeline.c
    src/utils.c
    src/logger.c
    src/rtmp.c
    src/hls.c
    src/srt.c
    src/transcode.c
    src/sdi_decklink.cpp
    BMSDK_14_2/Linux/include/DeckLinkAPIDispatch.cpp
)

add_executable(ixgIPStream ${SOURCES})

target_include_directories(ixgIPStream PRIVATE 
    src
    ${CMAKE_SOURCE_DIR}/BMSDK_14_2/Linux/include
)
if (SRT_INCLUDE_DIRS)
    target_include_directories(ixgIPStream PRIVATE ${SRT_INCLUDE_DIRS})
endif()

target_link_libraries(ixgIPStream PRIVATE
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::SWRESAMPLE
    ${SRT_LIBRARIES}
    pthread
    dl
    m
    stdc++
)

install(TARGETS ixgIPStream DESTINATION bin)
