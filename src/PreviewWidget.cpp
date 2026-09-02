#include "PreviewWidget.h"

#include <QDateTime>
#include <QPainter>

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent)
    , m_placeholder(QStringLiteral("No signal"))
{
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // The widget always paints its own background, so Qt need not clear it first.
    setAttribute(Qt::WA_OpaquePaintEvent);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setAutoFillBackground(true);
    setPalette(pal);
}

QSize PreviewWidget::sizeHint() const
{
    return QSize(960, 540);
}

void PreviewWidget::setFrame(const QImage& frame)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastFrameMs > 0) {
        const qint64 delta = now - m_lastFrameMs;
        if (delta > 0) {
            const double instant = 1000.0 / double(delta);
            // Exponential smoothing keeps the readout steady enough to read.
            m_measuredFps = (m_measuredFps > 0.0) ? (m_measuredFps * 0.9 + instant * 0.1)
                                                  : instant;
        }
    }
    m_lastFrameMs = now;

    m_frame = frame;
    update();
}

void PreviewWidget::clear()
{
    m_frame = QImage();
    m_lastFrameMs = 0;
    m_measuredFps = 0.0;
    update();
}

void PreviewWidget::setPlaceholderText(const QString& text)
{
    m_placeholder = text;
    if (m_frame.isNull())
        update();
}

void PreviewWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (m_frame.isNull()) {
        painter.setPen(QColor(190, 190, 190));
        // Placeholders carry real error text, which can be long, so it wraps
        // and is inset from the edges rather than being clipped.
        const QRect textArea = rect().adjusted(24, 24, -24, -24);
        painter.drawText(textArea, Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        return;
    }

    // Letterbox: scale to fit, centred, without distorting the image.
    const QSize scaled = m_frame.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2,
                              (height() - scaled.height()) / 2),
                       scaled);

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, m_frame);
}
