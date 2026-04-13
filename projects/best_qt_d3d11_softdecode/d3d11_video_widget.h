#pragma once

#include "avframe_queue.h"
#include "timing_stats.h"

#include <QPaintEngine>
#include <QWidget>

#include <d3d11.h>
#include <wrl/client.h>

class D3D11VideoWidget final : public QWidget {
    Q_OBJECT
public:
    explicit D3D11VideoWidget(QWidget* parent = nullptr);
    ~D3D11VideoWidget() override;

    void setFrameQueue(AVFrameQueue* frameQueue);

    // 打印“从队列取帧到渲染完成”耗时统计。
    void printRenderTimingStats() const;

public slots:
    void onFrameQueued();

protected:
    QPaintEngine* paintEngine() const override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void ensureDevice();
    void ensureSwapChain();
    void ensurePipeline();
    void ensureTextures(int width, int height);
    void uploadFrame(const AVFrame* frame);
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
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> yTex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> uTex_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> vTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ySrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uSrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> vSrv_;

    int texWidth_ = 0;
    int texHeight_ = 0;

    AVFrameQueue* frameQueue_ = nullptr;
    AVFrame* lastFrame_ = nullptr;
    TimingStats renderStats_;
    bool hasPresentedFrame_ = false;
};
