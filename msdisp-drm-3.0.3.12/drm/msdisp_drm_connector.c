/* Copyright (C) 2023 MacroSilicon Technology Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * msdisp_drm_connector.c -- Drm driver for MacroSilicon chip 913x and 912x
 */


#include <linux/version.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_modes.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>

#if KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_probe_helper.h>
#endif

#include "msdisp_drm_drv.h"
#include "msdisp_drm_connector.h"
#include "msdisp_usb_interface.h"
#include "msdisp_drm_mode.h"

/* Resolucion preferida del monitor USB (la que configura el MENU).
 * El driver genera con ella el modo PREFERRED que GNOME adopta al arrancar,
 * sin modeset en caliente -> evita el use-after-free de usb_hal_update_frame.
 * Default 800x600: seguro para cualquier monitor VGA. */
static uint msdisp_drm_pref_w = 800;
static uint msdisp_drm_pref_h = 600;
module_param_named(pref_w, msdisp_drm_pref_w, uint, 0644);
MODULE_PARM_DESC(pref_w, "Preferred width for the USB monitor (default 800)");
module_param_named(pref_h, msdisp_drm_pref_h, uint, 0644);
MODULE_PARM_DESC(pref_h, "Preferred height for the USB monitor (default 600)");



static struct msdisp_drm_pipeline* get_pipeline_by_connector(struct drm_connector* connector) 
{
	struct msdisp_drm_device *msdisp_drm = to_msdisp_drm(connector->dev);
	struct msdisp_drm_connector *msdisp_connector =
					container_of(connector,
					struct msdisp_drm_connector,
					connector);
	struct msdisp_drm_pipeline* pipeline;

	pipeline = &msdisp_drm->pipeline[msdisp_connector->pipeline_index];

	return pipeline;
}


static int msdisp_drm_add_modes_by_cea_vic(struct drm_connector* connector)
{
	struct msdisp_drm_pipeline* pipeline;
	struct msdisp_usb_hal* usb_hal;
	int cnt = 0, get_cnt = 0, ret, i;
	unsigned char vics[8];
	struct drm_display_mode* mode = NULL;

	pipeline = get_pipeline_by_connector(connector);
	mutex_lock(&pipeline->hal_lock);
	usb_hal = pipeline->usb_hal;
	if (!usb_hal) {
		goto out;
	}

	ret = usb_hal->funcs->get_custom_cea_vic(usb_hal, vics, 8, &get_cnt);
	if (ret) {
		goto out;
	}

	for (i = 0; i < get_cnt; i++) {
		mode = msdisp_mode_from_cea_vic(connector->dev, vics[i]);
		if (!mode) {
			continue;
		}
		drm_mode_probed_add(connector, mode);
		cnt++;
	}

out:
	mutex_unlock(&pipeline->hal_lock);
	return cnt;
}

static int msdisp_drm_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	int cnt = 0, i;

	/* This adapter is a VGA/HDMI dongle, not a panel. get_custom_cea_vic
	 * returns no VICs for the VGA port type, so offer the VESA 60Hz modes
	 * from g_support_mode instead. These are the only modes mode_valid
	 * accepts (matches width/height/rate), so build them via drm_cvt_mode.
	 *
	 * The PREFERRED mode comes from the pref_w/pref_h module params
	 * (choices made with the MENU). Reason: applying a *different* mode at
	 * login makes mutter run a modeset while the KMS thread is still copying
	 * the previous framebuffer -> use-after-free -> kernel Oops in
	 * usb_hal_update_frame (gray screen). If the driver already boots at the
	 * user's resolution, GNOME adopts it with no mode change -> no crash.
	 * Default pref_w/pref_h = 800x600, a resolution every VGA monitor
	 * accepts (safe fallback for any user). */
	static const struct { int w, h; } vesa_modes[] = {
		{ 1920, 1080 }, { 1680, 1050 }, { 1440, 900 }, { 1400, 1050 },
		{ 1366, 768 }, { 1360, 768 }, { 1280, 1024 }, { 1280, 960 },
		{ 1280, 800 }, { 1280, 768 }, { 1280, 720 }, { 1280, 600 },
		{ 1152, 864 }, { 1024, 768 }, { 800, 600 }, { 640, 480 },
	};

	cnt = msdisp_drm_add_modes_by_cea_vic(connector);

	mode = drm_cvt_mode(connector->dev, msdisp_drm_pref_w, msdisp_drm_pref_h,
			    60, false, false, false);
	if (mode) {
		mode->type |= DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
		drm_mode_probed_add(connector, mode);
		cnt++;
	}

	for (i = 0; i < ARRAY_SIZE(vesa_modes); i++) {
		if (vesa_modes[i].w == msdisp_drm_pref_w &&
		    vesa_modes[i].h == msdisp_drm_pref_h)
			continue;
		mode = drm_cvt_mode(connector->dev, vesa_modes[i].w, vesa_modes[i].h,
				    60, false, false, false);
		if (mode) {
			mode->type |= DRM_MODE_TYPE_DRIVER;
			drm_mode_probed_add(connector, mode);
			cnt++;
		}
	}
	return cnt;
}

/* connector_helper_funcs.mode_valid took a const mode pointer since kernel 6.13. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
static enum drm_mode_status msdisp_drm_mode_valid(struct drm_connector *connector,
					    const struct drm_display_mode *mode)
#else
static enum drm_mode_status msdisp_drm_mode_valid(struct drm_connector *connector,
					    struct drm_display_mode *mode)
#endif
{
	struct msdisp_drm_pipeline* pipeline;
	struct msdisp_usb_hal* usb_hal;
	int ret;

	pipeline = get_pipeline_by_connector(connector);
	mutex_lock(&pipeline->hal_lock);
	usb_hal = pipeline->usb_hal;
	if (!usb_hal) {
		ret = 1;
		goto out;
	}

	ret = usb_hal->funcs->mode_valid(usb_hal, mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode));

out:
	mutex_unlock(&pipeline->hal_lock);
	return (0 == ret) ? MODE_OK : MODE_BAD;
}

static enum drm_connector_status
msdisp_drm_detect(struct drm_connector *connector, __always_unused bool force)
{
	enum drm_connector_status status;
	struct msdisp_drm_device* msdisp_drm = to_msdisp_drm(connector->dev);
	struct msdisp_usb_hal* usb_hal;
	struct msdisp_drm_pipeline* pipeline;
	struct msdisp_drm_connector* msdisp_connector =
				container_of(connector,
				struct msdisp_drm_connector,
				connector);

	pipeline = &msdisp_drm->pipeline[msdisp_connector->pipeline_index];
	mutex_lock(&pipeline->hal_lock);
	usb_hal = pipeline->usb_hal;
	/* Integrated panel: present whenever the USB transport is bound. There is
	 * no DDC/EDID to probe, so connectivity follows the USB device presence. */
	status = usb_hal ? connector_status_connected : connector_status_disconnected;
	mutex_unlock(&pipeline->hal_lock);

	if (msdisp_connector->status != status) {
		dev_info(connector->dev->dev, "status changed! old:%d new:%d\n",
			 msdisp_connector->status, status);
	}
	msdisp_connector->status = status;

	return status;
}

static void msdisp_drm_connector_destroy(struct drm_connector *connector)
{
	struct msdisp_drm_connector *msdisp_conn =
					container_of(connector,
					struct msdisp_drm_connector,
					connector);

	drm_connector_cleanup(connector);
	kfree(msdisp_conn->edid);
	kfree(msdisp_conn);
}

static struct drm_encoder *msdisp_drm_best_encoder(struct drm_connector *connector)
{
#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE || defined(EL8)
	struct drm_encoder *encoder;

	drm_connector_for_each_possible_encoder(connector, encoder) {
		return encoder;
	}

	return NULL;
#else
	return drm_encoder_find(connector->dev,
				NULL,
				connector->encoder_ids[0]);
#endif
}

static struct drm_connector_helper_funcs msdisp_drm_connector_helper_funcs = {
	.get_modes = msdisp_drm_get_modes,
	.mode_valid = msdisp_drm_mode_valid,
	.best_encoder = msdisp_drm_best_encoder,
};

static const struct drm_connector_funcs msdisp_drm_connector_funcs = {
	.detect = msdisp_drm_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = msdisp_drm_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state
};

struct msdisp_drm_connector* msdisp_drm_connector_init(struct drm_device *dev, struct drm_encoder *encoder, int index)
{
	struct drm_connector *connector;
	struct msdisp_drm_connector* msdisp_conn;

	msdisp_conn = kzalloc(sizeof(struct msdisp_drm_connector), GFP_KERNEL);
	if (!msdisp_conn) {
		return NULL;
	}
		
	connector = &msdisp_conn->connector; 

	/* TODO: Initialize connector with actual connector type */
	drm_connector_init(dev, connector, &msdisp_drm_connector_funcs,
			   DRM_MODE_CONNECTOR_HDMIA);
	drm_connector_helper_add(connector, &msdisp_drm_connector_helper_funcs);
	connector->polled =  DRM_CONNECTOR_POLL_HPD |
		DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

#if KERNEL_VERSION(4, 19, 0) <= LINUX_VERSION_CODE  || defined(EL8)
	drm_connector_attach_encoder(connector, encoder);
#else
	drm_mode_connector_attach_encoder(connector, encoder);
#endif

	msdisp_conn->status = connector_status_unknown;
	msdisp_conn->pipeline_index = index;
	return msdisp_conn;
}
