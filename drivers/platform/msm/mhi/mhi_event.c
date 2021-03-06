/* Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */


#include <linux/types.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/dma-mapping.h>

#include "mhi.h"
#include "mhi_macros.h"
#include "mhi_sys.h"

int mhi_populate_event_cfg(struct mhi_device_ctxt *mhi_dev_ctxt)
{
	int r, i;
	char dt_prop[MAX_BUF_SIZE];
	const struct device_node *np =
		mhi_dev_ctxt->dev_info->plat_dev->dev.of_node;

	r = of_property_read_u32(np, "mhi-event-rings",
			&mhi_dev_ctxt->mmio_info.nr_event_rings);
	if (r) {
		mhi_log(MHI_MSG_CRITICAL,
			"Failed to pull event ring info from DT, %d\n", r);
		goto dt_error;
	}
	mhi_dev_ctxt->ev_ring_props =
				kzalloc(sizeof(struct mhi_event_ring_cfg) *
					mhi_dev_ctxt->mmio_info.nr_event_rings,
					GFP_KERNEL);
	if (!mhi_dev_ctxt->ev_ring_props) {
		r = -ENOMEM;
		goto dt_error;
	}

	for (i = 0; i < mhi_dev_ctxt->mmio_info.nr_event_rings; ++i) {
		scnprintf(dt_prop, MAX_BUF_SIZE, "%s%d", "mhi-event-cfg-", i);
		r = of_property_read_u32_array(np, dt_prop,
					(u32 *)&mhi_dev_ctxt->ev_ring_props[i],
					4);
		if (r) {
			mhi_log(MHI_MSG_CRITICAL,
				"Failed to pull ev ring %d info from DT %d\n",
				i, r);
			goto dt_error;
		}
		mhi_log(MHI_MSG_INFO,
			"Pulled ev ring %d, desc: 0x%x, msi_vec: 0x%x, intmod %d flags 0x%x\n",
			i, mhi_dev_ctxt->ev_ring_props[i].nr_desc,
			   mhi_dev_ctxt->ev_ring_props[i].msi_vec,
			   mhi_dev_ctxt->ev_ring_props[i].intmod,
			   mhi_dev_ctxt->ev_ring_props[i].flags);
		if (GET_EV_PROPS(EV_MANAGED,
			mhi_dev_ctxt->ev_ring_props[i].flags))
			mhi_dev_ctxt->ev_ring_props[i].mhi_handler_ptr =
							mhi_msi_handlr;
		else
			mhi_dev_ctxt->ev_ring_props[i].mhi_handler_ptr =
							mhi_msi_ipa_handlr;
	}
dt_error:
	return r;
}

int create_ev_rings(struct mhi_device_ctxt *mhi_dev_ctxt)
{
	int r = 0, i;
	struct mhi_event_ctxt *ev_ctxt = NULL;

	size_t ctxt_size = sizeof(struct mhi_event_ctxt) *
				    mhi_dev_ctxt->mmio_info.nr_event_rings;
	/* Allocate the event contexts in uncached memory */
	mhi_dev_ctxt->mhi_ctrl_seg->mhi_ec_list =
				dma_alloc_coherent(
				&mhi_dev_ctxt->dev_info->plat_dev->dev,
				ctxt_size,
				&mhi_dev_ctxt->mmio_info.dma_ev_ctxt,
				GFP_KERNEL);
	if (!mhi_dev_ctxt->mhi_ctrl_seg->mhi_ec_list)
		return -ENOMEM;

	mhi_dev_ctxt->mhi_local_event_ctxt = kzalloc(sizeof(struct mhi_ring) *
				     mhi_dev_ctxt->mmio_info.nr_event_rings,
				     GFP_KERNEL);
	if (!mhi_dev_ctxt->mhi_local_event_ctxt) {
		r = -ENOMEM;
		goto free_ec_list;
	}
	mhi_dev_ctxt->counters.ev_counter = kzalloc(sizeof(u32) *
				     mhi_dev_ctxt->mmio_info.nr_event_rings,
				     GFP_KERNEL);
	if (!mhi_dev_ctxt->counters.ev_counter) {
		r = -ENOMEM;
		goto free_local_ec_list;
	}
	mhi_dev_ctxt->counters.msi_counter = kzalloc(sizeof(u32) *
				     mhi_dev_ctxt->mmio_info.nr_event_rings,
				     GFP_KERNEL);
	if (!mhi_dev_ctxt->counters.msi_counter) {
		r = -ENOMEM;
		goto free_ev_counter;
	}

	mhi_dev_ctxt->mmio_info.dma_ev_rings = kzalloc(sizeof(void *) *
				     mhi_dev_ctxt->mmio_info.nr_event_rings,
				     GFP_KERNEL);
	if (!mhi_dev_ctxt->mmio_info.dma_ev_rings) {
		r = -ENOMEM;
		goto free_msi_counter;
	}
	mhi_log(MHI_MSG_INFO, "Allocated ECABAP at Virt: 0x%pK, Phys 0x%lx\n",
			mhi_dev_ctxt->mhi_ctrl_seg->mhi_ec_list,
			(uintptr_t)mhi_dev_ctxt->mmio_info.dma_ev_ctxt);

	/* Allocate event ring elements for each ring */
	for (i = 0; i < mhi_dev_ctxt->mmio_info.nr_event_rings; ++i) {
		dma_addr_t ring_base_addr;
		ev_ctxt = &mhi_dev_ctxt->mhi_ctrl_seg->mhi_ec_list[i];
		mhi_dev_ctxt->mhi_local_event_ctxt[i].base =
			dma_alloc_coherent(
				&mhi_dev_ctxt->dev_info->plat_dev->dev,
				sizeof(union mhi_event_pkt) *
				mhi_dev_ctxt->ev_ring_props[i].nr_desc,
				&ring_base_addr,
				GFP_KERNEL);
		if (!mhi_dev_ctxt->mhi_local_event_ctxt[i].base) {
			r = -ENOMEM;
			goto free_event_ring;
		}

		ev_ctxt->mhi_event_ring_base_addr = ring_base_addr;
		ev_ctxt->mhi_event_read_ptr = ring_base_addr;
		ev_ctxt->mhi_event_write_ptr = ring_base_addr;

		mhi_dev_ctxt->mhi_local_event_ctxt[i].wp =
				mhi_dev_ctxt->mhi_local_event_ctxt[i].base;
		mhi_dev_ctxt->mhi_local_event_ctxt[i].rp =
				mhi_dev_ctxt->mhi_local_event_ctxt[i].base;
		mhi_log(MHI_MSG_INFO, "Allocated Event Ring %d\n", i);
	}
	return r;

free_event_ring:
	for (; i > 0; --i) {
		ev_ctxt = &mhi_dev_ctxt->mhi_ctrl_seg->mhi_ec_list[i];
		dma_free_coherent(&mhi_dev_ctxt->dev_info->plat_dev->dev,
				  sizeof(union mhi_event_pkt *) *
				  mhi_dev_ctxt->ev_ring_props[i].nr_desc,
				  mhi_dev_ctxt->mhi_local_event_ctxt[i].base,
				  ev_ctxt->mhi_event_ring_base_addr);
	}
	kfree(mhi_dev_ctxt->mmio_info.dma_ev_rings);
free_msi_counter:
	kfree(mhi_dev_ctxt->counters.msi_counter);
free_ev_counter:
	kfree(mhi_dev_ctxt->counters.ev_counter);
free_local_ec_list:
	kfree(mhi_dev_ctxt->mhi_local_event_ctxt);
free_ec_list:
	dma_free_coherent(&mhi_dev_ctxt->dev_info->plat_dev->dev,
				ctxt_size,
				mhi_dev_ctxt->mhi_ctrl_seg->mhi_ec_list,
				mhi_dev_ctxt->mmio_info.dma_ev_ctxt);
	return r;
}
void ring_ev_db(struct mhi_device_ctxt *mhi_dev_ctxt, u32 event_ring_index)
{
	struct mhi_ring *event_ctxt = NULL;
	u64 db_value = 0;
	event_ctxt =
		&mhi_dev_ctxt->mhi_local_event_ctxt[event_ring_index];
	db_value = virt_to_dma(NULL, event_ctxt->wp);
	mhi_process_db(mhi_dev_ctxt, mhi_dev_ctxt->mmio_info.event_db_addr,
					event_ring_index, db_value);
}

static enum MHI_STATUS mhi_event_ring_init(struct mhi_event_ctxt *ev_list,
				struct mhi_ring *ring, u32 el_per_ring,
				u32 intmodt_val, u32 msi_vec)
{
	ev_list->mhi_event_er_type  = MHI_EVENT_RING_TYPE_VALID;
	ev_list->mhi_msi_vector     = msi_vec;
	ev_list->mhi_event_ring_len = el_per_ring*sizeof(union mhi_event_pkt);
	MHI_SET_EV_CTXT(EVENT_CTXT_INTMODT, ev_list, intmodt_val);
	ring->len = ((size_t)(el_per_ring)*sizeof(union mhi_event_pkt));
	ring->el_size = sizeof(union mhi_event_pkt);
	ring->overwrite_en = 0;
	/* Flush writes to MMIO */
	wmb();
	return MHI_STATUS_SUCCESS;
}

int init_event_ctxt_array(struct mhi_device_ctxt *mhi_dev_ctxt)
{
	int i;
	struct mhi_ring *mhi_local_event_ctxt = NULL;
	struct mhi_event_ctxt *event_ctxt;
	struct mhi_control_seg *mhi_ctrl = mhi_dev_ctxt->mhi_ctrl_seg;

	for (i = 0; i < mhi_dev_ctxt->mmio_info.nr_event_rings; ++i) {
		event_ctxt = &mhi_ctrl->mhi_ec_list[i];
		mhi_local_event_ctxt = &mhi_dev_ctxt->mhi_local_event_ctxt[i];
		mhi_event_ring_init(event_ctxt, mhi_local_event_ctxt,
			mhi_dev_ctxt->ev_ring_props[i].nr_desc,
			mhi_dev_ctxt->ev_ring_props[i].intmod,
			mhi_dev_ctxt->ev_ring_props[i].msi_vec);
	}
	return 0;
}

int init_local_ev_ring_by_type(struct mhi_device_ctxt *mhi_dev_ctxt,
		  enum MHI_TYPE_EVENT_RING type)
{
	enum MHI_STATUS ret_val = MHI_STATUS_SUCCESS;
	u32 i;

	mhi_log(MHI_MSG_INFO, "Entered\n");
	for (i = 0; i < mhi_dev_ctxt->mmio_info.nr_event_rings; i++) {
		if (GET_EV_PROPS(EV_TYPE,
			mhi_dev_ctxt->ev_ring_props[i].flags) == type &&
		    !mhi_dev_ctxt->ev_ring_props[i].state) {
			ret_val = mhi_init_local_event_ring(mhi_dev_ctxt,
					mhi_dev_ctxt->ev_ring_props[i].nr_desc,
					i);
			if (ret_val)
				return ret_val;
		}
		ring_ev_db(mhi_dev_ctxt, i);
		mhi_log(MHI_MSG_INFO, "Finished ev ring init %d\n", i);
	}
	mhi_log(MHI_MSG_INFO, "Exited\n");
	return 0;
}

enum MHI_STATUS mhi_add_elements_to_event_rings(
					struct mhi_device_ctxt *mhi_dev_ctxt,
					enum STATE_TRANSITION new_state)
{
	enum MHI_STATUS ret_val = MHI_STATUS_SUCCESS;

	switch (new_state) {
	case STATE_TRANSITION_READY:
		ret_val = init_local_ev_ring_by_type(mhi_dev_ctxt,
							MHI_ER_CTRL_TYPE);
		break;
	case STATE_TRANSITION_AMSS:
		ret_val = init_local_ev_ring_by_type(mhi_dev_ctxt,
							MHI_ER_DATA_TYPE);
		break;
	default:
		mhi_log(MHI_MSG_ERROR,
			"Unrecognized event stage, %d\n", new_state);
		ret_val = MHI_STATUS_ERROR;
		break;
	}
	return ret_val;
}

enum MHI_STATUS mhi_init_local_event_ring(struct mhi_device_ctxt *mhi_dev_ctxt,
					u32 nr_ev_el, u32 ring_index)
{
	union mhi_event_pkt *ev_pkt = NULL;
	u32 i = 0;
	unsigned long flags = 0;
	enum MHI_STATUS ret_val = MHI_STATUS_SUCCESS;
	spinlock_t *lock =
		&mhi_dev_ctxt->mhi_ev_spinlock_list[ring_index];
	struct mhi_ring *event_ctxt =
		&mhi_dev_ctxt->mhi_local_event_ctxt[ring_index];

	if (NULL == mhi_dev_ctxt || 0 == nr_ev_el) {
		mhi_log(MHI_MSG_ERROR, "Bad Input data, quitting\n");
		return MHI_STATUS_ERROR;
	}

	spin_lock_irqsave(lock, flags);

	mhi_log(MHI_MSG_INFO, "mmio_addr = 0x%pK, mmio_len = 0x%llx\n",
			mhi_dev_ctxt->mmio_info.mmio_addr,
			mhi_dev_ctxt->mmio_info.mmio_len);
	mhi_log(MHI_MSG_INFO, "Initializing event ring %d\n", ring_index);

	for (i = 0; i < nr_ev_el - 1; ++i) {
		ret_val = ctxt_add_element(event_ctxt, (void *)&ev_pkt);
		if (MHI_STATUS_SUCCESS != ret_val) {
			mhi_log(MHI_MSG_ERROR,
				"Failed to insert el in ev ctxt\n");
			ret_val = MHI_STATUS_ERROR;
			break;
		}
	}
	mhi_dev_ctxt->ev_ring_props[ring_index].state = MHI_EVENT_RING_INIT;
	spin_unlock_irqrestore(lock, flags);
	return ret_val;
}

void mhi_reset_ev_ctxt(struct mhi_device_ctxt *mhi_dev_ctxt,
				int index)
{
	struct mhi_event_ctxt *ev_ctxt;
	struct mhi_ring *local_ev_ctxt;
	mhi_log(MHI_MSG_VERBOSE, "Resetting event index %d\n", index);
	ev_ctxt =
	    &mhi_dev_ctxt->mhi_ctrl_seg->mhi_ec_list[index];
	local_ev_ctxt =
	    &mhi_dev_ctxt->mhi_local_event_ctxt[index];
	ev_ctxt->mhi_event_read_ptr = ev_ctxt->mhi_event_ring_base_addr;
	ev_ctxt->mhi_event_write_ptr = ev_ctxt->mhi_event_ring_base_addr;
	local_ev_ctxt->rp = local_ev_ctxt->base;
	local_ev_ctxt->wp = local_ev_ctxt->base;
	/* Flush writes to MMIO */
	wmb();
}

