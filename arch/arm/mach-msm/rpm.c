/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/bug.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <asm/hardware/gic.h>
#include <mach/msm_iomap.h>
#include <linux/slab.h>

#include "rpm.h"

/******************************************************************************
 * Data type and structure definitions
 *****************************************************************************/

enum {
	MSM_RPM_CTRL_VERSION_MAJOR,
	MSM_RPM_CTRL_VERSION_MINOR,
	MSM_RPM_CTRL_VERSION_BUILD,

	MSM_RPM_CTRL_REQ_CTX_0,
	MSM_RPM_CTRL_REQ_CTX_7 = MSM_RPM_CTRL_REQ_CTX_0 + 7,
	MSM_RPM_CTRL_REQ_SEL_0,
	MSM_RPM_CTRL_REQ_SEL_7 = MSM_RPM_CTRL_REQ_SEL_0 + 7,
	MSM_RPM_CTRL_ACK_CTX_0,
	MSM_RPM_CTRL_ACK_CTX_7 = MSM_RPM_CTRL_ACK_CTX_0 + 7,
	MSM_RPM_CTRL_ACK_SEL_0,
	MSM_RPM_CTRL_ACK_SEL_7 = MSM_RPM_CTRL_ACK_SEL_0 + 7,
};

struct msm_rpm_request {
	struct msm_rpm_iv_pair *req;
	int count;
	uint32_t *ctx_mask_ack;
	uint32_t *sel_masks_ack;
	struct completion *done;
};

struct msm_rpm_map_data {
	uint32_t id;
	uint32_t sel;
	uint32_t count;
};

#define MSM_RPM_MAP(i, s, c) { \
	.id = MSM_RPM_ID_##i, .sel = MSM_RPM_SEL_##s, .count = c }

struct msm_rpm_notif_config {
	struct msm_rpm_iv_pair iv[MSM_RPM_SEL_MASK_SIZE * 2];
};

#define configured_iv(notif_cfg) ((notif_cfg)->iv)
#define registered_iv(notif_cfg) ((notif_cfg)->iv + MSM_RPM_SEL_MASK_SIZE)

#define APPS_IPC (MSM_GCC_BASE + 0x008)
#define APPS_IPC_RPM 4

/******************************************************************************
 * Internal variables
 *****************************************************************************/

static struct msm_rpm_map_data msm_rpm_raw_map[] __initdata = {
	MSM_RPM_MAP(TRIGGER_TIMED_TO, TRIGGER_TIMED, 1),
	MSM_RPM_MAP(TRIGGER_TIMED_SCLK_COUNT, TRIGGER_TIMED, 1),
	MSM_RPM_MAP(TRIGGER_SET_FROM, TRIGGER_SET, 1),
	MSM_RPM_MAP(TRIGGER_SET_TO, TRIGGER_SET, 1),
	MSM_RPM_MAP(TRIGGER_SET_TRIGGER, TRIGGER_SET, 1),
	MSM_RPM_MAP(TRIGGER_CLEAR_FROM, TRIGGER_CLEAR, 1),
	MSM_RPM_MAP(TRIGGER_CLEAR_TO, TRIGGER_CLEAR, 1),
	MSM_RPM_MAP(TRIGGER_CLEAR_TRIGGER, TRIGGER_CLEAR, 1),

	MSM_RPM_MAP(CXO_CLK, CXO_CLK, 1),
	MSM_RPM_MAP(PXO_CLK, PXO_CLK, 1),
	MSM_RPM_MAP(PLL_4, PLL_4, 1),
	MSM_RPM_MAP(APPS_FABRIC_CLK, APPS_FABRIC_CLK, 1),
	MSM_RPM_MAP(SYSTEM_FABRIC_CLK, SYSTEM_FABRIC_CLK, 1),
	MSM_RPM_MAP(MM_FABRIC_CLK, MM_FABRIC_CLK, 1),
	MSM_RPM_MAP(DAYTONA_FABRIC_CLK, DAYTONA_FABRIC_CLK, 1),
	MSM_RPM_MAP(SFPB_CLK, SFPB_CLK, 1),
	MSM_RPM_MAP(CFPB_CLK, CFPB_CLK, 1),
	MSM_RPM_MAP(MMFPB_CLK, MMFPB_CLK, 1),
	MSM_RPM_MAP(SMI_CLK, SMI_CLK, 1),
	MSM_RPM_MAP(EBI1_CLK, EBI1_CLK, 1),

	MSM_RPM_MAP(APPS_L2_CACHE_CTL, APPS_L2_CACHE_CTL, 1),

	MSM_RPM_MAP(APPS_FABRIC_HALT_0, APPS_FABRIC_HALT, 2),
	MSM_RPM_MAP(APPS_FABRIC_CLOCK_MODE_0, APPS_FABRIC_CLOCK_MODE, 3),
	MSM_RPM_MAP(APPS_FABRIC_ARB_0, APPS_FABRIC_ARB, 6),

	MSM_RPM_MAP(SYSTEM_FABRIC_HALT_0, SYSTEM_FABRIC_HALT, 2),
	MSM_RPM_MAP(SYSTEM_FABRIC_CLOCK_MODE_0, SYSTEM_FABRIC_CLOCK_MODE, 3),
	MSM_RPM_MAP(SYSTEM_FABRIC_ARB_0, SYSTEM_FABRIC_ARB, 22),

	MSM_RPM_MAP(MM_FABRIC_HALT_0, MM_FABRIC_HALT, 2),
	MSM_RPM_MAP(MM_FABRIC_CLOCK_MODE_0, MM_FABRIC_CLOCK_MODE, 3),
	MSM_RPM_MAP(MM_FABRIC_ARB_0, MM_FABRIC_ARB, 23),

	MSM_RPM_MAP(SMPS0B_0, SMPS0B, 2),
	MSM_RPM_MAP(SMPS1B_0, SMPS1B, 2),
	MSM_RPM_MAP(SMPS2B_0, SMPS2B, 2),
	MSM_RPM_MAP(SMPS3B_0, SMPS3B, 2),
	MSM_RPM_MAP(SMPS4B_0, SMPS4B, 2),
	MSM_RPM_MAP(LDO0B_0, LDO0B, 2),
	MSM_RPM_MAP(LDO1B_0, LDO1B, 2),
	MSM_RPM_MAP(LDO2B_0, LDO2B, 2),
	MSM_RPM_MAP(LDO3B_0, LDO3B, 2),
	MSM_RPM_MAP(LDO4B_0, LDO4B, 2),
	MSM_RPM_MAP(LDO5B_0, LDO5B, 2),
	MSM_RPM_MAP(LDO6B_0, LDO6B, 2),
	MSM_RPM_MAP(LVS0B, LVS0B, 1),
	MSM_RPM_MAP(LVS1B, LVS1B, 1),
	MSM_RPM_MAP(LVS2B, LVS2B, 1),
	MSM_RPM_MAP(LVS3B, LVS3B, 1),
	MSM_RPM_MAP(MVS, MVS, 1),

	MSM_RPM_MAP(SMPS0_0, SMPS0, 2),
	MSM_RPM_MAP(SMPS1_0, SMPS1, 2),
	MSM_RPM_MAP(SMPS2_0, SMPS2, 2),
	MSM_RPM_MAP(SMPS3_0, SMPS3, 2),
	MSM_RPM_MAP(SMPS4_0, SMPS4, 2),
	MSM_RPM_MAP(LDO0_0, LDO0, 2),
	MSM_RPM_MAP(LDO1_0, LDO1, 2),
	MSM_RPM_MAP(LDO2_0, LDO2, 2),
	MSM_RPM_MAP(LDO3_0, LDO3, 2),
	MSM_RPM_MAP(LDO4_0, LDO4, 2),
	MSM_RPM_MAP(LDO5_0, LDO5, 2),
	MSM_RPM_MAP(LDO6_0, LDO6, 2),
	MSM_RPM_MAP(LDO7_0, LDO7, 2),
	MSM_RPM_MAP(LDO8_0, LDO8, 2),
	MSM_RPM_MAP(LDO9_0, LDO9, 2),
	MSM_RPM_MAP(LDO10_0, LDO10, 2),
	MSM_RPM_MAP(LDO11_0, LDO11, 2),
	MSM_RPM_MAP(LDO12_0, LDO12, 2),
	MSM_RPM_MAP(LDO13_0, LDO13, 2),
	MSM_RPM_MAP(LDO14_0, LDO14, 2),
	MSM_RPM_MAP(LDO15_0, LDO15, 2),
	MSM_RPM_MAP(LDO16_0, LDO16, 2),
	MSM_RPM_MAP(LDO17_0, LDO17, 2),
	MSM_RPM_MAP(LDO18_0, LDO18, 2),
	MSM_RPM_MAP(LDO19_0, LDO19, 2),
	MSM_RPM_MAP(LDO20_0, LDO20, 2),
	MSM_RPM_MAP(LDO21_0, LDO21, 2),
	MSM_RPM_MAP(LDO22_0, LDO22, 2),
	MSM_RPM_MAP(LDO23_0, LDO23, 2),
	MSM_RPM_MAP(LDO24_0, LDO24, 2),
	MSM_RPM_MAP(LDO25_0, LDO25, 2),
	MSM_RPM_MAP(LVS0, LVS0, 1),
	MSM_RPM_MAP(LVS1, LVS1, 1),
	MSM_RPM_MAP(NCP_0, NCP, 2),

	MSM_RPM_MAP(CXO_BUFFERS, CXO_BUFFERS, 1),
};

static struct msm_rpm_platform_data *msm_rpm_platform;
static uint32_t msm_rpm_map[MSM_RPM_ID_LAST + 1];
static volatile stats_blob * msm_rpm_stat_data;

static DEFINE_MUTEX(msm_rpm_mutex);
static DEFINE_SPINLOCK(msm_rpm_lock);
static DEFINE_SPINLOCK(msm_rpm_irq_lock);

static struct msm_rpm_request *msm_rpm_request;
static struct msm_rpm_request msm_rpm_request_irq_mode;
static struct msm_rpm_request msm_rpm_request_poll_mode;

static LIST_HEAD(msm_rpm_notifications);
static struct msm_rpm_notif_config msm_rpm_notif_cfgs[MSM_RPM_CTX_SET_COUNT];
static bool msm_rpm_init_notif_done;

/******************************************************************************
 * Internal functions
 *****************************************************************************/

static inline uint32_t msm_rpm_read(unsigned int page, unsigned int reg)
{
	return readl(msm_rpm_platform->reg_base_addrs[page] + reg * 4);
}

static inline void msm_rpm_write(
	unsigned int page, unsigned int reg, uint32_t value)
{
	writel(value, msm_rpm_platform->reg_base_addrs[page] + reg * 4);
}

static inline void msm_rpm_read_contiguous(
	unsigned int page, unsigned int reg, uint32_t *values, int count)
{
	int i;

	for (i = 0; i < count; i++)
		values[i] = msm_rpm_read(page, reg + i);
}

static inline void msm_rpm_write_contiguous(
	unsigned int page, unsigned int reg, uint32_t *values, int count)
{
	int i;

	for (i = 0; i < count; i++)
		msm_rpm_write(page, reg + i, values[i]);
}

static inline void msm_rpm_write_contiguous_zeros(
	unsigned int page, unsigned int reg, int count)
{
	int i;

	for (i = 0; i < count; i++)
		msm_rpm_write(page, reg + i, 0);
}

static inline uint32_t msm_rpm_map_id_to_sel(uint32_t id)
{
	return (id > MSM_RPM_ID_LAST) ? MSM_RPM_SEL_LAST + 1 : msm_rpm_map[id];
}

/*
 * Note: the function does not clear the masks before filling them.
 *
 * Return value:
 *   0: success
 *   -EINVAL: invalid id in <req> array
 */
static int msm_rpm_fill_sel_masks(
	uint32_t *sel_masks, struct msm_rpm_iv_pair *req, int count)
{
	uint32_t sel;
	int i;

	for (i = 0; i < count; i++) {
		sel = msm_rpm_map_id_to_sel(req[i].id);

		if (sel > MSM_RPM_SEL_LAST)
			return -EINVAL;

		sel_masks[msm_rpm_get_sel_mask_reg(sel)] |=
			msm_rpm_get_sel_mask(sel);
	}

	return 0;
}

static void __init msm_rpm_populate_map(void)
{
	int i, k;

	for (i = 0; i < ARRAY_SIZE(msm_rpm_map); i++)
		msm_rpm_map[i] = MSM_RPM_SEL_LAST + 1;

	for (i = 0; i < ARRAY_SIZE(msm_rpm_raw_map); i++) {
		struct msm_rpm_map_data *raw_data = &msm_rpm_raw_map[i];

		for (k = 0; k < raw_data->count; k++)
			msm_rpm_map[raw_data->id + k] = raw_data->sel;
	}
}


static inline void msm_rpm_send_req_interrupt(void)
{
	writel(APPS_IPC_RPM, APPS_IPC);
}

/*
 * Note: assumes caller has acquired <msm_rpm_irq_lock>.
 *
 * Return value:
 *   0: request acknowledgement
 *   1: notification
 *   2: spurious interrupt
 */
static int msm_rpm_process_ack_interrupt(void)
{
	uint32_t ctx_mask_ack;
	uint32_t sel_masks_ack[MSM_RPM_SEL_MASK_SIZE];

	ctx_mask_ack = msm_rpm_read(MSM_RPM_PAGE_CTRL, MSM_RPM_CTRL_ACK_CTX_0);
	msm_rpm_read_contiguous(MSM_RPM_PAGE_CTRL,
		MSM_RPM_CTRL_ACK_SEL_0, sel_masks_ack, MSM_RPM_SEL_MASK_SIZE);

	if (ctx_mask_ack & msm_rpm_get_ctx_mask(MSM_RPM_CTX_NOTIFICATION)) {
		struct msm_rpm_notification *n;
		int i;

		list_for_each_entry(n, &msm_rpm_notifications, list)
			for (i = 0; i < MSM_RPM_SEL_MASK_SIZE; i++)
				if (sel_masks_ack[i] & n->sel_masks[i]) {
					up(&n->sem);
					break;
				}

		msm_rpm_write_contiguous_zeros(MSM_RPM_PAGE_CTRL,
			MSM_RPM_CTRL_ACK_SEL_0, MSM_RPM_SEL_MASK_SIZE);
		msm_rpm_write(MSM_RPM_PAGE_CTRL, MSM_RPM_CTRL_ACK_CTX_0, 0);
		/* Ensure write is complete before return */
		dsb();

		return 1;
	}

	if (msm_rpm_request) {
		int i;

		*(msm_rpm_request->ctx_mask_ack) = ctx_mask_ack;
		memcpy(msm_rpm_request->sel_masks_ack, sel_masks_ack,
			sizeof(sel_masks_ack));

		for (i = 0; i < msm_rpm_request->count; i++)
			msm_rpm_request->req[i].value =
				msm_rpm_read(MSM_RPM_PAGE_ACK,
						msm_rpm_request->req[i].id);

		msm_rpm_write_contiguous_zeros(MSM_RPM_PAGE_CTRL,
			MSM_RPM_CTRL_ACK_SEL_0, MSM_RPM_SEL_MASK_SIZE);
		msm_rpm_write(MSM_RPM_PAGE_CTRL, MSM_RPM_CTRL_ACK_CTX_0, 0);
		/* Ensure write is complete before return */
		dsb();

		if (msm_rpm_request->done)
			complete_all(msm_rpm_request->done);

		msm_rpm_request = NULL;
		return 0;
	}

	return 2;
}

static irqreturn_t msm_rpm_ack_interrupt(int irq, void *dev_id)
{
	unsigned long flags;
	int rc;

	if (dev_id != &msm_rpm_ack_interrupt)
		return IRQ_NONE;

	spin_lock_irqsave(&msm_rpm_irq_lock, flags);
	rc = msm_rpm_process_ack_interrupt();
	spin_unlock_irqrestore(&msm_rpm_irq_lock, flags);

	return IRQ_HANDLED;
}

/*
 * Note: assumes caller has acquired <msm_rpm_irq_lock>.
 */
static void msm_rpm_busy_wait_for_request_completion(
	bool allow_async_completion)
{
	int rc;

	do {
		while (!gic_is_spi_pending(msm_rpm_platform->irq_ack) &&
				msm_rpm_request) {
			if (allow_async_completion)
				spin_unlock(&msm_rpm_irq_lock);
			udelay(1);
			if (allow_async_completion)
				spin_lock(&msm_rpm_irq_lock);
		}

		if (!msm_rpm_request)
			break;

		rc = msm_rpm_process_ack_interrupt();
		gic_clear_spi_pending(msm_rpm_platform->irq_ack);
	} while (rc);
}

/* Upon return, the <req> array will contain values from the ack page.
 *
 * Note: assumes caller has acquired <msm_rpm_mutex>.
 *
 * Return value:
 *   0: success
 *   -ENOSPC: request rejected
 */
static int msm_rpm_set_exclusive(int ctx,
	uint32_t *sel_masks, struct msm_rpm_iv_pair *req, int count)
{
	DECLARE_COMPLETION_ONSTACK(ack);
	unsigned long flags;
	uint32_t ctx_mask = msm_rpm_get_ctx_mask(ctx);
	uint32_t ctx_mask_ack;
	uint32_t sel_masks_ack[MSM_RPM_SEL_MASK_SIZE];
	int i;

	msm_rpm_request_irq_mode.req = req;
	msm_rpm_request_irq_mode.count = count;
	msm_rpm_request_irq_mode.ctx_mask_ack = &ctx_mask_ack;
	msm_rpm_request_irq_mode.sel_masks_ack = sel_masks_ack;
	msm_rpm_request_irq_mode.done = &ack;

	spin_lock_irqsave(&msm_rpm_lock, flags);
	spin_lock(&msm_rpm_irq_lock);

	BUG_ON(msm_rpm_request);
	msm_rpm_request = &msm_rpm_request_irq_mode;

	for (i = 0; i < count; i++) {
		BUG_ON(req[i].id > MSM_RPM_ID_LAST);
		msm_rpm_write(MSM_RPM_PAGE_REQ, req[i].id, req[i].value);
	}

	msm_rpm_write_contiguous(MSM_RPM_PAGE_CTRL,
		MSM_RPM_CTRL_REQ_SEL_0, sel_masks, MSM_RPM_SEL_MASK_SIZE);
	msm_rpm_write(MSM_RPM_PAGE_CTRL, MSM_RPM_CTRL_REQ_CTX_0, ctx_mask);

	/* Ensure RPM data is written before sending the interrupt */
	dsb();
	msm_rpm_send_req_interrupt();

	spin_unlock(&msm_rpm_irq_lock);
	spin_unlock_irqrestore(&msm_rpm_lock, flags);

	wait_for_completion(&ack);

	BUG_ON((ctx_mask_ack & ~(msm_rpm_get_ctx_mask(MSM_RPM_CTX_REJECTED)))
		!= ctx_mask);
	BUG_ON(memcmp(sel_masks, sel_masks_ack, sizeof(sel_masks_ack)));

	return (ctx_mask_ack & msm_rpm_get_ctx_mask(MSM_RPM_CTX_REJECTED))
		? -ENOSPC : 0;
}

/* Upon return, the <req> array will contain values from the ack page.
 *
 * Note: assumes caller has acquired <msm_rpm_lock>.
 *
 * Return value:
 *   0: success
 *   -ENOSPC: request rejected
 */
static int msm_rpm_set_exclusive_noirq(int ctx,
	uint32_t *sel_masks, struct msm_rpm_iv_pair *req, int count)
{
	unsigned int irq = msm_rpm_platform->irq_ack;
	unsigned long flags;
	uint32_t ctx_mask = msm_rpm_get_ctx_mask(ctx);
	uint32_t ctx_mask_ack;
	uint32_t sel_masks_ack[MSM_RPM_SEL_MASK_SIZE];
	int i;

	msm_rpm_request_poll_mode.req = req;
	msm_rpm_request_poll_mode.count = count;
	msm_rpm_request_poll_mode.ctx_mask_ack = &ctx_mask_ack;
	msm_rpm_request_poll_mode.sel_masks_ack = sel_masks_ack;
	msm_rpm_request_poll_mode.done = NULL;

	spin_lock_irqsave(&msm_rpm_irq_lock, flags);
	get_irq_chip(irq)->mask(irq);

	if (msm_rpm_request) {
		msm_rpm_busy_wait_for_request_completion(true);
		BUG_ON(msm_rpm_request);
	}

	msm_rpm_request = &msm_rpm_request_poll_mode;

	for (i = 0; i < count; i++) {
		BUG_ON(req[i].id > MSM_RPM_ID_LAST);
		msm_rpm_write(MSM_RPM_PAGE_REQ, req[i].id, req[i].value);
	}

	msm_rpm_write_contiguous(MSM_RPM_PAGE_CTRL,
		MSM_RPM_CTRL_REQ_SEL_0, sel_masks, MSM_RPM_SEL_MASK_SIZE);
	msm_rpm_write(MSM_RPM_PAGE_CTRL, MSM_RPM_CTRL_REQ_CTX_0, ctx_mask);

	/* Ensure RPM data is written before sending the interrupt */
	dsb();
	msm_rpm_send_req_interrupt();

	msm_rpm_busy_wait_for_request_completion(false);
	BUG_ON(msm_rpm_request);

	get_irq_chip(irq)->unmask(irq);
	spin_unlock_irqrestore(&msm_rpm_irq_lock, flags);

	BUG_ON((ctx_mask_ack & ~(msm_rpm_get_ctx_mask(MSM_RPM_CTX_REJECTED)))
		!= ctx_mask);
	BUG_ON(memcmp(sel_masks, sel_masks_ack, sizeof(sel_masks_ack)));

	return (ctx_mask_ack & msm_rpm_get_ctx_mask(MSM_RPM_CTX_REJECTED))
		? -ENOSPC : 0;
}

/* Upon return, the <req> array will contain values from the ack page.
 *
 * Return value:
 *   0: success
 *   -EINTR: interrupted
 *   -EINVAL: invalid <ctx> or invalid id in <req> array
 *   -ENOSPC: request rejected
 */
static int msm_rpm_set_common(
	int ctx, struct msm_rpm_iv_pair *req, int count, bool noirq)
{
	uint32_t sel_masks[MSM_RPM_SEL_MASK_SIZE] = {};
	int rc;

	if (ctx >= MSM_RPM_CTX_SET_COUNT) {
		rc = -EINVAL;
		goto set_common_exit;
	}

	rc = msm_rpm_fill_sel_masks(sel_masks, req, count);
	if (rc)
		goto set_common_exit;

	if (noirq) {
		unsigned long flags;

		spin_lock_irqsave(&msm_rpm_lock, flags);
		rc = msm_rpm_set_exclusive_noirq(ctx, sel_masks, req, count);
		spin_unlock_irqrestore(&msm_rpm_lock, flags);
	} else {
		rc = mutex_lock_interruptible(&msm_rpm_mutex);
		if (rc)
			goto set_common_exit;

		rc = msm_rpm_set_exclusive(ctx, sel_masks, req, count);
		mutex_unlock(&msm_rpm_mutex);
	}

set_common_exit:
	return rc;
}

/*
 * Return value:
 *   0: success
 *   -EINTR: interrupted
 *   -EINVAL: invalid <ctx> or invalid id in <req> array
 */
static int msm_rpm_clear_common(
	int ctx, struct msm_rpm_iv_pair *req, int count, bool noirq)
{
	uint32_t sel_masks[MSM_RPM_SEL_MASK_SIZE] = {};
	struct msm_rpm_iv_pair r[MSM_RPM_SEL_MASK_SIZE];
	int rc;
	int i;

	if (ctx >= MSM_RPM_CTX_SET_COUNT) {
		rc = -EINVAL;
		goto clear_common_exit;
	}

	rc = msm_rpm_fill_sel_masks(sel_masks, req, count);
	if (rc)
		goto clear_common_exit;

	for (i = 0; i < ARRAY_SIZE(r); i++) {
		r[i].id = MSM_RPM_ID_INVALIDATE_0 + i;
		r[i].value = sel_masks[i];
	}

	memset(sel_masks, 0, sizeof(sel_masks));
	sel_masks[msm_rpm_get_sel_mask_reg(MSM_RPM_SEL_INVALIDATE)] |=
		msm_rpm_get_sel_mask(MSM_RPM_SEL_INVALIDATE);

	if (noirq) {
		unsigned long flags;

		spin_lock_irqsave(&msm_rpm_lock, flags);
		rc = msm_rpm_set_exclusive_noirq(ctx, sel_masks, r,
			ARRAY_SIZE(r));
		spin_unlock_irqrestore(&msm_rpm_lock, flags);
		BUG_ON(rc);
	} else {
		rc = mutex_lock_interruptible(&msm_rpm_mutex);
		if (rc)
			goto clear_common_exit;

		rc = msm_rpm_set_exclusive(ctx, sel_masks, r, ARRAY_SIZE(r));
		mutex_unlock(&msm_rpm_mutex);
		BUG_ON(rc);
	}

clear_common_exit:
	return rc;
}

/*
 * Note: assumes caller has acquired <msm_rpm_mutex>.
 */
static void msm_rpm_update_notification(uint32_t ctx,
	struct msm_rpm_notif_config *curr_cfg,
	struct msm_rpm_notif_config *new_cfg)
{
	if (memcmp(curr_cfg, new_cfg, sizeof(*new_cfg))) {
		uint32_t sel_masks[MSM_RPM_SEL_MASK_SIZE] = {};
		int rc;

		sel_masks[msm_rpm_get_sel_mask_reg(MSM_RPM_SEL_NOTIFICATION)]
			|= msm_rpm_get_sel_mask(MSM_RPM_SEL_NOTIFICATION);

		rc = msm_rpm_set_exclusive(ctx,
			sel_masks, new_cfg->iv, ARRAY_SIZE(new_cfg->iv));
		BUG_ON(rc);

		memcpy(curr_cfg, new_cfg, sizeof(*new_cfg));
	}
}

/*
 * Note: assumes caller has acquired <msm_rpm_mutex>.
 */
static void msm_rpm_initialize_notification(void)
{
	struct msm_rpm_notif_config cfg;
	unsigned int ctx;
	int i;

	for (ctx = MSM_RPM_CTX_SET_0; ctx <= MSM_RPM_CTX_SET_SLEEP; ctx++) {
		cfg = msm_rpm_notif_cfgs[ctx];

		for (i = 0; i < MSM_RPM_SEL_MASK_SIZE; i++) {
			configured_iv(&cfg)[i].id =
				MSM_RPM_ID_NOTIFICATION_CONFIGURED_0 + i;
			configured_iv(&cfg)[i].value = ~0UL;

			registered_iv(&cfg)[i].id =
				MSM_RPM_ID_NOTIFICATION_REGISTERED_0 + i;
			registered_iv(&cfg)[i].value = 0;
		}

		msm_rpm_update_notification(ctx,
			&msm_rpm_notif_cfgs[ctx], &cfg);
	}
}

/******************************************************************************
 * Public functions
 *****************************************************************************/

void msm_rpm_print_sleep_tick(void)
{
	uint32_t *mpm_sleep_tick = (void *) (MSM_RPM_MPM_BASE + 0x24);
	uint64_t timestamp = (uint64_t)(*mpm_sleep_tick);

	pr_info("MPM_SLEEP_TICK: %llums\n", (timestamp * 1000) >> 15);
}
EXPORT_SYMBOL(msm_rpm_print_sleep_tick);

int msm_rpm_local_request_is_outstanding(void)
{
	unsigned long flags;
	int outstanding = 0;

	if (!spin_trylock_irqsave(&msm_rpm_lock, flags))
		goto local_request_is_outstanding_exit;

	if (!spin_trylock(&msm_rpm_irq_lock))
		goto local_request_is_outstanding_unlock;

	outstanding = (msm_rpm_request != NULL);
	spin_unlock(&msm_rpm_irq_lock);

local_request_is_outstanding_unlock:
	spin_unlock_irqrestore(&msm_rpm_lock, flags);

local_request_is_outstanding_exit:
	return outstanding;
}

/*
 * Read the specified status registers and return their values.
 *
 * status: array of id-value pairs.  Each <id> specifies a status register,
 *         i.e, one of MSM_RPM_STATUS_ID_xxxx.  Upon return, each <value> will
 *         contain the value of the status register.
 * count: number of id-value pairs in the array
 *
 * Return value:
 *   0: success
 *   -EBUSY: RPM is updating the status page; values across different registers
 *           may not be consistent
 *   -EINVAL: invalid id in <status> array
 */
int msm_rpm_get_status(struct msm_rpm_iv_pair *status, int count)
{
	uint32_t seq_begin;
	uint32_t seq_end;
	int rc;
	int i;

	seq_begin = msm_rpm_read(MSM_RPM_PAGE_STATUS,
				MSM_RPM_STATUS_ID_SEQUENCE);

	for (i = 0; i < count; i++) {
		if (status[i].id > MSM_RPM_STATUS_ID_LAST) {
			rc = -EINVAL;
			goto get_status_exit;
		}

		status[i].value = msm_rpm_read(MSM_RPM_PAGE_STATUS,
						status[i].id);
	}

	seq_end = msm_rpm_read(MSM_RPM_PAGE_STATUS,
				MSM_RPM_STATUS_ID_SEQUENCE);

	rc = (seq_begin != seq_end || (seq_begin & 0x01)) ? -EBUSY : 0;

get_status_exit:
	return rc;
}
EXPORT_SYMBOL(msm_rpm_get_status);

/*
 * Issue a resource request to RPM to set resource values.
 *
 * Note: the function may sleep and must be called in a task context.
 *
 * ctx: the request's context.
 *      There two contexts that a RPM driver client can use:
 *      MSM_RPM_CTX_SET_0 and MSM_RPM_CTX_SET_SLEEP.  For resource values
 *      that are intended to take effect when the CPU is active,
 *      MSM_RPM_CTX_SET_0 should be used.  For resource values that are
 *      intended to take effect when the CPU is not active,
 *      MSM_RPM_CTX_SET_SLEEP should be used.
 * req: array of id-value pairs.  Each <id> specifies a RPM resource,
 *      i.e, one of MSM_RPM_ID_xxxx.  Each <value> specifies the requested
 *      resource value.
 * count: number of id-value pairs in the array
 *
 * Return value:
 *   0: success
 *   -EINTR: interrupted
 *   -EINVAL: invalid <ctx> or invalid id in <req> array
 *   -ENOSPC: request rejected
 */
int msm_rpm_set(int ctx, struct msm_rpm_iv_pair *req, int count)
{
	return msm_rpm_set_common(ctx, req, count, false);
}
EXPORT_SYMBOL(msm_rpm_set);

/*
 * Issue a resource request to RPM to set resource values.
 *
 * Note: the function is similar to msm_rpm_set() except that it must be
 *       called with interrupts masked.  If possible, use msm_rpm_set()
 *       instead, to maximize CPU throughput.
 */
int msm_rpm_set_noirq(int ctx, struct msm_rpm_iv_pair *req, int count)
{
	WARN(!irqs_disabled(), "msm_rpm_set_noirq can only be called "
		"safely when local irqs are disabled.  Consider using "
		"msm_rpm_set or msm_rpm_set_nosleep instead.");
	return msm_rpm_set_common(ctx, req, count, true);
}
EXPORT_SYMBOL(msm_rpm_set_noirq);

/*
 * Issue a resource request to RPM to clear resource values.  Once the
 * values are cleared, the resources revert back to their default values
 * for this RPM master.
 *
 * Note: the function may sleep and must be called in a task context.
 *
 * ctx: the request's context.
 * req: array of id-value pairs.  Each <id> specifies a RPM resource,
 *      i.e, one of MSM_RPM_ID_xxxx.  <value>'s are ignored.
 * count: number of id-value pairs in the array
 *
 * Return value:
 *   0: success
 *   -EINTR: interrupted
 *   -EINVAL: invalid <ctx> or invalid id in <req> array
 */
int msm_rpm_clear(int ctx, struct msm_rpm_iv_pair *req, int count)
{
	return msm_rpm_clear_common(ctx, req, count, false);
}
EXPORT_SYMBOL(msm_rpm_clear);

/*
 * Issue a resource request to RPM to clear resource values.
 *
 * Note: the function is similar to msm_rpm_clear() except that it must be
 *       called with interrupts masked.  If possible, use msm_rpm_clear()
 *       instead, to maximize CPU throughput.
 */
int msm_rpm_clear_noirq(int ctx, struct msm_rpm_iv_pair *req, int count)
{
	WARN(!irqs_disabled(), "msm_rpm_clear_noirq can only be called "
		"safely when local irqs are disabled.  Consider using "
		"msm_rpm_clear or msm_rpm_clear_nosleep instead.");
	return msm_rpm_clear_common(ctx, req, count, true);
}
EXPORT_SYMBOL(msm_rpm_clear_noirq);

/*
 * Register for RPM notification.  When the specified resources
 * change their status on RPM, RPM sends out notifications and the
 * driver will "up" the semaphore in struct msm_rpm_notification.
 *
 * Note: the function may sleep and must be called in a task context.
 *
 *       Memory for <n> must not be freed until the notification is
 *       unregistered.  Memory for <req> can be freed after this
 *       function returns.
 *
 * n: the notifcation object.  Caller should initialize only the
 *    semaphore field.  When a notification arrives later, the
 *    semaphore will be "up"ed.
 * req: array of id-value pairs.  Each <id> specifies a status register,
 *      i.e, one of MSM_RPM_STATUS_ID_xxxx.  <value>'s are ignored.
 * count: number of id-value pairs in the array
 *
 * Return value:
 *   0: success
 *   -EINTR: interrupted
 *   -EINVAL: invalid id in <req> array
 */
int msm_rpm_register_notification(struct msm_rpm_notification *n,
	struct msm_rpm_iv_pair *req, int count)
{
	unsigned long flags;
	unsigned int ctx;
	struct msm_rpm_notif_config cfg;
	int rc;
	int i;

	INIT_LIST_HEAD(&n->list);
	rc = msm_rpm_fill_sel_masks(n->sel_masks, req, count);
	if (rc)
		goto register_notification_exit;

	rc = mutex_lock_interruptible(&msm_rpm_mutex);
	if (rc)
		goto register_notification_exit;

	if (!msm_rpm_init_notif_done) {
		msm_rpm_initialize_notification();
		msm_rpm_init_notif_done = true;
	}

	spin_lock_irqsave(&msm_rpm_irq_lock, flags);
	list_add(&n->list, &msm_rpm_notifications);
	spin_unlock_irqrestore(&msm_rpm_irq_lock, flags);

	ctx = MSM_RPM_CTX_SET_0;
	cfg = msm_rpm_notif_cfgs[ctx];

	for (i = 0; i < MSM_RPM_SEL_MASK_SIZE; i++)
		registered_iv(&cfg)[i].value |= n->sel_masks[i];

	msm_rpm_update_notification(ctx, &msm_rpm_notif_cfgs[ctx], &cfg);
	mutex_unlock(&msm_rpm_mutex);

register_notification_exit:
	return rc;
}
EXPORT_SYMBOL(msm_rpm_register_notification);

/*
 * Unregister a notification.
 *
 * Note: the function may sleep and must be called in a task context.
 *
 * n: the notifcation object that was registered previously.
 *
 * Return value:
 *   0: success
 *   -EINTR: interrupted
 */
int msm_rpm_unregister_notification(struct msm_rpm_notification *n)
{
	unsigned long flags;
	unsigned int ctx;
	struct msm_rpm_notif_config cfg;
	int rc;
	int i;

	rc = mutex_lock_interruptible(&msm_rpm_mutex);
	if (rc)
		goto unregister_notification_exit;

	ctx = MSM_RPM_CTX_SET_0;
	cfg = msm_rpm_notif_cfgs[ctx];

	for (i = 0; i < MSM_RPM_SEL_MASK_SIZE; i++)
		registered_iv(&cfg)[i].value = 0;

	spin_lock_irqsave(&msm_rpm_irq_lock, flags);
	list_del(&n->list);
	list_for_each_entry(n, &msm_rpm_notifications, list)
		for (i = 0; i < MSM_RPM_SEL_MASK_SIZE; i++)
			registered_iv(&cfg)[i].value |= n->sel_masks[i];
	spin_unlock_irqrestore(&msm_rpm_irq_lock, flags);

	msm_rpm_update_notification(ctx, &msm_rpm_notif_cfgs[ctx], &cfg);
	mutex_unlock(&msm_rpm_mutex);

unregister_notification_exit:
	return rc;
}
EXPORT_SYMBOL(msm_rpm_unregister_notification);

int __init msm_rpm_init(struct msm_rpm_platform_data *data)
{
	uint32_t major;
	uint32_t minor;
	uint32_t build;
	uint32_t local_build;
	unsigned int irq;
	int rc;

	msm_rpm_platform = data;
	msm_rpm_stat_data = (stats_blob *)msm_rpm_platform->reg_base_addrs[MSM_RPM_PAGE_STAT];

	major = msm_rpm_read(MSM_RPM_PAGE_STATUS,
					MSM_RPM_STATUS_ID_VERSION_MAJOR);
	minor = msm_rpm_read(MSM_RPM_PAGE_STATUS,
					MSM_RPM_STATUS_ID_VERSION_MINOR);
	build = msm_rpm_read(MSM_RPM_PAGE_STATUS,
					MSM_RPM_STATUS_ID_VERSION_BUILD);

	local_build = (build >> 24) & 0xff;
	build = build & 0xffffff;
	pr_info("%s: RPM firmware %u.%u.%u.%u\n", __func__,
		major, minor, build, local_build);

	msm_rpm_write(MSM_RPM_PAGE_CTRL, MSM_RPM_CTRL_VERSION_MAJOR, 2);
	msm_rpm_write(MSM_RPM_PAGE_CTRL, MSM_RPM_CTRL_VERSION_MINOR, 0);
	msm_rpm_write(MSM_RPM_PAGE_CTRL, MSM_RPM_CTRL_VERSION_BUILD, 0);

	irq = msm_rpm_platform->irq_ack;

	rc = request_irq(irq, msm_rpm_ack_interrupt,
			IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND,
			"rpm_drv", msm_rpm_ack_interrupt);
	if (rc) {
		pr_err("%s: failed to request irq %d: %d\n",
			__func__, irq, rc);
		return rc;
	}

	rc = set_irq_wake(irq, 1);
	if (rc) {
		pr_err("%s: failed to set wakeup irq %u: %d\n",
			__func__, irq, rc);
		return rc;
	}

	msm_rpm_populate_map();
	msm_rpm_print_sleep_tick();
	return 0;
}

void __init msm_rpm_lpm_init(uint32_t *lpm_setting, uint32_t num)
{
	uint32_t i = 0;
	for (i = 0; i < num; i++)
		msm_rpm_write(MSM_RPM_PAGE_STAT, RPM_LPM_PM8058 + i, lpm_setting[i]);
}

void msm_rpm_dump_stat(void)
{
	int i = 0;

	if (msm_rpm_stat_data) {
		pr_info("%s: %u, %llums, %u, %llums, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n", __func__,
			msm_rpm_stat_data->stats[RPM_STAT_XO_SHUTDOWN_COUNT].value,
			((uint64_t)msm_rpm_stat_data->stats[RPM_STAT_XO_SHUTDOWN_TIME].value * 1000) >> 15,
			msm_rpm_stat_data->stats[RPM_STAT_VDD_MIN_COUNT].value,
			((uint64_t)msm_rpm_stat_data->stats[RPM_STAT_VDD_MIN_TIME].value * 1000) >> 15,
			msm_rpm_stat_data->mpm_int_status[0], msm_rpm_stat_data->mpm_int_status[1],
			msm_rpm_stat_data->mpm_trigger[RPM_MASTER_0][0], msm_rpm_stat_data->mpm_trigger[RPM_MASTER_0][1],
			msm_rpm_stat_data->mpm_trigger[RPM_MASTER_1][0], msm_rpm_stat_data->mpm_trigger[RPM_MASTER_1][1],
			msm_rpm_stat_data->mpm_trigger[RPM_MASTER_2][0], msm_rpm_stat_data->mpm_trigger[RPM_MASTER_2][1]);
		for (i = 0; i < RPM_MASTER_COUNT; i++) {
			pr_info("sleep_info_m.%d - %llums, %llums, %d %d %d %d\n", i, ((uint64_t)msm_rpm_stat_data->wake_info[i].timestamp * 1000) >> 15,
				((uint64_t)msm_rpm_stat_data->sleep_info[i].timestamp * 1000) >> 15, msm_rpm_stat_data->sleep_info[i].cxo,
				msm_rpm_stat_data->sleep_info[i].pxo, msm_rpm_stat_data->sleep_info[i].vdd_mem,
				msm_rpm_stat_data->sleep_info[i].vdd_dig);
		}
		for (i = 0; i < 2; i++) {
			msm_rpm_stat_data->mpm_int_status[i] = 0;
			msm_rpm_stat_data->mpm_trigger[RPM_MASTER_0][i] = 0;
			msm_rpm_stat_data->mpm_trigger[RPM_MASTER_1][i] = 0;
			msm_rpm_stat_data->mpm_trigger[RPM_MASTER_2][i] = 0;
		}
	}
}

void msm_rpm_set_suspend_flag(bool isSuspendMode)
{
	msm_rpm_stat_data->isSuspendMode = (!!isSuspendMode);
}
EXPORT_SYMBOL(msm_rpm_set_suspend_flag);
