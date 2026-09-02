# Applying controls through the IQ file

Implemented in `src/IqOverride.{h,cpp}`, driven by
`CameraPipeline::applyManualValues()`.

## Why

The calibration rkaiq parses at `sysctl_init()` is the one path that does **not**
depend on statistics. So when [06-statistics.md](06-statistics.md) applies, the
app writes the value into a private copy of the IQ file and brings the engine
back up against it.

Everything still goes through rkaiq. Only the route the value takes changes.
There is no V4L2 bypass, and the ISP pipeline is untouched.

## What gets written

| Control | IQ path |
| --- | --- |
| AE mode | `ae_calib.CommCtrl.AecOpType` |
| Exposure, gain, ISP digital gain | `ae_calib.CommCtrl.AecManualCtrl.LinearAE` |
| AWB mode | `wb_v21.control.mode` |
| WB gains | `wb_v21.manualPara.cfg.mwbGain` as `[R, Gr, Gb, B]` |
| Colour temperature | `wb_v21.manualPara.cfg.cct.CCT` |
| Brightness, contrast, saturation, hue | `cproc.param` |
| Sharpness | every `sharp_v4` `sharp_ratio`, scaled by `slider / 50` |

The enum spellings here are the *calibration* ones, a different set from the
runtime uAPI enums: `RK_AIQ_OP_MODE_MANUAL`, `CALIB_WB_MODE_MANUAL`,
`CALIB_MWB_MODE_WBGAIN`, `CALIB_MWB_MODE_CCT`.

Every `main_scene` / `sub_scene` in the file is patched, because which scene
rkaiq selects depends on `preInit_scene` and guessing would be fragile.

## Three traps, all found the hard way

**1. The IQ parser is order-sensitive.** Parsing the file and writing it back
through a JSON library that sorts keys, as Qt's `QJsonObject` does, reorders
every object in the file. rkaiq then rejects sections it previously accepted:

```
orig:    "lumaCCM": { "rgb2y_para", "low_bound_pos_bit", "gain_yalp_curve", … }
sorted:  "lumaCCM": { "gain_alphaScale_curve", "gain_yalp_curve", … }
```

So `IqOverride` edits the file **textually**, replacing only the values that
change and leaving every other byte, and the key order, exactly as shipped. A
patched file differs from the original by about four lines. The helpers that do
this (`endOfValue`, `findMember`, `locate`, `replaceAt`) are a small
order-preserving JSON value editor, not a parser.

**2. Parsed calibration is cached against the file path.** Writing new values to
the same path leaves the engine using the first version it ever read, so only
the first change takes effect and every later one silently does nothing. Each
write therefore goes to a **fresh numbered directory**
(`/tmp/camera-ctls-iq-<pid>/<n>/`), and the previous one is removed once the
engine has been handed the new path.

**3. rkaiq derives the filename itself.** `sysctl_init()` takes a directory and
re-derives `<sensor>_<module>_<lens>.json` from `RKMODULE_GET_MODULE_INFO`, so
the copy must keep the original name.
`CameraEnumerator::iqFileNameFor()` reproduces it from the same ioctl.

## Cost, and how the UI hides it

Each change costs a full engine stop/start, roughly a second. Edits are
coalesced over 700 ms on top of the slider's own debounce, and the sensor panel
says what is happening.

A fourth trap is on the application side rather than rkaiq's: the restart runs
the Qt event loop, so a coalesce timer firing inside it re-enters the apply and
turns one slider nudge into a restart loop. `MainWindow::applyManualNow()`
guards against re-entry, stops the timer on entry, and the mode toggles schedule
through the same timer as the sliders instead of applying immediately.

An override is only written once something actually differs from the shipped
calibration (`IqOverride::Values::isNeutral()`), so a freshly started app with
every control at its default does not restart at all.

## Measured results

Three successive applies in one process, through the real `CameraPipeline`:

```
plain start     -> sensor exposure  139, gain 960   (the AE fallback)
exposure 0.002s -> sensor exposure   67, gain   1
exposure 0.010s -> sensor exposure  337, gain   1
exposure 0.040s -> sensor exposure 1349, gain   1

wbGain 1.3/1.0/3.3 -> frame mean R  15.5  B  81.2
wbGain 4.0/1.0/1.2 -> frame mean R  98.0  B  16.1
wbGain 1.0/1.0/5.0 -> frame mean R   5.2  B 151.0

neutral             -> Ymean  50.0  Ystd 37.3  chroma  74.6  hf 4.44
brightness  40      -> Ymean  16.9
brightness 220      -> Ymean 138.3
contrast    40      ->               Ystd 19.0
contrast   220      ->               Ystd 52.9
saturation   0      ->                          chroma   0.0
saturation 255      ->                          chroma 115.3
sharpness    0      ->                                        hf 2.27
sharpness  100      ->                                        hf 6.68
hue 128 / 0 / 200   -> mean R 82.3/42.8/74.9,  mean B 41.7/30.4/76.4
```

## Safety

`/etc/iqfiles` is never modified. The patched copy lives under the system temp
path in a per-process directory and is removed when the app exits.

## One cosmetic wart

Manual AE makes rkaiq log `ACCM:E:check yalp-gains[12])`, which the stock file
does not. It comes from the mode itself rather than from the editing: a
byte-preserving patch produces it too, and the shipped `gain_yalp_curve` has
thirteen entries all at `iso: 50`, which looks like what the check trips on.
Exposure, gain, white balance and the image controls all respond correctly
regardless, so the colour-matrix curve is falling back rather than breaking
anything.
