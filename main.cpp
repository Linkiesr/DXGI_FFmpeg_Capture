#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <numeric>
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

/**
 * ScreenCaptureEncoder 类：实现全 GPU 流程的屏幕捕获与 H.264 编码。
 * 流程：DXGI (捕获纹理) -> D3D11 VideoProcessor (颜色空间转换) -> FFmpeg NVENC (编码)
 */
class ScreenCaptureEncoder {
public:
    ScreenCaptureEncoder(const char* outputFilename) : output_filename(outputFilename) {}
    ~ScreenCaptureEncoder() { Cleanup(); }

    /**
     * 初始化所有硬件加速组件
     * @param w 捕获宽度
     * @param h 捕获高度
     */
    bool Initialize(int w, int h) {
        this->width = w; this->height = h;
        std::cout << "[Init] 初始化 D3D11 设备..." << std::endl;
        if (!InitD3D11()) return false;
        
        std::cout << "[Init] 设置 DXGI 桌面复制..." << std::endl;
        if (!InitDXGI()) return false;
        
        std::cout << "[Init] 配置 FFmpeg 硬件编码器..." << std::endl;
        if (!InitFFmpeg()) return false;
        
        std::cout << "[Init] 初始化 GPU 视频处理器 (用于格式转换)..." << std::endl;
        if (!InitVideoProcessor()) return false;
        
        // 初始化持久纹理，用于无上限推帧
        if (!InitPersistentTexture()) return false;
        
        std::cout << "[Init] 所有硬件模块就绪。" << std::endl;
        return true;
    }

    /**
     * 执行捕获和编码循环
     * @param totalFramesToCapture 目标捕获总帧数
     */
    void Run(int totalFramesToCapture) {
        std::ofstream outFile(output_filename, std::ios::binary);
        if (!outFile) { std::cerr << "无法打开输出文件!" << std::endl; return; }

        std::cout << "编码器: " << (codec_ctx && codec_ctx->codec ? codec_ctx->codec->name : "未知") << std::endl;

        int total_captured = 0;
        auto last_report_time = high_resolution_clock::now();
        int frames_in_second = 0;
        std::vector<double> latencies_in_second;

        while (total_captured < totalFramesToCapture) {
            auto frame_start = high_resolution_clock::now();

            // 执行单帧捕获与编码
            if (CaptureAndEncode(outFile)) {
                auto frame_end = high_resolution_clock::now();
                // 计算从开始截图到编码完成的耗时
                double latency = duration_cast<microseconds>(frame_end - frame_start).count() / 1000.0;
                
                latencies_in_second.push_back(latency);
                frames_in_second++;
                total_captured++;

                // 每秒输出一次性能指标
                auto now_time = high_resolution_clock::now();
                if (duration_cast<milliseconds>(now_time - last_report_time).count() >= 1000) {
                    if (!latencies_in_second.empty()) {
                        double avg = std::accumulate(latencies_in_second.begin(), latencies_in_second.end(), 0.0) / latencies_in_second.size();
                        double max_lat = *std::max_element(latencies_in_second.begin(), latencies_in_second.end());
                        double min_lat = *std::min_element(latencies_in_second.begin(), latencies_in_second.end());
                        printf("FPS: %4d | 延迟: 平均 %.2fms, 最大 %.2fms, 最小 %.2fms\n", 
                               frames_in_second, avg, max_lat, min_lat);
                    }
                    frames_in_second = 0;
                    latencies_in_second.clear();
                    last_report_time = now_time;
                }
            } else {
                std::cerr << "捕获过程中断!" << std::endl;
                break;
            }
        }
        Flush(outFile); // 刷新编码器剩余帧
        outFile.close();
    }

private:
    int width, height;
    const char* output_filename;

    // D3D11 & DXGI 资源
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutputDuplication* desk_dupl = nullptr;

    // GPU 视频处理器 (用于在 VRAM 中转换 BGRA 到 NV12)
    ID3D11VideoDevice* video_device = nullptr;
    ID3D11VideoContext* video_context = nullptr;
    ID3D11VideoProcessor* video_processor = nullptr;
    ID3D11VideoProcessorEnumerator* video_enum = nullptr;

    // 持久纹理：即使屏幕画面没变，也能持续推帧，实现无上限 FPS
    ID3D11Texture2D* persistent_tex = nullptr;

    // FFmpeg 硬件编码资源
    AVCodecContext* codec_ctx = nullptr;
    AVBufferRef* hw_device_ctx = nullptr;
    AVPacket* packet = nullptr;

    // 初始化 D3D11 设备，启用视频和 BGRA 支持
    bool InitD3D11() {
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr)) std::cerr << "D3D11CreateDevice 失败: " << std::hex << hr << std::endl;
        return SUCCEEDED(hr);
    }

    // 初始化 DXGI 桌面复制 API
    bool InitDXGI() {
        IDXGIDevice* dxgiDevice = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) return false;
        IDXGIAdapter* adapter = nullptr;
        dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&adapter);
        IDXGIOutput* output = nullptr;
        if (FAILED(adapter->EnumOutputs(0, &output))) return false;
        IDXGIOutput1* output1 = nullptr;
        if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) return false;
        
        // 创建桌面复制实例
        HRESULT hr = output1->DuplicateOutput(device, &desk_dupl);
        output1->Release(); output->Release(); adapter->Release(); dxgiDevice->Release();
        if (FAILED(hr)) std::cerr << "DuplicateOutput 失败: " << std::hex << hr << std::endl;
        return SUCCEEDED(hr);
    }

    // 初始化持久纹理，用于 GPU 内部缓存
    bool InitPersistentTexture() {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // DXGI 原始格式
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &persistent_tex);
        if (FAILED(hr)) std::cerr << "创建持久纹理失败: " << std::hex << hr << std::endl;
        return SUCCEEDED(hr);
    }

    // 初始化 D3D11 视频处理器，用于极速 GPU 颜色转换
    bool InitVideoProcessor() {
        if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&video_device))) return false;
        if (FAILED(context->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&video_context))) return false;
        
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = { 
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, {30, 1}, (UINT)width, (UINT)height, {30, 1}, (UINT)width, (UINT)height 
        };
        if (FAILED(video_device->CreateVideoProcessorEnumerator(&desc, &video_enum))) return false;
        HRESULT hr = video_device->CreateVideoProcessor(video_enum, 0, &video_processor);
        if (FAILED(hr)) std::cerr << "创建视频处理器失败: " << std::hex << hr << std::endl;
        return SUCCEEDED(hr);
    }

    // 初始化 FFmpeg 硬件编码环境
    bool InitFFmpeg() {
        // 优先查找 NVENC (NVIDIA), 备选 AMF (AMD), QSV (Intel)
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) { std::cerr << "未找到 H.264 硬件编码器!" << std::endl; return false; }

        codec_ctx = avcodec_alloc_context3(codec);
        codec_ctx->width = width; codec_ctx->height = height;
        codec_ctx->time_base = { 1, 1000 }; 
        codec_ctx->framerate = { 0, 1 };    // 解除帧率上限控制
        codec_ctx->pix_fmt = AV_PIX_FMT_D3D11;

        // 创建硬件设备上下文并绑定我们的 D3D11 设备
        hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        ((AVD3D11VADeviceContext*)((AVHWDeviceContext*)hw_device_ctx->data)->hwctx)->device = device;
        device->AddRef();
        av_hwdevice_ctx_init(hw_device_ctx);
        codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

        // 分配硬件帧上下文 (NV12 格式：H.264 最优格式)
        AVBufferRef* frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
        AVHWFramesContext* f_ctx = (AVHWFramesContext*)frames_ctx->data;
        f_ctx->format = AV_PIX_FMT_D3D11;
        f_ctx->sw_format = AV_PIX_FMT_NV12; 
        f_ctx->width = width; f_ctx->height = height;
        f_ctx->initial_pool_size = 5;
        ((AVD3D11VAFramesContext*)f_ctx->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER;

        if (av_hwframe_ctx_init(frames_ctx) < 0) { std::cerr << "硬件帧上下文初始化失败" << std::endl; return false; }
        codec_ctx->hw_frames_ctx = av_buffer_ref(frames_ctx);
        av_buffer_unref(&frames_ctx);

        // 编码器参数：极速预设，零延迟
        av_opt_set(codec_ctx->priv_data, "preset", "p1", 0);
        av_opt_set(codec_ctx->priv_data, "tune", "ull", 0);
        av_opt_set(codec_ctx->priv_data, "zerolatency", "1", 0);

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) { std::cerr << "编码器打开失败" << std::endl; return false; }
        packet = av_packet_alloc();
        return true;
    }

    /**
     * 核心捕获与编码逻辑
     * 捕获 DXGI 纹理 -> GPU 转换 -> 推送至编码器
     */
    bool CaptureAndEncode(std::ofstream& outFile) {
        IDXGIResource* desktopRes = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        
        // 非阻塞捕获：Timeout 0
        HRESULT hr = desk_dupl->AcquireNextFrame(0, &frameInfo, &desktopRes);
        if (SUCCEEDED(hr)) {
            ID3D11Texture2D* capturedTex = nullptr;
            desktopRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&capturedTex);
            // 将截取的屏幕纹理复制到持久纹理 (GPU 内部复制)
            context->CopyResource(persistent_tex, capturedTex);
            capturedTex->Release();
            desktopRes->Release();
            desk_dupl->ReleaseFrame();
        }

        // 从 FFmpeg 池中获取一个硬件帧
        AVFrame* frame = av_frame_alloc();
        if (av_hwframe_get_buffer(codec_ctx->hw_frames_ctx, frame, 0) >= 0) {
            ID3D11Texture2D* hwTex = (ID3D11Texture2D*)frame->data[0];

            // 使用 GPU 视频处理器执行 BGRA 到 NV12 的颜色转换
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = { 0, D3D11_VPIV_DIMENSION_TEXTURE2D, {0, 0} };
            ID3D11VideoProcessorInputView* inView = nullptr;
            video_device->CreateVideoProcessorInputView(persistent_tex, video_enum, &inDesc, &inView);
            
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc = { D3D11_VPOV_DIMENSION_TEXTURE2D, {0} };
            ID3D11VideoProcessorOutputView* outView = nullptr;
            video_device->CreateVideoProcessorOutputView(hwTex, video_enum, &outDesc, &outView);

            if (inView && outView) {
                // GPU 执行转换指令
                D3D11_VIDEO_PROCESSOR_STREAM stream = { TRUE, 0, 0, 0, 0, NULL, inView, NULL };
                video_context->VideoProcessorBlt(video_processor, outView, 0, 1, &stream);
            }
            if (inView) inView->Release();
            if (outView) outView->Release();

            // 将转换后的帧送入硬件编码器
            if (avcodec_send_frame(codec_ctx, frame) >= 0) {
                // 接收编码后的 H.264 数据包
                while (avcodec_receive_packet(codec_ctx, packet) >= 0) {
                    outFile.write((char*)packet->data, packet->size); // 写入磁盘 (最终数据)
                    av_packet_unref(packet);
                }
            }
        }
        av_frame_free(&frame);
        return true;
    }

    // 刷新编码器缓冲区
    void Flush(std::ofstream& outFile) {
        avcodec_send_frame(codec_ctx, nullptr);
        while (avcodec_receive_packet(codec_ctx, packet) >= 0) {
            outFile.write((char*)packet->data, packet->size);
            av_packet_unref(packet);
        }
    }

    // 释放资源
    void Cleanup() {
        if (persistent_tex) persistent_tex->Release();
        if (video_processor) video_processor->Release();
        if (video_enum) video_enum->Release();
        if (video_device) video_device->Release();
        if (video_context) video_context->Release();
        if (desk_dupl) desk_dupl->Release();
        if (context) context->Release();
        if (device) device->Release();
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
        if (packet) av_packet_free(&packet);
    }
};

int main() {
    // 获取主屏幕分辨率
    int w = GetSystemMetrics(SM_CXSCREEN) & ~1; // H.264 宽度要求是偶数
    int h = GetSystemMetrics(SM_CYSCREEN) & ~1;
    std::cout << "启动全 GPU 性能监控 (" << w << "x" << h << ")" << std::endl;

    ScreenCaptureEncoder encoder("output.h264");
    if (encoder.Initialize(w, h)) {
        // 捕获 1000 帧来评估极限性能
        encoder.Run(1000);
        std::cout << "测试完成。视频流已保存至 output.h264" << std::endl;
    } else {
        std::cerr << "初始化失败!" << std::endl;
    }
    return 0;
}
