#include "DecodeThread.h"
#include "gl_video_widget.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>


int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    if (argc < 2) {
        QMessageBox::information(nullptr, "Usage", "second_qt_gl_softdecode.exe <input_video>");
        return 0;
    }

    AVFrameQueue frameQueue;

    QMainWindow window;
    auto* central = new QWidget(&window);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* video = new GLVideoWidget(central);
    video->setFrameQueue(&frameQueue);

    auto* overlay = new QLabel("次优方案: Qt内OpenGL渲染 + FFmpeg软解", central);
    overlay->setStyleSheet("QLabel { color: white; background: rgba(0,0,0,120); padding: 6px; }");
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents);

    layout->addWidget(video);
    window.setCentralWidget(central);
    window.resize(1280, 720);
    window.show();

    overlay->move(12, 12);
    overlay->raise();
    overlay->show();

    DecodeThread decoder(&frameQueue);
    decoder.setInputPath(QString::fromLocal8Bit(argv[1]));

    decoder.setFrameQueuedCallback([video]() {
        QMetaObject::invokeMethod(video, "onFrameQueued", Qt::QueuedConnection);
    });
    decoder.setDecodeErrorCallback([](const QString& msg) {
        qWarning("Decode error: %s", qPrintable(msg));
    });
    decoder.setDecodeFinishedCallback([]() {
        qInfo("Decode finished.");
        QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
    });

    decoder.start();
    const int rc = app.exec();

    decoder.stop();
    decoder.wait();

    decoder.printDecodeTimingStats();
    video->printRenderTimingStats();

    return rc;
}
