add_library(common_softdecode STATIC
    decoder_thread.cpp
    decoder_thread.h
    decoded_frame.h
)

# 两个渲染示例共用的软解码模块。
target_include_directories(common_softdecode
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FFMPEG_DIR}/include
)

target_link_libraries(common_softdecode
    PUBLIC
        Qt5::Core
        avcodec
        avformat
        avutil
        swscale
)
