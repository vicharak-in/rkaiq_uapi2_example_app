#include "CameraEnumerator.h"

#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {
#include "rk_aiq_user_api2_sysctl.h"
#include "rk-camera-module.h"
}

namespace {

QString g_lastError;

// Retry an ioctl across EINTR, the way rkisp_demo's xioctl() does.
int xioctl(int fd, unsigned long request, void* arg)
{
    int r;
    do {
        r = ::ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

// Entity names look like "m00_b_ov5647 2-0036"; the readable part is the
// sensor model between the fixed "mNN_x_" prefix and the I2C address.
QString processNameFor(const QString& pid)
{
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!comm.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("pid %1").arg(pid);
    return QString::fromLatin1(comm.readAll()).trimmed();
}

QString sensorNameFromEntity(const QString& entity)
{
    static const QRegularExpression re(QStringLiteral("^m\\d+_[a-zA-Z]_([^ ]+)"));
    const auto match = re.match(entity);
    if (match.hasMatch())
        return match.captured(1);

    return entity.section(QLatin1Char(' '), 0, 0);
}

} // namespace

QString CameraMode::label() const
{
    QString text = QStringLiteral("%1x%2").arg(width).arg(height);
    if (fps > 0)
        text += QStringLiteral(" @ %1 fps").arg(fps);
    if (isHdr())
        text += (hdrMode == HDR_X3) ? QStringLiteral(" HDR3") : QStringLiteral(" HDR2");
    return text;
}

bool CameraMode::isHdr() const
{
    return hdrMode == HDR_X2 || hdrMode == HDR_X3;
}

bool CameraInfo::supportsHdr() const
{
    for (const CameraMode& mode : modes) {
        if (mode.isHdr())
            return true;
    }
    return false;
}

QVector<QPair<int, int>> CameraInfo::resolutions() const
{
    QVector<QPair<int, int>> out;
    for (const CameraMode& mode : modes) {
        const QPair<int, int> res(mode.width, mode.height);
        if (!out.contains(res))
            out.append(res);
    }
    return out;
}

QVector<int> CameraInfo::fpsFor(int width, int height) const
{
    QVector<int> out;
    for (const CameraMode& mode : modes) {
        if (mode.width == width && mode.height == height && mode.fps > 0
            && !out.contains(mode.fps)) {
            out.append(mode.fps);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

QVector<VideoNode> CameraEnumerator::enumerateVideoNodes()
{
    QVector<VideoNode> nodes;

    QDir dev(QStringLiteral("/dev"));
    QStringList entries = dev.entryList({QStringLiteral("video*")}, QDir::System);

    // "video10" must sort after "video9", so compare the numeric suffix.
    std::sort(entries.begin(), entries.end(), [](const QString& a, const QString& b) {
        return a.mid(5).toInt() < b.mid(5).toInt();
    });

    for (const QString& entry : entries) {
        const QString path = QStringLiteral("/dev/") + entry;

        const int fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        v4l2_capability cap {};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            const quint32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                                     ? cap.device_caps
                                     : cap.capabilities;

            const bool mplane = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE;
            const bool splane = caps & V4L2_CAP_VIDEO_CAPTURE;
            // The ISP statistics and input-params nodes are metadata devices;
            // they are not capture candidates but are needed to spot a
            // conflicting engine owner.
            const bool meta = caps & (V4L2_CAP_META_CAPTURE | V4L2_CAP_META_OUTPUT);

            if (mplane || splane || meta) {
                VideoNode node;
                node.path           = path;
                node.card           = QString::fromLatin1(reinterpret_cast<const char*>(cap.card));
                node.driver         = QString::fromLatin1(reinterpret_cast<const char*>(cap.driver));
                node.busInfo        = QString::fromLatin1(reinterpret_cast<const char*>(cap.bus_info));
                node.isMultiplanar  = mplane;
                node.isMetaCapture  = meta && !(mplane || splane);
                nodes.append(node);
            }
        }
        ::close(fd);
    }

    return nodes;
}

QString CameraEnumerator::subdevForEntity(const QString& entity)
{
    // Subdev numbering is no more stable than /dev/videoN, so the entity name
    // is matched rather than the node index.
    QDir sysfs(QStringLiteral("/sys/class/video4linux"));
    const auto entries = sysfs.entryList(QStringList{QStringLiteral("v4l-subdev*")},
                                         QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& entry : entries) {
        QFile nameFile(sysfs.filePath(entry + QStringLiteral("/name")));
        if (!nameFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        if (QString::fromLatin1(nameFile.readAll()).trimmed() == entity)
            return QStringLiteral("/dev/") + entry;
    }
    return {};
}

QVector<CameraMode> CameraEnumerator::enumerateSubdevModes(const QString& subdevPath)
{
    QVector<CameraMode> modes;
    if (subdevPath.isEmpty())
        return modes;

    const int fd = ::open(subdevPath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return modes;

    // The sensor's current mode is the one interval reliably readable, since
    // ENUM_FRAME_INTERVAL is exactly what these drivers omit. Where it matches
    // an enumerated size, that size gets a real frame rate instead of none.
    int currentWidth = 0, currentHeight = 0, currentFps = 0;

    v4l2_subdev_format fmt {};
    fmt.pad   = 0;
    fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    if (xioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt) == 0) {
        currentWidth  = int(fmt.format.width);
        currentHeight = int(fmt.format.height);

        v4l2_subdev_frame_interval interval {};
        interval.pad = 0;
        if (xioctl(fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &interval) == 0
            && interval.interval.numerator > 0) {
            currentFps = int(interval.interval.denominator / interval.interval.numerator);
        }
    }

    v4l2_subdev_mbus_code_enum code {};
    code.pad   = 0;
    code.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    for (; xioctl(fd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &code) == 0; ++code.index) {
        v4l2_subdev_frame_size_enum size {};
        size.pad   = 0;
        size.code  = code.code;
        size.which = V4L2_SUBDEV_FORMAT_ACTIVE;

        for (; xioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &size) == 0; ++size.index) {
            const int width  = int(size.max_width);
            const int height = int(size.max_height);
            if (width <= 0 || height <= 0)
                continue;

            bool haveInterval = false;
            v4l2_subdev_frame_interval_enum fie {};
            fie.pad    = 0;
            fie.code   = code.code;
            fie.width  = size.max_width;
            fie.height = size.max_height;
            fie.which  = V4L2_SUBDEV_FORMAT_ACTIVE;

            for (; xioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL, &fie) == 0; ++fie.index) {
                if (fie.interval.numerator == 0)
                    continue;
                CameraMode mode;
                mode.width  = width;
                mode.height = height;
                mode.fps    = int(fie.interval.denominator / fie.interval.numerator);
                mode.hdrMode = NO_HDR;   // no rkmodule HDR config to ask
                modes.append(mode);
                haveInterval = true;
            }

            if (haveInterval)
                continue;

            CameraMode mode;
            mode.width   = width;
            mode.height  = height;
            mode.hdrMode = NO_HDR;
            // 0 means "leave the sensor at its own rate"; the UI shows it as
            // "Sensor default" and skips the setFrameRate() call.
            mode.fps = (width == currentWidth && height == currentHeight) ? currentFps : 0;
            modes.append(mode);
        }
    }

    ::close(fd);
    return modes;
}

QVector<CameraInfo> CameraEnumerator::enumerateCameras()
{
    g_lastError.clear();

    const QVector<VideoNode> nodes = enumerateVideoNodes();

    // The ISP main path is the full-resolution capture node. Self path is a
    // second, usually smaller stream; keep it only as a fallback.
    QVector<VideoNode> mainPaths;
    QVector<VideoNode> selfPaths;
    for (const VideoNode& node : nodes) {
        if (node.isMetaCapture)
            continue;
        if (node.card.compare(QStringLiteral("rkisp_mainpath"), Qt::CaseInsensitive) == 0)
            mainPaths.append(node);
        else if (node.card.compare(QStringLiteral("rkisp_selfpath"), Qt::CaseInsensitive) == 0)
            selfPaths.append(node);
    }

    if (mainPaths.isEmpty() && selfPaths.isEmpty()) {
        g_lastError = QStringLiteral(
            "No ISP capture node found. Scanned %1 video node(s) for an "
            "'rkisp_mainpath' device; the ISP driver may not be loaded.")
            .arg(nodes.size());
        return {};
    }

    QVector<VideoNode> candidates = mainPaths;
    candidates += selfPaths;

    QVector<CameraInfo> cameras;

    for (int index = 0; index < 16; ++index) {
        rk_aiq_static_info_t info {};
        if (rk_aiq_uapi2_sysctl_enumStaticMetas(index, &info) != XCAM_RETURN_NO_ERROR)
            break;

        const QString entity = QString::fromLatin1(info.sensor_info.sensor_name);
        if (entity.isEmpty())
            continue;

        CameraInfo camera;
        camera.sensorEntity = entity;
        camera.sensorName   = sensorNameFromEntity(entity);
        camera.hasVcm       = info.has_lens_vcm;
        camera.hasFlash     = info.has_fl;
        camera.hasIrCut     = info.has_irc;
        camera.ispHwVer     = info.isp_hw_ver;
        camera.phyId        = info.sensor_info.phyId;

        const int modeCount = qBound(0, info.sensor_info.num, SUPPORT_FMT_MAX);
        for (int i = 0; i < modeCount; ++i) {
            const rk_frame_fmt_t& fmt = info.sensor_info.support_fmt[i];
            if (fmt.width <= 0 || fmt.height <= 0)
                continue;

            CameraMode mode;
            mode.width   = fmt.width;
            mode.height  = fmt.height;
            mode.fps     = fmt.fps;
            mode.hdrMode = fmt.hdr_mode;
            camera.modes.append(mode);
        }

        camera.sensorSubdev = CameraEnumerator::subdevForEntity(entity);

        // rkaiq describes only sensors carrying Rockchip's rkmodule extensions.
        // For anything else its table comes back empty even though the driver
        // enumerates its modes perfectly well over plain V4L2, so ask the
        // sensor directly rather than declaring the camera unusable.
        if (camera.modes.isEmpty()) {
            camera.modes = CameraEnumerator::enumerateSubdevModes(camera.sensorSubdev);
            camera.modesFromSubdev = !camera.modes.isEmpty();
            if (camera.modesFromSubdev) {
                qInfo("rkaiq reported no modes for %s; read %d from %s instead",
                      qPrintable(camera.sensorName), camera.modes.size(),
                      qPrintable(camera.sensorSubdev));
            }
        }

        cameras.append(camera);
    }

    if (cameras.isEmpty()) {
        g_lastError = QStringLiteral(
            "rkaiq reports no cameras (enumStaticMetas found none). Check that a "
            "sensor is attached and its media links are enabled.");
        return {};
    }

    // Pair each sensor with its capture node. getBindedSnsEntNmByVd() is the
    // authoritative link, but it can come back NULL when the sensor and the ISP
    // live on different /dev/mediaN. Every result is therefore checked, and
    // unmatched cameras fall back to positional pairing below.
    QVector<bool> nodeTaken(candidates.size(), false);

    for (CameraInfo& camera : cameras) {
        for (int i = 0; i < candidates.size(); ++i) {
            if (nodeTaken[i])
                continue;

            const char* bound = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(
                candidates[i].path.toLocal8Bit().constData());
            if (!bound || !*bound)
                continue;

            if (camera.sensorEntity == QString::fromLatin1(bound)) {
                camera.videoNode = candidates[i].path;
                camera.busInfo   = candidates[i].busInfo;
                nodeTaken[i]     = true;
                break;
            }
        }
    }

    for (CameraInfo& camera : cameras) {
        if (!camera.videoNode.isEmpty())
            continue;

        for (int i = 0; i < candidates.size(); ++i) {
            if (nodeTaken[i])
                continue;

            qWarning() << "CameraEnumerator: no VD binding reported for"
                       << camera.sensorEntity << "- falling back to"
                       << candidates[i].path;
            camera.videoNode = candidates[i].path;
            camera.busInfo   = candidates[i].busInfo;
            nodeTaken[i]     = true;
            break;
        }
    }

    // A camera with no capture node cannot be previewed; drop it rather than
    // letting the pipeline fail later with a less obvious message.
    for (int i = cameras.size() - 1; i >= 0; --i) {
        if (cameras[i].videoNode.isEmpty()) {
            qWarning() << "CameraEnumerator: no capture node for"
                       << cameras[i].sensorEntity << "- skipping";
            cameras.removeAt(i);
        }
    }

    if (cameras.isEmpty())
        g_lastError = QStringLiteral("Found sensors, but none could be paired with a capture node.");

    return cameras;
}

QString CameraEnumerator::lastError()
{
    return g_lastError;
}

QString CameraEnumerator::iqFileNameFor(const QString& subdevPath)
{
    if (subdevPath.isEmpty())
        return {};

    const int fd = ::open(subdevPath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return {};

    rkmodule_inf info {};
    const bool ok = xioctl(fd, RKMODULE_GET_MODULE_INFO, &info) == 0;
    ::close(fd);
    if (!ok)
        return {};

    const QString sensor = QString::fromLatin1(info.base.sensor).trimmed();
    const QString module = QString::fromLatin1(info.base.module).trimmed();
    const QString lens   = QString::fromLatin1(info.base.lens).trimmed();
    if (sensor.isEmpty())
        return {};

    return QStringLiteral("%1_%2_%3.json").arg(sensor, module, lens);
}

QString CameraEnumerator::engineLockProblem(int phyId)
{
    const QString path = QStringLiteral("/tmp/aiq%1.lock").arg(qMax(0, phyId));
    if (!QFile::exists(path))
        return {};   // rkaiq creates it fresh, which always works

    if (::access(path.toLocal8Bit().constData(), W_OK) == 0)
        return {};

    const QFileInfo info(path);
    return QStringLiteral(
        "rkaiq's lock file %1 belongs to %2 and this process cannot write it, "
        "which makes the engine crash rather than report an error. It is a "
        "leftover, safe to delete: 'sudo rm %1'. Running the app consistently "
        "as one user avoids it.")
        .arg(path, info.owner().isEmpty() ? QStringLiteral("another user") : info.owner());
}

EngineOwner CameraEnumerator::findEngineOwner()
{
    EngineOwner owner;

    // The engine owner keeps the ISP statistics and input-params nodes open for
    // its whole lifetime, so whoever holds one of those is running an engine.
    QStringList ispNodes;
    for (const VideoNode& node : enumerateVideoNodes()) {
        if (node.card.compare(QStringLiteral("rkisp-statistics"), Qt::CaseInsensitive) == 0
            || node.card.compare(QStringLiteral("rkisp-input-params"), Qt::CaseInsensitive) == 0) {
            ispNodes.append(node.path);
        }
    }

    const QString selfPid = QString::number(QCoreApplication::applicationPid());

    QDir proc(QStringLiteral("/proc"));
    const QStringList pids = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& pid : pids) {
        bool numeric = false;
        const int pidValue = pid.toInt(&numeric);
        if (!numeric || pid == selfPid)
            continue;

        QDir fdDir(QStringLiteral("/proc/%1/fd").arg(pid));
        const QStringList fds = fdDir.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);

        for (const QString& fd : fds) {
            const QString target =
                QFile::symLinkTarget(QStringLiteral("/proc/%1/fd/%2").arg(pid, fd));
            if (target.isEmpty() || !ispNodes.contains(target))
                continue;

            owner.pid         = pidValue;
            owner.nodePath    = target;
            owner.processName = processNameFor(pid);
            return owner;
        }
    }

    // Without privilege the loop above sees nothing, so fall back to spotting
    // the stock daemon by name. This can miss a custom engine owner, but it
    // never reports one that is not there.
    for (const QString& pid : pids) {
        bool numeric = false;
        const int pidValue = pid.toInt(&numeric);
        if (!numeric || pid == selfPid)
            continue;

        const QString name = processNameFor(pid);
        if (name == QLatin1String("rkaiq_3A_server")) {
            owner.pid         = pidValue;
            owner.processName = name;
            return owner;
        }
    }

    return owner;
}
