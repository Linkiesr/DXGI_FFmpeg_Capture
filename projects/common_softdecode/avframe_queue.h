#pragma once

extern "C" {
#include <libavutil/frame.h>
}

#include <mutex>
#include <queue>

// 线程安全 AVFrame 队列：解码线程 push，渲染线程 popLatest。
class AVFrameQueue {
public:
    static constexpr size_t kMaxFrames = 2;

    ~AVFrameQueue() {
        clear();
    }

    void push(AVFrame* frame) {
        if (!frame) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // 固定队列容量，优先保留最新帧：满了就淘汰最旧帧。
        while (queue_.size() >= kMaxFrames) {
            AVFrame* stale = queue_.front();
            queue_.pop();
            av_frame_free(&stale);
        }
        queue_.push(frame);
    }

    // 取最新一帧并丢弃积压旧帧，优先低延迟显示。
    AVFrame* popLatest() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return nullptr;
        }

        while (queue_.size() > 1) {
            AVFrame* stale = queue_.front();
            queue_.pop();
            av_frame_free(&stale);
        }

        AVFrame* latest = queue_.front();
        queue_.pop();
        return latest;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            AVFrame* frame = queue_.front();
            queue_.pop();
            av_frame_free(&frame);
        }
    }

private:
    std::mutex mutex_;
    std::queue<AVFrame*> queue_;
};
