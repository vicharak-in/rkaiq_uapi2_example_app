# Image processing and noise reduction

Implemented in `AiqController::setContrast()` and below.

## Colour processing

```c
XCamReturn rk_aiq_uapi2_setContrast(const rk_aiq_sys_ctx_t *ctx, unsigned int level);
XCamReturn rk_aiq_uapi2_setBrightness(const rk_aiq_sys_ctx_t *ctx, unsigned int level);
XCamReturn rk_aiq_uapi2_setSaturation(const rk_aiq_sys_ctx_t *ctx, unsigned int level);
XCamReturn rk_aiq_uapi2_setHue(const rk_aiq_sys_ctx_t *ctx, unsigned int level);
```

All four are 0–255 with 128 neutral, and all four drive the ISP's CPROC block.
Each has a matching getter. These ranges are fixed properties of the ISP module,
not sensor-dependent, which is why the app hard-codes them rather than querying.

In the IQ file they appear as four plain fields under `cproc.param`
(`brightness`, `contrast`, `saturation`, `hue`, plus `enable`), which is what
makes them straightforward to apply through the override path.

## Sharpening

```c
XCamReturn rk_aiq_uapi2_setSharpness(const rk_aiq_sys_ctx_t *ctx, unsigned int level);
XCamReturn rk_aiq_uapi2_getSharpness(const rk_aiq_sys_ctx_t *ctx, unsigned int *level);
```

0–100, where the app treats 50 as "as tuned". Unlike CPROC there is no single
sharpening field in the IQ file: `sharp_v4` is tuned per ISO across two sensor
modes (13 ISO entries × 2 settings on the imx519 file), each with its own
`sharp_ratio`. The override path scales every `sharp_ratio` by `level / 50`,
which preserves the shape of the tuned curve while moving its overall strength.

## Noise reduction

```c
XCamReturn rk_aiq_uapi2_setNRMode(const rk_aiq_sys_ctx_t *ctx, opMode_t mode);
XCamReturn rk_aiq_uapi2_setANRStrth(const rk_aiq_sys_ctx_t *ctx, unsigned int level);
XCamReturn rk_aiq_uapi2_setMSpaNRStrth(const rk_aiq_sys_ctx_t *ctx, bool on, unsigned int level);
XCamReturn rk_aiq_uapi2_setMTNRStrth(const rk_aiq_sys_ctx_t *ctx, bool on, unsigned int level);
```

`setANRStrth` is the overall automatic strength. The manual pair splits into
**spatial** (within a frame) and **temporal** (across frames) denoising; each
takes an `on` flag alongside its 0–100 level, so either can be disabled
independently.

Temporal denoising is the one that causes motion trails on moving subjects, so
it is worth being able to turn off separately.

## Mirror and flip

```c
XCamReturn rk_aiq_uapi2_setMirrorFlip(const rk_aiq_sys_ctx_t *ctx,
                                      bool mirror, bool flip, int skip_frm_cnt);
```

The third argument is how many frames to drop while the change settles; the app
passes 3. This is a sensor-level readout change, not an ISP transform.

## What works on this platform

| Control | Works? | How |
| --- | --- | --- |
| Contrast, brightness, saturation, hue | Yes | Through the IQ file (`cproc.param`) |
| Sharpness | Yes | Through the IQ file (`sharp_v4` `sharp_ratio` scaling) |
| Mirror / flip | Yes | Direct uAPI2 call |
| Noise reduction | **No** | Not yet implemented through the IQ file |

The uAPI2 setters for the first two rows return 0 and read back correctly, yet
never reach the picture. Setting brightness to 255 left frame luma at 119.5
against a baseline of 120.6. The cause is the missing statistics described in
[06-statistics.md](06-statistics.md).

Noise reduction is inert for the same reason and has **not** been given the
override treatment: `setANRStrth(0)` and `setANRStrth(100)` produce identical
luma spread (70.9 either way). Fixing it means scaling strength fields across
the per-ISO `ynr_v3`, `cnr_v2`, `bayer2dnr_v2` and `bayertnr_v2` tables, the
same approach used for sharpening but across four modules.
