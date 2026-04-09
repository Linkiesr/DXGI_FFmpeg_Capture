#include "best_main_window.h"

#include "d3d11_video_widget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>

BestMainWindow::BestMainWindow(AVFrameQueue* frameQueue, QWidget* parent)
    : QWidget(parent) {
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);

    video_ = new D3D11VideoWidget(this);
    video_->setFrameQueue(frameQueue);
    layout_->addWidget(video_);

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

void BestMainWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (overlay_) {
        overlay_->move(12, 12);
        overlay_->raise();
    }
}

