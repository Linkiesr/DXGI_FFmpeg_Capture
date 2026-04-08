#include "gl_video_widget.h"

#include <QMetaObject>
#include <QDebug>

#include <chrono>

GLVideoWidget::GLVideoWidget(QWidget* parent)
    : QOpenGLWidget(parent) {
}

GLVideoWidget::~GLVideoWidget() {
    makeCurrent();
    if (texY_) glDeleteTextures(1, &texY_);
    if (texU_) glDeleteTextures(1, &texU_);
    if (texV_) glDeleteTextures(1, &texV_);
    doneCurrent();

    if (lastFrame_) {
        av_frame_free(&lastFrame_);
    }
}

void GLVideoWidget::setFrameQueue(AVFrameQueue* frameQueue) {
    frameQueue_ = frameQueue;
}

void GLVideoWidget::printRenderTimingStats() const {
    qInfo().noquote() << renderStats_.summary("QueueToRenderTime(OpenGL)");
}

void GLVideoWidget::onFrameQueued() {
    QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
}

void GLVideoWidget::initializeGL() {
    initializeOpenGLFunctions();

    const char* vs = R"(
        #version 330 core
        out vec2 uv;
        const vec2 pos[3] = vec2[3](
            vec2(-1.0, -1.0),
            vec2(-1.0, 3.0),
            vec2(3.0, -1.0)
        );
        const vec2 t[3] = vec2[3](
            // 翻转 V 坐标，使 FFmpeg YUV（左上原点）与 OpenGL 采样坐标系一致。
            vec2(0.0, 1.0),
            vec2(0.0, -1.0),
            vec2(2.0, 1.0)
        );
        void main() {
            gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
            uv = t[gl_VertexID];
        }
    )";

    const char* fs = R"(
        #version 330 core
        in vec2 uv;
        out vec4 color;
        uniform sampler2D texY;
        uniform sampler2D texU;
        uniform sampler2D texV;
        void main() {
            float y = texture(texY, uv).r;
            float u = texture(texU, uv).r - 0.5;
            float v = texture(texV, uv).r - 0.5;
            float r = y + 1.402 * v;
            float g = y - 0.344136 * u - 0.714136 * v;
            float b = y + 1.772 * u;
            color = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
        }
    )";

    program_.addShaderFromSourceCode(QOpenGLShader::Vertex, vs);
    program_.addShaderFromSourceCode(QOpenGLShader::Fragment, fs);
    program_.link();

    glGenTextures(1, &texY_);
    glGenTextures(1, &texU_);
    glGenTextures(1, &texV_);
}

void GLVideoWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void GLVideoWidget::ensureTextures(int width, int height) {
    if (width == texWidth_ && height == texHeight_) {
        return;
    }

    texWidth_ = width;
    texHeight_ = height;

    auto initPlane = [this](GLuint tex, int w, int h) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    };

    initPlane(texY_, width, height);
    initPlane(texU_, width / 2, height / 2);
    initPlane(texV_, width / 2, height / 2);
}

void GLVideoWidget::uploadFrame(const AVFrame* frame) {
    ensureTextures(frame->width, frame->height);

    // 每个平面仅调用一次 glTexSubImage2D，避免逐行提交带来的大量驱动调用开销。
    auto uploadPlane = [this](GLuint tex, int w, int h, const uint8_t* data, int stride) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifdef GL_UNPACK_ROW_LENGTH
        if (stride != w) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
        }
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            w,
            h,
            GL_RED,
            GL_UNSIGNED_BYTE,
            data);
        if (stride != w) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
#else
        for (int row = 0; row < h; ++row) {
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                row,
                w,
                1,
                GL_RED,
                GL_UNSIGNED_BYTE,
                data + row * stride);
        }
#endif
    };

    uploadPlane(texY_, frame->width, frame->height, frame->data[0], frame->linesize[0]);
    uploadPlane(texU_, frame->width / 2, frame->height / 2, frame->data[1], frame->linesize[1]);
    uploadPlane(texV_, frame->width / 2, frame->height / 2, frame->data[2], frame->linesize[2]);
}

void GLVideoWidget::paintGL() {
    bool hasNewFrame = false;
    auto t0 = std::chrono::steady_clock::time_point{};

    if (frameQueue_) {
        AVFrame* latest = frameQueue_->popLatest();
        if (latest) {
            if (lastFrame_) {
                av_frame_free(&lastFrame_);
            }
            lastFrame_ = latest;
            hasNewFrame = true;
            t0 = std::chrono::steady_clock::now();
        }
    }

    if (lastFrame_) {
        uploadFrame(lastFrame_);
    }

    glClearColor(0.03f, 0.03f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!texY_ || !texU_ || !texV_ || !program_.isLinked()) {
        return;
    }

    program_.bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texY_);
    program_.setUniformValue("texY", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texU_);
    program_.setUniformValue("texU", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, texV_);
    program_.setUniformValue("texV", 2);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    program_.release();

    if (hasNewFrame) {
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        renderStats_.addSample(ms);
    }
}
