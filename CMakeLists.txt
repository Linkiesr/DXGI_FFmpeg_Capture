cmake_minimum_required(VERSION 3.10)
project(DXGI_FFmpeg_Capture)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(FFMPEG_DIR "D:/ffmpeg" CACHE PATH "FFmpeg root directory")
set(QT_ROOT "D:/Qt/5.15.2/msvc2019_64" CACHE PATH "Qt root directory")
option(BUILD_QT_SOFTDECODE_DEMOS "Build Qt soft decode render demos" ON)

if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFMPEG_DIR is invalid: ${FFMPEG_DIR}. Expected ${FFMPEG_DIR}/include/libavcodec/avcodec.h")
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

function(copy_qt_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    set(_qt_bin_dir "${QT_ROOT}/bin")
    set(_qt_plugins_dir "${QT_ROOT}/plugins")

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_qt_bin_dir}/Qt5Core.dll"
            "${_qt_bin_dir}/Qt5Gui.dll"
            "${_qt_bin_dir}/Qt5Widgets.dll"
            $<TARGET_FILE_DIR:${target_name}>
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${target_name}>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_qt_plugins_dir}/platforms/qwindows.dll"
            "$<TARGET_FILE_DIR:${target_name}>/platforms"
    )
endfunction()

include_directories(
    ${FFMPEG_DIR}/include
)

link_directories(
    ${FFMPEG_DIR}/lib
)

add_executable(DXGI_FFmpeg_Capture main.cpp)

target_link_libraries(DXGI_FFmpeg_Capture
    avcodec
    avformat
    avutil
    swscale
    d3d11
    dxgi
)

add_custom_command(TARGET DXGI_FFmpeg_Capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_DIR}/bin"
        $<TARGET_FILE_DIR:DXGI_FFmpeg_Capture>
)

if(BUILD_QT_SOFTDECODE_DEMOS)
    list(APPEND CMAKE_PREFIX_PATH "${QT_ROOT}")
    find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)

    add_subdirectory(projects/common_softdecode)
    add_subdirectory(projects/best_qt_d3d11_softdecode)
    add_subdirectory(projects/second_qt_gl_softdecode)
endif()
