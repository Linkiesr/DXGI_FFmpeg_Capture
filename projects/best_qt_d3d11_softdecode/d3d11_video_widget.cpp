#include "d3d11_video_widget.h"

#include <QMetaObject>
#include <QResizeEvent>
#include <QShowEvent>
#include <cstring>

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace {
// 全屏三角形着色器：采样 Y/U/V 并在像素着色器中转换为 RGB。
constexpr const char* kVsCode = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 pos[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    float2 uv[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };
    o.pos = float4(pos[id], 0.0, 1.0);
    o.uv = uv[id];
    return o;
}
)";

constexpr const char* kPsCode = R"(
Texture2D texY : register(t0);
Texture2D texU : register(t1);
Texture2D texV : register(t2);
SamplerState samp0 : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float y = texY.Sample(samp0, uv).r;
    float u = texU.Sample(samp0, uv).r - 0.5;
    float v = texV.Sample(samp0, uv).r - 0.5;

    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;

    return float4(saturate(r), saturate(g), saturate(b), 1.0);
}
)";
} // namespace

D3D11VideoWidget::D3D11VideoWidget(QWidget* parent)
    : QWidget(parent) {
    // 使用原生子窗口句柄，供 DXGI 交换链绑定。
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen, false);
    setAutoFillBackground(false);
}

D3D11VideoWidget::~D3D11VideoWidget() = default;

void D3D11VideoWidget::onFrameReady(const DecodedFramePtr& frame) {
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        latestFrame_ = frame;
    }
    // 在 UI 线程中安排重绘。
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void D3D11VideoWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    ensureDevice();
    ensureSwapChain();
    ensurePipeline();
}

void D3D11VideoWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (swapChain_ && width() > 0 && height() > 0) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        rtv_.Reset();
        swapChain_->ResizeBuffers(0, width(), height(), DXGI_FORMAT_UNKNOWN, 0);
        ensureSwapChain();
    }
}

void D3D11VideoWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    renderFrame();
}

void D3D11VideoWidget::ensureDevice() {
    if (device_) {
        return;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL level;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &device_,
        &level,
        &context_);
}

void D3D11VideoWidget::ensureSwapChain() {
    if (!device_ || width() <= 0 || height() <= 0) {
        return;
    }

    if (!swapChain_) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;

        device_.As(&dxgiDevice);
        dxgiDevice->GetAdapter(&adapter);
        adapter->GetParent(__uuidof(IDXGIFactory), &factory);

        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferCount = 2;
        desc.BufferDesc.Width = static_cast<UINT>(width());
        desc.BufferDesc.Height = static_cast<UINT>(height());
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = reinterpret_cast<HWND>(winId());
        desc.SampleDesc.Count = 1;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        factory->CreateSwapChain(device_.Get(), &desc, &swapChain_);
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_);
}

void D3D11VideoWidget::ensurePipeline() {
    if (!device_ || (vs_ && ps_ && sampler_)) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;

    D3DCompile(kVsCode, strlen(kVsCode), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    D3DCompile(kPsCode, strlen(kPsCode), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &psBlob, &errBlob);

    device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sampDesc, &sampler_);
}

void D3D11VideoWidget::ensureTextures(int width, int height) {
    if (!device_) {
        return;
    }

    if (width == texWidth_ && height == texHeight_ && yTex_ && uTex_ && vTex_) {
        return;
    }

    texWidth_ = width;
    texHeight_ = height;

    yTex_.Reset();
    uTex_.Reset();
    vTex_.Reset();
    ySrv_.Reset();
    uSrv_.Reset();
    vSrv_.Reset();

    auto createPlane = [&](int w, int h, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.ArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device_->CreateTexture2D(&desc, nullptr, tex);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(*tex, &srvDesc, srv);
    };

    createPlane(width, height, &yTex_, &ySrv_);
    createPlane(width / 2, height / 2, &uTex_, &uSrv_);
    createPlane(width / 2, height / 2, &vTex_, &vSrv_);
}

void D3D11VideoWidget::uploadFrame(const DecodedFrame& frame) {
    ensureTextures(frame.width, frame.height);

    // 逐平面上传纹理数据，并处理行步长差异。
    auto updatePlane = [&](ID3D11Texture2D* tex, const uint8_t* src, int srcStride, int w, int h) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        context_->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        for (int row = 0; row < h; ++row) {
            memcpy(dst + row * mapped.RowPitch, src + row * srcStride, static_cast<size_t>(w));
        }
        context_->Unmap(tex, 0);
    };

    updatePlane(yTex_.Get(), frame.y.data(), frame.width, frame.width, frame.height);
    updatePlane(uTex_.Get(), frame.u.data(), frame.width / 2, frame.width / 2, frame.height / 2);
    updatePlane(vTex_.Get(), frame.v.data(), frame.width / 2, frame.width / 2, frame.height / 2);
}

void D3D11VideoWidget::renderFrame() {
    ensureDevice();
    ensureSwapChain();
    ensurePipeline();

    if (!context_ || !swapChain_ || !rtv_ || !vs_ || !ps_) {
        return;
    }

    DecodedFramePtr frame;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frame = latestFrame_;
    }

    if (frame) {
        uploadFrame(*frame);
    }

    if (!ySrv_ || !uSrv_ || !vSrv_) {
        return;
    }

    const float clearColor[4] = {0.03f, 0.03f, 0.03f, 1.0f};
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width());
    vp.Height = static_cast<float>(height());
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* srvs[] = {ySrv_.Get(), uSrv_.Get(), vSrv_.Get()};
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 3, srvs);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->Draw(3, 0);

    // 解绑 SRV，避免纹理在其他阶段复用时产生警告。
    ID3D11ShaderResourceView* nullSrvs[] = {nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 3, nullSrvs);

    swapChain_->Present(0, 0);
}
