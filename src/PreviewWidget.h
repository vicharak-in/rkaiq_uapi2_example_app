#pragma once

#include <QImage>
#include <QWidget>

// Paints the most recent camera frame, letterboxed to preserve aspect ratio.
class PreviewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    // Frames per second as actually delivered, for the status bar.
    double measuredFps() const { return m_measuredFps; }

public slots:
    void setFrame(const QImage& frame);
    void clear();
    void setPlaceholderText(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage m_frame;
    QString m_placeholder;

    // Frame-interval smoothing for the fps readout.
    qint64 m_lastFrameMs = 0;
    double m_measuredFps = 0.0;
};
