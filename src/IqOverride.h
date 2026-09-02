#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Writes manual AE/AWB values into a private copy of the sensor's IQ file.
//
// This exists because on this platform rkaiq never receives ISP statistics, and
// its manual exposure path runs through the AE algorithm's per-frame pass. That
// is why rk_aiq_user_api2_ae_setExpSwAttr() and rk_aiq_uapi2_sysctl_updateIq()
// both report success and change nothing. The one path that does take effect is the
// calibration parsed at sysctl_init(), so a manual value is applied by patching
// the IQ file and bringing the engine back up.
//
// Everything still goes through rkaiq; only the route the value takes changes.
class IqOverride
{
public:
    struct Values {
        bool   aeManual        = false;
        double exposureSeconds = 0.01;
        double analogueGain    = 1.0;
        double ispDigitalGain  = 1.0;

        bool   awbManual = false;
        // Two mutually exclusive ways to drive manual WB, matching the UI.
        bool   wbByTemperature  = false;
        int    colourTemperature = 5000;
        double wbGainR   = 1.0;   // rkaiq orders these R, Gr, Gb, B
        double wbGainGr  = 1.0;
        double wbGainGb  = 1.0;
        double wbGainB   = 1.0;

        // Image controls, which rkaiq stores but never applies for the same
        // reason. Ranges match the UI: 0-255 around a neutral 128, and a
        // sharpening strength where 50 means "as the IQ file was tuned".
        int    contrast   = 128;
        int    brightness = 128;
        int    saturation = 128;
        int    hue        = 128;
        double sharpness  = 50.0;

        // Does anything here differ from the shipped calibration? Used to
        // decide whether an override is needed at all.
        bool isNeutral() const;

        bool operator==(const Values& other) const;
        bool operator!=(const Values& other) const { return !(*this == other); }
    };

    IqOverride() = default;
    ~IqOverride();

    IqOverride(const IqOverride&) = delete;
    IqOverride& operator=(const IqOverride&) = delete;

    // Load the sensor's IQ file so it can be patched. iqFileName must be the
    // exact name rkaiq itself would pick, since it re-derives it from the
    // module info rather than taking a path.
    bool load(const QString& baseIqDir, const QString& iqFileName);
    bool isLoaded() const { return !m_original.isEmpty(); }

    // Patch in the values and write the result. Returns the directory to hand
    // to sysctl_init(), or an empty string on failure.
    QString write(const Values& values);

    QString lastError() const { return m_lastError; }

private:
    void discardScratch();
    // Returns the change in length, so the caller can track shifting offsets.
    int patchScene(QByteArray& text, int sceneStart, int sceneEnd,
                   const Values& values) const;

    QByteArray m_original;
    QString m_fileName;
    QString m_scratchDir;
    QString m_scratchRoot;
    unsigned m_generation = 0;
    QString m_lastError;
};
