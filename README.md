# DXGI_FFmpeg_Capture

一个基于 Windows 的全 GPU 屏幕采集与 H.264 编码示例项目。  
采集链路使用 `DXGI Desktop Duplication`，颜色转换使用 `D3D11 VideoProcessor`，编码使用 `FFmpeg`（优先 `h264_nvenc`，失败时回退软件 H.264）。

## 功能特点

- DXGI 抓屏：低开销获取桌面纹理
- GPU 色彩转换：`BGRA -> NV12`
- 硬件编码优先：优先 `h264_nvenc`
- 帧复用：复用 `AVFrame`，减少频繁申请释放
- 变化驱动编码：屏幕无变化时降低无效编码
- 控制台性能输出：每秒打印 FPS 与延迟统计

## 运行环境

- Windows 10/11
- Visual Studio 2019/2022（MSVC）
- CMake >= 3.10
- FFmpeg 开发库（包含 `include` / `lib` / `bin`）
- 支持 D3D11 的显卡（使用 NVENC 时建议 NVIDIA）

## 目录说明

- `main.cpp`：核心实现（抓屏、转换、编码、主流程）
- `CMakeLists.txt`：构建脚本
- `CMakeSettings.json`：VS 的 CMake 配置示例

## 构建前配置

当前 `CMakeLists.txt` 中写死了：

```cmake
set(FFMPEG_DIR "D:/ffmpeg")
```

请改成你本机的 FFmpeg 路径，例如：

```cmake
set(FFMPEG_DIR "C:/ffmpeg")
```

并确保以下路径存在：

- `${FFMPEG_DIR}/include`
- `${FFMPEG_DIR}/lib`
- `${FFMPEG_DIR}/bin`

## 构建方法

在项目根目录执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

可执行文件会在：

- `build/Release/DXGI_FFmpeg_Capture.exe`（或 VS 对应输出目录）

构建后会自动把 `${FFMPEG_DIR}/bin` 下 DLL 复制到可执行文件目录。

## 运行

```powershell
.\build\Release\DXGI_FFmpeg_Capture.exe
```

程序默认行为：

- 读取主屏分辨率（宽高会处理为偶数）
- 采集并编码 1000 帧
- 输出裸流文件 `output.h264`

## 输出文件说明

- `output.h264` 是**裸 H.264 Annex B 流**，不是 MP4 容器。
- 可用 FFplay 直接播放：

```powershell
ffplay .\output.h264
```

- 如需封装成 MP4：

```powershell
ffmpeg -framerate 60 -i .\output.h264 -c copy .\output.mp4
```

`-framerate` 请按实际编码帧率调整。

## 常见问题

1. `h264_nvenc` 打不开
- 可能截图输出不在 NVIDIA 适配器上，或驱动/NVENC 运行环境不匹配。
- 程序会尝试回退到软件 H.264。

2. 运行时报找不到 FFmpeg DLL
- 确认可执行目录下存在 FFmpeg 的运行时 DLL（`avcodec*.dll` 等）。

3. 黑屏或抓取失败
- 检查系统是否支持 Desktop Duplication（Win10/11 通常支持）。
- 远程桌面、权限、显卡切换策略都可能影响抓屏。

## 代码结构（主流程）

- `DxgiScreenCapturer`：负责桌面捕获
- `H264TextureEncoder`：负责纹理编码
- `ScreenCapturePipeline`：负责调度与性能统计

