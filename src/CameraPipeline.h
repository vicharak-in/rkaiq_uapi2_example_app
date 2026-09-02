#pragma once

#include <QObject>

#include "AiqController.h"
#include "CameraEnumerator.h"
#include "IqOverride.h"

class V4l2Capture;
class QImage;

// Owns the rkaiq engine and the V4L2 capture thread, and enforces the order in
// which they must be brought up.
//
// That order is not the obvious one: the V4L2 format is set AFTER the rkaiq
// engine has been started, matching rkisp_demo's rkisp_routine(). Teardown is
// the exact reverse, and must always run: an ISP left streaming will refuse the
// next launch.
class CameraPipeline : public QObject
{
    Q_OBJECT

public:
    explicit CameraPipeline(QObject* parent = nullptr);
    ~CameraPipeline() override;

    // Discover cameras. Returns false and sets lastError() when none are usable.
    bool discover();
    const QVector<CameraInfo>& cameras() const { return m_cameras; }
    const CameraInfo& currentCamera() const { return m_current; }

    bool start(const CameraInfo& camera, int width, int height,
               AiqController::WorkingMode mode, const QString& iqFileDir);
    void stop();
    bool isRunning() const { return m_running; }

    // Full teardown and rebuild, for a resolution or HDR-mode change.
    bool restart(int width, int height, AiqController::WorkingMode mode);

    // False when the engine receives no ISP statistics, which makes every
    // algorithm-driven setter a no-op. Valid only while running.
    bool statisticsAvailable() const { return m_statsAvailable; }

    // Apply manual AE/AWB by patching the IQ file and bringing the engine back
    // up, for the case above. Costs a stream restart, so callers should
    // coalesce. A no-op returning true when the values are already in effect.
    bool applyManualValues(const IqOverride::Values& values);
    const IqOverride::Values& manualValues() const { return m_manual; }
    bool manualOverrideActive() const { return m_overrideActive; }

    AiqController* aiq() { return m_aiq; }
    QString lastError() const { return m_lastError; }
    QString iqFileDir() const { return m_iqFileDir; }

signals:
    void frameReady(const QImage& frame);
    void statusMessage(const QString& message);
    void errorOccurred(const QString& message);

private:
    AiqController* m_aiq = nullptr;
    V4l2Capture* m_capture = nullptr;

    QVector<CameraInfo> m_cameras;
    CameraInfo m_current;
    QString m_iqFileDir;        // the base directory, never the patched copy
    QString m_lastError;

    IqOverride m_iqOverride;
    IqOverride::Values m_manual;
    bool m_overrideActive = false;
    bool m_statsAvailable = true;
    bool m_statsProbed = false;

    int m_width = 0;
    int m_height = 0;
    AiqController::WorkingMode m_mode = AiqController::WorkingMode::Normal;
    bool m_running = false;
    bool m_firstFrameSeen = false;
};
