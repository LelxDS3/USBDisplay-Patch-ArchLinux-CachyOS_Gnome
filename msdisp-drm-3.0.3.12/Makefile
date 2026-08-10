# Unified out-of-tree Makefile for the MacroSilicon MS912x USB display driver.
# Builds two modules: usbdisp_drm (DRM/KMS) + usbdisp_usb (USB transport).
#
# Gating on KBUILD_EXTMOD (set only inside a real kbuild "M=" build) rather than
# KERNELRELEASE lets DKMS work: DKMS invokes plain `make` with KERNELRELEASE set,
# which must take the wrapper branch and re-enter kbuild itself.

ifeq ($(KBUILD_EXTMOD),)

# ---------- wrapper (plain make / dkms) ----------
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean

else

# ---------- kbuild context (M= build) ----------
EL8 := $(shell cat /etc/redhat-release 2>/dev/null | grep -c " 8." )
ifneq (,$(findstring 1,$(EL8)))
EL8FLAG := -DEL8
endif
Raspbian := $(shell grep -Eic 'raspb(erry|ian)' /proc/cpuinfo /etc/os-release 2>/dev/null)
ifeq (,$(findstring 0,$(Raspbian)))
RPIFLAG := -DRPI
endif

obj-m += usbdisp_drm.o
obj-m += usbdisp_usb.o

usbdisp_drm-y := \
	drm/msdisp_plat_drv.o drm/msdisp_plat_dev.o drm/msdisp_drm_drv.o \
	drm/msdisp_drm_modeset.o \
	drm/msdisp_drm_encoder.o drm/msdisp_drm_connector.o drm/msdisp_drm_interface.o \
	drm/msdisp_drm_sysfs.o drm/msdisp_common_util.o drm/msdisp_drm_mode.o

usbdisp_usb-y := \
	drm/msdisp_usb_drv.o drm/ms9132_hal.o \
	usb_hal/hal_adaptor.o usb_hal/usb_device.o usb_hal/usb_hal_interface.o \
	usb_hal/usb_hal_sysfs.o usb_hal/usb_hal_thread.o

ccflags-y := -I$(src)/drm -I$(src)/usb_hal $(EL8FLAG) $(RPIFLAG)

endif
