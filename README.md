# camera-ctls-rk3588

A Qt5 control panel for RK3588 ISP cameras, driving exposure, white balance,
focus and image processing through the **rkaiq uAPI2** API.

The app is **sensor-agnostic**. Cameras, resolutions, frame rates, control
ranges and hardware capabilities are all discovered at runtime, so the same
binary works with any sensor the rkaiq stack supports (imx219, imx415, imx519,
imx708, os04a10, ov5647, …). Nothing about a specific module is hard-coded.

Full API reference and platform notes live in **[docs/](docs/README.md)**.

## Requirements

| Need | Notes |
| --- | --- |
| RK3588 board | ISP v3.0. For another Rockchip SoC see `ISP_HW_VERSION` below. |
| `camera-engine-rkaiq` | Provides `librkaiq.so` in `/usr/lib` and headers in `/usr/include/rkaiq`. |
| IQ tuning file | `/etc/iqfiles/<sensor>_<module>_<lens>.json` for the attached sensor. |
| Qt 5 | `qtbase5-dev`. |

## Build

```sh
sudo apt install qtbase5-dev cmake g++
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The binary lands at `build/camera-ctls`. To install it system-wide:

```sh
sudo cmake --install build          # -> /usr/local/bin/camera-ctls
```

### Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `ISP_HW_VERSION` | `ISP_HW_V30` | Must match the flags `librkaiq.so` was built with, or struct layouts disagree between your translation units and the library. `ISP_HW_V30` is correct for RK3588. |
| `RKAIQ_ROOT` | `/usr/include/rkaiq` | Header root, if rkaiq is installed somewhere else. |

```sh
cmake -B build -DISP_HW_VERSION=ISP_HW_V21      # e.g. RK3568
```

### Troubleshooting the build

* **`fatal error: rk_aiq.h: No such file or directory`**. The rkaiq headers
  cross-include each other by short name, so several subdirectories have to be
  on the include path. `CMakeLists.txt` already lists them. If you are
  compiling a test program by hand, add `-I/usr/include` plus these
  subdirectories of `$RKAIQ_ROOT`: `uAPI2`, `common`, `xcore`, `algos`,
  `iq_parser`, `iq_parser_v2` and `isp`.
* **Undefined references to `rk_aiq_uapi2_*`**. `librkaiq.so` is missing or too
  old. Check with `nm -D /usr/lib/librkaiq.so | grep sysctl_init`.
* **`uAPI2/rk_aiq_user_api2_wrapper.h` will not compile.** It includes a header
  that is not installed. Do not include it.

## Run

```sh
sudo systemctl stop rkaiq_3A.service   # only one process may own the ISP
sudo ./build/camera-ctls
```

Run it from a graphical session. From a bare TTY, pass `-platform eglfs` or
`-platform linuxfb`. Instead of `sudo`, you can add your user to the `video`
group and re-login. Whichever you choose, stay with it: mixing `sudo` and
non-`sudo` runs leaves behind a lock file that crashes rkaiq, as described in
[docs/08-platform-notes.md](docs/08-platform-notes.md).

**Do not start `rkaiq_3A_server` alongside this app.** The app is itself the 3A
client. Only one process may own the engine, and starting a second one does
not fail. It hangs.

### Command-line options

Both are overrides; discovery is automatic.

| Option | Meaning |
| --- | --- |
| `--iqdir`, `-i` `<dir>` | Directory of IQ tuning files. Default `/etc/iqfiles`. This is a *directory*: rkaiq derives the filename from the attached module. |
| `--device`, `-d` `<path>` | Prefer the camera on this capture node. Without it, the first discovered camera is used. |

`--help` lists them; `--version` prints the application version (the AIQ
library version is shown in the sensor panel at runtime).

## Using the controls

Each group has an **Auto** checkbox. Unchecking it switches that algorithm to
manual and seeds the sliders from whatever auto had converged to, so the picture
does not jump.

| Group | Manual controls | Reference |
| --- | --- | --- |
| Stream | resolution, frame rate, HDR mode | [01](docs/01-engine-lifecycle.md) |
| Exposure | exposure time, analogue gain, digital gain, flicker, AE lock | [02](docs/02-exposure.md) |
| White balance | colour temperature **or** R/G/B gain | [03](docs/03-white-balance.md) |
| Focus | focus position, focus-once, continuous autofocus, zoom | [04](docs/04-focus-and-zoom.md) |
| Image | contrast, brightness, saturation, hue, sharpness, mirror/flip | [05](docs/05-image-and-noise.md) |
| Denoise | overall strength, or spatial (2D) + temporal (3D) | [05](docs/05-image-and-noise.md) |

**Frame rate applies live.** Resolution and HDR mode cannot change while the ISP
is streaming, so they sit behind the **Apply** button, which tears down and
rebuilds the pipeline. When nothing is streaming, that button becomes
**Retry**, which is handy after stopping whatever else was holding the engine.

**Capability gating.** Groups the attached module cannot support are greyed out
with a tooltip explaining why, rather than being hidden or failing silently. A
fixed-focus module greys the Focus group ("No focus motor on this sensor"); the
same code enables it, with the real focus range, on a module that has a VCM.

## If the controls appear to do nothing

This is the expected symptom on a board where the ISP delivers no 3A
statistics, which is the case here. rkaiq stores control values and applies
them on an algorithm pass that statistics drive, so with none arriving every
setter succeeds and nothing changes.

The app detects this at startup and says so in the sensor panel. It then applies
manual values by patching the IQ file and restarting the engine. That does
work, at a cost of roughly a second per change. Exposure, gain, white balance,
brightness, contrast, saturation, hue and sharpness are all handled this way.

Read [docs/06-statistics.md](docs/06-statistics.md) for the diagnosis and
[docs/07-iq-file-override.md](docs/07-iq-file-override.md) for the mechanism.

Focus is handled differently again. rkaiq cannot see the motor on modules whose VCM driver
does not follow its naming convention, so the app finds the motor through the device tree
and drives it over V4L2, and runs its own contrast autofocus from the preview frames. That
works today with no kernel change, and it is instant rather than costing a restart. See
[docs/04-focus-and-zoom.md](docs/04-focus-and-zoom.md).

Still inert: **auto** exposure/white balance (they need the statistics
themselves) and the denoise group (not yet routed through the override).

## Project layout

```
src/
  main.cpp            Command line, application setup
  MainWindow.*        UI, control wiring, coalescing of manual applies
  CameraPipeline.*    Owns the engine and the capture thread; bring-up order
  AiqController.*     Thin wrapper over rkaiq uAPI2
  CameraEnumerator.*  Camera/node/subdev discovery, engine-owner detection
  IqOverride.*        Order-preserving IQ file patching
  LensController.*    Focus motor over V4L2
  ContrastAutoFocus.* Sweep-and-score autofocus
  V4l2Capture.*       Capture thread, NV12 -> QImage
  PreviewWidget.*     Preview rendering
  LabeledSlider.*     Slider + spin box with change coalescing
docs/                 rkaiq API reference and platform notes
```

## Notes for anyone extending this

* **Bring-up order is load-bearing and not the obvious one.** The V4L2 format is
  set *after* the rkaiq engine has started, matching `rkisp_demo`'s
  `rkisp_routine()`. See `CameraPipeline::start()`. Teardown must be the exact
  reverse, or the ISP stays claimed and the next launch fails.

* **Three different auto/manual enums exist and disagree numerically**
  (`opMode_t` AUTO=0/MANUAL=1, `RKAiqOPMode_t` AUTO=1/MANUAL=2, and
  `rk_aiq_wb_op_mode_t` inverted at MANUAL=0/AUTO=1). Mixing them compiles
  cleanly and silently selects the wrong mode. `AiqController` exposes a single
  `Mode` enum and converts at each call site; keep it that way.

* **Every v2 attribute API is get-modify-set**, and `sync.sync_mode` /
  `sync.done` must be filled in explicitly or they hold garbage.

* **A success return does not mean the value reached the picture.** On this
  platform most setters return 0 and change nothing. Verify against the sensor
  (`v4l2-ctl -d /dev/v4l-subdevN --get-ctrl exposure`) or against pixels, never
  against the return code alone.

* **The imgproc denoise/sharpness calls are used deliberately** in preference to
  the per-module APIs (`aynrV3`, `acnrV30`, `abayertnrV30`, …), which are
  versioned per ISP generation and would pin the app to one SoC.

* **Never hard-code a sensor name, `/dev/videoN` number or resolution.**
  Everything is discovered. A quick check that nothing has crept in:

  ```sh
  for f in src/*.cpp src/*.h; do
    gcc -fpreprocessed -dD -E -P "$f" | grep -nE "imx[0-9]|ov[0-9]|video1[0-9]"
  done
  ```

## Licence

Not yet specified.
