#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// 解码输出像素格式。
enum class DecodedPixelFormat {
    YUV420P,
    RGB24,
};

// CPU 侧视频帧结构，用于解码模块与渲染模块之间传递。
struct DecodedFrame {
    int width = 0;
    int height = 0;
    int64_t pts = 0;
    DecodedPixelFormat format = DecodedPixelFormat::YUV420P;

    // format=YUV420P 时使用。
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;

    // format=RGB24 时使用（按 RGBRGB... 紧凑排列）。
    std::vector<uint8_t> rgb;
};

// 共享所有权便于在 Qt 信号跨线程排队传递时保证生命周期安全。
using DecodedFramePtr = std::shared_ptr<DecodedFrame>;
