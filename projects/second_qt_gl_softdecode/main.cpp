#include "DecodeThread.h"
#include "second_main_window.h"
#include "gl_video_widget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QMetaObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QProcess>
#include <QSurfaceFormat>

namespace {
enum class GlBackend {
    Desktop,
    AngleEs2,
    Software
};

GlBackend parseBackendFromArgv(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--gl-backend=desktop") return GlBackend::Desktop;
        if (arg == "--gl-backend=angle") return GlBackend::AngleEs2;
        if (arg == "--gl-backend=software") return GlBackend::Software;
    }
    return GlBackend::Desktop;
}

QString backendArg(GlBackend backend) {
    switch (backend) {
    case GlBackend::Desktop: return "--gl-backend=desktop";
    case GlBackend::AngleEs2: return "--gl-backend=angle";
    case GlBackend::Software: return "--gl-backend=software";
    }
    return "--gl-backend=desktop";
}

bool nextBackend(GlBackend current, GlBackend& next) {
    if (current == GlBackend::Desktop) {
        next = GlBackend::AngleEs2;
        return true;
    }
    if (current == GlBackend::AngleEs2) {
        next = GlBackend::Software;
        return true;
    }
    return false;
}

void configureBackendBeforeApp(GlBackend backend) {
    QSurfaceFormat fmt;
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(1);

    if (backend == GlBackend::Desktop) {
        QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        fmt.setVersion(3, 3);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
    } else if (backend == GlBackend::AngleEs2) {
        QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);
        fmt.setRenderableType(QSurfaceFormat::OpenGLES);
        fmt.setVersion(2, 0);
        fmt.setProfile(QSurfaceFormat::NoProfile);
    } else {
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        fmt.setVersion(2, 0);
        fmt.setProfile(QSurfaceFormat::NoProfile);
    }

    QSurfaceFormat::setDefaultFormat(fmt);
}

bool probeOpenGlContext(GlBackend backend, QString& reason) {
    QOffscreenSurface surface;
    surface.setFormat(QSurfaceFormat::defaultFormat());
    surface.create();
    if (!surface.isValid()) {
        reason = "QOffscreenSurface create failed";
        return false;
    }

    QOpenGLContext ctx;
    ctx.setFormat(surface.format());
    if (!ctx.create()) {
        reason = "QOpenGLContext create failed";
        return false;
    }
    if (!ctx.makeCurrent(&surface)) {
        reason = "QOpenGLContext makeCurrent failed";
        return false;
    }

    const QSurfaceFormat got = ctx.format();
    if (backend == GlBackend::Desktop) {
        if (got.renderableType() != QSurfaceFormat::OpenGL ||
            got.majorVersion() < 3 ||
            (got.majorVersion() == 3 && got.minorVersion() < 3)) {
            reason = QString("Need Desktop GL >=3.3, got type=%1 ver=%2.%3")
                .arg(static_cast<int>(got.renderableType()))
                .arg(got.majorVersion())
                .arg(got.minorVersion());
            ctx.doneCurrent();
            return false;
        }
    } else if (backend == GlBackend::AngleEs2) {
        if (got.renderableType() != QSurfaceFormat::OpenGLES ||
            got.majorVersion() < 2) {
            reason = QString("Need OpenGLES >=2.0, got type=%1 ver=%2.%3")
                .arg(static_cast<int>(got.renderableType()))
                .arg(got.majorVersion())
                .arg(got.minorVersion());
            ctx.doneCurrent();
            return false;
        }
    } else {
        if (got.majorVersion() < 2) {
            reason = QString("Need software OpenGL >=2.0, got ver=%1.%2")
                .arg(got.majorVersion())
                .arg(got.minorVersion());
            ctx.doneCurrent();
            return false;
        }
    }

    ctx.doneCurrent();
    return true;
}

int relaunchWithBackend(GlBackend nextBackendValue) {
    QStringList args = QCoreApplication::arguments();
    QStringList newArgs;
    for (int i = 1; i < args.size(); ++i) {
        if (!args[i].startsWith("--gl-backend=")) {
            newArgs.push_back(args[i]);
        }
    }
    newArgs.push_back(backendArg(nextBackendValue));

    const bool ok = QProcess::startDetached(QCoreApplication::applicationFilePath(), newArgs);
    return ok ? 0 : -1;
}
} // namespace

int main(int argc, char* argv[]) {
    const GlBackend backend = parseBackendFromArgv(argc, argv);
    configureBackendBeforeApp(backend);

    QApplication app(argc, argv);

    QString probeReason;
    if (!probeOpenGlContext(backend, probeReason)) {
        GlBackend next;
        if (nextBackend(backend, next)) {
            qWarning("Current GL backend probe failed: %s. Relaunching with next backend...",
                     qPrintable(probeReason));
            return relaunchWithBackend(next);
        }
        QMessageBox::critical(nullptr, "OpenGL Init Failed",
                              QString::fromLocal8Bit("OpenGL 初始化失败，且所有后端回退均失败。\n原因: %1")
                                  .arg(probeReason));
        return -1;
    }

    if (argc < 2) {
        QMessageBox::information(nullptr, "Usage", "second_qt_gl_softdecode.exe <input_video>");
        return 0;
    }

    AVFrameQueue frameQueue;

    SecondMainWindow window(&frameQueue);
    auto* video = window.videoWidget();

    window.resize(1280, 720);
    window.show();
    //window.showMaximized();


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
