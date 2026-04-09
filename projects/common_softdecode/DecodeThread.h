#pragma once

#include "avframe_queue.h"
#include "timing_stats.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include <QString>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

// 纯 C++ 解码类：不继承 QObject，不使用 Qt 信号槽。
// - 输入端 oneFrameToQue + _data_que 消费逻辑保持不变。
// - 输出端改为 AVFrameQueue，供渲染线程直接取 AVFrame。
class DecodeThread {
public:
    explicit DecodeThread(AVFrameQueue* frameQueue);
    ~DecodeThread();

    void setInputPath(const QString& path);

    // 启动/停止：内部包含“输入喂包线程 + 解码线程”。
    void start();
    void stop();
    void wait();

    // 输入端：保持你提供的入队方式。
    void oneFrameToQue(int size, uint8_t* fdata);

    // 事件回调（从工作线程触发）。
    void setFrameQueuedCallback(std::function<void()> cb);
    void setFrameSizeCallback(std::function<void(int, int)> cb);
    void setDecodeErrorCallback(std::function<void(const QString&)> cb);
    void setDecodeFinishedCallback(std::function<void()> cb);

    // 打印每帧解码耗时统计。
    void printDecodeTimingStats() const;

private:
    void decodeFrame();
    void feedPackets();

    AVCodecContext* _ctx = nullptr;
    AVCodecParserContext* _parser = nullptr;
    AVFrame* _frame = nullptr;
    AVPacket* _pkt = nullptr;

    std::thread _worker_thread;
    std::thread _feed_thread;
    std::atomic<bool> _running{false};

    std::mutex _mutex;
    std::condition_variable _consume;
    std::queue<std::shared_ptr<std::pair<int, uint8_t*>>> _data_que;
    std::shared_ptr<std::pair<int, uint8_t*>> _buff;

    AVFrameQueue* _render_queue = nullptr;
    QString _inputPath;

    TimingStats _decodeStats;

    std::function<void()> _onFrameQueued;
    std::function<void(int, int)> _onFrameSize;
    std::function<void(const QString&)> _onDecodeError;
    std::function<void()> _onDecodeFinished;
};
