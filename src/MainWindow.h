#pragma once

#include <QMainWindow>

#include "AiqController.h"
#include "CameraEnumerator.h"
#include "IqOverride.h"

class CameraPipeline;
class PreviewWidget;
class LabeledSlider;

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const QString& iqFileDir, const QString& preferredNode,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onApplyStreamSettings();
    void onCameraSelected(int index);
    void onResolutionSelected(int index);

    void onAutoExposureToggled(bool automatic);
    void onAutoWhiteBalanceToggled(bool automatic);
    void onAutoFocusToggled(bool automatic);
    void onAutoNoiseReductionToggled(bool automatic);
    void onWhiteBalanceModeToggled();

    // Manual values reach the engine through the IQ file on platforms with no
    // ISP statistics. That costs a stream restart, so edits are coalesced.
    void applyManualNow();

    void onStatusMessage(const QString& message);
    void onPipelineError(const QString& message);
    void refreshStatusBar();

private:
    // True when the engine gets no statistics, so its live setters do nothing
    // and manual values have to be written into the IQ file instead.
    bool manualGoesThroughIq() const;
    IqOverride::Values currentManualValues() const;
    void scheduleManualApply();
    void updateStatisticsNotice();

    QWidget* buildControlPanel();
    QGroupBox* buildStreamGroup();
    QGroupBox* buildExposureGroup();
    QGroupBox* buildWhiteBalanceGroup();
    QGroupBox* buildFocusGroup();
    QGroupBox* buildImageGroup();
    QGroupBox* buildDenoiseGroup();

    void startSelectedCamera();
    void populateCameraCombo();
    void populateResolutionCombo();
    void populateFpsCombo();
    void populateHdrCombo();

    // Enable or grey each group according to what the attached module actually
    // supports, then seed every control from the engine's current state.
    void applyCapabilityGating();
    void seedControlsFromEngine();
    void setGroupEnabled(QGroupBox* group, bool enabled, const QString& disabledReason);

    AiqController* aiq() const;
    AiqController::WorkingMode selectedWorkingMode() const;

    CameraPipeline* m_pipeline = nullptr;
    PreviewWidget* m_preview = nullptr;
    QTimer* m_statusTimer = nullptr;
    QTimer* m_manualApplyTimer = nullptr;
    QString m_sensorBaseText;
    bool m_applyingManual = false;

    QString m_iqFileDir;
    QString m_preferredNode;
    QString m_lastApiMessage;

    // Stream
    QGroupBox* m_streamGroup = nullptr;
    QComboBox* m_cameraCombo = nullptr;
    QComboBox* m_resolutionCombo = nullptr;
    QComboBox* m_fpsCombo = nullptr;
    QComboBox* m_hdrCombo = nullptr;
    QPushButton* m_applyButton = nullptr;
    QLabel* m_sensorLabel = nullptr;

    // Exposure
    QGroupBox* m_exposureGroup = nullptr;
    QCheckBox* m_autoExposure = nullptr;
    LabeledSlider* m_exposureSlider = nullptr;
    LabeledSlider* m_analogueGainSlider = nullptr;
    LabeledSlider* m_digitalGainSlider = nullptr;
    QComboBox* m_flickerCombo = nullptr;
    QCheckBox* m_aeLock = nullptr;

    // White balance
    QGroupBox* m_whiteBalanceGroup = nullptr;
    QCheckBox* m_autoWhiteBalance = nullptr;
    QRadioButton* m_wbByTemperature = nullptr;
    QRadioButton* m_wbByRgbGain = nullptr;
    LabeledSlider* m_temperatureSlider = nullptr;
    LabeledSlider* m_redGainSlider = nullptr;
    LabeledSlider* m_greenGainSlider = nullptr;
    LabeledSlider* m_blueGainSlider = nullptr;

    // Focus
    QGroupBox* m_focusGroup = nullptr;
    QCheckBox* m_autoFocus = nullptr;
    LabeledSlider* m_focusSlider = nullptr;
    QPushButton* m_oneshotFocusButton = nullptr;
    LabeledSlider* m_zoomSlider = nullptr;

    // Image
    QGroupBox* m_imageGroup = nullptr;
    LabeledSlider* m_contrastSlider = nullptr;
    LabeledSlider* m_brightnessSlider = nullptr;
    LabeledSlider* m_saturationSlider = nullptr;
    LabeledSlider* m_hueSlider = nullptr;
    LabeledSlider* m_sharpnessSlider = nullptr;
    QCheckBox* m_mirror = nullptr;
    QCheckBox* m_flip = nullptr;

    // Denoise
    QGroupBox* m_denoiseGroup = nullptr;
    QCheckBox* m_autoDenoise = nullptr;
    LabeledSlider* m_denoiseSlider = nullptr;
    LabeledSlider* m_spatialDenoiseSlider = nullptr;
    LabeledSlider* m_temporalDenoiseSlider = nullptr;
};
