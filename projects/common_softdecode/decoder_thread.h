#pragma once

#include "decoded_frame.h"

#include <QThread>
#include <QString>

// 解码线程：解复用 + 软解码 + 统一输出为 YUV420。
class DecoderThread final : public QThread {
    Q_OBJECT
public:
    explicit DecoderThread(QObject* parent = nullptr);
    ~DecoderThread() override;

    // 输入媒体路径（文件路径或 FFmpeg 支持的流 URL）。
    void setInputPath(const QString& path);

    // 协作式停止标记，由 run() 循环读取。
    void stop();

signals:
    // 每解出一帧即发出该信号。
    void frameReady(const DecodedFramePtr& frame);

    // 输出可读的 FFmpeg 错误信息。
    void decodeError(const QString& message);

    // 解码循环退出且资源清理完成后发出。
    void decodeFinished();

protected:
    void run() override;

private:
    QString inputPath_;
};

Q_DECLARE_METATYPE(DecodedFramePtr)
