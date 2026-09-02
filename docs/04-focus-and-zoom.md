# Focus and zoom

Implemented in `AiqController::setAfMode()` and below. These controls are only
meaningful when the module actually has the hardware, which is why the app gates
them on `rk_aiq_static_info_t::has_lens_vcm` rather than offering them always.

## Focus mode

```c
XCamReturn rk_aiq_uapi2_setFocusMode(const rk_aiq_sys_ctx_t *ctx, opMode_t mode);
```

`OP_AUTO` runs continuous AF; `OP_MANUAL` hands control to
`setFocusPosition()`. `OP_SEMI_AUTO` exists and is what one-shot focus uses.

## Manual position

```c
XCamReturn rk_aiq_uapi2_setFocusPosition(const rk_aiq_sys_ctx_t *ctx, short code);
XCamReturn rk_aiq_uapi2_getFocusRange(const rk_aiq_sys_ctx_t *ctx,
                                      rk_aiq_af_focusrange *range);
```

The position is a raw VCM DAC code, not a distance, and its meaning is specific
to the lens module. Always take the bounds from `getFocusRange()`, which fills
in `min` and `max` on an `rk_aiq_af_focusrange`, rather than assuming a range.

## One-shot autofocus

```c
XCamReturn rk_aiq_uapi2_oneshotFocus(const rk_aiq_sys_ctx_t *ctx);
```

Runs a single search and then holds. The app switches the mode to semi-auto
before calling it, which is what makes "focus once, then stay put" behave the
way users expect.

## Zoom

```c
XCamReturn rk_aiq_uapi2_getZoomRange(const rk_aiq_sys_ctx_t *ctx,
                                     rk_aiq_af_zoomrange *range);
XCamReturn rk_aiq_uapi2_setOpZoomPosition(const rk_aiq_sys_ctx_t *ctx, int pos);
XCamReturn rk_aiq_uapi2_endOpZoomChange(const rk_aiq_sys_ctx_t *ctx);
```

Zoom is a two-step call: set the position, then `endOpZoomChange()` to commit
it. Omitting the second call leaves the move uncommitted.

This is optical zoom on a motorised lens, not a digital crop. Most modules,
including every one tested here, have no zoom motor and report an empty range,
and the app hides the control in that case.

## On the imx519 module

The attached module pairs the sensor with an `ak7375` VCM, and the media graph
shows it as a lens subdev. But rkaiq reports `has_lens_vcm = 0` for it, because
the VCM has no pads and no links in the media graph, so the engine does not
associate it with the sensor. The focus controls therefore stay disabled.
Forcing them on would not help: rkaiq's AF algorithm has no lens to drive, so
they would be a second set of dead controls.
