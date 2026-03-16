cmake_minimum_required(VERSION 3.10)
project(DXGI_FFmpeg_Capture)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FFMPEG_DIR "C:/Users/wulintao/Desktop/ffmpeg")

if(MSVC)
    # /utf-8 标志：将源文件和执行字符集都设置为 UTF-8
    add_compile_options(/utf-8)
    
    # 可选：如果还需要设置执行字符集为 UTF-8 (通常 /utf-8 已经包含)
    # add_compile_options(/execution-charset:utf-8) 
endif()

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
