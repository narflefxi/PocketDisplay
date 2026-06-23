cmake_minimum_required(VERSION 3.20)

# Honour MSVC_RUNTIME_LIBRARY target property (CMP0091) so we can link the
# static CRT below. Must be set before project().
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

project(PocketDisplay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(POCKETDISPLAY_ENABLE_X264 "Enable x264 encoder (GPL license)" OFF)

if(POCKETDISPLAY_ENABLE_X264)
    find_path(X264_INCLUDE_DIR NAMES x264.h REQUIRED)
    find_library(X264_LIBRARY NAMES libx264 x264 REQUIRED)
    get_filename_component(X264_LIBRARY_DIR "${X264_LIBRARY}" DIRECTORY)
    find_file(X264_RUNTIME_DLL NAMES libx264-164.dll libx264.dll
        PATHS
            "${X264_LIBRARY_DIR}/../bin"
            "${X264_LIBRARY_DIR}"
        NO_DEFAULT_PATH)
    if(X264_RUNTIME_DLL)
        get_filename_component(X264_RUNTIME_DLL_NAME "${X264_RUNTIME_DLL}" NAME)
    endif()
endif()

add_executable(PocketDisplay
    src/main.cpp
    src/Session.cpp
    src/GuiApp.cpp
    src/ScreenCapture.cpp
    src/Encoder.cpp
    src/HwEncoder.cpp
    src/TcpVideoServer.cpp
    src/AdbUsbSetup.cpp
    src/TouchReceiver.cpp
    src/VirtualDisplayDriver.cpp
    resources/resource.rc
)

set_source_files_properties(resources/resource.rc PROPERTIES
    COMPILE_FLAGS "/I\"${CMAKE_CURRENT_SOURCE_DIR}/resources\"")

# Link the static CRT so target machines do not need the VC++ Redistributable.
# Debug -> MultiThreadedDebug (/MTd), Release -> MultiThreaded (/MT).
set_property(TARGET PocketDisplay PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_include_directories(PocketDisplay PRIVATE src resources)
if(POCKETDISPLAY_ENABLE_X264)
    target_include_directories(PocketDisplay PRIVATE ${X264_INCLUDE_DIR})
endif()

target_link_libraries(PocketDisplay PRIVATE
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
    urlmon
    windowscodecs
    msimg32
)
if(POCKETDISPLAY_ENABLE_X264)
    target_link_libraries(PocketDisplay PRIVATE ${X264_LIBRARY})
endif()

add_custom_command(TARGET PocketDisplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../assets/logo-primary.png"
        "$<TARGET_FILE_DIR:PocketDisplay>/logo-primary.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
        "$<TARGET_FILE_DIR:PocketDisplay>/icon.ico"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_LICENSES.md"
        "$<TARGET_FILE_DIR:PocketDisplay>/LICENSES.txt"
    COMMENT "Copying runtime assets to output dir")

foreach(FONT Anton-Regular.ttf SpaceGrotesk-Medium.ttf SpaceGrotesk-Bold.ttf)
    add_custom_command(TARGET PocketDisplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../assets/${FONT}"
            "$<TARGET_FILE_DIR:PocketDisplay>/${FONT}")
endforeach()

if(POCKETDISPLAY_ENABLE_X264 AND X264_RUNTIME_DLL)
    add_custom_command(TARGET PocketDisplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${X264_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:PocketDisplay>/${X264_RUNTIME_DLL_NAME}")
endif()

# Stage the bundled adb (platform-tools) next to the exe so USB mode works
# without the user having adb on PATH. All 3 files must stay together.
set(PLATFORM_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/installer/platform-tools")
foreach(PT_FILE adb.exe AdbWinApi.dll AdbWinUsbApi.dll)
    if(EXISTS "${PLATFORM_TOOLS_DIR}/${PT_FILE}")
        add_custom_command(TARGET PocketDisplay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PocketDisplay>/platform-tools"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PLATFORM_TOOLS_DIR}/${PT_FILE}"
                "$<TARGET_FILE_DIR:PocketDisplay>/platform-tools/${PT_FILE}")
    endif()
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
if(POCKETDISPLAY_ENABLE_X264)
    target_compile_definitions(PocketDisplay PRIVATE POCKETDISPLAY_ENABLE_X264)
endif()

target_compile_options(PocketDisplay PRIVATE /W4 /WX- /utf-8)
