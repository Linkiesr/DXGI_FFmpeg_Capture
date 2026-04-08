#pragma once

#include "avframe_queue.h"
#include "timing_stats.h"

#include <QThread>
#include <QString>

// 解码线程：解复用 + 软解码，输出 AVFrame(YUV420P) 并入队。
class DecoderThread final : public QThread {
    Q_OBJECT
public:
    explicit DecoderThread(AVFrameQueue* frameQueue, QObject* parent = nullptr);
    ~DecoderThread() override;

    // 输入媒体路径（文件路径或 FFmpeg 支持的流 URL）。
    void setInputPath(const QString& path);

    // 协作式停止标记，由 run() 循环读取。
    void stop();

    // 打印“每解码一帧”耗时统计。
    void printDecodeTimingStats() const;

signals:
    // 每有新帧入队时发出，用于通知渲染线程刷新。
    void frameQueued();

    // 输出可读的 FFmpeg 错误信息。
    void decodeError(const QString& message);

    // 解码循环退出且资源清理完成后发出。
    void decodeFinished();

protected:
    void run() override;

private:
    QString inputPath_;
    AVFrameQueue* frameQueue_ = nullptr;
    TimingStats decodeStats_;
};
