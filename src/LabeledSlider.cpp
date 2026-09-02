#include "LabeledSlider.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QTimer>

#include <cmath>

namespace {
// rkaiq setters land on the next ISP frame; ~50 ms of coalescing keeps a drag
// from queueing hundreds of calls without feeling laggy.
constexpr int kCoalesceMs = 50;
constexpr int kLabelWidth = 120;
}

LabeledSlider::LabeledSlider(const QString& label, QWidget* parent)
    : QWidget(parent)
{
    m_label = new QLabel(label, this);
    m_label->setMinimumWidth(kLabelWidth);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setMinimumWidth(120);

    m_spinBox = new QDoubleSpinBox(this);
    m_spinBox->setDecimals(0);
    m_spinBox->setKeyboardTracking(false);
    m_spinBox->setMinimumWidth(90);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_label);
    layout->addWidget(m_slider, 1);
    layout->addWidget(m_spinBox);

    m_coalesceTimer = new QTimer(this);
    m_coalesceTimer->setSingleShot(true);
    m_coalesceTimer->setInterval(kCoalesceMs);

    connect(m_slider, &QSlider::valueChanged, this, &LabeledSlider::onSliderMoved);
    connect(m_spinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &LabeledSlider::onSpinBoxChanged);
    connect(m_coalesceTimer, &QTimer::timeout, this, &LabeledSlider::emitPendingChange);

    setRange(0.0, 100.0);
}

void LabeledSlider::setDecimals(int decimals)
{
    m_decimals = qBound(0, decimals, 4);
    m_scale = std::pow(10.0, m_decimals);
    m_spinBox->setDecimals(m_decimals);

    // Re-apply the range so the slider's integer units track the new precision.
    setRange(m_spinBox->minimum(), m_spinBox->maximum());
}

void LabeledSlider::setRange(double minimum, double maximum)
{
    if (maximum < minimum)
        std::swap(minimum, maximum);

    const bool wasUpdating = m_updating;
    m_updating = true;

    m_spinBox->setRange(minimum, maximum);
    m_slider->setRange(toSliderUnits(minimum), toSliderUnits(maximum));

    m_updating = wasUpdating;
}

void LabeledSlider::setSingleStep(double step)
{
    m_spinBox->setSingleStep(step);
}

void LabeledSlider::setSuffix(const QString& suffix)
{
    m_spinBox->setSuffix(suffix);
}

void LabeledSlider::setLabel(const QString& label)
{
    m_label->setText(label);
}

int LabeledSlider::toSliderUnits(double value) const
{
    return int(std::lround(value * m_scale));
}

double LabeledSlider::fromSliderUnits(int position) const
{
    return double(position) / m_scale;
}

double LabeledSlider::value() const
{
    return m_spinBox->value();
}

void LabeledSlider::setValueSilently(double value)
{
    m_updating = true;
    m_spinBox->setValue(value);
    m_slider->setValue(toSliderUnits(value));
    m_updating = false;

    // Seeding must not look like user input, so any queued emit is dropped.
    m_coalesceTimer->stop();
}

void LabeledSlider::onSliderMoved(int position)
{
    if (m_updating)
        return;

    m_updating = true;
    const double value = fromSliderUnits(position);
    m_spinBox->setValue(value);
    m_updating = false;

    m_pendingValue = value;
    m_coalesceTimer->start();
}

void LabeledSlider::onSpinBoxChanged(double value)
{
    if (m_updating)
        return;

    m_updating = true;
    m_slider->setValue(toSliderUnits(value));
    m_updating = false;

    m_pendingValue = value;
    m_coalesceTimer->start();
}

void LabeledSlider::emitPendingChange()
{
    emit valueChanged(m_pendingValue);
}
