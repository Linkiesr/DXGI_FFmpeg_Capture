add_executable(best_qt_d3d11_softdecode
    main.cpp
    best_main_window.cpp
    best_main_window.h
    d3d11_video_widget.cpp
    d3d11_video_widget.h
)

target_include_directories(best_qt_d3d11_softdecode
    PRIVATE
        ${CMAKE_SOURCE_DIR}/projects/common_softdecode
)

# 最佳路径示例：Qt 负责界面外壳，原生 D3D11 负责渲染。
target_link_libraries(best_qt_d3d11_softdecode
    PRIVATE
        common_softdecode
        Qt5::Core
        Qt5::Gui
        Qt5::Widgets
        d3d11
        dxgi
        d3dcompiler
)

# 复制 FFmpeg 运行时 DLL。
add_custom_command(TARGET best_qt_d3d11_softdecode POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_DIR}/bin"
        $<TARGET_FILE_DIR:best_qt_d3d11_softdecode>
)

# 复制 Qt 运行时 DLL 与 platforms 插件。
copy_qt_runtime_dlls(best_qt_d3d11_softdecode)

