# Documentation index

Reference for every rkaiq API this application uses, plus the platform
behaviour you need to know to make sense of it.

| Document | Covers |
| --- | --- |
| [01-engine-lifecycle.md](01-engine-lifecycle.md) | Discovery and the init → prepare → start → stop → deinit sequence |
| [02-exposure.md](02-exposure.md) | Auto and manual exposure, gain, frame rate, anti-flicker |
| [03-white-balance.md](03-white-balance.md) | Auto white balance, manual gains, colour temperature |
| [04-focus-and-zoom.md](04-focus-and-zoom.md) | Focus modes, VCM position, one-shot AF, zoom |
| [05-image-and-noise.md](05-image-and-noise.md) | Contrast, brightness, saturation, hue, sharpness, denoise, mirror/flip |
| [06-statistics.md](06-statistics.md) | 3A statistics, and why their absence breaks every algorithm-driven call |
| [07-iq-file-override.md](07-iq-file-override.md) | Applying controls through the IQ file when statistics never arrive |
| [08-platform-notes.md](08-platform-notes.md) | Engine ownership, the lock-file crash, the imx519 kernel patch, open issues |

## Headers

Only three rkaiq headers are needed, and they are included with `extern "C"`
because the library is C:

| Header | Provides |
| --- | --- |
| `rk_aiq_user_api2_sysctl.h` | Engine lifecycle, sensor enumeration, 3A statistics, version info |
| `rk_aiq_user_api2_imgproc.h` | The one-call setters: exposure/WB/focus modes, ranges, CPROC, sharpness, denoise, mirror/flip |
| `rk_aiq_user_api2_ae.h` | The AE attribute struct (`Uapi_ExpSwAttrV2_t`) for manual exposure and frame rate |

`rk-camera-module.h` from the kernel UAPI is also used, for
`RKMODULE_GET_MODULE_INFO` when resolving the IQ file name.

Do **not** include `uAPI2/rk_aiq_user_api2_wrapper.h`. It pulls in a header
that is not installed, so it will not compile.

## Conventions used throughout

Every uAPI2 call returns `XCamReturn`, where `XCAM_RETURN_NO_ERROR` (0) means
success and negative values are errors. **A success return does not mean the
value reached the picture.** That distinction matters more here than on a
healthy board, and [06-statistics.md](06-statistics.md) explains why.

Three different auto/manual enums exist and they disagree on their numeric
values:

| Enum | AUTO | MANUAL |
| --- | --- | --- |
| `opMode_t` | 0 | 1 |
| `RKAiqOPMode_t` | 1 | 2 |
| `rk_aiq_wb_op_mode_t` | 1 | **0** |

Mixing them compiles silently and selects the wrong mode. `AiqController`
exposes a single `Mode` enum and converts at each call site to keep that
mistake impossible to make from the UI layer.
