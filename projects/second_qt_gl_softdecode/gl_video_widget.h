#pragma once

#include "avframe_queue.h"

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

// Qt 原生 OpenGL 渲染器（次优方案路径）。
class GLVideoWidget final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit GLVideoWidget(QWidget* parent = nullptr);
    ~GLVideoWidget() override;

    void setFrameQueue(AVFrameQueue* frameQueue);

public slots:
    // 仅通知有新帧可用，具体取帧在渲染线程完成。
    void onFrameQueued();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void ensureTextures(int width, int height);
    void uploadFrame(const AVFrame* frame);

    QOpenGLShaderProgram program_;
    GLuint texY_ = 0;
    GLuint texU_ = 0;
    GLuint texV_ = 0;
    int texWidth_ = 0;
    int texHeight_ = 0;

    AVFrameQueue* frameQueue_ = nullptr;
    AVFrame* lastFrame_ = nullptr;
};
