#include "DecodeThread.h"

#include <chrono>
#include <cstring>

#include <QDebug>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace {
QString ffErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromLocal8Bit(buf);
}

AVFrame* cloneYuv420(const AVFrame* src) {
    AVFrame* dst = av_frame_alloc();
    if (!dst) {
        return nullptr;
    }

    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = src->width;
    dst->height = src->height;
    dst->pts = src->best_effort_timestamp;

    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }

    if (av_frame_make_writable(dst) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }

    av_image_copy(
        dst->data,
        dst->linesize,
        (const uint8_t* const*)src->data,
        src->linesize,
        AV_PIX_FMT_YUV420P,
        src->width,
        src->height);

    return dst;
}
} // namespace

DecodeThread::DecodeThread(AVFrameQueue* frameQueue)
    : _render_queue(frameQueue) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        if (_onDecodeError) _onDecodeError("Codec not found.");
        return;
    }

    _parser = av_parser_init(codec->id);
    if (!_parser) {
        if (_onDecodeError) _onDecodeError("Parser not found.");
        return;
    }

    _ctx = avcodec_alloc_context3(codec);
    if (!_ctx) {
        if (_onDecodeError) _onDecodeError("Could not allocate codec context.");
        return;
    }

    if (avcodec_open2(_ctx, codec, nullptr) < 0) {
        if (_onDecodeError) _onDecodeError("Could not open codec.");
        return;
    }

    _frame = av_frame_alloc();
    _pkt = av_packet_alloc();
    if (!_frame || !_pkt) {
        if (_onDecodeError) _onDecodeError("Could not allocate frame/packet.");
    }
}

DecodeThread::~DecodeThread() {
    stop();
    wait();

    {
        std::lock_guard<std::mutex> lk(_mutex);
        while (!_data_que.empty()) {
            auto item = _data_que.front();
            _data_que.pop();
            if (item && item->second) {
                delete[] item->second;
            }
        }
    }

    if (_pkt) {
        av_packet_free(&_pkt);
    }
    if (_frame) {
        av_frame_free(&_frame);
    }
    if (_ctx) {
        avcodec_free_context(&_ctx);
    }
    if (_parser) {
        av_parser_close(_parser);
        _parser = nullptr;
    }
}

void DecodeThread::setInputPath(const QString& path) {
    _inputPath = path;
}

void DecodeThread::setFrameQueuedCallback(std::function<void()> cb) {
    _onFrameQueued = std::move(cb);
}

void DecodeThread::setFrameSizeCallback(std::function<void(int, int)> cb) {
    _onFrameSize = std::move(cb);
}

void DecodeThread::setDecodeErrorCallback(std::function<void(const QString&)> cb) {
    _onDecodeError = std::move(cb);
}

void DecodeThread::setDecodeFinishedCallback(std::function<void()> cb) {
    _onDecodeFinished = std::move(cb);
}

void DecodeThread::start() {
    if (_running.load()) {
        return;
    }

    _running.store(true);
    _decodeStats = TimingStats{};

    _worker_thread = std::thread(&DecodeThread::decodeFrame, this);
    _feed_thread = std::thread(&DecodeThread::feedPackets, this);
}

void DecodeThread::stop() {
    _running.store(false);
    _consume.notify_all();
}

void DecodeThread::wait() {
    if (_feed_thread.joinable()) {
        _feed_thread.join();
    }
    if (_worker_thread.joinable()) {
        _worker_thread.join();
    }
}

void DecodeThread::printDecodeTimingStats() const {
    qInfo().noquote() << _decodeStats.summary("DecodeFrameTime");
}

void DecodeThread::oneFrameToQue(int size, uint8_t* fdata)
{
    uint8_t* data = new uint8_t[size];
    memcpy(data, fdata, size);
    std::unique_lock<std::mutex> unique_lk(_mutex);
    _data_que.push(std::make_shared<std::pair<int, uint8_t*>>(size, data));
    //由0变为1则发送通知信号
    if (_data_que.size() == 1) {
        unique_lk.unlock();
        _consume.notify_one();
    }
}

void DecodeThread::feedPackets() {
    if (_inputPath.isEmpty()) {
        if (_onDecodeError) _onDecodeError("Input path is empty.");
        _running.store(false);
        _consume.notify_all();
        return;
    }

    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, _inputPath.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0) {
        if (_onDecodeError) _onDecodeError("avformat_open_input failed: " + ffErr(ret));
        _running.store(false);
        _consume.notify_all();
        return;
    }

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        if (_onDecodeError) _onDecodeError("avformat_find_stream_info failed: " + ffErr(ret));
        avformat_close_input(&fmt);
        _running.store(false);
        _consume.notify_all();
        return;
    }

    int videoStream = -1;
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = static_cast<int>(i);
            break;
        }
    }

    if (videoStream < 0) {
        if (_onDecodeError) _onDecodeError("No video stream found.");
        avformat_close_input(&fmt);
        _running.store(false);
        _consume.notify_all();
        return;
    }

    AVPacket* in = av_packet_alloc();
    while (_running.load() && av_read_frame(fmt, in) >= 0) {
        if (in->stream_index == videoStream && in->size > 0 && in->data) {
            oneFrameToQue(in->size, in->data);
        }
        av_packet_unref(in);
    }

    av_packet_free(&in);
    avformat_close_input(&fmt);

    _running.store(false);
    _consume.notify_all();
}

void DecodeThread::decodeFrame()
{
    SwsContext* swsCtx = nullptr;
    AVFrame* yuvFrame = av_frame_alloc();

    while (_running.load() || !_data_que.empty()) {
        {
            std::unique_lock<std::mutex> unique_lk(_mutex);
            //判断队列为空则用条件变量阻塞等待，并释放锁
            while (_data_que.empty()) {
                if (!_running.load()) {
                    break;
                }
                _consume.wait(unique_lk);
            }
            if (_data_que.empty()) {
                continue;
            }
            //如果没有停服，且说明队列中有数据
            _buff = _data_que.front();
            _data_que.pop();
        }
        //如果队列中有数据则进行解码
        if (_buff) {
            _pkt->data = _buff->second;
            _pkt->size = _buff->first;

            int ret = avcodec_send_packet(_ctx, _pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                if (_onDecodeError) _onDecodeError("avcodec_send_packet failed: " + ffErr(ret));
                delete[] _buff->second;
                _buff.reset();
                continue;
            }

            delete[] _buff->second;
            _buff.reset();

            while (ret >= 0) {
                ret = avcodec_receive_frame(_ctx, _frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                }
                if (ret < 0) {
                    if (_onDecodeError) _onDecodeError("avcodec_receive_frame failed: " + ffErr(ret));
                    break;
                }

                const auto t0 = std::chrono::steady_clock::now();

                AVFrame* src = _frame;
                if (_frame->format != AV_PIX_FMT_YUV420P) {
                    if (!swsCtx ||
                        yuvFrame->width != _frame->width ||
                        yuvFrame->height != _frame->height) {
                        sws_freeContext(swsCtx);
                        swsCtx = sws_getContext(
                            _frame->width,
                            _frame->height,
                            static_cast<AVPixelFormat>(_frame->format),
                            _frame->width,
                            _frame->height,
                            AV_PIX_FMT_YUV420P,
                            SWS_BILINEAR,
                            nullptr,
                            nullptr,
                            nullptr);

                        if (yuvFrame->data[0]) {
                            av_frame_unref(yuvFrame);
                        }

                        yuvFrame->format = AV_PIX_FMT_YUV420P;
                        yuvFrame->width = _frame->width;
                        yuvFrame->height = _frame->height;
                        if (av_frame_get_buffer(yuvFrame, 32) < 0) {
                            if (_onDecodeError) _onDecodeError("av_frame_get_buffer(yuvFrame) failed.");
                            av_frame_unref(_frame);
                            continue;
                        }
                    }

                    if (av_frame_make_writable(yuvFrame) < 0) {
                        if (_onDecodeError) _onDecodeError("av_frame_make_writable(yuvFrame) failed.");
                        av_frame_unref(_frame);
                        continue;
                    }

                    sws_scale(
                        swsCtx,
                        _frame->data,
                        _frame->linesize,
                        0,
                        _frame->height,
                        yuvFrame->data,
                        yuvFrame->linesize);
                    yuvFrame->best_effort_timestamp = _frame->best_effort_timestamp;
                    src = yuvFrame;
                }

                AVFrame* out = cloneYuv420(src);
                if (out && _render_queue) {
                    _render_queue->push(out);
                    if (_onFrameSize) {
                        _onFrameSize(out->width, out->height);
                    }
                    if (_onFrameQueued) {
                        _onFrameQueued();
                    }
                }

                const auto t1 = std::chrono::steady_clock::now();
                _decodeStats.addSample(std::chrono::duration<double, std::milli>(t1 - t0).count());

                av_frame_unref(_frame);
            }
        }
    }

    if (yuvFrame) {
        av_frame_free(&yuvFrame);
    }
    if (swsCtx) {
        sws_freeContext(swsCtx);
    }

    if (_onDecodeFinished) {
        _onDecodeFinished();
    }
}
