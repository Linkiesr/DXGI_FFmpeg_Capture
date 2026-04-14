#pragma once

#include "avframe_queue.h"
#include "timing_stats.h"

#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

class GLVideoWidget final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit GLVideoWidget(QWidget* parent = nullptr);
    ~GLVideoWidget() override;

    void setFrameQueue(AVFrameQueue* frameQueue);

    // 打印“从队列取帧到渲染提交”耗时统计。
    void printRenderTimingStats() const;

public slots:
    void onFrameQueued();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void ensureTextures(int width, int height);
    void uploadFrame(const AVFrame* frame);

    QOpenGLShaderProgram program_;
    bool useEs2Path_ = false;
    int attrPosLoc_ = -1;
    int attrUvLoc_ = -1;
    GLuint texY_ = 0;
    GLuint texU_ = 0;
    GLuint texV_ = 0;
    GLuint desktopVao_ = 0;
    int texWidth_ = 0;
    int texHeight_ = 0;

    AVFrameQueue* frameQueue_ = nullptr;
    AVFrame* lastFrame_ = nullptr;
    TimingStats renderStats_;
};
