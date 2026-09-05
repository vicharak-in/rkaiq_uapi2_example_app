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

## Why rkaiq does not see the motor, and what the app does instead

The attached imx519 module pairs the sensor with an `ak7375` VCM. The device tree
binds them correctly:

```dts
cam0_vcm_0c: cam0-vcm@0c { compatible = "asahi-kasei,ak7375"; reg = <0x0c>; };
cam0_imx519_1a: cam0-imx519@1a { ... lens-focus = <&cam0_vcm_0c>; ... };
```

Yet rkaiq reports `has_lens_vcm = 0`. The reason is that rkaiq identifies the lens purely
by **media entity name** (`CamHwIsp20.cpp:1039-1055`):

```c
if (entity_info->type == MEDIA_ENT_T_V4L2_SUBDEV_LENS) {
    if ((entity_info->name[0] == 'm') &&
        (strncmp(entity_info->name, s_info->module_index_str.c_str(), 3) == 0))
        s_info->module_lens_dev_name = media_entity_get_devname(entity);
}
```

`module_index_str` is `"m00"`, taken from the sensor entity `m00_f_imx519 2-001a`. Our VCM
is named `ak7375 2-000c`, so the test fails, the name stays empty, `has_lens_vcm` is false
(`CamHwIsp20.cpp:1220`) and no `LensHw` is ever constructed (`CamHwIsp20.cpp:1542`). rkaiq
therefore never writes the lens at all.

Rockchip's own VCM drivers, `fp5510.c:703-744` for instance, read
`rockchip,camera-module-index` and `-facing` from the device tree and name the subdev
`m00_f_fp5510 1-000c`. `ak7375.c` and `dw9807-vcm.c` are pristine upstream drivers that do
not, and none of the camera overlays carry those two properties on their VCM nodes.

Note that "0 pads, 0 links" in `media-ctl` output is **not** the problem. Every VCM driver
on this platform registers with zero pads, because a lens is not in the data path.

### The app drives the motor itself

`LensController` writes `V4L2_CID_FOCUS_ABSOLUTE` on the lens subdev directly. rkaiq is not
touching that control, so there is nothing to contend with.

Finding the motor is generic, with no sensor names anywhere
(`CameraEnumerator::lensSubdevFor()`). The device tree is the authority: a sensor node
carries `lens-focus` holding its VCM's phandle, and sysfs exposes both that property and
every subdev's own phandle, so the two can be matched directly:

```
/sys/class/video4linux/v4l-subdev2/device/of_node/lens-focus   -> phandle 0x4ac
/sys/class/video4linux/v4l-subdev3/device/of_node/phandle      -> 0x4ac      match
```

If the device tree says nothing, it falls back to any subdev that answers
`VIDIOC_QUERYCTRL` for `V4L2_CID_FOCUS_ABSOLUTE`. The position range always comes from the
driver; VCMs differ widely (0..4095 raw on ak7375, 0..64 logical on Rockchip's own drivers)
and guessing would put the lens somewhere arbitrary.

### Autofocus is done in the app

rkaiq's AF cannot run here for the same reason AE and AWB cannot: it bypasses whenever ISP
statistics are missing (`RkAiqAfHandle.cpp:516`, `"no af stats, ignore!"`). So
`ContrastAutoFocus` sweeps the lens and keeps the sharpest position, scoring the preview
frames the app already decodes.

* **Score.** Mean absolute luma gradient over the middle third of the frame. Centre-weighted
  because that is what people point at, and it keeps a busy background from winning.
* **Sweep.** A coarse pass of 9 positions, then a fine pass of 6 across one coarse step
  either side of the winner.
* **Averaging.** Three frames per position. The peak is shallow on a dim or low-contrast
  scene, shallow enough that single-frame noise picks the wrong winner.
* **Confidence.** If the best score is less than 1.12x the flattest point of the sweep,
  there is no real peak, so the lens is returned to where it started and the status bar
  says "Not enough detail to focus on". Moving the lens on noise is worse than not moving.
* **Continuous mode.** After parking, it keeps scoring and re-sweeps when sharpness stays
  below 70% of the parked value for twelve frames running. The gap between the two stops a
  passing hand from triggering a whole sweep.

Everything is driven by arriving frames rather than by sleeping, so a sweep never blocks
the UI and always advances at whatever rate the pipeline is actually delivering.

**On timing.** A sweep costs `(9 + 6) * (2 + 3) = 75` frames. The preview here runs at
roughly 9 fps, so focusing takes about eight seconds; on a pipeline delivering 30 fps it
would be under three. A driver implementing `RK_VIDIOC_VCM_TIMEINFO` would let the settle
wait be exact rather than a fixed two frames, which is the main thing holding this back.

Measured on the imx519, sweeping from both ends of the range four times, against a manual
best of 1488: the search landed at 1430, 1430, 1838 and 1430.

### If you want rkaiq to own focus instead

Two kernel changes are needed, and neither is in this repository:

1. Add `rockchip,camera-module-index` and `rockchip,camera-module-facing` to the VCM nodes
   in the camera overlays, matching the sensor they belong to.
2. Teach `ak7375.c` and `dw9807-vcm.c` the Rockchip subdev naming
   (`m%02d_%s_%s %s`), and implement `RK_VIDIOC_VCM_TIMEINFO`, which `LensHw.cpp:446`
   issues after every focus write and treats failure as an error.

With those in place `has_lens_vcm` becomes true, a `LensHw` is created, and a fixed position
written into the IQ file's `af_v30` section would be applied at `sysctl_prepare()` time
without needing statistics, because focus results bypass the ISP params assembler and go
straight to `VIDIOC_S_CTRL` (`CamHwIsp20.cpp:5636`).
