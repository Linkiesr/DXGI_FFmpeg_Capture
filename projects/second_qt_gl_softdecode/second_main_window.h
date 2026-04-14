#pragma once

#include "avframe_queue.h"

#include <QWidget>

class GLVideoWidget;
class QLabel;
class QResizeEvent;

class SecondMainWindow final : public QWidget {
    Q_OBJECT
public:
    explicit SecondMainWindow(AVFrameQueue* frameQueue, QWidget* parent = nullptr);

    GLVideoWidget* videoWidget() const;
public slots:
    void setFrameSize(int frameWidth, int frameHeight);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateVideoDisplaySize();

    GLVideoWidget* video_ = nullptr;
    QLabel* overlay_ = nullptr;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
};
