#pragma once

#include <QWidget>

class QLabel;
class QSlider;
class QDoubleSpinBox;
class QTimer;

// A labelled slider paired with a spin box, kept in sync.
//
// Values are doubles with a configurable decimal count; integer controls simply
// use zero decimals. Changes are coalesced through a short timer because the
// rkaiq setters take effect on the next ISP frame, and firing on every pixel of
// a drag would flood the engine.
class LabeledSlider : public QWidget
{
    Q_OBJECT

public:
    explicit LabeledSlider(const QString& label, QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    void setDecimals(int decimals);
    void setSingleStep(double step);
    void setSuffix(const QString& suffix);
    void setLabel(const QString& label);

    double value() const;
    // Updates the widgets without emitting valueChanged, for seeding controls
    // from the engine's current state.
    void setValueSilently(double value);

signals:
    void valueChanged(double value);

private slots:
    void onSliderMoved(int position);
    void onSpinBoxChanged(double value);
    void emitPendingChange();

private:
    int toSliderUnits(double value) const;
    double fromSliderUnits(int position) const;

    QLabel* m_label = nullptr;
    QSlider* m_slider = nullptr;
    QDoubleSpinBox* m_spinBox = nullptr;
    QTimer* m_coalesceTimer = nullptr;

    int m_decimals = 0;
    double m_scale = 1.0;   // slider works in integers; this maps to real units
    bool m_updating = false;
    double m_pendingValue = 0.0;
};
