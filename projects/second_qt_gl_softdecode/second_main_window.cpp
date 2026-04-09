#include "second_main_window.h"

#include "gl_video_widget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>

SecondMainWindow::SecondMainWindow(AVFrameQueue* frameQueue, QWidget* parent)
    : QWidget(parent) {
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);

    video_ = new GLVideoWidget(this);
    video_->setFrameQueue(frameQueue);
    layout_->addWidget(video_);

    overlay_ = new QLabel("Second: Qt OpenGL + FFmpeg Soft Decode", this);
    overlay_->setStyleSheet("QLabel { color: white; background: rgba(0,0,0,120); padding: 6px; }");
    overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay_->move(12, 12);
    overlay_->raise();
    overlay_->show();
}

GLVideoWidget* SecondMainWindow::videoWidget() const {
    return video_;
}

void SecondMainWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (overlay_) {
        overlay_->move(12, 12);
        overlay_->raise();
    }
}

