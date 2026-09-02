# Exposure

Implemented in `AiqController::setAeMode()` and the setters below it.

## Mode

```c
XCamReturn rk_aiq_uapi2_setExpMode(const rk_aiq_sys_ctx_t *ctx, opMode_t mode);
```

`opMode_t` is `OP_AUTO` (0) / `OP_MANUAL` (1). Beware: this is *not* the same
numbering as `RKAiqOPMode_t`, which the attribute struct below uses.

```c
XCamReturn rk_aiq_uapi2_setAeLock(const rk_aiq_sys_ctx_t *ctx, bool on);
```

Freezes AE at its current exposure while leaving it in auto mode.

## Ranges

```c
XCamReturn rk_aiq_uapi2_getExpTimeRange(const rk_aiq_sys_ctx_t *ctx, paRange_t *time);
XCamReturn rk_aiq_uapi2_getExpGainRange(const rk_aiq_sys_ctx_t *ctx, paRange_t *gain);
```

`paRange_t` is `{ float min; float max; }`. **Exposure time is in seconds**, so
`AiqController::exposureTimeRangeMs()` multiplies by 1000 for the UI. Never
hard-code slider bounds. Ask the engine, because the limits depend on both the
sensor and the current mode. On an imx519 at 1280x720 it reports an exposure
range of `0.000237` to `0.050000` seconds and a gain range of `1` to `22`.

## Manual values

All three manual quantities live in one attribute struct, so each setter is a
get-modify-set:

```c
XCamReturn rk_aiq_user_api2_ae_getExpSwAttr(const rk_aiq_sys_ctx_t *ctx,
                                            Uapi_ExpSwAttrV2_t *pExpSwAttr);
XCamReturn rk_aiq_user_api2_ae_setExpSwAttr(const rk_aiq_sys_ctx_t *ctx,
                                            const Uapi_ExpSwAttrV2_t expSwAttr);
```

The fields that matter for linear (non-HDR) mode:

```c
attr.sync.sync_mode = RK_AIQ_UAPI_MODE_DEFAULT;
attr.sync.done      = false;
attr.AecOpType      = RK_AIQ_OP_MODE_MANUAL;      /* RKAiqOPMode_t, MANUAL = 2 */

attr.stManual.LinearAE.ManualTimeEn     = true;
attr.stManual.LinearAE.TimeValue        = 0.030f; /* seconds */
attr.stManual.LinearAE.ManualGainEn     = true;
attr.stManual.LinearAE.GainValue        = 8.0f;   /* analogue gain, linear */
attr.stManual.LinearAE.ManualIspDgainEn = true;
attr.stManual.LinearAE.IspDGainValue    = 1.0f;   /* ISP digital gain */
```

The `sync` block must be filled in explicitly. Leaving it as read can hand the
engine garbage.

> **On this platform these calls do nothing.** They return 0 and read back
> correctly, but the value never reaches the sensor, because rkaiq applies
> manual exposure on the AE algorithm's per-frame pass and that pass needs
> statistics which never arrive. See [06-statistics.md](06-statistics.md) for
> the evidence and [07-iq-file-override.md](07-iq-file-override.md) for the
> route the app uses instead.

## Frame rate

Frame rate is also part of the AE attribute:

```c
attr.stAuto.stFrmRate.isFpsFix = true;
attr.stAuto.stFrmRate.FpsValue = 30.0f;
```

Unlike resolution, this takes effect without restarting the pipeline.

## Anti-flicker

```c
XCamReturn rk_aiq_uapi2_setExpPwrLineFreqMode(const rk_aiq_sys_ctx_t *ctx,
                                              expPwrLineFreq_t freq);
```

`EXP_PWR_LINE_FREQ_DIS`, `EXP_PWR_LINE_FREQ_50HZ` or `EXP_PWR_LINE_FREQ_60HZ`.
Constrains exposure times to multiples of the mains period so artificial
lighting does not band.

## Reading back what AE decided

```c
XCamReturn rk_aiq_user_api2_ae_queryExpResInfo(const rk_aiq_sys_ctx_t *ctx,
                                               Uapi_ExpQueryInfo_t *pExpResInfo);
```

`Uapi_ExpQueryInfo_t::IsConverged` drives the "AE converged" indicator in the
status bar. On a board with no statistics this never becomes true.
