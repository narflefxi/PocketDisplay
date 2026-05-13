cmake_minimum_required(VERSION 3.20)
project(PocketDisplay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(X264_INCLUDE_DIR NAMES x264.h REQUIRED)
find_library(X264_LIBRARY NAMES libx264 x264 REQUIRED)

add_executable(PocketDisplay
    src/main.cpp
    src/ScreenCapture.cpp
    src/Encoder.cpp
    src/HwEncoder.cpp
    src/UdpStreamer.cpp
    src/TcpStreamer.cpp
    src/TcpVideoServer.cpp
    src/AdbUsbSetup.cpp
    src/TouchReceiver.cpp
)

target_include_directories(PocketDisplay PRIVATE src ${X264_INCLUDE_DIR})

target_link_libraries(PocketDisplay PRIVATE
    ${X264_LIBRARY}
    d3d11
    dxgi
    dxguid
    ws2_32
    shell32
    mf
    mfplat
    mfuuid
    ole32
)

target_compile_definitions(PocketDisplay PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_compile_options(PocketDisplay PRIVATE /W4 /WX- /utf-8)
