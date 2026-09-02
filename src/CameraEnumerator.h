#pragma once

#include <QString>
#include <QVector>

// One selectable capture mode, as reported by the rkaiq engine for a sensor.
// Mirrors rk_frame_fmt_t without leaking rkaiq headers into the UI layer.
struct CameraMode {
    int width   = 0;
    int height  = 0;
    int fps     = 0;
    int hdrMode = 0;   // NO_HDR / HDR_X2 / HDR_X3, as reported by the sensor

    QString label() const;      // "1920x1080 @ 30"
    bool isHdr() const;
};

// Everything discovered about one camera. Nothing here is hard-coded: it all
// comes from rk_aiq_uapi2_sysctl_enumStaticMetas() plus a VIDIOC_QUERYCAP scan.
struct CameraInfo {
    QString sensorEntity;        // "m00_b_ov5647 2-0036"
    QString sensorName;          // "ov5647", parsed out of the entity name
    QString videoNode;           // "/dev/videoN", discovered not assumed
    QString busInfo;             // "platform:rkisp0-vir0"
    QString sensorSubdev;        // "/dev/v4l-subdevN" for the sensor entity
    QVector<CameraMode> modes;

    // True when rkaiq reported no modes and they were read off the sensor
    // subdev instead. Such a sensor lacks Rockchip's rkmodule extensions, so
    // HDR and lens info are unavailable and the 3A algorithms may not converge.
    bool modesFromSubdev = false;

    bool hasVcm    = false;      // focus motor present -> AF controls usable
    bool hasFlash  = false;
    bool hasIrCut  = false;
    int  ispHwVer  = 0;
    int  phyId     = -1;

    bool isValid() const { return !sensorEntity.isEmpty(); }
    bool supportsHdr() const;
    // Distinct resolutions, and the frame rates offered for a given one.
    QVector<QPair<int, int>> resolutions() const;
    QVector<int> fpsFor(int width, int height) const;
};

// A node found by scanning /dev/video* with VIDIOC_QUERYCAP.
struct VideoNode {
    QString path;
    QString card;      // "rkisp_mainpath", "rkisp-statistics", ...
    QString driver;    // "rkisp_v6"
    QString busInfo;   // "platform:rkisp0-vir0"
    bool isMultiplanar = false;
    bool isMetaCapture = false;   // statistics / params nodes
};

// Another process already running an rkaiq engine on this ISP.
//
// Only one process may own the engine. If a second one calls sysctl_init()
// anyway it blocks indefinitely rather than returning an error, so this is
// checked up front and reported instead.
struct EngineOwner {
    int pid = -1;
    QString processName;
    QString nodePath;      // the ISP node it holds

    bool detected() const { return pid > 0; }
};

namespace CameraEnumerator {

// Scan /dev/video* for ISP capture nodes. Node numbering is not stable across
// boots, so this is the only sanctioned way to find one.
QVector<VideoNode> enumerateVideoNodes();

// Ask rkaiq which sensors it knows about, then pair each with its capture node.
// Returns an empty list when no camera is attached or the engine sees none.
QVector<CameraInfo> enumerateCameras();

// The /dev/v4l-subdevN backing a media entity, found by name via
// /sys/class/video4linux. Empty when the entity has no subdev node.
QString subdevForEntity(const QString& entity);

// Capture modes read directly from a sensor subdev with VIDIOC_SUBDEV_ENUM_*.
//
// This is the fallback for sensors rkaiq cannot describe. rkaiq builds its own
// mode table solely from VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL (its ENUM_FRAME_SIZE
// path is compiled out), and it gives up entirely if RKMODULE_GET_MODULE_INFO
// fails. Mainline sensor drivers leave both of those unimplemented, but they do
// implement ENUM_FRAME_SIZE, so the modes are there to be read.
QVector<CameraMode> enumerateSubdevModes(const QString& subdevPath);

// Human-readable explanation when enumerateCameras() comes back empty.
QString lastError();

// Is another process already running an rkaiq engine on this ISP? Detected by
// finding who holds the ISP statistics / input-params nodes, which the engine
// owner keeps open for as long as it runs.
//
// Reading another process's descriptors needs privilege; when that is not
// available this falls back to matching well-known daemon names, so it can
// under-report but never falsely accuse.
EngineOwner findEngineOwner();

// rkaiq serialises engine startup on /tmp/aiq<N>.lock, opening it with fopen()
// and calling fileno() on the result without checking for NULL. So a lock file
// left behind by a different user is not an error inside the engine, it is a
// segfault. Returns an explanation when the lock exists but cannot be opened
// for writing, or an empty string when startup is safe.
QString engineLockProblem(int phyId);

// The IQ file name rkaiq will look for, "<sensor>_<module>_<lens>.json", read
// from the sensor with RKMODULE_GET_MODULE_INFO. rkaiq derives this name itself
// and only accepts a directory, so anything wanting to substitute an IQ file
// must reproduce the name exactly. Empty when the sensor cannot report it.
QString iqFileNameFor(const QString& subdevPath);

} // namespace CameraEnumerator
