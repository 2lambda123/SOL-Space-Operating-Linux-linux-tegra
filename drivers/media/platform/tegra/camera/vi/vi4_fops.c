/*
 * Tegra Video Input 4 device common APIs
 *
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Frank Chen <frank@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/device.h>
#include <linux/nvhost.h>
#include <linux/tegra-powergate.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <media/tegra_camera_platform.h>
#include "nvhost_acm.h"
#include "linux/nvhost_ioctl.h"
#include "vi/vi4.h"
#include "mc_common.h"
#include "mipical/mipi_cal.h"
#include "vi4_registers.h"
#include "vi/vi_notify.h"

#define FRAMERATE	30
#define BPP_MEM		2
#define MAX_VI_CHANNEL 12

void tegra_channel_queued_buf_done(struct tegra_channel *chan,
					  enum vb2_buffer_state state);
int tegra_channel_set_stream(struct tegra_channel *chan, bool on);
void tegra_channel_ring_buffer(struct tegra_channel *chan,
		struct vb2_v4l2_buffer *vb,
		struct timespec *ts, int state);
struct tegra_channel_buffer *dequeue_buffer(struct tegra_channel *chan);
int update_clk(struct tegra_mc_vi *vi);
void tegra_channel_init_ring_buffer(struct tegra_channel *chan);
void free_ring_buffers(struct tegra_channel *chan, int frames);
int tegra_channel_set_power(struct tegra_channel *chan, bool on);
static void tegra_channel_stop_kthreads(struct tegra_channel *chan);
static int tegra_channel_stop_increments(struct tegra_channel *chan);
static void tegra_channel_notify_status_callback(
				struct vi_notify_channel *,
				const struct vi_capture_status *,
				void *);
static void tegra_channel_notify_error_callback(void *);

u32 csimux_config_stream[] = {
	CSIMUX_CONFIG_STREAM_0,
	CSIMUX_CONFIG_STREAM_1,
	CSIMUX_CONFIG_STREAM_2,
	CSIMUX_CONFIG_STREAM_3,
	CSIMUX_CONFIG_STREAM_4,
	CSIMUX_CONFIG_STREAM_5
};

static void vi4_write(struct tegra_channel *chan, unsigned int addr, u32 val)
{
	writel(val, chan->vi->iomem + addr);
}

static u32 vi4_read(struct tegra_channel *chan, unsigned int addr)
{
	return readl(chan->vi->iomem + addr);
}

static void vi4_channel_write(struct tegra_channel *chan,
		unsigned int index, unsigned int addr, u32 val)
{
	writel(val, chan->vi->iomem + 0x10000 + (0x10000 * index) + addr);
}

static bool vi4_init(struct tegra_channel *chan)
{
	vi4_write(chan, CFG_INTERRUPT_MASK, 0x3f0000f9);
	vi4_write(chan, CFG_INTERRUPT_STATUS, 0x3f000001);
	vi4_write(chan, NOTIFY_ERROR, 0x1);
	vi4_write(chan, NOTIFY_TAG_CLASSIFY_0, 0xe39c08e3);

	return true;
}

static bool vi4_check_status(struct tegra_channel *chan)
{
	int status;

	/* check interrupt status error */
	status = vi4_read(chan, CFG_INTERRUPT_STATUS);
	if (status & 0x1)
		dev_err(chan->vi->dev,
			"VI_CFG_INTERRUPT_STATUS_0: MASTER_ERR_STATUS error!\n");

	/* Check VI NOTIFY input FIFO error */
	status = vi4_read(chan, NOTIFY_ERROR);
	if (status & 0x1)
		dev_err(chan->vi->dev,
			"VI_NOTIFY_ERROR_0: NOTIFY_FIFO_OVERFLOW error!\n");

	return true;
}

static bool vi_notify_wait(struct tegra_channel *chan)
{
	int err;
	u32 thresh;
	struct timespec ts;
	struct tegra_vi4_syncpts_req req = {
		.syncpt_ids = {
			chan->syncpt[0],
			chan->syncpt[1],
			chan->syncpt[1],
		},
		.stream = chan->port[0],
		.vc = 0,
	};

	if (!chan->bfirst_fstart)
		vi_notify_channel_enable_reports(chan->vnc_id, chan->vnc, &req);

	/*
	 * Increment syncpt for ATOMP_FE
	 *
	 * This is needed in order to keep the syncpt max up to date,
	 * even if we are not waiting for ATOMP_FE here
	 */
	thresh = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[1], 1);

	/*
	 * Increment syncpt for PXL_SOF
	 *
	 * Increment and retrieve PXL_SOF syncpt max value.
	 * This value will be used to wait for next syncpt
	 */
	thresh = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[0], 1);

	/*
	 * Wait for PXL_SOF syncpt
	 *
	 * Use the syncpt max value we just set as threshold
	 */
	err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
			chan->syncpt[0], thresh,
			250, NULL, &ts);
	if (err < 0)
		dev_err(chan->vi->dev,
			"PXL_SOF syncpt timeout! err = %d\n", err);

	return true;
}

static bool tegra_channel_surface_setup(struct tegra_channel *chan,
	struct tegra_channel_buffer *buf)
{
	vi4_channel_write(chan, chan->vnc_id, ATOMP_EMB_SURFACE_OFFSET0, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_EMB_SURFACE_OFFSET0_H, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_EMB_SURFACE_STRIDE0, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_OFFSET0, buf->addr);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_STRIDE0,
		chan->format.bytesperline);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_OFFSET0_H, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_OFFSET1, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_OFFSET1_H, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_STRIDE1, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_OFFSET2, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_OFFSET2_H, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_SURFACE_STRIDE2, 0x0);

	return true;
}

static void tegra_channel_handle_error(struct tegra_channel *chan)
{
	struct v4l2_subdev *sd_on_csi = chan->subdev_on_csi;
	static const struct v4l2_event source_ev_fmt = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_ERROR,
	};

	tegra_channel_stop_increments(chan);
	vb2_queue_error(&chan->queue);

	/* Application gets notified after CSI Tx's are reset */
	if (sd_on_csi->devnode)
		v4l2_subdev_notify_event(sd_on_csi, &source_ev_fmt);
}


static void tegra_channel_notify_status_callback(
				struct vi_notify_channel *vnc,
				const struct vi_capture_status *status,
				void *client_data)
{
	struct tegra_channel *chan = (struct tegra_channel *)client_data;

	spin_lock(&chan->capture_state_lock);
	if (chan->capture_state == CAPTURE_GOOD)
		chan->capture_state = CAPTURE_ERROR;
	else {
		spin_unlock(&chan->capture_state_lock);
		return;
	}
	spin_unlock(&chan->capture_state_lock);

	dev_err(chan->vi->dev, "Status: %2u channel:%02X frame:%04X\n",
		status->status, chan->vnc_id, status->frame);
	dev_err(chan->vi->dev, "         timestamp sof %llu eof %llu data 0x%08x\n",
		status->sof_ts, status->eof_ts, status->data);
	dev_err(chan->vi->dev, "         capture_id %u stream %2u vchan %2u\n",
		status->capture_id, status->st, status->vc);

	tegra_channel_handle_error(chan);
}

static bool tegra_channel_notify_setup(struct tegra_channel *chan)
{
	int i;

	chan->vnc_id = -1;
	for (i = 0; i < MAX_VI_CHANNEL; i++) {
		chan->vnc = vi_notify_channel_open(i);
		if (!IS_ERR(chan->vnc)) {
			chan->vnc_id = i;
			break;
		}
	}
	if (chan->vnc_id < 0) {
		dev_err(chan->vi->dev, "No VI channel available!\n");
		return false;
	}

	vi_notify_channel_set_notify_funcs(chan->vnc,
			&tegra_channel_notify_status_callback,
			&tegra_channel_notify_error_callback,
			(void *)chan);

	/* get PXL_SOF syncpt id */
	chan->syncpt[0] =
		nvhost_get_syncpt_client_managed(chan->vi->ndev, "tegra-vi4");

	/* get ATOMP_FE syncpt id */
	chan->syncpt[1] =
		nvhost_get_syncpt_client_managed(chan->vi->ndev, "tegra-vi4");

	nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev, chan->syncpt[0]);
	nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev, chan->syncpt[1]);

	return true;
}


static int tegra_channel_capture_setup(struct tegra_channel *chan)
{
	u32 height = chan->format.height;
	u32 width = chan->format.width;
	u32 format = chan->fmtinfo->img_fmt;
	u32 data_type = chan->fmtinfo->img_dt;
	u32 csi_port = chan->port[0];
	u32 stream = 1U << csi_port;
	u32 virtual_ch = 1U << 0;

	tegra_channel_notify_setup(chan);

	vi4_write(chan, csimux_config_stream[csi_port], 0x1);

	vi4_channel_write(chan, chan->vnc_id, MATCH,
			((stream << STREAM_SHIFT) & STREAM) |
			STREAM_MASK |
			((virtual_ch << VIRTUAL_CHANNEL_SHIFT) &
			VIRTUAL_CHANNEL)  |
			VIRTUAL_CHANNEL_MASK);

	vi4_channel_write(chan, chan->vnc_id, MATCH_DATATYPE,
			((data_type << DATATYPE_SHIFT) & DATATYPE) |
			DATATYPE_MASK);

	vi4_channel_write(chan, chan->vnc_id, DT_OVERRIDE, 0x0);

	vi4_channel_write(chan, chan->vnc_id, MATCH_FRAMEID,
			((0 << FRAMEID_SHIFT) & FRAMEID) | 0);

	vi4_channel_write(chan, chan->vnc_id, FRAME_X, width);
	vi4_channel_write(chan, chan->vnc_id, FRAME_Y, height);
	vi4_channel_write(chan, chan->vnc_id, SKIP_X, 0x0);
	vi4_channel_write(chan, chan->vnc_id, CROP_X, width);
	vi4_channel_write(chan, chan->vnc_id, OUT_X, width);
	vi4_channel_write(chan, chan->vnc_id, SKIP_Y, 0x0);
	vi4_channel_write(chan, chan->vnc_id, CROP_Y, height);
	vi4_channel_write(chan, chan->vnc_id, OUT_Y, height);
	vi4_channel_write(chan, chan->vnc_id, PIXFMT_ENABLE, PIXFMT_EN);
	vi4_channel_write(chan, chan->vnc_id, PIXFMT_WIDE, 0x0);
	vi4_channel_write(chan, chan->vnc_id, PIXFMT_FORMAT, format);
	vi4_channel_write(chan, chan->vnc_id, DPCM_STRIP, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ATOMP_DPCM_CHUNK, 0x0);
	vi4_channel_write(chan, chan->vnc_id, ISPBUFA, 0x0);
	vi4_channel_write(chan, chan->vnc_id, LINE_TIMER, 0x1000000);
	vi4_channel_write(chan, chan->vnc_id, EMBED_X, 0x0);
	vi4_channel_write(chan, chan->vnc_id, EMBED_Y, 0x0);
	/*
	 * Set ATOMP_RESERVE to 0 so rctpu won't increment syncpt
	 * for captureInfo. This is copied from nvvi driver.
	 *
	 * If we don't set this register to 0, ATOMP_FE syncpt
	 * will be increment by 2 for each frame
	 */
	vi4_channel_write(chan, chan->vnc_id, ATOMP_RESERVE, 0x0);

	dev_dbg(chan->vi->dev,
		"Create Surface with imgW=%d, imgH=%d, memFmt=%d\n",
		width, height, format);

	return 0;
}

static int tegra_channel_enable_stream(struct tegra_channel *chan)
{
	int ret = 0;

	ret = tegra_channel_set_stream(chan, true);
	if (ret < 0)
		return ret;

	return ret;
}

static int tegra_channel_capture_frame(struct tegra_channel *chan,
				       struct tegra_channel_buffer *buf)
{
	struct vb2_v4l2_buffer *vb = &buf->buf;
	struct timespec ts;
	int state = VB2_BUF_STATE_DONE;
	unsigned long flags;
	int err = false;

	tegra_channel_surface_setup(chan, buf);

	vi4_channel_write(chan, chan->vnc_id, CHANNEL_COMMAND, LOAD);
	vi4_channel_write(chan, chan->vnc_id,
		CONTROL, SINGLESHOT | MATCH_STATE_EN);

	if (!chan->bfirst_fstart)
		err = tegra_channel_enable_stream(chan);

	/* wait for vi notifier events */
	vi_notify_wait(chan);

	vi4_check_status(chan);

	spin_lock_irqsave(&chan->capture_state_lock, flags);
	if (chan->capture_state != CAPTURE_ERROR)
		chan->capture_state = CAPTURE_GOOD;
	spin_unlock_irqrestore(&chan->capture_state_lock, flags);

	getnstimeofday(&ts);
	tegra_channel_ring_buffer(chan, vb, &ts, state);

	return 0;
}

static int tegra_channel_stop_increments(struct tegra_channel *chan)
{
	struct tegra_vi4_syncpts_req req = {
		.syncpt_ids = {
		0xffffffff,
		0xffffffff,
		0xffffffff,
	},
				.stream = chan->port[0],
				.vc = 0,
	};

	/* No need to check errors. There's nothing we could do. */
	return vi_notify_channel_reset(chan->vnc_id, chan->vnc, &req);
}

static void tegra_channel_capture_done(struct tegra_channel *chan)
{
	struct timespec ts;
	struct tegra_channel_buffer *buf;
	int state = VB2_BUF_STATE_DONE;
	u32 thresh;
	unsigned long flags;
	int err;

	spin_lock_irqsave(&chan->capture_state_lock, flags);
	if (chan->capture_state == CAPTURE_IDLE) {
		spin_unlock_irqrestore(&chan->capture_state_lock, flags);
		return;
	} else if (chan->capture_state == CAPTURE_GOOD) {
		/* Mark capture state to IDLE as capture is finished */
		chan->capture_state = CAPTURE_IDLE;
		spin_unlock_irqrestore(&chan->capture_state_lock, flags);

		/* Get current ATOMP_FE syncpt min value */
		if (!nvhost_syncpt_read_ext_check(chan->vi->ndev,
				chan->syncpt[1], &thresh)) {
			/* Wait for ATOMP_FE syncpt
			 *
			 * This is to make sure we don't exit the capture thread
			 * before the last frame is done writing to memory
			 */
			err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
					chan->syncpt[1], thresh + 1,
					250, NULL, &ts);
			if (err < 0)
				dev_err(chan->vi->dev, "ATOMP_FE syncpt timeout!\n");
		}
	} else
		spin_unlock_irqrestore(&chan->capture_state_lock, flags);

	/* Get current ATOMP_FE syncpt min value */
	/* close vi-notifier */
	tegra_channel_stop_increments(chan);
	vi_notify_channel_close(chan->vnc_id, chan->vnc);
	chan->vnc_id = -1;

	/* free syncpts */
	nvhost_syncpt_put_ref_ext(chan->vi->ndev, chan->syncpt[0]);
	nvhost_syncpt_put_ref_ext(chan->vi->ndev, chan->syncpt[1]);

	/* dequeue buffer and return if no buffer exists */
	buf = dequeue_buffer(chan);
	if (!buf)
		return;

	tegra_channel_ring_buffer(chan, &buf->buf, &ts, state);
}

static int tegra_channel_kthread_capture_start(void *data)
{
	struct tegra_channel *chan = data;
	struct tegra_channel_buffer *buf;
	int err = 0;

	set_freezable();

	while (1) {

		try_to_freeze();

		wait_event_interruptible(chan->start_wait,
					 !list_empty(&chan->capture) ||
					 kthread_should_stop());

		if (kthread_should_stop()) {
			complete(&chan->capture_comp);
			break;
		}

		/* source is not streaming if error is non-zero */
		/* wait till kthread stop and dont DeQ buffers */
		if (err)
			continue;

		buf = dequeue_buffer(chan);
		if (!buf)
			continue;

		err = tegra_channel_capture_frame(chan, buf);
	}

	return 0;
}

static void tegra_channel_stop_kthreads(struct tegra_channel *chan)
{
	mutex_lock(&chan->stop_kthread_lock);
	/* Stop the kthread for capture */
	if (chan->kthread_capture_start) {
		kthread_stop(chan->kthread_capture_start);
		wait_for_completion(&chan->capture_comp);
		chan->kthread_capture_start = NULL;
	}
	mutex_unlock(&chan->stop_kthread_lock);
}

static int tegra_channel_update_clknbw(struct tegra_channel *chan, u8 on)
{
	int ret = 0;

	/* width * height * fps * KBytes write to memory
	 * WAR: Using fix fps until we have a way to set it
	 */
	chan->requested_kbyteps = (on > 0 ? 1 : -1) *
		((long long)(chan->format.width * chan->format.height
		* FRAMERATE * BPP_MEM) * 115 / 100) / 1000;
	mutex_lock(&chan->vi->bw_update_lock);
	chan->vi->aggregated_kbyteps += chan->requested_kbyteps;
	ret = vi_v4l2_update_isobw(chan->vi->aggregated_kbyteps, 0);
	mutex_unlock(&chan->vi->bw_update_lock);
	if (ret)
		dev_info(chan->vi->dev,
		"WAR:Calculation not precise.Ignore BW request failure\n");
	ret = vi4_v4l2_set_la(chan->vi->ndev, 0, 0);
	if (ret)
		dev_info(chan->vi->dev,
		"WAR:Calculation not precise.Ignore LA failure\n");
	return 0;
}

int vi4_channel_start_streaming(struct vb2_queue *vq, u32 count)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	struct media_pipeline *pipe = chan->video.entity.pipe;
	int ret = 0, i;
	unsigned long request_pixelrate;
	unsigned long flags;

	vi4_init(chan);
	if (!chan->vi->pg_mode) {
		/* Start the pipeline. */
		ret = media_entity_pipeline_start(&chan->video.entity, pipe);
		if (ret < 0)
			goto error_pipeline_start;
	}

	if (chan->bypass) {
		ret = tegra_channel_set_stream(chan, true);
		if (ret < 0)
			goto error_set_stream;
		tegra_mipi_bias_pad_enable();
		mutex_lock(&chan->vi->mipical_lock);
		csi_mipi_cal(chan, 1);
		mutex_unlock(&chan->vi->mipical_lock);
		return ret;
	}

	tegra_mipi_bias_pad_enable();
	spin_lock_irqsave(&chan->capture_state_lock, flags);
	chan->capture_state = CAPTURE_IDLE;
	spin_unlock_irqrestore(&chan->capture_state_lock, flags);
	for (i = 0; i < chan->valid_ports; i++) {
		/* ensure sync point state is clean */
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
							chan->syncpt[i]);
	}

	/* Note: Program VI registers after TPG, sensors and CSI streaming */
	ret = tegra_channel_capture_setup(chan);
	if (ret < 0)
		goto error_capture_setup;

	chan->sequence = 0;
	tegra_channel_init_ring_buffer(chan);

	/* Update clock and bandwidth based on the format */
	mutex_lock(&chan->vi->bw_update_lock);
	request_pixelrate = (long long)(chan->format.width * chan->format.height
			 * FRAMERATE / 100) * 115;
	ret = nvhost_module_set_rate(chan->vi->ndev, &chan->video,
			request_pixelrate, 0, NVHOST_PIXELRATE);
	mutex_unlock(&chan->vi->bw_update_lock);
	if (ret)
		goto error_capture_setup;
	ret = tegra_channel_update_clknbw(chan, 1);
	if (ret)
		goto error_capture_setup;
	/* Start kthread to capture data to buffer */
	chan->kthread_capture_start = kthread_run(
					tegra_channel_kthread_capture_start,
					chan, chan->video.name);
	if (IS_ERR(chan->kthread_capture_start)) {
		dev_err(&chan->video.dev,
			"failed to run kthread for capture start\n");
		ret = PTR_ERR(chan->kthread_capture_start);
		goto error_capture_setup;
	}

	return 0;

error_capture_setup:
	if (!chan->vi->pg_mode)
		tegra_channel_set_stream(chan, false);
error_set_stream:
	if (!chan->vi->pg_mode)
		media_entity_pipeline_stop(&chan->video.entity);
error_pipeline_start:
	vq->start_streaming_called = 0;
	tegra_channel_queued_buf_done(chan, VB2_BUF_STATE_QUEUED);

	return ret;
}

int vi4_channel_stop_streaming(struct vb2_queue *vq)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	bool is_streaming = atomic_read(&chan->is_streaming);

	if (chan->vnc_id == -1)
		return 0;

	if (!chan->bypass) {
		tegra_channel_stop_kthreads(chan);
		/* wait for last frame memory write ack */
		if (is_streaming)
			tegra_channel_capture_done(chan);
		/* free all the ring buffers */
		free_ring_buffers(chan, chan->num_buffers);
		/* dequeue buffers back to app which are in capture queue */
		tegra_channel_queued_buf_done(chan, VB2_BUF_STATE_ERROR);
	}

	if (!chan->vi->pg_mode) {
		tegra_channel_set_stream(chan, false);
		media_entity_pipeline_stop(&chan->video.entity);
		tegra_mipi_bias_pad_disable();
	}

	if (!chan->bypass)
		tegra_channel_update_clknbw(chan, 0);

	return 0;
}

int tegra_vi4_power_on(struct tegra_mc_vi *vi)
{
	int ret;

	ret = nvhost_module_busy(vi->ndev);
	if (ret) {
		dev_err(vi->dev, "%s:nvhost module is busy\n", __func__);
		return ret;
	}

	ret = tegra_camera_emc_clk_enable();
	if (ret)
		goto err_emc_enable;

	return 0;

err_emc_enable:
	nvhost_module_idle(vi->ndev);

	return ret;
}

void tegra_vi4_power_off(struct tegra_mc_vi *vi)
{
	tegra_channel_ec_close(vi);
	tegra_camera_emc_clk_disable();
	nvhost_module_idle(vi->ndev);
}

int vi4_power_on(struct tegra_channel *chan)
{
	int ret = 0;
	struct tegra_mc_vi *vi;
	struct tegra_csi_device *csi;

	vi = chan->vi;
	csi = vi->csi;

	/* Use chan->video as identifier of vi4 nvhost_module client
	 * since they are unique per channel
	 */
	ret = nvhost_module_add_client(vi->ndev, &chan->video);
	if (ret)
		return ret;
	tegra_vi4_power_on(vi);

	if (!vi->pg_mode &&
		(atomic_add_return(1, &chan->power_on_refcnt) == 1)) {
		ret = tegra_channel_set_power(chan, 1);
	}

	return ret;
}

void vi4_power_off(struct tegra_channel *chan)
{
	int ret = 0;
	struct tegra_mc_vi *vi;
	struct tegra_csi_device *csi;

	vi = chan->vi;
	csi = vi->csi;

	if (!vi->pg_mode &&
		atomic_dec_and_test(&chan->power_on_refcnt)) {
		ret = tegra_channel_set_power(chan, 0);
		if (ret < 0)
			dev_err(vi->dev, "Failed to power off subdevices\n");
	}

	tegra_vi4_power_off(vi);
	nvhost_module_remove_client(vi->ndev, &chan->video);
}

static void tegra_channel_notify_error_callback(void *client_data)
{
	struct tegra_channel *chan = (struct tegra_channel *)client_data;

	spin_lock(&chan->capture_state_lock);
	if (chan->capture_state == CAPTURE_GOOD)
		chan->capture_state = CAPTURE_ERROR;
	else {
		spin_unlock(&chan->capture_state_lock);
		return;
	}
	spin_unlock(&chan->capture_state_lock);

	vi4_power_off(chan);
	tegra_channel_handle_error(chan);
}

