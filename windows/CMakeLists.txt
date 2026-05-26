cmake_minimum_required(VERSION 3.20)
project(PocketDisplay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(X264_INCLUDE_DIR NAMES x264.h REQUIRED)
find_library(X264_LIBRARY NAMES libx264 x264 REQUIRED)

add_executable(PocketDisplay
    src/main.cpp
    src/GuiApp.cpp
    src/ScreenCapture.cpp
    src/Encoder.cpp
    src/HwEncoder.cpp
    src/UdpStreamer.cpp
    src/TcpVideoServer.cpp
    src/AdbUsbSetup.cpp
    src/TouchReceiver.cpp
    src/VirtualDisplayDriver.cpp
    resources/resource.rc
)

set_source_files_properties(resources/resource.rc PROPERTIES
    COMPILE_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}/resources\"")

target_include_directories(PocketDisplay PRIVATE src resources ${X264_INCLUDE_DIR})

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
    user32
    gdi32
    setupapi
    newdev
    windowscodecs
    msimg32
)

add_custom_command(TARGET PocketDisplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../assets/logo-primary.png"
        "$<TARGET_FILE_DIR:PocketDisplay>/logo-primary.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
        "$<TARGET_FILE_DIR:PocketDisplay>/icon.ico"
    COMMENT "Copying runtime assets to output dir")

foreach(FONT Anton-Regular.ttf SpaceGrotesk-Medium.ttf SpaceGrotesk-Bold.ttf)
    add_custom_command(TARGET PocketDisplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../assets/${FONT}"
            "$<TARGET_FILE_DIR:PocketDisplay>/${FONT}")
endforeach()

set(VDD_DRIVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/drivers/virtual-display")
foreach(VDD_FILE MttVDD.inf MttVDD.cat MttVDD.dll vdd_settings.xml)
    if(EXISTS "${VDD_DRIVER_DIR}/${VDD_FILE}")
        add_custom_command(TARGET PocketDisplay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PocketDisplay>/drivers/virtual-display"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VDD_DRIVER_DIR}/${VDD_FILE}"
                "$<TARGET_FILE_DIR:PocketDisplay>/drivers/virtual-display/${VDD_FILE}")
    endif()
endforeach()

target_compile_definitions(PocketDisplay PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
)

target_compile_options(PocketDisplay PRIVATE /W4 /WX- /utf-8)
