#include "CameraPipeline.h"

#include "V4l2Capture.h"

#include <QDebug>

CameraPipeline::CameraPipeline(QObject* parent)
    : QObject(parent)
    , m_aiq(new AiqController(this))
    , m_capture(new V4l2Capture(this))
{
    connect(m_capture, &V4l2Capture::frameReady, this, &CameraPipeline::frameReady);
    connect(m_capture, &V4l2Capture::frameReady, this, [this](const QImage& frame) {
        // Logged once per stream: confirms frames are arriving, not merely that
        // STREAMON succeeded.
        if (!m_firstFrameSeen) {
            m_firstFrameSeen = true;
            qInfo().noquote() << QStringLiteral("First frame received: %1x%2")
                                     .arg(frame.width()).arg(frame.height());
        }
    });
    connect(m_capture, &V4l2Capture::errorOccurred, this, &CameraPipeline::errorOccurred);

    connect(m_aiq, &AiqController::apiCallFailed, this,
            [this](const QString& function, int code) {
                emit statusMessage(QStringLiteral("%1 failed (%2)").arg(function).arg(code));
            });
    connect(m_aiq, &AiqController::apiCallSucceeded, this,
            [this](const QString& function) {
                emit statusMessage(QStringLiteral("%1 ok").arg(function));
            });
}

CameraPipeline::~CameraPipeline()
{
    stop();
}

bool CameraPipeline::discover()
{
    m_cameras = CameraEnumerator::enumerateCameras();

    if (m_cameras.isEmpty()) {
        m_lastError = CameraEnumerator::lastError();
        return false;
    }

    m_lastError.clear();
    return true;
}

bool CameraPipeline::start(const CameraInfo& camera, int width, int height,
                           AiqController::WorkingMode mode, const QString& iqFileDir)
{
    if (m_running)
        stop();

    if (!camera.isValid() || camera.videoNode.isEmpty()) {
        m_lastError = QStringLiteral("No usable camera to start");
        return false;
    }

    // A different sensor may well answer differently, so the cached probe below
    // is only good for as long as the camera does not change.
    if (m_current.sensorEntity != camera.sensorEntity)
        m_statsProbed = false;

    m_firstFrameSeen = false;
    m_current   = camera;
    m_width     = width;
    m_height    = height;
    m_mode      = mode;
    m_iqFileDir = iqFileDir;

    // The engine takes a directory and re-derives the file name itself, so an
    // override is served by pointing it at a directory holding a patched copy
    // under the same name.
    QString effectiveIqDir = iqFileDir;
    if (m_overrideActive) {
        if (!m_iqOverride.isLoaded())
            m_iqOverride.load(iqFileDir, CameraEnumerator::iqFileNameFor(camera.sensorSubdev));

        const QString dir = m_iqOverride.write(m_manual);
        if (dir.isEmpty()) {
            m_lastError = QStringLiteral("Could not apply manual values: %1")
                              .arg(m_iqOverride.lastError());
            return false;
        }
        effectiveIqDir = dir;
    }

    // Only one process may own the rkaiq engine. A second sysctl_init() does
    // not fail, it blocks forever, so the conflict is caught here instead while
    // there is still someone to report it to.
    const QString lockProblem = CameraEnumerator::engineLockProblem(camera.phyId);
    if (!lockProblem.isEmpty()) {
        m_lastError = lockProblem;
        return false;
    }

    const EngineOwner owner = CameraEnumerator::findEngineOwner();
    if (owner.detected()) {
        m_lastError = QStringLiteral(
            "Another process is already running the rkaiq engine: %1 (pid %2)%3. "
            "Only one owner is allowed, and starting a second would hang. "
            "Stop it first, e.g. 'sudo systemctl stop rkaiq_3A.service' or "
            "'sudo killall %1'.")
            .arg(owner.processName)
            .arg(owner.pid)
            .arg(owner.nodePath.isEmpty() ? QString()
                                          : QStringLiteral(", holding %1").arg(owner.nodePath));
        return false;
    }

    // Order below follows rkisp_demo's rkisp_routine() exactly.

    // 1-5. preInit_scene + sysctl_init, keyed on the discovered sensor entity.
    if (!m_aiq->init(camera.sensorEntity, effectiveIqDir, mode)) {
        m_lastError = QStringLiteral(
            "rkaiq init failed for '%1' with IQ directory '%2'. Check that a matching "
            "IQ file exists and that no other process (e.g. rkaiq_3A.service) owns the ISP.")
            .arg(camera.sensorEntity, effectiveIqDir);
        return false;
    }

    // 6-7. prepare then start, with the same geometry S_FMT will be given.
    if (!m_aiq->prepare(width, height, mode)) {
        m_lastError = QStringLiteral("rkaiq prepare failed at %1x%2").arg(width).arg(height);
        m_aiq->deinit();
        return false;
    }

    if (!m_aiq->start()) {
        m_lastError = QStringLiteral("rkaiq start failed");
        m_aiq->deinit();
        return false;
    }

    // 8-9. Only now is the capture node configured and streamed.
    if (!m_capture->open(camera.videoNode, width, height)) {
        m_lastError = m_capture->lastError();
        m_aiq->stop();
        m_aiq->deinit();
        return false;
    }

    m_capture->start();
    m_running = true;

    // The focus motor is independent of the engine, so it is opened here rather
    // than through rkaiq. Restarting the engine for an exposure change must not
    // lose the focus position, so the last requested one is re-applied.
    const int previousFocus = m_lens.lastRequested();
    if (camera.hasLens() && m_lens.open(camera.lensSubdev) && previousFocus >= 0)
        m_lens.setPosition(previousFocus);

    // Ask once, now that frames are flowing: if no statistics arrive the
    // algorithm-driven setters are inert and manual values have to go through
    // the IQ file instead.
    // Probing blocks for up to a second and a half, and the answer cannot
    // change between restarts of the same camera, so it is asked once.
    if (!m_statsProbed) {
        m_statsAvailable = m_aiq->statisticsAvailable();
        m_statsProbed = true;
    }

    const QString summary = QStringLiteral("Streaming %1 at %2x%3 (%4) from %5")
                                .arg(camera.sensorName)
                                .arg(m_capture->width())
                                .arg(m_capture->height())
                                .arg(m_capture->pixelFormatName(), camera.videoNode);
    qInfo().noquote() << summary;
    emit statusMessage(summary);
    return true;
}

void CameraPipeline::stop()
{
    m_lens.close();

    if (!m_running && !m_aiq->isInitialised())
        return;

    // Reverse of start: stream off, engine stopped, buffers released, engine
    // deinitialised. Skipping any step here leaves the ISP claimed.
    m_capture->stopCapture();
    m_capture->close();

    m_aiq->stop();
    m_aiq->deinit();

    m_running = false;
}

bool CameraPipeline::applyManualValues(const IqOverride::Values& values)
{
    if (m_overrideActive && values == m_manual)
        return true;

    if (!m_current.isValid()) {
        m_lastError = QStringLiteral("No camera running to apply manual values to");
        return false;
    }

    const IqOverride::Values previous = m_manual;
    const bool wasActive = m_overrideActive;

    m_manual = values;
    m_overrideActive = true;

    if (restart(m_width, m_height, m_mode))
        return true;

    // Leave the previous state intact so a failed apply does not strand the
    // caller with values that never took effect.
    m_manual = previous;
    m_overrideActive = wasActive;
    return false;
}

bool CameraPipeline::restart(int width, int height, AiqController::WorkingMode mode)
{
    const CameraInfo camera = m_current;
    const QString iqDir = m_iqFileDir;

    stop();

    if (!camera.isValid()) {
        m_lastError = QStringLiteral("No camera selected to restart");
        return false;
    }

    return start(camera, width, height, mode, iqDir);
}
