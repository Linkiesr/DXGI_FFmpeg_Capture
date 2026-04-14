#include "best_main_window.h"
#include "DecodeThread.h"
#include "d3d11_video_widget.h"

#include <QApplication>
#include <QMessageBox>
#include <QMetaObject>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    if (argc < 2) {
        QMessageBox::information(nullptr, "Usage", "best_qt_d3d11_softdecode.exe <input_video>");
        return 0;
    }

    AVFrameQueue frameQueue;

    BestMainWindow window(&frameQueue);
    auto* video = window.videoWidget();

    window.resize(1280, 720);
    window.show();

    DecodeThread decoder(&frameQueue);
    decoder.setInputPath(QString::fromLocal8Bit(argv[1]));

    decoder.setFrameQueuedCallback([video]() {
        QMetaObject::invokeMethod(video, "onFrameQueued", Qt::QueuedConnection);
    });
    decoder.setFrameSizeCallback([&window](int w, int h) {
        QMetaObject::invokeMethod(
            &window,
            "setFrameSize",
            Qt::QueuedConnection,
            Q_ARG(int, w),
            Q_ARG(int, h));
    });
    decoder.setDecodeErrorCallback([](const QString& msg) {
        qWarning("Decode error: %s", qPrintable(msg));
    });
    decoder.setDecodeFinishedCallback([]() {
        qInfo("Decode finished.");
        //QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
    });

    decoder.start();
    const int rc = app.exec();

    decoder.stop();
    decoder.wait();

    decoder.printDecodeTimingStats();
    video->printRenderTimingStats();

    return rc;
}
