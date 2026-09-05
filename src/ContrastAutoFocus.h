#pragma once

#include <QImage>
#include <QObject>
#include <QVector>

class LensController;

// Autofocus done in the application, by sweeping the lens and keeping the
// position where the picture is sharpest.
//
// This exists because rkaiq's own AF cannot run on this platform: its algorithm
// bypasses whenever ISP statistics are missing ("no af stats, ignore!"), exactly
// as AE and AWB do. Sharpness is instead measured from the preview frames the
// app already decodes, so no statistics are involved.
//
// Every step is driven by arriving frames rather than by sleeping, so a sweep
// never blocks the UI thread and always advances at the pipeline's real frame
// rate however slow that is.
class ContrastAutoFocus : public QObject
{
    Q_OBJECT

public:
    explicit ContrastAutoFocus(QObject* parent = nullptr);

    void setLens(LensController* lens);

    bool isSweeping() const { return m_phase != Phase::Idle; }
    bool isContinuous() const { return m_continuous; }

    // Keep refocusing when the scene changes. Starts with a sweep.
    void setContinuous(bool on);

public slots:
    // One sweep, then hold. Restarts the search if one is already running.
    void focusOnce();
    void cancel();

    // Feed the preview. Ignored unless a sweep is running or continuous mode is
    // watching for the scene to change.
    void onFrame(const QImage& frame);

signals:
    void finished(int position);
    void statusChanged(const QString& message);

private:
    enum class Phase { Idle, Coarse, Fine, Settled };

    void beginSweep();
    void beginFinePass();
    void advance();
    void parkAt(int position);
    static double sharpness(const QImage& frame);

    LensController* m_lens = nullptr;

    Phase m_phase = Phase::Idle;
    QVector<int> m_positions;   // what this pass will visit
    int m_index = 0;            // where in m_positions we are
    int m_settleFrames = 0;     // frames still to discard after the last move
    double m_scoreSum = 0.0;    // scores accumulated at the current position
    int m_scoreCount = 0;

    int m_bestPosition = -1;
    double m_bestScore = -1.0;
    double m_worstScore = -1.0;
    int m_coarseStep = 0;
    int m_startPosition = -1;

    bool m_continuous = false;
    double m_parkedScore = -1.0;
    int m_staleFrames = 0;
};
