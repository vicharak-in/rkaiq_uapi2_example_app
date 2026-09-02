# 3A statistics

This is the single most important document here. Almost every "the control does
nothing" symptom on this platform traces back to it.

## The API

```c
XCamReturn rk_aiq_uapi2_sysctl_get3AStatsBlk(const rk_aiq_sys_ctx_t *ctx,
                                             rk_aiq_isp_stats_t **stats,
                                             int timeout_ms);
XCamReturn rk_aiq_uapi2_sysctl_release3AStatsRef(const rk_aiq_sys_ctx_t *ctx,
                                                 rk_aiq_isp_stats_t *stats);
```

Blocks until a statistics buffer arrives or the timeout expires. Every
successful call must be paired with a release. `rk_aiq_isp_stats_t` carries
`frame_id`, `aec_stats`, and version-tagged unions for AWB and AF results.

`AiqController::statisticsAvailable()` uses it as a probe: three attempts at a
500 ms timeout, once per camera after `start()`. The answer decides whether the
app uses the live setters or the IQ-file override.

## Why it matters

rkaiq does not write control values to hardware when you set them. It stores
them, and the relevant algorithm applies them on its next per-frame pass. That
pass is driven by statistics arriving from the ISP.

No statistics means no pass, which means:

* auto exposure and auto white balance never converge;
* **manual** exposure and white balance are equally dead, because they run
  through the same algorithm pass;
* the CPROC and sharpening controls are dead for the same reason.

Every one of those setters still returns `XCAM_RETURN_NO_ERROR`, and the
matching getter still reads the value back. Success tells you the value was
stored, nothing more.

## What was measured on this board

`get3AStatsBlk` called five times with a 1 s timeout while the camera is
streaming:

```
[0] ret=-20 stats=(nil)   (no statistics delivered)
[1] ret=-20 stats=(nil)   ... all five identical
```

With `persist_camera_engine_log=0x7ff4` the AE algorithm says the same thing
directly, and emits exactly **one** exposure decision for an entire run:

```
AEC:W:the xcamvideobuffer of aec stats is null
AEC:D:ae_stat == NULL
AEC:D:exp_set0,gain=0x3c0,time=0x8b,ispG=1.000000,dcg=-1
```

`time=0x8b` is 139 and `gain=0x3c0` is 960, exactly where the sensor stays
pinned. The Rockchip guide notes that a healthy AE log prints a
`Cur-Exp: FrmId=…` line every frame; none ever appear.

## What this is not

Ruled out by direct test, so you need not re-check:

* **Not this application.** Rockchip's own `rkaiq_3A_server` fails identically
  on the same board, with the same single `exp_set0` and the same pinned sensor.
* **Not the sensor mode table.** Fixed in the kernel (the imx519 now enumerates
  all five modes with correct frame rates); 3A still does not run.
* **Not the IQ file.** Substituting the imx708, imx219 and imx415 tuning files
  for the imx519's gives byte-identical `-20`/NULL results.
* **Not the media topology.** `rkisp-isp-subdev:3 -> rkisp-statistics` is
  `[ENABLED]` and the stats node starts cleanly
  (`rkisp_stats_vb2_start_streaming cnt:4`).
* **Not a missing interrupt.** With `irq_dbg=1` on the `video_rkisp` module the
  ISP raises `ISP3X_FRAME` (`isp_irq_hdl:0x2`) every frame, so
  `rkisp_stats_isr_v3x()` runs each frame. (`mipi_irq` sitting at zero in
  `/proc/interrupts` looks alarming but only counts MIPI errors.)

## Where the trail ends

In `isp_stats_v3x.c:rkisp_stats_send_meas_v3x()` the AE block is only filled
when the ISP's separate 3A status register signals it:

```c
if (meas_work->isp3a_ris & ISP3X_3A_RAWAE_BIG)
        ops->get_rawae3_meas(stats_vdev, cur_stat_buf, 0);
```

The frame interrupt arrives, but `ISP3X_3A_RAWAE_BIG` is evidently never set,
so the RAWAE measurement block is not signalling that it is ready. Confirming
that, and finding which parameter write should have enabled it, needs a kernel
print of `ISP3X_ISP_3A_RIS` per frame. It cannot be seen from userspace.

## Reproducing the probe

```sh
sudo systemctl stop rkaiq_3A.service        # nothing else may own the engine
sudo persist_camera_engine_log=0x7ff4 ./camera-ctls
```

Log level bits worth knowing: `0x1ff3` is the level the Rockchip guide
recommends for AE debugging; bit 5 covers the AE statistics-processing module
and bit 6 the exposure calculation.
