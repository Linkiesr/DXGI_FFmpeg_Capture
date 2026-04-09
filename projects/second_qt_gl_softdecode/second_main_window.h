#pragma once

#include "avframe_queue.h"

#include <QWidget>

class GLVideoWidget;
class QLabel;
class QHBoxLayout;
class QResizeEvent;

class SecondMainWindow final : public QWidget {
public:
    explicit SecondMainWindow(AVFrameQueue* frameQueue, QWidget* parent = nullptr);

    GLVideoWidget* videoWidget() const;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    GLVideoWidget* video_ = nullptr;
    QLabel* overlay_ = nullptr;
    QHBoxLayout* layout_ = nullptr;
};

