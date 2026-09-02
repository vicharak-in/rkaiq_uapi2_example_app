#pragma once

#include <QObject>
#include <QString>

// Opaque rkaiq context, forward-declared so no rkaiq header leaks into the UI.
struct rk_aiq_sys_ctx_s;

// Wraps the rkaiq uAPI2 engine.
//
// Guarding the enum traps: rkaiq exposes three different auto/manual enums that
// disagree on their numeric values (opMode_t AUTO=0/MANUAL=1, RKAiqOPMode_t
// AUTO=1/MANUAL=2, and rk_aiq_wb_op_mode_t which is inverted at MANUAL=0/AUTO=1).
// Mixing them compiles silently and selects the wrong mode, so callers only ever
// see Mode below and the conversion happens once, at each call site inside the .cpp.
class AiqController : public QObject
{
    Q_OBJECT

public:
    enum class Mode { Auto, Manual, SemiAuto };
    enum class WorkingMode { Normal, Hdr2, Hdr3 };

    struct Range {
        float min = 0.0f;
        float max = 0.0f;
        bool valid = false;
    };
    struct IntRange {
        int min = 0;
        int max = 0;
        bool valid = false;
    };
    struct RgbGain {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
    };

    explicit AiqController(QObject* parent = nullptr);
    ~AiqController() override;

    // --- lifecycle, in the order rkisp_demo uses -------------------------
    bool init(const QString& sensorEntity, const QString& iqFileDir, WorkingMode mode);
    bool prepare(int width, int height, WorkingMode mode);
    bool start();
    void stop();
    void deinit();

    bool isRunning() const { return m_running; }

    // Does the engine actually receive ISP 3A statistics? On platforms where it
    // does not, every algorithm-driven setter below silently does nothing,
    // because rkaiq applies them on the AE/AWB pass that statistics drive.
    // Blocks briefly, so call it once after start() rather than per control.
    bool statisticsAvailable();
    bool isInitialised() const { return m_ctx != nullptr; }
    QString versionInfo() const;

    // --- auto exposure ---------------------------------------------------
    bool setAeMode(Mode mode);
    bool setExposureTimeMs(double milliseconds);
    bool setAnalogueGain(double gain);
    bool setDigitalGain(double gain);          // ISP digital gain, distinct from above
    bool setFrameRate(double fps);             // live: no pipeline restart needed
    bool setAeLock(bool locked);
    bool setPowerLineFrequency(int hz);        // 0 = disabled, 50, 60
    Range exposureTimeRangeMs() const;
    Range analogueGainRange() const;
    bool aeConverged() const;

    // --- auto white balance ----------------------------------------------
    bool setAwbMode(Mode mode);
    bool setRgbGain(const RgbGain& gain);
    bool setColourTemperature(int kelvin);
    RgbGain rgbGain() const;
    int colourTemperature() const;

    // --- auto focus (only meaningful when the module has a VCM) -----------
    bool setAfMode(Mode mode);
    bool setFocusPosition(int code);
    bool triggerOneshotFocus();
    IntRange focusRange() const;
    IntRange zoomRange() const;
    bool setZoomPosition(int position);

    // --- image adjustment -------------------------------------------------
    bool setContrast(int level);      // [0, 255]
    bool setBrightness(int level);    // [0, 255]
    bool setSaturation(int level);    // [0, 255]
    bool setHue(int level);           // [0, 255]
    bool setSharpness(int level);     // [0, 100]
    bool setMirrorFlip(bool mirror, bool flip);

    // --- denoise ----------------------------------------------------------
    bool setNoiseReductionMode(Mode mode);
    bool setNoiseReductionStrength(int level);        // [0, 100], auto mode
    bool setSpatialNoiseReduction(bool on, int level);   // 2D, manual mode
    bool setTemporalNoiseReduction(bool on, int level);  // 3D, manual mode

signals:
    // Emitted for every uAPI2 call that fails, so an unsupported control
    // reports itself in the UI instead of silently doing nothing.
    void apiCallFailed(const QString& function, int returnCode);
    void apiCallSucceeded(const QString& function);

private:
    bool check(int ret, const char* function);

    rk_aiq_sys_ctx_s* m_ctx = nullptr;
    QString m_sensorEntity;
    bool m_running = false;
};
