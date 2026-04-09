#pragma once

#include "avframe_queue.h"

#include <QWidget>

class D3D11VideoWidget;
class QLabel;
class QHBoxLayout;
class QResizeEvent;

class BestMainWindow final : public QWidget {
public:
    explicit BestMainWindow(AVFrameQueue* frameQueue, QWidget* parent = nullptr);

    D3D11VideoWidget* videoWidget() const;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    D3D11VideoWidget* video_ = nullptr;
    QLabel* overlay_ = nullptr;
    QHBoxLayout* layout_ = nullptr;
};

