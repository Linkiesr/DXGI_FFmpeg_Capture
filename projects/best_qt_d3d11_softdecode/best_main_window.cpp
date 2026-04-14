#include "best_main_window.h"

#include "d3d11_video_widget.h"

#include <QLabel>
#include <QResizeEvent>

BestMainWindow::BestMainWindow(AVFrameQueue* frameQueue, QWidget* parent)
    : QWidget(parent) {
    video_ = new D3D11VideoWidget(this);
    video_->setFrameQueue(frameQueue);
    video_->setGeometry(rect());

    overlay_ = new QLabel("Best: Qt Shell + Native D3D11 + FFmpeg Soft Decode", this);
    overlay_->setStyleSheet("QLabel { color: white; background: rgba(0,0,0,120); padding: 6px; }");
    overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay_->move(12, 12);
    overlay_->raise();
    overlay_->show();
}

D3D11VideoWidget* BestMainWindow::videoWidget() const {
    return video_;
}

void BestMainWindow::setFrameSize(int frameWidth, int frameHeight) {
    if (frameWidth <= 0 || frameHeight <= 0) {
        return;
    }
    frameWidth_ = frameWidth;
    frameHeight_ = frameHeight;
    updateVideoDisplaySize();
}

void BestMainWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateVideoDisplaySize();
    if (overlay_) {
        overlay_->move(12, 12);
        overlay_->raise();
    }
}

void BestMainWindow::updateVideoDisplaySize() {
    if (!video_) {
        return;
    }
    if (frameWidth_ <= 0 || frameHeight_ <= 0) {
        return;
    }

    QRect clientRect = this->rect();
    const int clientW = clientRect.width();
    const int clientH = clientRect.height();
    if (clientW <= 0 || clientH <= 0) {
        return;
    }

    const double scaleX = static_cast<double>(clientW) / static_cast<double>(frameWidth_);
    const double scaleY = static_cast<double>(clientH) / static_cast<double>(frameHeight_);
    const double scale = (scaleX < scaleY) ? scaleX : scaleY;
    int targetW = static_cast<int>(frameWidth_ * scale);
    int targetH = static_cast<int>(frameHeight_ * scale);

    if (targetW < 1) targetW = 1;
    if (targetH < 1) targetH = 1;

    const int x = (clientW - targetW) / 2;
    const int y = (clientH - targetH) / 2;
    video_->setGeometry(x, y, targetW, targetH);
    video_->update();
}

