add_executable(second_qt_gl_softdecode
    main.cpp
    second_main_window.cpp
    second_main_window.h
    gl_video_widget.cpp
    gl_video_widget.h
)

target_include_directories(second_qt_gl_softdecode
    PRIVATE
        ${CMAKE_SOURCE_DIR}/projects/common_softdecode
)

# 次优路径示例：在 Qt/OpenGL 内部完成渲染。
target_link_libraries(second_qt_gl_softdecode
    PRIVATE
        common_softdecode
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
        opengl32
)

# 复制 FFmpeg 运行时 DLL。
add_custom_command(TARGET second_qt_gl_softdecode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_DIR}/bin"
        $<TARGET_FILE_DIR:second_qt_gl_softdecode>
)

# 复制 Qt 运行时 DLL 与 platforms 插件。
copy_qt_runtime_dlls(second_qt_gl_softdecode)

