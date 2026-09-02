#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>

#include <csignal>

namespace {

// Default IQ file location. This is a directory: rkaiq matches the attached
// sensor to a .json inside it, so no filename is ever hard-coded.
const char* kDefaultIqDir = "/etc/iqfiles";

QApplication* g_app = nullptr;

// The ISP stays claimed if the process dies without tearing the pipeline down,
// and the next launch then fails. Ctrl-C must therefore go through Qt's normal
// shutdown rather than killing the process outright.
void handleTerminationSignal(int signalNumber)
{
    qWarning("\nReceived signal %d, shutting down...", signalNumber);
    if (g_app)
        QMetaObject::invokeMethod(g_app, "quit", Qt::QueuedConnection);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("camera-ctls"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));

    g_app = &app;
    std::signal(SIGINT, handleTerminationSignal);
    std::signal(SIGTERM, handleTerminationSignal);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Qt control panel for RK3588 ISP cameras via the rkaiq uAPI2.\n"
                       "Cameras, resolutions, frame rates and hardware capabilities are "
                       "all discovered at runtime; the options below are only overrides."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption iqDirOption(
        { QStringLiteral("iqdir"), QStringLiteral("i") },
        QStringLiteral("Directory holding the IQ tuning files (default: %1).")
            .arg(QLatin1String(kDefaultIqDir)),
        QStringLiteral("directory"), QLatin1String(kDefaultIqDir));
    parser.addOption(iqDirOption);

    const QCommandLineOption deviceOption(
        { QStringLiteral("device"), QStringLiteral("d") },
        QStringLiteral("Prefer the camera on this capture node (e.g. the ISP main path). "
                       "Without it, the first discovered camera is used."),
        QStringLiteral("path"));
    parser.addOption(deviceOption);

    parser.process(app);

    MainWindow window(parser.value(iqDirOption), parser.value(deviceOption));
    window.resize(1280, 760);
    window.show();

    return app.exec();
}
