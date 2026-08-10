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
 * msdisp_drm_drv.c -- Drm driver for MacroSilicon chip 913x and 912x
 */

#include <linux/version.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#if KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_vblank.h>
#elif KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
#include <drm/drmP.h>
#endif
#if KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_probe_helper.h>
#endif
#if KERNEL_VERSION(5, 8, 0) <= LINUX_VERSION_CODE
#include <drm/drm_managed.h>
#endif
#include <drm/drm_atomic_helper.h>

/* fbdev emulation: makes the panel show the kernel text console (fbcon) with no
 * X11/Wayland, and auto-repaints on hotplug. API moved to per-allocator setup
 * in 6.11 (drm_client_setup + .fbdev_probe); older kernels use the generic one. */
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#include <drm/drm_fbdev_shmem.h>
#include <drm/clients/drm_client_setup.h>
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
#include <drm/drm_fbdev_generic.h>
#else
#include <drm/drm_fb_helper.h>
#endif

#include "msdisp_drm_drv.h"
#include "msdisp_plat_drv.h"

/* Timer API renames in kernel 6.16: from_timer() -> timer_container_of(),
 * del_timer() -> timer_delete(). Provide the old spellings on newer kernels. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#endif
#ifndef del_timer
#define del_timer(t) timer_delete(t)
#endif
#endif

#define	MSDISP_DRM_VBLANK_TIMER_OUT_MS				20

static ushort msdisp_drm_initial_pipeline_count = 3;
module_param_named(initial_pipeline_count,
		   msdisp_drm_initial_pipeline_count, ushort, 0644);
MODULE_PARM_DESC(initial_pipeline_count, "Initial DRM device pipeline counts (default: 3)");

int msdisp_drm_get_pipeline_init_count(void)
{
	return msdisp_drm_initial_pipeline_count;
}


void msdisp_drm_sysfs_init(struct msdisp_drm_device * msdisp_drm);
void msdisp_drm_sysfs_exit(struct msdisp_drm_device * msdisp_drm);

/* Standard DRM GEM fops: open/release/ioctl/poll/read + mmap routed through the
 * shmem object's vm ops, and FOP_UNSIGNED_OFFSET set automatically. */
DEFINE_DRM_GEM_FOPS(msdisp_drm_driver_fops);

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
static int msdisp_drm_enable_vblank(struct drm_device *dev, unsigned int pipe)
{
	return 0;
}

static void msdisp_drm_disable_vblank(struct drm_device *dev, unsigned int pipe)
{

}
#endif

static struct drm_driver driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,

	/* GEM dumb buffers + prime import via the standard shmem helpers. */
	DRM_GEM_SHMEM_DRIVER_OPS,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	/* fbdev emulation (shmem) -> the kernel text console renders on the panel. */
	DRM_FBDEV_SHMEM_DRIVER_OPS,
#endif

	.fops = &msdisp_drm_driver_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 14, 0)
	.date = DRIVER_DATE,   /* struct drm_driver.date removed in kernel 6.14 */
#endif
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCH,
};

static void msdisp_drm_handle_page_flip(struct msdisp_drm_pipeline* pipeline)
{
	struct drm_crtc* crtc = pipeline->crtc;
	struct drm_device* dev = crtc->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	if (pipeline->event) {
		drm_crtc_send_vblank_event(crtc, pipeline->event);
		pipeline->event = NULL;
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static void msidsip_drm_timer_func(struct timer_list* t)
{
	struct msdisp_drm_device *msdisp = from_timer(msdisp, t, vblank_timer); 
	struct drm_crtc* crtc;
	int i;

	for (i = 0; i < msdisp->pipeline_cnt; i++) {
		crtc = msdisp->pipeline[i].crtc;
		drm_crtc_handle_vblank(crtc);
		msdisp_drm_handle_page_flip(&msdisp->pipeline[i]);
	}

	mod_timer(&msdisp->vblank_timer, jiffies + msecs_to_jiffies(MSDISP_DRM_VBLANK_TIMER_OUT_MS));
}

static int msdisp_drm_init(struct msdisp_drm_device *msdisp)
{
	struct drm_device *dev = &msdisp->drm;
	int i;
	int ret = -ENOMEM;

	for (i = 0; i < msdisp->pipeline_cnt; i++) {
		msdisp->pipeline[i].drm_status = MSDISP_DRM_STATUS_DISABLE;
		mutex_init(&msdisp->pipeline[i].hal_lock);
	}

	timer_setup(&msdisp->vblank_timer, msidsip_drm_timer_func, 0);
	msdisp->vblank_timer.expires = (jiffies + msecs_to_jiffies(MSDISP_DRM_VBLANK_TIMER_OUT_MS));
	add_timer(&msdisp->vblank_timer);
 
	ret = msdisp_drm_modeset_init(dev);
	if (ret) {
		goto err;
	}

#if KERNEL_VERSION(6, 0, 0) <= LINUX_VERSION_CODE
	dev->vblank_disable_immediate = true;
#else
#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
#if IS_ENABLED(CONFIG_DRM_LEGACY)
	dev->irq_enabled = true;
#endif
#else
	dev->irq_enabled = true;
#endif
#endif
    ret = drm_vblank_init(dev, msdisp->pipeline_cnt);
	if (ret)
		goto err;

	drm_kms_helper_poll_init(dev);

err:
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0)
#else
/* added by Fuzhao Jin */
static void devm_drm_dev_init_release(void *data)
{
	drm_dev_put(data);
}

int devm_drm_dev_init(struct device *parent,
		      struct drm_device *dev,
		      struct drm_driver *driver)
{
	int ret;

	ret = drm_dev_init(dev, driver, parent);
	if (ret)
		return ret;

	ret = devm_add_action(parent, devm_drm_dev_init_release, dev);
	if (ret)
		devm_drm_dev_init_release(dev);

	return ret;
}

void *__devm_drm_dev_alloc(struct device *parent, struct drm_driver *driver,
			   size_t size, size_t offset)
{
	void *container;
	struct drm_device *drm;
	int ret;

	container = kzalloc(size, GFP_KERNEL);
	if (!container)
		return ERR_PTR(-ENOMEM);

	drm = container + offset;
	ret = devm_drm_dev_init(parent, drm, driver);
	if (ret) {
		kfree(container);
		return ERR_PTR(ret);
	}

	return container;
}

#define devm_drm_dev_alloc(parent, driver, type, member) \
	((type *) __devm_drm_dev_alloc(parent, driver, sizeof(type), \
				       offsetof(type, member)))
#endif

struct drm_device *msdisp_drm_device_create(struct device *parent)
{
    struct msdisp_drm_device *msdisp_drm = NULL;
	struct drm_device *drm = NULL;
	int ret, i, alloc_fifo_cnt = 0;

	if (msdisp_drm_initial_pipeline_count > MSDISP_DRM_MAX_PIPELINE_CNT) {
		printk("%s: Max pipeline is :%d module param is:%d\n", __func__, MSDISP_DRM_MAX_PIPELINE_CNT, msdisp_drm_initial_pipeline_count);
		return NULL;
	}

	msdisp_drm = devm_drm_dev_alloc(parent, &driver, struct msdisp_drm_device, drm);

    if(NULL == msdisp_drm) {
        dev_err(parent, "alloc msdisp drm device failed!\n");
        return NULL;
    }

    drm = &msdisp_drm->drm;
	msdisp_drm->pipeline_cnt = msdisp_drm_initial_pipeline_count;


	for (i = 0; i < msdisp_drm->pipeline_cnt; i++) {
		ret = kfifo_alloc(&msdisp_drm->pipeline[i].fifo, 1024, GFP_KERNEL);
		if (ret) {
			dev_err(drm->dev, "alloc kfifo%d failed!\n ret = %d\n", i, ret);
			ret = -ENOMEM;
			goto err_free;
		}
		alloc_fifo_cnt = i;
	}
	
	ret = msdisp_drm_init(msdisp_drm);
	if (ret)
		goto err_free;


	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_free;

	/* Activate fbdev emulation: the panel shows the kernel text console (fbcon)
	 * with no X11/Wayland, and the fbdev client repaints automatically on hotplug. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	drm_client_setup(drm, NULL);
#else
	drm_fbdev_generic_setup(drm, 32);
#endif

	msdisp_drm_sysfs_init(msdisp_drm);
	return drm;

err_free:
	for (i = 0; i < alloc_fifo_cnt; i++) {
		(void)kfifo_free(&msdisp_drm->pipeline[i].fifo);
	}
	return ERR_PTR(ret);
}

int msdisp_drm_device_remove(struct drm_device *drm)
{
	int i;
	struct msdisp_drm_device* msdisp_drm = to_msdisp_drm(drm);

	for (i = 0; i < msdisp_drm->pipeline_cnt; i++) {
		(void)kfifo_free(&msdisp_drm->pipeline[i].fifo);
	}

	del_timer(&msdisp_drm->vblank_timer);
	msdisp_drm_sysfs_exit(msdisp_drm);
	drm_dev_unplug(drm);

	return 0;
}