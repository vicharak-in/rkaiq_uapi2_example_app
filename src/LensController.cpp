#include "LensController.h"

#include <algorithm>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

int xioctl(int fd, unsigned long request, void* arg)
{
    int r;
    do {
        r = ::ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

} // namespace

LensController::~LensController()
{
    close();
}

bool LensController::open(const QString& subdevPath)
{
    // A different motor means the remembered position is meaningless, but
    // reopening the same one (which happens on every engine restart) must keep
    // it, or an exposure change would quietly send the lens back to its default.
    const bool sameDevice = (subdevPath == m_path);
    close();
    if (!sameDevice)
        m_lastRequested = -1;

    if (subdevPath.isEmpty()) {
        m_lastError = QStringLiteral("No focus motor on this camera");
        return false;
    }

    const int fd = ::open(subdevPath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        m_lastError = QStringLiteral("Cannot open %1: %2")
                          .arg(subdevPath, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    v4l2_queryctrl query {};
    query.id = V4L2_CID_FOCUS_ABSOLUTE;
    if (xioctl(fd, VIDIOC_QUERYCTRL, &query) != 0 || (query.flags & V4L2_CTRL_FLAG_DISABLED)) {
        m_lastError = QStringLiteral("%1 does not support focus control").arg(subdevPath);
        ::close(fd);
        return false;
    }

    if (query.maximum <= query.minimum) {
        m_lastError = QStringLiteral("%1 reported an unusable focus range").arg(subdevPath);
        ::close(fd);
        return false;
    }

    m_fd = fd;
    m_path = subdevPath;
    m_minimum = query.minimum;
    m_maximum = query.maximum;
    return true;
}

void LensController::close()
{
    if (m_fd >= 0)
        ::close(m_fd);
    m_fd = -1;
    m_minimum = 0;
    m_maximum = 0;
    // m_path and m_lastRequested deliberately survive, so reopening the same
    // motor can restore where it was pointing.
}

bool LensController::setPosition(int code)
{
    if (m_fd < 0)
        return false;

    const int clamped = std::clamp(code, m_minimum, m_maximum);

    v4l2_control control {};
    control.id = V4L2_CID_FOCUS_ABSOLUTE;
    control.value = clamped;
    if (xioctl(m_fd, VIDIOC_S_CTRL, &control) != 0) {
        m_lastError = QStringLiteral("Focus move to %1 failed: %2")
                          .arg(clamped)
                          .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    m_lastRequested = clamped;
    return true;
}

int LensController::position() const
{
    if (m_fd < 0)
        return -1;

    v4l2_control control {};
    control.id = V4L2_CID_FOCUS_ABSOLUTE;
    if (xioctl(m_fd, VIDIOC_G_CTRL, &control) != 0)
        return -1;

    return control.value;
}
