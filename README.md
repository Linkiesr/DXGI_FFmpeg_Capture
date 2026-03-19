# DXGI_FFmpeg_Capture

一个基于 Windows 的全 GPU 屏幕采集与 H.264 编码示例项目。  
当前主链路：

- `DXGI Desktop Duplication` 抓取桌面 BGRA 纹理
- `D3D11 shader (bilinear)` 先在 BGRA 空间缩放到可视区域
- `D3D11 VideoProcessor` 仅做 `BGRA -> NV12` 颜色转换
- `FFmpeg h264_nvenc` 硬件编码输出 `output.h264`

## 功能特性

- 全 GPU 流水线，CPU 参与度低
- 可视区域动态缩放（码流分辨率保持屏幕分辨率）
- 可视区域左上角对齐显示
- 屏幕无变化时降低无效编码推帧
- 控制台实时输出 FPS/延迟

## 运行环境

- Windows 10/11 或 Windows Server（需可用交互桌面会话）
- Visual Studio 2019/2022（MSVC）
- CMake >= 3.10
- FFmpeg 开发库（`include/lib/bin` 同一套版本）
- 支持 D3D11 的 NVIDIA GPU（NVENC）

## 项目结构

- `main.cpp`: 核心实现（抓屏、缩放、颜色转换、编码、性能统计）
- `CMakeLists.txt`: 构建脚本
- `CMakeSettings.json`: VS CMake 示例配置

## 构建前配置

`CMakeLists.txt` 中需设置 FFmpeg 根目录：

```cmake
set(FFMPEG_DIR "D:/ffmpeg")
```

请改为你本机路径，且确保存在：

- `${FFMPEG_DIR}/include`
- `${FFMPEG_DIR}/lib`
- `${FFMPEG_DIR}/bin`

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

构建后会自动把 `${FFMPEG_DIR}/bin` 下 DLL 拷贝到可执行目录。

## 运行

```powershell
.\build\Release\DXGI_FFmpeg_Capture.exe [visible_width] [visible_height]
```

参数说明：

- 不传参数：可视区域默认等于屏幕分辨率
- 传参数：可视区域使用指定宽高（会自动修正为偶数，并钳制不超过屏幕分辨率）

热键：

- `F6`: 切换可视区域为 `1280x720`
- `F7`: 切换可视区域为 `1920x1080`
- `F8`: 恢复可视区域为屏幕分辨率

默认测试会编码 1000 帧，输出 `output.h264`（裸 H.264 Annex B 流）。

## 输出文件

- 输出文件：`output.h264`
- 直接播放：

```powershell
ffplay .\output.h264
```

- 封装为 MP4：

```powershell
ffmpeg -framerate 60 -i .\output.h264 -c copy .\output.mp4
```

## 常见问题

1. `h264_nvenc` 打不开（如 `OpenEncodeSessionEx failed` 或 `cuInit failed`）

- 多数是 NVENC/CUDA 运行环境问题，不是业务逻辑问题。
- 优先检查：
  - 驱动是否最新并重启
  - 是否本地交互会话（RDP 会话常不稳定）
  - 是否混用了多套 FFmpeg / NVIDIA DLL
  - 可执行目录是否误放了不匹配的 `nvcuda.dll` / `nvEncodeAPI64.dll`

2. 1080p 缩到 720p 出现边缘混色（粉边）

- 已改为“先 BGRA bilinear 缩放，再 VP 仅做 BGRA->NV12”来降低该问题。
- 若仍有少量残留，通常是 NV12（4:2:0）色度抽样带来的边缘色度损失。

3. 颜色偏亮/过曝

- 本项目已采用 limited-range 配置（16-235）降低过曝风险。
- 若播放端颜色管理异常，仍可能出现观感偏差。

## 说明

本项目用于测试 GPU 编码链路与问题定位，不包含软件编码兜底路径。  
若 NVENC 环境不可用，程序会初始化失败并打印错误日志。

