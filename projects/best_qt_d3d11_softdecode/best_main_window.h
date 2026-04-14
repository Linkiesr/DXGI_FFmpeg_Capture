#pragma once

#include "avframe_queue.h"

#include <QWidget>

class D3D11VideoWidget;
class QLabel;
class QResizeEvent;

class BestMainWindow final : public QWidget {
    Q_OBJECT
public:
    explicit BestMainWindow(AVFrameQueue* frameQueue, QWidget* parent = nullptr);

    D3D11VideoWidget* videoWidget() const;
public slots:
    void setFrameSize(int frameWidth, int frameHeight);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateVideoDisplaySize();

    D3D11VideoWidget* video_ = nullptr;
    QLabel* overlay_ = nullptr;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
};
