#include "d3d11_video_widget.h"
#include "decoder_thread.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    if (argc < 2) {
        QMessageBox::information(nullptr, "Usage", "best_qt_d3d11_softdecode.exe <input_video>");
        return 0;
    }

    AVFrameQueue frameQueue;

    QMainWindow window;
    auto* central = new QWidget(&window);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* video = new D3D11VideoWidget(central);
    video->setFrameQueue(&frameQueue);

    auto* overlay = new QLabel("Best方案: Qt壳 + 原生D3D11渲染 + FFmpeg软解", central);
    overlay->setStyleSheet("QLabel { color: white; background: rgba(0,0,0,120); padding: 6px; }");
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents);

    layout->addWidget(video);
    window.setCentralWidget(central);
    window.resize(1280, 720);
    window.show();

    overlay->move(12, 12);
    overlay->raise();
    overlay->show();

    DecoderThread decoder(&frameQueue);
    decoder.setInputPath(QString::fromLocal8Bit(argv[1]));

    QObject::connect(&decoder, &DecoderThread::frameQueued, video, &D3D11VideoWidget::onFrameQueued, Qt::QueuedConnection);
    QObject::connect(&decoder, &DecoderThread::decodeError, &window, [](const QString& msg) {
        qWarning("Decode error: %s", qPrintable(msg));
    });
    QObject::connect(&decoder, &DecoderThread::decodeFinished, &window, []() {
        qInfo("Decode finished.");
        qApp->quit();
    });

    decoder.start();
    const int rc = app.exec();

    decoder.stop();
    decoder.wait();

    decoder.printDecodeTimingStats();
    video->printRenderTimingStats();

    return rc;
}
