# Engine lifecycle and discovery

Implemented in `src/AiqController.cpp` and sequenced by
`src/CameraPipeline.cpp`.

## Discovery

### `rk_aiq_uapi2_sysctl_enumStaticMetas`

```c
XCamReturn rk_aiq_uapi2_sysctl_enumStaticMetas(int index,
                                               rk_aiq_static_info_t *static_info);
```

Enumerates the sensors rkaiq knows about. Call with `index` 0, 1, 2 … until it
returns an error. Needs no context, so it works before `sysctl_init()`.

The parts of `rk_aiq_static_info_t` this app reads:

| Field | Meaning |
| --- | --- |
| `sensor_info.sensor_name` | Media entity name, e.g. `m00_f_imx519 2-001a` |
| `sensor_info.num` | How many capture modes follow |
| `sensor_info.support_fmt[]` | `width`, `height`, `fps`, `hdr_mode` per mode |
| `sensor_info.phyId` | Physical camera index, parsed from the `mNN_` prefix |
| `isp_hw_ver` | ISP hardware version |
| `has_lens_vcm`, `has_fl`, `has_irc` | Whether a focus motor, flash or IR-cut is present |

`sensor_info.num` comes back as 0 for sensors whose driver lacks
`.enum_frame_interval`; see [08-platform-notes.md](08-platform-notes.md). The
app falls back to reading modes off the sensor subdev in that case.

### `rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd`

```c
const char *rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(const char *vd);
```

Maps a `/dev/videoN` capture node to the sensor entity bound to it. This is the
authoritative pairing, but it returns `NULL` when the sensor and the ISP live on
different `/dev/mediaN`, which is the normal arrangement on RK3588. Every result
is checked and unmatched cameras fall back to positional pairing.

### `rk_aiq_uapi2_get_version_info`

```c
void rk_aiq_uapi2_get_version_info(rk_aiq_ver_info_t *vers);
```

Fills in the AIQ and ISP version strings shown in the sensor panel.

## Bring-up

The order below is not arbitrary. It mirrors `rkisp_demo`'s `rkisp_routine()`,
and the V4L2 format is deliberately set **after** the engine has started.

### 1. `rk_aiq_uapi2_sysctl_preInit_scene`

```c
XCamReturn rk_aiq_uapi2_sysctl_preInit_scene(const char *sns_ent_name,
                                             const char *main_scene,
                                             const char *sub_scene);
```

Selects the scene inside the IQ file, `"normal"` / `"day"` here. It runs before
the IQ file is parsed, so it logs `XCORE:E:invalid main scene len!` on this
platform. That message is harmless; `rkisp_demo` produces it too.

### 2. `rk_aiq_uapi2_sysctl_init`

```c
rk_aiq_sys_ctx_t *rk_aiq_uapi2_sysctl_init(const char *sns_ent_name,
                                           const char *iq_file_dir,
                                           rk_aiq_error_cb err_cb,
                                           rk_aiq_metas_cb metas_cb);
```

Creates the engine context. Note it takes a **directory**, not a file: rkaiq
derives the filename itself as `<sensor>_<module>_<lens>.json` from
`RKMODULE_GET_MODULE_INFO`. Returns `NULL` on failure.

Two things to know before calling it:

* **Only one process may own the engine.** A second `sysctl_init()` on the same
  sensor does not fail, it blocks forever. The app checks for an existing owner
  first, as described in [08-platform-notes.md](08-platform-notes.md).
* **It can segfault on a stale lock file.** rkaiq opens `/tmp/aiq<N>.lock` with
  `fopen()` and passes the result to `fileno()` without a NULL check.

### 3. `rk_aiq_uapi2_sysctl_prepare`

```c
XCamReturn rk_aiq_uapi2_sysctl_prepare(const rk_aiq_sys_ctx_t *ctx,
                                       uint32_t width, uint32_t height,
                                       rk_aiq_working_mode_t mode);
```

Configures geometry and HDR mode. The width and height must match what you will
later give `VIDIOC_S_FMT`. `mode` is one of:

| Value | Meaning |
| --- | --- |
| `RK_AIQ_WORKING_MODE_NORMAL` | Linear, non-HDR |
| `RK_AIQ_WORKING_MODE_ISP_HDR2` | Two-frame HDR |
| `RK_AIQ_WORKING_MODE_ISP_HDR3` | Three-frame HDR |

Only offer the HDR modes when `support_fmt[].hdr_mode` actually reports them.

### 4. `rk_aiq_uapi2_sysctl_start`

```c
XCamReturn rk_aiq_uapi2_sysctl_start(const rk_aiq_sys_ctx_t *ctx);
```

Starts the 3A loop. Only after this does the app open the capture node, set the
format and call `VIDIOC_STREAMON`.

## Teardown

```c
XCamReturn rk_aiq_uapi2_sysctl_stop(const rk_aiq_sys_ctx_t *ctx, bool keep_ext_hw_st);
XCamReturn rk_aiq_uapi2_sysctl_deinit(rk_aiq_sys_ctx_t *ctx);
```

Teardown is the exact reverse of bring-up: stop capture, then `sysctl_stop()`,
then `sysctl_deinit()`. `keep_ext_hw_st` is passed `false` because the app
wants the hardware fully released.

**This must always run.** An ISP left streaming refuses the next launch, and a
`SIGKILL`ed instance will be reported as the conflicting engine owner on the
next start. Exit through the window close button or Ctrl-C, both of which tear
down cleanly.
