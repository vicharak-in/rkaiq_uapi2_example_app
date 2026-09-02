#pragma once

#include <QImage>
#include <QMutex>
#include <QString>
#include <QThread>
#include <QVector>

#include <atomic>

// V4L2 mmap capture on an ISP capture node.
//
// Everything negotiable is negotiated at runtime: the buffer type comes from
// VIDIOC_QUERYCAP (single-plane vs multiplanar) and the pixel format is chosen
// from VIDIOC_ENUM_FMT, so the same code works on a different node or SoC.
class V4l2Capture : public QThread
{
    Q_OBJECT

public:
    explicit V4l2Capture(QObject* parent = nullptr);
    ~V4l2Capture() override;

    // Opens the node, negotiates format, maps buffers and starts streaming.
    // Call after the rkaiq engine has been started; the ordering matters.
    bool open(const QString& devicePath, int width, int height);
    void close();

    void stopCapture();

    int width() const { return m_width; }
    int height() const { return m_height; }
    QString pixelFormatName() const { return m_formatName; }
    QString lastError() const;

signals:
    void frameReady(const QImage& frame);
    void errorOccurred(const QString& message);

protected:
    void run() override;

private:
    struct Buffer {
        void* start  = nullptr;
        size_t length = 0;
        int dmaFd    = -1;   // from VIDIOC_EXPBUF; the zero-copy upgrade path
    };

    bool queryCapabilities();
    bool negotiateFormat(int width, int height);
    bool mapBuffers();
    bool startStreaming();
    void stopStreaming();
    void unmapBuffers();
    void setError(const QString& message);

    // Converts whichever format was negotiated into the RGB image the UI paints.
    bool convertToImage(const Buffer& buffer, QImage& out) const;

    int m_fd = -1;
    QString m_devicePath;
    QString m_formatName;

    quint32 m_bufferType   = 0;
    quint32 m_pixelFormat  = 0;
    bool m_multiplanar     = false;
    int m_width  = 0;
    int m_height = 0;
    int m_bytesPerLine = 0;

    QVector<Buffer> m_buffers;
    std::atomic_bool m_stopRequested { false };
    std::atomic_bool m_streaming { false };

    mutable QMutex m_errorMutex;
    QString m_lastError;

    QImage m_scratch;   // reused so each frame does not reallocate
};
