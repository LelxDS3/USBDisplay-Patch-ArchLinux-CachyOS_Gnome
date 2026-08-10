# msdisp-drm — Linux DRM/KMS driver for MacroSilicon MS912x USB displays

A from-source **DRM/KMS** driver for MacroSilicon **MS912x** USB display controllers,
adapted for the integrated 7-inch 1024×600 USB-C "PC sub display" (LCDwiki and clones),
USB ID **`345f:9132`**.

Because it registers a real DRM/KMS device (`/dev/dri/cardN`), one driver covers everything:

- **X11** — via the generic `modesetting` driver
- **Wayland** — via the compositor's DRM backend
- **Text console (fbcon)** — the panel shows the Linux TTY with **no X11/Wayland at all**, like an ordinary monitor
- **Hotplug-safe** — unplug/replug auto-rebinds and repaints with no user action

|                | Status |
|----------------|--------|
| Kernels        | **6.0 – 6.18+** (version-guarded; built & verified on 6.18). Forward-compatible to 7.x — see [Kernel compatibility](#kernel-compatibility). |
| Architectures  | **x86_64** and **arm64** (arch-neutral C; DKMS rebuilds per kernel/arch) |
| Modules        | `usbdisp_drm` (DRM/KMS) + `usbdisp_usb` (USB transport, binds the device) |
| Native mode    | 1024×600 @ 60 Hz, RGB888, ~25–30 FPS (USB 2.0 bulk) |

---

## Hardware

Targets MS912x controllers exposing the vendor display interface. The id table binds
`345f:9132`, `345f:9133`, `345f:9135`. The shipped default is tuned for the **1024×600**
LCDwiki panel, whose controller has **no EDID** — its native timing is the vendor mode
**VIC 150**, registered via the `custom_mode` parameter (default in `modprobe.d`).

## Install (DKMS — recommended)

DKMS auto-rebuilds the modules on every kernel upgrade.

```sh
# from the repository root (contains dkms.conf):
ver=3.0.3.12
sudo cp -r .  /usr/src/msdisp-drm-$ver
sudo cp modprobe.d-msdisp-drm.conf /etc/modprobe.d/msdisp-drm.conf   # default 1024x600 mode
sudo cp 99-msdisp-fbcon.rules       /etc/udev/rules.d/                # console-on-panel (optional)
sudo dkms add    -m msdisp-drm -v $ver
sudo dkms build  -m msdisp-drm -v $ver
sudo dkms install -m msdisp-drm -v $ver
sudo depmod -a
```

Plug in the display — udev auto-loads the driver and creates `/dev/dri/cardN`. The same
flow works identically on an x86_64 host.

**Dependencies:** kernel headers for your running kernel (`linux-headers-$(uname -r)` /
`kernel-devel`), `dkms`, a C toolchain. The optional console-on-panel rule uses
`con2fbmap` (Debian/Ubuntu package **`fbset`**).

### Manual build (development)

```sh
make                                  # builds usbdisp_drm.ko + usbdisp_usb.ko
sudo insmod ./usbdisp_drm.ko          # load the DRM module first
sudo insmod ./usbdisp_usb.ko custom_mode="150_1024x600@60"
```

`make clean` to clean. (Load order matters: `usbdisp_usb` depends on `usbdisp_drm`.)

## Configuration

- **Panel mode.** The panel has no EDID, so its native timing is supplied as a vendor VIC:
  `options usbdisp_usb custom_mode=150_1024x600@60` (shipped in `modprobe.d-msdisp-drm.conf`).
  Format: `custom_mode="<vic>_<w>x<h>@<hz>[,<vic>_<w>x<h>@<hz>...]`.
- **Console on the panel.** `99-msdisp-fbcon.rules` maps the text consoles (VT 1–6) to the
  panel's framebuffer automatically when it appears. To do it by hand:
  `for v in 1 2 3 4 5 6; do sudo con2fbmap $v <N>; done` where `<N>` is the panel's `fbNN`
  (`cat /sys/class/graphics/fb*/name` → the one named `msdispdrmfb`).

## Usage

### Text console (no compositor)
Once loaded with the udev rule installed, the panel shows the Linux console — boot messages,
login prompt, anything you `echo` to the mapped VT. Switch VTs with `chvt`.

### X11
Driven by `xf86-video-modesetting` (no GPU on the controller → GL is software/llvmpipe).
- **Sole display:** Xorg uses the card directly.
- **Extended/second screen** beside a real GPU (PRIME output offload):
  ```sh
  xrandr --listproviders
  xrandr --setprovideroutputsource <usb-provider> <gpu-provider>
  xrandr --output <usb-connector> --auto --right-of <main-output>
  ```

### Wayland
Works via the compositor's DRM backend with **software rendering**:
- Weston: `weston --renderer=pixman`
- wlroots/sway: `WLR_RENDERER=pixman sway`
- Mutter / KDE: multi-DRM-device, used as an additional output

### Replug
Unplug/replug re-binds the transport, fires a hotplug event, and the in-kernel fbdev client
(or your compositor) repaints — the image returns on its own.

## Kernel compatibility

Built and verified on **6.18 (aarch64)**. Every kernel-API difference is wrapped in an
**open-ended `LINUX_VERSION_CODE` guard** (e.g. `>= 6.11`, `>= 6.13`, `>= 6.16`), and the only
upper-bounded guards are for features the kernel *removed* (`drm_driver.date` in 6.14, the
`int`-returning `platform_driver.remove` in 6.11). Those comparisons all resolve correctly for
**7.x** (a 7.0 version bump is not an API rewrite), and the driver builds on the current stable
DRM helpers (`drm_gem_shmem`, `drm_client_setup`, `drm_gem_fb_create_with_dirty`,
`DRM_FBDEV_SHMEM_DRIVER_OPS`). Because DKMS recompiles against each installed kernel, it adapts
automatically. 7.x is unreleased and therefore untested; if a future kernel changes one of these
APIs, the fix is a localized version guard.

## Limitations
- **No hardware acceleration** — software rendering (llvmpipe / pixman).
- **USB 2.0 bulk** — full-frame ≈ 25–30 FPS at 1024×600.
- Fixed mode (no EDID); set via `custom_mode` (shipped default = VIC 150 / 1024×600).

## How it works
The chip selects output timing by a **VIC index**; 1024×600 is vendor **VIC 150** (verified by
USB capture of the Windows tool). Frames are sent as full-frame **RGB888** over the bulk endpoint.
`usbdisp_drm` is a platform DRM/KMS device using `drm_gem_shmem` buffers; `usbdisp_usb` binds the
USB device and feeds frames. The connector is forced *connected* with the fixed 1024×600 mode.

## Credits & License

This is a community **port and adaptation** of MacroSilicon's GPL-2.0 vendor DRM driver
(`DRM_SourceCode_V3.0.3.12`, © MacroSilicon Technology Co., Ltd.). Changes here: kernel-6.0+/7.x
porting, EDID-less fixed-mode (VIC 150) support, RGB color + frame-delivery fixes, migration to
`drm_gem_shmem` + DRM fbdev emulation (console), and DKMS packaging. The MS912x USB protocol
understanding was aided by the reverse-engineered community driver
[`rhgndf/ms912x`](https://github.com/rhgndf/ms912x).

Licensed under **GPL-2.0** (inherited from the vendor source). See [LICENSE](LICENSE). Original
per-file copyright notices are preserved.
