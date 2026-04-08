#pragma once

#include "decoded_frame.h"

#include <QWidget>

#include <d3d11.h>
#include <wrl/client.h>

#include <mutex>

// 嵌入 Qt 控件中的原生 D3D11 渲染器。
class D3D11VideoWidget final : public QWidget {
    Q_OBJECT
public:
    explicit D3D11VideoWidget(QWidget* parent = nullptr);
    ~D3D11VideoWidget() override;

public slots:
    // 接收解码线程送来的最新帧。
    void onFrameReady(const DecodedFramePtr& frame);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void ensureDevice();
    void ensureSwapChain();
    void ensurePipeline();
    void ensureTextures(int width, int height);
    void uploadFrame(const DecodedFrame& frame);
    void renderFrame();

    using ComPtrDevice = Microsoft::WRL::ComPtr<ID3D11Device>;
    using ComPtrCtx = Microsoft::WRL::ComPtr<ID3D11DeviceContext>;
    using ComPtrSwapChain = Microsoft::WRL::ComPtr<IDXGISwapChain>;

    ComPtrDevice device_;
    ComPtrCtx context_;
    ComPtrSwapChain swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

    // Y/U/V 平面对应的动态 R8 纹理。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> yTex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> uTex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> vTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ySrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uSrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> vSrv_;

    int texWidth_ = 0;
    int texHeight_ = 0;

    // 解码线程与 UI/渲染线程共享的“最新帧”缓存。
    std::mutex frameMutex_;
    DecodedFramePtr latestFrame_;
};

