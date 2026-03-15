cmake_minimum_required(VERSION 3.10)
project(DXGI_FFmpeg_Capture)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FFMPEG_DIR "C:/Users/Lenovo/Desktop/Test/ffmpeg")

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

# Copy FFmpeg DLLs to output directory
add_custom_command(TARGET DXGI_FFmpeg_Capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_DIR}/bin"
        $<TARGET_FILE_DIR:DXGI_FFmpeg_Capture>
)
