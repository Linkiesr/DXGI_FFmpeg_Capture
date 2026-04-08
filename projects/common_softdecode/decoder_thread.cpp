#include "decoder_thread.h"

#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {
std::atomic<bool> g_stopRequested{false};

QString ffErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromLocal8Bit(buf);
}

// 分配一个可写的 YUV420P AVFrame。
AVFrame* allocYuv420Frame(int width, int height, int64_t pts) {
    AVFrame* dst = av_frame_alloc();
    if (!dst) {
        return nullptr;
    }

    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = width;
    dst->height = height;
    dst->pts = pts;

    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }
    if (av_frame_make_writable(dst) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }
    return dst;
}

// 将 YUV420 帧深拷贝，保证入队后生命周期独立。
AVFrame* cloneYuv420(const AVFrame* src) {
    AVFrame* dst = allocYuv420Frame(src->width, src->height, src->best_effort_timestamp);
    if (!dst) {
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

DecoderThread::DecoderThread(AVFrameQueue* frameQueue, QObject* parent)
    : QThread(parent), frameQueue_(frameQueue) {
}

DecoderThread::~DecoderThread() {
    stop();
    wait();
}

void DecoderThread::setInputPath(const QString& path) {
    inputPath_ = path;
}

void DecoderThread::stop() {
    g_stopRequested.store(true);
}

void DecoderThread::run() {
    g_stopRequested.store(false);

    if (inputPath_.isEmpty()) {
        emit decodeError("Input path is empty.");
        emit decodeFinished();
        return;
    }

    if (!frameQueue_) {
        emit decodeError("Frame queue is null.");
        emit decodeFinished();
        return;
    }

    AVFormatContext* fmt = nullptr;
    AVCodecContext* codecCtx = nullptr;
    const AVCodec* codec = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* convFrame = av_frame_alloc();
    int videoStream = -1;

    auto cleanup = [&]() {
        if (convFrame) {
            av_frame_free(&convFrame);
        }
        if (frame) {
            av_frame_free(&frame);
        }
        if (pkt) {
            av_packet_free(&pkt);
        }
        if (sws) {
            sws_freeContext(sws);
        }
        if (codecCtx) {
            avcodec_free_context(&codecCtx);
        }
        if (fmt) {
            avformat_close_input(&fmt);
        }
    };

    if (!pkt || !frame || !convFrame) {
        emit decodeError("Failed to allocate FFmpeg packet/frame.");
        cleanup();
        emit decodeFinished();
        return;
    }

    int ret = avformat_open_input(&fmt, inputPath_.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0) {
        emit decodeError("avformat_open_input failed: " + ffErr(ret));
        cleanup();
        emit decodeFinished();
        return;
    }

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        emit decodeError("avformat_find_stream_info failed: " + ffErr(ret));
        cleanup();
        emit decodeFinished();
        return;
    }

    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = static_cast<int>(i);
            break;
        }
    }

    if (videoStream < 0) {
        emit decodeError("No video stream found.");
        cleanup();
        emit decodeFinished();
        return;
    }

    codec = avcodec_find_decoder(fmt->streams[videoStream]->codecpar->codec_id);
    if (!codec) {
        emit decodeError("Decoder not found.");
        cleanup();
        emit decodeFinished();
        return;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        emit decodeError("avcodec_alloc_context3 failed.");
        cleanup();
        emit decodeFinished();
        return;
    }

    ret = avcodec_parameters_to_context(codecCtx, fmt->streams[videoStream]->codecpar);
    if (ret < 0) {
        emit decodeError("avcodec_parameters_to_context failed: " + ffErr(ret));
        cleanup();
        emit decodeFinished();
        return;
    }

    codecCtx->thread_count = 0;
    codecCtx->thread_type = FF_THREAD_FRAME;

    ret = avcodec_open2(codecCtx, codec, nullptr);
    if (ret < 0) {
        emit decodeError("avcodec_open2 failed: " + ffErr(ret));
        cleanup();
        emit decodeFinished();
        return;
    }

    const int outW = codecCtx->width;
    const int outH = codecCtx->height;
    convFrame->format = AV_PIX_FMT_YUV420P;
    convFrame->width = outW;
    convFrame->height = outH;
    if (av_frame_get_buffer(convFrame, 32) < 0) {
        emit decodeError("av_frame_get_buffer(convFrame) failed.");
        cleanup();
        emit decodeFinished();
        return;
    }

    auto enqueueFrame = [&](AVFrame* src) {
        AVFrame* yuvSrc = src;

        if (src->format != AV_PIX_FMT_YUV420P) {
            sws = sws_getCachedContext(
                sws,
                src->width,
                src->height,
                static_cast<AVPixelFormat>(src->format),
                outW,
                outH,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr);

            if (!sws) {
                emit decodeError("sws_getCachedContext failed.");
                return;
            }

            if (av_frame_make_writable(convFrame) < 0) {
                emit decodeError("av_frame_make_writable(convFrame) failed.");
                return;
            }

            sws_scale(
                sws,
                src->data,
                src->linesize,
                0,
                src->height,
                convFrame->data,
                convFrame->linesize);
            convFrame->best_effort_timestamp = src->best_effort_timestamp;
            yuvSrc = convFrame;
        }

        AVFrame* queued = cloneYuv420(yuvSrc);
        if (!queued) {
            emit decodeError("cloneYuv420 failed.");
            return;
        }

        frameQueue_->push(queued);
        emit frameQueued();
    };

    while (!g_stopRequested.load() && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != videoStream) {
            av_packet_unref(pkt);
            continue;
        }

        ret = avcodec_send_packet(codecCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            emit decodeError("avcodec_send_packet failed: " + ffErr(ret));
            break;
        }

        while (!g_stopRequested.load()) {
            ret = avcodec_receive_frame(codecCtx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                emit decodeError("avcodec_receive_frame failed: " + ffErr(ret));
                g_stopRequested.store(true);
                break;
            }

            enqueueFrame(frame);
            av_frame_unref(frame);
        }
    }

    avcodec_send_packet(codecCtx, nullptr);
    while (!g_stopRequested.load()) {
        ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            break;
        }
        if (ret < 0) {
            break;
        }
        enqueueFrame(frame);
        av_frame_unref(frame);
    }

    cleanup();
    emit decodeFinished();
}
