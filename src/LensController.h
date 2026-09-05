#pragma once

#include <QString>

// Drives a focus motor directly over V4L2, using V4L2_CID_FOCUS_ABSOLUTE on the
// lens subdev.
//
// rkaiq would normally own this control, but it only recognises a VCM whose
// media entity name begins with the sensor's module index (m00_, m01_, ...).
// Upstream VCM drivers such as ak7375 and dw9807-vcm do not apply that Rockchip
// naming convention, so on those modules rkaiq creates no LensHw and never
// touches the lens at all, which leaves the control free for us to use.
//
// The position range is always read from the driver; VCMs differ widely (0..4095
// raw on ak7375, 0..64 logical on Rockchip's own drivers) and guessing would put
// the lens somewhere arbitrary.
class LensController
{
public:
    LensController() = default;
    ~LensController();

    LensController(const LensController&) = delete;
    LensController& operator=(const LensController&) = delete;

    bool open(const QString& subdevPath);
    void close();
    bool isOpen() const { return m_fd >= 0; }

    int minimum() const { return m_minimum; }
    int maximum() const { return m_maximum; }

    // Clamped to the driver's range, so a stale slider value cannot drive the
    // motor past its stop.
    bool setPosition(int code);
    int  position() const;

    // The last value we asked for, which survives the engine restarts that an
    // exposure change causes. -1 until something has been set.
    int lastRequested() const { return m_lastRequested; }

    QString devicePath() const { return m_path; }
    QString lastError() const { return m_lastError; }

private:
    int m_fd = -1;
    int m_minimum = 0;
    int m_maximum = 0;
    int m_lastRequested = -1;
    QString m_path;
    mutable QString m_lastError;
};
