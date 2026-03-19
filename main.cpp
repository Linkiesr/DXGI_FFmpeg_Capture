#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <thread>
#include <string>
#include <cstdint>
#include <clocale>
#include <cstdlib>
#include <d3d11.h>
#include <dxgi1_2.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

// 链接 D3D11 和 DXGI 库
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace std::chrono;

// 简单的 COM 资源释放辅助函数
template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

static int NormalizeEvenDimension(int value) {
    if (value < 2) value = 2;
    return value & ~1;
}

/**
 * DxgiScreenCapturer 类：专门负责屏幕截图。
 * 流程：D3D11 设备初始化 -> DXGI 桌面复制 -> 输出最新一帧的桌面纹理
 *
 * 说明：
 * 1. 这个类只关心“拿到最新屏幕画面”；
 * 2. 如果桌面画面没有变化，则不会返回新帧；
 * 3. 为了让外部在 ReleaseFrame 后还能继续使用纹理，这里会先复制到持久纹理再输出。
 */
class DxgiScreenCapturer {
public:
    DxgiScreenCapturer() = default;
    ~DxgiScreenCapturer() { Cleanup(); }

    /**
     * 初始化截图相关硬件资源
     * @param w 捕获宽度
     * @param h 捕获高度
     */
    bool Initialize(int w, int h) {
        Cleanup();
        width = NormalizeEvenDimension(w);
        height = NormalizeEvenDimension(h);

        std::cout << "[Init] 初始化 D3D11 设备..." << std::endl;
        if (!InitD3D11()) {
            Cleanup();
            return false;
        }

        std::cout << "[Init] 设置 DXGI 桌面复制..." << std::endl;
        if (!InitDXGI()) {
            Cleanup();
            return false;
        }

        std::cout << "[Init] 初始化截图持久纹理..." << std::endl;
        if (!InitPersistentTexture()) {
            Cleanup();
            return false;
        }

        std::cout << "[Init] 截图模块就绪。" << std::endl;
        return true;
    }

    /**
     * 捕获一帧桌面纹理
     * @param hasNewFrame 是否真的拿到了新画面
     * @param textureOut 输出的 BGRA 纹理（调用方使用完后需要 Release）
     * @return true 表示流程正常；false 表示截图流程出错
     */
    bool CaptureFrame(bool& hasNewFrame, ID3D11Texture2D** textureOut) {
        hasNewFrame = false;
        if (textureOut) *textureOut = nullptr;

        IDXGIResource* desktopRes = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

        // 非阻塞捕获：Timeout 0
        HRESULT hr = desk_dupl->AcquireNextFrame(0, &frameInfo, &desktopRes);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // 屏幕没有新变化：直接跳过，不继续推帧
            return true;
        }
        if (FAILED(hr)) {
            std::cerr << "AcquireNextFrame 失败: " << std::hex << hr << std::endl;
            return false;
        }

        ID3D11Texture2D* capturedTex = nullptr;
        hr = desktopRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&capturedTex);
        SafeRelease(desktopRes);
        if (FAILED(hr) || !capturedTex) {
            desk_dupl->ReleaseFrame();
            std::cerr << "桌面纹理转换失败: " << std::hex << hr << std::endl;
            return false;
        }

        // 将截取的屏幕纹理复制到持久纹理 (GPU 内部复制)
        context->CopyResource(persistent_tex, capturedTex);
        SafeRelease(capturedTex);
        desk_dupl->ReleaseFrame();

        if (!persistent_tex || !textureOut) {
            std::cerr << "持久纹理无效" << std::endl;
            return false;
        }

        // 输出持久纹理，供后续编码阶段继续使用
        persistent_tex->AddRef();
        *textureOut = persistent_tex;
        hasNewFrame = true;
        return true;
    }

    // 对外提供底层 D3D11 设备，供编码器复用同一套 GPU 资源
    ID3D11Device* GetDevice() const { return device; }
    ID3D11DeviceContext* GetContext() const { return context; }

private:
    int width = 0;
    int height = 0;

    // D3D11 & DXGI 资源
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* desk_dupl = nullptr;

    // 为了让截图和编码稳定落在同一张显卡上，这里显式记录选中的适配器和输出
    IDXGIAdapter1* selected_adapter = nullptr;
    UINT selected_output_index = 0;
    DXGI_ADAPTER_DESC1 selected_adapter_desc = {};
    DXGI_OUTPUT_DESC selected_output_desc = {};

    // 持久纹理：缓存最近一次捕获到的桌面画面
    ID3D11Texture2D* persistent_tex = nullptr;

    // 判断一个输出是否为主显示器（桌面左上角通常为 0,0）
    static bool IsPrimaryOutput(const DXGI_OUTPUT_DESC& desc) {
        return desc.AttachedToDesktop && desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;
    }

    // 选择最合适的输出：优先主显示器，其次优先匹配当前分辨率，再其次优先 NVIDIA
    bool SelectCaptureOutput() {
        SafeRelease(selected_adapter);
        selected_output_index = 0;
        selected_adapter_desc = {};
        selected_output_desc = {};

        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
        if (FAILED(hr) || !factory) {
            std::cerr << "CreateDXGIFactory1 失败: " << std::hex << hr << std::endl;
            return false;
        }

        int bestScore = -1;
        for (UINT adapterIndex = 0;; ++adapterIndex) {
            IDXGIAdapter1* adapter = nullptr;
            hr = factory->EnumAdapters1(adapterIndex, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr) || !adapter) continue;

            DXGI_ADAPTER_DESC1 adapterDesc = {};
            if (FAILED(adapter->GetDesc1(&adapterDesc))) {
                SafeRelease(adapter);
                continue;
            }

            // 跳过软件适配器
            if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                SafeRelease(adapter);
                continue;
            }

            for (UINT outputIndex = 0;; ++outputIndex) {
                IDXGIOutput* output = nullptr;
                hr = adapter->EnumOutputs(outputIndex, &output);
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hr) || !output) continue;

                DXGI_OUTPUT_DESC outputDesc = {};
                if (FAILED(output->GetDesc(&outputDesc))) {
                    SafeRelease(output);
                    continue;
                }
                SafeRelease(output);

                if (!outputDesc.AttachedToDesktop) {
                    continue;
                }

                int outputWidth = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
                int outputHeight = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

                int score = 0;
                if (IsPrimaryOutput(outputDesc)) score += 10000;
                if (outputWidth == width && outputHeight == height) score += 1000;
                if (adapterDesc.VendorId == 0x10de) score += 100;

                if (score > bestScore) {
                    bestScore = score;
                    SafeRelease(selected_adapter);
                    adapter->AddRef();
                    selected_adapter = adapter;
                    selected_output_index = outputIndex;
                    selected_adapter_desc = adapterDesc;
                    selected_output_desc = outputDesc;
                }
            }

            SafeRelease(adapter);
        }

        SafeRelease(factory);

        if (!selected_adapter) {
            std::cerr << "未找到可用于桌面复制的显示输出" << std::endl;
            return false;
        }

        return true;
    }

    // 初始化 D3D11 设备，启用视频和 BGRA 支持
    bool InitD3D11() {
        if (!SelectCaptureOutput()) {
            return false;
        }

        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(
            selected_adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            1,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context);

        if (FAILED(hr)) {
            std::cerr << "D3D11CreateDevice 失败: " << std::hex << hr << std::endl;
            return false;
        }

        std::wcout << L"[GPU] D3D11 Device Adapter: " << selected_adapter_desc.Description
            << L", VendorId=0x" << std::hex << selected_adapter_desc.VendorId << std::dec << std::endl;

        if (selected_adapter_desc.VendorId != 0x10de) {
            std::wcout << L"[Warn] 当前截图输出不在 NVIDIA 适配器上，h264_nvenc 可能无法打开。" << std::endl;
        }
        return true;
    }

    // 初始化 DXGI 桌面复制 API
    bool InitDXGI() {
        if (!selected_adapter) {
            std::cerr << "未选择 DXGI 适配器" << std::endl;
            return false;
        }

        IDXGIOutput* output = nullptr;
        IDXGIOutput1* output1 = nullptr;

        HRESULT hr = selected_adapter->EnumOutputs(selected_output_index, &output);
        if (FAILED(hr) || !output) {
            std::cerr << "EnumOutputs 失败: " << std::hex << hr << std::endl;
            return false;
        }

        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        if (FAILED(hr) || !output1) {
            SafeRelease(output);
            std::cerr << "获取 IDXGIOutput1 失败: " << std::hex << hr << std::endl;
            return false;
        }

        std::wcout << L"[DXGI] 使用输出: " << selected_output_desc.DeviceName
            << L" [" << selected_output_desc.DesktopCoordinates.left << L","
            << selected_output_desc.DesktopCoordinates.top << L" - "
            << selected_output_desc.DesktopCoordinates.right << L","
            << selected_output_desc.DesktopCoordinates.bottom << L"]" << std::endl;

        // 创建桌面复制实例
        hr = output1->DuplicateOutput(device, &desk_dupl);
        SafeRelease(output1);
        SafeRelease(output);

        if (FAILED(hr)) {
            std::cerr << "DuplicateOutput 失败: " << std::hex << hr << std::endl;
        }
        return SUCCEEDED(hr);
    }

    // 初始化持久纹理，用于 GPU 内部缓存
    bool InitPersistentTexture() {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // DXGI 原始格式
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &persistent_tex);
        if (FAILED(hr)) {
            std::cerr << "创建持久纹理失败: " << std::hex << hr << std::endl;
        }
        return SUCCEEDED(hr);
    }

    // 释放资源
    void Cleanup() {
        SafeRelease(persistent_tex);
        SafeRelease(desk_dupl);
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(selected_adapter);
        selected_output_index = 0;
        selected_adapter_desc = {};
        selected_output_desc = {};
    }
};

/**
 * H264TextureEncoder 类：专门负责将截图纹理编码为 H.264。
 * 流程：BGRA 纹理 -> D3D11 VideoProcessor (转换成 NV12) -> FFmpeg NVENC (编码)
 *
 * 说明：
 * 1. 这个类不负责“抓屏”，只负责“把已有纹理编码成 H.264”；
 * 2. 编码阶段继续复用 AVFrame，避免每帧申请一次内存；
 * 3. 仍然保留最高 150 FPS 的编码时间基设置。
 */
class H264TextureEncoder {
public:
    static constexpr int kMaxFps = 150;      // 最高推流帧率限制
    static constexpr int kFramePoolSize = 6; // 复用的 AVFrame 数量，避免每帧申请/释放

    explicit H264TextureEncoder(const char* outputFilename)
        : output_filename(outputFilename ? outputFilename : "output.h264") {
    }

    ~H264TextureEncoder() { Cleanup(); }

    /**
     * 初始化编码器相关硬件资源
     * @param d3dDevice 截图模块创建好的 D3D11 设备
     * @param d3dContext 截图模块创建好的 D3D11 上下文
     * @param w 编码宽度
     * @param h 编码高度
     */
    bool Initialize(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext, int w, int h) {
        Cleanup();

        if (!d3dDevice || !d3dContext) {
            std::cerr << "编码器初始化失败: 传入的 D3D11 设备无效" << std::endl;
            return false;
        }

        width = w;
        height = h;
        device = d3dDevice;
        context = d3dContext;
        device->AddRef();
        context->AddRef();

        out_file.open(output_filename, std::ios::binary);
        if (!out_file) {
            std::cerr << "无法打开输出文件!" << std::endl;
            Cleanup();
            return false;
        }

        std::cout << "[Init] 配置 FFmpeg 硬件编码器..." << std::endl;
        if (!InitFFmpeg()) {
            Cleanup();
            return false;
        }

        std::cout << "[Init] 初始化 GPU 视频处理器 (用于格式转换)..." << std::endl;
        if (!InitVideoProcessor()) {
            Cleanup();
            return false;
        }

        // 初始化可复用的硬件帧池，避免每帧 av_frame_alloc/av_frame_free
        if (!InitFramePool()) {
            Cleanup();
            return false;
        }

        std::cout << "[Init] 编码模块就绪。" << std::endl;
        return true;
    }

    bool SetVisibleResolution(int requestedW, int requestedH) {
        requestedW = NormalizeEvenDimension(requestedW);
        requestedH = NormalizeEvenDimension(requestedH);

        int clampedW = (std::min)(requestedW, width);
        int clampedH = (std::min)(requestedH, height);
        if (clampedW == visible_width && clampedH == visible_height) {
            return true;
        }

        visible_width = clampedW;
        visible_height = clampedH;
        std::cout << "[Encoder] visible area => " << visible_width << "x" << visible_height
            << " (stream: " << width << "x" << height << ")" << std::endl;
        return true;
    }

    /**
     * 将一张 BGRA 屏幕纹理编码成 H.264
     * @param capturedTex 截图模块输出的桌面纹理
     * @param encoded 是否完成了一次编码动作
     */
    bool EncodeTexture(ID3D11Texture2D* capturedTex, bool& encoded) {
        encoded = false;

        if (!capturedTex) {
            std::cerr << "EncodeTexture 失败: 输入纹理为空" << std::endl;
            return false;
        }

        AVFrame* frame = AcquireReusableFrame();
        if (!frame) {
            std::cerr << "没有可用的复用帧" << std::endl;
            return false;
        }

        // 复用 AVFrame 对象，仅更新动态字段
        frame->pts = next_pts++;
        ID3D11Texture2D* hwTex = (ID3D11Texture2D*)frame->data[0];
        if (!hwTex) {
            std::cerr << "复用帧中的硬件纹理为空" << std::endl;
            return false;
        }

        // 使用 GPU 视频处理器执行 BGRA 到 NV12 的颜色转换
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = {};
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inDesc.Texture2D.MipSlice = 0;
        inDesc.Texture2D.ArraySlice = 0;

        ID3D11VideoProcessorInputView* inView = nullptr;
        HRESULT hr = video_device->CreateVideoProcessorInputView(capturedTex, video_enum, &inDesc, &inView);
        if (FAILED(hr)) {
            std::cerr << "CreateVideoProcessorInputView 失败: " << std::hex << hr << std::endl;
            return false;
        }

        // D3D11 硬件帧通常来自纹理数组，需要取出当前帧所在的数组切片
        UINT arraySlice = (UINT)(uintptr_t)frame->data[1];

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc = {};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        outDesc.Texture2DArray.MipSlice = 0;
        outDesc.Texture2DArray.FirstArraySlice = arraySlice;
        outDesc.Texture2DArray.ArraySize = 1;

        ID3D11VideoProcessorOutputView* outView = nullptr;
        hr = video_device->CreateVideoProcessorOutputView(hwTex, video_enum, &outDesc, &outView);
        if (FAILED(hr)) {
            SafeRelease(inView);
            std::cerr << "CreateVideoProcessorOutputView 失败: " << std::hex << hr << std::endl;
            return false;
        }

        // GPU 执行转换指令
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE stream_cs = {};
        stream_cs.Usage = 0;
        stream_cs.RGB_Range = 1; // full range RGB input (desktop BGRA)
        stream_cs.YCbCr_Matrix = 1; // BT.709
        stream_cs.YCbCr_xvYCC = 0;
        stream_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        video_context->VideoProcessorSetStreamColorSpace(video_processor, 0, &stream_cs);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_cs = {};
        output_cs.Usage = 0;
        output_cs.RGB_Range = 0;
        output_cs.YCbCr_Matrix = 1; // BT.709
        output_cs.YCbCr_xvYCC = 0;
        output_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        video_context->VideoProcessorSetOutputColorSpace(video_processor, &output_cs);

        // 输出码流分辨率固定，变化的是可视区域（居中显示）
        D3D11_VIDEO_COLOR bgColor = {};
        bgColor.YCbCr = { 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1.0f };
        video_context->VideoProcessorSetOutputBackgroundColor(video_processor, FALSE, &bgColor);

        D3D11_TEXTURE2D_DESC srcDesc = {};
        capturedTex->GetDesc(&srcDesc);
        RECT srcRect = { 0, 0, (LONG)srcDesc.Width, (LONG)srcDesc.Height };
        video_context->VideoProcessorSetStreamSourceRect(video_processor, 0, TRUE, &srcRect);

        int dstW = (std::min)(visible_width, width);
        int dstH = (std::min)(visible_height, height);
        LONG dstLeft = 0;
        LONG dstTop = 0;
        RECT dstRect = { dstLeft, dstTop, dstLeft + dstW, dstTop + dstH };
        video_context->VideoProcessorSetOutputTargetRect(video_processor, TRUE, &dstRect);
        video_context->VideoProcessorSetStreamDestRect(video_processor, 0, TRUE, &dstRect);

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = inView;
        hr = video_context->VideoProcessorBlt(video_processor, outView, 0, 1, &stream);
        SafeRelease(inView);
        SafeRelease(outView);
        if (FAILED(hr)) {
            std::cerr << "VideoProcessorBlt 失败: " << std::hex << hr << std::endl;
            return false;
        }

        // 将转换后的帧送入硬件编码器
        hr = avcodec_send_frame(codec_ctx, frame);
        if (hr < 0) {
            std::cerr << "avcodec_send_frame 失败: " << hr << std::endl;
            return false;
        }

        // 接收编码后的 H.264 数据包
        while ((hr = avcodec_receive_packet(codec_ctx, packet)) >= 0) {
            out_file.write((char*)packet->data, packet->size); // 写入磁盘 (最终数据)
            av_packet_unref(packet);
            encoded = true;
        }

        // EAGAIN 表示当前没有更多包可取，不视为错误
        if (hr != AVERROR(EAGAIN) && hr != AVERROR_EOF) {
            std::cerr << "avcodec_receive_packet 失败: " << hr << std::endl;
            return false;
        }

        // 即使当前还没吐包，只要成功送帧，也认为本次已发生编码动作
        if (!encoded) {
            encoded = true;
        }
        return true;
    }

    // 刷新编码器缓冲区
    void Flush() {
        if (!codec_ctx || !packet) return;

        avcodec_send_frame(codec_ctx, nullptr);
        while (avcodec_receive_packet(codec_ctx, packet) >= 0) {
            out_file.write((char*)packet->data, packet->size);
            av_packet_unref(packet);
        }
    }

    const char* GetCodecName() const {
        return (codec_ctx && codec_ctx->codec) ? codec_ctx->codec->name : "未知";
    }

    int GetVisibleWidth() const { return visible_width; }
    int GetVisibleHeight() const { return visible_height; }

private:
    int width = 0;
    int height = 0;
    int visible_width = 0;
    int visible_height = 0;
    std::string output_filename;

    // 复用截图模块提供的 D3D11 设备
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    // GPU 视频处理器 (用于在 VRAM 中转换 BGRA 到 NV12)
    ID3D11VideoDevice* video_device = nullptr;
    ID3D11VideoContext* video_context = nullptr;
    ID3D11VideoProcessor* video_processor = nullptr;
    ID3D11VideoProcessorEnumerator* video_enum = nullptr;

    // FFmpeg 硬件编码资源
    AVCodecContext* codec_ctx = nullptr;
    AVBufferRef* hw_device_ctx = nullptr;
    AVPacket* packet = nullptr;

    // 可复用的 AVFrame 池：避免每帧重复申请/释放
    std::vector<AVFrame*> frame_pool;
    size_t frame_pool_index = 0;
    int64_t next_pts = 0;

    // 输出 H.264 文件
    std::ofstream out_file;

    // 初始化 D3D11 视频处理器，用于极速 GPU 颜色转换
    bool InitVideoProcessor() {
        HRESULT hr = device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&video_device);
        if (FAILED(hr)) return false;

        hr = context->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&video_context);
        if (FAILED(hr)) return false;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
            {kMaxFps, 1},
            (UINT)width,
            (UINT)height,
            {kMaxFps, 1},
            (UINT)width,
            (UINT)height
        };

        if (FAILED(video_device->CreateVideoProcessorEnumerator(&desc, &video_enum))) {
            return false;
        }

        hr = video_device->CreateVideoProcessor(video_enum, 0, &video_processor);
        if (FAILED(hr)) {
            std::cerr << "创建视频处理器失败: " << std::hex << hr << std::endl;
        }
        return SUCCEEDED(hr);
    }

    // 初始化 FFmpeg 硬件编码环境
    bool InitFFmpeg() {
        // 优先查找 NVENC (NVIDIA), 备选软件 H.264
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cerr << "未找到 H.264 编码器!" << std::endl;
            return false;
        }

        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cerr << "avcodec_alloc_context3 失败" << std::endl;
            return false;
        }

        codec_ctx->width = width;
        codec_ctx->height = height;
        codec_ctx->time_base = { 1, kMaxFps };
        codec_ctx->framerate = { kMaxFps, 1 }; // 限制最高编码帧率为 150 FPS
        codec_ctx->pix_fmt = AV_PIX_FMT_D3D11;
        codec_ctx->color_primaries = AVCOL_PRI_BT709;
        codec_ctx->color_trc = AVCOL_TRC_BT709;
        codec_ctx->colorspace = AVCOL_SPC_BT709;
        codec_ctx->color_range = AVCOL_RANGE_MPEG;
        codec_ctx->bit_rate = 30000000; // 设置比特率
        codec_ctx->max_b_frames = 0;    // 低延迟场景下关闭 B 帧

        // 创建硬件设备上下文并绑定我们的 D3D11 设备
        hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hw_device_ctx) {
            std::cerr << "av_hwdevice_ctx_alloc 失败" << std::endl;
            return false;
        }

        ((AVD3D11VADeviceContext*)((AVHWDeviceContext*)hw_device_ctx->data)->hwctx)->device = device;
        device->AddRef();
        if (av_hwdevice_ctx_init(hw_device_ctx) < 0) {
            std::cerr << "硬件设备上下文初始化失败" << std::endl;
            return false;
        }
        codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

        // 分配硬件帧上下文 (NV12 格式：H.264 最优格式)
        AVBufferRef* frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
        if (!frames_ctx) {
            std::cerr << "av_hwframe_ctx_alloc 失败" << std::endl;
            return false;
        }

        AVHWFramesContext* f_ctx = (AVHWFramesContext*)frames_ctx->data;
        f_ctx->format = AV_PIX_FMT_D3D11;
        f_ctx->sw_format = AV_PIX_FMT_NV12;
        f_ctx->width = width;
        f_ctx->height = height;
        f_ctx->initial_pool_size = kFramePoolSize;
        ((AVD3D11VAFramesContext*)f_ctx->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;

        if (av_hwframe_ctx_init(frames_ctx) < 0) {
            std::cerr << "硬件帧上下文初始化失败" << std::endl;
            av_buffer_unref(&frames_ctx);
            return false;
        }
        codec_ctx->hw_frames_ctx = av_buffer_ref(frames_ctx);
        av_buffer_unref(&frames_ctx);

        // 编码器参数：极速预设，零延迟
        av_opt_set(codec_ctx->priv_data, "preset", "p1", 0);
        av_opt_set(codec_ctx->priv_data, "tune", "ull", 0);
        av_opt_set(codec_ctx->priv_data, "zerolatency", "1", 0);

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cerr << "编码器打开失败" << std::endl;
            return false;
        }

        packet = av_packet_alloc();
        if (!packet) {
            std::cerr << "av_packet_alloc 失败" << std::endl;
            return false;
        }
        return true;
    }

    // 初始化可复用的硬件帧池
    bool InitFramePool() {
        frame_pool.reserve(kFramePoolSize);
        for (int i = 0; i < kFramePoolSize; ++i) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                std::cerr << "av_frame_alloc 失败" << std::endl;
                return false;
            }

            if (av_hwframe_get_buffer(codec_ctx->hw_frames_ctx, frame, 0) < 0) {
                std::cerr << "av_hwframe_get_buffer 失败" << std::endl;
                av_frame_free(&frame);
                return false;
            }

            frame_pool.push_back(frame);
        }
        return true;
    }

    // 取一个可复用的 AVFrame
    AVFrame* AcquireReusableFrame() {
        if (frame_pool.empty()) return nullptr;
        AVFrame* frame = frame_pool[frame_pool_index];
        frame_pool_index = (frame_pool_index + 1) % frame_pool.size();
        return frame;
    }

    // 释放资源
    void Cleanup() {
        for (AVFrame* frame : frame_pool) {
            if (frame) av_frame_free(&frame);
        }
        frame_pool.clear();
        frame_pool_index = 0;
        next_pts = 0;

        if (out_file.is_open()) {
            out_file.close();
        }

        if (packet) av_packet_free(&packet);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);

        SafeRelease(video_processor);
        SafeRelease(video_enum);
        SafeRelease(video_device);
        SafeRelease(video_context);
        SafeRelease(context);
        SafeRelease(device);
    }
};

/**
 * ScreenCapturePipeline 类：只负责“调度”截图模块和编码模块。
 *
 * 说明：
 * 1. DXGI 截图逻辑已经拆到 DxgiScreenCapturer；
 * 2. H.264 编码逻辑已经拆到 H264TextureEncoder；
 * 3. 这里不再直接管理 DXGI 或编码细节，只负责控制流程与统计性能。
 */
class ScreenCapturePipeline {
public:
    explicit ScreenCapturePipeline(const char* outputFilename)
        : encoder(outputFilename) {
    }

    /**
     * 初始化整个流程
     * @param w 捕获宽度
     * @param h 捕获高度
     */
    bool Initialize(int captureW, int captureH, int visibleW, int visibleH) {
        width = captureW;
        height = captureH;
        visible_width = NormalizeEvenDimension(visibleW > 0 ? visibleW : captureW);
        visible_height = NormalizeEvenDimension(visibleH > 0 ? visibleH : captureH);

        if (!capturer.Initialize(captureW, captureH)) return false;
        if (!encoder.Initialize(capturer.GetDevice(), capturer.GetContext(), captureW, captureH)) return false;
        encoder.SetVisibleResolution(visible_width, visible_height);

        std::cout << "[Init] 所有硬件模块就绪。" << std::endl;
        std::cout << "[Init] stream=" << width << "x" << height
            << ", visible=" << encoder.GetVisibleWidth() << "x" << encoder.GetVisibleHeight() << std::endl;
        std::cout << "[Hotkey] F6=1280x720, F7=1920x1080, F8=Screen" << std::endl;
        return true;
    }

    bool RequestVisibleResolution(int w, int h) {
        pending_visible_width = NormalizeEvenDimension(w);
        pending_visible_height = NormalizeEvenDimension(h);
        has_pending_visible_change = true;
        return true;
    }

    /**
     * 执行捕获和编码循环
     * @param totalFramesToCapture 目标编码总帧数
     */
    void Run(int totalFramesToCapture) {
        std::cout << "编码器: " << encoder.GetCodecName() << std::endl;
        std::cout << "最高 FPS 限制: " << H264TextureEncoder::kMaxFps << std::endl;

        int total_encoded = 0;
        auto last_report_time = high_resolution_clock::now();
        int frames_in_second = 0;
        std::vector<double> latencies_in_second;

        const auto frame_interval = microseconds(1000000 / H264TextureEncoder::kMaxFps);
        const auto static_frame_interval = milliseconds(400); // no-change frame keepalive: about 2~3 FPS
        auto next_frame_time = steady_clock::now();
        auto last_static_push_time = steady_clock::now() - static_frame_interval;
        ID3D11Texture2D* last_frame_tex = nullptr;

        while (total_encoded < totalFramesToCapture) {
            // 速率限制到最高 150 FPS，避免无意义空转
            next_frame_time += frame_interval;
            std::this_thread::sleep_until(next_frame_time);

            auto frame_start = high_resolution_clock::now();
            PollDynamicResolutionHotkeys();
            if (has_pending_visible_change) {
                if (!encoder.SetVisibleResolution(pending_visible_width, pending_visible_height)) {
                    std::cerr << "切换可视分辨率失败!" << std::endl;
                    break;
                }
                visible_width = encoder.GetVisibleWidth();
                visible_height = encoder.GetVisibleHeight();
                has_pending_visible_change = false;
            }

            bool hasNewFrame = false;
            ID3D11Texture2D* capturedTex = nullptr;
            if (!capturer.CaptureFrame(hasNewFrame, &capturedTex)) {
                std::cerr << "捕获过程中断!" << std::endl;
                break;
            }

            // 屏幕画面未变化：不继续推帧
            ID3D11Texture2D* encodeTex = nullptr;
            if (hasNewFrame) {
                SafeRelease(last_frame_tex);
                if (capturedTex) {
                    capturedTex->AddRef();
                    last_frame_tex = capturedTex;
                }
                encodeTex = capturedTex;
            }
            else {
                auto now = steady_clock::now();
                if (!last_frame_tex || (now - last_static_push_time) < static_frame_interval) {
                    continue;
                }
                last_frame_tex->AddRef();
                encodeTex = last_frame_tex;
                last_static_push_time = now;
            }

            if (!encodeTex) {
                SafeRelease(capturedTex);
                continue;
            }

            bool encoded = false;
            bool ok = encoder.EncodeTexture(encodeTex, encoded);
            SafeRelease(encodeTex);
            if (!ok) {
                std::cerr << "编码过程中断!" << std::endl;
                break;
            }

            if (!encoded) {
                continue;
            }

            auto frame_end = high_resolution_clock::now();
            // 计算从开始截图到编码完成的耗时
            double latency = duration_cast<microseconds>(frame_end - frame_start).count() / 1000.0;

            latencies_in_second.push_back(latency);
            frames_in_second++;
            total_encoded++;

            // 每秒输出一次性能指标
            auto now_time = high_resolution_clock::now();
            if (duration_cast<milliseconds>(now_time - last_report_time).count() >= 1000) {
                if (!latencies_in_second.empty()) {
                    double avg = std::accumulate(latencies_in_second.begin(), latencies_in_second.end(), 0.0) /
                        latencies_in_second.size();
                    double max_lat = *std::max_element(latencies_in_second.begin(), latencies_in_second.end());
                    double min_lat = *std::min_element(latencies_in_second.begin(), latencies_in_second.end());
                    printf("FPS: %4d | 延迟: 平均 %.2fms, 最大 %.2fms, 最小 %.2fms\n",
                        frames_in_second,
                        avg,
                        max_lat,
                        min_lat);
                }
                frames_in_second = 0;
                latencies_in_second.clear();
                last_report_time = now_time;
            }
        }

        SafeRelease(last_frame_tex);
        encoder.Flush(); // 刷新编码器剩余帧
    }

private:
    int width = 0;
    int height = 0;
    int visible_width = 0;
    int visible_height = 0;
    int pending_visible_width = 0;
    int pending_visible_height = 0;
    bool has_pending_visible_change = false;
    bool f6_last_down = false;
    bool f7_last_down = false;
    bool f8_last_down = false;

    DxgiScreenCapturer capturer;
    H264TextureEncoder encoder;

    void PollDynamicResolutionHotkeys() {
        bool f6_down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        bool f7_down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;

        if (f6_down && !f6_last_down) RequestVisibleResolution(1280, 720);
        if (f7_down && !f7_last_down) RequestVisibleResolution(1920, 1080);
        if (f8_down && !f8_last_down) RequestVisibleResolution(width, height);

        f6_last_down = f6_down;
        f7_last_down = f7_down;
        f8_last_down = f8_down;
    }
};

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");

    // 获取主屏幕分辨率
    int w = GetSystemMetrics(SM_CXSCREEN) & ~1; // H.264 宽度要求是偶数
    int h = GetSystemMetrics(SM_CYSCREEN) & ~1;
    int visibleW = w;
    int visibleH = h;
    if (argc >= 3) {
        visibleW = NormalizeEvenDimension(std::atoi(argv[1]));
        visibleH = NormalizeEvenDimension(std::atoi(argv[2]));
    }
    std::cout << "启动全 GPU 性能监控 (" << w << "x" << h << ")" << std::endl;
    std::cout << "H.264 stream resolution: " << w << "x" << h << std::endl;
    std::cout << "Visible area request: " << visibleW << "x" << visibleH << std::endl;

    ScreenCapturePipeline pipeline("output.h264");
    if (pipeline.Initialize(w, h, visibleW, visibleH)) {
        // 捕获 1000 帧来评估性能
        pipeline.Run(1000);
        std::cout << "测试完成。视频流已保存至 output.h264" << std::endl;
    }
    else {
        std::cerr << "初始化失败!" << std::endl;
    }
    return 0;
}
