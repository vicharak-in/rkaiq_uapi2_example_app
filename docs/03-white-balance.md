# White balance

Implemented in `AiqController::setAwbMode()` and below.

## Mode

```c
XCamReturn rk_aiq_uapi2_setWBMode(const rk_aiq_sys_ctx_t *ctx, opMode_t mode);
```

Takes `opMode_t` (`OP_AUTO` = 0, `OP_MANUAL` = 1) like the exposure mode call.

Do not confuse this with `rk_aiq_wb_op_mode_t`, which appears elsewhere in the
white-balance headers and is **inverted**: `RK_AIQ_WB_MODE_MANUAL` = 0,
`RK_AIQ_WB_MODE_AUTO` = 1. Passing one where the other is expected compiles
cleanly and silently selects the wrong mode.

## Manual, by RGB gain

```c
XCamReturn rk_aiq_uapi2_setMWBGain(const rk_aiq_sys_ctx_t *ctx, rk_aiq_wb_gain_t *gain);
XCamReturn rk_aiq_uapi2_getWBGain(const rk_aiq_sys_ctx_t *ctx, rk_aiq_wb_gain_t *gain);
```

`rk_aiq_wb_gain_t` carries four channels, not three:

```c
typedef struct {
    float rgain;
    float grgain;   /* green on red rows */
    float gbgain;   /* green on blue rows */
    float bgain;
} rk_aiq_wb_gain_t;
```

The UI offers a single green slider, so `grgain` and `gbgain` are both set from
it. Splitting them is only useful for correcting a sensor with asymmetric green
response.

## Manual, by colour temperature

```c
XCamReturn rk_aiq_uapi2_setMWBCT(const rk_aiq_sys_ctx_t *ctx, unsigned int ct);
XCamReturn rk_aiq_uapi2_getWBCT(const rk_aiq_sys_ctx_t *ctx, unsigned int *ct);
```

Kelvin. The app exposes 2000–10000 K. This and the gain form are mutually
exclusive ways of driving manual WB, which is why the UI radio-selects between
them rather than showing both as live.

`getWBGain()` and `getWBCT()` are also used when switching from auto to manual,
to seed the controls with whatever auto had converged to. That keeps the picture
steady across the transition instead of jumping.

> **On this platform these calls do nothing**, for the same reason manual
> exposure does not. The app writes white balance through the IQ file instead;
> see
> [07-iq-file-override.md](07-iq-file-override.md). Manual gains there land in
> `wb_v21.manualPara.cfg.mwbGain` as `[R, Gr, Gb, B]`, and colour temperature in
> `wb_v21.manualPara.cfg.cct.CCT`, with `manualPara.mode` selecting which of the
> two rkaiq reads.
