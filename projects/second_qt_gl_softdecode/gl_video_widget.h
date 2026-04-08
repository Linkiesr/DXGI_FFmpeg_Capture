#pragma once

#include "decoded_frame.h"

#include <QMutex>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

// Qt 原生 OpenGL 渲染器（次优方案路径）。
class GLVideoWidget final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit GLVideoWidget(QWidget* parent = nullptr);
    ~GLVideoWidget() override;

public slots:
    // 接收解码线程送来的最新帧。
    void onFrameReady(const DecodedFramePtr& frame);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void ensureTextures(int width, int height);
    void uploadFrame(const DecodedFrame& frame);

    QOpenGLShaderProgram program_;
    GLuint texY_ = 0;
    GLuint texU_ = 0;
    GLuint texV_ = 0;
    int texWidth_ = 0;
    int texHeight_ = 0;

    QMutex mutex_;
    DecodedFramePtr latestFrame_;
};

