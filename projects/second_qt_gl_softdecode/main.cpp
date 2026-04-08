#include "decoder_thread.h"
#include "gl_video_widget.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // 命令行参数：second_qt_gl_softdecode.exe <input_video>
    if (argc < 2) {
        QMessageBox::information(nullptr, "Usage", "second_qt_gl_softdecode.exe <input_video>");
        return 0;
    }

    QMainWindow window;
    auto* central = new QWidget(&window);
    auto* layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* video = new GLVideoWidget(central);
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

    DecoderThread decoder;
    decoder.setInputPath(QString::fromLocal8Bit(argv[1]));

    QObject::connect(&decoder, &DecoderThread::frameReady, video, &GLVideoWidget::onFrameReady, Qt::QueuedConnection);
    QObject::connect(&decoder, &DecoderThread::decodeError, &window, [](const QString& msg) {
        qWarning("Decode error: %s", qPrintable(msg));
    });
    QObject::connect(&decoder, &DecoderThread::decodeFinished, &window, []() {
        qInfo("Decode finished.");
    });

    decoder.start();
    const int rc = app.exec();

    // 在应用退出前确保解码线程已停止。
    decoder.stop();
    decoder.wait();
    return rc;
}

