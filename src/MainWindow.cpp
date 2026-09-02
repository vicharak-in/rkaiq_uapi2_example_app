#include "MainWindow.h"

#include "CameraPipeline.h"
#include "LabeledSlider.h"
#include "PreviewWidget.h"
#include "V4l2Capture.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDebug>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "rk-camera-module.h"

namespace {

constexpr int kStatusRefreshMs = 500;

// Long enough that dragging a slider settles before the engine is restarted.
// Each apply costs a full stop/start, so this is deliberately slower than the
// slider's own coalescing.
constexpr int kManualApplyMs = 700;
constexpr int kControlPanelWidth = 420;

// Fallback slider bounds, used only when the engine declines to report a range.
constexpr double kFallbackExposureMinMs = 0.1;
constexpr double kFallbackExposureMaxMs = 100.0;
constexpr double kFallbackGainMin = 1.0;
constexpr double kFallbackGainMax = 64.0;

// setMWBCT's documented range.
constexpr int kColourTemperatureMin = 2800;
constexpr int kColourTemperatureMax = 7500;

QString hdrModeName(int hdrMode)
{
    switch (hdrMode) {
    case HDR_X2: return QStringLiteral("HDR (2 frame)");
    case HDR_X3: return QStringLiteral("HDR (3 frame)");
    default:     return QStringLiteral("Normal");
    }
}

} // namespace

MainWindow::MainWindow(const QString& iqFileDir, const QString& preferredNode, QWidget* parent)
    : QMainWindow(parent)
    , m_pipeline(new CameraPipeline(this))
    , m_iqFileDir(iqFileDir)
    , m_preferredNode(preferredNode)
{
    setWindowTitle(QStringLiteral("RK3588 Camera Controls"));

    m_preview = new PreviewWidget(this);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_preview);
    splitter->addWidget(buildControlPanel());
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    setCentralWidget(splitter);

    connect(m_pipeline, &CameraPipeline::frameReady, m_preview, &PreviewWidget::setFrame);
    connect(m_pipeline, &CameraPipeline::statusMessage, this, &MainWindow::onStatusMessage);
    connect(m_pipeline, &CameraPipeline::errorOccurred, this, &MainWindow::onPipelineError);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusRefreshMs);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshStatusBar);
    m_statusTimer->start();

    m_manualApplyTimer = new QTimer(this);
    m_manualApplyTimer->setSingleShot(true);
    m_manualApplyTimer->setInterval(kManualApplyMs);
    connect(m_manualApplyTimer, &QTimer::timeout, this, &MainWindow::applyManualNow);

    if (!m_pipeline->discover()) {
        m_preview->setPlaceholderText(QStringLiteral("No camera found"));
        statusBar()->showMessage(m_pipeline->lastError());
        m_streamGroup->setEnabled(false);
        applyCapabilityGating();
        return;
    }

    populateCameraCombo();
    startSelectedCamera();
}

MainWindow::~MainWindow()
{
    if (m_pipeline)
        m_pipeline->stop();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Teardown must happen before the process exits or the ISP stays claimed
    // and the next launch fails.
    m_statusTimer->stop();
    m_pipeline->stop();
    event->accept();
}

AiqController* MainWindow::aiq() const
{
    return m_pipeline->aiq();
}

// --- panel construction ---------------------------------------------------

QWidget* MainWindow::buildControlPanel()
{
    auto* container = new QWidget;
    auto* layout = new QVBoxLayout(container);

    m_sensorLabel = new QLabel(QStringLiteral("Detecting camera..."), container);
    m_sensorLabel->setWordWrap(true);
    layout->addWidget(m_sensorLabel);

    layout->addWidget(m_streamGroup       = buildStreamGroup());
    layout->addWidget(m_exposureGroup     = buildExposureGroup());
    layout->addWidget(m_whiteBalanceGroup = buildWhiteBalanceGroup());
    layout->addWidget(m_focusGroup        = buildFocusGroup());
    layout->addWidget(m_imageGroup        = buildImageGroup());
    layout->addWidget(m_denoiseGroup      = buildDenoiseGroup());
    layout->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidget(container);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(kControlPanelWidth);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return scroll;
}

QGroupBox* MainWindow::buildStreamGroup()
{
    auto* group = new QGroupBox(QStringLiteral("Stream"));
    auto* form = new QFormLayout(group);

    m_cameraCombo     = new QComboBox(group);
    m_resolutionCombo = new QComboBox(group);
    m_fpsCombo        = new QComboBox(group);
    m_hdrCombo        = new QComboBox(group);

    // Shown only when discovery finds more than one option, so a single-camera
    // board is not cluttered with dead widgets.
    m_cameraCombo->setVisible(false);
    m_hdrCombo->setVisible(false);

    form->addRow(QStringLiteral("Camera"), m_cameraCombo);
    form->addRow(QStringLiteral("Resolution"), m_resolutionCombo);
    form->addRow(QStringLiteral("Frame rate"), m_fpsCombo);
    form->addRow(QStringLiteral("Mode"), m_hdrCombo);

    m_applyButton = new QPushButton(QStringLiteral("Apply (restarts stream)"), group);
    form->addRow(m_applyButton);

    connect(m_cameraCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onCameraSelected);
    connect(m_resolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onResolutionSelected);
    connect(m_applyButton, &QPushButton::clicked, this, &MainWindow::onApplyStreamSettings);

    // Frame rate is an AE attribute, so it applies live rather than waiting for
    // the restart that a resolution change needs.
    connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const double fps = m_fpsCombo->currentData().toDouble();
        if (fps > 0.0 && m_pipeline->isRunning())
            aiq()->setFrameRate(fps);
    });

    return group;
}

QGroupBox* MainWindow::buildExposureGroup()
{
    auto* group = new QGroupBox(QStringLiteral("Exposure"));
    auto* layout = new QVBoxLayout(group);

    m_autoExposure = new QCheckBox(QStringLiteral("Auto exposure (AE)"), group);
    m_autoExposure->setChecked(true);
    layout->addWidget(m_autoExposure);

    m_exposureSlider = new LabeledSlider(QStringLiteral("Exposure"), group);
    m_exposureSlider->setDecimals(2);
    m_exposureSlider->setRange(kFallbackExposureMinMs, kFallbackExposureMaxMs);
    m_exposureSlider->setSuffix(QStringLiteral(" ms"));
    layout->addWidget(m_exposureSlider);

    m_analogueGainSlider = new LabeledSlider(QStringLiteral("Analogue gain"), group);
    m_analogueGainSlider->setDecimals(2);
    m_analogueGainSlider->setRange(kFallbackGainMin, kFallbackGainMax);
    m_analogueGainSlider->setSuffix(QStringLiteral("x"));
    layout->addWidget(m_analogueGainSlider);

    // Distinct from analogue gain: this is the ISP's digital gain, which
    // brightens without changing integration time.
    m_digitalGainSlider = new LabeledSlider(QStringLiteral("Digital gain"), group);
    m_digitalGainSlider->setDecimals(2);
    m_digitalGainSlider->setRange(1.0, 16.0);
    m_digitalGainSlider->setSuffix(QStringLiteral("x"));
    layout->addWidget(m_digitalGainSlider);

    auto* row = new QHBoxLayout;
    m_flickerCombo = new QComboBox(group);
    m_flickerCombo->addItem(QStringLiteral("No anti-flicker"), 0);
    m_flickerCombo->addItem(QStringLiteral("50 Hz"), 50);
    m_flickerCombo->addItem(QStringLiteral("60 Hz"), 60);
    m_aeLock = new QCheckBox(QStringLiteral("Lock AE"), group);
    row->addWidget(new QLabel(QStringLiteral("Flicker"), group));
    row->addWidget(m_flickerCombo, 1);
    row->addWidget(m_aeLock);
    layout->addLayout(row);

    connect(m_autoExposure, &QCheckBox::toggled, this, &MainWindow::onAutoExposureToggled);
    connect(m_exposureSlider, &LabeledSlider::valueChanged, this, [this](double ms) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setExposureTimeMs(ms);
    });
    connect(m_analogueGainSlider, &LabeledSlider::valueChanged, this, [this](double gain) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setAnalogueGain(gain);
    });
    connect(m_digitalGainSlider, &LabeledSlider::valueChanged, this, [this](double gain) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setDigitalGain(gain);
    });
    connect(m_flickerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { aiq()->setPowerLineFrequency(m_flickerCombo->currentData().toInt()); });
    connect(m_aeLock, &QCheckBox::toggled, this,
            [this](bool locked) { aiq()->setAeLock(locked); });

    return group;
}

QGroupBox* MainWindow::buildWhiteBalanceGroup()
{
    auto* group = new QGroupBox(QStringLiteral("White balance"));
    auto* layout = new QVBoxLayout(group);

    m_autoWhiteBalance = new QCheckBox(QStringLiteral("Auto white balance (AWB)"), group);
    m_autoWhiteBalance->setChecked(true);
    layout->addWidget(m_autoWhiteBalance);

    // Colour temperature and explicit RGB gains are two mutually exclusive ways
    // of driving manual WB, so they are radio-selected rather than both live.
    m_wbByTemperature = new QRadioButton(QStringLiteral("By colour temperature"), group);
    m_wbByRgbGain     = new QRadioButton(QStringLiteral("By RGB gain"), group);
    m_wbByTemperature->setChecked(true);

    auto* modeGroup = new QButtonGroup(group);
    modeGroup->addButton(m_wbByTemperature);
    modeGroup->addButton(m_wbByRgbGain);

    layout->addWidget(m_wbByTemperature);

    m_temperatureSlider = new LabeledSlider(QStringLiteral("Temperature"), group);
    m_temperatureSlider->setRange(kColourTemperatureMin, kColourTemperatureMax);
    m_temperatureSlider->setSuffix(QStringLiteral(" K"));
    layout->addWidget(m_temperatureSlider);

    layout->addWidget(m_wbByRgbGain);

    m_redGainSlider   = new LabeledSlider(QStringLiteral("Red gain"), group);
    m_greenGainSlider = new LabeledSlider(QStringLiteral("Green gain"), group);
    m_blueGainSlider  = new LabeledSlider(QStringLiteral("Blue gain"), group);
    for (LabeledSlider* slider : { m_redGainSlider, m_greenGainSlider, m_blueGainSlider }) {
        slider->setDecimals(2);
        slider->setRange(0.1, 8.0);
        slider->setSingleStep(0.05);
        layout->addWidget(slider);
    }

    connect(m_autoWhiteBalance, &QCheckBox::toggled, this,
            &MainWindow::onAutoWhiteBalanceToggled);
    connect(m_wbByTemperature, &QRadioButton::toggled, this,
            &MainWindow::onWhiteBalanceModeToggled);
    connect(m_temperatureSlider, &LabeledSlider::valueChanged, this, [this](double kelvin) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setColourTemperature(int(kelvin));
    });

    const auto pushRgbGain = [this](double) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        AiqController::RgbGain gain;
        gain.r = float(m_redGainSlider->value());
        gain.g = float(m_greenGainSlider->value());
        gain.b = float(m_blueGainSlider->value());
        aiq()->setRgbGain(gain);
    };
    connect(m_redGainSlider, &LabeledSlider::valueChanged, this, pushRgbGain);
    connect(m_greenGainSlider, &LabeledSlider::valueChanged, this, pushRgbGain);
    connect(m_blueGainSlider, &LabeledSlider::valueChanged, this, pushRgbGain);

    return group;
}

QGroupBox* MainWindow::buildFocusGroup()
{
    auto* group = new QGroupBox(QStringLiteral("Focus"));
    auto* layout = new QVBoxLayout(group);

    m_autoFocus = new QCheckBox(QStringLiteral("Auto focus (AF)"), group);
    m_autoFocus->setChecked(true);
    layout->addWidget(m_autoFocus);

    m_focusSlider = new LabeledSlider(QStringLiteral("Focus position"), group);
    layout->addWidget(m_focusSlider);

    m_oneshotFocusButton = new QPushButton(QStringLiteral("Focus once"), group);
    layout->addWidget(m_oneshotFocusButton);

    m_zoomSlider = new LabeledSlider(QStringLiteral("Zoom position"), group);
    layout->addWidget(m_zoomSlider);

    connect(m_autoFocus, &QCheckBox::toggled, this, &MainWindow::onAutoFocusToggled);
    connect(m_focusSlider, &LabeledSlider::valueChanged, this,
            [this](double code) { aiq()->setFocusPosition(int(code)); });
    connect(m_oneshotFocusButton, &QPushButton::clicked, this,
            [this] { aiq()->triggerOneshotFocus(); });
    connect(m_zoomSlider, &LabeledSlider::valueChanged, this,
            [this](double position) { aiq()->setZoomPosition(int(position)); });

    return group;
}

QGroupBox* MainWindow::buildImageGroup()
{
    auto* group = new QGroupBox(QStringLiteral("Image"));
    auto* layout = new QVBoxLayout(group);

    // These ranges are fixed ISP-module properties, not sensor-dependent.
    struct SliderSpec {
        LabeledSlider** target;
        const char* label;
        int maximum;
    };
    const SliderSpec specs[] = {
        { &m_contrastSlider,   "Contrast",   255 },
        { &m_brightnessSlider, "Brightness", 255 },
        { &m_saturationSlider, "Saturation", 255 },
        { &m_hueSlider,        "Hue",        255 },
        { &m_sharpnessSlider,  "Sharpness",  100 },
    };

    for (const SliderSpec& spec : specs) {
        auto* slider = new LabeledSlider(QString::fromLatin1(spec.label), group);
        slider->setRange(0, spec.maximum);
        layout->addWidget(slider);
        *spec.target = slider;
    }

    auto* row = new QHBoxLayout;
    m_mirror = new QCheckBox(QStringLiteral("Mirror"), group);
    m_flip   = new QCheckBox(QStringLiteral("Flip"), group);
    row->addWidget(m_mirror);
    row->addWidget(m_flip);
    row->addStretch(1);
    layout->addLayout(row);

    connect(m_contrastSlider, &LabeledSlider::valueChanged, this, [this](double v) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setContrast(int(v));
    });
    connect(m_brightnessSlider, &LabeledSlider::valueChanged, this, [this](double v) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setBrightness(int(v));
    });
    connect(m_saturationSlider, &LabeledSlider::valueChanged, this, [this](double v) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setSaturation(int(v));
    });
    connect(m_hueSlider, &LabeledSlider::valueChanged, this, [this](double v) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setHue(int(v));
    });
    connect(m_sharpnessSlider, &LabeledSlider::valueChanged, this, [this](double v) {
        if (manualGoesThroughIq()) { scheduleManualApply(); return; }
        aiq()->setSharpness(int(v));
    });

    const auto pushMirrorFlip = [this](bool) {
        aiq()->setMirrorFlip(m_mirror->isChecked(), m_flip->isChecked());
    };
    connect(m_mirror, &QCheckBox::toggled, this, pushMirrorFlip);
    connect(m_flip, &QCheckBox::toggled, this, pushMirrorFlip);

    return group;
}

QGroupBox* MainWindow::buildDenoiseGroup()
{
    auto* group = new QGroupBox(QStringLiteral("Denoise"));
    auto* layout = new QVBoxLayout(group);

    m_autoDenoise = new QCheckBox(QStringLiteral("Auto noise reduction"), group);
    m_autoDenoise->setChecked(true);
    layout->addWidget(m_autoDenoise);

    m_denoiseSlider = new LabeledSlider(QStringLiteral("Strength"), group);
    m_denoiseSlider->setRange(0, 100);
    layout->addWidget(m_denoiseSlider);

    m_spatialDenoiseSlider = new LabeledSlider(QStringLiteral("Spatial (2D)"), group);
    m_spatialDenoiseSlider->setRange(0, 100);
    layout->addWidget(m_spatialDenoiseSlider);

    m_temporalDenoiseSlider = new LabeledSlider(QStringLiteral("Temporal (3D)"), group);
    m_temporalDenoiseSlider->setRange(0, 100);
    layout->addWidget(m_temporalDenoiseSlider);

    connect(m_autoDenoise, &QCheckBox::toggled, this,
            &MainWindow::onAutoNoiseReductionToggled);
    connect(m_denoiseSlider, &LabeledSlider::valueChanged, this,
            [this](double v) { aiq()->setNoiseReductionStrength(int(v)); });
    connect(m_spatialDenoiseSlider, &LabeledSlider::valueChanged, this,
            [this](double v) { aiq()->setSpatialNoiseReduction(true, int(v)); });
    connect(m_temporalDenoiseSlider, &LabeledSlider::valueChanged, this,
            [this](double v) { aiq()->setTemporalNoiseReduction(true, int(v)); });

    return group;
}

// --- combo population -----------------------------------------------------

void MainWindow::populateCameraCombo()
{
    const QSignalBlocker blocker(m_cameraCombo);
    m_cameraCombo->clear();

    const auto& cameras = m_pipeline->cameras();
    for (int i = 0; i < cameras.size(); ++i)
        m_cameraCombo->addItem(cameras[i].sensorName, i);

    // Only worth showing when there is a genuine choice to make.
    m_cameraCombo->setVisible(cameras.size() > 1);

    if (!m_preferredNode.isEmpty()) {
        for (int i = 0; i < cameras.size(); ++i) {
            if (cameras[i].videoNode == m_preferredNode) {
                m_cameraCombo->setCurrentIndex(i);
                break;
            }
        }
    }
}

void MainWindow::populateResolutionCombo()
{
    const QSignalBlocker blocker(m_resolutionCombo);
    m_resolutionCombo->clear();

    const int cameraIndex = qMax(0, m_cameraCombo->currentIndex());
    if (cameraIndex >= m_pipeline->cameras().size())
        return;

    const CameraInfo& camera = m_pipeline->cameras().at(cameraIndex);

    // Straight from the engine's support_fmt table, never a hard-coded list.
    int defaultIndex = 0;
    const auto resolutions = camera.resolutions();
    for (int i = 0; i < resolutions.size(); ++i) {
        const auto& res = resolutions[i];
        m_resolutionCombo->addItem(QStringLiteral("%1 x %2").arg(res.first).arg(res.second),
                                   QPoint(res.first, res.second));

        // Default to the smallest mode at least 720p tall: big enough to judge
        // the controls, small enough for the software colour conversion.
        const auto& best = resolutions[defaultIndex];
        const bool currentOk = best.second >= 720;
        const bool candidateOk = res.second >= 720;
        if ((candidateOk && !currentOk) || (candidateOk == currentOk && res.second < best.second))
            defaultIndex = i;
    }

    if (m_resolutionCombo->count() > 0)
        m_resolutionCombo->setCurrentIndex(defaultIndex);

    populateFpsCombo();
    populateHdrCombo();
}

void MainWindow::populateFpsCombo()
{
    const QSignalBlocker blocker(m_fpsCombo);
    m_fpsCombo->clear();

    const int cameraIndex = qMax(0, m_cameraCombo->currentIndex());
    if (cameraIndex >= m_pipeline->cameras().size())
        return;

    const CameraInfo& camera = m_pipeline->cameras().at(cameraIndex);
    const QPoint size = m_resolutionCombo->currentData().toPoint();

    const QVector<int> rates = camera.fpsFor(size.x(), size.y());
    for (int fps : rates)
        m_fpsCombo->addItem(QStringLiteral("%1 fps").arg(fps), fps);

    if (m_fpsCombo->count() == 0)
        m_fpsCombo->addItem(QStringLiteral("Sensor default"), 0);

    m_fpsCombo->setCurrentIndex(m_fpsCombo->count() - 1);
}

void MainWindow::populateHdrCombo()
{
    const QSignalBlocker blocker(m_hdrCombo);
    m_hdrCombo->clear();

    const int cameraIndex = qMax(0, m_cameraCombo->currentIndex());
    if (cameraIndex >= m_pipeline->cameras().size())
        return;

    const CameraInfo& camera = m_pipeline->cameras().at(cameraIndex);
    const QPoint size = m_resolutionCombo->currentData().toPoint();

    QVector<int> hdrModes;
    for (const CameraMode& mode : camera.modes) {
        if (mode.width == size.x() && mode.height == size.y()
            && !hdrModes.contains(mode.hdrMode)) {
            hdrModes.append(mode.hdrMode);
        }
    }
    if (hdrModes.isEmpty())
        hdrModes.append(NO_HDR);

    std::sort(hdrModes.begin(), hdrModes.end());
    for (int hdrMode : hdrModes)
        m_hdrCombo->addItem(hdrModeName(hdrMode), hdrMode);

    // Hidden unless the sensor genuinely offers a choice.
    m_hdrCombo->setVisible(hdrModes.size() > 1);
}

AiqController::WorkingMode MainWindow::selectedWorkingMode() const
{
    switch (m_hdrCombo->currentData().toInt()) {
    case HDR_X2: return AiqController::WorkingMode::Hdr2;
    case HDR_X3: return AiqController::WorkingMode::Hdr3;
    default:     return AiqController::WorkingMode::Normal;
    }
}

// --- manual controls without ISP statistics --------------------------------

void MainWindow::updateStatisticsNotice()
{
    if (m_sensorBaseText.isEmpty() || !m_pipeline->isRunning())
        return;

    QString text = m_sensorBaseText;
    if (!m_pipeline->statisticsAvailable()) {
        text += QStringLiteral(
            "<br/><span style='color:#c8a000'>no ISP statistics reach rkaiq, so its "
            "3A never runs. Auto exposure and auto white balance are inert; manual "
            "values are written to the IQ file, which restarts the stream on each "
            "change.</span>");
    }
    m_sensorLabel->setText(text);
}

bool MainWindow::manualGoesThroughIq() const
{
    return m_pipeline->isRunning() && !m_pipeline->statisticsAvailable();
}

IqOverride::Values MainWindow::currentManualValues() const
{
    IqOverride::Values values;

    values.aeManual        = !m_autoExposure->isChecked();
    values.exposureSeconds = m_exposureSlider->value() / 1000.0;   // slider is ms
    values.analogueGain    = m_analogueGainSlider->value();
    values.ispDigitalGain  = m_digitalGainSlider->value();

    values.awbManual        = !m_autoWhiteBalance->isChecked();
    values.wbByTemperature  = m_wbByTemperature->isChecked();
    values.colourTemperature = int(m_temperatureSlider->value());

    // The IQ file wants R, Gr, Gb, B; the UI offers a single green.
    values.wbGainR  = m_redGainSlider->value();
    values.wbGainGr = m_greenGainSlider->value();
    values.wbGainGb = m_greenGainSlider->value();
    values.wbGainB  = m_blueGainSlider->value();

    values.contrast   = int(m_contrastSlider->value());
    values.brightness = int(m_brightnessSlider->value());
    values.saturation = int(m_saturationSlider->value());
    values.hue        = int(m_hueSlider->value());
    values.sharpness  = m_sharpnessSlider->value();

    return values;
}

void MainWindow::scheduleManualApply()
{
    if (!manualGoesThroughIq())
        return;
    m_manualApplyTimer->start();
}

void MainWindow::applyManualNow()
{
    // Re-entrancy is real here: the restart below runs the event loop, and the
    // coalesce timer or a mode handler firing inside it would restart the
    // engine again from within this call, which is how a single slider nudge
    // turned into a restart loop.
    if (m_applyingManual || !manualGoesThroughIq())
        return;

    m_manualApplyTimer->stop();

    const IqOverride::Values values = currentManualValues();

    // Nothing to do while both algorithms are on auto and no override has been
    // written yet. Seeding the controls at startup runs the mode handlers, and
    // restarting the stream for that would be gratuitous.
    if (values.isNeutral() && !m_pipeline->manualOverrideActive())
        return;

    if (m_pipeline->manualOverrideActive() && values == m_pipeline->manualValues())
        return;

    m_preview->setPlaceholderText(QStringLiteral("Applying manual values..."));

    m_applyingManual = true;
    const bool ok = m_pipeline->applyManualValues(values);
    m_applyingManual = false;

    if (!ok) {
        onPipelineError(m_pipeline->lastError());
        applyCapabilityGating();
        return;
    }

    applyCapabilityGating();
    updateStatisticsNotice();
    onStatusMessage(QStringLiteral("Manual values applied (engine restarted)"));
}

// --- lifecycle ------------------------------------------------------------

void MainWindow::startSelectedCamera()
{
    const int cameraIndex = qMax(0, m_cameraCombo->currentIndex());
    if (cameraIndex >= m_pipeline->cameras().size())
        return;

    const CameraInfo& camera = m_pipeline->cameras().at(cameraIndex);

    populateResolutionCombo();

    const QPoint size = m_resolutionCombo->currentData().toPoint();
    if (size.isNull()) {
        onPipelineError(QStringLiteral("Camera '%1' reported no usable capture modes")
                            .arg(camera.sensorName));
        return;
    }

    QString sensorText =
        QStringLiteral("<b>%1</b><br/>entity: %2<br/>node: %3<br/>ISP v%4 &nbsp; AIQ %5")
            .arg(camera.sensorName, camera.sensorEntity, camera.videoNode)
            .arg(camera.ispHwVer)
            .arg(aiq()->versionInfo());

    // Worth saying plainly: a sensor rkaiq could not describe is also one whose
    // 3A may never converge, so the dead exposure controls are not a surprise.
    if (camera.modesFromSubdev) {
        sensorText += QStringLiteral(
            "<br/><span style='color:#c8a000'>modes read from %1 &mdash; rkaiq "
            "reported none, so its 3A may not converge</span>")
            .arg(camera.sensorSubdev);
    }

    m_sensorLabel->setText(sensorText);
    m_sensorBaseText = sensorText;

    if (!m_pipeline->start(camera, size.x(), size.y(), selectedWorkingMode(), m_iqFileDir)) {
        // The reason matters more than the fact. An engine-ownership clash is
        // fixable in one command, so show the whole message, not a summary.
        m_preview->setPlaceholderText(m_pipeline->lastError());
        onPipelineError(m_pipeline->lastError());
        applyCapabilityGating();
        return;
    }

    applyCapabilityGating();
    seedControlsFromEngine();
    updateStatisticsNotice();

    const double fps = m_fpsCombo->currentData().toDouble();
    if (fps > 0.0)
        aiq()->setFrameRate(fps);
}

void MainWindow::onApplyStreamSettings()
{
    const QPoint size = m_resolutionCombo->currentData().toPoint();
    if (size.isNull())
        return;

    m_preview->clear();
    m_preview->setPlaceholderText(QStringLiteral("Restarting..."));
    QApplication::processEvents();

    // Nothing has ever started successfully (e.g. the first attempt hit an
    // engine clash), so this is a fresh start rather than a restart.
    if (!m_pipeline->currentCamera().isValid()) {
        startSelectedCamera();
        return;
    }

    // A resolution or HDR-mode change cannot be applied live: the whole
    // pipeline is torn down and rebuilt.
    if (!m_pipeline->restart(size.x(), size.y(), selectedWorkingMode())) {
        m_preview->setPlaceholderText(m_pipeline->lastError());
        onPipelineError(m_pipeline->lastError());
        applyCapabilityGating();
        return;
    }

    applyCapabilityGating();
    seedControlsFromEngine();

    const double fps = m_fpsCombo->currentData().toDouble();
    if (fps > 0.0)
        aiq()->setFrameRate(fps);
}

void MainWindow::onCameraSelected(int /*index*/)
{
    m_preview->clear();
    m_pipeline->stop();
    startSelectedCamera();
}

void MainWindow::onResolutionSelected(int /*index*/)
{
    // Only refresh the dependent combos; nothing restarts until Apply.
    populateFpsCombo();
    populateHdrCombo();
}

// --- capability gating ----------------------------------------------------

void MainWindow::setGroupEnabled(QGroupBox* group, bool enabled, const QString& disabledReason)
{
    group->setEnabled(enabled);
    group->setToolTip(enabled ? QString() : disabledReason);
}

void MainWindow::applyCapabilityGating()
{
    const bool running = m_pipeline->isRunning();

    setGroupEnabled(m_exposureGroup, running,
                    QStringLiteral("Camera is not streaming"));
    setGroupEnabled(m_whiteBalanceGroup, running,
                    QStringLiteral("Camera is not streaming"));
    setGroupEnabled(m_imageGroup, running,
                    QStringLiteral("Camera is not streaming"));
    setGroupEnabled(m_denoiseGroup, running,
                    QStringLiteral("Camera is not streaming"));

    // When nothing is streaming, Apply becomes the retry: the usual reason for
    // a failed start (another process owning the engine) is fixed externally,
    // and the user needs a way back in without restarting the app.
    m_applyButton->setText(running ? QStringLiteral("Apply (restarts stream)")
                                   : QStringLiteral("Retry"));

    // Focus is gated on the module actually having a motor. has_lens_vcm is the
    // primary signal; a missing focus range is the secondary check. On a
    // fixed-focus module the whole group greys out, and on a VCM module the very
    // same code enables it with the real range. No per-sensor branching.
    const CameraInfo& camera = m_pipeline->currentCamera();
    const AiqController::IntRange focus = running ? aiq()->focusRange()
                                                  : AiqController::IntRange {};
    const bool focusUsable = running && camera.hasVcm && focus.valid;

    QString focusReason;
    if (!running)
        focusReason = QStringLiteral("Camera is not streaming");
    else if (!camera.hasVcm)
        focusReason = QStringLiteral("No focus motor on this sensor");
    else if (!focus.valid)
        focusReason = QStringLiteral("Sensor reported no usable focus range");

    setGroupEnabled(m_focusGroup, focusUsable, focusReason);

    if (focusUsable) {
        m_focusSlider->setRange(focus.min, focus.max);

        const AiqController::IntRange zoom = aiq()->zoomRange();
        m_zoomSlider->setVisible(zoom.valid);
        if (zoom.valid)
            m_zoomSlider->setRange(zoom.min, zoom.max);
    } else {
        m_zoomSlider->setVisible(false);
    }

    // Manual sub-controls follow their group's Auto checkbox.
    onAutoExposureToggled(m_autoExposure->isChecked());
    onAutoWhiteBalanceToggled(m_autoWhiteBalance->isChecked());
    onAutoFocusToggled(m_autoFocus->isChecked());
    onAutoNoiseReductionToggled(m_autoDenoise->isChecked());
}

// Seeding from the engine means manual values start wherever auto had settled,
// so switching a group to manual does not make the picture jump.
void MainWindow::seedControlsFromEngine()
{
    if (!m_pipeline->isRunning())
        return;

    const AiqController::Range exposure = aiq()->exposureTimeRangeMs();
    if (exposure.valid) {
        m_exposureSlider->setRange(exposure.min, exposure.max);
        m_exposureSlider->setValueSilently(exposure.min
                                           + (exposure.max - exposure.min) * 0.5);
    }

    const AiqController::Range gain = aiq()->analogueGainRange();
    if (gain.valid) {
        m_analogueGainSlider->setRange(gain.min, gain.max);
        m_analogueGainSlider->setValueSilently(gain.min);
    }

    m_digitalGainSlider->setValueSilently(1.0);

    const AiqController::RgbGain wb = aiq()->rgbGain();
    m_redGainSlider->setValueSilently(wb.r);
    m_greenGainSlider->setValueSilently(wb.g);
    m_blueGainSlider->setValueSilently(wb.b);

    const int cct = aiq()->colourTemperature();
    if (cct >= kColourTemperatureMin && cct <= kColourTemperatureMax)
        m_temperatureSlider->setValueSilently(cct);
    else
        m_temperatureSlider->setValueSilently(5000);

    // The image and denoise modules have fixed ranges; midpoints are a sane
    // neutral starting position.
    m_contrastSlider->setValueSilently(128);
    m_brightnessSlider->setValueSilently(128);
    m_saturationSlider->setValueSilently(128);
    m_hueSlider->setValueSilently(128);
    m_sharpnessSlider->setValueSilently(50);
    m_denoiseSlider->setValueSilently(50);
    m_spatialDenoiseSlider->setValueSilently(50);
    m_temporalDenoiseSlider->setValueSilently(50);
}

// --- auto/manual toggles --------------------------------------------------

void MainWindow::onAutoExposureToggled(bool automatic)
{
    const bool manual = !automatic && m_exposureGroup->isEnabled();

    m_exposureSlider->setEnabled(manual);
    m_analogueGainSlider->setEnabled(manual);
    m_digitalGainSlider->setEnabled(manual);

    if (!m_pipeline->isRunning())
        return;

    // Mode first, then push the current slider values so manual takes effect
    // at the values the user can see.
    if (manualGoesThroughIq()) {
        scheduleManualApply();
        return;
    }

    aiq()->setAeMode(automatic ? AiqController::Mode::Auto : AiqController::Mode::Manual);

    if (manual) {
        aiq()->setExposureTimeMs(m_exposureSlider->value());
        aiq()->setAnalogueGain(m_analogueGainSlider->value());
        aiq()->setDigitalGain(m_digitalGainSlider->value());
    }
}

void MainWindow::onAutoWhiteBalanceToggled(bool automatic)
{
    const bool manual = !automatic && m_whiteBalanceGroup->isEnabled();

    m_wbByTemperature->setEnabled(manual);
    m_wbByRgbGain->setEnabled(manual);
    onWhiteBalanceModeToggled();

    if (!m_pipeline->isRunning())
        return;

    if (manualGoesThroughIq()) {
        scheduleManualApply();
        return;
    }

    if (automatic) {
        aiq()->setAwbMode(AiqController::Mode::Auto);
        return;
    }

    // Seed from what auto had converged to before switching, so the image holds
    // steady across the transition.
    const AiqController::RgbGain gain = aiq()->rgbGain();
    const int cct = aiq()->colourTemperature();

    m_redGainSlider->setValueSilently(gain.r);
    m_greenGainSlider->setValueSilently(gain.g);
    m_blueGainSlider->setValueSilently(gain.b);
    if (cct >= kColourTemperatureMin && cct <= kColourTemperatureMax)
        m_temperatureSlider->setValueSilently(cct);

    aiq()->setAwbMode(AiqController::Mode::Manual);

    if (m_wbByTemperature->isChecked())
        aiq()->setColourTemperature(int(m_temperatureSlider->value()));
    else
        aiq()->setRgbGain(gain);
}

void MainWindow::onWhiteBalanceModeToggled()
{
    const bool manual = !m_autoWhiteBalance->isChecked() && m_whiteBalanceGroup->isEnabled();
    const bool byTemperature = m_wbByTemperature->isChecked();

    m_temperatureSlider->setEnabled(manual && byTemperature);
    m_redGainSlider->setEnabled(manual && !byTemperature);
    m_greenGainSlider->setEnabled(manual && !byTemperature);
    m_blueGainSlider->setEnabled(manual && !byTemperature);

    if (!manual || !m_pipeline->isRunning())
        return;

    if (manualGoesThroughIq()) {
        scheduleManualApply();
        return;
    }

    if (byTemperature) {
        aiq()->setColourTemperature(int(m_temperatureSlider->value()));
    } else {
        AiqController::RgbGain gain;
        gain.r = float(m_redGainSlider->value());
        gain.g = float(m_greenGainSlider->value());
        gain.b = float(m_blueGainSlider->value());
        aiq()->setRgbGain(gain);
    }
}

void MainWindow::onAutoFocusToggled(bool automatic)
{
    const bool manual = !automatic && m_focusGroup->isEnabled();

    m_focusSlider->setEnabled(manual);
    m_zoomSlider->setEnabled(m_focusGroup->isEnabled());
    m_oneshotFocusButton->setEnabled(automatic && m_focusGroup->isEnabled());

    if (!m_pipeline->isRunning() || !m_focusGroup->isEnabled())
        return;

    aiq()->setAfMode(automatic ? AiqController::Mode::Auto : AiqController::Mode::Manual);

    if (manual)
        aiq()->setFocusPosition(int(m_focusSlider->value()));
}

void MainWindow::onAutoNoiseReductionToggled(bool automatic)
{
    const bool enabled = m_denoiseGroup->isEnabled();

    // Auto exposes one overall strength; manual splits into spatial and temporal.
    m_denoiseSlider->setEnabled(automatic && enabled);
    m_spatialDenoiseSlider->setEnabled(!automatic && enabled);
    m_temporalDenoiseSlider->setEnabled(!automatic && enabled);

    if (!m_pipeline->isRunning())
        return;

    aiq()->setNoiseReductionMode(automatic ? AiqController::Mode::Auto
                                           : AiqController::Mode::Manual);

    if (automatic) {
        aiq()->setNoiseReductionStrength(int(m_denoiseSlider->value()));
    } else {
        aiq()->setSpatialNoiseReduction(true, int(m_spatialDenoiseSlider->value()));
        aiq()->setTemporalNoiseReduction(true, int(m_temporalDenoiseSlider->value()));
    }
}

// --- status ---------------------------------------------------------------

void MainWindow::onStatusMessage(const QString& message)
{
    m_lastApiMessage = message;
}

void MainWindow::onPipelineError(const QString& message)
{
    m_lastApiMessage = message;
    statusBar()->showMessage(message);
    qWarning() << "Pipeline error:" << message;
}

void MainWindow::refreshStatusBar()
{
    if (!m_pipeline->isRunning()) {
        if (!m_lastApiMessage.isEmpty())
            statusBar()->showMessage(m_lastApiMessage);
        return;
    }

    const QString text = QStringLiteral("%1 fps  |  AE %2  |  %3")
                             .arg(m_preview->measuredFps(), 0, 'f', 1)
                             .arg(aiq()->aeConverged() ? QStringLiteral("converged")
                                                       : QStringLiteral("adjusting"),
                                  m_lastApiMessage);
    statusBar()->showMessage(text);
}
