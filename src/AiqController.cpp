#include "AiqController.h"

#include <QDebug>

extern "C" {
#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_user_api2_imgproc.h"
#include "rk_aiq_user_api2_sysctl.h"
}

namespace {

// The three auto/manual enums rkaiq uses do not share numeric values, so each
// conversion is spelled out rather than cast. See the class comment.
opMode_t toOpMode(AiqController::Mode mode)
{
    switch (mode) {
    case AiqController::Mode::Manual:   return OP_MANUAL;
    case AiqController::Mode::SemiAuto: return OP_SEMI_AUTO;
    case AiqController::Mode::Auto:     break;
    }
    return OP_AUTO;
}

RKAiqOPMode_t toAecOpMode(AiqController::Mode mode)
{
    return (mode == AiqController::Mode::Auto) ? RK_AIQ_OP_MODE_AUTO
                                               : RK_AIQ_OP_MODE_MANUAL;
}

rk_aiq_working_mode_t toWorkingMode(AiqController::WorkingMode mode)
{
    switch (mode) {
    case AiqController::WorkingMode::Hdr2: return RK_AIQ_WORKING_MODE_ISP_HDR2;
    case AiqController::WorkingMode::Hdr3: return RK_AIQ_WORKING_MODE_ISP_HDR3;
    case AiqController::WorkingMode::Normal: break;
    }
    return RK_AIQ_WORKING_MODE_NORMAL;
}

const char* toSceneName(AiqController::WorkingMode mode)
{
    return (mode == AiqController::WorkingMode::Normal) ? "normal" : "hdr";
}

} // namespace

AiqController::AiqController(QObject* parent)
    : QObject(parent)
{
}

AiqController::~AiqController()
{
    stop();
    deinit();
}

bool AiqController::check(int ret, const char* function)
{
    if (ret == XCAM_RETURN_NO_ERROR) {
        emit apiCallSucceeded(QString::fromLatin1(function));
        return true;
    }

    qWarning() << "rkaiq:" << function << "failed, ret =" << ret;
    emit apiCallFailed(QString::fromLatin1(function), ret);
    return false;
}

// --- lifecycle ------------------------------------------------------------

bool AiqController::init(const QString& sensorEntity, const QString& iqFileDir, WorkingMode mode)
{
    if (m_ctx) {
        qWarning() << "AiqController::init called while already initialised";
        return false;
    }

    m_sensorEntity = sensorEntity;

    const QByteArray entity = sensorEntity.toLocal8Bit();
    const QByteArray iqDir  = iqFileDir.toLocal8Bit();

    // Scene must be selected before init, and must agree with the working mode
    // later handed to prepare().
    //
    // Deliberately not routed through check(): this runs before the IQ file is
    // parsed, so rkaiq logs "invalid main scene len!" and may report an error
    // even on a perfectly good tuning file. rkisp_demo makes the identical call
    // at the identical point and sees the same thing, and init still succeeds,
    // so it is logged rather than surfaced to the user as a failure.
    const XCamReturn sceneRet =
        rk_aiq_uapi2_sysctl_preInit_scene(entity.constData(), toSceneName(mode), "day");
    if (sceneRet != XCAM_RETURN_NO_ERROR) {
        qInfo() << "rkaiq: preInit_scene returned" << sceneRet
                << "- continuing, the IQ file selects the scene itself";
    }

    // iqFileDir is a directory: rkaiq matches the sensor to a .json itself, so
    // no filename is ever hard-coded here.
    m_ctx = rk_aiq_uapi2_sysctl_init(entity.constData(), iqDir.constData(), nullptr, nullptr);

    if (!m_ctx) {
        qWarning() << "rkaiq: sysctl_init returned null for" << sensorEntity
                   << "with IQ dir" << iqFileDir;
        emit apiCallFailed(QStringLiteral("sysctl_init"), -1);
        return false;
    }

    return true;
}

bool AiqController::prepare(int width, int height, WorkingMode mode)
{
    if (!m_ctx)
        return false;

    // width/height must match what is later given to VIDIOC_S_FMT.
    return check(rk_aiq_uapi2_sysctl_prepare(m_ctx, static_cast<uint32_t>(width),
                                             static_cast<uint32_t>(height),
                                             toWorkingMode(mode)),
                 "sysctl_prepare");
}

bool AiqController::start()
{
    if (!m_ctx || m_running)
        return false;

    if (!check(rk_aiq_uapi2_sysctl_start(m_ctx), "sysctl_start"))
        return false;

    m_running = true;
    return true;
}

void AiqController::stop()
{
    if (!m_ctx || !m_running)
        return;

    check(rk_aiq_uapi2_sysctl_stop(m_ctx, false), "sysctl_stop");
    m_running = false;
}

void AiqController::deinit()
{
    if (!m_ctx)
        return;

    rk_aiq_uapi2_sysctl_deinit(m_ctx);
    m_ctx = nullptr;
}

QString AiqController::versionInfo() const
{
    rk_aiq_ver_info_t vers {};
    rk_aiq_uapi2_get_version_info(&vers);
    return QString::fromLatin1(vers.aiq_ver);
}

// --- auto exposure --------------------------------------------------------

bool AiqController::statisticsAvailable()
{
    if (!m_ctx || !m_running)
        return false;

    // A couple of attempts, because the very first frames after start can be
    // late even on a healthy engine.
    for (int attempt = 0; attempt < 3; ++attempt) {
        rk_aiq_isp_stats_t* stats = nullptr;
        if (rk_aiq_uapi2_sysctl_get3AStatsBlk(m_ctx, &stats, 500) == XCAM_RETURN_NO_ERROR
            && stats != nullptr) {
            rk_aiq_uapi2_sysctl_release3AStatsRef(m_ctx, stats);
            return true;
        }
    }
    return false;
}

bool AiqController::setAeMode(Mode mode)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setExpMode(m_ctx, toOpMode(mode)), "setExpMode");
}

// Exposure time, analogue gain and ISP digital gain all live in the same AE
// attribute, so each setter is a get-modify-set of that one struct. The sync
// block must be filled in explicitly or it is left holding garbage.
bool AiqController::setExposureTimeMs(double milliseconds)
{
    if (!m_ctx)
        return false;

    Uapi_ExpSwAttrV2_t attr {};
    if (!check(rk_aiq_user_api2_ae_getExpSwAttr(m_ctx, &attr), "ae_getExpSwAttr"))
        return false;

    attr.sync.sync_mode = RK_AIQ_UAPI_MODE_DEFAULT;
    attr.sync.done      = false;
    attr.AecOpType      = RK_AIQ_OP_MODE_MANUAL;
    attr.stManual.LinearAE.ManualTimeEn = true;
    attr.stManual.LinearAE.TimeValue    = static_cast<float>(milliseconds / 1000.0);

    return check(rk_aiq_user_api2_ae_setExpSwAttr(m_ctx, attr), "ae_setExpSwAttr(time)");
}

bool AiqController::setAnalogueGain(double gain)
{
    if (!m_ctx)
        return false;

    Uapi_ExpSwAttrV2_t attr {};
    if (!check(rk_aiq_user_api2_ae_getExpSwAttr(m_ctx, &attr), "ae_getExpSwAttr"))
        return false;

    attr.sync.sync_mode = RK_AIQ_UAPI_MODE_DEFAULT;
    attr.sync.done      = false;
    attr.AecOpType      = RK_AIQ_OP_MODE_MANUAL;
    attr.stManual.LinearAE.ManualGainEn = true;
    attr.stManual.LinearAE.GainValue    = static_cast<float>(gain);

    return check(rk_aiq_user_api2_ae_setExpSwAttr(m_ctx, attr), "ae_setExpSwAttr(gain)");
}

bool AiqController::setDigitalGain(double gain)
{
    if (!m_ctx)
        return false;

    Uapi_ExpSwAttrV2_t attr {};
    if (!check(rk_aiq_user_api2_ae_getExpSwAttr(m_ctx, &attr), "ae_getExpSwAttr"))
        return false;

    attr.sync.sync_mode = RK_AIQ_UAPI_MODE_DEFAULT;
    attr.sync.done      = false;
    attr.AecOpType      = RK_AIQ_OP_MODE_MANUAL;
    attr.stManual.LinearAE.ManualIspDgainEn = true;
    attr.stManual.LinearAE.IspDGainValue    = static_cast<float>(gain);

    return check(rk_aiq_user_api2_ae_setExpSwAttr(m_ctx, attr), "ae_setExpSwAttr(dgain)");
}

// Frame rate is an AE attribute, not a sysctl call: it takes effect live, with
// no pipeline restart. Note this caps AE's exposure-driven rate; it does not
// change the sensor's V4L2 mode, which is what the Apply button is for.
bool AiqController::setFrameRate(double fps)
{
    if (!m_ctx)
        return false;

    Uapi_ExpSwAttrV2_t attr {};
    if (!check(rk_aiq_user_api2_ae_getExpSwAttr(m_ctx, &attr), "ae_getExpSwAttr"))
        return false;

    attr.sync.sync_mode = RK_AIQ_UAPI_MODE_DEFAULT;
    attr.sync.done      = false;
    attr.stAuto.stFrmRate.isFpsFix = (fps > 0.0);
    attr.stAuto.stFrmRate.FpsValue = static_cast<float>(fps);

    return check(rk_aiq_user_api2_ae_setExpSwAttr(m_ctx, attr), "ae_setExpSwAttr(fps)");
}

bool AiqController::setAeLock(bool locked)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setAeLock(m_ctx, locked), "setAeLock");
}

bool AiqController::setPowerLineFrequency(int hz)
{
    if (!m_ctx)
        return false;

    expPwrLineFreq_t freq = EXP_PWR_LINE_FREQ_DIS;
    if (hz == 50)
        freq = EXP_PWR_LINE_FREQ_50HZ;
    else if (hz == 60)
        freq = EXP_PWR_LINE_FREQ_60HZ;

    return check(rk_aiq_uapi2_setExpPwrLineFreqMode(m_ctx, freq), "setExpPwrLineFreqMode");
}

// Slider bounds come from the sensor's own IQ file rather than a constant.
// paRange_t declares max before min, so the fields are named explicitly.
AiqController::Range AiqController::exposureTimeRangeMs() const
{
    Range range;
    if (!m_ctx)
        return range;

    paRange_t seconds {};
    if (rk_aiq_uapi2_getExpTimeRange(m_ctx, &seconds) != XCAM_RETURN_NO_ERROR)
        return range;

    range.min   = seconds.min * 1000.0f;
    range.max   = seconds.max * 1000.0f;
    range.valid = range.max > range.min;
    return range;
}

AiqController::Range AiqController::analogueGainRange() const
{
    Range range;
    if (!m_ctx)
        return range;

    paRange_t gain {};
    if (rk_aiq_uapi2_getExpGainRange(m_ctx, &gain) != XCAM_RETURN_NO_ERROR)
        return range;

    range.min   = gain.min;
    range.max   = gain.max;
    range.valid = range.max > range.min;
    return range;
}

bool AiqController::aeConverged() const
{
    if (!m_ctx)
        return false;

    Uapi_ExpQueryInfo_t info {};
    if (rk_aiq_user_api2_ae_queryExpResInfo(m_ctx, &info) != XCAM_RETURN_NO_ERROR)
        return false;

    return info.IsConverged;
}

// --- auto white balance ---------------------------------------------------

bool AiqController::setAwbMode(Mode mode)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setWBMode(m_ctx, toOpMode(mode)), "setWBMode");
}

// rk_aiq_wb_gain_t carries four channels; the UI offers three, so the two green
// taps are driven together.
bool AiqController::setRgbGain(const RgbGain& gain)
{
    if (!m_ctx)
        return false;

    rk_aiq_wb_gain_t wb {};
    wb.rgain  = gain.r;
    wb.grgain = gain.g;
    wb.gbgain = gain.g;
    wb.bgain  = gain.b;

    return check(rk_aiq_uapi2_setMWBGain(m_ctx, &wb), "setMWBGain");
}

bool AiqController::setColourTemperature(int kelvin)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setMWBCT(m_ctx, static_cast<unsigned int>(kelvin)), "setMWBCT");
}

AiqController::RgbGain AiqController::rgbGain() const
{
    RgbGain gain;
    if (!m_ctx)
        return gain;

    rk_aiq_wb_gain_t wb {};
    if (rk_aiq_uapi2_getWBGain(m_ctx, &wb) != XCAM_RETURN_NO_ERROR)
        return gain;

    gain.r = wb.rgain;
    gain.g = (wb.grgain + wb.gbgain) / 2.0f;
    gain.b = wb.bgain;
    return gain;
}

int AiqController::colourTemperature() const
{
    if (!m_ctx)
        return 0;

    unsigned int cct = 0;
    if (rk_aiq_uapi2_getWBCT(m_ctx, &cct) != XCAM_RETURN_NO_ERROR)
        return 0;

    return static_cast<int>(cct);
}

// --- auto focus -----------------------------------------------------------

bool AiqController::setAfMode(Mode mode)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setFocusMode(m_ctx, toOpMode(mode)), "setFocusMode");
}

bool AiqController::setFocusPosition(int code)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setFocusPosition(m_ctx, static_cast<short>(code)),
                 "setFocusPosition");
}

bool AiqController::triggerOneshotFocus()
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_oneshotFocus(m_ctx), "oneshotFocus");
}

AiqController::IntRange AiqController::focusRange() const
{
    IntRange range;
    if (!m_ctx)
        return range;

    rk_aiq_af_focusrange focus {};
    if (rk_aiq_uapi2_getFocusRange(m_ctx, &focus) != XCAM_RETURN_NO_ERROR)
        return range;

    range.min   = focus.min_pos;
    range.max   = focus.max_pos;
    range.valid = range.max > range.min;
    return range;
}

AiqController::IntRange AiqController::zoomRange() const
{
    IntRange range;
    if (!m_ctx)
        return range;

    rk_aiq_af_zoomrange zoom {};
    if (rk_aiq_uapi2_getZoomRange(m_ctx, &zoom) != XCAM_RETURN_NO_ERROR)
        return range;

    range.min   = zoom.min_pos;
    range.max   = zoom.max_pos;
    range.valid = range.max > range.min;
    return range;
}

bool AiqController::setZoomPosition(int position)
{
    if (!m_ctx)
        return false;

    if (!check(rk_aiq_uapi2_setOpZoomPosition(m_ctx, position), "setOpZoomPosition"))
        return false;

    return check(rk_aiq_uapi2_endOpZoomChange(m_ctx), "endOpZoomChange");
}

// --- image adjustment -----------------------------------------------------

bool AiqController::setContrast(int level)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setContrast(m_ctx, static_cast<unsigned int>(level)), "setContrast");
}

bool AiqController::setBrightness(int level)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setBrightness(m_ctx, static_cast<unsigned int>(level)), "setBrightness");
}

bool AiqController::setSaturation(int level)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setSaturation(m_ctx, static_cast<unsigned int>(level)), "setSaturation");
}

bool AiqController::setHue(int level)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setHue(m_ctx, static_cast<unsigned int>(level)), "setHue");
}

bool AiqController::setSharpness(int level)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setSharpness(m_ctx, static_cast<unsigned int>(level)), "setSharpness");
}

bool AiqController::setMirrorFlip(bool mirror, bool flip)
{
    if (!m_ctx)
        return false;
    // Third argument is the frame skip count; rkisp_demo passes 3.
    return check(rk_aiq_uapi2_setMirrorFlip(m_ctx, mirror, flip, 3), "setMirrorFlip");
}

// --- denoise --------------------------------------------------------------
//
// These imgproc entry points are deliberately preferred over the per-module NR
// APIs (aynrV3 / acnrV30 / abayertnrV30 / ...), which are versioned per ISP
// generation and would pin the app to one SoC.

bool AiqController::setNoiseReductionMode(Mode mode)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setNRMode(m_ctx, toOpMode(mode)), "setNRMode");
}

bool AiqController::setNoiseReductionStrength(int level)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setANRStrth(m_ctx, static_cast<unsigned int>(level)), "setANRStrth");
}

bool AiqController::setSpatialNoiseReduction(bool on, int level)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setMSpaNRStrth(m_ctx, on, static_cast<unsigned int>(level)),
                 "setMSpaNRStrth");
}

bool AiqController::setTemporalNoiseReduction(bool on, int level)
{
    if (!m_ctx)
        return false;
    return check(rk_aiq_uapi2_setMTNRStrth(m_ctx, on, static_cast<unsigned int>(level)),
                 "setMTNRStrth");
}
