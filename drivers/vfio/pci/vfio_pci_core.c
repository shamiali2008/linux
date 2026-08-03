// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/aperture.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/pagemap.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vgaarb.h>
#include <linux/nospec.h>
#include <linux/sched/mm.h>
#include <linux/iommufd.h>
#include <linux/pci-p2pdma.h>
#include <linux/seq_file.h>
#if IS_ENABLED(CONFIG_EEH)
#include <asm/eeh.h>
#endif

#include "vfio_pci_priv.h"

#define DRIVER_AUTHOR   "Alex Williamson <alex.williamson@redhat.com>"
#define DRIVER_DESC "core driver for VFIO based PCI devices"

static void vfio_pci_eventfd_rcu_free(struct rcu_head *rcu)
{
	struct vfio_pci_eventfd *eventfd =
		container_of(rcu, struct vfio_pci_eventfd, rcu);

	eventfd_ctx_put(eventfd->ctx);
	kfree(eventfd);
}

int vfio_pci_eventfd_replace_locked(struct vfio_pci_core_device *vdev,
				    struct vfio_pci_eventfd __rcu **peventfd,
				    struct eventfd_ctx *ctx)
{
	struct vfio_pci_eventfd *new = NULL;
	struct vfio_pci_eventfd *old;

	lockdep_assert_held(&vdev->igate);

	if (ctx) {
		new = kzalloc_obj(*new, GFP_KERNEL_ACCOUNT);
		if (!new)
			return -ENOMEM;

		new->ctx = ctx;
	}

	old = rcu_replace_pointer(*peventfd, new,
				  lockdep_is_held(&vdev->igate));
	if (old)
		call_rcu(&old->rcu, vfio_pci_eventfd_rcu_free);

	return 0;
}

/* List of PF's that vfio_pci_core_sriov_configure() has been called on */
static DEFINE_MUTEX(vfio_pci_sriov_pfs_mutex);
static LIST_HEAD(vfio_pci_sriov_pfs);

struct vfio_pci_dummy_resource {
	struct resource		resource;
	int			index;
	struct list_head	res_next;
};

struct vfio_pci_vf_token {
	struct mutex		lock;
	uuid_t			uuid;
	int			users;
};

static inline bool vfio_vga_disabled(struct vfio_pci_core_device *vdev)
{
#ifdef CONFIG_VFIO_PCI_VGA
	return vdev->disable_vga;
#else
	return true;
#endif
}

#ifdef CONFIG_VFIO_DEBUGFS
static struct vfio_pci_core_device *
vfio_pci_core_debugfs_private(struct seq_file *seq)
{
	struct device *dev = seq->private;
	struct vfio_device *core_vdev = container_of(dev, struct vfio_device,
						     device);

	return container_of(core_vdev, struct vfio_pci_core_device, vdev);
}

static int vfio_pci_core_debugfs_nointxmask(struct seq_file *seq, void *data)
{
	struct vfio_pci_core_device *vdev = vfio_pci_core_debugfs_private(seq);

	seq_puts(seq, vdev->nointxmask ? "Y\n" : "N\n");
	return 0;
}

static int vfio_pci_core_debugfs_disable_idle_d3(struct seq_file *seq,
						 void *data)
{
	struct vfio_pci_core_device *vdev = vfio_pci_core_debugfs_private(seq);

	seq_puts(seq, vdev->disable_idle_d3 ? "Y\n" : "N\n");
	return 0;
}

/*
 * disable_idle_d3 and nointxmask are writable module parameters latched
 * per device at init, so a device's effective value can differ from the
 * current parameter setting.  Expose the per-device (read-only) values
 * here for visibility; read-only parameters can't drift and are omitted.
 */
static void vfio_pci_core_debugfs_init(struct vfio_pci_core_device *vdev)
{
	struct device *dev = &vdev->vdev.device;
	struct dentry *pci_dir;

	if (IS_ERR_OR_NULL(vdev->vdev.debug_root))
		return;

	pci_dir = debugfs_create_dir("pci", vdev->vdev.debug_root);
	debugfs_create_devm_seqfile(dev, "nointxmask", pci_dir,
				    vfio_pci_core_debugfs_nointxmask);
	debugfs_create_devm_seqfile(dev, "disable_idle_d3", pci_dir,
				    vfio_pci_core_debugfs_disable_idle_d3);
}
#else
static inline void vfio_pci_core_debugfs_init(struct vfio_pci_core_device *vdev)
{
}
#endif /* CONFIG_VFIO_DEBUGFS */

/*
 * Our VGA arbiter participation is limited since we don't know anything
 * about the device itself.  However, if the device is the only VGA device
 * downstream of a bridge and VFIO VGA support is disabled, then we can
 * safely return legacy VGA IO and memory as not decoded since the user
 * has no way to get to it and routing can be disabled externally at the
 * bridge.
 */
static unsigned int vfio_pci_set_decode(struct pci_dev *pdev, bool single_vga)
{
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);
	struct pci_dev *tmp = NULL;
	unsigned char max_busnr;
	unsigned int decodes;

	if (single_vga || !vfio_vga_disabled(vdev) || pci_is_root_bus(pdev->bus))
		return VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM |
		       VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM;

	max_busnr = pci_bus_max_busnr(pdev->bus);
	decodes = VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;

	while ((tmp = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, tmp)) != NULL) {
		if (tmp == pdev ||
		    pci_domain_nr(tmp->bus) != pci_domain_nr(pdev->bus) ||
		    pci_is_root_bus(tmp->bus))
			continue;

		if (tmp->bus->number >= pdev->bus->number &&
		    tmp->bus->number <= max_busnr) {
			pci_dev_put(tmp);
			decodes |= VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM;
			break;
		}
	}

	return decodes;
}

static void vfio_pci_probe_mmaps(struct vfio_pci_core_device *vdev)
{
	struct resource *res;
	int i;
	struct vfio_pci_dummy_resource *dummy_res;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		int bar = i + PCI_STD_RESOURCES;

		res = &vdev->pdev->resource[bar];

		if (vdev->pdev->non_mappable_bars)
			goto no_mmap;

		if (!(res->flags & IORESOURCE_MEM))
			goto no_mmap;

		/*
		 * The PCI core shouldn't set up a resource with a
		 * type but zero size. But there may be bugs that
		 * cause us to do that.
		 */
		if (!resource_size(res))
			goto no_mmap;

		if (resource_size(res) >= PAGE_SIZE) {
			vdev->bar_mmap_supported[bar] = true;
			continue;
		}

		if (!(res->start & ~PAGE_MASK)) {
			/*
			 * Add a dummy resource to reserve the remainder
			 * of the exclusive page in case that hot-add
			 * device's bar is assigned into it.
			 */
			dummy_res = kzalloc_obj(*dummy_res, GFP_KERNEL_ACCOUNT);
			if (dummy_res == NULL)
				goto no_mmap;

			dummy_res->resource.name = "vfio sub-page reserved";
			dummy_res->resource.start = res->end + 1;
			dummy_res->resource.end = res->start + PAGE_SIZE - 1;
			dummy_res->resource.flags = res->flags;
			if (request_resource(res->parent,
						&dummy_res->resource)) {
				kfree(dummy_res);
				goto no_mmap;
			}
			dummy_res->index = bar;
			list_add(&dummy_res->res_next,
					&vdev->dummy_resources_list);
			vdev->bar_mmap_supported[bar] = true;
			continue;
		}
		/*
		 * Here we don't handle the case when the BAR is not page
		 * aligned because we can't expect the BAR will be
		 * assigned into the same location in a page in guest
		 * when we passthrough the BAR. And it's hard to access
		 * this BAR in userspace because we have no way to get
		 * the BAR's location in a page.
		 */
no_mmap:
		vdev->bar_mmap_supported[bar] = false;
	}
}

struct vfio_pci_group_info;
static void vfio_pci_dev_set_try_reset(struct vfio_device_set *dev_set);
static int vfio_pci_dev_set_hot_reset(struct vfio_device_set *dev_set,
				      struct vfio_pci_group_info *groups,
				      struct iommufd_ctx *iommufd_ctx);

/*
 * INTx masking requires the ability to disable INTx signaling via PCI_COMMAND
 * _and_ the ability detect when the device is asserting INTx via PCI_STATUS.
 * If a device implements the former but not the latter we would typically
 * expect broken_intx_masking be set and require an exclusive interrupt.
 * However since we do have control of the device's ability to assert INTx,
 * we can instead pretend that the device does not implement INTx, virtualizing
 * the pin register to report zero and maintaining DisINTx set on the host.
 */
static bool vfio_pci_nointx(struct pci_dev *pdev)
{
	switch (pdev->vendor) {
	case PCI_VENDOR_ID_INTEL:
		switch (pdev->device) {
		/* All i40e (XL710/X710/XXV710) 10/20/25/40GbE NICs */
		case 0x1572:
		case 0x1574:
		case 0x1580 ... 0x1581:
		case 0x1583 ... 0x158b:
		case 0x37d0 ... 0x37d2:
		/* X550 */
		case 0x1563:
			return true;
		default:
			return false;
		}
	}

	return false;
}

static void vfio_pci_probe_power_state(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;
	u16 pmcsr;

	if (!pdev->pm_cap)
		return;

	pci_read_config_word(pdev, pdev->pm_cap + PCI_PM_CTRL, &pmcsr);

	vdev->needs_pm_restore = !(pmcsr & PCI_PM_CTRL_NO_SOFT_RESET);
}

/*
 * pci_set_power_state() wrapper handling devices which perform a soft reset on
 * D3->D0 transition.  Save state prior to D0/1/2->D3, stash it on the vdev,
 * restore when returned to D0.  Saved separately from pci_saved_state for use
 * by PM capability emulation and separately from pci_dev internal saved state
 * to avoid it being overwritten and consumed around other resets.
 */
int vfio_pci_set_power_state(struct vfio_pci_core_device *vdev, pci_power_t state)
{
	struct pci_dev *pdev = vdev->pdev;
	bool needs_restore = false, needs_save = false;
	int ret;

	/* Prevent changing power state for PFs with VFs enabled */
	if (state > PCI_D0) {
		lockdep_assert_held_write(&vdev->memory_lock);
		if (vdev->sriov_active)
			return -EBUSY;
	}

	if (vdev->needs_pm_restore) {
		if (pdev->current_state < PCI_D3hot && state >= PCI_D3hot) {
			pci_save_state(pdev);
			needs_save = true;
		}

		if (pdev->current_state >= PCI_D3hot && state <= PCI_D0)
			needs_restore = true;
	}

	ret = pci_set_power_state(pdev, state);

	if (!ret) {
		/* D3 might be unsupported via quirk, skip unless in D3 */
		if (needs_save && pdev->current_state >= PCI_D3hot) {
			/*
			 * The current PCI state will be saved locally in
			 * 'pm_save' during the D3hot transition. When the
			 * device state is changed to D0 again with the current
			 * function, then pci_store_saved_state() will restore
			 * the state and will free the memory pointed by
			 * 'pm_save'. There are few cases where the PCI power
			 * state can be changed to D0 without the involvement
			 * of the driver. For these cases, free the earlier
			 * allocated memory first before overwriting 'pm_save'
			 * to prevent the memory leak.
			 */
			kfree(vdev->pm_save);
			vdev->pm_save = pci_store_saved_state(pdev);
		} else if (needs_restore) {
			pci_load_and_free_saved_state(pdev, &vdev->pm_save);
			pci_restore_state(pdev);
		}
	}

	return ret;
}

static int vfio_pci_runtime_pm_entry(struct vfio_pci_core_device *vdev,
				     struct eventfd_ctx *efdctx)
{
	int ret;

	/*
	 * The vdev power related flags are protected with 'memory_lock'
	 * semaphore.
	 */
	ret = vfio_pci_core_access_begin(vdev);
	if (ret)
		return ret;
	vfio_pci_zap_and_down_write_memory_lock(vdev);
	vfio_pci_dma_buf_move(vdev, true);

	if (vdev->pm_runtime_engaged) {
		up_write(&vdev->memory_lock);
		vfio_pci_core_access_end(vdev);
		return -EINVAL;
	}

	vdev->pm_runtime_engaged = true;
	vdev->pm_wake_eventfd_ctx = efdctx;
	pm_runtime_put_noidle(&vdev->pdev->dev);
	up_write(&vdev->memory_lock);
	vfio_pci_core_access_end(vdev);

	return 0;
}

static int vfio_pci_core_pm_entry(struct vfio_pci_core_device *vdev, u32 flags,
				  void __user *arg, size_t argsz)
{
	int ret;

	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET, 0);
	if (ret != 1)
		return ret;

	/*
	 * Inside vfio_pci_runtime_pm_entry(), only the runtime PM usage count
	 * will be decremented. The pm_runtime_put() will be invoked again
	 * while returning from the ioctl and then the device can go into
	 * runtime suspended state.
	 */
	return vfio_pci_runtime_pm_entry(vdev, NULL);
}

static int vfio_pci_core_pm_entry_with_wakeup(
	struct vfio_pci_core_device *vdev, u32 flags,
	struct vfio_device_low_power_entry_with_wakeup __user *arg,
	size_t argsz)
{
	struct vfio_device_low_power_entry_with_wakeup entry;
	struct eventfd_ctx *efdctx;
	int ret;

	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET,
				 sizeof(entry));
	if (ret != 1)
		return ret;

	if (copy_from_user(&entry, arg, sizeof(entry)))
		return -EFAULT;

	if (entry.wakeup_eventfd < 0)
		return -EINVAL;

	efdctx = eventfd_ctx_fdget(entry.wakeup_eventfd);
	if (IS_ERR(efdctx))
		return PTR_ERR(efdctx);

	ret = vfio_pci_runtime_pm_entry(vdev, efdctx);
	if (ret)
		eventfd_ctx_put(efdctx);

	return ret;
}

static void __vfio_pci_runtime_pm_exit(struct vfio_pci_core_device *vdev)
{
	if (vdev->pm_runtime_engaged) {
		vdev->pm_runtime_engaged = false;
		pm_runtime_get_noresume(&vdev->pdev->dev);

		if (vdev->pm_wake_eventfd_ctx) {
			eventfd_ctx_put(vdev->pm_wake_eventfd_ctx);
			vdev->pm_wake_eventfd_ctx = NULL;
		}
	}
}

static void vfio_pci_runtime_pm_exit(struct vfio_pci_core_device *vdev)
{
	/*
	 * The vdev power related flags are protected with 'memory_lock'
	 * semaphore.
	 */
	down_write(&vdev->memory_lock);
	__vfio_pci_runtime_pm_exit(vdev);
	if (__vfio_pci_memory_enabled(vdev))
		vfio_pci_dma_buf_move(vdev, false);
	up_write(&vdev->memory_lock);
}

static int vfio_pci_core_pm_exit(struct vfio_pci_core_device *vdev, u32 flags,
				 void __user *arg, size_t argsz)
{
	int ret;

	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET, 0);
	if (ret != 1)
		return ret;

	/*
	 * The device is always in the active state here due to pm wrappers
	 * around ioctls. If the device had entered a low power state and
	 * pm_wake_eventfd_ctx is valid, vfio_pci_core_runtime_resume() has
	 * already signaled the eventfd and exited low power mode itself.
	 * pm_runtime_engaged protects the redundant call here.
	 */
	ret = vfio_pci_core_access_begin(vdev);
	if (ret)
		return ret;
	vfio_pci_runtime_pm_exit(vdev);
	vfio_pci_core_access_end(vdev);
	return 0;
}

#ifdef CONFIG_PM
static int vfio_pci_core_runtime_suspend(struct device *dev)
{
	struct vfio_pci_core_device *vdev = dev_get_drvdata(dev);

	down_write(&vdev->memory_lock);
	/*
	 * The user can move the device into D3hot state before invoking
	 * power management IOCTL. Move the device into D0 state here and then
	 * the pci-driver core runtime PM suspend function will move the device
	 * into the low power state. Also, for the devices which have
	 * NoSoftRst-, it will help in restoring the original state
	 * (saved locally in 'vdev->pm_save').
	 */
	vfio_pci_set_power_state(vdev, PCI_D0);
	up_write(&vdev->memory_lock);

	/*
	 * If INTx is enabled, then mask INTx before going into the runtime
	 * suspended state and unmask the same in the runtime resume.
	 * If INTx has already been masked by the user, then
	 * vfio_pci_intx_mask() will return false and in that case, INTx
	 * should not be unmasked in the runtime resume.
	 */
	vdev->pm_intx_masked = ((vdev->irq_type == VFIO_PCI_INTX_IRQ_INDEX) &&
				vfio_pci_intx_mask(vdev));

	return 0;
}

static int vfio_pci_core_runtime_resume(struct device *dev)
{
	struct vfio_pci_core_device *vdev = dev_get_drvdata(dev);

	/*
	 * Resume with a pm_wake_eventfd_ctx signals the eventfd and exit
	 * low power mode.
	 */
	down_write(&vdev->memory_lock);
	if (vdev->pm_wake_eventfd_ctx) {
		eventfd_signal(vdev->pm_wake_eventfd_ctx);
		__vfio_pci_runtime_pm_exit(vdev);
	}
	up_write(&vdev->memory_lock);

	if (vdev->pm_intx_masked)
		vfio_pci_intx_unmask(vdev);

	return 0;
}
#endif /* CONFIG_PM */

/*
 * Eager-request BAR resources, and iomap them.  Soft failures are
 * allowed, and consumers must check the barmap before use in order to
 * give compatible user-visible behaviour with the previous on-demand
 * allocation method.
 */
static void vfio_pci_core_map_bars(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;
	int i;

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		int bar = i + PCI_STD_RESOURCES;

		vdev->barmap[bar] = IOMEM_ERR_PTR(-ENODEV);

		if (pdev->non_mappable_bars)
			continue;

		if (!pci_resource_len(pdev, i))
			continue;

		if (pci_request_selected_regions(pdev, 1 << bar, "vfio")) {
			pci_dbg(pdev, "Failed to reserve region %d\n", bar);
			vdev->barmap[bar] = IOMEM_ERR_PTR(-EBUSY);
			continue;
		}

		vdev->barmap[bar] = pci_iomap(pdev, bar, 0);
		if (!vdev->barmap[bar]) {
			pci_dbg(pdev, "Failed to iomap region %d\n", bar);
			pci_release_selected_regions(pdev, 1 << bar);
			vdev->barmap[bar] = IOMEM_ERR_PTR(-ENOMEM);
		}
	}
}

/*
 * The pci-driver core runtime PM routines always save the device state
 * before going into suspended state. If the device is going into low power
 * state with only with runtime PM ops, then no explicit handling is needed
 * for the devices which have NoSoftRst-.
 */
static const struct dev_pm_ops vfio_pci_core_pm_ops = {
	SET_RUNTIME_PM_OPS(vfio_pci_core_runtime_suspend,
			   vfio_pci_core_runtime_resume,
			   NULL)
};

int vfio_pci_core_enable(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;
	bool supported = vdev->pci_recovery_supported;
	int ret;
	u16 cmd;
	u8 msix_pos;

	if (supported) {
		down_write(&vdev->recovery_lock);
		if (pci_dev_is_disconnected(pdev)) {
			up_write(&vdev->recovery_lock);
			return -ENODEV;
		}

		vdev->pci_recovery_command_valid = false;
		WRITE_ONCE(vdev->pci_recovery_device_open, false);
		up_write(&vdev->recovery_lock);
	}

	if (!vdev->disable_idle_d3) {
		ret = pm_runtime_resume_and_get(&pdev->dev);
		if (ret < 0)
			return ret;
	}

	/* Don't allow our initial saved state to include busmaster */
	pci_clear_master(pdev);

	ret = pci_enable_device(pdev);
	if (ret)
		goto out_power;

	/* If reset fails because of the device lock, fail this path entirely */
	ret = pci_try_reset_function(pdev);
	if (ret == -EAGAIN)
		goto out_disable_device;

	vdev->reset_works = !ret;
	pci_save_state(pdev);
	vdev->pci_saved_state = pci_store_saved_state(pdev);
	if (!vdev->pci_saved_state)
		pci_dbg(pdev, "%s: Couldn't store saved state\n", __func__);

	if (likely(!vdev->nointxmask)) {
		if (vfio_pci_nointx(pdev)) {
			pci_info(pdev, "Masking broken INTx support\n");
			vdev->nointx = true;
			pci_intx(pdev, 0);
		} else
			vdev->pci_2_3 = pci_intx_mask_supported(pdev);
	}

	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	if (vdev->pci_2_3 && (cmd & PCI_COMMAND_INTX_DISABLE)) {
		cmd &= ~PCI_COMMAND_INTX_DISABLE;
		pci_write_config_word(pdev, PCI_COMMAND, cmd);
	}

	ret = vfio_pci_zdev_open_device(vdev);
	if (ret)
		goto out_free_state;

	ret = vfio_config_init(vdev);
	if (ret)
		goto out_free_zdev;

	msix_pos = pdev->msix_cap;
	if (msix_pos) {
		u16 flags;
		u32 table;

		pci_read_config_word(pdev, msix_pos + PCI_MSIX_FLAGS, &flags);
		pci_read_config_dword(pdev, msix_pos + PCI_MSIX_TABLE, &table);

		vdev->msix_bar = table & PCI_MSIX_TABLE_BIR;
		vdev->msix_offset = table & PCI_MSIX_TABLE_OFFSET;
		vdev->msix_size = ((flags & PCI_MSIX_FLAGS_QSIZE) + 1) * 16;
		vdev->has_dyn_msix = pci_msix_can_alloc_dyn(pdev);
	} else {
		vdev->msix_bar = 0xFF;
		vdev->has_dyn_msix = false;
	}

	if (!vfio_vga_disabled(vdev) && vfio_pci_is_vga(pdev))
		vdev->has_vga = true;

	vfio_pci_core_map_bars(vdev);

	return 0;

out_free_zdev:
	vfio_pci_zdev_close_device(vdev);
out_free_state:
	kfree(vdev->pci_saved_state);
	vdev->pci_saved_state = NULL;
out_disable_device:
	pci_disable_device(pdev);
out_power:
	if (!vdev->disable_idle_d3)
		pm_runtime_put(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_enable);

void vfio_pci_core_disable(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *bridge;
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_dummy_resource *dummy_res, *tmp;
	struct vfio_pci_ioeventfd *ioeventfd, *ioeventfd_tmp;
	int i, bar;

	/* For needs_reset */
	lockdep_assert_held(&vdev->vdev.dev_set->lock);

	/*
	 * This function can be invoked while the power state is non-D0.
	 * This non-D0 power state can be with or without runtime PM.
	 * vfio_pci_runtime_pm_exit() will internally increment the usage
	 * count corresponding to pm_runtime_put() called during low power
	 * feature entry and then pm_runtime_resume() will wake up the device,
	 * if the device has already gone into the suspended state. Otherwise,
	 * the vfio_pci_set_power_state() will change the device power state
	 * to D0.
	 */
	vfio_pci_runtime_pm_exit(vdev);
	pm_runtime_resume(&pdev->dev);

	/*
	 * This function calls __pci_reset_function_locked() which internally
	 * can use pci_pm_reset() for the function reset. pci_pm_reset() will
	 * fail if the power state is non-D0. Also, for the devices which
	 * have NoSoftRst-, the reset function can cause the PCI config space
	 * reset without restoring the original state (saved locally in
	 * 'vdev->pm_save').
	 */
	vfio_pci_set_power_state(vdev, PCI_D0);

	/* Stop the device from further DMA */
	pci_clear_master(pdev);

	vfio_pci_set_irqs_ioctl(vdev, VFIO_IRQ_SET_DATA_NONE |
				VFIO_IRQ_SET_ACTION_TRIGGER,
				vdev->irq_type, 0, 0, NULL);

	/* Device closed, don't need mutex here */
	list_for_each_entry_safe(ioeventfd, ioeventfd_tmp,
				 &vdev->ioeventfds_list, next) {
		vfio_virqfd_disable(&ioeventfd->virqfd);
		list_del(&ioeventfd->next);
		kfree(ioeventfd);
	}
	vdev->ioeventfds_nr = 0;

	vdev->virq_disabled = false;

	for (i = 0; i < vdev->num_regions; i++)
		vdev->region[i].ops->release(vdev, &vdev->region[i]);

	vdev->num_regions = 0;
	kfree(vdev->region);
	vdev->region = NULL; /* don't krealloc a freed pointer */

	vfio_config_free(vdev);

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		bar = i + PCI_STD_RESOURCES;
		if (IS_ERR_OR_NULL(vdev->barmap[bar]))
			continue;
		pci_iounmap(pdev, vdev->barmap[bar]);
		pci_release_selected_regions(pdev, 1 << bar);
		vdev->barmap[bar] = NULL;
	}

	list_for_each_entry_safe(dummy_res, tmp,
				 &vdev->dummy_resources_list, res_next) {
		list_del(&dummy_res->res_next);
		release_resource(&dummy_res->resource);
		kfree(dummy_res);
	}

	vdev->needs_reset = true;

	vfio_pci_zdev_close_device(vdev);

	/*
	 * If we have saved state, restore it.  If we can reset the device,
	 * even better.  Resetting with current state seems better than
	 * nothing, but saving and restoring current state without reset
	 * is just busy work.
	 */
	if (pci_load_and_free_saved_state(pdev, &vdev->pci_saved_state)) {
		pci_info(pdev, "%s: Couldn't reload saved state\n", __func__);

		if (!vdev->reset_works)
			goto out;

		pci_save_state(pdev);
	}

	/*
	 * Disable INTx and MSI, presumably to avoid spurious interrupts
	 * during reset.  Stolen from pci_reset_function()
	 */
	pci_write_config_word(pdev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE);

	/*
	 * Try to get the locks ourselves to prevent a deadlock. The
	 * success of this is dependent on being able to lock the device,
	 * which is not always possible.
	 * We can not use the "try" reset interface here, which will
	 * overwrite the previously restored configuration information.
	 */
	if (vdev->reset_works) {
		bridge = pci_upstream_bridge(pdev);
		if (bridge && !pci_dev_trylock(bridge))
			goto out_restore_state;
		if (pci_dev_trylock(pdev)) {
			if (!__pci_reset_function_locked(pdev))
				vdev->needs_reset = false;
			pci_dev_unlock(pdev);
		}
		if (bridge)
			pci_dev_unlock(bridge);
	}

out_restore_state:
	pci_restore_state(pdev);
out:
	pci_disable_device(pdev);

	vfio_pci_dev_set_try_reset(vdev->vdev.dev_set);

	/* Put the pm-runtime usage counter acquired during enable */
	if (!vdev->disable_idle_d3)
		pm_runtime_put(&pdev->dev);
}
EXPORT_SYMBOL_GPL(vfio_pci_core_disable);

static void vfio_pci_core_prepare_close(struct vfio_pci_core_device *vdev)
{
	if (!vdev->pci_recovery_supported)
		return;

	down_write(&vdev->recovery_lock);
	WRITE_ONCE(vdev->pci_recovery_enabled, false);
	vdev->pci_recovery_command_valid = false;
	/*
	 * Clear access_blocked before device_open, so a lock-free reader
	 * never sees it set on a device which is no longer open. A
	 * transaction which is still running cannot clear it once
	 * device_open is gone, and paths which refuse work on a blocked
	 * device would then refuse it for good.
	 */
	WRITE_ONCE(vdev->pci_recovery_access_blocked, false);
	WRITE_ONCE(vdev->pci_recovery_device_open, false);
	WRITE_ONCE(vdev->pci_recovery_flags, 0);

	/*
	 * Publish the closing state and drop recovery_lock before any
	 * teardown. Recovery is disabled and its state cleared, so
	 * slot_reset() and resume() become no-ops and a later
	 * error_detected() only follows the legacy notification path.
	 * Holding the lock across vfio_pci_core_disable() protects nothing
	 * and inverts the lock order. disable() reaches pci_reset_bus(),
	 * which takes pci_bus_sem, while error_detected() takes
	 * recovery_lock from under pci_bus_sem.
	 */
	up_write(&vdev->recovery_lock);
	wake_up_all(&vdev->pci_recovery_wait);
}

static void vfio_pci_core_finish_close(struct vfio_device *core_vdev)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	if (vdev->sriov_pf_core_dev) {
		mutex_lock(&vdev->sriov_pf_core_dev->vf_token->lock);
		WARN_ON(!vdev->sriov_pf_core_dev->vf_token->users);
		vdev->sriov_pf_core_dev->vf_token->users--;
		mutex_unlock(&vdev->sriov_pf_core_dev->vf_token->lock);
	}
#if IS_ENABLED(CONFIG_EEH)
	eeh_dev_release(vdev->pdev);
#endif
	vfio_pci_dma_buf_cleanup(vdev);

	vfio_pci_core_disable(vdev);

	mutex_lock(&vdev->igate);
	vfio_pci_eventfd_replace_locked(vdev, &vdev->err_trigger, NULL);
	vfio_pci_eventfd_replace_locked(vdev, &vdev->req_trigger, NULL);
	mutex_unlock(&vdev->igate);
}

void vfio_pci_core_close_device(struct vfio_device *core_vdev)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	vfio_pci_core_prepare_close(vdev);
	vfio_pci_core_finish_close(core_vdev);
}
EXPORT_SYMBOL_GPL(vfio_pci_core_close_device);

void vfio_pci_core_finish_enable(struct vfio_pci_core_device *vdev)
{
	vfio_pci_probe_mmaps(vdev);
#if IS_ENABLED(CONFIG_EEH)
	eeh_dev_open(vdev->pdev);
#endif

	if (vdev->sriov_pf_core_dev) {
		mutex_lock(&vdev->sriov_pf_core_dev->vf_token->lock);
		vdev->sriov_pf_core_dev->vf_token->users++;
		mutex_unlock(&vdev->sriov_pf_core_dev->vf_token->lock);
	}

	if (vdev->pci_recovery_supported) {
		down_write(&vdev->recovery_lock);
		WRITE_ONCE(vdev->pci_recovery_flags, 0);
		vdev->pci_recovery_sequence = 0;
		WRITE_ONCE(vdev->pci_recovery_enabled, false);
		/* Close clears this too. Start unblocked either way. */
		WRITE_ONCE(vdev->pci_recovery_access_blocked, false);
		WRITE_ONCE(vdev->pci_recovery_device_open, true);
		WRITE_ONCE(vdev->pci_recovery_rom_disable, false);
		up_write(&vdev->recovery_lock);
	}
}
EXPORT_SYMBOL_GPL(vfio_pci_core_finish_enable);

static int vfio_pci_get_irq_count(struct vfio_pci_core_device *vdev, int irq_type)
{
	if (irq_type == VFIO_PCI_INTX_IRQ_INDEX) {
		return vdev->vconfig[PCI_INTERRUPT_PIN] ? 1 : 0;
	} else if (irq_type == VFIO_PCI_MSI_IRQ_INDEX) {
		u8 pos;
		u16 flags;

		pos = vdev->pdev->msi_cap;
		if (pos) {
			pci_read_config_word(vdev->pdev,
					     pos + PCI_MSI_FLAGS, &flags);
			return 1 << ((flags & PCI_MSI_FLAGS_QMASK) >> 1);
		}
	} else if (irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
		u8 pos;
		u16 flags;

		pos = vdev->pdev->msix_cap;
		if (pos) {
			pci_read_config_word(vdev->pdev,
					     pos + PCI_MSIX_FLAGS, &flags);

			return (flags & PCI_MSIX_FLAGS_QSIZE) + 1;
		}
	} else if (irq_type == VFIO_PCI_ERR_IRQ_INDEX) {
		return 1;
	} else if (irq_type == VFIO_PCI_REQ_IRQ_INDEX) {
		return 1;
	}

	return 0;
}

static int vfio_pci_count_devs(struct pci_dev *pdev, void *data)
{
	(*(int *)data)++;
	return 0;
}

struct vfio_pci_fill_info {
	struct vfio_device *vdev;
	struct vfio_pci_dependent_device *devices;
	int nr_devices;
	u32 count;
	u32 flags;
};

static int vfio_pci_fill_devs(struct pci_dev *pdev, void *data)
{
	struct vfio_pci_dependent_device *info;
	struct vfio_pci_fill_info *fill = data;

	/* The topology changed since we counted devices */
	if (fill->count >= fill->nr_devices)
		return -EAGAIN;

	info = &fill->devices[fill->count++];
	info->segment = pci_domain_nr(pdev->bus);
	info->bus = pdev->bus->number;
	info->devfn = pdev->devfn;

	if (fill->flags & VFIO_PCI_HOT_RESET_FLAG_DEV_ID) {
		struct iommufd_ctx *iommufd = vfio_iommufd_device_ictx(fill->vdev);
		struct vfio_device_set *dev_set = fill->vdev->dev_set;
		struct vfio_device *vdev;

		/*
		 * hot-reset requires all affected devices be represented in
		 * the dev_set.
		 */
		vdev = vfio_find_device_in_devset(dev_set, &pdev->dev);
		if (!vdev) {
			info->devid = VFIO_PCI_DEVID_NOT_OWNED;
		} else {
			int id = vfio_iommufd_get_dev_id(vdev, iommufd);

			if (id > 0)
				info->devid = id;
			else if (id == -ENOENT)
				info->devid = VFIO_PCI_DEVID_OWNED;
			else
				info->devid = VFIO_PCI_DEVID_NOT_OWNED;
		}
		/* If devid is VFIO_PCI_DEVID_NOT_OWNED, clear owned flag. */
		if (info->devid == VFIO_PCI_DEVID_NOT_OWNED)
			fill->flags &= ~VFIO_PCI_HOT_RESET_FLAG_DEV_ID_OWNED;
	} else {
		struct iommu_group *iommu_group;

		iommu_group = iommu_group_get(&pdev->dev);
		if (!iommu_group)
			return -EPERM; /* Cannot reset non-isolated devices */

		info->group_id = iommu_group_id(iommu_group);
		iommu_group_put(iommu_group);
	}

	return 0;
}

struct vfio_pci_group_info {
	int count;
	struct file **files;
};

static bool vfio_pci_dev_below_slot(struct pci_dev *pdev, struct pci_slot *slot)
{
	for (; pdev; pdev = pdev->bus->self)
		if (pdev->bus == slot->bus)
			return (pdev->slot == slot);
	return false;
}

struct vfio_pci_walk_info {
	int (*fn)(struct pci_dev *pdev, void *data);
	void *data;
	struct pci_dev *pdev;
	bool slot;
	int ret;
};

static int vfio_pci_walk_wrapper(struct pci_dev *pdev, void *data)
{
	struct vfio_pci_walk_info *walk = data;

	if (!walk->slot || vfio_pci_dev_below_slot(pdev, walk->pdev->slot))
		walk->ret = walk->fn(pdev, walk->data);

	return walk->ret;
}

static int vfio_pci_for_each_slot_or_bus(struct pci_dev *pdev,
					 int (*fn)(struct pci_dev *,
						   void *data), void *data,
					 bool slot)
{
	struct vfio_pci_walk_info walk = {
		.fn = fn, .data = data, .pdev = pdev, .slot = slot, .ret = 0,
	};

	pci_walk_bus(pdev->bus, vfio_pci_walk_wrapper, &walk);

	return walk.ret;
}

static int msix_mmappable_cap(struct vfio_pci_core_device *vdev,
			      struct vfio_info_cap *caps)
{
	struct vfio_info_cap_header header = {
		.id = VFIO_REGION_INFO_CAP_MSIX_MAPPABLE,
		.version = 1
	};

	return vfio_info_add_capability(caps, &header, sizeof(header));
}

int vfio_pci_core_register_dev_region(struct vfio_pci_core_device *vdev,
				      unsigned int type, unsigned int subtype,
				      const struct vfio_pci_regops *ops,
				      size_t size, u32 flags, void *data)
{
	struct vfio_pci_region *region;

	region = krealloc(vdev->region,
			  (vdev->num_regions + 1) * sizeof(*region),
			  GFP_KERNEL_ACCOUNT);
	if (!region)
		return -ENOMEM;

	vdev->region = region;
	vdev->region[vdev->num_regions].type = type;
	vdev->region[vdev->num_regions].subtype = subtype;
	vdev->region[vdev->num_regions].ops = ops;
	vdev->region[vdev->num_regions].size = size;
	vdev->region[vdev->num_regions].flags = flags;
	vdev->region[vdev->num_regions].data = data;

	vdev->num_regions++;

	return 0;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_register_dev_region);

static int vfio_pci_info_atomic_cap(struct vfio_pci_core_device *vdev,
				    struct vfio_info_cap *caps)
{
	struct vfio_device_info_cap_pci_atomic_comp cap = {
		.header.id = VFIO_DEVICE_INFO_CAP_PCI_ATOMIC_COMP,
		.header.version = 1
	};
	struct pci_dev *pdev = pci_physfn(vdev->pdev);
	u32 devcap2;

	pcie_capability_read_dword(pdev, PCI_EXP_DEVCAP2, &devcap2);

	if ((devcap2 & PCI_EXP_DEVCAP2_ATOMIC_COMP32) &&
	    !pci_enable_atomic_ops_to_root(pdev, PCI_EXP_DEVCAP2_ATOMIC_COMP32))
		cap.flags |= VFIO_PCI_ATOMIC_COMP32;

	if ((devcap2 & PCI_EXP_DEVCAP2_ATOMIC_COMP64) &&
	    !pci_enable_atomic_ops_to_root(pdev, PCI_EXP_DEVCAP2_ATOMIC_COMP64))
		cap.flags |= VFIO_PCI_ATOMIC_COMP64;

	if ((devcap2 & PCI_EXP_DEVCAP2_ATOMIC_COMP128) &&
	    !pci_enable_atomic_ops_to_root(pdev,
					   PCI_EXP_DEVCAP2_ATOMIC_COMP128))
		cap.flags |= VFIO_PCI_ATOMIC_COMP128;

	if (!cap.flags)
		return -ENODEV;

	return vfio_info_add_capability(caps, &cap.header, sizeof(cap));
}

static int vfio_pci_ioctl_get_info(struct vfio_pci_core_device *vdev,
				   struct vfio_device_info __user *arg)
{
	unsigned long minsz = offsetofend(struct vfio_device_info, num_irqs);
	struct vfio_device_info info = {};
	struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
	int ret;

	if (copy_from_user(&info, arg, minsz))
		return -EFAULT;

	if (info.argsz < minsz)
		return -EINVAL;

	minsz = min_t(size_t, info.argsz, sizeof(info));

	info.flags = VFIO_DEVICE_FLAGS_PCI;

	if (vdev->reset_works)
		info.flags |= VFIO_DEVICE_FLAGS_RESET;

	info.num_regions = VFIO_PCI_NUM_REGIONS + vdev->num_regions;
	info.num_irqs = VFIO_PCI_NUM_IRQS;

	ret = vfio_pci_info_zdev_add_caps(vdev, &caps);
	if (ret && ret != -ENODEV) {
		pci_warn(vdev->pdev,
			 "Failed to setup zPCI info capabilities\n");
		return ret;
	}

	ret = vfio_pci_core_access_begin(vdev);
	if (ret) {
		kfree(caps.buf);
		return ret;
	}
	ret = vfio_pci_info_atomic_cap(vdev, &caps);
	vfio_pci_core_access_end(vdev);
	if (ret && ret != -ENODEV) {
		pci_warn(vdev->pdev,
			 "Failed to setup AtomicOps info capability\n");
		return ret;
	}

	if (caps.size) {
		info.flags |= VFIO_DEVICE_FLAGS_CAPS;
		if (info.argsz < sizeof(info) + caps.size) {
			info.argsz = sizeof(info) + caps.size;
		} else {
			vfio_info_cap_shift(&caps, sizeof(info));
			if (copy_to_user(arg + 1, caps.buf, caps.size)) {
				kfree(caps.buf);
				return -EFAULT;
			}
			info.cap_offset = sizeof(*arg);
		}

		kfree(caps.buf);
	}

	return copy_to_user(arg, &info, minsz) ? -EFAULT : 0;
}

int vfio_pci_ioctl_get_region_info(struct vfio_device *core_vdev,
				   struct vfio_region_info *info,
				   struct vfio_info_cap *caps)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	struct pci_dev *pdev = vdev->pdev;
	int i, ret;

	switch (info->index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		info->size = pdev->cfg_size;
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		break;
	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		info->size = pci_resource_len(pdev, info->index);
		if (!info->size) {
			info->flags = 0;
			break;
		}

		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		if (vdev->bar_mmap_supported[info->index]) {
			info->flags |= VFIO_REGION_INFO_FLAG_MMAP;
			if (info->index == vdev->msix_bar) {
				ret = msix_mmappable_cap(vdev, caps);
				if (ret)
					return ret;
			}
		}

		break;
	case VFIO_PCI_ROM_REGION_INDEX: {
		void __iomem *io;
		size_t size;
		u16 cmd;

		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		info->flags = 0;
		info->size = 0;

		if (pci_resource_start(pdev, PCI_ROM_RESOURCE)) {
			/*
			 * Check ROM content is valid. Need to enable memory
			 * decode for ROM access in pci_map_rom().
			 */
			ret = vfio_pci_core_access_begin(vdev);
			if (ret)
				return ret;
			cmd = vfio_pci_memory_lock_and_enable(vdev);
			io = pci_map_rom(pdev, &size);
			if (io) {
				info->flags = VFIO_REGION_INFO_FLAG_READ;
				/* Report the BAR size, not the ROM size. */
				info->size = pci_resource_len(pdev,
							      PCI_ROM_RESOURCE);
				pci_unmap_rom(pdev, io);
			}
			vfio_pci_memory_unlock_and_restore(vdev, cmd);
			vfio_pci_core_access_end(vdev);
		} else if (pdev->rom && pdev->romlen) {
			info->flags = VFIO_REGION_INFO_FLAG_READ;
			/* Report BAR size as power of two. */
			info->size = roundup_pow_of_two(pdev->romlen);
		}

		break;
	}
	case VFIO_PCI_VGA_REGION_INDEX:
		if (!vdev->has_vga)
			return -EINVAL;

		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		info->size = 0xc0000;
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;

		break;
	default: {
		struct vfio_region_info_cap_type cap_type = {
			.header.id = VFIO_REGION_INFO_CAP_TYPE,
			.header.version = 1
		};

		if (info->index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
			return -EINVAL;
		info->index = array_index_nospec(
			info->index, VFIO_PCI_NUM_REGIONS + vdev->num_regions);

		i = info->index - VFIO_PCI_NUM_REGIONS;

		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		info->size = vdev->region[i].size;
		info->flags = vdev->region[i].flags;

		cap_type.type = vdev->region[i].type;
		cap_type.subtype = vdev->region[i].subtype;

		ret = vfio_info_add_capability(caps, &cap_type.header,
					       sizeof(cap_type));
		if (ret)
			return ret;

		if (vdev->region[i].ops->add_capability) {
			ret = vdev->region[i].ops->add_capability(
				vdev, &vdev->region[i], caps);
			if (ret)
				return ret;
		}
	}
	}
	return 0;
}
EXPORT_SYMBOL_GPL(vfio_pci_ioctl_get_region_info);

/*
 * Which IRQ indexes can reach the device. ERR and REQ are software only.
 * An index added later gets no access guard until it is listed here.
 */
static bool vfio_pci_irq_index_is_device(u32 index)
{
	switch (index) {
	case VFIO_PCI_INTX_IRQ_INDEX:
	case VFIO_PCI_MSI_IRQ_INDEX:
	case VFIO_PCI_MSIX_IRQ_INDEX:
		return true;
	default:
		return false;
	}
}

static int vfio_pci_ioctl_get_irq_info(struct vfio_pci_core_device *vdev,
				       struct vfio_irq_info __user *arg)
{
	unsigned long minsz = offsetofend(struct vfio_irq_info, count);
	struct vfio_irq_info info;
	bool device_irq;
	int ret;

	if (copy_from_user(&info, arg, minsz))
		return -EFAULT;

	if (info.argsz < minsz || info.index >= VFIO_PCI_NUM_IRQS)
		return -EINVAL;

	switch (info.index) {
	case VFIO_PCI_INTX_IRQ_INDEX ... VFIO_PCI_MSIX_IRQ_INDEX:
	case VFIO_PCI_REQ_IRQ_INDEX:
	case VFIO_PCI_ERR_IRQ_INDEX:
		break;
	default:
		return -EINVAL;
	}

	info.flags = VFIO_IRQ_INFO_EVENTFD;

	device_irq = vfio_pci_irq_index_is_device(info.index);
	if (device_irq) {
		ret = vfio_pci_core_access_begin(vdev);
		if (ret)
			return ret;
	}
	info.count = vfio_pci_get_irq_count(vdev, info.index);
	if (device_irq)
		vfio_pci_core_access_end(vdev);

	if (info.index == VFIO_PCI_INTX_IRQ_INDEX)
		info.flags |=
			(VFIO_IRQ_INFO_MASKABLE | VFIO_IRQ_INFO_AUTOMASKED);
	else if (info.index != VFIO_PCI_MSIX_IRQ_INDEX || !vdev->has_dyn_msix)
		info.flags |= VFIO_IRQ_INFO_NORESIZE;

	return copy_to_user(arg, &info, minsz) ? -EFAULT : 0;
}

static int vfio_pci_ioctl_set_irqs(struct vfio_pci_core_device *vdev,
				   struct vfio_irq_set __user *arg)
{
	unsigned long minsz = offsetofend(struct vfio_irq_set, count);
	struct vfio_irq_set hdr;
	u8 *data = NULL;
	bool device_irq;
	int max, ret = 0;
	size_t data_size = 0;

	if (copy_from_user(&hdr, arg, minsz))
		return -EFAULT;

	device_irq = vfio_pci_irq_index_is_device(hdr.index);
	if (device_irq) {
		ret = vfio_pci_core_access_begin(vdev);
		if (ret)
			return ret;
	}
	max = vfio_pci_get_irq_count(vdev, hdr.index);
	/* Dropped for the user copy below, which can fault under userfaultfd. */
	if (device_irq)
		vfio_pci_core_access_end(vdev);

	ret = vfio_set_irqs_validate_and_prepare(&hdr, max, VFIO_PCI_NUM_IRQS,
						 &data_size);
	if (ret)
		return ret;

	if (data_size) {
		data = memdup_user(&arg->data, data_size);
		if (IS_ERR(data))
			return PTR_ERR(data);
	}

	/*
	 * Interrupt teardown reaches vfio_virqfd_disable(), which flushes the
	 * global virqfd cleanup workqueue, so recovery_lock is held here for
	 * as long as work queued by any vfio device takes. Shutdown work waits
	 * for its inject worker, and an ioeventfd inject takes that device's
	 * memory_lock, so the wait can last as long as a reset there. That is
	 * only a wait. Nothing on that workqueue takes recovery_lock, which is
	 * why the ioeventfd write path reads the recovery state without it. A
	 * callback there which used the vfio_pci_core_iowrite*() accessors
	 * would break that and deadlock against a queued writer.
	 */
	if (device_irq) {
		ret = vfio_pci_core_access_begin(vdev);
		if (ret)
			goto out_free;
	}
	mutex_lock(&vdev->igate);

	ret = vfio_pci_set_irqs_ioctl(vdev, hdr.flags, hdr.index, hdr.start,
				      hdr.count, data);

	mutex_unlock(&vdev->igate);
	if (device_irq)
		vfio_pci_core_access_end(vdev);
out_free:
	kfree(data);

	return ret;
}

/*
 * Complete a ROM unmap which could not disable decode through config space.
 * Call once whatever blocked access has finished. A closed device is skipped.
 * It runs without recovery_lock, and close puts the device back through reset
 * and config restore.
 *
 * The IORESOURCE_ROM_ENABLE test is what pci_unmap_rom() would have done.
 * A ROM which firmware left enabled is not ours to turn off.
 */
static void vfio_pci_recovery_rom_disable(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;

	lockdep_assert_held_write(&vdev->recovery_lock);

	if (!vdev->pci_recovery_device_open ||
	    !READ_ONCE(vdev->pci_recovery_rom_disable))
		return;

	if (!(pdev->resource[PCI_ROM_RESOURCE].flags & IORESOURCE_ROM_ENABLE))
		pci_disable_rom(pdev);
	WRITE_ONCE(vdev->pci_recovery_rom_disable, false);
}

int vfio_pci_try_reset_function(struct vfio_pci_core_device *vdev,
				bool reset_power_state)
{
	struct pci_dev *pdev = vdev->pdev;
	bool enabled = false;
	bool supported = vdev->pci_recovery_supported;
	int ret;

	/*
	 * Claim the device against recovery before resetting it. The PCI
	 * error callbacks hold recovery_lock for their whole body, so taking
	 * it for writing here waits for one already running, and
	 * access_blocked keeps a later one away while the lock is dropped.
	 */
	if (supported) {
		down_write(&vdev->recovery_lock);
		if (!vdev->pci_recovery_device_open) {
			ret = -ENODEV;
			goto out_recovery;
		}

		enabled = vdev->pci_recovery_enabled;

		/*
		 * Only claim access_blocked when recovery is enabled.
		 * error_detected() returns early for a device which has not
		 * enabled it, so there is nothing to exclude, and claiming it
		 * anyway would fail the second of two concurrent
		 * VFIO_DEVICE_RESET calls with -EBUSY.
		 */
		if (enabled) {
			if (vdev->pci_recovery_access_blocked) {
				ret = -EBUSY;
				goto out_recovery;
			}
			WRITE_ONCE(vdev->pci_recovery_access_blocked, true);
		}
		up_write(&vdev->recovery_lock);
	}

	/*
	 * On a device which supports recovery, taking recovery_lock for
	 * writing above waited for anything already past its access check,
	 * and if recovery is enabled access_blocked keeps new ones out. Do
	 * not hold recovery_lock while taking memory_lock or running a reset
	 * method, since a reset can take pci_bus_sem.
	 */
	vfio_pci_zap_and_down_write_memory_lock(vdev);

	/*
	 * This function can be invoked while the power state is non-D0. If
	 * pci_try_reset_function() has been called while the power state is
	 * non-D0, then pci_try_reset_function() will internally set the power
	 * state to D0 without vfio driver involvement. For the devices which
	 * have NoSoftRst-, the reset function can cause the PCI config space
	 * reset without restoring the original state (saved locally in
	 * 'vdev->pm_save').
	 */
	if (reset_power_state)
		vfio_pci_set_power_state(vdev, PCI_D0);

	vfio_pci_dma_buf_move(vdev, true);
	ret = pci_try_reset_function(pdev);
	if (__vfio_pci_memory_enabled(vdev))
		vfio_pci_dma_buf_move(vdev, false);
	up_write(&vdev->memory_lock);

	if (enabled) {
		down_write(&vdev->recovery_lock);
		/*
		 * An error callback can have started an event while the lock
		 * was down. Leave the state to it. Only unblock access for a
		 * reset which is still the one holding it.
		 */
		if (vdev->pci_recovery_device_open &&
		    !(vdev->pci_recovery_flags & (VFIO_PCI_RECOVERY_IN_PROGRESS |
						  VFIO_PCI_RECOVERY_FAILED))) {
			vfio_pci_recovery_rom_disable(vdev);
			WRITE_ONCE(vdev->pci_recovery_access_blocked, false);
			vfio_pci_intx_recovery_finish(vdev);
		}
		up_write(&vdev->recovery_lock);
		/*
		 * Access is blocked for the length of the reset, so anything
		 * waiting for it to clear has to be woken here. A later patch
		 * adds the BAR fault path which waits on this.
		 */
		wake_up_all(&vdev->pci_recovery_wait);
	}

	return ret;

out_recovery:
	up_write(&vdev->recovery_lock);
	return ret;
}

static int vfio_pci_ioctl_reset(struct vfio_pci_core_device *vdev,
				void __user *arg)
{
	if (!vdev->reset_works)
		return -EINVAL;

	return vfio_pci_try_reset_function(vdev, true);
}

static int vfio_pci_ioctl_get_pci_hot_reset_info(
	struct vfio_pci_core_device *vdev,
	struct vfio_pci_hot_reset_info __user *arg)
{
	unsigned long minsz =
		offsetofend(struct vfio_pci_hot_reset_info, count);
	struct vfio_pci_dependent_device *devices = NULL;
	struct vfio_pci_hot_reset_info hdr;
	struct vfio_pci_fill_info fill = {};
	bool slot = false;
	int ret, count = 0;

	if (copy_from_user(&hdr, arg, minsz))
		return -EFAULT;

	if (hdr.argsz < minsz)
		return -EINVAL;

	hdr.flags = 0;

	/* Can we do a slot or bus reset or neither? */
	if (!pci_probe_reset_slot(vdev->pdev->slot))
		slot = true;
	else if (pci_probe_reset_bus(vdev->pdev->bus))
		return -ENODEV;

	ret = vfio_pci_for_each_slot_or_bus(vdev->pdev, vfio_pci_count_devs,
					    &count, slot);
	if (ret)
		return ret;

	if (WARN_ON(!count)) /* Should always be at least one */
		return -ERANGE;

	if (count > (hdr.argsz - sizeof(hdr)) / sizeof(*devices)) {
		hdr.count = count;
		ret = -ENOSPC;
		goto header;
	}

	devices = kzalloc_objs(*devices, count);
	if (!devices)
		return -ENOMEM;

	fill.devices = devices;
	fill.nr_devices = count;
	fill.vdev = &vdev->vdev;

	if (vfio_device_cdev_opened(&vdev->vdev))
		fill.flags |= VFIO_PCI_HOT_RESET_FLAG_DEV_ID |
			     VFIO_PCI_HOT_RESET_FLAG_DEV_ID_OWNED;

	mutex_lock(&vdev->vdev.dev_set->lock);
	ret = vfio_pci_for_each_slot_or_bus(vdev->pdev, vfio_pci_fill_devs,
					    &fill, slot);
	mutex_unlock(&vdev->vdev.dev_set->lock);
	if (ret)
		goto out;

	if (copy_to_user(arg->devices, devices,
			 sizeof(*devices) * fill.count)) {
		ret = -EFAULT;
		goto out;
	}

	hdr.count = fill.count;
	hdr.flags = fill.flags;

header:
	if (copy_to_user(arg, &hdr, minsz))
		ret = -EFAULT;
out:
	kfree(devices);
	return ret;
}

static int
vfio_pci_ioctl_pci_hot_reset_groups(struct vfio_pci_core_device *vdev,
				    u32 array_count, bool slot,
				    struct vfio_pci_hot_reset __user *arg)
{
	int32_t *group_fds;
	struct file **files;
	struct vfio_pci_group_info info;
	int file_idx, count = 0, ret = 0;

	/*
	 * We can't let userspace give us an arbitrarily large buffer to copy,
	 * so verify how many we think there could be.  Note groups can have
	 * multiple devices so one group per device is the max.
	 */
	ret = vfio_pci_for_each_slot_or_bus(vdev->pdev, vfio_pci_count_devs,
					    &count, slot);
	if (ret)
		return ret;

	if (array_count > count)
		return -EINVAL;

	group_fds = kzalloc_objs(*group_fds, array_count);
	files = kzalloc_objs(*files, array_count);
	if (!group_fds || !files) {
		kfree(group_fds);
		kfree(files);
		return -ENOMEM;
	}

	if (copy_from_user(group_fds, arg->group_fds,
			   array_count * sizeof(*group_fds))) {
		kfree(group_fds);
		kfree(files);
		return -EFAULT;
	}

	/*
	 * Get the group file for each fd to ensure the group is held across
	 * the reset
	 */
	for (file_idx = 0; file_idx < array_count; file_idx++) {
		struct file *file = fget(group_fds[file_idx]);

		if (!file) {
			ret = -EBADF;
			break;
		}

		/* Ensure the FD is a vfio group FD.*/
		if (!vfio_file_is_group(file)) {
			fput(file);
			ret = -EINVAL;
			break;
		}

		files[file_idx] = file;
	}

	kfree(group_fds);

	/* release reference to groups on error */
	if (ret)
		goto hot_reset_release;

	info.count = array_count;
	info.files = files;

	ret = vfio_pci_dev_set_hot_reset(vdev->vdev.dev_set, &info, NULL);

hot_reset_release:
	for (file_idx--; file_idx >= 0; file_idx--)
		fput(files[file_idx]);

	kfree(files);
	return ret;
}

static int vfio_pci_ioctl_pci_hot_reset(struct vfio_pci_core_device *vdev,
					struct vfio_pci_hot_reset __user *arg)
{
	unsigned long minsz = offsetofend(struct vfio_pci_hot_reset, count);
	struct vfio_pci_hot_reset hdr;
	bool slot = false;

	if (copy_from_user(&hdr, arg, minsz))
		return -EFAULT;

	if (hdr.argsz < minsz || hdr.flags)
		return -EINVAL;

	/* zero-length array is only for cdev opened devices */
	if (!!hdr.count == vfio_device_cdev_opened(&vdev->vdev))
		return -EINVAL;

	/* Can we do a slot or bus reset or neither? */
	if (!pci_probe_reset_slot(vdev->pdev->slot))
		slot = true;
	else if (pci_probe_reset_bus(vdev->pdev->bus))
		return -ENODEV;

	if (hdr.count)
		return vfio_pci_ioctl_pci_hot_reset_groups(vdev, hdr.count, slot, arg);

	return vfio_pci_dev_set_hot_reset(vdev->vdev.dev_set, NULL,
					  vfio_iommufd_device_ictx(&vdev->vdev));
}

static int vfio_pci_ioctl_ioeventfd(struct vfio_pci_core_device *vdev,
				    struct vfio_device_ioeventfd __user *arg)
{
	unsigned long minsz = offsetofend(struct vfio_device_ioeventfd, fd);
	struct vfio_device_ioeventfd ioeventfd;
	int count;

	if (copy_from_user(&ioeventfd, arg, minsz))
		return -EFAULT;

	if (ioeventfd.argsz < minsz)
		return -EINVAL;

	if (ioeventfd.flags & ~VFIO_DEVICE_IOEVENTFD_SIZE_MASK)
		return -EINVAL;

	count = ioeventfd.flags & VFIO_DEVICE_IOEVENTFD_SIZE_MASK;

	if (hweight8(count) != 1 || ioeventfd.fd < -1)
		return -EINVAL;

	return vfio_pci_ioeventfd(vdev, ioeventfd.offset, ioeventfd.data, count,
				  ioeventfd.fd);
}

long vfio_pci_core_ioctl(struct vfio_device *core_vdev, unsigned int cmd,
			 unsigned long arg)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO:
		return vfio_pci_ioctl_get_info(vdev, uarg);
	case VFIO_DEVICE_GET_IRQ_INFO:
		return vfio_pci_ioctl_get_irq_info(vdev, uarg);
	case VFIO_DEVICE_GET_PCI_HOT_RESET_INFO:
		return vfio_pci_ioctl_get_pci_hot_reset_info(vdev, uarg);
	case VFIO_DEVICE_IOEVENTFD:
		return vfio_pci_ioctl_ioeventfd(vdev, uarg);
	case VFIO_DEVICE_PCI_HOT_RESET:
		return vfio_pci_ioctl_pci_hot_reset(vdev, uarg);
	case VFIO_DEVICE_RESET:
		return vfio_pci_ioctl_reset(vdev, uarg);
	case VFIO_DEVICE_SET_IRQS:
		return vfio_pci_ioctl_set_irqs(vdev, uarg);
	default:
		return -ENOTTY;
	}
}
EXPORT_SYMBOL_GPL(vfio_pci_core_ioctl);

static int vfio_pci_core_feature_token(struct vfio_pci_core_device *vdev,
				       u32 flags, uuid_t __user *arg,
				       size_t argsz)
{
	uuid_t uuid;
	int ret;

	if (!vdev->vf_token)
		return -ENOTTY;
	/*
	 * We do not support GET of the VF Token UUID as this could
	 * expose the token of the previous device user.
	 */
	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET,
				 sizeof(uuid));
	if (ret != 1)
		return ret;

	if (copy_from_user(&uuid, arg, sizeof(uuid)))
		return -EFAULT;

	mutex_lock(&vdev->vf_token->lock);
	uuid_copy(&vdev->vf_token->uuid, &uuid);
	mutex_unlock(&vdev->vf_token->lock);
	return 0;
}

int vfio_pci_core_ioctl_feature(struct vfio_device *device, u32 flags,
				void __user *arg, size_t argsz)
{
	struct vfio_pci_core_device *vdev =
		container_of(device, struct vfio_pci_core_device, vdev);

	switch (flags & VFIO_DEVICE_FEATURE_MASK) {
	case VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY:
		return vfio_pci_core_pm_entry(vdev, flags, arg, argsz);
	case VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY_WITH_WAKEUP:
		return vfio_pci_core_pm_entry_with_wakeup(vdev, flags,
							  arg, argsz);
	case VFIO_DEVICE_FEATURE_LOW_POWER_EXIT:
		return vfio_pci_core_pm_exit(vdev, flags, arg, argsz);
	case VFIO_DEVICE_FEATURE_PCI_VF_TOKEN:
		return vfio_pci_core_feature_token(vdev, flags, arg, argsz);
	case VFIO_DEVICE_FEATURE_DMA_BUF:
		return vfio_pci_core_feature_dma_buf(vdev, flags, arg, argsz);
	case VFIO_DEVICE_FEATURE_ZPCI_ERROR:
		return vfio_pci_zdev_feature_err(device, flags, arg, argsz);
	default:
		return -ENOTTY;
	}
}
EXPORT_SYMBOL_GPL(vfio_pci_core_ioctl_feature);

static ssize_t vfio_pci_rw(struct vfio_pci_core_device *vdev, char __user *buf,
			   size_t count, loff_t *ppos, bool iswrite)
{
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	int ret;

	if (index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
		return -EINVAL;

	ret = vfio_pci_core_access_begin(vdev);
	if (ret)
		return ret;
	vfio_pci_core_access_end(vdev);

	/*
	 * Resume with the guard dropped. A resume takes pci_bus_sem, through
	 * pcie_aspm_pm_state_change() and, from D3cold, through
	 * pci_bridge_wait_for_secondary_bus(). The error callbacks take
	 * recovery_lock from under pci_bus_sem, so holding it here would
	 * invert the order.
	 *
	 * The check above only avoids waking a device whose access is already
	 * blocked. A recovery which starts in between is not excluded, and
	 * does not need to be. pcie_do_recovery() has already resumed every
	 * device under the bridge and holds the reference until it finishes.
	 * The region access below takes the guard for itself.
	 */
	ret = pm_runtime_resume_and_get(&vdev->pdev->dev);
	if (ret) {
		pci_info_ratelimited(vdev->pdev, "runtime resume failed %d\n",
				     ret);
		return -EIO;
	}

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		ret = vfio_pci_config_rw(vdev, buf, count, ppos, iswrite);
		break;

	case VFIO_PCI_ROM_REGION_INDEX:
		if (iswrite)
			ret = -EINVAL;
		else
			ret = vfio_pci_bar_rw(vdev, buf, count, ppos, false);
		break;

	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		ret = vfio_pci_bar_rw(vdev, buf, count, ppos, iswrite);
		break;

	case VFIO_PCI_VGA_REGION_INDEX:
		ret = vfio_pci_vga_rw(vdev, buf, count, ppos, iswrite);
		break;

	default:
		index -= VFIO_PCI_NUM_REGIONS;
		ret = vdev->region[index].ops->rw(vdev, buf,
						   count, ppos, iswrite);
		break;
	}

	pm_runtime_put(&vdev->pdev->dev);
	return ret;
}

int vfio_pci_core_access_begin(struct vfio_pci_core_device *vdev)
{
	if (!vdev->pci_recovery_supported)
		return 0;

	down_read(&vdev->recovery_lock);
	if (unlikely(!vdev->pci_recovery_device_open ||
		     vdev->pci_recovery_access_blocked)) {
		up_read(&vdev->recovery_lock);
		return -EIO;
	}

	return 0;
}

void vfio_pci_core_access_end(struct vfio_pci_core_device *vdev)
{
	if (vdev->pci_recovery_supported)
		up_read(&vdev->recovery_lock);
}

ssize_t vfio_pci_core_read(struct vfio_device *core_vdev, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	if (!count)
		return 0;

	return vfio_pci_rw(vdev, buf, count, ppos, false);
}
EXPORT_SYMBOL_GPL(vfio_pci_core_read);

ssize_t vfio_pci_core_write(struct vfio_device *core_vdev, const char __user *buf,
		size_t count, loff_t *ppos)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	if (!count)
		return 0;

	return vfio_pci_rw(vdev, (char __user *)buf, count, ppos, true);
}
EXPORT_SYMBOL_GPL(vfio_pci_core_write);

static void vfio_pci_zap_bars(struct vfio_pci_core_device *vdev)
{
	struct vfio_device *core_vdev = &vdev->vdev;
	loff_t start = VFIO_PCI_INDEX_TO_OFFSET(VFIO_PCI_BAR0_REGION_INDEX);
	loff_t end = VFIO_PCI_INDEX_TO_OFFSET(VFIO_PCI_ROM_REGION_INDEX);
	loff_t len = end - start;

	unmap_mapping_range(core_vdev->inode->i_mapping, start, len, true);
}

void vfio_pci_zap_and_down_write_memory_lock(struct vfio_pci_core_device *vdev)
{
	down_write(&vdev->memory_lock);
	vfio_pci_zap_bars(vdev);
}

u16 vfio_pci_memory_lock_and_enable(struct vfio_pci_core_device *vdev)
{
	u16 cmd;

	down_write(&vdev->memory_lock);
	pci_read_config_word(vdev->pdev, PCI_COMMAND, &cmd);
	if (!(cmd & PCI_COMMAND_MEMORY))
		pci_write_config_word(vdev->pdev, PCI_COMMAND,
				      cmd | PCI_COMMAND_MEMORY);

	return cmd;
}

void vfio_pci_memory_unlock_and_restore(struct vfio_pci_core_device *vdev, u16 cmd)
{
	pci_write_config_word(vdev->pdev, PCI_COMMAND, cmd);
	up_write(&vdev->memory_lock);
}

static unsigned long vma_to_pfn(struct vm_area_struct *vma)
{
	struct vfio_pci_core_device *vdev = vma->vm_private_data;
	int index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);
	u64 pgoff;

	pgoff = vma->vm_pgoff &
		((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);

	return (pci_resource_start(vdev->pdev, index) >> PAGE_SHIFT) + pgoff;
}

vm_fault_t vfio_pci_vmf_insert_pfn(struct vfio_pci_core_device *vdev,
				   struct vm_fault *vmf,
				   unsigned long pfn,
				   unsigned int order)
{
	lockdep_assert_held_read(&vdev->memory_lock);

	if (vdev->pm_runtime_engaged || !__vfio_pci_memory_enabled(vdev))
		return VM_FAULT_SIGBUS;

	if (!order)
		return vmf_insert_pfn(vmf->vma, vmf->address, pfn);

	if (IS_ENABLED(CONFIG_ARCH_SUPPORTS_PMD_PFNMAP) && order == PMD_ORDER)
		return vmf_insert_pfn_pmd(vmf, pfn, false);

	if (IS_ENABLED(CONFIG_ARCH_SUPPORTS_PUD_PFNMAP) && order == PUD_ORDER)
		return vmf_insert_pfn_pud(vmf, pfn, false);

	return VM_FAULT_FALLBACK;
}
EXPORT_SYMBOL_GPL(vfio_pci_vmf_insert_pfn);

/*
 * Whether a fault which found access blocked is worth retrying. Read
 * without recovery_lock, so it is only a hint about when to look again.
 * vfio_pci_fault_trylock_once() takes the lock and decides. Read the flags
 * once so the two tests below see the same value. Every writer which can
 * make this true wakes pci_recovery_wait.
 */
static bool vfio_pci_recovery_done(struct vfio_pci_core_device *vdev)
{
	u32 flags = READ_ONCE(vdev->pci_recovery_flags);

	if (!READ_ONCE(vdev->pci_recovery_device_open))
		return true;
	if (flags & VFIO_PCI_RECOVERY_IN_PROGRESS)
		return false;
	if (flags & VFIO_PCI_RECOVERY_FAILED)
		return true;
	return !READ_ONCE(vdev->pci_recovery_access_blocked);
}

static int vfio_pci_wait_for_recovery(struct vfio_pci_core_device *vdev)
{
	return wait_event_killable(vdev->pci_recovery_wait,
				   vfio_pci_recovery_done(vdev));
}

/* What one look at the recovery state says the fault should do. */
enum vfio_pci_fault_action {
	VFIO_PCI_FAULT_PROCEED,	/* returns with memory_lock held */
	VFIO_PCI_FAULT_WAIT,	/* recovery is running, may still recover */
	VFIO_PCI_FAULT_FAIL,	/* closed, or failed for good */
};

static enum vfio_pci_fault_action
vfio_pci_fault_trylock_once(struct vfio_pci_core_device *vdev)
{
	enum vfio_pci_fault_action action;

	down_read(&vdev->recovery_lock);
	if (!vdev->pci_recovery_device_open ||
	    (vdev->pci_recovery_flags & VFIO_PCI_RECOVERY_FAILED)) {
		action = VFIO_PCI_FAULT_FAIL;
	} else if (vdev->pci_recovery_access_blocked) {
		/*
		 * Blocked for a reason which still ends: a recovery which has
		 * not failed, or a function reset. Test FAILED above rather
		 * than IN_PROGRESS here, so a fault does not fail for good
		 * while a reset is running, or in the window where
		 * error_detected() has blocked access but not yet published
		 * the flags.
		 */
		action = VFIO_PCI_FAULT_WAIT;
	} else {
		down_read(&vdev->memory_lock);
		action = VFIO_PCI_FAULT_PROCEED;
	}
	up_read(&vdev->recovery_lock);

	return action;
}

/*
 * Return true with memory_lock held for a fault that may proceed. Otherwise
 * return false with @ret set to the result the fault handler should return.
 */
static bool vfio_pci_core_fault_trylock(struct vfio_pci_core_device *vdev,
					struct vm_fault *vmf,
					vm_fault_t *ret)
{
	if (!vdev->pci_recovery_supported) {
		down_read(&vdev->memory_lock);
		return true;
	}

	switch (vfio_pci_fault_trylock_once(vdev)) {
	case VFIO_PCI_FAULT_PROCEED:
		return true;
	case VFIO_PCI_FAULT_FAIL:
		*ret = VM_FAULT_SIGBUS;
		return false;
	case VFIO_PCI_FAULT_WAIT:
		break;
	}

	if (fault_flag_allow_retry_first(vmf->flags)) {
		if (vmf->flags & FAULT_FLAG_RETRY_NOWAIT) {
			*ret = VM_FAULT_RETRY;
			return false;
		}

		if (!vfio_device_try_get_registration(&vdev->vdev)) {
			*ret = VM_FAULT_SIGBUS;
			return false;
		}

		release_fault_lock(vmf);
		vfio_pci_wait_for_recovery(vdev);
		vfio_device_put_registration(&vdev->vdev);
		*ret = VM_FAULT_RETRY;
		return false;
	}

	/*
	 * The fault lock cannot be dropped here: either the caller did not
	 * allow a retry, or this fault has already used one. So wait with
	 * it held. It is not a deadlock. Recovery revokes mappings through
	 * unmap_mapping_range(), which never takes mmap_lock. The wait is
	 * killable.
	 */
	if (vfio_pci_wait_for_recovery(vdev)) {
		*ret = VM_FAULT_NOPAGE;
		return false;
	}

	if (vfio_pci_fault_trylock_once(vdev) == VFIO_PCI_FAULT_PROCEED)
		return true;

	*ret = VM_FAULT_SIGBUS;
	return false;
}

static vm_fault_t vfio_pci_mmap_huge_fault(struct vm_fault *vmf,
					   unsigned int order)
{
	struct vm_area_struct *vma = vmf->vma;
	struct vfio_pci_core_device *vdev = vma->vm_private_data;
	unsigned long addr = vmf->address & ~((PAGE_SIZE << order) - 1);
	unsigned long pgoff = linear_page_delta(vma, addr);
	unsigned long pfn = vma_to_pfn(vma) + pgoff;
	vm_fault_t ret = VM_FAULT_FALLBACK;

	if (is_aligned_for_order(vma, addr, pfn, order)) {
		if (!vfio_pci_core_fault_trylock(vdev, vmf, &ret))
			return ret;

		ret = vfio_pci_vmf_insert_pfn(vdev, vmf, pfn, order);
		up_read(&vdev->memory_lock);
	}

	dev_dbg_ratelimited(&vdev->pdev->dev,
			   "%s(,order = %d) BAR %ld page offset 0x%lx: 0x%x\n",
			    __func__, order,
			    vma->vm_pgoff >>
				(VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT),
			    pgoff, (unsigned int)ret);

	return ret;
}

static vm_fault_t vfio_pci_mmap_page_fault(struct vm_fault *vmf)
{
	return vfio_pci_mmap_huge_fault(vmf, 0);
}

static const struct vm_operations_struct vfio_pci_mmap_ops = {
	.fault = vfio_pci_mmap_page_fault,
#ifdef CONFIG_ARCH_SUPPORTS_HUGE_PFNMAP
	.huge_fault = vfio_pci_mmap_huge_fault,
#endif
};

int vfio_pci_core_mmap(struct vfio_device *core_vdev, struct vm_area_struct *vma)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	struct pci_dev *pdev = vdev->pdev;
	unsigned int index;
	u64 phys_len, req_len, pgoff, req_start;
	void __iomem *bar_io;

	index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);

	if (index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
		return -EINVAL;
	if (vma->vm_end < vma->vm_start)
		return -EINVAL;
	if ((vma->vm_flags & VM_SHARED) == 0)
		return -EINVAL;
	if (index >= VFIO_PCI_NUM_REGIONS) {
		int regnum = index - VFIO_PCI_NUM_REGIONS;
		struct vfio_pci_region *region = vdev->region + regnum;

		if (region->ops && region->ops->mmap &&
		    (region->flags & VFIO_REGION_INFO_FLAG_MMAP))
			return region->ops->mmap(vdev, region, vma);
		return -EINVAL;
	}
	if (index >= VFIO_PCI_ROM_REGION_INDEX)
		return -EINVAL;
	if (!vdev->bar_mmap_supported[index])
		return -EINVAL;

	phys_len = PAGE_ALIGN(pci_resource_len(pdev, index));
	req_len = vma->vm_end - vma->vm_start;
	pgoff = vma->vm_pgoff &
		((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);
	req_start = pgoff << PAGE_SHIFT;

	if (req_start + req_len > phys_len)
		return -EINVAL;

	/*
	 * Ensure the BAR resource region is reserved for use.
	 */
	bar_io = vfio_pci_core_get_iomap(vdev, index);
	if (IS_ERR(bar_io))
		return PTR_ERR(bar_io);

	vma->vm_private_data = vdev;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	/*
	 * Set vm_flags now, they should not be changed in the fault handler.
	 * We want the same flags and page protection (decrypted above) as
	 * io_remap_pfn_range() would set.
	 *
	 * VM_ALLOW_ANY_UNCACHED: The VMA flag is implemented for ARM64,
	 * allowing KVM stage 2 device mapping attributes to use Normal-NC
	 * rather than DEVICE_nGnRE, which allows guest mappings
	 * supporting write-combining attributes (WC). ARM does not
	 * architecturally guarantee this is safe, and indeed some MMIO
	 * regions like the GICv2 VCPU interface can trigger uncontained
	 * faults if Normal-NC is used.
	 *
	 * To safely use VFIO in KVM the platform must guarantee full
	 * safety in the guest where no action taken against a MMIO
	 * mapping can trigger an uncontained failure. The assumption is
	 * that most VFIO PCI platforms support this for both mapping types,
	 * at least in common flows, based on some expectations of how
	 * PCI IP is integrated. Hence VM_ALLOW_ANY_UNCACHED is set in
	 * the VMA flags.
	 */
	vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED | VM_IO | VM_PFNMAP |
			VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_ops = &vfio_pci_mmap_ops;

	return 0;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_mmap);

void vfio_pci_core_request(struct vfio_device *core_vdev, unsigned int count)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_eventfd *eventfd;

	rcu_read_lock();
	eventfd = rcu_dereference(vdev->req_trigger);
	if (eventfd) {
		if (!(count % 10))
			pci_notice_ratelimited(pdev,
				"Relaying device request to user (#%u)\n",
				count);
		eventfd_signal(eventfd->ctx);
	} else if (count == 0) {
		pci_warn(pdev,
			"No device request channel registered, blocked until released by user\n");
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(vfio_pci_core_request);

int vfio_pci_core_match_token_uuid(struct vfio_device *core_vdev,
				   const uuid_t *uuid)

{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	/*
	 * There's always some degree of trust or collaboration between SR-IOV
	 * PF and VFs, even if just that the PF hosts the SR-IOV capability and
	 * can disrupt VFs with a reset, but often the PF has more explicit
	 * access to deny service to the VF or access data passed through the
	 * VF.  We therefore require an opt-in via a shared VF token (UUID) to
	 * represent this trust.  This both prevents that a VF driver might
	 * assume the PF driver is a trusted, in-kernel driver, and also that
	 * a PF driver might be replaced with a rogue driver, unknown to in-use
	 * VF drivers.
	 *
	 * Therefore when presented with a VF, if the PF is a vfio device and
	 * it is bound to the vfio-pci driver, the user needs to provide a VF
	 * token to access the device, in the form of appending a vf_token to
	 * the device name, for example:
	 *
	 * "0000:04:10.0 vf_token=bd8d9d2b-5a5f-4f5a-a211-f591514ba1f3"
	 *
	 * When presented with a PF which has VFs in use, the user must also
	 * provide the current VF token to prove collaboration with existing
	 * VF users.  If VFs are not in use, the VF token provided for the PF
	 * device will act to set the VF token.
	 *
	 * If the VF token is provided but unused, an error is generated.
	 */
	if (vdev->pdev->is_virtfn) {
		struct vfio_pci_core_device *pf_vdev = vdev->sriov_pf_core_dev;
		bool match;

		if (!pf_vdev) {
			if (!uuid)
				return 0; /* PF is not vfio-pci, no VF token */

			pci_info_ratelimited(vdev->pdev,
				"VF token incorrectly provided, PF not bound to vfio-pci\n");
			return -EINVAL;
		}

		if (!uuid) {
			pci_info_ratelimited(vdev->pdev,
				"VF token required to access device\n");
			return -EACCES;
		}

		mutex_lock(&pf_vdev->vf_token->lock);
		match = uuid_equal(uuid, &pf_vdev->vf_token->uuid);
		mutex_unlock(&pf_vdev->vf_token->lock);

		if (!match) {
			pci_info_ratelimited(vdev->pdev,
				"Incorrect VF token provided for device\n");
			return -EACCES;
		}
	} else if (vdev->vf_token) {
		mutex_lock(&vdev->vf_token->lock);
		if (vdev->vf_token->users) {
			if (!uuid) {
				mutex_unlock(&vdev->vf_token->lock);
				pci_info_ratelimited(vdev->pdev,
					"VF token required to access device\n");
				return -EACCES;
			}

			if (!uuid_equal(uuid, &vdev->vf_token->uuid)) {
				mutex_unlock(&vdev->vf_token->lock);
				pci_info_ratelimited(vdev->pdev,
					"Incorrect VF token provided for device\n");
				return -EACCES;
			}
		} else if (uuid) {
			uuid_copy(&vdev->vf_token->uuid, uuid);
		}

		mutex_unlock(&vdev->vf_token->lock);
	} else if (uuid) {
		pci_info_ratelimited(vdev->pdev,
			"VF token incorrectly provided, not a PF or VF\n");
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_match_token_uuid);

#define VF_TOKEN_ARG "vf_token="

int vfio_pci_core_match(struct vfio_device *core_vdev, char *buf)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	bool vf_token = false;
	uuid_t uuid;
	int ret;

	if (strncmp(pci_name(vdev->pdev), buf, strlen(pci_name(vdev->pdev))))
		return 0; /* No match */

	if (strlen(buf) > strlen(pci_name(vdev->pdev))) {
		buf += strlen(pci_name(vdev->pdev));

		if (*buf != ' ')
			return 0; /* No match: non-whitespace after name */

		while (*buf) {
			if (*buf == ' ') {
				buf++;
				continue;
			}

			if (!vf_token && !strncmp(buf, VF_TOKEN_ARG,
						  strlen(VF_TOKEN_ARG))) {
				buf += strlen(VF_TOKEN_ARG);

				if (strlen(buf) < UUID_STRING_LEN)
					return -EINVAL;

				ret = uuid_parse(buf, &uuid);
				if (ret)
					return ret;

				vf_token = true;
				buf += UUID_STRING_LEN;
			} else {
				/* Unknown/duplicate option */
				return -EINVAL;
			}
		}
	}

	ret = core_vdev->ops->match_token_uuid(core_vdev,
					       vf_token ? &uuid : NULL);
	if (ret)
		return ret;

	return 1; /* Match */
}
EXPORT_SYMBOL_GPL(vfio_pci_core_match);

static int vfio_pci_bus_notifier(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct vfio_pci_core_device *vdev = container_of(nb,
						    struct vfio_pci_core_device, nb);
	struct device *dev = data;
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_dev *physfn = pci_physfn(pdev);

	if (action == BUS_NOTIFY_ADD_DEVICE &&
	    pdev->is_virtfn && physfn == vdev->pdev) {
		pci_info(vdev->pdev, "Captured SR-IOV VF %s driver_override\n",
			 pci_name(pdev));
		WARN_ON(device_set_driver_override(&pdev->dev,
						   vdev->vdev.ops->name));
	} else if (action == BUS_NOTIFY_BOUND_DRIVER &&
		   pdev->is_virtfn && physfn == vdev->pdev) {
		struct pci_driver *drv = pci_dev_driver(pdev);

		if (drv && drv != pci_dev_driver(vdev->pdev))
			pci_warn(vdev->pdev,
				 "VF %s bound to driver %s while PF bound to driver %s\n",
				 pci_name(pdev), drv->name,
				 pci_dev_driver(vdev->pdev)->name);
	}

	return 0;
}

static int vfio_pci_vf_init(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;
	struct vfio_pci_core_device *cur;
	struct pci_dev *physfn;
	int ret;

	if (pdev->is_virtfn) {
		/*
		 * If this VF was created by our vfio_pci_core_sriov_configure()
		 * then we can find the PF vfio_pci_core_device now, and due to
		 * the locking in pci_disable_sriov() it cannot change until
		 * this VF device driver is removed.
		 */
		physfn = pci_physfn(vdev->pdev);
		mutex_lock(&vfio_pci_sriov_pfs_mutex);
		list_for_each_entry(cur, &vfio_pci_sriov_pfs, sriov_pfs_item) {
			if (cur->pdev == physfn) {
				vdev->sriov_pf_core_dev = cur;
				break;
			}
		}
		mutex_unlock(&vfio_pci_sriov_pfs_mutex);
		return 0;
	}

	/* Not a SRIOV PF */
	if (!pdev->is_physfn)
		return 0;

	vdev->vf_token = kzalloc_obj(*vdev->vf_token);
	if (!vdev->vf_token)
		return -ENOMEM;

	mutex_init(&vdev->vf_token->lock);
	uuid_gen(&vdev->vf_token->uuid);

	vdev->nb.notifier_call = vfio_pci_bus_notifier;
	ret = bus_register_notifier(&pci_bus_type, &vdev->nb);
	if (ret) {
		kfree(vdev->vf_token);
		return ret;
	}
	return 0;
}

static void vfio_pci_vf_uninit(struct vfio_pci_core_device *vdev)
{
	if (!vdev->vf_token)
		return;

	bus_unregister_notifier(&pci_bus_type, &vdev->nb);
	WARN_ON(vdev->vf_token->users);
	mutex_destroy(&vdev->vf_token->lock);
	kfree(vdev->vf_token);
}

static int vfio_pci_vga_init(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;
	int ret;

	if (!vfio_pci_is_vga(pdev))
		return 0;

	ret = aperture_remove_conflicting_pci_devices(pdev, vdev->vdev.ops->name);
	if (ret)
		return ret;

	ret = vga_client_register(pdev, vfio_pci_set_decode);
	if (ret)
		return ret;
	vga_set_legacy_decoding(pdev, vfio_pci_set_decode(pdev, false));
	return 0;
}

static void vfio_pci_vga_uninit(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;

	if (!vfio_pci_is_vga(pdev))
		return;
	vga_client_unregister(pdev);
	vga_set_legacy_decoding(pdev, VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM |
					      VGA_RSRC_LEGACY_IO |
					      VGA_RSRC_LEGACY_MEM);
}

int vfio_pci_core_init_dev(struct vfio_device *core_vdev)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	int ret;

	vdev->pdev = to_pci_dev(core_vdev->dev);
	vdev->irq_type = VFIO_PCI_NUM_IRQS;
	mutex_init(&vdev->igate);
	spin_lock_init(&vdev->irqlock);
	mutex_init(&vdev->ioeventfds_lock);
	INIT_LIST_HEAD(&vdev->dummy_resources_list);
	INIT_LIST_HEAD(&vdev->ioeventfds_list);
	INIT_LIST_HEAD(&vdev->sriov_pfs_item);
	ret = pcim_p2pdma_init(vdev->pdev);
	if (ret && ret != -EOPNOTSUPP)
		return ret;
	INIT_LIST_HEAD(&vdev->dmabufs);
	init_rwsem(&vdev->recovery_lock);
	init_rwsem(&vdev->memory_lock);
	init_waitqueue_head(&vdev->pci_recovery_wait);
	xa_init(&vdev->ctx);

	return 0;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_init_dev);

void vfio_pci_core_release_dev(struct vfio_device *core_vdev)
{
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	mutex_destroy(&vdev->igate);
	mutex_destroy(&vdev->ioeventfds_lock);
	kfree(vdev->region);
	kfree(vdev->pm_save);
}
EXPORT_SYMBOL_GPL(vfio_pci_core_release_dev);

int vfio_pci_core_register_device(struct vfio_pci_core_device *vdev)
{
	struct pci_dev *pdev = vdev->pdev;
	struct device *dev = &pdev->dev;
	int ret;

	/* Drivers must set the vfio_pci_core_device to their drvdata */
	if (WARN_ON(vdev != dev_get_drvdata(dev)))
		return -EINVAL;

	/* Drivers must set a name.  Required for sequestering SR-IOV VFs */
	if (WARN_ON(!vdev->vdev.ops->name))
		return -EINVAL;

	if (pdev->hdr_type != PCI_HEADER_TYPE_NORMAL)
		return -EINVAL;

	if (vdev->vdev.mig_ops) {
		if (!(vdev->vdev.mig_ops->migration_get_state &&
		      vdev->vdev.mig_ops->migration_set_state &&
		      vdev->vdev.mig_ops->migration_get_data_size) ||
		    !(vdev->vdev.migration_flags & VFIO_MIGRATION_STOP_COPY))
			return -EINVAL;
	}

	if (vdev->vdev.log_ops && !(vdev->vdev.log_ops->log_start &&
	    vdev->vdev.log_ops->log_stop &&
	    vdev->vdev.log_ops->log_read_and_clear))
		return -EINVAL;

	/*
	 * Prevent binding to PFs with VFs enabled, the VFs might be in use
	 * by the host or other users.  We cannot capture the VFs if they
	 * already exist, nor can we track VF users.  Disabling SR-IOV here
	 * would initiate removing the VFs, which would unbind the driver,
	 * which is prone to blocking if that VF is also in use by vfio-pci.
	 * Just reject these PFs and let the user sort it out.
	 */
	if (pci_num_vf(pdev)) {
		pci_warn(pdev, "Cannot bind to PF with SR-IOV enabled\n");
		return -EBUSY;
	}

	if (pci_is_root_bus(pdev->bus) || pdev->is_virtfn) {
		ret = vfio_assign_device_set(&vdev->vdev, vdev);
	} else if (!pci_probe_reset_slot(pdev->slot)) {
		ret = vfio_assign_device_set(&vdev->vdev, pdev->slot);
	} else {
		/*
		 * If there is no slot reset support for this device, the whole
		 * bus needs to be grouped together to support bus-wide resets.
		 */
		ret = vfio_assign_device_set(&vdev->vdev, pdev->bus);
	}

	if (ret)
		return ret;
	ret = vfio_pci_vf_init(vdev);
	if (ret)
		return ret;
	ret = vfio_pci_vga_init(vdev);
	if (ret)
		goto out_vf;

	vfio_pci_probe_power_state(vdev);

	/*
	 * pci-core sets the device power state to an unknown value at
	 * bootup and after being removed from a driver.  The only
	 * transition it allows from this unknown state is to D0, which
	 * typically happens when a driver calls pci_enable_device().
	 * We're not ready to enable the device yet, but we do want to
	 * be able to get to D3.  Therefore first do a D0 transition
	 * before enabling runtime PM.
	 */
	vfio_pci_set_power_state(vdev, PCI_D0);

	dev->driver->pm = &vfio_pci_core_pm_ops;
	pm_runtime_allow(dev);
	if (!vdev->disable_idle_d3)
		pm_runtime_put(dev);

	ret = vfio_register_group_dev(&vdev->vdev);
	if (ret)
		goto out_power;

	vfio_pci_core_debugfs_init(vdev);

	return 0;

out_power:
	if (!vdev->disable_idle_d3)
		pm_runtime_get_noresume(dev);

	pm_runtime_forbid(dev);
	vfio_pci_vga_uninit(vdev);
out_vf:
	vfio_pci_vf_uninit(vdev);
	return ret;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_register_device);

void vfio_pci_core_unregister_device(struct vfio_pci_core_device *vdev)
{
	vfio_pci_core_sriov_configure(vdev, 0);

	vfio_unregister_group_dev(&vdev->vdev);

	vfio_pci_vf_uninit(vdev);
	vfio_pci_vga_uninit(vdev);

	if (!vdev->disable_idle_d3)
		pm_runtime_get_noresume(&vdev->pdev->dev);

	pm_runtime_forbid(&vdev->pdev->dev);
}
EXPORT_SYMBOL_GPL(vfio_pci_core_unregister_device);

pci_ers_result_t vfio_pci_core_aer_err_detected(struct pci_dev *pdev,
						pci_channel_state_t state)
{
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);
	struct vfio_pci_eventfd *eventfd;

	rcu_read_lock();
	eventfd = rcu_dereference(vdev->err_trigger);
	if (eventfd)
		eventfd_signal(eventfd->ctx);
	rcu_read_unlock();

	return PCI_ERS_RESULT_CAN_RECOVER;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_aer_err_detected);

static pci_ers_result_t vfio_pci_core_aer_slot_reset(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);
	pci_ers_result_t result = PCI_ERS_RESULT_RECOVERED;
	int ret = 0;

	down_write(&vdev->recovery_lock);
	if (!(vdev->pci_recovery_flags & VFIO_PCI_RECOVERY_IN_PROGRESS) ||
	    !vdev->pci_recovery_device_open) {
		up_write(&vdev->recovery_lock);
		return PCI_ERS_RESULT_NONE;
	}

	/*
	 * Restore first. aer_root_reset() resets the link with
	 * PCI_RESET_NO_RESTORE, so on entry the BARs read as zero. Tearing
	 * down MSI-X before this would have pci_msix_shutdown() write through
	 * the stale table mapping to an address the device no longer decodes.
	 */
	pci_restore_state(pdev);

	/*
	 * Hold recovery_lock across the interrupt teardown.
	 * vfio_pci_core_disable() runs the same teardown on close without
	 * taking igate, and running the per-vector teardown twice frees the
	 * irq, the name and the eventfd context twice.
	 * vfio_pci_core_prepare_close() takes recovery_lock for writing
	 * before it, so holding it here keeps the two apart.
	 */
	mutex_lock(&vdev->igate);
	if (vdev->irq_type < VFIO_PCI_NUM_IRQS)
		ret = vfio_pci_set_irqs_ioctl(vdev,
					      VFIO_IRQ_SET_DATA_NONE |
					      VFIO_IRQ_SET_ACTION_TRIGGER,
					      vdev->irq_type, 0, 0, NULL);
	mutex_unlock(&vdev->igate);

	if (ret) {
		WRITE_ONCE(vdev->pci_recovery_flags,
			   (vdev->pci_recovery_flags |
			    VFIO_PCI_RECOVERY_FAILED) &
			   ~VFIO_PCI_RECOVERY_IN_PROGRESS);
		vdev->pci_recovery_command_valid = false;
		/*
		 * Vote NONE, not DISCONNECT. A DISCONNECT anywhere in the
		 * domain makes the core skip resume() for every device under
		 * the bridge and report permanent failure for all of them.
		 * Our interrupt teardown failing says nothing about the
		 * others, so record it locally and leave the domain verdict
		 * alone.
		 */
		result = PCI_ERS_RESULT_NONE;
	} else {
		WRITE_ONCE(vdev->pci_recovery_flags,
			   vdev->pci_recovery_flags |
			   VFIO_PCI_RECOVERY_RESET);
	}

	up_write(&vdev->recovery_lock);
	/*
	 * Whoever clears IN_PROGRESS owes the wake. resume() will not do it,
	 * since it bails once IN_PROGRESS is clear, and the core skips it
	 * altogether if the domain verdict is not RECOVERED. On success the
	 * transaction carries on and resume() wakes.
	 */
	if (ret)
		wake_up_all(&vdev->pci_recovery_wait);

	return result;
}

static void vfio_pci_core_aer_resume(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);
	unsigned long irq_flags;
	u32 flags;
	int ret = 0;

	down_write(&vdev->recovery_lock);

	/*
	 * Pay any deferred ROM disable before the in-progress check below,
	 * which a failed transaction has already cleared, or it would be
	 * lost.
	 */
	vfio_pci_recovery_rom_disable(vdev);

	if (!(vdev->pci_recovery_flags & VFIO_PCI_RECOVERY_IN_PROGRESS))
		goto out_unlock;

	if (!vdev->pci_recovery_device_open) {
		vdev->pci_recovery_command_valid = false;
		WRITE_ONCE(vdev->pci_recovery_flags,
			   vdev->pci_recovery_flags &
			   ~VFIO_PCI_RECOVERY_IN_PROGRESS);
		goto out_unlock;
	}

	down_write(&vdev->memory_lock);
	/*
	 * Restore the command word and clear access_blocked under irqlock.
	 * The INTx handler writes the same register through
	 * pci_check_and_mask_intx(), so it must not interleave with the
	 * restore, and it must not see access blocked cleared while the
	 * temporary command value is still installed.
	 *
	 * INTX_DISABLE comes from the INTx state rather than from the saved
	 * word, which can be older than the last mask. The replay below is
	 * what unmasks the line.
	 */
	spin_lock_irqsave(&vdev->irqlock, irq_flags);
	if (!(vdev->pci_recovery_flags & VFIO_PCI_RECOVERY_RESET) &&
	    vdev->pci_recovery_command_valid) {
		u16 cmd = vdev->pci_recovery_command;

		cmd = vfio_pci_intx_recovery_command(vdev, cmd);
		ret = pci_write_config_word(pdev, PCI_COMMAND, cmd);
	}
	if (!ret)
		WRITE_ONCE(vdev->pci_recovery_access_blocked, false);
	spin_unlock_irqrestore(&vdev->irqlock, irq_flags);
	if (!ret && __vfio_pci_memory_enabled(vdev))
		vfio_pci_dma_buf_move(vdev, false);
	up_write(&vdev->memory_lock);

	vdev->pci_recovery_command_valid = false;
	flags = vdev->pci_recovery_flags & ~VFIO_PCI_RECOVERY_IN_PROGRESS;
	if (ret)
		flags |= VFIO_PCI_RECOVERY_FAILED;
	WRITE_ONCE(vdev->pci_recovery_flags, flags);
	if (!ret)
		vfio_pci_intx_recovery_finish(vdev);

out_unlock:
	up_write(&vdev->recovery_lock);
	wake_up_all(&vdev->pci_recovery_wait);
}

int vfio_pci_core_sriov_configure(struct vfio_pci_core_device *vdev,
				  int nr_virtfn)
{
	struct pci_dev *pdev = vdev->pdev;
	int ret = 0;

	device_lock_assert(&pdev->dev);

	if (nr_virtfn) {
		mutex_lock(&vfio_pci_sriov_pfs_mutex);
		/*
		 * The thread that adds the vdev to the list is the only thread
		 * that gets to call pci_enable_sriov() and we will only allow
		 * it to be called once without going through
		 * pci_disable_sriov()
		 */
		if (!list_empty(&vdev->sriov_pfs_item)) {
			ret = -EINVAL;
			goto out_unlock;
		}
		list_add_tail(&vdev->sriov_pfs_item, &vfio_pci_sriov_pfs);
		mutex_unlock(&vfio_pci_sriov_pfs_mutex);

		/*
		 * The PF power state should always be higher than the VF power
		 * state. The PF can be in low power state either with runtime
		 * power management (when there is no user) or PCI_PM_CTRL
		 * register write by the user. If PF is in the low power state,
		 * then change the power state to D0 first before enabling
		 * SR-IOV. Also, this function can be called at any time, and
		 * userspace PCI_PM_CTRL write can race against this code path,
		 * so protect the same with 'memory_lock'.
		 */
		ret = pm_runtime_resume_and_get(&pdev->dev);
		if (ret)
			goto out_del;

		down_write(&vdev->memory_lock);
		vfio_pci_set_power_state(vdev, PCI_D0);
		vdev->sriov_active = true;
		up_write(&vdev->memory_lock);
		ret = pci_enable_sriov(pdev, nr_virtfn);
		if (ret) {
			pm_runtime_put(&pdev->dev);
			goto out_del;
		}
		return nr_virtfn;
	}

	if (pci_num_vf(pdev)) {
		pci_disable_sriov(pdev);
		pm_runtime_put(&pdev->dev);
	}

out_del:
	/*
	 * Avoid taking the memory_lock intentionally. A race with a power
	 * state transition would at most result in an -EBUSY, leaving the
	 * device in PCI_D0.
	 */
	vdev->sriov_active = false;

	mutex_lock(&vfio_pci_sriov_pfs_mutex);
	list_del_init(&vdev->sriov_pfs_item);
out_unlock:
	mutex_unlock(&vfio_pci_sriov_pfs_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_sriov_configure);

const struct pci_error_handlers vfio_pci_core_err_handlers = {
	.error_detected = vfio_pci_core_aer_err_detected,
	.slot_reset = vfio_pci_core_aer_slot_reset,
	.resume = vfio_pci_core_aer_resume,
};
EXPORT_SYMBOL_GPL(vfio_pci_core_err_handlers);

static bool vfio_dev_in_groups(struct vfio_device *vdev,
			       struct vfio_pci_group_info *groups)
{
	unsigned int i;

	if (!groups)
		return false;

	for (i = 0; i < groups->count; i++)
		if (vfio_file_has_dev(groups->files[i], vdev))
			return true;
	return false;
}

static int vfio_pci_is_device_in_set(struct pci_dev *pdev, void *data)
{
	struct vfio_device_set *dev_set = data;

	return vfio_find_device_in_devset(dev_set, &pdev->dev) ? 0 : -ENODEV;
}

/*
 * vfio-core considers a group to be viable and will create a vfio_device even
 * if some devices are bound to drivers like pci-stub or pcieport. Here we
 * require all PCI devices to be inside our dev_set since that ensures they stay
 * put and that every driver controlling the device can co-ordinate with the
 * device reset.
 *
 * Returns the pci_dev to pass to pci_reset_bus() if every PCI device to be
 * reset is inside the dev_set, and pci_reset_bus() can succeed. NULL otherwise.
 */
static struct pci_dev *
vfio_pci_dev_set_resettable(struct vfio_device_set *dev_set)
{
	struct pci_dev *pdev;

	lockdep_assert_held(&dev_set->lock);

	/*
	 * By definition all PCI devices in the dev_set share the same PCI
	 * reset, so any pci_dev will have the same outcomes for
	 * pci_probe_reset_*() and pci_reset_bus().
	 */
	pdev = list_first_entry(&dev_set->device_list,
				struct vfio_pci_core_device,
				vdev.dev_set_list)->pdev;

	/* pci_reset_bus() is supported */
	if (pci_probe_reset_slot(pdev->slot) && pci_probe_reset_bus(pdev->bus))
		return NULL;

	if (vfio_pci_for_each_slot_or_bus(pdev, vfio_pci_is_device_in_set,
					  dev_set,
					  !pci_probe_reset_slot(pdev->slot)))
		return NULL;
	return pdev;
}

static int vfio_pci_dev_set_pm_runtime_get(struct vfio_device_set *dev_set)
{
	struct vfio_pci_core_device *cur;
	int ret;

	list_for_each_entry(cur, &dev_set->device_list, vdev.dev_set_list) {
		ret = pm_runtime_resume_and_get(&cur->pdev->dev);
		if (ret)
			goto unwind;
	}

	return 0;

unwind:
	list_for_each_entry_continue_reverse(cur, &dev_set->device_list,
					     vdev.dev_set_list)
		pm_runtime_put(&cur->pdev->dev);

	return ret;
}

static int vfio_pci_dev_set_hot_reset(struct vfio_device_set *dev_set,
				      struct vfio_pci_group_info *groups,
				      struct iommufd_ctx *iommufd_ctx)
{
	struct vfio_pci_core_device *vdev;
	struct pci_dev *pdev;
	int ret;

	mutex_lock(&dev_set->lock);

	pdev = vfio_pci_dev_set_resettable(dev_set);
	if (!pdev) {
		ret = -EINVAL;
		goto err_unlock;
	}

	/*
	 * Some of the devices in the dev_set can be in the runtime suspended
	 * state. Increment the usage count for all the devices in the dev_set
	 * before reset and decrement the same after reset.
	 */
	ret = vfio_pci_dev_set_pm_runtime_get(dev_set);
	if (ret)
		goto err_unlock;

	list_for_each_entry(vdev, &dev_set->device_list, vdev.dev_set_list) {
		bool owned;

		/*
		 * Test whether all the affected devices can be reset by the
		 * user.
		 *
		 * If called from a group opened device and the user provides
		 * a set of groups, all the devices in the dev_set should be
		 * contained by the set of groups provided by the user.
		 *
		 * If called from a cdev opened device and the user provides
		 * a zero-length array, all the devices in the dev_set must
		 * be bound to the same iommufd_ctx as the input iommufd_ctx.
		 * If there is any device that has not been bound to any
		 * iommufd_ctx yet, check if its iommu_group has any device
		 * bound to the input iommufd_ctx.  Such devices can be
		 * considered owned by the input iommufd_ctx as the device
		 * cannot be owned by another iommufd_ctx when its iommu_group
		 * is owned.
		 *
		 * Otherwise, reset is not allowed.
		 */
		if (iommufd_ctx) {
			int devid = vfio_iommufd_get_dev_id(&vdev->vdev,
							    iommufd_ctx);

			owned = (devid > 0 || devid == -ENOENT);
		} else {
			owned = vfio_dev_in_groups(&vdev->vdev, groups);
		}

		if (!owned) {
			ret = -EINVAL;
			break;
		}

		/*
		 * Between the AER callbacks memory_lock is not held, so refuse
		 * the reset on access_blocked as well. Read it rather than
		 * take recovery_lock, which would have to be released before
		 * pci_reset_bus() anyway since that reaches pci_bus_sem.
		 *
		 * Let a device which has failed for good through. Nothing is
		 * running on its behalf, and this reset covers the whole set,
		 * so refusing there would stop a healthy sibling from being
		 * reset because an unrelated device errored. The failed one
		 * stays blocked, since the reset does not unblock a device
		 * with FAILED set.
		 */
		if (vdev->pci_recovery_supported &&
		    READ_ONCE(vdev->pci_recovery_access_blocked) &&
		    !(READ_ONCE(vdev->pci_recovery_flags) &
		      VFIO_PCI_RECOVERY_FAILED)) {
			ret = -EBUSY;
			break;
		}

		/*
		 * Take the memory write lock for each device and zap BAR
		 * mappings to prevent the user accessing the device while in
		 * reset.  Locking multiple devices is prone to deadlock,
		 * runaway and unwind if we hit contention.
		 */
		if (!down_write_trylock(&vdev->memory_lock)) {
			ret = -EBUSY;
			break;
		}

		vfio_pci_dma_buf_move(vdev, true);
		vfio_pci_zap_bars(vdev);
	}

	if (!list_entry_is_head(vdev,
				&dev_set->device_list, vdev.dev_set_list)) {
		vdev = list_prev_entry(vdev, vdev.dev_set_list);
		goto err_undo;
	}

	/*
	 * The pci_reset_bus() will reset all the devices in the bus.
	 * The power state can be non-D0 for some of the devices in the bus.
	 * For these devices, the pci_reset_bus() will internally set
	 * the power state to D0 without vfio driver involvement.
	 * For the devices which have NoSoftRst-, the reset function can
	 * cause the PCI config space reset without restoring the original
	 * state (saved locally in 'vdev->pm_save').
	 */
	list_for_each_entry(vdev, &dev_set->device_list, vdev.dev_set_list)
		vfio_pci_set_power_state(vdev, PCI_D0);

	ret = pci_reset_bus(pdev);

	vdev = list_last_entry(&dev_set->device_list,
			       struct vfio_pci_core_device, vdev.dev_set_list);

err_undo:
	list_for_each_entry_from_reverse(vdev, &dev_set->device_list,
					 vdev.dev_set_list) {
		if (vdev->vdev.open_count && __vfio_pci_memory_enabled(vdev))
			vfio_pci_dma_buf_move(vdev, false);
		up_write(&vdev->memory_lock);
	}

	list_for_each_entry(vdev, &dev_set->device_list, vdev.dev_set_list)
		pm_runtime_put(&vdev->pdev->dev);

err_unlock:
	mutex_unlock(&dev_set->lock);
	return ret;
}

static bool vfio_pci_dev_set_needs_reset(struct vfio_device_set *dev_set)
{
	struct vfio_pci_core_device *cur;
	bool needs_reset = false;

	/* No other VFIO device in the set can be open. */
	if (vfio_device_set_open_count(dev_set) > 1)
		return false;

	list_for_each_entry(cur, &dev_set->device_list, vdev.dev_set_list)
		needs_reset |= cur->needs_reset;
	return needs_reset;
}

/*
 * If a bus or slot reset is available for the provided dev_set and:
 *  - All of the devices affected by that bus or slot reset are unused
 *  - At least one of the affected devices is marked dirty via
 *    needs_reset (such as by lack of FLR support)
 * Then attempt to perform that bus or slot reset.
 */
static void vfio_pci_dev_set_try_reset(struct vfio_device_set *dev_set)
{
	struct vfio_pci_core_device *cur;
	struct pci_dev *pdev;
	bool reset_done = false;

	if (!vfio_pci_dev_set_needs_reset(dev_set))
		return;

	pdev = vfio_pci_dev_set_resettable(dev_set);
	if (!pdev)
		return;

	/*
	 * Some of the devices in the bus can be in the runtime suspended
	 * state. Increment the usage count for all the devices in the dev_set
	 * before reset and decrement the same after reset.
	 */
	if (vfio_pci_dev_set_pm_runtime_get(dev_set))
		return;

	if (!pci_reset_bus(pdev))
		reset_done = true;

	list_for_each_entry(cur, &dev_set->device_list, vdev.dev_set_list) {
		if (reset_done)
			cur->needs_reset = false;

		pm_runtime_put(&cur->pdev->dev);
	}
}

static void vfio_pci_core_cleanup(void)
{
	vfio_pci_uninit_perm_bits();
}

static int __init vfio_pci_core_init(void)
{
	/* Allocate shared config space permission data used by all devices */
	return vfio_pci_init_perm_bits();
}

module_init(vfio_pci_core_init);
module_exit(vfio_pci_core_cleanup);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
