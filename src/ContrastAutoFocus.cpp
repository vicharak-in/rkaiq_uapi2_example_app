#include "ContrastAutoFocus.h"

#include "LensController.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {

// Positions visited on the first pass. Enough to find which part of the range
// holds the peak without making the sweep feel slow.
constexpr int kCoarseSteps = 9;

// Positions visited on the second pass, spread across one coarse step either
// side of the coarse winner.
constexpr int kFineSteps = 6;

// Frames discarded after each move, then frames averaged to score it. Both are
// kept small because a whole sweep costs
// (kCoarseSteps + kFineSteps) * (kSettleFrames + kScoreFrames) frames, and the
// preview here runs at about 9 fps, so every extra frame is over a second added
// to how long focusing appears to take. A driver implementing
// RK_VIDIOC_VCM_TIMEINFO would let us wait exactly as long as the move needs;
// ak7375 and dw9807-vcm do not, so the settle count is a fixed guess.
constexpr int kSettleFrames = 2;

// Scores averaged at each position before moving on. The peak can be shallow on
// a low-contrast scene, shallow enough that single-frame noise picks the wrong
// winner, so each position is measured more than once.
constexpr int kScoreFrames = 3;

// Continuous mode refocuses when sharpness falls to this fraction of what it
// was at the parked position, for kStaleFramesNeeded frames running. The gap
// between the two keeps a passing hand or a moment of motion blur from
// triggering a whole sweep.
// How much the peak must stand above the flattest part of the sweep before the
// result is believed. A blank wall, or a scene too dim to show detail, produces
// an almost flat curve where the winner is just noise; moving the lens on that
// basis is worse than leaving it alone.
constexpr double kMinPeakProminence = 1.12;

constexpr double kRefocusRatio = 0.70;
constexpr int kStaleFramesNeeded = 12;

} // namespace

ContrastAutoFocus::ContrastAutoFocus(QObject* parent)
    : QObject(parent)
{
}

void ContrastAutoFocus::setLens(LensController* lens)
{
    cancel();
    m_lens = lens;
}

void ContrastAutoFocus::setContinuous(bool on)
{
    m_continuous = on;
    if (on)
        focusOnce();
    else
        cancel();
}

void ContrastAutoFocus::cancel()
{
    m_phase = Phase::Idle;
    m_positions.clear();
    m_index = 0;
    m_settleFrames = 0;
    m_scoreSum = 0.0;
    m_scoreCount = 0;
    m_bestPosition = -1;
    m_bestScore = -1.0;
    m_staleFrames = 0;
}

void ContrastAutoFocus::focusOnce()
{
    if (!m_lens || !m_lens->isOpen())
        return;
    beginSweep();
}

void ContrastAutoFocus::beginSweep()
{
    const int minimum = m_lens->minimum();
    const int maximum = m_lens->maximum();

    m_positions.clear();
    m_coarseStep = std::max(1, (maximum - minimum) / (kCoarseSteps - 1));
    for (int i = 0; i < kCoarseSteps; ++i)
        m_positions.append(std::min(maximum, minimum + i * m_coarseStep));

    m_phase = Phase::Coarse;
    m_index = 0;
    m_bestPosition = -1;
    m_bestScore = -1.0;
    m_worstScore = -1.0;
    m_staleFrames = 0;
    m_startPosition = m_lens->lastRequested();

    emit statusChanged(QStringLiteral("Focusing..."));

    m_lens->setPosition(m_positions.first());
    m_settleFrames = kSettleFrames;
    m_scoreSum = 0.0;
    m_scoreCount = 0;
}

void ContrastAutoFocus::beginFinePass()
{
    const int minimum = m_lens->minimum();
    const int maximum = m_lens->maximum();

    // Search one coarse step either side of the winner, which is where the true
    // peak must lie if the coarse pass bracketed it.
    const int from = std::max(minimum, m_bestPosition - m_coarseStep);
    const int to   = std::min(maximum, m_bestPosition + m_coarseStep);
    const int step = std::max(1, (to - from) / (kFineSteps - 1));

    m_positions.clear();
    for (int position = from; position <= to; position += step)
        m_positions.append(position);

    if (m_positions.size() < 2) {
        parkAt(m_bestPosition);
        return;
    }

    m_phase = Phase::Fine;
    m_index = 0;
    m_bestPosition = -1;
    m_bestScore = -1.0;

    m_lens->setPosition(m_positions.first());
    m_settleFrames = kSettleFrames;
    m_scoreSum = 0.0;
    m_scoreCount = 0;
}

void ContrastAutoFocus::advance()
{
    ++m_index;

    if (m_index < m_positions.size()) {
        m_lens->setPosition(m_positions.at(m_index));
        m_settleFrames = kSettleFrames;
        m_scoreSum = 0.0;
        m_scoreCount = 0;
        return;
    }

    if (m_phase == Phase::Coarse) {
        // Refuse to act on a curve with no real peak in it.
        const bool prominent = m_bestScore > 0.0 && m_worstScore > 0.0
                               && m_bestScore >= m_worstScore * kMinPeakProminence;
        if (!prominent) {
            if (m_startPosition >= 0)
                m_lens->setPosition(m_startPosition);
            m_phase = m_continuous ? Phase::Settled : Phase::Idle;
            m_positions.clear();
            m_settleFrames = kSettleFrames;
            m_staleFrames = 0;
            m_parkedScore = -1.0;
            emit statusChanged(QStringLiteral("Not enough detail to focus on"));
            emit finished(m_startPosition);
            return;
        }
        beginFinePass();
        return;
    }

    parkAt(m_bestPosition);
}

void ContrastAutoFocus::parkAt(int position)
{
    if (position >= 0)
        m_lens->setPosition(position);

    m_parkedScore = m_bestScore;
    m_phase = m_continuous ? Phase::Settled : Phase::Idle;
    m_positions.clear();
    m_settleFrames = kSettleFrames;
    m_staleFrames = 0;

    emit statusChanged(position >= 0
                           ? QStringLiteral("Focused at %1").arg(position)
                           : QStringLiteral("Focus search found no peak"));
    emit finished(position);
}

void ContrastAutoFocus::onFrame(const QImage& frame)
{
    if (!m_lens || !m_lens->isOpen() || m_phase == Phase::Idle)
        return;

    if (m_settleFrames > 0) {
        --m_settleFrames;
        return;
    }

    const double score = sharpness(frame);
    if (score < 0.0)
        return;

    if (m_phase == Phase::Settled) {
        // Watching for the scene to change enough to be worth refocusing.
        if (m_parkedScore > 0.0 && score < m_parkedScore * kRefocusRatio) {
            if (++m_staleFrames >= kStaleFramesNeeded)
                beginSweep();
        } else {
            m_staleFrames = 0;
        }
        return;
    }

    m_scoreSum += score;
    if (++m_scoreCount < kScoreFrames)
        return;

    const double averaged = m_scoreSum / m_scoreCount;
    if (m_worstScore < 0.0 || averaged < m_worstScore)
        m_worstScore = averaged;
    if (averaged > m_bestScore) {
        m_bestScore = averaged;
        m_bestPosition = m_positions.at(m_index);
    }

    advance();
}

double ContrastAutoFocus::sharpness(const QImage& frame)
{
    if (frame.isNull() || frame.width() < 16 || frame.height() < 16)
        return -1.0;

    const QImage image = frame.convertToFormat(QImage::Format_RGB32);

    // Score the middle third only. Focusing on whatever fills the centre is what
    // people expect, and it keeps a busy background from winning the search.
    const int left   = image.width() / 3;
    const int right  = image.width() - left;
    const int top    = image.height() / 3;
    const int bottom = image.height() - top;

    double total = 0.0;
    quint64 samples = 0;

    for (int y = top + 1; y < bottom; y += 2) {
        const auto* row  = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        const auto* prev = reinterpret_cast<const QRgb*>(image.constScanLine(y - 1));

        for (int x = left + 1; x < right; x += 2) {
            const int luma = qGray(row[x]);
            total += std::abs(luma - qGray(row[x - 1]))
                   + std::abs(luma - qGray(prev[x]));
            ++samples;
        }
    }

    return samples ? total / double(samples) : -1.0;
}
