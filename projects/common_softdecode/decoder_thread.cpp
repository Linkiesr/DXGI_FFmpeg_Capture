#include "decoder_thread.h"

#include <atomic>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {
// 示例程序单一解码线程使用的全局停止标记。
std::atomic<bool> g_stopRequested{false};

QString ffErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromLocal8Bit(buf);
}

// 将 FFmpeg 帧平面拷贝到紧凑连续的向量内存中。
DecodedFramePtr copyYuv420(const AVFrame* src) {
    auto out = std::make_shared<DecodedFrame>();
    out->width = src->width;
    out->height = src->height;
    out->pts = src->best_effort_timestamp;

    const int ySize = src->width * src->height;
    const int cWidth = src->width / 2;
    const int cHeight = src->height / 2;
    const int cSize = cWidth * cHeight;

    out->y.resize(ySize);
    out->u.resize(cSize);
    out->v.resize(cSize);

    for (int row = 0; row < src->height; ++row) {
        memcpy(out->y.data() + row * src->width,
               src->data[0] + row * src->linesize[0],
               static_cast<size_t>(src->width));
    }

    for (int row = 0; row < cHeight; ++row) {
        memcpy(out->u.data() + row * cWidth,
               src->data[1] + row * src->linesize[1],
               static_cast<size_t>(cWidth));
        memcpy(out->v.data() + row * cWidth,
               src->data[2] + row * src->linesize[2],
               static_cast<size_t>(cWidth));
    }

    return out;
}
} // namespace

DecoderThread::DecoderThread(QObject* parent)
    : QThread(parent) {
    qRegisterMetaType<DecodedFramePtr>("DecodedFramePtr");
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

    AVFormatContext* fmt = nullptr;
    AVCodecContext* codecCtx = nullptr;
    const AVCodec* codec = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* convFrame = av_frame_alloc();
    int videoStream = -1;

    // 统一清理逻辑，覆盖所有提前返回分支。
    auto cleanup = [&]() {
        if (convFrame) {
            av_freep(&convFrame->data[0]);
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

    ret = av_image_alloc(convFrame->data, convFrame->linesize, outW, outH, AV_PIX_FMT_YUV420P, 32);
    if (ret < 0) {
        emit decodeError("av_image_alloc failed: " + ffErr(ret));
        cleanup();
        emit decodeFinished();
        return;
    }

    convFrame->width = outW;
    convFrame->height = outH;
    convFrame->format = AV_PIX_FMT_YUV420P;

    // 若已是 YUV420 则直接输出；仅在像素格式不一致时使用 swscale 转换。
    auto emitFrame = [&](AVFrame* src) {
        if (src->format == AV_PIX_FMT_YUV420P) {
            emit frameReady(copyYuv420(src));
            return;
        }

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

        sws_scale(
            sws,
            src->data,
            src->linesize,
            0,
            src->height,
            convFrame->data,
            convFrame->linesize);

        convFrame->best_effort_timestamp = src->best_effort_timestamp;
        emit frameReady(copyYuv420(convFrame));
    };

    // 主读取与解码循环。
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

            emitFrame(frame);
            av_frame_unref(frame);
        }
    }

    // 清空解码器缓冲中的延迟帧。
    avcodec_send_packet(codecCtx, nullptr);
    while (!g_stopRequested.load()) {
        ret = avcodec_receive_frame(codecCtx, frame);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            break;
        }
        if (ret < 0) {
            break;
        }
        emitFrame(frame);
        av_frame_unref(frame);
    }

    cleanup();
    emit decodeFinished();
}
