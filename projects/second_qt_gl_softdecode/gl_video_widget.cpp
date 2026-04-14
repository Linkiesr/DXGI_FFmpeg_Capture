#include "gl_video_widget.h"

#include <QMetaObject>
#include <QDebug>
#include <QOpenGLContext>

#include <algorithm>
#include <chrono>

#ifndef GL_VIDEO_WIDGET_VERBOSE_LOG
#define GL_VIDEO_WIDGET_VERBOSE_LOG 0
#endif

#if GL_VIDEO_WIDGET_VERBOSE_LOG
#define GLW_LOG() qDebug()
#define GLW_LOG_NQ() qDebug().noquote()
#else
#define GLW_LOG() while (false) qDebug()
#define GLW_LOG_NQ() while (false) qDebug().noquote()
#endif

GLVideoWidget::GLVideoWidget(QWidget* parent)
    : QOpenGLWidget(parent) {
}

GLVideoWidget::~GLVideoWidget() {
    makeCurrent();
    if (desktopVao_) {
        if (QOpenGLContext::currentContext()) {
            QOpenGLExtraFunctions* ex = QOpenGLContext::currentContext()->extraFunctions();
            if (ex) {
                ex->glDeleteVertexArrays(1, &desktopVao_);
            }
        }
        desktopVao_ = 0;
    }
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

    QSurfaceFormat fmt = context()->format();
    useEs2Path_ = (fmt.renderableType() == QSurfaceFormat::OpenGLES || fmt.majorVersion() < 3);
    GLW_LOG() << "[initializeGL] renderableType=" << fmt.renderableType()
              << "version=" << fmt.majorVersion() << "." << fmt.minorVersion()
              << "profile=" << fmt.profile()
              << "useEs2Path=" << useEs2Path_;

    const char* vs = nullptr;
    const char* fs = nullptr;
    if (useEs2Path_) {
        vs = R"(
            attribute vec2 aPos;
            attribute vec2 aUv;
            varying vec2 uv;
            void main() {
                gl_Position = vec4(aPos, 0.0, 1.0);
                uv = aUv;
            }
        )";
        fs = R"(
            #ifdef GL_ES
            precision mediump float;
            #endif
            varying vec2 uv;
            uniform sampler2D texY;
            uniform sampler2D texU;
            uniform sampler2D texV;
            void main() {
                float y = texture2D(texY, uv).r;
                float u = texture2D(texU, uv).r - 0.5;
                float v = texture2D(texV, uv).r - 0.5;
                float r = y + 1.402 * v;
                float g = y - 0.344136 * u - 0.714136 * v;
                float b = y + 1.772 * u;
                gl_FragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
            }
        )";
    } else {
        vs = R"(
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
        fs = R"(
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
    }

    const bool vsOk = program_.addShaderFromSourceCode(QOpenGLShader::Vertex, vs);
    GLW_LOG_NQ() << "[initializeGL] VS log:\n" << program_.log();
    const bool fsOk = program_.addShaderFromSourceCode(QOpenGLShader::Fragment, fs);
    GLW_LOG_NQ() << "[initializeGL] FS log:\n" << program_.log();
    const bool linkOk = program_.link();
    GLW_LOG_NQ() << "[initializeGL] link log:\n" << program_.log();
    GLW_LOG() << "[initializeGL] shader status: vsOk=" << vsOk
              << "fsOk=" << fsOk
              << "linkOk=" << linkOk
              << "isLinked=" << program_.isLinked();

    if (useEs2Path_) {
        attrPosLoc_ = program_.attributeLocation("aPos");
        attrUvLoc_ = program_.attributeLocation("aUv");
    } else {
        // CoreProfile 路径必须绑定 VAO，否则 glDrawArrays 会报 GL_INVALID_OPERATION(0x502)。
        if (QOpenGLContext::currentContext()) {
            QOpenGLExtraFunctions* ex = QOpenGLContext::currentContext()->extraFunctions();
            if (ex) {
                ex->glGenVertexArrays(1, &desktopVao_);
                ex->glBindVertexArray(desktopVao_);
                GLW_LOG() << "[initializeGL] desktop dummy VAO =" << desktopVao_;
            }
        }
    }

    glGenTextures(1, &texY_);
    glGenTextures(1, &texU_);
    glGenTextures(1, &texV_);
    GLW_LOG() << "[initializeGL] tex ids: Y=" << texY_ << "U=" << texU_ << "V=" << texV_;
    const GLenum texErr = this->glGetError();
    GLW_LOG() << "[initializeGL] glGetError after glGenTextures ="
              << QString("0x%1").arg(static_cast<int>(texErr), 0, 16);
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
        if (useEs2Path_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        }
    };

    initPlane(texY_, width, height);
    initPlane(texU_, width / 2, height / 2);
    initPlane(texV_, width / 2, height / 2);
}

void GLVideoWidget::uploadFrame(const AVFrame* frame) {
    GLW_LOG() << "[uploadFrame] fmt=" << frame->format
              << "w=" << frame->width
              << "h=" << frame->height
              << "ls0=" << frame->linesize[0]
              << "ls1=" << frame->linesize[1]
              << "ls2=" << frame->linesize[2];

    int uploadW = ((frame->width & ~1) > 2) ? (frame->width & ~1) : 2;
    int uploadH = ((frame->height & ~1) > 2) ? (frame->height & ~1) : 2;
    // 1:1 不缩放：窗口比帧小时，仅上传左上角可见区域，避免整帧被压缩采样。
    if (width() > 0) {
        uploadW = (std::min)(uploadW, width());
    }
    if (height() > 0) {
        uploadH = (std::min)(uploadH, height());
    }
    uploadW = ((uploadW & ~1) > 2) ? (uploadW & ~1) : 2;
    uploadH = ((uploadH & ~1) > 2) ? (uploadH & ~1) : 2;
    ensureTextures(uploadW, uploadH);

    // 每个平面仅调用一次 glTexSubImage2D，避免逐行提交带来的大量驱动调用开销。
    auto uploadPlane = [this](GLuint tex, int w, int h, const uint8_t* data, int stride) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifdef GL_UNPACK_ROW_LENGTH
        if (stride != w) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
        }
        const GLenum pixelFormat = useEs2Path_ ? GL_LUMINANCE : GL_RED;
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            w,
            h,
            pixelFormat,
            GL_UNSIGNED_BYTE,
            data);
        if (stride != w) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
#else
        for (int row = 0; row < h; ++row) {
            const GLenum pixelFormat = useEs2Path_ ? GL_LUMINANCE : GL_RED;
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                row,
                w,
                1,
                pixelFormat,
                GL_UNSIGNED_BYTE,
                data + row * stride);
        }
#endif
    };

    uploadPlane(texY_, uploadW, uploadH, frame->data[0], frame->linesize[0]);
    uploadPlane(texU_, uploadW / 2, uploadH / 2, frame->data[1], frame->linesize[1]);
    uploadPlane(texV_, uploadW / 2, uploadH / 2, frame->data[2], frame->linesize[2]);
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

    // 当渲染控件尺寸大于帧尺寸时允许放大填充；
    // 当渲染控件尺寸小于帧尺寸时保持 1:1 并裁剪，不做缩小。
    // OpenGL 视口原点在左下，因此要把 y 移到顶部区域以保持“左上角对齐”。
    const int drawW = (std::max)(texWidth_, width());
    const int drawH = (std::max)(texHeight_, height());
    const int drawY = height() - drawH;
    glViewport(0, drawY, drawW, drawH);

    glClearColor(0.03f, 0.03f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLW_LOG() << "[paintGL] texY=" << texY_
              << "texU=" << texU_
              << "texV=" << texV_
              << "linked=" << program_.isLinked()
              << "widget=" << width() << "x" << height()
              << "tex=" << texWidth_ << "x" << texHeight_;

    if (!texY_ || !texU_ || !texV_ || !program_.isLinked()) {
        GLW_LOG() << "[paintGL] early return: texture/program invalid";
        return;
    }

    GLenum errBefore = this->glGetError();
    GLW_LOG() << "[paintGL] glGetError before bind ="
              << QString("0x%1").arg(static_cast<int>(errBefore), 0, 16);

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

    if (useEs2Path_) {
        // ES2 路径不支持 gl_VertexID，使用 attribute 顶点数据提交一个全屏四边形。
        const GLfloat pos[] = {
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f
        };
        const GLfloat uv[] = {
            0.0f, 1.0f,
            1.0f, 1.0f,
            0.0f, 0.0f,
            1.0f, 0.0f
        };
        if (attrPosLoc_ >= 0) {
            glEnableVertexAttribArray(static_cast<GLuint>(attrPosLoc_));
            glVertexAttribPointer(static_cast<GLuint>(attrPosLoc_), 2, GL_FLOAT, GL_FALSE, 0, pos);
        }
        if (attrUvLoc_ >= 0) {
            glEnableVertexAttribArray(static_cast<GLuint>(attrUvLoc_));
            glVertexAttribPointer(static_cast<GLuint>(attrUvLoc_), 2, GL_FLOAT, GL_FALSE, 0, uv);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (attrPosLoc_ >= 0) {
            glDisableVertexAttribArray(static_cast<GLuint>(attrPosLoc_));
        }
        if (attrUvLoc_ >= 0) {
            glDisableVertexAttribArray(static_cast<GLuint>(attrUvLoc_));
        }
        GLW_LOG() << "[paintGL] draw submitted (ES2 path)";
    } else {
        if (QOpenGLContext::currentContext()) {
            QOpenGLExtraFunctions* ex = QOpenGLContext::currentContext()->extraFunctions();
            if (ex) {
                ex->glBindVertexArray(desktopVao_);
            }
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);
        GLW_LOG() << "[paintGL] draw submitted (Desktop path)";
    }

    GLenum errAfter = this->glGetError();
    GLW_LOG() << "[paintGL] glGetError after draw ="
              << QString("0x%1").arg(static_cast<int>(errAfter), 0, 16);

    program_.release();

    if (hasNewFrame) {
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        renderStats_.addSample(ms);
    }
}
