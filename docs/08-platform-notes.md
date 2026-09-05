# Platform notes

Board-level behaviour that shapes how the application has to be written.

## Only one process may own the rkaiq engine

`rkaiq_3A_server` (the `rkaiq_3A.service` daemon) owns the AIQ engine for the
whole system. This app owns it too, and the two cannot coexist: a second
`rk_aiq_uapi2_sysctl_init()` on the same sensor does not return an error, it
**blocks forever**.

The app therefore checks before initialising, finding which process holds the
ISP statistics / input-params nodes, and refuses to start with a message naming
the culprit:

```
Another process is already running the rkaiq engine: rkaiq_3A_server (pid 26148),
holding /dev/video19. Only one owner is allowed, and starting a second would hang.
Stop it first, e.g. 'sudo systemctl stop rkaiq_3A.service' or 'sudo killall rkaiq_3A_server'.
```

Stop the daemon and press **Retry**. The Apply button turns into Retry when
nothing is streaming, so there is no need to restart the app.

**You do not need to run `rkaiq_3A_server` alongside this app.** The app *is*
the 3A client; the `XCORE:K:cid[0] rk_aiq_uapi_sysctl_init/prepare/start`
lines in its output are it running the engine.

A `SIGKILL`ed instance of this app leaves the ISP claimed too, since teardown
cannot run, and will then be reported as the conflicting owner. Always exit via
the window close button or Ctrl-C.

## The /tmp/aiq0.lock landmine

rkaiq serialises engine startup on `/tmp/aiq<N>.lock`. It opens that file with
`fopen()` and passes the result straight to `fileno()` with no NULL check, so a
lock file it cannot open is not an error. It is a segfault inside the library,
with nothing printed:

```
Thread 1 "camera-ctls" received signal SIGSEGV, Segmentation fault.
0x0000007ff63c8618 in __GI___fileno (fp=0x0) at ./libio/fileno.c:35
#1  0x0000007ff75c3a58 in ?? () from /lib/librkaiq.so
#2  AiqController::init(...)
```

The file is created by whoever starts the engine first and keeps that owner, so
running once with `sudo` and once without triggers it in either direction. This
is not specific to this app. Anything using librkaiq crashes the same way,
including `rkaiq_3A_server`.

The app checks the lock before calling into the engine and reports it instead:

```
rkaiq's lock file /tmp/aiq0.lock belongs to root and this process cannot write
it, which makes the engine crash rather than report an error. It is a leftover,
safe to delete: 'sudo rm /tmp/aiq0.lock'.
```

The simplest habit that avoids it: run the app consistently as one user.

## Sensors rkaiq cannot describe

rkaiq builds its mode table in `CamHwIsp20.cpp:get_sensor_caps()`, and that
function reads modes from exactly one source:
`VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL`. Its `VIDIOC_SUBDEV_ENUM_FRAME_SIZE` path
is compiled out behind `#if 0`, commented "TODO: sensor driver not supported
now". A driver without `.enum_frame_interval` therefore gets no modes, however
well it describes itself otherwise.

When rkaiq reports no modes, the app reads them off the sensor subdev itself
(`CameraEnumerator::enumerateSubdevModes()`), found by matching the entity name
under `/sys/class/video4linux`. Frame rates come from `ENUM_FRAME_INTERVAL`
where implemented, otherwise from `G_FRAME_INTERVAL` for the current mode;
anything else is reported as "Sensor default" and no rate is forced.

This is a fallback rather than a replacement. It runs only when rkaiq's own
table is empty, so supported sensors are unaffected.

## The imx519 kernel patch

The imx519 driver in `rockchip-linux-kernel-priv` was exactly the case above.
`RKMODULE_GET_MODULE_INFO` worked (returning
`sensor='imx519' module='arducam' lens='default'`, matching the shipped
`imx519_arducam_default.json`), but there was no `.enum_frame_interval` pad op,
so `enumStaticMetas()` reported `num=0`.

The patch to `drivers/media/i2c/imx519.c` adds:

* `imx519_enum_frame_interval()` and its `.enum_frame_interval` pad op, taking
  frame rates from each mode's existing `timeperframe_min` and reporting
  `NO_HDR` in `reserved[0]` (the Rockchip convention for the HDR mode);
* `RKMODULE_GET_HDR_CFG` / `RKMODULE_SET_HDR_CFG` plus their `compat_ioctl32`
  handling, answering the query rkaiq makes unconditionally.

After building and booting it, rkaiq enumerates the sensor properly:

```
[0] sensor='m00_f_imx519 2-001a' num=5
    4656x3496 fps=9   3840x2160 fps=18   2328x1748 fps=30
    1920x1080 fps=60  1280x720  fps=80
```

It also transformed the picture: with no mode table rkaiq drove the ISP with
garbage parameters and the preview was black (frame luma 0.4); with modes it is
correctly exposed. `CAMHW:E:failed to set hdr mode 0` is gone too.

Deliberately **not** added, having checked the rkaiq source rather than guessed:

* `RKMODULE_SET_QUICK_STREAM`, which is defined in `rk-camera-module.h` but
  never called anywhere in rkaiq, so it would be dead code.
* `RKMODULE_GET_CHANNEL_INFO`, which is used only for PDAF detection. Its
  failure is tolerated (`if (io_control(...) == 0)`), giving
  `pdaf_support = false`.

## Upstream VCM drivers are invisible to rkaiq

Same class of problem as the missing `enum_frame_interval` above: a driver that works
perfectly well by itself, but does not follow a Rockchip convention rkaiq depends on.

rkaiq finds a focus motor by **media entity name**, requiring it to begin with the sensor's
module index (`CamHwIsp20.cpp:1049`). Rockchip's own VCM drivers build that name from
`rockchip,camera-module-index` and `-facing` in the device tree. The upstream `ak7375` and
`dw9807-vcm` drivers do not, and the camera overlays do not set those properties, so the
subdev is called `ak7375 2-000c` rather than `m00_f_ak7375 2-000c` and rkaiq concludes there
is no lens.

The app does not depend on that. It resolves the motor from the device tree's `lens-focus`
phandle and drives `V4L2_CID_FOCUS_ABSOLUTE` itself, so focus works regardless. See
[04-focus-and-zoom.md](04-focus-and-zoom.md) for the detail and for what a kernel fix would
involve.

## Known platform issue: 3A never runs

Fixing mode enumeration did not fix 3A. See
[06-statistics.md](06-statistics.md) for the full evidence: no ISP statistics
reach rkaiq, on either sensor tested (ov5647 and imx519), and Rockchip's own
daemon fails identically.

The consequence for users: **auto exposure and auto white balance do not
converge.** Manual control of exposure, gain, white balance and the image
settings does work, through the mechanism in
[07-iq-file-override.md](07-iq-file-override.md). Noise reduction remains
inert.

## Harmless messages

| Message | Meaning |
| --- | --- |
| `XCORE:E:invalid main scene len!` | `preInit_scene` runs before the IQ file is parsed. `rkisp_demo` logs it too. |
| `ACCM:E:check yalp-gains[12])` | Appears in manual AE mode; the colour-matrix curve falls back. Colours still respond correctly. |
| `QSocketNotifier: Can only be used with threads started with QThread` | Qt noticing librkaiq's own threads. Harmless. |
| Focus group enabled while rkaiq reports `has_lens_vcm = 0` | Expected. The app found the motor through the device tree; rkaiq's own detection is name-based and fails on upstream VCM drivers. |

## Useful debug knobs

```sh
# rkaiq engine logging (0x7ff4 broad, 0x1ff3 is the AE-focused level)
sudo persist_camera_engine_log=0x7ff4 ./camera-ctls

# ISP driver tracing
echo 1 | sudo tee /sys/module/video_rkisp/parameters/debug
echo 1 | sudo tee /sys/module/video_rkisp/parameters/irq_dbg
# ... remember to set both back to 0

# What the media graph actually looks like
media-ctl -d /dev/media0 -p
media-ctl -d /dev/media1 -p

# Sensor registers directly, bypassing rkaiq entirely
v4l2-ctl -d /dev/v4l-subdev2 --get-ctrl exposure,analogue_gain
```
