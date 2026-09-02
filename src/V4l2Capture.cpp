#include "V4l2Capture.h"

#include <QDebug>
#include <QMutexLocker>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr int kBufferCount = 4;
constexpr int kPollTimeoutMs = 2000;

int xioctl(int fd, unsigned long request, void* arg)
{
    int r;
    do {
        r = ::ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

QString fourccName(quint32 fourcc)
{
    char text[5] = { char(fourcc & 0xff), char((fourcc >> 8) & 0xff),
                     char((fourcc >> 16) & 0xff), char((fourcc >> 24) & 0xff), '\0' };
    return QString::fromLatin1(text);
}

// Preference order: the semi-planar YUV formats the ISP emits, best first.
// Chosen against what the node actually enumerates rather than assumed.
const quint32 kPreferredFormats[] = {
    V4L2_PIX_FMT_NV12,
    V4L2_PIX_FMT_NV21,
    V4L2_PIX_FMT_NV16,
    V4L2_PIX_FMT_NV61,
    V4L2_PIX_FMT_UYVY,
    V4L2_PIX_FMT_YUYV,
};

inline uchar clampToByte(int value)
{
    return static_cast<uchar>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// BT.601 full-range YUV -> RGB, matching the ISP's default quantisation.
inline void yuvToRgb(int y, int u, int v, uchar* rgb)
{
    const int c = y;
    const int d = u - 128;
    const int e = v - 128;

    rgb[0] = clampToByte((256 * c + 359 * e + 128) >> 8);
    rgb[1] = clampToByte((256 * c -  88 * d - 183 * e + 128) >> 8);
    rgb[2] = clampToByte((256 * c + 454 * d + 128) >> 8);
}

} // namespace

V4l2Capture::V4l2Capture(QObject* parent)
    : QThread(parent)
{
}

V4l2Capture::~V4l2Capture()
{
    stopCapture();
    close();
}

void V4l2Capture::setError(const QString& message)
{
    {
        QMutexLocker locker(&m_errorMutex);
        m_lastError = message;
    }
    qWarning() << "V4l2Capture:" << message;
    emit errorOccurred(message);
}

QString V4l2Capture::lastError() const
{
    QMutexLocker locker(&m_errorMutex);
    return m_lastError;
}

bool V4l2Capture::queryCapabilities()
{
    v4l2_capability cap {};
    if (xioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        setError(QStringLiteral("VIDIOC_QUERYCAP failed on %1: %2")
                     .arg(m_devicePath, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    const quint32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                   : cap.capabilities;

    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        m_multiplanar = true;
        m_bufferType  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        m_multiplanar = false;
        m_bufferType  = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else {
        setError(QStringLiteral("%1 is not a video capture device").arg(m_devicePath));
        return false;
    }

    if (!(caps & V4L2_CAP_STREAMING)) {
        setError(QStringLiteral("%1 does not support streaming I/O").arg(m_devicePath));
        return false;
    }

    return true;
}

bool V4l2Capture::negotiateFormat(int width, int height)
{
    // Ask the node what it offers, then take the best match from our preference
    // list. Never assume NV12 is present.
    QVector<quint32> available;
    for (quint32 index = 0;; ++index) {
        v4l2_fmtdesc desc {};
        desc.index = index;
        desc.type  = m_bufferType;
        if (xioctl(m_fd, VIDIOC_ENUM_FMT, &desc) < 0)
            break;
        available.append(desc.pixelformat);
    }

    if (available.isEmpty()) {
        setError(QStringLiteral("%1 enumerated no pixel formats").arg(m_devicePath));
        return false;
    }

    quint32 chosen = 0;
    for (quint32 candidate : kPreferredFormats) {
        if (available.contains(candidate)) {
            chosen = candidate;
            break;
        }
    }
    if (chosen == 0) {
        setError(QStringLiteral("%1 offers no format this app can convert (saw %2)")
                     .arg(m_devicePath, fourccName(available.first())));
        return false;
    }

    v4l2_format fmt {};
    fmt.type = m_bufferType;

    if (m_multiplanar) {
        fmt.fmt.pix_mp.width       = width;
        fmt.fmt.pix_mp.height      = height;
        fmt.fmt.pix_mp.pixelformat = chosen;
        fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    } else {
        fmt.fmt.pix.width       = width;
        fmt.fmt.pix.height      = height;
        fmt.fmt.pix.pixelformat = chosen;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    }

    if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        setError(QStringLiteral("VIDIOC_S_FMT %1x%2 %3 failed: %4")
                     .arg(width).arg(height).arg(fourccName(chosen),
                          QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    // The driver may have adjusted the request; take what it actually set.
    if (m_multiplanar) {
        m_width        = fmt.fmt.pix_mp.width;
        m_height       = fmt.fmt.pix_mp.height;
        m_pixelFormat  = fmt.fmt.pix_mp.pixelformat;
        m_bytesPerLine = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    } else {
        m_width        = fmt.fmt.pix.width;
        m_height       = fmt.fmt.pix.height;
        m_pixelFormat  = fmt.fmt.pix.pixelformat;
        m_bytesPerLine = fmt.fmt.pix.bytesperline;
    }

    if (m_bytesPerLine <= 0)
        m_bytesPerLine = m_width;

    m_formatName = fourccName(m_pixelFormat);

    if (m_width != width || m_height != height) {
        qWarning() << "V4l2Capture: driver adjusted" << width << "x" << height
                   << "to" << m_width << "x" << m_height;
    }

    return true;
}

bool V4l2Capture::mapBuffers()
{
    v4l2_requestbuffers req {};
    req.count  = kBufferCount;
    req.type   = m_bufferType;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        setError(QStringLiteral("VIDIOC_REQBUFS failed: %1")
                     .arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    if (req.count < 2) {
        setError(QStringLiteral("Insufficient buffer memory on %1").arg(m_devicePath));
        return false;
    }

    for (quint32 i = 0; i < req.count; ++i) {
        v4l2_buffer buf {};
        v4l2_plane planes[VIDEO_MAX_PLANES] {};

        buf.type   = m_bufferType;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (m_multiplanar) {
            buf.m.planes = planes;
            buf.length   = VIDEO_MAX_PLANES;
        }

        if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            setError(QStringLiteral("VIDIOC_QUERYBUF %1 failed: %2")
                         .arg(i).arg(QString::fromLocal8Bit(strerror(errno))));
            return false;
        }

        Buffer buffer;
        const size_t length = m_multiplanar ? buf.m.planes[0].length : buf.length;
        const off_t offset  = m_multiplanar ? buf.m.planes[0].m.mem_offset : buf.m.offset;

        buffer.length = length;
        buffer.start  = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED,
                               m_fd, offset);

        if (buffer.start == MAP_FAILED) {
            setError(QStringLiteral("mmap of buffer %1 failed: %2")
                         .arg(i).arg(QString::fromLocal8Bit(strerror(errno))));
            buffer.start = nullptr;
            return false;
        }

        // Also export a dmabuf fd. Unused today, but it is what a future
        // zero-copy EGL preview path would consume.
        v4l2_exportbuffer expbuf {};
        expbuf.type  = m_bufferType;
        expbuf.index = i;
        expbuf.flags = O_CLOEXEC;
        if (xioctl(m_fd, VIDIOC_EXPBUF, &expbuf) == 0)
            buffer.dmaFd = expbuf.fd;

        m_buffers.append(buffer);
    }

    return true;
}

bool V4l2Capture::startStreaming()
{
    for (int i = 0; i < m_buffers.size(); ++i) {
        v4l2_buffer buf {};
        v4l2_plane planes[VIDEO_MAX_PLANES] {};

        buf.type   = m_bufferType;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (m_multiplanar) {
            buf.m.planes = planes;
            buf.length   = 1;
        }

        if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            setError(QStringLiteral("VIDIOC_QBUF %1 failed: %2")
                         .arg(i).arg(QString::fromLocal8Bit(strerror(errno))));
            return false;
        }
    }

    int type = static_cast<int>(m_bufferType);
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        setError(QStringLiteral("VIDIOC_STREAMON failed: %1")
                     .arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    m_streaming = true;
    return true;
}

void V4l2Capture::stopStreaming()
{
    if (m_fd < 0 || !m_streaming)
        return;

    int type = static_cast<int>(m_bufferType);
    if (xioctl(m_fd, VIDIOC_STREAMOFF, &type) < 0)
        qWarning() << "V4l2Capture: VIDIOC_STREAMOFF failed:" << strerror(errno);

    m_streaming = false;
}

void V4l2Capture::unmapBuffers()
{
    for (Buffer& buffer : m_buffers) {
        if (buffer.start)
            ::munmap(buffer.start, buffer.length);
        if (buffer.dmaFd >= 0)
            ::close(buffer.dmaFd);
    }
    m_buffers.clear();
}

bool V4l2Capture::open(const QString& devicePath, int width, int height)
{
    close();

    m_devicePath = devicePath;
    m_stopRequested = false;

    // Non-blocking so the capture loop polls and stays responsive to stop
    // requests, rather than parking indefinitely in DQBUF.
    m_fd = ::open(devicePath.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0) {
        setError(QStringLiteral("Cannot open %1: %2")
                     .arg(devicePath, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    if (!queryCapabilities() || !negotiateFormat(width, height) || !mapBuffers()
        || !startStreaming()) {
        close();
        return false;
    }

    return true;
}

void V4l2Capture::close()
{
    stopStreaming();
    unmapBuffers();

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }

    m_width = m_height = 0;
    m_streaming = false;
}

void V4l2Capture::stopCapture()
{
    m_stopRequested = true;
    if (isRunning()) {
        // Poll timeout bounds how long the loop can be mid-wait.
        if (!wait(kPollTimeoutMs + 1000)) {
            qWarning() << "V4l2Capture: capture thread did not stop cleanly; terminating";
            terminate();
            wait(500);
        }
    }
}

void V4l2Capture::run()
{
    while (!m_stopRequested) {
        pollfd pfd {};
        pfd.fd     = m_fd;
        pfd.events = POLLIN;

        const int ready = ::poll(&pfd, 1, kPollTimeoutMs);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            setError(QStringLiteral("poll failed: %1")
                         .arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }
        if (ready == 0)
            continue;   // timeout; loop back and re-check the stop flag

        v4l2_buffer buf {};
        v4l2_plane planes[VIDEO_MAX_PLANES] {};

        buf.type   = m_bufferType;
        buf.memory = V4L2_MEMORY_MMAP;
        if (m_multiplanar) {
            buf.m.planes = planes;
            buf.length   = VIDEO_MAX_PLANES;
        }

        if (xioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN)
                continue;
            setError(QStringLiteral("VIDIOC_DQBUF failed: %1")
                         .arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }

        if (static_cast<int>(buf.index) < m_buffers.size()) {
            if (convertToImage(m_buffers[buf.index], m_scratch)) {
                // Deep copy: the mapped buffer is requeued immediately below,
                // so the consumer must not be looking at it any more.
                emit frameReady(m_scratch.copy());
            }
        }

        // Requeue at once so the driver is never starved of buffers.
        if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0) {
            setError(QStringLiteral("VIDIOC_QBUF failed: %1")
                         .arg(QString::fromLocal8Bit(strerror(errno))));
            return;
        }
    }
}

bool V4l2Capture::convertToImage(const Buffer& buffer, QImage& out) const
{
    if (!buffer.start || m_width <= 0 || m_height <= 0)
        return false;

    if (out.width() != m_width || out.height() != m_height
        || out.format() != QImage::Format_RGB888) {
        out = QImage(m_width, m_height, QImage::Format_RGB888);
    }

    const uchar* src = static_cast<const uchar*>(buffer.start);
    const int stride = m_bytesPerLine;

    switch (m_pixelFormat) {
    case V4L2_PIX_FMT_NV12:
    case V4L2_PIX_FMT_NV21:
    case V4L2_PIX_FMT_NV16:
    case V4L2_PIX_FMT_NV61: {
        // Semi-planar: a full luma plane followed by interleaved chroma.
        // 4:2:0 (NV12/NV21) subsamples chroma vertically, 4:2:2 does not.
        const bool is420  = (m_pixelFormat == V4L2_PIX_FMT_NV12
                             || m_pixelFormat == V4L2_PIX_FMT_NV21);
        const bool vFirst = (m_pixelFormat == V4L2_PIX_FMT_NV21
                             || m_pixelFormat == V4L2_PIX_FMT_NV61);

        const uchar* luma   = src;
        const uchar* chroma = src + static_cast<size_t>(stride) * m_height;

        const size_t needed = static_cast<size_t>(stride) * m_height
                              + (is420 ? static_cast<size_t>(stride) * m_height / 2
                                       : static_cast<size_t>(stride) * m_height);
        if (buffer.length < needed)
            return false;

        for (int y = 0; y < m_height; ++y) {
            const uchar* lumaRow   = luma + static_cast<size_t>(y) * stride;
            const int chromaRow    = is420 ? (y / 2) : y;
            const uchar* chromaPtr = chroma + static_cast<size_t>(chromaRow) * stride;
            uchar* dst             = out.scanLine(y);

            for (int x = 0; x < m_width; ++x) {
                const int pair = (x / 2) * 2;
                const int u = vFirst ? chromaPtr[pair + 1] : chromaPtr[pair];
                const int v = vFirst ? chromaPtr[pair]     : chromaPtr[pair + 1];
                yuvToRgb(lumaRow[x], u, v, dst + x * 3);
            }
        }
        return true;
    }

    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_YUYV: {
        // Packed 4:2:2: two pixels share a chroma pair.
        const bool uyvy = (m_pixelFormat == V4L2_PIX_FMT_UYVY);
        if (buffer.length < static_cast<size_t>(stride) * m_height)
            return false;

        for (int y = 0; y < m_height; ++y) {
            const uchar* row = src + static_cast<size_t>(y) * stride;
            uchar* dst       = out.scanLine(y);

            for (int x = 0; x < m_width; x += 2) {
                const uchar* quad = row + x * 2;
                const int y0 = uyvy ? quad[1] : quad[0];
                const int u  = uyvy ? quad[0] : quad[1];
                const int y1 = uyvy ? quad[3] : quad[2];
                const int v  = uyvy ? quad[2] : quad[3];

                yuvToRgb(y0, u, v, dst + x * 3);
                if (x + 1 < m_width)
                    yuvToRgb(y1, u, v, dst + (x + 1) * 3);
            }
        }
        return true;
    }

    default:
        return false;
    }
}
