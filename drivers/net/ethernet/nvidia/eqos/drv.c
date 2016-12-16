/* =========================================================================
 * The Synopsys DWC ETHER QOS Software Driver and documentation (hereinafter
 * "Software") is an unsupported proprietary work of Synopsys, Inc. unless
 * otherwise expressly agreed to in writing between Synopsys and you.
 *
 * The Software IS NOT an item of Licensed Software or Licensed Product under
 * any End User Software License Agreement or Agreement for Licensed Product
 * with Synopsys or any supplement thereto.  Permission is hereby granted,
 * free of charge, to any person obtaining a copy of this software annotated
 * with this license and the Software, to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THIS SOFTWARE IS BEING DISTRIBUTED BY SYNOPSYS SOLELY ON AN "AS IS" BASIS
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE HEREBY DISCLAIMED. IN NO EVENT SHALL SYNOPSYS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * ========================================================================= */
/*
 * Copyright (c) 2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
/*!@file: eqos_drv.c
 * @brief: Driver functions.
 */

#include "yheader.h"
#include "yapphdr.h"
#include "drv.h"

extern ULONG eqos_base_addr;
#include "yregacc.h"
#include "nvregacc.h"
#include <linux/inet_lro.h>
#include <linux/tegra-soc.h>

static INT eqos_status;
static void eqos_poll_timer(unsigned long data);
static void eqos_all_chans_timer(unsigned long data);
static int handle_txrx_completions(struct eqos_prv_data *pdata,
					int qinx);

static void eqos_stop_dev(struct eqos_prv_data *pdata);
static void eqos_start_dev(struct eqos_prv_data *pdata);

/* SA(Source Address) operations on TX */
unsigned char mac_addr0[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
unsigned char mac_addr1[6] = { 0x00, 0x66, 0x77, 0x88, 0x99, 0xaa };

/* module parameters for configuring the queue modes
 * set default mode as GENERIC
 * */
/* Value of "2" enables mtl tx q */
static int q_op_mode[EQOS_MAX_TX_QUEUE_CNT] = {
	2,
	2,
	2,
	2,
	2,
	2,
	2,
	2
};
module_param_array(q_op_mode, int, NULL, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(q_op_mode,
	"MTL queue operation mode [0-DISABLED, 1-AVB, 2-DCB, 3-GENERIC]");

void eqos_stop_all_ch_tx_dma(struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT qinx;

	DBGPR("-->eqos_stop_all_ch_tx_dma\n");

	for (qinx = 0; qinx < EQOS_TX_QUEUE_CNT; qinx++)
		hw_if->stop_dma_tx(pdata, qinx);

	DBGPR("<--eqos_stop_all_ch_tx_dma\n");
}

static int is_ptp_addr(char *addr)
{
	if ((addr[0] == PTP1_MAC0) &&
		(addr[1] == PTP1_MAC1) &&
		(addr[2] == PTP1_MAC2) &&
		(addr[3] == PTP1_MAC3) &&
		(addr[4] == PTP1_MAC4) &&
		(addr[5] == PTP1_MAC5))
		return 1;
	else if ((addr[0] == PTP2_MAC0) &&
		(addr[1] == PTP2_MAC1) &&
		(addr[2] == PTP2_MAC2) &&
		(addr[3] == PTP2_MAC3) &&
		(addr[4] == PTP2_MAC4) &&
		(addr[5] == PTP2_MAC5))
		return 1;
	else
		return 0;
}

/*Check if Channel 0 is PTP and has data 0xee
  Check if Channel 1 is AV and has data 0xbb or 0xcc
  Check if Channel 2 is AV and has data 0xdd*/
#ifdef ENABLE_CHANNEL_DATA_CHECK
static void check_channel_data(struct sk_buff *skb, unsigned int qinx, int is_rx)
{
	if (((qinx == 0) && ((*(((short *)skb->data) + 6)  & 0xFFFF) == 0xF788) &&
				((*(((char *)skb->data) + 80) & 0xFF) != 0xee)) ||
	   ((qinx == 1) && ((*(((short *)skb->data) + 6)  & 0xFFFF) == 0xF022) &&
			(((*(((char *)skb->data) + 80) & 0xFF) != 0xbb) &&
			 ((*(((char *)skb->data) + 80) & 0xFF) != 0xcc))) ||
	   ((qinx == 2) && ((*(((short *)skb->data) + 6) & 0xFFFF) == 0xF022) &&
			((*(((char *)skb->data) + 80) & 0xFF) != 0xdd))) {
			while (1)
				printk(KERN_ALERT "Incorrect %s data 0x%x in Q %d\n",
						((is_rx) ? "RX" : "TX"),*(((char *)skb->data) + 80), qinx);
	}
}
#endif

static void eqos_stop_all_ch_rx_dma(struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT qinx;

	DBGPR("-->eqos_stop_all_ch_rx_dma\n");

	for(qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++)
		hw_if->stop_dma_rx(qinx);

	DBGPR("<--eqos_stop_all_ch_rx_dma\n");
}

static void eqos_start_all_ch_tx_dma(struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT i;

	DBGPR("-->eqos_start_all_ch_tx_dma\n");

	for(i = 0; i < EQOS_TX_QUEUE_CNT; i++)
		hw_if->start_dma_tx(i);

	DBGPR("<--eqos_start_all_ch_tx_dma\n");
}

static void eqos_start_all_ch_rx_dma(struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT i;

	DBGPR("-->eqos_start_all_ch_rx_dma\n");

	for(i = 0; i < EQOS_RX_QUEUE_CNT; i++)
		hw_if->start_dma_rx(i);

	DBGPR("<--eqos_start_all_ch_rx_dma\n");
}

static void eqos_napi_enable_mq(struct eqos_prv_data *pdata)
{
	struct eqos_rx_queue *rx_queue = NULL;
	int qinx;

	DBGPR("-->eqos_napi_enable_mq\n");

	for (qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++) {
		rx_queue = GET_RX_QUEUE_PTR(qinx);
		napi_enable(&rx_queue->napi);
	}

	DBGPR("<--eqos_napi_enable_mq\n");
}

static void eqos_all_ch_napi_disable(struct eqos_prv_data *pdata)
{
	struct eqos_rx_queue *rx_queue = NULL;
	int qinx;

	DBGPR("-->eqos_napi_disable\n");

	for (qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++) {
		rx_queue = GET_RX_QUEUE_PTR(qinx);
		napi_disable(&rx_queue->napi);
	}

	DBGPR("<--eqos_napi_disable\n");
}


void eqos_disable_all_ch_rx_interrpt(
			struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT qinx;

	DBGPR("-->eqos_disable_all_ch_rx_interrpt\n");

	for (qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++)
		hw_if->disable_rx_interrupt(qinx, pdata);

	DBGPR("<--eqos_disable_all_ch_rx_interrpt\n");
}

void eqos_enable_all_ch_rx_interrpt(
			struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT qinx;

	DBGPR("-->eqos_enable_all_ch_rx_interrpt\n");

	for (qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++)
		hw_if->enable_rx_interrupt(qinx, pdata);

	DBGPR("<--eqos_enable_all_ch_rx_interrpt\n");
}


/* pnapi_sched is only needed for intr_mode=COMMON_IRQ.  In COMMON_IRQ,
 * there is one napi handler to process all queues.  So we need to ensure
 * we only schedule napi once.
 * For MULTI_IRQ, each IRQ has own napi handler.
 */
void handle_non_ti_ri_chan_intrs(struct eqos_prv_data *pdata, int qinx)
{
	ULONG dma_sr;
	ULONG dma_ier;

	DBGPR("-->%s(), chan=%d\n", __func__, qinx);

	DMA_SR_RD(qinx, dma_sr);

	DMA_IER_RD(qinx, dma_ier);

	DBGPR("DMA_SR[%d] = %#lx, DMA_IER= %#lx\n", qinx, dma_sr,
		dma_ier);

	/*on ufpga, update of DMA_IER is really slow, such that interrupt
	 * would happen, but read of IER returns old value.  This would
	 * cause driver to return when there really was an interrupt asserted.
	 * so for now, comment this out.
	 */
	/* process only those interrupts which we
	 * have enabled.
	 */
	if (!(tegra_platform_is_unit_fpga()))
		dma_sr = (dma_sr & dma_ier);

	/* mask off ri and ti */
	dma_sr &= ~(((0x1) << 6) | 1);

	if (dma_sr == 0)
		return;

	/* ack non ti/ri ints */
	DMA_SR_WR(qinx, dma_sr);

	if ((GET_VALUE(dma_sr, DMA_SR_RBU_LPOS, DMA_SR_RBU_HPOS) & 1))
		pdata->xstats.rx_buf_unavailable_irq_n[qinx]++;

	if (tegra_platform_is_unit_fpga())
		dma_sr = (dma_sr & dma_ier);

	if (GET_VALUE(dma_sr, DMA_SR_TPS_LPOS, DMA_SR_TPS_HPOS) & 1) {
		pdata->xstats.tx_process_stopped_irq_n[qinx]++;
		eqos_status = -E_DMA_SR_TPS;
	}
	if (GET_VALUE(dma_sr, DMA_SR_TBU_LPOS, DMA_SR_TBU_HPOS) & 1) {
		pdata->xstats.tx_buf_unavailable_irq_n[qinx]++;
		eqos_status = -E_DMA_SR_TBU;
	}
	if (GET_VALUE(dma_sr, DMA_SR_RPS_LPOS, DMA_SR_RPS_HPOS) & 1) {
		pdata->xstats.rx_process_stopped_irq_n[qinx]++;
		eqos_status = -E_DMA_SR_RPS;
	}
	if (GET_VALUE(dma_sr, DMA_SR_RWT_LPOS, DMA_SR_RWT_HPOS) & 1) {
		pdata->xstats.rx_watchdog_irq_n++;
		eqos_status = S_DMA_SR_RWT;
	}
	if (GET_VALUE(dma_sr, DMA_SR_FBE_LPOS, DMA_SR_FBE_HPOS) & 1) {
		pdata->xstats.fatal_bus_error_irq_n++;
		pdata->fbe_chan_mask |= (1 << qinx);
		eqos_status = -E_DMA_SR_FBE;
		queue_work(pdata->fbe_wq, &pdata->fbe_work);
	}

	DBGPR("<--%s()\n", __func__);
}


/* pnapi_sched is only needed for intr_mode=COMMON_IRQ.  In COMMON_IRQ,
 * there is one napi handler to process all queues.  So we need to ensure
 * we only schedule napi once.
 * For MULTI_IRQ, each IRQ has own napi handler.
 */
void handle_ti_ri_chan_intrs(struct eqos_prv_data *pdata,
			int qinx, int *pnapi_sched)
{
	ULONG dma_sr;
	ULONG dma_ier;
	u32 ch_crtl_reg;
	u32 ch_stat_reg;
	struct hw_if_struct *hw_if = &(pdata->hw_if);

	struct eqos_rx_queue *rx_queue = NULL;

	DBGPR("-->%s(), chan=%d\n", __func__, qinx);

	rx_queue = GET_RX_QUEUE_PTR(qinx);

	DMA_SR_RD(qinx, dma_sr);

	DMA_IER_RD(qinx, dma_ier);
	VIRT_INTR_CH_STAT_RD(qinx, ch_stat_reg);
	VIRT_INTR_CH_CRTL_RD(qinx, ch_crtl_reg);

	DBGPR("DMA_SR[%d] = %#lx, DMA_IER= %#lx\n", qinx, dma_sr,
		dma_ier);

	DBGPR("VIRT_INTR_CH_STAT[%d] = %#lx, VIRT_INTR_CH_CRTL= %#lx\n",
		qinx, ch_stat_reg, ch_crtl_reg);

	/*on ufpga, update of DMA_IER is really slow, such that interrupt
	 * would happen, but read of IER returns old value.  This would
	 * cause driver to return when there really was an interrupt asserted.
	 * so for now, comment this out.
	 */
	/* process only those interrupts which we
	 * have enabled.
	 */
	if (!(tegra_platform_is_unit_fpga()))
		ch_stat_reg &= ch_crtl_reg;

	if (ch_stat_reg == 0)
		return;

	if (ch_stat_reg & VIRT_INTR_CH_CRTL_RX_WR_MASK) {
		DMA_SR_WR(qinx, ((0x1) << 6) | ((0x1) << 15));
		VIRT_INTR_CH_STAT_WR(qinx, VIRT_INTR_CH_CRTL_RX_WR_MASK);
		pdata->xstats.rx_normal_irq_n[qinx]++;
	}

	if (tegra_platform_is_unit_fpga())
		ch_stat_reg &= ch_crtl_reg;

	if (ch_stat_reg & VIRT_INTR_CH_CRTL_TX_WR_MASK) {
		DMA_SR_WR(qinx, ((0x1) << 0) | ((0x1) << 15));
		VIRT_INTR_CH_STAT_WR(qinx, VIRT_INTR_CH_CRTL_TX_WR_MASK);
		pdata->xstats.tx_normal_irq_n[qinx]++;
	}

	if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ) {
		switch (pdata->dt_cfg.chan_mode[qinx]) {
		case CHAN_MODE_NAPI:
			if (likely(napi_schedule_prep(&rx_queue->napi))) {
				hw_if->disable_chan_interrupts(qinx, pdata);
				__napi_schedule(&rx_queue->napi);
			} else {
				printk(KERN_ALERT
				"driver bug! Rx interrupt while in poll\n");
				hw_if->disable_chan_interrupts(qinx, pdata);
			}
			break;
		case CHAN_MODE_INTR:
			handle_txrx_completions(pdata, qinx);
			break;
		default:
			printk(KERN_ALERT "driver bug! rx interrupt when chan mode is polling\n");
		}
	} else {
		*pnapi_sched = 1;
		if (likely(napi_schedule_prep(&rx_queue->napi))) {
			eqos_disable_all_ch_rx_interrpt(pdata);
			__napi_schedule(&rx_queue->napi);
		} else {
			printk(KERN_ALERT "driver bug! Rx interrupt while in poll\n");
			eqos_disable_all_ch_rx_interrpt(pdata);
		}
	}

	DBGPR("<--%s()\n", __func__);
}

void handle_mac_intrs(struct eqos_prv_data *pdata,
			ULONG dma_isr)
{
	ULONG mac_imr;
	ULONG mac_pmtcsr;
	ULONG mac_ans = 0;
	ULONG mac_pcs = 0;
	ULONG mac_isr;
	struct net_device *dev = pdata->dev;

	DBGPR("-->%s()\n", __func__);

	MAC_ISR_RD(mac_isr);

	/* Handle MAC interrupts */
	if (GET_VALUE(dma_isr, DMA_ISR_MACIS_LPOS, DMA_ISR_MACIS_HPOS) & 1) {
		/* handle only those MAC interrupts which are enabled */
		MAC_IMR_RD(mac_imr);
		mac_isr = (mac_isr & mac_imr);

		/* PMT interrupt
		 * RemoteWake and MagicPacket events will be received by PHY supporting
		 * these features on silicon and can be used to wake up Tegra.
		 * Still let the below code be here in case we ever get this interrupt.
		 */
		if (GET_VALUE(mac_isr, MAC_ISR_PMTIS_LPOS, MAC_ISR_PMTIS_HPOS) & 1) {
			pdata->xstats.pmt_irq_n++;
			eqos_status = S_MAC_ISR_PMTIS;
			MAC_PMTCSR_RD(mac_pmtcsr);
			pr_info("commonisr: PMTCSR : %#lx\n", mac_pmtcsr);
			if (pdata->power_down)
				eqos_powerup(pdata->dev, EQOS_IOCTL_CONTEXT);
		}

		/* RGMII/SMII interrupt */
		if (GET_VALUE(mac_isr, MAC_ISR_RGSMIIS_LPOS, MAC_ISR_RGSMIIS_HPOS) & 1) {
			MAC_PCS_RD(mac_pcs);
			printk(KERN_ALERT "RGMII/SMII interrupt: MAC_PCS = %#lx\n", mac_pcs);
#ifdef HWA_NV_1637630

#else
			/* Comment out this block of code(1637630)
	 		 * as it was preventing 10mb to work.
			 */
			if ((mac_pcs & 0x80000) == 0x80000) {
				pdata->pcs_link = 1;
				netif_carrier_on(dev);
				if ((mac_pcs & 0x10000) == 0x10000) {
					pdata->pcs_duplex = 1;
					hw_if->set_full_duplex(); //TODO: may not be required
				} else {
					pdata->pcs_duplex = 0;
					hw_if->set_half_duplex(); //TODO: may not be required
				}

				if ((mac_pcs & 0x60000) == 0x0) {
					pdata->pcs_speed = SPEED_10;
					hw_if->set_mii_speed_10(); //TODO: may not be required
				} else if ((mac_pcs & 0x60000) == 0x20000) {
					pdata->pcs_speed = SPEED_100;
					hw_if->set_mii_speed_100(); //TODO: may not be required
				} else if ((mac_pcs & 0x60000) == 0x30000) {
					pdata->pcs_speed = SPEED_1000;
					hw_if->set_gmii_speed(); //TODO: may not be required
				}
				printk(KERN_ALERT "Link is UP:%dMbps & %s duplex\n",
					pdata->pcs_speed, pdata->pcs_duplex ? "Full" : "Half");
			} else {
				printk(KERN_ALERT "Link is Down\n");
				pdata->pcs_link = 0;
				netif_carrier_off(dev);
			}
#endif
		}

		/* PCS Link Status interrupt */
		if (GET_VALUE(mac_isr, MAC_ISR_PCSLCHGIS_LPOS, MAC_ISR_PCSLCHGIS_HPOS) & 1) {
			printk(KERN_ALERT "PCS Link Status interrupt\n");
			MAC_ANS_RD(mac_ans);
			if (GET_VALUE(mac_ans, MAC_ANS_LS_LPOS, MAC_ANS_LS_HPOS) & 1) {
				printk(KERN_ALERT "Link: Up\n");
				netif_carrier_on(dev);
				pdata->pcs_link = 1;
			} else {
				printk(KERN_ALERT "Link: Down\n");
				netif_carrier_off(dev);
				pdata->pcs_link = 0;
			}
		}

		/* PCS Auto-Negotiation Complete interrupt */
		if (GET_VALUE(mac_isr, MAC_ISR_PCSANCIS_LPOS, MAC_ISR_PCSANCIS_HPOS) & 1) {
			printk(KERN_ALERT "PCS Auto-Negotiation Complete interrupt\n");
			MAC_ANS_RD(mac_ans);
		}

		/* EEE interrupts */
		if (GET_VALUE(mac_isr, MAC_ISR_LPI_LPOS, MAC_ISR_LPI_HPOS) & 1) {
			eqos_handle_eee_interrupt(pdata);
		}
	}

	DBGPR("<--%s()\n", __func__);
}

/*!
* \brief Interrupt Service Routine
* \details Interrupt Service Routine
*
* \param[in] irq         - interrupt number for particular device
* \param[in] device_id   - pointer to device structure
* \return returns positive integer
* \retval IRQ_HANDLED
*/

irqreturn_t eqos_isr(int irq, void *device_id)
{
	ULONG dma_isr;
	struct eqos_prv_data *pdata =
	    (struct eqos_prv_data *)device_id;
	UINT qinx;
	int napi_sched = 0;

	DBGPR("-->eqos_isr\n");

	DMA_ISR_RD(dma_isr);
	if (dma_isr == 0x0)
		return IRQ_NONE;


	DBGPR("DMA_ISR = %#lx\n", dma_isr);

	/* Handle DMA interrupts */
#ifdef ALL_POLLING
	if (pdata->dt_cfg.intr_mode != MODE_ALL_POLLING)
#endif
		if (dma_isr & 0xf)
			for (qinx = 0; qinx < EQOS_TX_QUEUE_CNT;
					qinx++) {
				handle_ti_ri_chan_intrs(pdata, qinx,
					&napi_sched);
				handle_non_ti_ri_chan_intrs(pdata, qinx);
			}

	handle_mac_intrs(pdata, dma_isr);

	DBGPR("<--eqos_isr\n");

	return IRQ_HANDLED;

}


/*!
* Only used when multi irq is enabled
*/

irqreturn_t eqos_common_isr(int irq, void *device_id)
{
	ULONG dma_isr;
	struct eqos_prv_data *pdata =
	    (struct eqos_prv_data *)device_id;
	UINT qinx;

	DBGPR("-->%s()\n", __func__);

	DMA_ISR_RD(dma_isr);
	if (dma_isr == 0x0)
		return IRQ_NONE;

	DBGPR("DMA_ISR = %#lx\n", dma_isr);

	if (dma_isr & 0xf)
		for (qinx = 0; qinx < EQOS_TX_QUEUE_CNT; qinx++)
			handle_non_ti_ri_chan_intrs(pdata, qinx);

	handle_mac_intrs(pdata, dma_isr);

	DBGPR("<--%s()\n", __func__);

	return IRQ_HANDLED;

}


/* Only used when multi irq is enabled.
 * Will only handle tx/rx for one channel.
 */
irqreturn_t eqos_ch_isr(int irq, void *device_id)
{
	struct eqos_prv_data *pdata =
	    (struct eqos_prv_data *)device_id;
	uint i;
	UINT qinx;
	int napi_sched = 0;

	i = smp_processor_id();

	if (irq < pdata->rx_irqs[0])
		qinx = irq - pdata->tx_irqs[0];
	else
		qinx = irq - pdata->rx_irqs[0];

	DBGPR("-->%s(): cpu=%d, chan=%d\n", __func__, i, qinx);

	handle_ti_ri_chan_intrs(pdata, qinx, &napi_sched);

	DBGPR("<--%s()\n", __func__);

	return IRQ_HANDLED;

}

/*!
* \brief API to get all hw features.
*
* \details This function is used to check what are all the different
* features the device supports.
*
* \param[in] pdata - pointer to driver private structure
*
* \return none
*/

void eqos_get_all_hw_features(struct eqos_prv_data *pdata)
{
	unsigned int varMAC_HFR0;
	unsigned int mac_hfr1;
	unsigned int varMAC_HFR2;

	DBGPR("-->eqos_get_all_hw_features\n");

	MAC_HFR0_RD(varMAC_HFR0);
	MAC_HFR1_RD(mac_hfr1);
	MAC_HFR2_RD(varMAC_HFR2);

	memset(&pdata->hw_feat, 0, sizeof(pdata->hw_feat));
	pdata->hw_feat.mii_sel = ((varMAC_HFR0 >> 0) & MAC_HFR0_MIISEL_MASK);
	pdata->hw_feat.gmii_sel = ((varMAC_HFR0 >> 1) & MAC_HFR0_GMIISEL_MASK);
	pdata->hw_feat.hd_sel = ((varMAC_HFR0 >> 2) & MAC_HFR0_HDSEL_MASK);
	pdata->hw_feat.pcs_sel = ((varMAC_HFR0 >> 3) & MAC_HFR0_PCSSEL_MASK);
#ifdef ENABLE_VLAN_FILTER
	pdata->hw_feat.vlan_hash_en =
	    ((varMAC_HFR0 >> 4) & MAC_HFR0_VLANHASEL_MASK);
#else
	pdata->hw_feat.vlan_hash_en = 0;
#endif
	pdata->hw_feat.sma_sel = ((varMAC_HFR0 >> 5) & MAC_HFR0_SMASEL_MASK);
	pdata->hw_feat.rwk_sel = ((varMAC_HFR0 >> 6) & MAC_HFR0_RWKSEL_MASK);
	pdata->hw_feat.mgk_sel = ((varMAC_HFR0 >> 7) & MAC_HFR0_MGKSEL_MASK);
	pdata->hw_feat.mmc_sel = ((varMAC_HFR0 >> 8) & MAC_HFR0_MMCSEL_MASK);
	pdata->hw_feat.arp_offld_en =
	    ((varMAC_HFR0 >> 9) & MAC_HFR0_ARPOFFLDEN_MASK);
	pdata->hw_feat.ts_sel =
	    ((varMAC_HFR0 >> 12) & MAC_HFR0_TSSSEL_MASK);
	pdata->hw_feat.eee_sel = ((varMAC_HFR0 >> 13) & MAC_HFR0_EEESEL_MASK);
	pdata->hw_feat.tx_coe_sel =
	    ((varMAC_HFR0 >> 14) & MAC_HFR0_TXCOESEL_MASK);
	pdata->hw_feat.rx_coe_sel =
	    ((varMAC_HFR0 >> 16) & MAC_HFR0_RXCOE_MASK);
	pdata->hw_feat.mac_addr16_sel =
	    ((varMAC_HFR0 >> 18) & MAC_HFR0_ADDMACADRSEL_MASK);
	pdata->hw_feat.mac_addr32_sel =
	    ((varMAC_HFR0 >> 23) & MAC_HFR0_MACADR32SEL_MASK);
	pdata->hw_feat.mac_addr64_sel =
	    ((varMAC_HFR0 >> 24) & MAC_HFR0_MACADR64SEL_MASK);
	pdata->hw_feat.tsstssel =
	    ((varMAC_HFR0 >> 25) & MAC_HFR0_TSINTSEL_MASK);
	pdata->hw_feat.sa_vlan_ins =
	    ((varMAC_HFR0 >> 27) & MAC_HFR0_SAVLANINS_MASK);
	pdata->hw_feat.act_phy_sel =
	    ((varMAC_HFR0 >> 28) & MAC_HFR0_ACTPHYSEL_MASK);

	pdata->hw_feat.rx_fifo_size =
	    ((mac_hfr1 >> 0) & MAC_HFR1_RXFIFOSIZE_MASK);
	    //8;
	pdata->hw_feat.tx_fifo_size =
	    ((mac_hfr1 >> 6) & MAC_HFR1_TXFIFOSIZE_MASK);
	    //8;
	pdata->hw_feat.adv_ts_hword =
	    ((mac_hfr1 >> 13) & MAC_HFR1_ADVTHWORD_MASK);
	pdata->hw_feat.dcb_en = ((mac_hfr1 >> 16) & MAC_HFR1_DCBEN_MASK);
	pdata->hw_feat.sph_en = ((mac_hfr1 >> 17) & MAC_HFR1_SPHEN_MASK);
	pdata->hw_feat.tso_en = ((mac_hfr1 >> 18) & MAC_HFR1_TSOEN_MASK);
	pdata->hw_feat.dma_debug_gen =
	    ((mac_hfr1 >> 19) & MAC_HFR1_DMADEBUGEN_MASK);
	pdata->hw_feat.av_sel = ((mac_hfr1 >> 20) & MAC_HFR1_AVSEL_MASK);
	pdata->hw_feat.lp_mode_en =
	    ((mac_hfr1 >> 23) & MAC_HFR1_LPMODEEN_MASK);
#ifdef ENABLE_PERFECT_L2_FILTER
	pdata->hw_feat.hash_tbl_sz = 0;
#else
	pdata->hw_feat.hash_tbl_sz =
	    ((mac_hfr1 >> 24) & MAC_HFR1_HASHTBLSZ_MASK);
#endif
	pdata->hw_feat.l3l4_filter_num =
	    ((mac_hfr1 >> 27) & MAC_HFR1_L3L4FILTERNUM_MASK);

	pdata->hw_feat.rx_q_cnt = ((varMAC_HFR2 >> 0) & MAC_HFR2_RXQCNT_MASK);
	pdata->hw_feat.tx_q_cnt = ((varMAC_HFR2 >> 6) & MAC_HFR2_TXQCNT_MASK);
	pdata->hw_feat.rx_ch_cnt =
	    ((varMAC_HFR2 >> 12) & MAC_HFR2_RXCHCNT_MASK);
	pdata->hw_feat.tx_ch_cnt =
	    ((varMAC_HFR2 >> 18) & MAC_HFR2_TXCHCNT_MASK);
	pdata->hw_feat.pps_out_num =
	    ((varMAC_HFR2 >> 24) & MAC_HFR2_PPSOUTNUM_MASK);
	pdata->hw_feat.aux_snap_num =
	    ((varMAC_HFR2 >> 28) & MAC_HFR2_AUXSNAPNUM_MASK);

	DBGPR("<--eqos_get_all_hw_features\n");
}


/*!
* \brief API to print all hw features.
*
* \details This function is used to print all the device feature.
*
* \param[in] pdata - pointer to driver private structure
*
* \return none
*/

void eqos_print_all_hw_features(struct eqos_prv_data *pdata)
{
	char *str = NULL;

	DBGPR("-->eqos_print_all_hw_features\n");

	printk(KERN_ALERT "\n");
	printk(KERN_ALERT "=====================================================/\n");
	printk(KERN_ALERT "\n");
	printk(KERN_ALERT "10/100 Mbps Support                         : %s\n",
		pdata->hw_feat.mii_sel ? "YES" : "NO");
	printk(KERN_ALERT "1000 Mbps Support                           : %s\n",
		pdata->hw_feat.gmii_sel ? "YES" : "NO");
	printk(KERN_ALERT "Half-duplex Support                         : %s\n",
		pdata->hw_feat.hd_sel ? "YES" : "NO");
	printk(KERN_ALERT "PCS Registers(TBI/SGMII/RTBI PHY interface) : %s\n",
		pdata->hw_feat.pcs_sel ? "YES" : "NO");
	printk(KERN_ALERT "VLAN Hash Filter Selected                   : %s\n",
		pdata->hw_feat.vlan_hash_en ? "YES" : "NO");
	pdata->vlan_hash_filtering = pdata->hw_feat.vlan_hash_en;
	printk(KERN_ALERT "SMA (MDIO) Interface                        : %s\n",
		pdata->hw_feat.sma_sel ? "YES" : "NO");
	printk(KERN_ALERT "PMT Remote Wake-up Packet Enable            : %s\n",
		pdata->hw_feat.rwk_sel ? "YES" : "NO");
	printk(KERN_ALERT "PMT Magic Packet Enable                     : %s\n",
		pdata->hw_feat.mgk_sel ? "YES" : "NO");
	printk(KERN_ALERT "RMON/MMC Module Enable                      : %s\n",
		pdata->hw_feat.mmc_sel ? "YES" : "NO");
	printk(KERN_ALERT "ARP Offload Enabled                         : %s\n",
		pdata->hw_feat.arp_offld_en ? "YES" : "NO");
	printk(KERN_ALERT "IEEE 1588-2008 Timestamp Enabled            : %s\n",
		pdata->hw_feat.ts_sel ? "YES" : "NO");
	printk(KERN_ALERT "Energy Efficient Ethernet Enabled           : %s\n",
		pdata->hw_feat.eee_sel ? "YES" : "NO");
	printk(KERN_ALERT "Transmit Checksum Offload Enabled           : %s\n",
		pdata->hw_feat.tx_coe_sel ? "YES" : "NO");
	printk(KERN_ALERT "Receive Checksum Offload Enabled            : %s\n",
		pdata->hw_feat.rx_coe_sel ? "YES" : "NO");
	printk(KERN_ALERT "MAC Addresses 16–31 Selected                : %s\n",
		pdata->hw_feat.mac_addr16_sel ? "YES" : "NO");
	printk(KERN_ALERT "MAC Addresses 32–63 Selected                : %s\n",
		pdata->hw_feat.mac_addr32_sel ? "YES" : "NO");
	printk(KERN_ALERT "MAC Addresses 64–127 Selected               : %s\n",
		pdata->hw_feat.mac_addr64_sel ? "YES" : "NO");

	if (pdata->hw_feat.mac_addr64_sel)
		pdata->max_addr_reg_cnt = 128;
	else if (pdata->hw_feat.mac_addr32_sel)
		pdata->max_addr_reg_cnt = 64;
	else if (pdata->hw_feat.mac_addr16_sel)
		pdata->max_addr_reg_cnt = 32;
	else
		pdata->max_addr_reg_cnt = 1;

	switch(pdata->hw_feat.tsstssel) {
	case 0:
		str = "RESERVED";
		break;
	case 1:
		str = "INTERNAL";
		break;
	case 2:
		str = "EXTERNAL";
		break;
	case 3:
		str = "BOTH";
		break;
	}
	printk(KERN_ALERT "Timestamp System Time Source                : %s\n",
		str);
	printk(KERN_ALERT "Source Address or VLAN Insertion Enable     : %s\n",
		pdata->hw_feat.sa_vlan_ins ? "YES" : "NO");

	switch (pdata->hw_feat.act_phy_sel) {
	case 0:
		str = "GMII/MII";
		break;
	case 1:
		str = "RGMII";
		break;
	case 2:
		str = "SGMII";
		break;
	case 3:
		str = "TBI";
		break;
	case 4:
		str = "RMII";
		break;
	case 5:
		str = "RTBI";
		break;
	case 6:
		str = "SMII";
		break;
	case 7:
		str = "RevMII";
		break;
	default:
		str = "RESERVED";
	}
	printk(KERN_ALERT "Active PHY Selected                         : %s\n",
		str);

	switch(pdata->hw_feat.rx_fifo_size) {
	case 0:
		str = "128 bytes";
		break;
	case 1:
		str = "256 bytes";
		break;
	case 2:
		str = "512 bytes";
		break;
	case 3:
		str = "1 KBytes";
		break;
	case 4:
		str = "2 KBytes";
		break;
	case 5:
		str = "4 KBytes";
		break;
	case 6:
		str = "8 KBytes";
		break;
	case 7:
		str = "16 KBytes";
		break;
	case 8:
		str = "32 kBytes";
		break;
	case 9:
		str = "64 KBytes";
		break;
	case 10:
		str = "128 KBytes";
		break;
	case 11:
		str = "256 KBytes";
		break;
	default:
		str = "RESERVED";
	}
	printk(KERN_ALERT "MTL Receive FIFO Size                       : %s\n",
		str);

	switch(pdata->hw_feat.tx_fifo_size) {
	case 0:
		str = "128 bytes";
		break;
	case 1:
		str = "256 bytes";
		break;
	case 2:
		str = "512 bytes";
		break;
	case 3:
		str = "1 KBytes";
		break;
	case 4:
		str = "2 KBytes";
		break;
	case 5:
		str = "4 KBytes";
		break;
	case 6:
		str = "8 KBytes";
		break;
	case 7:
		str = "16 KBytes";
		break;
	case 8:
		str = "32 kBytes";
		break;
	case 9:
		str = "64 KBytes";
		break;
	case 10:
		str = "128 KBytes";
		break;
	case 11:
		str = "256 KBytes";
		break;
	default:
		str = "RESERVED";
	}
	printk(KERN_ALERT "MTL Transmit FIFO Size                       : %s\n",
		str);
	printk(KERN_ALERT "IEEE 1588 High Word Register Enable          : %s\n",
		pdata->hw_feat.adv_ts_hword ? "YES" : "NO");
	printk(KERN_ALERT "DCB Feature Enable                           : %s\n",
		pdata->hw_feat.dcb_en ? "YES" : "NO");
	printk(KERN_ALERT "Split Header Feature Enable                  : %s\n",
		pdata->hw_feat.sph_en ? "YES" : "NO");
	printk(KERN_ALERT "TCP Segmentation Offload Enable              : %s\n",
		pdata->hw_feat.tso_en ? "YES" : "NO");
	printk(KERN_ALERT "DMA Debug Registers Enabled                  : %s\n",
		pdata->hw_feat.dma_debug_gen ? "YES" : "NO");
	printk(KERN_ALERT "AV Feature Enabled                           : %s\n",
		pdata->hw_feat.av_sel ? "YES" : "NO");
	printk(KERN_ALERT "Low Power Mode Enabled                       : %s\n",
		pdata->hw_feat.lp_mode_en ? "YES" : "NO");

	switch(pdata->hw_feat.hash_tbl_sz) {
	case 0:
		str = "No hash table selected";
		pdata->max_hash_table_size = 0;
		break;
	case 1:
		str = "64";
		pdata->max_hash_table_size = 64;
		break;
	case 2:
		str = "128";
		pdata->max_hash_table_size = 128;
		break;
	case 3:
		str = "256";
		pdata->max_hash_table_size = 256;
		break;
	}
	printk(KERN_ALERT "Hash Table Size                              : %s\n",
		str);
	printk(KERN_ALERT "Total number of L3 or L4 Filters             : %d L3/L4 Filter\n",
		pdata->hw_feat.l3l4_filter_num);
	printk(KERN_ALERT "Number of MTL Receive Queues                 : %d\n",
		(pdata->hw_feat.rx_q_cnt + 1));
	printk(KERN_ALERT "Number of MTL Transmit Queues                : %d\n",
		(pdata->hw_feat.tx_q_cnt + 1));
	printk(KERN_ALERT "Number of DMA Receive Channels               : %d\n",
		(pdata->hw_feat.rx_ch_cnt + 1));
	printk(KERN_ALERT "Number of DMA Transmit Channels              : %d\n",
		(pdata->hw_feat.tx_ch_cnt + 1));

	switch(pdata->hw_feat.pps_out_num) {
	case 0:
		str = "No PPS output";
		break;
	case 1:
		str = "1 PPS output";
		break;
	case 2:
		str = "2 PPS output";
		break;
	case 3:
		str = "3 PPS output";
		break;
	case 4:
		str = "4 PPS output";
		break;
	default:
		str = "RESERVED";
	}
	printk(KERN_ALERT "Number of PPS Outputs                        : %s\n",
		str);

	switch(pdata->hw_feat.aux_snap_num) {
	case 0:
		str = "No auxillary input";
		break;
	case 1:
		str = "1 auxillary input";
		break;
	case 2:
		str = "2 auxillary input";
		break;
	case 3:
		str = "3 auxillary input";
		break;
	case 4:
		str = "4 auxillary input";
		break;
	default:
		str = "RESERVED";
	}
	printk(KERN_ALERT "Number of Auxiliary Snapshot Inputs          : %s",
		str);

	printk(KERN_ALERT "\n");
	printk(KERN_ALERT "=====================================================/\n");

	DBGPR("<--eqos_print_all_hw_features\n");
}

/*!
 * \brief allcation of Rx skb's for split header feature.
 *
 * \details This function is invoked by other api's for
 * allocating the Rx skb's if split header feature is enabled.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] buffer – pointer to wrapper receive buffer data structure.
 * \param[in] gfp – the type of memory allocation.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */

static int eqos_alloc_split_hdr_rx_buf(
		struct eqos_prv_data *pdata,
		struct eqos_rx_buffer *buffer,
		gfp_t gfp)
{
	struct sk_buff *skb = buffer->skb;

	DBGPR("-->eqos_alloc_split_hdr_rx_buf\n");

	if (skb) {
		skb_trim(skb, 0);
		goto check_page;
	}

	buffer->rx_hdr_size = EQOS_MAX_HDR_SIZE;
	/* allocate twice the maximum header size */
	skb = __netdev_alloc_skb_ip_align(pdata->dev,
			(2*buffer->rx_hdr_size),
			gfp);
	if (skb == NULL) {
		printk(KERN_ALERT "Failed to allocate skb\n");
		return -ENOMEM;
	}
	buffer->skb = skb;
	DBGPR("Maximum header buffer size allocated = %d\n",
		buffer->rx_hdr_size);
 check_page:
	if (!buffer->dma)
		buffer->dma = dma_map_single(&pdata->pdev->dev,
					buffer->skb->data,
					ALIGN_SIZE(2*buffer->rx_hdr_size),
					DMA_FROM_DEVICE);
	buffer->len = buffer->rx_hdr_size;

	/* allocate a new page if necessary */
	if (buffer->page2 == NULL) {
		buffer->page2 = alloc_page(gfp);
		if (unlikely(!buffer->page2)) {
			printk(KERN_ALERT
			"Failed to allocate page for second buffer\n");
			return -ENOMEM;
		}
	}
	if (!buffer->dma2)
		buffer->dma2 = dma_map_page(&pdata->pdev->dev,
				    buffer->page2, 0,
				    PAGE_SIZE, DMA_FROM_DEVICE);
	buffer->len2 = PAGE_SIZE;
	buffer->mapped_as_page = Y_TRUE;

	DBGPR("<--eqos_alloc_split_hdr_rx_buf\n");

	return 0;
}

/*!
 * \brief allcation of Rx skb's for jumbo frame.
 *
 * \details This function is invoked by other api's for
 * allocating the Rx skb's if jumbo frame is enabled.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] buffer – pointer to wrapper receive buffer data structure.
 * \param[in] gfp – the type of memory allocation.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */

static int eqos_alloc_jumbo_rx_buf(struct eqos_prv_data *pdata,
					  struct eqos_rx_buffer *buffer,
					  gfp_t gfp)
{
	struct sk_buff *skb = buffer->skb;
	unsigned int bufsz = (256 - 16);	/* for skb_reserve */

	DBGPR("-->eqos_alloc_jumbo_rx_buf\n");

	if (skb) {
		skb_trim(skb, 0);
		goto check_page;
	}

	skb = __netdev_alloc_skb_ip_align(pdata->dev, bufsz, gfp);
	if (skb == NULL) {
		printk(KERN_ALERT "Failed to allocate skb\n");
		return -ENOMEM;
	}
	buffer->skb = skb;
 check_page:
	/* allocate a new page if necessary */
	if (buffer->page == NULL) {
		buffer->page = alloc_page(gfp);
		if (unlikely(!buffer->page)) {
			printk(KERN_ALERT "Failed to allocate page\n");
			return -ENOMEM;
		}
	}
	if (!buffer->dma)
		buffer->dma = dma_map_page(&pdata->pdev->dev,
					   buffer->page, 0,
					   PAGE_SIZE, DMA_FROM_DEVICE);
	buffer->len = PAGE_SIZE;
#ifdef EQOS_DMA_32BIT
	if (buffer->page2 == NULL) {
		buffer->page2 = alloc_page(gfp);
		if (unlikely(!buffer->page2)) {
			printk(KERN_ALERT
			       "Failed to allocate page for second buffer\n");
			return -ENOMEM;
		}
	}
	if (!buffer->dma2)
		buffer->dma2 = dma_map_page(&pdata->pdev->dev,
					    buffer->page2, 0,
					    PAGE_SIZE, DMA_FROM_DEVICE);
	buffer->len2 = PAGE_SIZE;
#else
	buffer->len2 = 0;
#endif

	buffer->mapped_as_page = Y_TRUE;

	DBGPR("<--eqos_alloc_jumbo_rx_buf\n");

	return 0;
}

/*!
 * \brief allcation of Rx skb's for default rx mode.
 *
 * \details This function is invoked by other api's for
 * allocating the Rx skb's with default Rx mode ie non-jumbo
 * and non-split header mode.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] buffer – pointer to wrapper receive buffer data structure.
 * \param[in] gfp – the type of memory allocation.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */

static int eqos_alloc_rx_buf(struct eqos_prv_data *pdata,
				    struct eqos_rx_buffer *buffer,
				    gfp_t gfp)
{
	struct sk_buff *skb = buffer->skb;

	DBGPR("-->eqos_alloc_rx_buf\n");

	if (skb) {
		skb_trim(skb, 0);
		goto map_skb;
	}

	skb = __netdev_alloc_skb_ip_align(pdata->dev, pdata->rx_buffer_len, gfp);
	if (skb == NULL) {
		printk(KERN_ALERT "Failed to allocate skb\n");
		return -ENOMEM;
	}
	buffer->skb = skb;
	buffer->len = pdata->rx_buffer_len;
 map_skb:
	buffer->dma = dma_map_single(&pdata->pdev->dev, skb->data,
				     ALIGN_SIZE(pdata->rx_buffer_len),
				     DMA_FROM_DEVICE);

	if (dma_mapping_error(&pdata->pdev->dev, buffer->dma))
		printk(KERN_ALERT "failed to do the RX dma map\n");

	buffer->mapped_as_page = Y_FALSE;

	DBGPR("<--eqos_alloc_rx_buf\n");

	return 0;
}


/*!
 * \brief api to configure Rx function pointer after reset.
 *
 * \details This function will initialize the receive function pointers
 * which are used for allocating skb's and receiving the packets based
 * Rx mode - default/jumbo/split header.
 *
 * \param[in] pdata – pointer to private data structure.
 *
 * \return void
 */

static void eqos_configure_rx_fun_ptr(struct eqos_prv_data *pdata)
{
	DBGPR("-->eqos_configure_rx_fun_ptr\n");

	if (pdata->rx_split_hdr) {
		pdata->clean_rx = eqos_clean_split_hdr_rx_irq;
		pdata->alloc_rx_buf = eqos_alloc_split_hdr_rx_buf;
	}
	else if (pdata->dev->mtu > EQOS_ETH_FRAME_LEN) {
		pdata->clean_rx = eqos_clean_jumbo_rx_irq;
		pdata->alloc_rx_buf = eqos_alloc_jumbo_rx_buf;
	} else {
		pdata->rx_buffer_len = EQOS_ETH_FRAME_LEN;
		pdata->clean_rx = eqos_clean_rx_irq;
		pdata->alloc_rx_buf = eqos_alloc_rx_buf;
	}

	DBGPR("<--eqos_configure_rx_fun_ptr\n");
}


/*!
 * \brief api to initialize default values.
 *
 * \details This function is used to initialize differnet parameters to
 * default values which are common parameters between Tx and Rx path.
 *
 * \param[in] pdata – pointer to private data structure.
 *
 * \return void
 */

static void eqos_default_common_confs(struct eqos_prv_data *pdata)
{
	DBGPR("-->eqos_default_common_confs\n");

	pdata->drop_tx_pktburstcnt = 1;
	pdata->mac_enable_count = 0;
	pdata->incr_incrx = EQOS_INCR_ENABLE;
	pdata->flow_ctrl = EQOS_FLOW_CTRL_TX_RX;
	pdata->oldflow_ctrl = EQOS_FLOW_CTRL_TX_RX;
	pdata->power_down = 0;
	pdata->tx_sa_ctrl_via_desc = EQOS_SA0_NONE;
	pdata->tx_sa_ctrl_via_reg = EQOS_SA0_NONE;
	pdata->hwts_tx_en = 0;
	pdata->hwts_rx_en = 0;
	pdata->l3_l4_filter = 0;
	pdata->l2_filtering_mode = !!pdata->hw_feat.hash_tbl_sz;
	pdata->tx_path_in_lpi_mode = 0;
	pdata->use_lpi_tx_automate = true;
	pdata->eee_active = 0;
	pdata->one_nsec_accuracy = 1;

	DBGPR("<--eqos_default_common_confs\n");
}


/*!
 * \brief api to initialize Tx parameters.
 *
 * \details This function is used to initialize all Tx
 * parameters to default values on reset.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] qinx – DMA channel/queue number to be initialized.
 *
 * \return void
 */

static void eqos_default_tx_confs_single_q(
		struct eqos_prv_data *pdata,
		UINT qinx)
{
	struct eqos_tx_queue *queue_data = GET_TX_QUEUE_PTR(qinx);
	struct eqos_tx_wrapper_descriptor *desc_data =
		GET_TX_WRAPPER_DESC(qinx);

	DBGPR("-->eqos_default_tx_confs_single_q\n");

	queue_data->q_op_mode = q_op_mode[qinx];

	desc_data->tx_threshold_val = EQOS_TX_THRESHOLD_32;
	desc_data->tsf_on = EQOS_TSF_ENABLE;
	desc_data->osf_on = EQOS_OSF_ENABLE;
	desc_data->tx_pbl = EQOS_PBL_16;
	desc_data->tx_vlan_tag_via_reg = Y_FALSE;
#ifdef ENABLE_VLAN_TAG_INSERTION
	desc_data->tx_vlan_tag_ctrl = EQOS_TX_VLAN_TAG_INSERT;
#else
	desc_data->tx_vlan_tag_ctrl = EQOS_TX_VLAN_TAG_NONE;
#endif
	desc_data->vlan_tag_present = 0;
	desc_data->context_setup = 0;
	desc_data->default_mss = 0;

	DBGPR("<--eqos_default_tx_confs_single_q\n");
}


/*!
 * \brief api to initialize Rx parameters.
 *
 * \details This function is used to initialize all Rx
 * parameters to default values on reset.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] qinx – DMA queue/channel number to be initialized.
 *
 * \return void
 */

static void eqos_default_rx_confs_single_q(
		struct eqos_prv_data *pdata,
		UINT qinx)
{
	struct eqos_rx_wrapper_descriptor *desc_data =
		GET_RX_WRAPPER_DESC(qinx);

	DBGPR("-->eqos_default_rx_confs_single_q\n");

	desc_data->rx_threshold_val = EQOS_RX_THRESHOLD_64;
	desc_data->rsf_on = EQOS_RSF_DISABLE;
	desc_data->rx_pbl = EQOS_PBL_16;
	desc_data->rx_outer_vlan_strip = EQOS_RX_VLAN_STRIP_ALWAYS;
	desc_data->rx_inner_vlan_strip = EQOS_RX_VLAN_STRIP_ALWAYS;

	DBGPR("<--eqos_default_rx_confs_single_q\n");
}

static void eqos_default_tx_confs(struct eqos_prv_data *pdata)
{
	UINT qinx;

	DBGPR("-->eqos_default_tx_confs\n");

	for (qinx = 0; qinx < EQOS_TX_QUEUE_CNT; qinx++) {
		eqos_default_tx_confs_single_q(pdata, qinx);
	}

	DBGPR("<--eqos_default_tx_confs\n");
}

static void eqos_default_rx_confs(struct eqos_prv_data *pdata)
{
	UINT qinx;

	DBGPR("-->eqos_default_rx_confs\n");

	for (qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++) {
		eqos_default_rx_confs_single_q(pdata, qinx);
	}

	DBGPR("<--eqos_default_rx_confs\n");
}


void free_txrx_irqs(struct eqos_prv_data *pdata)
{
	uint i;

	DBGPR("-->%s()\n", __func__);

	if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ) {
		free_irq(pdata->common_irq, pdata);

		for (i = 0; i < MAX_CHANS; i++) {
			if (pdata->rx_irq_alloc_mask & (1 << i)) {
				irq_set_affinity_hint(pdata->rx_irqs[i], NULL);
				free_irq(pdata->rx_irqs[i], pdata);
			}
			if (pdata->tx_irq_alloc_mask & (1 << i)) {
				irq_set_affinity_hint(pdata->tx_irqs[i], NULL);
				free_irq(pdata->tx_irqs[i], pdata);
			}
		}
	} else if (pdata->irq_number != 0) {
		free_irq(pdata->irq_number, pdata);
		pdata->irq_number = 0;
	}

	DBGPR("<--%s()\n", __func__);
}

int request_txrx_irqs(struct eqos_prv_data *pdata)
{
	int ret = Y_SUCCESS;
	uint i;
	struct chan_data *pchinfo;

	DBGPR("-->%s()\n", __func__);

	pdata->irq_number = pdata->dev->irq;

	if (pdata->dt_cfg.intr_mode != MODE_MULTI_IRQ) {
#ifdef EQOS_CONFIG_PGTEST
		ret = request_irq(pdata->irq_number,
				eqos_pg_isr,
				IRQF_SHARED, DEV_NAME, pdata);
#else
		ret = request_irq(pdata->irq_number,
				eqos_isr,
				IRQF_SHARED, DEV_NAME, pdata);
#endif /* end of DWC_ETH_QOS_CONFIG_PGTEST */

		if (ret != 0) {
			dev_err(&pdata->pdev->dev,
				"Unable to register IRQ %d\n",
				pdata->irq_number);
			return -EBUSY;
		}
		return Y_SUCCESS;
	}

	ret = request_irq(pdata->common_irq,
			eqos_common_isr, IRQF_SHARED, DEV_NAME, pdata);
	if (ret != Y_SUCCESS) {
		printk(KERN_ALERT "Unable to register  %d\n",
			pdata->common_irq);
		ret = -EBUSY;
		goto err_common_irq;
	}
	for (i = 0; i < MAX_CHANS; i++) {
		if ((pdata->dt_cfg.chan_mode[i] == CHAN_MODE_POLLING) ||
			(pdata->dt_cfg.chan_mode[i] == CHAN_MODE_NONE))
			continue;

		ret = request_irq(pdata->rx_irqs[i],
				eqos_ch_isr, 0, DEV_NAME, pdata);
		if (ret != 0) {
			printk(KERN_ALERT "Unable to register  %d\n",
				pdata->rx_irqs[i]);
			ret = -EBUSY;
			goto err_chan_irq;
		}
		ret = request_irq(pdata->tx_irqs[i],
				  eqos_ch_isr, 0, DEV_NAME, pdata);
		if (ret != 0) {
			printk(KERN_ALERT "Unable to register  %d\n",
			       pdata->tx_irqs[i]);
			ret = -EBUSY;
			goto err_chan_irq;
		}
		pchinfo = &pdata->chinfo[i];

		irq_set_affinity_hint(pdata->rx_irqs[i],
					cpumask_of(pchinfo->cpu));
		pdata->rx_irq_alloc_mask |= (1 << i);

		irq_set_affinity_hint(pdata->tx_irqs[i],
				      cpumask_of(pchinfo->cpu));
		pdata->tx_irq_alloc_mask |= (1 << i);
	}
	DBGPR("<--%s()\n", __func__);

	return ret;

 err_chan_irq:
	free_txrx_irqs(pdata);
	free_irq(pdata->common_irq, pdata);

 err_common_irq:
	DBGPR("<--%s(): error\n", __func__);
	return ret;
}


void init_txrx_poll_timers(struct eqos_prv_data *pdata)
{
	uint i;
	struct chan_data *pchinfo;

	DBGPR("-->%s()\n", __func__);

#ifdef ALL_POLLING
	if (pdata->dt_cfg.intr_mode == MODE_ALL_POLLING) {
		init_timer(&pdata->all_chans_timer);
		pdata->all_chans_timer.function = eqos_all_chans_timer;
		pdata->all_chans_timer.data = (unsigned long)pdata;
		pdata->all_chans_timer.expires =
			jiffies + msecs_to_jiffies(1000);  /* 1 s */
		add_timer(&pdata->all_chans_timer);
	}
#endif
	if (pdata->dt_cfg.intr_mode != MODE_MULTI_IRQ)
		return;

	for (i = 0; i < MAX_CHANS; i++) {
		pchinfo = &pdata->chinfo[i];

		if (pdata->dt_cfg.chan_mode[i] == CHAN_MODE_POLLING) {
			init_timer(&pchinfo->poll_timer);
			pchinfo->poll_timer.function = eqos_poll_timer;
			pchinfo->poll_timer.data = (unsigned long) pchinfo;
			pchinfo->poll_timer.expires =
				jiffies +
				msecs_to_jiffies(pchinfo->poll_interval);
			add_timer(&pchinfo->poll_timer);
		}
	}
	DBGPR("<--%s()\n", __func__);
}

void start_txrx_poll_timers(struct eqos_prv_data *pdata)
{
	uint i;
	struct chan_data *pchinfo;

	DBGPR("-->%s()\n", __func__);

#ifdef ALL_POLLING
	if (pdata->dt_cfg.intr_mode == MODE_ALL_POLLING)
		add_timer(&pdata->all_chans_timer);
#endif
	if (pdata->dt_cfg.intr_mode != MODE_MULTI_IRQ)
		return;

	for (i = 0; i < MAX_CHANS; i++) {
		pchinfo = &pdata->chinfo[i];

		if (pdata->dt_cfg.chan_mode[i] == CHAN_MODE_POLLING)
			add_timer(&pchinfo->poll_timer);
	}
	DBGPR("<--%s()\n", __func__);
}


void stop_txrx_poll_timers(struct eqos_prv_data *pdata)
{
	uint i;
	struct chan_data *pchinfo;

	DBGPR("-->%s()\n", __func__);
#ifdef ALL_POLLING
	if (pdata->dt_cfg.intr_mode == MODE_ALL_POLLING)
		del_timer_sync(&pdata->all_chans_timer);
#endif
	if (pdata->dt_cfg.intr_mode != MODE_MULTI_IRQ)
		return;

	for (i = 0; i < MAX_CHANS; i++) {
		pchinfo = &pdata->chinfo[i];

		if (pdata->dt_cfg.chan_mode[i] == CHAN_MODE_POLLING)
			del_timer_sync(&pchinfo->poll_timer);
	}
	DBGPR("<--%s()\n", __func__);
}


/*!
* \brief API to open a deivce for data transmission & reception.
*
* \details Opens the interface. The interface is opned whenever
* ifconfig activates it. The open method should register any
* system resource it needs like I/O ports, IRQ, DMA, etc,
* turn on the hardware, and perform any other setup your device requires.
*
* \param[in] dev - pointer to net_device structure
*
* \return integer
*
* \retval 0 on success & negative number on failure.
*/

static int eqos_open(struct net_device *dev)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	int ret = Y_SUCCESS;
	struct desc_if_struct *desc_if = &pdata->desc_if;

	DBGPR("-->eqos_open\n");

	if (!is_valid_ether_addr(dev->dev_addr))
		return -EADDRNOTAVAIL;

	ret = request_txrx_irqs(pdata);
	if (ret != Y_SUCCESS)
		goto err_irq_0;

	ret = desc_if->alloc_buff_and_desc(pdata);
	if (ret < 0) {
		dev_err(&pdata->pdev->dev,
			"Failed to allocate buffer/descriptor memory\n");
		ret = -ENOMEM;
		goto err_out_desc_buf_alloc_failed;
	}
	init_txrx_poll_timers(pdata);
	eqos_start_dev(pdata);

	DBGPR("<--%s()\n", __func__);
	return Y_SUCCESS;

err_out_desc_buf_alloc_failed:
	free_txrx_irqs(pdata);

err_irq_0:
	DBGPR("<--%s()\n", __func__);
	return ret;
}

/*!
* \brief API to close a device.
*
* \details Stops the interface. The interface is stopped when it is brought
* down. This function should reverse operations performed at open time.
*
* \param[in] dev - pointer to net_device structure
*
* \return integer
*
* \retval 0 on success & negative number on failure.
*/

static int eqos_close(struct net_device *dev)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct desc_if_struct *desc_if = &pdata->desc_if;

	DBGPR("-->%s\n", __func__);

	eqos_stop_dev(pdata);

	desc_if->free_buff_and_desc(pdata);
	free_txrx_irqs(pdata);

	DBGPR("<--%s\n", __func__);
	return Y_SUCCESS;
}


/*!
* \brief API to configure the multicast address in device.
*
* \details This function collects all the multicast addresse
* and updates the device.
*
* \param[in] dev - pointer to net_device structure.
*
* \retval 0 if perfect filtering is seleted & 1 if hash
* filtering is seleted.
*/
static int eqos_prepare_mc_list(struct net_device *dev)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	u32 mc_filter[EQOS_HTR_CNT];
	struct netdev_hw_addr *ha = NULL;
	int crc32_val = 0;
	int ret = 0, i = 1;

	DBGPR_FILTER("-->eqos_prepare_mc_list\n");

	if (pdata->l2_filtering_mode) {
		DBGPR_FILTER("select HASH FILTERING for mc addresses: mc_count = %d\n",
				netdev_mc_count(dev));
		ret = 1;
		memset(mc_filter, 0, sizeof(mc_filter));

		if (pdata->max_hash_table_size == 64) {
			netdev_for_each_mc_addr(ha, dev) {
				DBGPR_FILTER("mc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n",i++,
						ha->addr[0], ha->addr[1], ha->addr[2],
						ha->addr[3], ha->addr[4], ha->addr[5]);
				/* The upper 6 bits of the calculated CRC are used to
				 * index the content of the Hash Table Reg 0 and 1.
				 * */
				crc32_val =
					(bitrev32(~crc32_le(~0, ha->addr, 6)) >> 26);
				/* The most significant bit determines the register
				 * to use (Hash Table Reg X, X = 0 and 1) while the
				 * other 5(0x1F) bits determines the bit within the
				 * selected register
				 * */
				mc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
			}
		} else if (pdata->max_hash_table_size == 128) {
			netdev_for_each_mc_addr(ha, dev) {
				DBGPR_FILTER("mc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n",i++,
						ha->addr[0], ha->addr[1], ha->addr[2],
						ha->addr[3], ha->addr[4], ha->addr[5]);
				/* The upper 7 bits of the calculated CRC are used to
				 * index the content of the Hash Table Reg 0,1,2 and 3.
				 * */
				crc32_val =
					(bitrev32(~crc32_le(~0, ha->addr, 6)) >> 25);

				printk(KERN_ALERT "crc_le = %#x, crc_be = %#x\n",
						bitrev32(~crc32_le(~0, ha->addr, 6)),
						bitrev32(~crc32_be(~0, ha->addr, 6)));

				/* The most significant 2 bits determines the register
				 * to use (Hash Table Reg X, X = 0,1,2 and 3) while the
				 * other 5(0x1F) bits determines the bit within the
				 * selected register
				 * */
				mc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
			}
		} else if (pdata->max_hash_table_size == 256) {
			netdev_for_each_mc_addr(ha, dev) {
				DBGPR_FILTER("mc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n",i++,
						ha->addr[0], ha->addr[1], ha->addr[2],
						ha->addr[3], ha->addr[4], ha->addr[5]);
				/* The upper 8 bits of the calculated CRC are used to
				 * index the content of the Hash Table Reg 0,1,2,3,4,
				 * 5,6, and 7.
				 * */
				crc32_val =
					(bitrev32(~crc32_le(~0, ha->addr, 6)) >> 24);
				/* The most significant 3 bits determines the register
				 * to use (Hash Table Reg X, X = 0,1,2,3,4,5,6 and 7) while
				 * the other 5(0x1F) bits determines the bit within the
				 * selected register
				 * */
				mc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
			}
		}

		for (i = 0; i < EQOS_HTR_CNT; i++)
			hw_if->update_hash_table_reg(i, mc_filter[i]);

	} else {
		DBGPR_FILTER("select PERFECT FILTERING for mc addresses, mc_count = %d, max_addr_reg_cnt = %d\n",
				netdev_mc_count(dev), pdata->max_addr_reg_cnt);

		netdev_for_each_mc_addr(ha, dev) {
			DBGPR_FILTER("mc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n",
					i,
					ha->addr[0], ha->addr[1], ha->addr[2],
					ha->addr[3], ha->addr[4], ha->addr[5]);
			if (i < 32)
				hw_if->update_mac_addr1_31_low_high_reg(i,
					ha->addr);
			else
				hw_if->update_mac_addr32_127_low_high_reg(i,
					ha->addr);

			if ((pdata->ptp_cfg.use_tagged_ptp) &&
					(is_ptp_addr(ha->addr)))
				hw_if->config_ptp_channel(
					pdata->ptp_cfg.ptp_dma_ch_id, i);

			i++;
		}
	}

	DBGPR_FILTER("<--eqos_prepare_mc_list\n");

	return ret;
}

/*!
* \brief API to configure the unicast address in device.
*
* \details This function collects all the unicast addresses
* and updates the device.
*
* \param[in] dev - pointer to net_device structure.
*
* \retval 0 if perfect filtering is seleted  & 1 if hash
* filtering is seleted.
*/
static int eqos_prepare_uc_list(struct net_device *dev)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	u32 uc_filter[EQOS_HTR_CNT];
	struct netdev_hw_addr *ha = NULL;
	int crc32_val = 0;
	int ret = 0, i = 1;

	DBGPR_FILTER("-->eqos_prepare_uc_list\n");

	if (pdata->l2_filtering_mode) {
		DBGPR_FILTER("select HASH FILTERING for uc addresses: uc_count = %d\n",
				netdev_uc_count(dev));
		ret = 1;
		memset(uc_filter, 0, sizeof(uc_filter));

		if (pdata->max_hash_table_size == 64) {
			netdev_for_each_uc_addr(ha, dev) {
				DBGPR_FILTER("uc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n",i++,
						ha->addr[0], ha->addr[1], ha->addr[2],
						ha->addr[3], ha->addr[4], ha->addr[5]);
				crc32_val =
					(bitrev32(~crc32_le(~0, ha->addr, 6)) >> 26);
				uc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
			}
		} else if (pdata->max_hash_table_size == 128) {
			netdev_for_each_uc_addr(ha, dev) {
				DBGPR_FILTER("uc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n",i++,
						ha->addr[0], ha->addr[1], ha->addr[2],
						ha->addr[3], ha->addr[4], ha->addr[5]);
				crc32_val =
					(bitrev32(~crc32_le(~0, ha->addr, 6)) >> 25);
				uc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
			}
		} else if (pdata->max_hash_table_size == 256) {
			netdev_for_each_uc_addr(ha, dev) {
				DBGPR_FILTER("uc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n",i++,
						ha->addr[0], ha->addr[1], ha->addr[2],
						ha->addr[3], ha->addr[4], ha->addr[5]);
				crc32_val =
					(bitrev32(~crc32_le(~0, ha->addr, 6)) >> 24);
				uc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
			}
		}

		/* configure hash value of real/default interface also */
		DBGPR_FILTER("real/default dev_addr = %#x:%#x:%#x:%#x:%#x:%#x\n",
				dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
				dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5]);

		if (pdata->max_hash_table_size == 64) {
			crc32_val =
				(bitrev32(~crc32_le(~0, dev->dev_addr, 6)) >> 26);
			uc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
		} else if (pdata->max_hash_table_size == 128) {
			crc32_val =
				(bitrev32(~crc32_le(~0, dev->dev_addr, 6)) >> 25);
			uc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));

		} else if (pdata->max_hash_table_size == 256) {
			crc32_val =
				(bitrev32(~crc32_le(~0, dev->dev_addr, 6)) >> 24);
			uc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
		}

		for (i = 0; i < EQOS_HTR_CNT; i++)
			hw_if->update_hash_table_reg(i, uc_filter[i]);

	} else {
		DBGPR_FILTER("select PERFECT FILTERING for uc addresses: uc_count = %d\n",
				netdev_uc_count(dev));

		netdev_for_each_uc_addr(ha, dev) {
			DBGPR_FILTER("uc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n", i,
					ha->addr[0], ha->addr[1], ha->addr[2],
					ha->addr[3], ha->addr[4], ha->addr[5]);
			if (i < 32)
				hw_if->update_mac_addr1_31_low_high_reg(i, ha->addr);
			else
				hw_if->update_mac_addr32_127_low_high_reg(i, ha->addr);
			i++;
		}
	}

	DBGPR_FILTER("<--eqos_prepare_uc_list\n");

	return ret;
}

/*!
* \brief API to set the device receive mode
*
* \details The set_multicast_list function is called when the multicast list
* for the device changes and when the flags change.
*
* \param[in] dev - pointer to net_device structure.
*
* \return void
*/
static void eqos_set_rx_mode(struct net_device *dev)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	unsigned long flags;
	unsigned char pr_mode = 0;
	unsigned char huc_mode = 0;
	unsigned char hmc_mode = 0;
	unsigned char pm_mode = 0;
	unsigned char hpf_mode = 0;
	int mode, i;

	DBGPR_FILTER("-->eqos_set_rx_mode\n");

	spin_lock_irqsave(&pdata->lock, flags);

#ifdef EQOS_CONFIG_PGTEST
	DBGPR("PG Test running, no parameters will be changed\n");
	spin_unlock_irqrestore(&pdata->lock, flags);
	return;
#endif

	if (dev->flags & IFF_PROMISC) {
		DBGPR_FILTER("PROMISCUOUS MODE (Accept all packets irrespective of DA)\n");
		pr_mode = 1;
#ifdef ENABLE_PERFECT_L2_FILTER
	} else if ((dev->flags & IFF_ALLMULTI)) {
#else
	} else if ((dev->flags & IFF_ALLMULTI) ||
			(netdev_mc_count(dev) > (pdata->max_hash_table_size))) {
#endif
		DBGPR_FILTER("pass all multicast pkt\n");
		pm_mode = 1;
		if (pdata->max_hash_table_size) {
			for (i = 0; i < EQOS_HTR_CNT; i++)
				hw_if->update_hash_table_reg(i, 0xffffffff);
		}
	} else if (!netdev_mc_empty(dev)) {
		DBGPR_FILTER("pass list of multicast pkt\n");
		if ((netdev_mc_count(dev) > (pdata->max_addr_reg_cnt - 1)) &&
			(!pdata->max_hash_table_size)) {
			/* switch to PROMISCUOUS mode */
			pr_mode = 1;
		} else {
			mode = eqos_prepare_mc_list(dev);
			if (mode) {
				/* Hash filtering for multicast */
				hmc_mode = 1;
			} else {
				/* Perfect filtering for multicast */
				hmc_mode = 0;
				hpf_mode = 1;
			}
		}
	}

	/* Handle multiple unicast addresses */
	if ((netdev_uc_count(dev) > (pdata->max_addr_reg_cnt - 1)) &&
			(!pdata->max_hash_table_size)) {
		/* switch to PROMISCUOUS mode */
		pr_mode = 1;
	} else if (!netdev_uc_empty(dev)) {
		mode = eqos_prepare_uc_list(dev);
		if (mode) {
			/* Hash filtering for unicast */
			huc_mode = 1;
		} else {
			/* Perfect filtering for unicast */
			huc_mode = 0;
			hpf_mode = 1;
		}
	}

	hw_if->config_mac_pkt_filter_reg(pr_mode, huc_mode,
		hmc_mode, pm_mode, hpf_mode);

	spin_unlock_irqrestore(&pdata->lock, flags);

	DBGPR("<--eqos_set_rx_mode\n");
}

/*!
* \brief API to calculate number of descriptor.
*
* \details This function is invoked by start_xmit function. This function
* calculates number of transmit descriptor required for a given transfer.
*
* \param[in] pdata - pointer to private data structure
* \param[in] skb - pointer to sk_buff structure
* \param[in] qinx - Queue number.
*
* \return integer
*
* \retval number of descriptor required.
*/

UINT eqos_get_total_desc_cnt(struct eqos_prv_data *pdata,
		struct sk_buff *skb, UINT qinx)
{
	UINT count = 0, size = 0;
	INT length = 0;
#ifdef EQOS_ENABLE_VLAN_TAG
	struct hw_if_struct *hw_if = &pdata->hw_if;
	struct s_tx_pkt_features *tx_pkt_features = GET_TX_PKT_FEATURES_PTR;
	struct eqos_tx_wrapper_descriptor *desc_data =
	    GET_TX_WRAPPER_DESC(qinx);
#endif

	/* SG fragment count */
	count += skb_shinfo(skb)->nr_frags;

	/* descriptors required based on data limit per descriptor */
	length = (skb->len - skb->data_len);
	while (length) {
		size = min(length, EQOS_MAX_DATA_PER_TXD);
		count++;
		length = length - size;
	}

	/* we need one context descriptor to carry tso details */
	if (skb_shinfo(skb)->gso_size != 0)
		count++;

#ifdef EQOS_ENABLE_VLAN_TAG
	desc_data->vlan_tag_present = 0;
	if (vlan_tx_tag_present(skb)) {
		USHORT vlan_tag = vlan_tx_tag_get(skb);
		vlan_tag |= (skb->priority << 13);
		desc_data->vlan_tag_present = 1;
		if (vlan_tag != desc_data->vlan_tag_id ||
				desc_data->context_setup == 1) {
			desc_data->vlan_tag_id = vlan_tag;
			if (Y_TRUE == desc_data->tx_vlan_tag_via_reg) {
				printk(KERN_ALERT "VLAN control info update via register\n\n");
				hw_if->enable_vlan_reg_control(desc_data);
			} else {
				hw_if->enable_vlan_desc_control(pdata);
				TX_PKT_FEATURES_PKT_ATTRIBUTES_VLAN_PKT_WR
				    (tx_pkt_features->pkt_attributes, 1);
				TX_PKT_FEATURES_VLAN_TAG_VT_WR
					(tx_pkt_features->vlan_tag, vlan_tag);
				/* we need one context descriptor to carry vlan tag info */
				count++;
			}
		}
		pdata->xstats.tx_vlan_pkt_n++;
	}
#endif
#ifdef EQOS_ENABLE_DVLAN
	if (pdata->via_reg_or_desc == EQOS_VIA_DESC) {
		/* we need one context descriptor to carry vlan tag info */
		count++;
	}
#endif /* End of EQOS_ENABLE_DVLAN */

	return count;
}


/*!
* \brief API to transmit the packets
*
* \details The start_xmit function initiates the transmission of a packet.
* The full packet (protocol headers and all) is contained in a socket buffer
* (sk_buff) structure.
*
* \param[in] skb - pointer to sk_buff structure
* \param[in] dev - pointer to net_device structure
*
* \return integer
*
* \retval 0
*/

static int eqos_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	UINT qinx = skb_get_queue_mapping(skb);
	struct eqos_tx_wrapper_descriptor *desc_data =
	    GET_TX_WRAPPER_DESC(qinx);
	struct s_tx_pkt_features *tx_pkt_features = GET_TX_PKT_FEATURES_PTR;
	unsigned long flags;
	unsigned int desc_count = 0;
	unsigned int count = 0;
	struct hw_if_struct *hw_if = &pdata->hw_if;
	struct desc_if_struct *desc_if = &pdata->desc_if;
	INT retval = NETDEV_TX_OK;
#ifdef EQOS_ENABLE_VLAN_TAG
	UINT varvlan_pkt;
#endif
	int tso;

	DBGPR("-->eqos_start_xmit: skb->len = %d, qinx = %u\n",
		skb->len, qinx);

	if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ)
		spin_lock_irqsave(&pdata->chinfo[qinx].chan_tx_lock, flags);
	else
		spin_lock_irqsave(&pdata->tx_lock, flags);

#ifdef EQOS_CONFIG_PGTEST
	retval = NETDEV_TX_BUSY;
	goto tx_netdev_return;
#endif

	if (skb->len <= 0) {
		dev_kfree_skb_any(skb);
		printk(KERN_ERR "%s : Empty skb received from stack\n",
			dev->name);
		goto tx_netdev_return;
	}

	if ((pdata->eee_enabled) && (pdata->tx_path_in_lpi_mode) &&
		(!pdata->use_lpi_tx_automate))
		eqos_disable_eee_mode(pdata);

	memset(&pdata->tx_pkt_features, 0, sizeof(pdata->tx_pkt_features));

	/* check total number of desc required for current xfer */
	desc_count = eqos_get_total_desc_cnt(pdata, skb, qinx);
	if (desc_data->free_desc_cnt < desc_count) {
		desc_data->queue_stopped = 1;
		netif_stop_subqueue(dev, qinx);
		DBGPR("stopped TX queue(%d) since there are no sufficient "
			"descriptor available for the current transfer\n",
			qinx);
		retval = NETDEV_TX_BUSY;
		goto tx_netdev_return;
	}

	/* check for hw tstamping */
	if (pdata->hw_feat.tsstssel && pdata->hwts_tx_en) {
		if(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) {
			/* declare that device is doing timestamping */
			skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
			TX_PKT_FEATURES_PKT_ATTRIBUTES_PTP_ENABLE_WR(tx_pkt_features->pkt_attributes, 1);
			DBGPR_PTP("Got PTP pkt to transmit [qinx = %d, cur_tx = %d]\n",
				qinx, desc_data->cur_tx);
		}
	}

	tso = desc_if->handle_tso(dev, skb);
	if (tso < 0) {
		printk(KERN_ALERT "Unable to handle TSO\n");
		dev_kfree_skb_any(skb);
		retval = NETDEV_TX_OK;
		goto tx_netdev_return;
	}
	if (tso) {
		pdata->xstats.tx_tso_pkt_n++;
		TX_PKT_FEATURES_PKT_ATTRIBUTES_TSO_ENABLE_WR(tx_pkt_features->pkt_attributes, 1);
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		TX_PKT_FEATURES_PKT_ATTRIBUTES_CSUM_ENABLE_WR(tx_pkt_features->pkt_attributes, 1);
	}

	count = desc_if->map_tx_skb(dev, skb);
	if (count == 0) {
		dev_kfree_skb_any(skb);
		retval = NETDEV_TX_OK;
		goto tx_netdev_return;
	}

	desc_data->packet_count = count;

	if (tso && (desc_data->default_mss != tx_pkt_features->mss))
		count++;

	dev->trans_start = jiffies;

#ifdef EQOS_ENABLE_VLAN_TAG
	TX_PKT_FEATURES_PKT_ATTRIBUTES_VLAN_PKT_RD
		(tx_pkt_features->pkt_attributes, varvlan_pkt);
	if (varvlan_pkt == 0x1)
		count++;
#endif
#ifdef EQOS_ENABLE_DVLAN
	if (pdata->via_reg_or_desc == EQOS_VIA_DESC) {
		count++;
	}
#endif /* End of EQOS_ENABLE_DVLAN */

	desc_data->free_desc_cnt -= count;
	desc_data->tx_pkt_queued += count;

#ifdef EQOS_ENABLE_TX_PKT_DUMP
	print_pkt(skb, skb->len, 1, (desc_data->cur_tx - 1));
#endif

#ifdef ENABLE_CHANNEL_DATA_CHECK
	check_channel_data(skb, qinx, 0);
#endif

	/* fallback to software time stamping if core doesn't
	 * support hardware time stamping */
	if ((pdata->hw_feat.tsstssel == 0) || (pdata->hwts_tx_en == 0))
		skb_tx_timestamp(skb);

	/* configure required descriptor fields for transmission */
	hw_if->pre_xmit(pdata, qinx);

tx_netdev_return:
	if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ)
		spin_unlock_irqrestore(&pdata->chinfo[qinx].chan_tx_lock,
					flags);
	else
		spin_unlock_irqrestore(&pdata->tx_lock, flags);

	DBGPR("<--eqos_start_xmit\n");

	return retval;
}

static void eqos_print_rx_tstamp_info(struct s_rx_normal_desc *rxdesc,
	unsigned int qinx)
{
	u32 ptp_status = 0;
	u32 pkt_type = 0;
	char *tstamp_dropped = NULL;
	char *tstamp_available = NULL;
	char *ptp_version = NULL;
	char *ptp_pkt_type = NULL;
	char *ptp_msg_type = NULL;

	DBGPR_PTP("-->eqos_print_rx_tstamp_info\n");

	/* status in RDES1 is not valid */
	if (!(rxdesc->RDES3 & EQOS_RDESC3_RS1V))
		return;

	ptp_status = rxdesc->RDES1;
	tstamp_dropped = ((ptp_status & 0x8000) ? "YES" : "NO");
	tstamp_available = ((ptp_status & 0x4000) ? "YES" : "NO");
	ptp_version = ((ptp_status & 0x2000) ? "v2 (1588-2008)" : "v1 (1588-2002)");
	ptp_pkt_type = ((ptp_status & 0x1000) ? "ptp over Eth" : "ptp over IPv4/6");

	pkt_type = ((ptp_status & 0xF00) > 8);
	switch (pkt_type) {
	case 0:
		ptp_msg_type = "NO PTP msg received";
		break;
	case 1:
		ptp_msg_type = "SYNC";
		break;
	case 2:
		ptp_msg_type = "Follow_Up";
		break;
	case 3:
		ptp_msg_type = "Delay_Req";
		break;
	case 4:
		ptp_msg_type = "Delay_Resp";
		break;
	case 5:
		ptp_msg_type = "Pdelay_Req";
		break;
	case 6:
		ptp_msg_type = "Pdelay_Resp";
		break;
	case 7:
		ptp_msg_type = "Pdelay_Resp_Follow_up";
		break;
	case 8:
		ptp_msg_type = "Announce";
		break;
	case 9:
		ptp_msg_type = "Management";
		break;
	case 10:
		ptp_msg_type = "Signaling";
		break;
	case 11:
	case 12:
	case 13:
	case 14:
		ptp_msg_type = "Reserved";
		break;
	case 15:
		ptp_msg_type = "PTP pkr with Reserved Msg Type";
		break;
	}

	DBGPR_PTP("Rx timestamp detail for queue %d\n"
			"tstamp dropped    = %s\n"
			"tstamp available  = %s\n"
			"PTP version       = %s\n"
			"PTP Pkt Type      = %s\n"
			"PTP Msg Type      = %s\n",
			qinx, tstamp_dropped, tstamp_available,
			ptp_version, ptp_pkt_type, ptp_msg_type);

	DBGPR_PTP("<--eqos_print_rx_tstamp_info\n");
}


/*!
* \brief API to get rx time stamp value.
*
* \details This function will read received packet's timestamp from
* the descriptor and pass it to stack and also perform some sanity checks.
*
* \param[in] pdata - pointer to private data structure.
* \param[in] skb - pointer to sk_buff structure.
* \param[in] desc_data - pointer to wrapper receive descriptor structure.
* \param[in] qinx - Queue/Channel number.
*
* \return integer
*
* \retval 0 if no context descriptor
* \retval 1 if timestamp is valid
* \retval 2 if time stamp is corrupted
*/

static unsigned char eqos_get_rx_hwtstamp(
	struct eqos_prv_data *pdata,
	struct sk_buff *skb,
	struct eqos_rx_wrapper_descriptor *desc_data,
	unsigned int qinx)
{
	struct s_rx_normal_desc *rx_normal_desc =
		GET_RX_DESC_PTR(qinx, desc_data->cur_rx);
	struct s_rx_context_desc *rx_context_desc = NULL;
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct skb_shared_hwtstamps *shhwtstamp = NULL;
	u64 ns;
	int retry, ret;

	DBGPR_PTP("-->eqos_get_rx_hwtstamp\n");

	eqos_print_rx_tstamp_info(rx_normal_desc, qinx);

	desc_data->dirty_rx++;
	INCR_RX_DESC_INDEX(desc_data->cur_rx, 1);
	rx_context_desc = GET_RX_DESC_PTR(qinx, desc_data->cur_rx);

	DBGPR_PTP("\nRX_CONTEX_DESC[%d %4p %d RECEIVED FROM DEVICE]"\
			" = %#x:%#x:%#x:%#x",
			qinx, rx_context_desc, desc_data->cur_rx, rx_context_desc->RDES0,
			rx_context_desc->RDES1,
			rx_context_desc->RDES2, rx_context_desc->RDES3);

	/* check rx tsatmp */
	for (retry = 0; retry < 10; retry++) {
		ret = hw_if->get_rx_tstamp_status(rx_context_desc);
		if (ret == 1) {
			/* time stamp is valid */
			break;
		} else if (ret == 0) {
			printk(KERN_ALERT "Device has not yet updated the context "
				"desc to hold Rx time stamp(retry = %d)\n", retry);
		} else {
			printk(KERN_ALERT "Error: Rx time stamp is corrupted(retry = %d)\n", retry);
			return 2;
		}
	}

	if (retry == 10) {
			printk(KERN_ALERT "Device has not yet updated the context "
				"desc to hold Rx time stamp(retry = %d)\n", retry);
			desc_data->dirty_rx--;
			DECR_RX_DESC_INDEX(desc_data->cur_rx);
			return 0;
	}

	pdata->xstats.rx_timestamp_captured_n++;
	/* get valid tstamp */
	ns = hw_if->get_rx_tstamp(rx_context_desc);

	shhwtstamp = skb_hwtstamps(skb);
	memset(shhwtstamp, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamp->hwtstamp = ns_to_ktime(ns);

	DBGPR_PTP("<--eqos_get_rx_hwtstamp\n");

	return 1;
}


/*!
* \brief API to get tx time stamp value.
*
* \details This function will read timestamp from the descriptor
* and pass it to stack and also perform some sanity checks.
*
* \param[in] pdata - pointer to private data structure.
* \param[in] txdesc - pointer to transmit descriptor structure.
* \param[in] skb - pointer to sk_buff structure.
*
* \return integer
*
* \retval 1 if time stamp is taken
* \retval 0 if time stamp in not taken/valid
*/

static unsigned int eqos_get_tx_hwtstamp(
	struct eqos_prv_data *pdata,
	struct s_tx_normal_desc *txdesc,
	struct sk_buff *skb)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct skb_shared_hwtstamps shhwtstamp;
	u64 ns;

	DBGPR_PTP("-->eqos_get_tx_hwtstamp\n");

	if (hw_if->drop_tx_status_enabled() == 0) {
		/* check tx tstamp status */
		if (!hw_if->get_tx_tstamp_status(txdesc)) {
			printk(KERN_ALERT "tx timestamp is not captured for this packet\n");
			return 0;
		}

		/* get the valid tstamp */
		ns = hw_if->get_tx_tstamp(txdesc);
	} else {
		/* drop tx status mode is enabled, hence read time
		 * stamp from register instead of descriptor */

		/* check tx tstamp status */
		if (!hw_if->get_tx_tstamp_status_via_reg()) {
			printk(KERN_ALERT "tx timestamp is not captured for this packet\n");
			return 0;
		}

		/* get the valid tstamp */
		ns = hw_if->get_tx_tstamp_via_reg();
	}

	pdata->xstats.tx_timestamp_captured_n++;
	memset(&shhwtstamp, 0, sizeof(struct skb_shared_hwtstamps));
	shhwtstamp.hwtstamp = ns_to_ktime(ns);
	/* pass tstamp to stack */
	skb_tstamp_tx(skb, &shhwtstamp);

	DBGPR_PTP("<--eqos_get_tx_hwtstamp\n");

	return 1;
}


/*!
* \brief API to update the tx status.
*
* \details This function is called in isr handler once after getting
* transmit complete interrupt to update the transmited packet status
* and it does some house keeping work like updating the
* private data structure variables.
*
* \param[in] dev - pointer to net_device structure
* \param[in] pdata - pointer to private data structure.
*
* \return void
*/

static void eqos_tx_interrupt(struct net_device *dev,
				     struct eqos_prv_data *pdata,
				     UINT qinx)
{
	struct eqos_tx_wrapper_descriptor *desc_data =
	    GET_TX_WRAPPER_DESC(qinx);
	struct s_tx_normal_desc *txptr = NULL;
	struct eqos_tx_buffer *buffer = NULL;
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct desc_if_struct *desc_if = &(pdata->desc_if);
#ifndef EQOS_CERTIFICATION_PKTBURSTCNT
	int err_incremented;
#endif
	unsigned int tstamp_taken = 0;
	unsigned long flags;

	DBGPR("-->eqos_tx_interrupt: desc_data->tx_pkt_queued = %d"
		" dirty_tx = %d, qinx = %u\n",
		desc_data->tx_pkt_queued, desc_data->dirty_tx, qinx);

	if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ)
		spin_lock_irqsave(&pdata->chinfo[qinx].chan_tx_lock, flags);
	else
		spin_lock_irqsave(&pdata->tx_lock, flags);

	pdata->xstats.tx_clean_n[qinx]++;
	while (desc_data->tx_pkt_queued > 0) {
		txptr = GET_TX_DESC_PTR(qinx, desc_data->dirty_tx);
		buffer = GET_TX_BUF_PTR(qinx, desc_data->dirty_tx);
		tstamp_taken = 0;

		if (!hw_if->tx_complete(txptr))
			break;

#ifdef EQOS_ENABLE_TX_DESC_DUMP
		dump_tx_desc(pdata, desc_data->dirty_tx, desc_data->dirty_tx,
			     0, qinx);
#endif

#ifndef EQOS_CERTIFICATION_PKTBURSTCNT
		/* update the tx error if any by looking at last segment
		 * for NORMAL descriptors
		 * */
		if ((hw_if->get_tx_desc_ls(txptr)) && !(hw_if->get_tx_desc_ctxt(txptr))) {
			/* check whether skb support hw tstamp */
			if ((pdata->hw_feat.tsstssel) &&
				(skb_shinfo(buffer->skb)->tx_flags & SKBTX_IN_PROGRESS)) {
				tstamp_taken = eqos_get_tx_hwtstamp(pdata,
					txptr, buffer->skb);
				if (tstamp_taken) {
					//dump_tx_desc(pdata, desc_data->dirty_tx, desc_data->dirty_tx,
					//		0, qinx);
					DBGPR_PTP("passed tx timestamp to stack[qinx = %d, dirty_tx = %d]\n",
						qinx, desc_data->dirty_tx);
				}
			}

			err_incremented = 0;
			if (hw_if->tx_window_error) {
				if (hw_if->tx_window_error(txptr)) {
					err_incremented = 1;
					dev->stats.tx_window_errors++;
				}
			}
			if (hw_if->tx_aborted_error) {
				if (hw_if->tx_aborted_error(txptr)) {
					err_incremented = 1;
					dev->stats.tx_aborted_errors++;
					if (hw_if->tx_handle_aborted_error)
						hw_if->tx_handle_aborted_error(txptr);
				}
			}
			if (hw_if->tx_carrier_lost_error) {
				if (hw_if->tx_carrier_lost_error(txptr)) {
					err_incremented = 1;
					dev->stats.tx_carrier_errors++;
				}
			}
			if (hw_if->tx_fifo_underrun) {
				if (hw_if->tx_fifo_underrun(txptr)) {
					err_incremented = 1;
					dev->stats.tx_fifo_errors++;
					if (hw_if->tx_update_fifo_threshold)
						hw_if->tx_update_fifo_threshold(txptr);
				}
			}
			if (hw_if->tx_get_collision_count)
				dev->stats.collisions +=
				    hw_if->tx_get_collision_count(txptr);

			if (err_incremented == 1)
				dev->stats.tx_errors++;

			pdata->xstats.q_tx_pkt_n[qinx]++;
			pdata->xstats.tx_pkt_n++;
			dev->stats.tx_packets++;
		}
#else
		if ((hw_if->get_tx_desc_ls(txptr)) && !(hw_if->get_tx_desc_ctxt(txptr))) {
			/* check whether skb support hw tstamp */
			if ((pdata->hw_feat.tsstssel) &&
				(skb_shinfo(buffer->skb)->tx_flags & SKBTX_IN_PROGRESS)) {
				tstamp_taken = eqos_get_tx_hwtstamp(pdata,
					txptr, buffer->skb);
				if (tstamp_taken) {
					dump_tx_desc(pdata, desc_data->dirty_tx, desc_data->dirty_tx,
							0, qinx);
					DBGPR_PTP("passed tx timestamp to stack[qinx = %d, dirty_tx = %d]\n",
						qinx, desc_data->dirty_tx);
				}
			}
		}
#endif
		dev->stats.tx_bytes += buffer->len;
		dev->stats.tx_bytes += buffer->len2;
		desc_if->unmap_tx_skb(pdata, buffer);

		/* reset the descriptor so that driver/host can reuse it */
		hw_if->tx_desc_reset(desc_data->dirty_tx, pdata, qinx);

		INCR_TX_DESC_INDEX(desc_data->dirty_tx, 1);
		desc_data->free_desc_cnt++;
		desc_data->tx_pkt_queued--;
	}

	if ((desc_data->queue_stopped == 1) && (desc_data->free_desc_cnt > 0)) {
		desc_data->queue_stopped = 0;
		netif_wake_subqueue(dev, qinx);
	}
#ifdef EQOS_CERTIFICATION_PKTBURSTCNT
	/* DMA has finished Transmitting data to MAC Tx-Fifo */
	MAC_MCR_TE_WR(1);
#endif

	if ((pdata->eee_enabled) && (!pdata->tx_path_in_lpi_mode) &&
		(!pdata->use_lpi_tx_automate)) {
		eqos_enable_eee_mode(pdata);
		mod_timer(&pdata->eee_ctrl_timer,
			EQOS_LPI_TIMER(EQOS_DEFAULT_LPI_TIMER));
	}

	if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ)
		spin_unlock_irqrestore(&pdata->chinfo[qinx].chan_tx_lock, flags);
	else
		spin_unlock_irqrestore(&pdata->tx_lock, flags);

	DBGPR("<--eqos_tx_interrupt: desc_data->tx_pkt_queued = %d\n",
	      desc_data->tx_pkt_queued);
}

#ifdef YDEBUG_FILTER
static void eqos_check_rx_filter_status(struct s_rx_normal_desc *RX_NORMAL_DESC)
{
	u32 rdes2 = RX_NORMAL_DESC->RDES2;
	u32 rdes3 = RX_NORMAL_DESC->RDES3;

	/* Receive Status RDES2 Valid ? */
	if ((rdes3 & 0x8000000) == 0x8000000) {
		if ((rdes2 & 0x400) == 0x400)
			printk(KERN_ALERT "ARP pkt received\n");
		if ((rdes2 & 0x800) == 0x800)
			printk(KERN_ALERT "ARP reply not generated\n");
		if ((rdes2 & 0x8000) == 0x8000)
			printk(KERN_ALERT "VLAN pkt passed VLAN filter\n");
		if ((rdes2 & 0x10000) == 0x10000)
			printk(KERN_ALERT "SA Address filter fail\n");
		if ((rdes2 & 0x20000) == 0x20000)
			printk(KERN_ALERT "DA Addess filter fail\n");
		if ((rdes2 & 0x40000) == 0x40000)
			printk(KERN_ALERT "pkt passed the HASH filter in MAC and HASH value = %#x\n",
					(rdes2 >> 19) & 0xff);
		if ((rdes2 & 0x8000000) == 0x8000000)
			printk(KERN_ALERT "L3 filter(%d) Match\n", ((rdes2 >> 29) & 0x7));
		if ((rdes2 & 0x10000000) == 0x10000000)
			printk(KERN_ALERT "L4 filter(%d) Match\n", ((rdes2 >> 29) & 0x7));
	}
}
#endif /* YDEBUG_FILTER */


/* pass skb to upper layer */
static void eqos_receive_skb(struct eqos_prv_data *pdata,
				    struct net_device *dev, struct sk_buff *skb,
				    UINT qinx)
{
	struct eqos_rx_queue *rx_queue = GET_RX_QUEUE_PTR(qinx);

	skb_record_rx_queue(skb, qinx);
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);

	if (dev->features & NETIF_F_GRO) {
		napi_gro_receive(&rx_queue->napi, skb);
	} else if ((dev->features & NETIF_F_LRO) &&
		(skb->ip_summed == CHECKSUM_UNNECESSARY)) {
		lro_receive_skb(&rx_queue->lro_mgr, skb, (void *)pdata);
		rx_queue->lro_flush_needed = 1;
	} else {
		netif_receive_skb(skb);
	}
}

static void eqos_consume_page(struct eqos_rx_buffer *buffer,
				     struct sk_buff *skb,
				     u16 length, u16 buf2_used)
{
	buffer->page = NULL;
	if (buf2_used)
		buffer->page2 = NULL;
	skb->len += length;
	skb->data_len += length;
	skb->truesize += length;
}

static void eqos_consume_page_split_hdr(
				struct eqos_rx_buffer *buffer,
				struct sk_buff *skb,
				u16 length,
				USHORT page2_used)
{
	if (page2_used)
		buffer->page2 = NULL;

	skb->len += length;
	skb->data_len += length;
	skb->truesize += length;
}

/* Receive Checksum Offload configuration */
static inline void eqos_config_rx_csum(struct eqos_prv_data *pdata,
		struct sk_buff *skb,
		struct s_rx_normal_desc *rx_normal_desc)
{
	UINT varRDES1;

	skb->ip_summed = CHECKSUM_NONE;

	if ((pdata->dev_state & NETIF_F_RXCSUM) == NETIF_F_RXCSUM) {
		/* Receive Status RDES1 Valid ? */
		if ((rx_normal_desc->RDES3 & EQOS_RDESC3_RS1V)) {
			/* check(RDES1.IPCE bit) whether device has done csum correctly or not */
			RX_NORMAL_DESC_RDES1_RD(rx_normal_desc->RDES1, varRDES1);
			if ((varRDES1 & 0xC8) == 0x0)
				skb->ip_summed = CHECKSUM_UNNECESSARY;	/* csum done by device */
		}
	}
}

static inline void eqos_get_rx_vlan(struct eqos_prv_data *pdata,
			struct sk_buff *skb,
			struct s_rx_normal_desc *rx_normal_desc)
{
	USHORT vlan_tag = 0;

	if ((pdata->dev_state & NETIF_F_HW_VLAN_CTAG_RX) == NETIF_F_HW_VLAN_CTAG_RX) {
		/* Receive Status RDES0 Valid ? */
		if ((rx_normal_desc->RDES3 & EQOS_RDESC3_RS0V)) {
			/* device received frame with VLAN Tag or
			 * double VLAN Tag ? */
			if (((rx_normal_desc->RDES3 & EQOS_RDESC3_LT) == 0x40000)
				|| ((rx_normal_desc->RDES3 & EQOS_RDESC3_LT) == 0x50000)) {
				vlan_tag = rx_normal_desc->RDES0 & 0xffff;
				/* insert VLAN tag into skb */
				__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vlan_tag);
				pdata->xstats.rx_vlan_pkt_n++;
			}
		}
	}
}

/* This api check for payload type and returns
 * 1 if payload load is TCP else returns 0;
 * */
static int eqos_check_for_tcp_payload(struct s_rx_normal_desc *rxdesc)
{
		u32 pt_type = 0;
		int ret = 0;

		if (rxdesc->RDES3 & EQOS_RDESC3_RS1V) {
				pt_type = rxdesc->RDES1 & EQOS_RDESC1_PT;
				if (pt_type == EQOS_RDESC1_PT_TCP)
						ret = 1;
		}

		return ret;
}

/*!
* \brief API to pass the Rx packets to stack if split header
* feature is enabled.
*
* \details This function is invoked by main NAPI function if RX
* split header feature is enabled. This function checks the device
* descriptor for the packets and passes it to stack if any packtes
* are received by device.
*
* \param[in] pdata - pointer to private data structure.
* \param[in] quota - maximum no. of packets that we are allowed to pass
* to into the kernel.
* \param[in] qinx - DMA channel/queue no. to be checked for packet.
*
* \return integer
*
* \retval number of packets received.
*/

static int eqos_clean_split_hdr_rx_irq(
			struct eqos_prv_data *pdata,
			int quota,
			UINT qinx)
{
	struct eqos_rx_wrapper_descriptor *desc_data =
	    GET_RX_WRAPPER_DESC(qinx);
	struct net_device *dev = pdata->dev;
	struct desc_if_struct *desc_if = &pdata->desc_if;
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct sk_buff *skb = NULL;
	int received = 0;
	struct eqos_rx_buffer *buffer = NULL;
	struct s_rx_normal_desc *RX_NORMAL_DESC = NULL;
	u16 pkt_len;
	unsigned short hdr_len = 0;
	unsigned short payload_len = 0;
	unsigned char intermediate_desc_cnt = 0;
	unsigned char buf2_used = 0;
	int ret;

#ifdef HWA_NV_1618922
	UINT err_bits = 0x1200000;
#endif
	DBGPR("-->eqos_clean_split_hdr_rx_irq: qinx = %u, quota = %d\n",
		qinx, quota);

	while (received < quota) {
		buffer = GET_RX_BUF_PTR(qinx, desc_data->cur_rx);
		RX_NORMAL_DESC = GET_RX_DESC_PTR(qinx, desc_data->cur_rx);

		/* check for data availability */
		if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_OWN)) {
#ifdef EQOS_ENABLE_RX_DESC_DUMP
			dump_rx_desc(qinx, RX_NORMAL_DESC, desc_data->cur_rx);
#endif
			/* assign it to new skb */
			skb = buffer->skb;
			buffer->skb = NULL;

			/* first buffer pointer */
			dma_unmap_single(&pdata->pdev->dev, buffer->dma,
				       ALIGN_SIZE(2*buffer->rx_hdr_size),
				       DMA_FROM_DEVICE);
			buffer->dma = 0;

			/* second buffer pointer */
			dma_unmap_page(&pdata->pdev->dev, buffer->dma2,
				       PAGE_SIZE, DMA_FROM_DEVICE);
			buffer->dma2 = 0;

			/* get the packet length */
			pkt_len =
			    (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_PL);

			/* FIRST desc and Receive Status RDES2 Valid ? */
			if ((RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_FD) &&
				(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_RS2V)) {
				/* get header length */
				hdr_len = (RX_NORMAL_DESC->RDES2 & EQOS_RDESC2_HL);
				DBGPR("Device has %s HEADER SPLIT: hdr_len = %d\n",
						(hdr_len ? "done" : "not done"), hdr_len);
				if (hdr_len)
					pdata->xstats.rx_split_hdr_pkt_n++;
			}

			/* check for bad packet,
			 * error is valid only for last descriptor(OWN + LD bit set).
			 * */
#ifdef HWA_NV_1618922
			if ((RX_NORMAL_DESC->RDES3 & err_bits) &&
			    (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD)) {
#else
			if ((RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_ES) &&
			    (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD)) {
#endif
				DBGPR("Error in rcved pkt, failed to pass it to upper layer\n");
				dump_rx_desc(qinx, RX_NORMAL_DESC, desc_data->cur_rx);
				dev->stats.rx_errors++;
				eqos_update_rx_errors(dev,
					RX_NORMAL_DESC->RDES3);

				/* recycle both page/buff and skb */
				buffer->skb = skb;
				if (desc_data->skb_top)
					dev_kfree_skb_any(desc_data->skb_top);

				desc_data->skb_top = NULL;
				goto next_desc;
			}

			if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD)) {
				intermediate_desc_cnt++;
				buf2_used = 1;
				/* this descriptor is only the beginning/middle */
				if (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_FD) {
					/* this is the beginning of a chain */

					/* here skb/skb_top may contain
					 * if (device done split header)
					 *	only header
					 * else
					 *	header/(header + payload)
					 * */
					desc_data->skb_top = skb;
					/* page2 always contain only payload */
					if (hdr_len) {
						/* add header len to first skb->len */
						skb_put(skb, hdr_len);
						payload_len = pdata->rx_buffer_len;
						skb_fill_page_desc(skb, 0,
							buffer->page2, 0,
							payload_len);
					} else {
						/* add header len to first skb->len */
						skb_put(skb, buffer->rx_hdr_size);
						/* No split header, hence
						 * pkt_len = (payload + hdr_len)
						 * */
						payload_len = (pkt_len - buffer->rx_hdr_size);
						skb_fill_page_desc(skb, 0,
							buffer->page2, 0,
							payload_len);
					}
				} else {
					/* this is the middle of a chain */
					payload_len = pdata->rx_buffer_len;
					skb_fill_page_desc(desc_data->skb_top,
						skb_shinfo(desc_data->skb_top)->nr_frags,
						buffer->page2, 0,
						payload_len);

					/* re-use this skb, as consumed only the page */
					buffer->skb = skb;
				}
				eqos_consume_page_split_hdr(buffer,
							 desc_data->skb_top,
							 payload_len, buf2_used);
				goto next_desc;
			} else {
				if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_FD)) {
					buf2_used = 1;
					/* end of the chain */
					if (hdr_len) {
						payload_len = (pkt_len -
							(pdata->rx_buffer_len * intermediate_desc_cnt) -
							hdr_len);
					} else {
						payload_len = (pkt_len -
							(pdata->rx_buffer_len * intermediate_desc_cnt) -
							buffer->rx_hdr_size);
					}

					skb_fill_page_desc(desc_data->skb_top,
						skb_shinfo(desc_data->skb_top)->nr_frags,
						buffer->page2, 0,
						payload_len);

					/* re-use this skb, as consumed only the page */
					buffer->skb = skb;
					skb = desc_data->skb_top;
					desc_data->skb_top = NULL;
					eqos_consume_page_split_hdr(buffer, skb,
								 payload_len, buf2_used);
				} else {
					/* no chain, got both FD + LD together */
					if (hdr_len) {
						buf2_used = 1;
						/* add header len to first skb->len */
						skb_put(skb, hdr_len);

						payload_len = pkt_len - hdr_len;
						skb_fill_page_desc(skb, 0,
							buffer->page2, 0,
							payload_len);
					} else {
						/* No split header, hence
						 * payload_len = (payload + hdr_len)
						 * */
						if (pkt_len > buffer->rx_hdr_size) {
							buf2_used = 1;
							/* add header len to first skb->len */
							skb_put(skb, buffer->rx_hdr_size);

							payload_len = (pkt_len - buffer->rx_hdr_size);
							skb_fill_page_desc(skb, 0,
								buffer->page2, 0,
								payload_len);
						} else {
							buf2_used = 0;
							/* add header len to first skb->len */
							skb_put(skb, pkt_len);
							payload_len = 0; /* no data in page2 */
						}
					}
					eqos_consume_page_split_hdr(buffer,
							skb, payload_len,
							buf2_used);
				}
				/* reset for next new packet/frame */
				intermediate_desc_cnt = 0;
				hdr_len = 0;
			}

			eqos_config_rx_csum(pdata, skb, RX_NORMAL_DESC);

#ifdef EQOS_ENABLE_VLAN_TAG
			eqos_get_rx_vlan(pdata, skb, RX_NORMAL_DESC);
#endif

#ifdef YDEBUG_FILTER
			eqos_check_rx_filter_status(RX_NORMAL_DESC);
#endif

			if ((pdata->hw_feat.tsstssel) && (pdata->hwts_rx_en)) {
				/* get rx tstamp if available */
				if (hw_if->rx_tstamp_available(RX_NORMAL_DESC)) {
					ret = eqos_get_rx_hwtstamp(pdata,
							skb, desc_data, qinx);
					if (ret == 0) {
						/* device has not yet updated the CONTEXT desc to hold the
						 * time stamp, hence delay the packet reception
						 * */
						buffer->skb = skb;
						buffer->dma =
						  dma_map_single(
						      &pdata->pdev->dev,
						      skb->data,
						      ALIGN_SIZE(
							pdata->rx_buffer_len),
						      DMA_FROM_DEVICE);
						if (dma_mapping_error(&pdata->pdev->dev, buffer->dma))
							printk(KERN_ALERT "failed to do the RX dma map\n");

						goto rx_tstmp_failed;
					}
				}
			}

			if (!(dev->features & NETIF_F_GRO) &&
						(dev->features & NETIF_F_LRO)) {
					pdata->tcp_pkt =
							eqos_check_for_tcp_payload(RX_NORMAL_DESC);
			}

			dev->last_rx = jiffies;
			/* update the statistics */
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += skb->len;
			eqos_receive_skb(pdata, dev, skb, qinx);
			received++;
 next_desc:
			desc_data->dirty_rx++;
			if (desc_data->dirty_rx >= desc_data->skb_realloc_threshold)
				desc_if->realloc_skb(pdata, qinx);

			INCR_RX_DESC_INDEX(desc_data->cur_rx, 1);
			buf2_used = 0;
		} else {
			/* no more data to read */
			break;
		}
	}

rx_tstmp_failed:

	if (desc_data->dirty_rx)
		desc_if->realloc_skb(pdata, qinx);

	DBGPR("<--eqos_clean_split_hdr_rx_irq: received = %d\n",
		received);

	return received;
}


/*!
* \brief API to pass the Rx packets to stack if jumbo frame
* is enabled.
*
* \details This function is invoked by main NAPI function if Rx
* jumbe frame is enabled. This function checks the device descriptor
* for the packets and passes it to stack if any packtes are received
* by device.
*
* \param[in] pdata - pointer to private data structure.
* \param[in] quota - maximum no. of packets that we are allowed to pass
* to into the kernel.
* \param[in] qinx - DMA channel/queue no. to be checked for packet.
*
* \return integer
*
* \retval number of packets received.
*/

static int eqos_clean_jumbo_rx_irq(struct eqos_prv_data *pdata,
					  int quota,
					  UINT qinx)
{
	struct eqos_rx_wrapper_descriptor *desc_data =
	    GET_RX_WRAPPER_DESC(qinx);
	struct net_device *dev = pdata->dev;
	struct desc_if_struct *desc_if = &pdata->desc_if;
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct sk_buff *skb = NULL;
	int received = 0;
	struct eqos_rx_buffer *buffer = NULL;
	struct s_rx_normal_desc *RX_NORMAL_DESC = NULL;
	u16 pkt_len;
	UCHAR intermediate_desc_cnt = 0;
	unsigned int buf2_used = 0;
	int ret;
#ifdef HWA_NV_1618922
	UINT err_bits = 0x1200000;
#endif
	DBGPR("-->eqos_clean_jumbo_rx_irq: qinx = %u, quota = %d\n",
		qinx, quota);

	while (received < quota) {
		buffer = GET_RX_BUF_PTR(qinx, desc_data->cur_rx);
		RX_NORMAL_DESC = GET_RX_DESC_PTR(qinx, desc_data->cur_rx);

		/* check for data availability */
		if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_OWN)) {
#ifdef EQOS_ENABLE_RX_DESC_DUMP
			dump_rx_desc(qinx, RX_NORMAL_DESC, desc_data->cur_rx);
#endif
			/* assign it to new skb */
			skb = buffer->skb;
			buffer->skb = NULL;

			/* first buffer pointer */
			dma_unmap_page(&pdata->pdev->dev, buffer->dma,
				       PAGE_SIZE, DMA_FROM_DEVICE);
			buffer->dma = 0;

#ifdef EQOS_DMA_32BIT
			/* second buffer pointer */
			dma_unmap_page(&pdata->pdev->dev, buffer->dma2,
				       PAGE_SIZE, DMA_FROM_DEVICE);
			buffer->dma2 = 0;
#endif
			/* get the packet length */
			pkt_len =
				(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_PL);

			/* check for bad packet,
			 * error is valid only for last descriptor (OWN + LD bit set).
			 * */
#ifdef HWA_NV_1618922
			if ((RX_NORMAL_DESC->RDES3 & err_bits) &&
			    (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD)) {
#else
			if ((RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_ES) &&
			    (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD)) {
#endif
				DBGPR("Error in rcved pkt, failed to pass it to upper layer\n");
				dump_rx_desc(qinx, RX_NORMAL_DESC, desc_data->cur_rx);
				dev->stats.rx_errors++;
				eqos_update_rx_errors(dev,
					RX_NORMAL_DESC->RDES3);

				/* recycle both page and skb */
				buffer->skb = skb;
				if (desc_data->skb_top)
					dev_kfree_skb_any(desc_data->skb_top);

				desc_data->skb_top = NULL;
				goto next_desc;
			}

			if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD)) {
				intermediate_desc_cnt++;
				buf2_used = 1;
				/* this descriptor is only the beginning/middle */
				if (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_FD) {
					/* this is the beginning of a chain */
					desc_data->skb_top = skb;
					skb_fill_page_desc(skb, 0,
						buffer->page, 0,
						pdata->rx_buffer_len);

#ifdef EQOS_DMA_32BIT
					DBGPR("RX: pkt in second buffer pointer\n");
					skb_fill_page_desc(
						desc_data->skb_top,
						skb_shinfo(desc_data->skb_top)->nr_frags,
						buffer->page2, 0,
						pdata->rx_buffer_len);
#endif
				} else {
					/* this is the middle of a chain */
					skb_fill_page_desc(desc_data->skb_top,
						skb_shinfo(desc_data->skb_top)->nr_frags,
						buffer->page, 0,
						pdata->rx_buffer_len);

#ifdef EQOS_DMA_32BIT
					DBGPR("RX: pkt in second buffer pointer\n");
					skb_fill_page_desc(desc_data->skb_top,
						skb_shinfo(desc_data->skb_top)->nr_frags,
						buffer->page2, 0,
						pdata->rx_buffer_len);
#endif
					/* re-use this skb, as consumed only the page */
					buffer->skb = skb;
				}
				eqos_consume_page(buffer,
							 desc_data->skb_top,
							 (pdata->rx_buffer_len * 2),
							 buf2_used);
				goto next_desc;
			} else {
				if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_FD)) {
					/* end of the chain */
					pkt_len =
						(pkt_len - (pdata->rx_buffer_len * intermediate_desc_cnt));
					if (pkt_len > pdata->rx_buffer_len) {
						skb_fill_page_desc(desc_data->skb_top,
							skb_shinfo(desc_data->skb_top)->nr_frags,
							buffer->page, 0,
							pdata->rx_buffer_len);

#ifdef EQOS_DMA_32BIT
						DBGPR("RX: pkt in second buffer pointer\n");
						skb_fill_page_desc(desc_data->skb_top,
							skb_shinfo(desc_data->skb_top)->nr_frags,
							buffer->page2, 0,
							(pkt_len - pdata->rx_buffer_len));
						buf2_used = 1;
#endif
					} else {
						skb_fill_page_desc(desc_data->skb_top,
							skb_shinfo(desc_data->skb_top)->nr_frags,
							buffer->page, 0,
							pkt_len);
						buf2_used = 0;
					}
					/* re-use this skb, as consumed only the page */
					buffer->skb = skb;
					skb = desc_data->skb_top;
					desc_data->skb_top = NULL;
					eqos_consume_page(buffer, skb,
								 pkt_len,
								 buf2_used);
				} else {
					/* no chain, got both FD + LD together */

					/* code added for copybreak, this should improve
					 * performance for small pkts with large amount
					 * of reassembly being done in the stack
					 * */
					if ((pkt_len <= EQOS_COPYBREAK_DEFAULT)
					    && (skb_tailroom(skb) >= pkt_len)) {
						u8 *vaddr;
						vaddr =
						    kmap_atomic(buffer->page);
						memcpy(skb_tail_pointer(skb),
						       vaddr, pkt_len);
						kunmap_atomic(vaddr);
						/* re-use the page, so don't erase buffer->page/page2 */
						skb_put(skb, pkt_len);
					} else {
						if (pkt_len > pdata->rx_buffer_len) {
							skb_fill_page_desc(skb,
								0, buffer->page,
								0,
								pdata->rx_buffer_len);

#ifdef EQOS_DMA_32BIT
							DBGPR ("RX: pkt in second buffer pointer\n");
							skb_fill_page_desc(skb,
								skb_shinfo(skb)->nr_frags, buffer->page2,
								0,
								(pkt_len - pdata->rx_buffer_len));
							buf2_used = 1;
#endif
						} else {
							skb_fill_page_desc(skb,
								0, buffer->page,
								0,
								pkt_len);
							buf2_used = 0;
						}
						eqos_consume_page(buffer,
								skb,
								pkt_len,
								buf2_used);
					}
				}
				intermediate_desc_cnt = 0;
			}

			eqos_config_rx_csum(pdata, skb, RX_NORMAL_DESC);

#ifdef EQOS_ENABLE_VLAN_TAG
			eqos_get_rx_vlan(pdata, skb, RX_NORMAL_DESC);
#endif

#ifdef YDEBUG_FILTER
			eqos_check_rx_filter_status(RX_NORMAL_DESC);
#endif

			if ((pdata->hw_feat.tsstssel) && (pdata->hwts_rx_en)) {
				/* get rx tstamp if available */
				if (hw_if->rx_tstamp_available(RX_NORMAL_DESC)) {
					ret = eqos_get_rx_hwtstamp(pdata,
							skb, desc_data, qinx);
					if (ret == 0) {
						/* device has not yet updated the CONTEXT desc to hold the
						 * time stamp, hence delay the packet reception
						 * */
						buffer->skb = skb;
						buffer->dma =
						  dma_map_single(
						      &pdata->pdev->dev,
						      skb->data,
						      ALIGN_SIZE(
							pdata->rx_buffer_len),
						      DMA_FROM_DEVICE);
						if (dma_mapping_error(&pdata->pdev->dev, buffer->dma))
							printk(KERN_ALERT "failed to do the RX dma map\n");

						goto rx_tstmp_failed;
					}
				}
			}

			if (!(dev->features & NETIF_F_GRO) &&
						(dev->features & NETIF_F_LRO)) {
					pdata->tcp_pkt =
							eqos_check_for_tcp_payload(RX_NORMAL_DESC);
			}

			dev->last_rx = jiffies;
			/* update the statistics */
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += skb->len;

			/* eth type trans needs skb->data to point to something */
			if (!pskb_may_pull(skb, ETH_HLEN)) {
				printk(KERN_ALERT "pskb_may_pull failed\n");
				dev_kfree_skb_any(skb);
				goto next_desc;
			}

			eqos_receive_skb(pdata, dev, skb, qinx);
			received++;
 next_desc:
			desc_data->dirty_rx++;
			if (desc_data->dirty_rx >= desc_data->skb_realloc_threshold)
				desc_if->realloc_skb(pdata, qinx);

			INCR_RX_DESC_INDEX(desc_data->cur_rx, 1);
		} else {
			/* no more data to read */
			break;
		}
	}

rx_tstmp_failed:

	if (desc_data->dirty_rx)
		desc_if->realloc_skb(pdata, qinx);

	DBGPR("<--eqos_clean_jumbo_rx_irq: received = %d\n", received);

	return received;
}

/*!
* \brief API to pass the Rx packets to stack if default mode
* is enabled.
*
* \details This function is invoked by main NAPI function in default
* Rx mode(non jumbo and non split header). This function checks the
* device descriptor for the packets and passes it to stack if any packtes
* are received by device.
*
* \param[in] pdata - pointer to private data structure.
* \param[in] quota - maximum no. of packets that we are allowed to pass
* to into the kernel.
* \param[in] qinx - DMA channel/queue no. to be checked for packet.
*
* \return integer
*
* \retval number of packets received.
*/

static int eqos_clean_rx_irq(struct eqos_prv_data *pdata,
				    int quota,
				    UINT qinx)
{
	struct eqos_rx_wrapper_descriptor *desc_data =
	    GET_RX_WRAPPER_DESC(qinx);
	struct net_device *dev = pdata->dev;
	struct desc_if_struct *desc_if = &pdata->desc_if;
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct sk_buff *skb = NULL;
	int received = 0;
	struct eqos_rx_buffer *buffer = NULL;
	struct s_rx_normal_desc *RX_NORMAL_DESC = NULL;
	UINT pkt_len;
#ifdef HWA_NV_1618922
	UINT err_bits = 0x1200000;
#endif
	int ret;

	DBGPR("-->eqos_clean_rx_irq: qinx = %u, quota = %d\n",
		qinx, quota);

	while (received < quota) {
		buffer = GET_RX_BUF_PTR(qinx, desc_data->cur_rx);
		RX_NORMAL_DESC = GET_RX_DESC_PTR(qinx, desc_data->cur_rx);

		/* check for data availability */
		if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_OWN)) {
#ifdef EQOS_ENABLE_RX_DESC_DUMP
			dump_rx_desc(qinx, RX_NORMAL_DESC, desc_data->cur_rx);
#endif
			/* assign it to new skb */
			skb = buffer->skb;
			buffer->skb = NULL;

			dma_unmap_single(&pdata->pdev->dev, buffer->dma,
					 ALIGN_SIZE(
					   pdata->rx_buffer_len),
					 DMA_FROM_DEVICE);
			buffer->dma = 0;

			/* get the packet length */
			pkt_len =
			    (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_PL);

#ifdef EQOS_ENABLE_RX_PKT_DUMP
			print_pkt(skb, pkt_len, 0, (desc_data->cur_rx));
#endif

#ifdef ENABLE_CHANNEL_DATA_CHECK
	check_channel_data(skb, qinx, 1);
#endif

			/* check for bad/oversized packet,
			 * error is valid only for last descriptor (OWN + LD bit set).
			 * */
#ifdef HWA_NV_1618922
			if (!(RX_NORMAL_DESC->RDES3 & err_bits) &&
			    (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD)) {
#else
			if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_ES) &&
			    (RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD)) {
#endif
				/* pkt_len = pkt_len - 4; */ /* CRC stripping */

				/* code added for copybreak, this should improve
				 * performance for small pkts with large amount
				 * of reassembly being done in the stack
				 * */
				if (pkt_len < EQOS_COPYBREAK_DEFAULT) {
					struct sk_buff *new_skb =
					    netdev_alloc_skb_ip_align(dev,
								      pkt_len);
					if (new_skb) {
						skb_copy_to_linear_data_offset(new_skb,
							-NET_IP_ALIGN,
							(skb->data - NET_IP_ALIGN),
							(pkt_len + NET_IP_ALIGN));
						/* recycle actual desc skb */
						buffer->skb = skb;
						skb = new_skb;
					} else {
						/* just continue with the old skb */
					}
				}
				skb_put(skb, pkt_len);

				eqos_config_rx_csum(pdata, skb,
							RX_NORMAL_DESC);

#ifdef EQOS_ENABLE_VLAN_TAG
				eqos_get_rx_vlan(pdata, skb, RX_NORMAL_DESC);
#endif

#ifdef YDEBUG_FILTER
				eqos_check_rx_filter_status(RX_NORMAL_DESC);
#endif

				if ((pdata->hw_feat.tsstssel) && (pdata->hwts_rx_en)) {
					/* get rx tstamp if available */
					if (hw_if->rx_tstamp_available(RX_NORMAL_DESC)) {
						ret = eqos_get_rx_hwtstamp(pdata,
								skb, desc_data, qinx);
						if (ret == 0) {
							/* device has not yet updated the CONTEXT desc to hold the
							 * time stamp, hence delay the packet reception
							 * */
							buffer->skb = skb;
							buffer->dma =
							  dma_map_single(
							      &pdata->pdev->dev,
							      skb->data,
							      ALIGN_SIZE(
							pdata->rx_buffer_len),
							      DMA_FROM_DEVICE);

							if (dma_mapping_error(&pdata->pdev->dev, buffer->dma))
								printk(KERN_ALERT "failed to do the RX dma map\n");

							goto rx_tstmp_failed;
						}
					}
				}


				if (!(dev->features & NETIF_F_GRO) &&
						(dev->features & NETIF_F_LRO)) {
						pdata->tcp_pkt =
								eqos_check_for_tcp_payload(RX_NORMAL_DESC);
				}

				dev->last_rx = jiffies;
				/* update the statistics */
				dev->stats.rx_packets++;
				dev->stats.rx_bytes += skb->len;
				eqos_receive_skb(pdata, dev, skb, qinx);
				received++;
			} else {
				dump_rx_desc(qinx, RX_NORMAL_DESC, desc_data->cur_rx);
				if (!(RX_NORMAL_DESC->RDES3 & EQOS_RDESC3_LD))
					DBGPR("Received oversized pkt, spanned across multiple desc\n");

				/* recycle skb */
				buffer->skb = skb;
				dev->stats.rx_errors++;
				eqos_update_rx_errors(dev,
					RX_NORMAL_DESC->RDES3);
			}

			desc_data->dirty_rx++;
			if (desc_data->dirty_rx >= desc_data->skb_realloc_threshold)
				desc_if->realloc_skb(pdata, qinx);

			INCR_RX_DESC_INDEX(desc_data->cur_rx, 1);
		} else {
			/* no more data to read */
			break;
		}
	}

rx_tstmp_failed:

	if (desc_data->dirty_rx)
		desc_if->realloc_skb(pdata, qinx);

	DBGPR("<--eqos_clean_rx_irq: received = %d, qinx=%d\n",
		received, qinx);

	return received;
}

/*!
* \brief API to update the rx status.
*
* \details This function is called in poll function to update the
* status of received packets.
*
* \param[in] dev - pointer to net_device structure.
* \param[in] rx_status - value of received packet status.
*
* \return void.
*/

void eqos_update_rx_errors(struct net_device *dev,
				 unsigned int rx_status)
{
	DBGPR("-->eqos_update_rx_errors\n");

	/* received pkt with crc error */
	if ((rx_status & 0x1000000))
		dev->stats.rx_crc_errors++;

	/* received frame alignment */
	if ((rx_status & 0x100000))
		dev->stats.rx_frame_errors++;

	/* receiver fifo overrun */
	if ((rx_status & 0x200000))
		dev->stats.rx_fifo_errors++;

	DBGPR("<--eqos_update_rx_errors\n");
}


int handle_txrx_completions(struct eqos_prv_data *pdata, int qinx)
{
	struct eqos_rx_queue *rx_queue;
	int received = 0;
	int budget = pdata->dt_cfg.chan_napi_quota[qinx];

	DBGPR("-->%s(): chan=%d\n", __func__, qinx);

	rx_queue = GET_RX_QUEUE_PTR(qinx);

	/* check for tx descriptor status */
	eqos_tx_interrupt(pdata->dev, pdata, qinx);
	rx_queue->lro_flush_needed = 0;

#ifdef RX_OLD_CODE
	received = eqos_poll(pdata, budget, qinx);
#else
	received = pdata->clean_rx(pdata, budget, qinx);
#endif
	pdata->xstats.rx_pkt_n += received;
	pdata->xstats.q_rx_pkt_n[qinx] += received;

	if (rx_queue->lro_flush_needed)
		lro_flush_all(&rx_queue->lro_mgr);

	DBGPR("<--%s():\n", __func__);

	return received;
}


static void do_txrx_post_processing(struct eqos_prv_data *pdata,
				struct napi_struct *napi,
				int received,  int budget)
{
	struct eqos_rx_queue *rx_queue;
	int qinx = 0;
	struct hw_if_struct *hw_if = &(pdata->hw_if);

	DBGPR("-->%s():\n", __func__);

	/* If we processed all pkts, we are done;
	 * tell the kernel & re-enable interrupt
	 */
	if (received < budget) {
		if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ) {
			rx_queue =
				container_of(napi,
					struct eqos_rx_queue, napi);
			qinx = rx_queue->chan_num;
			hw_if = &(pdata->hw_if);
		}
		if (pdata->dev->features & NETIF_F_GRO) {
			/* to turn off polling */
			napi_complete(napi);

			/* Enable RX interrupt */
			if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ)
				hw_if->enable_chan_interrupts(qinx, pdata);
			else
				eqos_enable_all_ch_rx_interrpt(pdata);
		} else {
			unsigned long flags;

			spin_lock_irqsave(&pdata->lock, flags);
			__napi_complete(napi);

			/* Enable RX interrupt */
			if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ)
				hw_if->enable_chan_interrupts(qinx, pdata);
			else
				eqos_enable_all_ch_rx_interrpt(pdata);

			spin_unlock_irqrestore(&pdata->lock, flags);
		}
	}
	DBGPR("<--%s():\n", __func__);
}


/*!
* \brief API to pass the received packets to stack
*
* \details This function is provided by NAPI-compliant drivers to operate
* the interface in a polled mode, with interrupts disabled.
*
* \param[in] napi - pointer to napi_stuct structure.
* \param[in] budget - maximum no. of packets that we are allowed to pass
* to into the kernel.
*
* \return integer
*
* \retval number of packets received.
*/

int eqos_poll_mq(struct napi_struct *napi, int budget)
{
	struct eqos_rx_queue *rx_queue =
		container_of(napi, struct eqos_rx_queue, napi);
	struct eqos_prv_data *pdata = rx_queue->pdata;
	int qinx = 0;
	int received = 0;

	DBGPR("-->eqos_poll_mq:\n");

	pdata->xstats.napi_poll_n++;
	for (qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++) {
		received += handle_txrx_completions(pdata, qinx);
	}

	do_txrx_post_processing(pdata, napi, received,
		pdata->napi_quota_all_chans);

	DBGPR("<--eqos_poll_mq\n");

	return received;
}

/* Used only when multi-irq is disabled and all channels are being polled.
 * TYPE_POLL.  Main difference is that is will not enable interrupts.
 */
void eqos_poll_mq_all_chans(struct eqos_prv_data *pdata)
{
	int qinx = 0;

	DBGPR("-->%s():\n", __func__);

	pdata->xstats.napi_poll_n++;
	for (qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++)
		handle_txrx_completions(pdata, qinx);

	DBGPR("<--%s():\n", __func__);
}


/* Used only when multi-irq is enablded.  Used in two cases
 * 1) when one or more channels are configured for TYPE_POLL.
 * 2) Napi handler will call this function for TYPE_NAPI.
 *
 * Main difference is that is will not enable interrupts.  And it
 * will only handle tx/rx completions for one channel.
 */
void eqos_poll_chan_mq_nv(struct chan_data *pchinfo)
{

	struct eqos_prv_data *pdata =
			container_of((pchinfo - pchinfo->chan_num),
			struct eqos_prv_data, chinfo[0]);

	int qinx = pchinfo->chan_num;

	DBGPR("-->%s(): chan=%d\n", __func__, qinx);

	pdata->xstats.napi_poll_n++;

	handle_txrx_completions(pdata, qinx);

	DBGPR("<--%s():\n", __func__);
}

/* Used only when multi-irq is enabled and one or more channel is configured for
 * TYPE_NAPI.
 */
int eqos_poll_mq_napi(struct napi_struct *napi, int budget)
{
	struct eqos_rx_queue *rx_queue =
		container_of(napi, struct eqos_rx_queue, napi);
	struct eqos_prv_data *pdata = rx_queue->pdata;

	int qinx = rx_queue->chan_num;
	int received = 0;


	DBGPR("-->%s(): budget = %d\n", __func__, budget);

	pdata->xstats.napi_poll_n++;
	received = handle_txrx_completions(pdata, qinx);

	do_txrx_post_processing(pdata, napi, received,
			pdata->dt_cfg.chan_napi_quota[qinx]);

	DBGPR("<--%s()\n", __func__);

	return received;
}


/*!
* \brief API to return the device/interface status.
*
* \details The get_stats function is called whenever an application needs to
* get statistics for the interface. For example, this happend when ifconfig
* or netstat -i is run.
*
* \param[in] dev - pointer to net_device structure.
*
* \return net_device_stats structure
*
* \retval net_device_stats - returns pointer to net_device_stats structure.
*/

static struct net_device_stats *eqos_get_stats(struct net_device *dev)
{

	return &dev->stats;
}

#ifdef CONFIG_NET_POLL_CONTROLLER

/*!
* \brief API to receive packets in polling mode.
*
* \details This is polling receive function used by netconsole and other
* diagnostic tool to allow network i/o with interrupts disabled.
*
* \param[in] dev - pointer to net_device structure
*
* \return void
*/

static void eqos_poll_controller(struct net_device *dev)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);

	DBGPR("-->eqos_poll_controller\n");

	disable_irq(pdata->irq_number);
	eqos_isr(pdata->irq_number, pdata);
	enable_irq(pdata->irq_number);

	DBGPR("<--eqos_poll_controller\n");
}

#endif	/*end of CONFIG_NET_POLL_CONTROLLER */

/*!
 * \brief User defined parameter setting API
 *
 * \details This function is invoked by kernel to update the device
 * configuration to new features. This function supports enabling and
 * disabling of TX and RX csum features.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] features – device feature to be enabled/disabled.
 *
 * \return int
 *
 * \retval 0
 */

static int eqos_set_features(struct net_device *dev, netdev_features_t features)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT dev_rxcsum_enable;
#ifdef EQOS_ENABLE_VLAN_TAG
	UINT dev_rxvlan_enable, dev_txvlan_enable;
#endif
	DBGPR("-->eqos_set_features\n");

	if (pdata->hw_feat.rx_coe_sel) {
		dev_rxcsum_enable = !!(pdata->dev_state & NETIF_F_RXCSUM);

		if (((features & NETIF_F_RXCSUM) == NETIF_F_RXCSUM)
		    && !dev_rxcsum_enable) {
			hw_if->enable_rx_csum();
			pdata->dev_state |= NETIF_F_RXCSUM;
			printk(KERN_ALERT "State change - rxcsum enable\n");
		} else if (((features & NETIF_F_RXCSUM) == 0)
			   && dev_rxcsum_enable) {
			hw_if->disable_rx_csum();
			pdata->dev_state &= ~NETIF_F_RXCSUM;
			printk(KERN_ALERT "State change - rxcsum disable\n");
		}
	}
#ifdef EQOS_ENABLE_VLAN_TAG
	dev_rxvlan_enable = !!(pdata->dev_state & NETIF_F_HW_VLAN_CTAG_RX);
	if (((features & NETIF_F_HW_VLAN_CTAG_RX) == NETIF_F_HW_VLAN_CTAG_RX)
	    && !dev_rxvlan_enable) {
		pdata->dev_state |= NETIF_F_HW_VLAN_CTAG_RX;
		hw_if->config_rx_outer_vlan_stripping(EQOS_RX_VLAN_STRIP_ALWAYS);
		printk(KERN_ALERT "State change - rxvlan enable\n");
	} else if (((features & NETIF_F_HW_VLAN_CTAG_RX) == 0) &&
			dev_rxvlan_enable) {
		pdata->dev_state &= ~NETIF_F_HW_VLAN_CTAG_RX;
		hw_if->config_rx_outer_vlan_stripping(EQOS_RX_NO_VLAN_STRIP);
		printk(KERN_ALERT "State change - rxvlan disable\n");
	}

	dev_txvlan_enable = !!(pdata->dev_state & NETIF_F_HW_VLAN_CTAG_TX);
	if (((features & NETIF_F_HW_VLAN_CTAG_TX) == NETIF_F_HW_VLAN_CTAG_TX)
	    && !dev_txvlan_enable) {
		pdata->dev_state |= NETIF_F_HW_VLAN_CTAG_TX;
		printk(KERN_ALERT "State change - txvlan enable\n");
	} else if (((features & NETIF_F_HW_VLAN_CTAG_TX) == 0) &&
			dev_txvlan_enable) {
		pdata->dev_state &= ~NETIF_F_HW_VLAN_CTAG_TX;
		printk(KERN_ALERT "State change - txvlan disable\n");
	}
#endif	/* EQOS_ENABLE_VLAN_TAG */

	DBGPR("<--eqos_set_features\n");

	return 0;
}


/*!
 * \brief User defined parameter setting API
 *
 * \details This function is invoked by kernel to adjusts the requested
 * feature flags according to device-specific constraints, and returns the
 * resulting flags. This API must not modify the device state.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] features – device supported features.
 *
 * \return u32
 *
 * \retval modified flag
 */

static netdev_features_t eqos_fix_features(struct net_device *dev, netdev_features_t features)
{
#ifdef EQOS_ENABLE_VLAN_TAG
	struct eqos_prv_data *pdata = netdev_priv(dev);
#endif
	DBGPR("-->eqos_fix_features: %#llx\n", features);

#ifdef EQOS_ENABLE_VLAN_TAG
	if (pdata->rx_split_hdr) {
		/* The VLAN tag stripping must be set for the split function.
		 * For instance, the DMA separates the header and payload of
		 * an untagged packet only. Hence, when a tagged packet is
		 * received, the QOS must be programmed such that the VLAN
		 * tags are deleted/stripped from the received packets.
		 * */
		features |= NETIF_F_HW_VLAN_CTAG_RX;
	}
#endif /* end of EQOS_ENABLE_VLAN_TAG */

	DBGPR("<--eqos_fix_features: %#llx\n", features);

	return features;
}


/*!
 * \details This function is invoked by ioctl function when user issues
 * an ioctl command to enable/disable receive split header mode.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] flags – flag to indicate whether RX split to be
 *                  enabled/disabled.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_rx_split_hdr_mode(struct net_device *dev,
		unsigned int flags)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	unsigned int qinx;
	int ret = 0;

	DBGPR("-->eqos_config_rx_split_hdr_mode\n");

	if (flags && pdata->rx_split_hdr) {
		printk(KERN_ALERT
			"Rx Split header mode is already enabled\n");
		return -EINVAL;
	}

	if (!flags && !pdata->rx_split_hdr) {
		printk(KERN_ALERT
			"Rx Split header mode is already disabled\n");
		return -EINVAL;
	}

	eqos_stop_dev(pdata);

	/* If split header mode is disabled(ie flags == 0)
	 * then RX will be in default/jumbo mode based on MTU
	 * */
	pdata->rx_split_hdr = !!flags;

	eqos_start_dev(pdata);

	hw_if->config_header_size(EQOS_MAX_HDR_SIZE);
	/* enable/disable split header for all RX DMA channel */
	for (qinx = 0; qinx < EQOS_RX_QUEUE_CNT; qinx++)
		hw_if->config_split_header_mode(qinx, pdata->rx_split_hdr);

	printk(KERN_ALERT "Succesfully %s Rx Split header mode\n",
		(flags ? "enabled" : "disabled"));

	DBGPR("<--eqos_config_rx_split_hdr_mode\n");

	return ret;
}


/*!
 * \details This function is invoked by ioctl function when user issues
 * an ioctl command to enable/disable L3/L4 filtering.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] flags – flag to indicate whether L3/L4 filtering to be
 *                  enabled/disabled.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_l3_l4_filtering(struct net_device *dev,
		unsigned int flags)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	int ret = 0;

	DBGPR_FILTER("-->eqos_config_l3_l4_filtering\n");

	if (flags && pdata->l3_l4_filter) {
		printk(KERN_ALERT
			"L3/L4 filtering is already enabled\n");
		return -EINVAL;
	}

	if (!flags && !pdata->l3_l4_filter) {
		printk(KERN_ALERT
			"L3/L4 filtering is already disabled\n");
		return -EINVAL;
	}

	pdata->l3_l4_filter = !!flags;
	hw_if->config_l3_l4_filter_enable(pdata->l3_l4_filter);

	DBGPR_FILTER("Succesfully %s L3/L4 filtering\n",
		(flags ? "ENABLED" : "DISABLED"));

	DBGPR_FILTER("<--eqos_config_l3_l4_filtering\n");

	return ret;
}


/*!
 * \details This function is invoked by ioctl function when user issues an
 * ioctl command to configure L3(IPv4) filtering. This function does following,
 * - enable/disable IPv4 filtering.
 * - select source/destination address matching.
 * - select perfect/inverse matching.
 * - Update the IPv4 address into MAC register.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] req – pointer to IOCTL specific structure.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_ip4_filters(struct net_device *dev,
		struct ifr_data_struct *req)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct eqos_l3_l4_filter *u_l3_filter =
		(struct eqos_l3_l4_filter *)req->ptr;
	struct eqos_l3_l4_filter l_l3_filter;
	int ret = 0;

	DBGPR_FILTER("-->eqos_config_ip4_filters\n");

	if (pdata->hw_feat.l3l4_filter_num == 0)
		return EQOS_NO_HW_SUPPORT;

	if (copy_from_user(&l_l3_filter, u_l3_filter,
		sizeof(struct eqos_l3_l4_filter)))
		return -EFAULT;

	if ((l_l3_filter.filter_no + 1) > pdata->hw_feat.l3l4_filter_num) {
		printk(KERN_ALERT "%d filter is not supported in the HW\n",
			l_l3_filter.filter_no);
		return EQOS_NO_HW_SUPPORT;
	}

	if (!pdata->l3_l4_filter) {
		hw_if->config_l3_l4_filter_enable(1);
		pdata->l3_l4_filter = 1;
	}

	/* configure the L3 filters */
	hw_if->config_l3_filters(l_l3_filter.filter_no,
			l_l3_filter.filter_enb_dis, 0,
			l_l3_filter.src_dst_addr_match,
			l_l3_filter.perfect_inverse_match);

	if (!l_l3_filter.src_dst_addr_match)
		hw_if->update_ip4_addr0(l_l3_filter.filter_no,
				l_l3_filter.ip4_addr);
	else
		hw_if->update_ip4_addr1(l_l3_filter.filter_no,
				l_l3_filter.ip4_addr);

	DBGPR_FILTER("Successfully %s IPv4 %s %s addressing filtering on %d filter\n",
		(l_l3_filter.filter_enb_dis ? "ENABLED" : "DISABLED"),
		(l_l3_filter.perfect_inverse_match ? "INVERSE" : "PERFECT"),
		(l_l3_filter.src_dst_addr_match ? "DESTINATION" : "SOURCE"),
		l_l3_filter.filter_no);

	DBGPR_FILTER("<--eqos_config_ip4_filters\n");

	return ret;
}


/*!
 * \details This function is invoked by ioctl function when user issues an
 * ioctl command to configure L3(IPv6) filtering. This function does following,
 * - enable/disable IPv6 filtering.
 * - select source/destination address matching.
 * - select perfect/inverse matching.
 * - Update the IPv6 address into MAC register.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] req – pointer to IOCTL specific structure.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_ip6_filters(struct net_device *dev,
		struct ifr_data_struct *req)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct eqos_l3_l4_filter *u_l3_filter =
		(struct eqos_l3_l4_filter *)req->ptr;
	struct eqos_l3_l4_filter l_l3_filter;
	int ret = 0;

	DBGPR_FILTER("-->eqos_config_ip6_filters\n");

	if (pdata->hw_feat.l3l4_filter_num == 0)
		return EQOS_NO_HW_SUPPORT;

	if (copy_from_user(&l_l3_filter, u_l3_filter,
		sizeof(struct eqos_l3_l4_filter)))
		return -EFAULT;

	if ((l_l3_filter.filter_no + 1) > pdata->hw_feat.l3l4_filter_num) {
		printk(KERN_ALERT "%d filter is not supported in the HW\n",
			l_l3_filter.filter_no);
		return EQOS_NO_HW_SUPPORT;
	}

	if (!pdata->l3_l4_filter) {
		hw_if->config_l3_l4_filter_enable(1);
		pdata->l3_l4_filter = 1;
	}

	/* configure the L3 filters */
	hw_if->config_l3_filters(l_l3_filter.filter_no,
			l_l3_filter.filter_enb_dis, 1,
			l_l3_filter.src_dst_addr_match,
			l_l3_filter.perfect_inverse_match);

	hw_if->update_ip6_addr(l_l3_filter.filter_no,
			l_l3_filter.ip6_addr);

	DBGPR_FILTER("Successfully %s IPv6 %s %s addressing filtering on %d filter\n",
		(l_l3_filter.filter_enb_dis ? "ENABLED" : "DISABLED"),
		(l_l3_filter.perfect_inverse_match ? "INVERSE" : "PERFECT"),
		(l_l3_filter.src_dst_addr_match ? "DESTINATION" : "SOURCE"),
		l_l3_filter.filter_no);

	DBGPR_FILTER("<--eqos_config_ip6_filters\n");

	return ret;
}

/*!
 * \details This function is invoked by ioctl function when user issues an
 * ioctl command to configure L4(TCP/UDP) filtering. This function does following,
 * - enable/disable L4 filtering.
 * - select TCP/UDP filtering.
 * - select source/destination port matching.
 * - select perfect/inverse matching.
 * - Update the port number into MAC register.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] req – pointer to IOCTL specific structure.
 * \param[in] tcp_udp – flag to indicate TCP/UDP filtering.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_tcp_udp_filters(struct net_device *dev,
		struct ifr_data_struct *req,
		int tcp_udp)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct eqos_l3_l4_filter *u_l4_filter =
		(struct eqos_l3_l4_filter *)req->ptr;
	struct eqos_l3_l4_filter l_l4_filter;
	int ret = 0;

	DBGPR_FILTER("-->eqos_config_tcp_udp_filters\n");

	if (pdata->hw_feat.l3l4_filter_num == 0)
		return EQOS_NO_HW_SUPPORT;

	if (copy_from_user(&l_l4_filter, u_l4_filter,
		sizeof(struct eqos_l3_l4_filter)))
		return -EFAULT;

	if ((l_l4_filter.filter_no + 1) > pdata->hw_feat.l3l4_filter_num) {
		printk(KERN_ALERT "%d filter is not supported in the HW\n",
			l_l4_filter.filter_no);
		return EQOS_NO_HW_SUPPORT;
	}

	if (!pdata->l3_l4_filter) {
		hw_if->config_l3_l4_filter_enable(1);
		pdata->l3_l4_filter = 1;
	}

	/* configure the L4 filters */
	hw_if->config_l4_filters(l_l4_filter.filter_no,
			l_l4_filter.filter_enb_dis,
			tcp_udp,
			l_l4_filter.src_dst_addr_match,
			l_l4_filter.perfect_inverse_match);

	if (l_l4_filter.src_dst_addr_match)
		hw_if->update_l4_da_port_no(l_l4_filter.filter_no,
				l_l4_filter.port_no);
	else
		hw_if->update_l4_sa_port_no(l_l4_filter.filter_no,
				l_l4_filter.port_no);

	DBGPR_FILTER("Successfully %s %s %s %s Port number filtering on %d filter\n",
		(l_l4_filter.filter_enb_dis ? "ENABLED" : "DISABLED"),
		(tcp_udp ? "UDP" : "TCP"),
		(l_l4_filter.perfect_inverse_match ? "INVERSE" : "PERFECT"),
		(l_l4_filter.src_dst_addr_match ? "DESTINATION" : "SOURCE"),
		l_l4_filter.filter_no);

	DBGPR_FILTER("<--eqos_config_tcp_udp_filters\n");

	return ret;
}


/*!
 * \details This function is invoked by ioctl function when user issues an
 * ioctl command to configure VALN filtering. This function does following,
 * - enable/disable VLAN filtering.
 * - select perfect/hash filtering.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] req – pointer to IOCTL specific structure.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_vlan_filter(struct net_device *dev,
		struct ifr_data_struct *req)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct eqos_vlan_filter *u_vlan_filter =
		(struct eqos_vlan_filter *)req->ptr;
	struct eqos_vlan_filter l_vlan_filter;
	int ret = 0;

	DBGPR_FILTER("-->eqos_config_vlan_filter\n");

	if (copy_from_user(&l_vlan_filter, u_vlan_filter,
		sizeof(struct eqos_vlan_filter)))
		return -EFAULT;

	if ((l_vlan_filter.perfect_hash) &&
		(pdata->hw_feat.vlan_hash_en == 0)) {
		printk(KERN_ALERT "VLAN HASH filtering is not supported\n");
		return EQOS_NO_HW_SUPPORT;
	}

	/* configure the vlan filter */
	hw_if->config_vlan_filtering(l_vlan_filter.filter_enb_dis,
					l_vlan_filter.perfect_hash,
					l_vlan_filter.perfect_inverse_match);
	pdata->vlan_hash_filtering = l_vlan_filter.perfect_hash;

	DBGPR_FILTER("Successfully %s VLAN %s filtering and %s matching\n",
		(l_vlan_filter.filter_enb_dis ? "ENABLED" : "DISABLED"),
		(l_vlan_filter.perfect_hash ? "HASH" : "PERFECT"),
		(l_vlan_filter.perfect_inverse_match ? "INVERSE" : "PERFECT"));

	DBGPR_FILTER("<--eqos_config_vlan_filter\n");

	return ret;
}


/*!
 * \details This function is invoked by ioctl function when user issues an
 * ioctl command to enable/disable ARP offloading feature.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] req – pointer to IOCTL specific structure.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_arp_offload(struct net_device *dev,
		struct ifr_data_struct *req)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct eqos_arp_offload *u_arp_offload =
		(struct eqos_arp_offload *)req->ptr;
	struct eqos_arp_offload l_arp_offload;
	int ret = 0;

	printk(KERN_ALERT "-->eqos_config_arp_offload\n");

	if (pdata->hw_feat.arp_offld_en == 0)
		return EQOS_NO_HW_SUPPORT;

	if (copy_from_user(&l_arp_offload, u_arp_offload,
		sizeof(struct eqos_arp_offload)))
		return -EFAULT;

	/* configure the L3 filters */
	hw_if->config_arp_offload(req->flags);
	hw_if->update_arp_offload_ip_addr(l_arp_offload.ip_addr);
	pdata->arp_offload = req->flags;

	printk(KERN_ALERT "Successfully %s arp Offload\n",
		(req->flags ? "ENABLED" : "DISABLED"));

	printk(KERN_ALERT "<--eqos_config_arp_offload\n");

	return ret;
}


/*!
 * \details This function is invoked by ioctl function when user issues an
 * ioctl command to configure L2 destination addressing filtering mode. This
 * function dose following,
 * - selects perfect/hash filtering.
 * - selects perfect/inverse matching.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] req – pointer to IOCTL specific structure.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_confing_l2_da_filter(struct net_device *dev,
		struct ifr_data_struct *req)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct eqos_l2_da_filter *u_l2_da_filter =
	  (struct eqos_l2_da_filter *)req->ptr;
	struct eqos_l2_da_filter l_l2_da_filter;
	int ret = 0;

	DBGPR_FILTER("-->eqos_confing_l2_da_filter\n");

	if (copy_from_user(&l_l2_da_filter, u_l2_da_filter,
	      sizeof(struct eqos_l2_da_filter)))
		return - EFAULT;

	if (l_l2_da_filter.perfect_hash) {
		if (pdata->hw_feat.hash_tbl_sz > 0)
			pdata->l2_filtering_mode = 1;
		else
			ret = EQOS_NO_HW_SUPPORT;
	} else {
		if (pdata->max_addr_reg_cnt > 1)
			pdata->l2_filtering_mode = 0;
		else
			ret = EQOS_NO_HW_SUPPORT;
	}

	/* configure L2 DA perfect/inverse_matching */
	hw_if->config_l2_da_perfect_inverse_match(l_l2_da_filter.perfect_inverse_match);

	DBGPR_FILTER("Successfully selected L2 %s filtering and %s DA matching\n",
		(l_l2_da_filter.perfect_hash ? "HASH" : "PERFECT"),
		(l_l2_da_filter.perfect_inverse_match ? "INVERSE" : "PERFECT"));

	DBGPR_FILTER("<--eqos_confing_l2_da_filter\n");

	return ret;
}

/*!
 * \details This function is invoked by ioctl function when user issues
 * an ioctl command to enable/disable mac loopback mode.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] flags – flag to indicate whether mac loopback mode to be
 *                  enabled/disabled.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_mac_loopback_mode(struct net_device *dev,
		unsigned int flags)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	int ret = 0;

	DBGPR("-->eqos_config_mac_loopback_mode\n");

	if (flags && pdata->mac_loopback_mode) {
		printk(KERN_ALERT
			"MAC loopback mode is already enabled\n");
		return -EINVAL;
	}
	if (!flags && !pdata->mac_loopback_mode) {
		printk(KERN_ALERT
			"MAC loopback mode is already disabled\n");
		return -EINVAL;
	}
	pdata->mac_loopback_mode = !!flags;
	hw_if->config_mac_loopback_mode(flags);

	printk(KERN_ALERT "Succesfully %s MAC loopback mode\n",
		(flags ? "enabled" : "disabled"));

	DBGPR("<--eqos_config_mac_loopback_mode\n");

	return ret;
}

#ifdef EQOS_ENABLE_DVLAN
static INT config_tx_dvlan_processing_via_reg(struct eqos_prv_data *pdata,
						UINT flags)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);

	printk(KERN_ALERT "--> config_tx_dvlan_processing_via_reg()\n");

	if (pdata->in_out & EQOS_DVLAN_OUTER)
		hw_if->config_tx_outer_vlan(pdata->op_type,
					pdata->outer_vlan_tag);

	if (pdata->in_out & EQOS_DVLAN_INNER)
		hw_if->config_tx_inner_vlan(pdata->op_type,
					pdata->inner_vlan_tag);

	if (flags == EQOS_DVLAN_DISABLE)
		hw_if->config_mac_for_vlan_pkt(); /* restore default configurations */
	else
		hw_if->config_dvlan(1);

	printk(KERN_ALERT "<-- config_tx_dvlan_processing_via_reg()\n");

	return Y_SUCCESS;
}

static int config_tx_dvlan_processing_via_desc(struct eqos_prv_data *pdata,
						UINT flags)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);

	printk(KERN_ALERT "-->config_tx_dvlan_processing_via_desc\n");

	if (flags == EQOS_DVLAN_DISABLE) {
		hw_if->config_mac_for_vlan_pkt(); /* restore default configurations */
		pdata->via_reg_or_desc = 0;
	} else {
		hw_if->config_dvlan(1);
	}

	if (pdata->in_out & EQOS_DVLAN_INNER)
			MAC_IVLANTIRR_VLTI_WR(1);

	if (pdata->in_out & EQOS_DVLAN_OUTER)
			MAC_VLANTIRR_VLTI_WR(1);

	printk(KERN_ALERT "<--config_tx_dvlan_processing_via_desc\n");

	return Y_SUCCESS;
}

/*!
 * \details This function is invoked by ioctl function when user issues
 * an ioctl command to configure mac double vlan processing feature.
 *
 * \param[in] pdata - pointer to private data structure.
 * \param[in] flags – Each bit in this variable carry some information related
 *		      double vlan processing.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_tx_dvlan_processing(
		struct eqos_prv_data *pdata,
		struct ifr_data_struct *req)
{
	struct eqos_config_dvlan l_config_doubule_vlan,
					  *u_config_doubule_vlan = req->ptr;
	int ret = 0;

	DBGPR("-->eqos_config_tx_dvlan_processing\n");

	if(copy_from_user(&l_config_doubule_vlan, u_config_doubule_vlan,
				sizeof(struct eqos_config_dvlan))) {
		printk(KERN_ALERT "Failed to fetch Double vlan Struct info from user\n");
		return EQOS_CONFIG_FAIL;
	}

	pdata->inner_vlan_tag = l_config_doubule_vlan.inner_vlan_tag;
	pdata->outer_vlan_tag = l_config_doubule_vlan.outer_vlan_tag;
	pdata->op_type = l_config_doubule_vlan.op_type;
	pdata->in_out = l_config_doubule_vlan.in_out;
	pdata->via_reg_or_desc = l_config_doubule_vlan.via_reg_or_desc;

	if (pdata->via_reg_or_desc == EQOS_VIA_REG)
		ret = config_tx_dvlan_processing_via_reg(pdata, req->flags);
	else
		ret = config_tx_dvlan_processing_via_desc(pdata, req->flags);

	DBGPR("<--eqos_config_tx_dvlan_processing\n");

	return ret;
}

/*!
 * \details This function is invoked by ioctl function when user issues
 * an ioctl command to configure mac double vlan processing feature.
 *
 * \param[in] pdata - pointer to private data structure.
 * \param[in] flags – Each bit in this variable carry some information related
 *		      double vlan processing.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_rx_dvlan_processing(
		struct eqos_prv_data *pdata, unsigned int flags)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	int ret = 0;

	DBGPR("-->eqos_config_rx_dvlan_processing\n");

	hw_if->config_dvlan(1);
	if (flags == EQOS_DVLAN_NONE) {
		hw_if->config_dvlan(0);
		hw_if->config_rx_outer_vlan_stripping(EQOS_RX_NO_VLAN_STRIP);
		hw_if->config_rx_inner_vlan_stripping(EQOS_RX_NO_VLAN_STRIP);
	} else if (flags == EQOS_DVLAN_INNER) {
		hw_if->config_rx_outer_vlan_stripping(EQOS_RX_NO_VLAN_STRIP);
		hw_if->config_rx_inner_vlan_stripping(EQOS_RX_VLAN_STRIP_ALWAYS);
	} else if (flags == EQOS_DVLAN_OUTER) {
		hw_if->config_rx_outer_vlan_stripping(EQOS_RX_VLAN_STRIP_ALWAYS);
		hw_if->config_rx_inner_vlan_stripping(EQOS_RX_NO_VLAN_STRIP);
	} else if (flags == EQOS_DVLAN_BOTH) {
		hw_if->config_rx_outer_vlan_stripping(EQOS_RX_VLAN_STRIP_ALWAYS);
		hw_if->config_rx_inner_vlan_stripping(EQOS_RX_VLAN_STRIP_ALWAYS);
	} else {
		printk(KERN_ALERT "ERROR : double VLAN Rx configuration - Invalid argument");
		ret = EQOS_CONFIG_FAIL;
	}

	DBGPR("<--eqos_config_rx_dvlan_processing\n");

	return ret;
}

/*!
 * \details This function is invoked by ioctl function when user issues
 * an ioctl command to configure mac double vlan (svlan) processing feature.
 *
 * \param[in] pdata - pointer to private data structure.
 * \param[in] flags – Each bit in this variable carry some information related
 *		      double vlan processing.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_svlan(struct eqos_prv_data *pdata,
					unsigned int flags)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	int ret = 0;

	DBGPR("-->eqos_config_svlan\n");

	ret = hw_if->config_svlan(flags);
	if (ret == Y_FAILURE)
		ret = EQOS_CONFIG_FAIL;

	DBGPR("<--eqos_config_svlan\n");

	return ret;
}
#endif /* end of EQOS_ENABLE_DVLAN */

static VOID eqos_config_timer_registers(
				struct eqos_prv_data *pdata)
{
		struct timespec now;
		struct hw_if_struct *hw_if = &(pdata->hw_if);
		u64 temp;

		DBGPR("-->eqos_config_timer_registers\n");

		/* program Sub Second Increment Reg */
		hw_if->config_sub_second_increment(EQOS_SYSCLOCK);

		/* formula is :
		 * addend = 2^32/freq_div_ratio;
		 *
		 * where, freq_div_ratio = EQOS_SYSCLOCK/50MHz
		 *
		 * hence, addend = ((2^32) * 50MHz)/EQOS_SYSCLOCK;
		 *
		 * NOTE: EQOS_SYSCLOCK should be >= 50MHz to
		 *       achive 20ns accuracy.
		 *
		 * 2^x * y == (y << x), hence
		 * 2^32 * 6250000 ==> (6250000 << 32)
		 * */
		temp = (u64)(62500000ULL << 32);
		pdata->default_addend = div_u64(temp, 125000000);

		hw_if->config_addend(pdata->default_addend);

		/* initialize system time */
		getnstimeofday(&now);
		hw_if->init_systime(now.tv_sec, now.tv_nsec);

		DBGPR("-->eqos_config_timer_registers\n");
}

/*!
 * \details This function is invoked by ioctl function when user issues
 * an ioctl command to configure PTP offloading feature.
 *
 * \param[in] pdata - pointer to private data structure.
 * \param[in] flags – Each bit in this variable carry some information related
 *		      double vlan processing.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_ptpoffload(
		struct eqos_prv_data *pdata,
		struct eqos_config_ptpoffloading *u_conf_ptp)
{
	UINT pto_cntrl;
	UINT mac_tcr;
	struct eqos_config_ptpoffloading l_conf_ptp;
	struct hw_if_struct *hw_if = &(pdata->hw_if);


	if(copy_from_user(&l_conf_ptp, u_conf_ptp,
				sizeof(struct eqos_config_ptpoffloading))) {
		printk(KERN_ALERT "Failed to fetch Double vlan Struct info from user\n");
		return EQOS_CONFIG_FAIL;
	}

	printk(KERN_ALERT"-->eqos_config_ptpoffload - %d\n",l_conf_ptp.mode);

	pto_cntrl = MAC_PTOCR_PTOEN; /* enable ptp offloading */
	mac_tcr = MAC_TCR_TSENA | MAC_TCR_TSIPENA | MAC_TCR_TSVER2ENA
			| MAC_TCR_TSCFUPDT | MAC_TCR_TSCTRLSSR;
	if (l_conf_ptp.mode == EQOS_PTP_ORDINARY_SLAVE) {

		mac_tcr |= MAC_TCR_TSEVENTENA;
		pdata->ptp_offloading_mode = EQOS_PTP_ORDINARY_SLAVE;

	} else if (l_conf_ptp.mode == EQOS_PTP_TRASPARENT_SLAVE) {

		pto_cntrl |= MAC_PTOCR_APDREQEN;
		mac_tcr |= MAC_TCR_TSEVENTENA;
		mac_tcr |= MAC_TCR_SNAPTYPSEL_1;
		pdata->ptp_offloading_mode =
			EQOS_PTP_TRASPARENT_SLAVE;

	} else if (l_conf_ptp.mode == EQOS_PTP_ORDINARY_MASTER) {

		pto_cntrl |= MAC_PTOCR_ASYNCEN;
		mac_tcr |= MAC_TCR_TSEVENTENA;
		mac_tcr |= MAC_TCR_TSMASTERENA;
		pdata->ptp_offloading_mode = EQOS_PTP_ORDINARY_MASTER;

	} else if(l_conf_ptp.mode == EQOS_PTP_TRASPARENT_MASTER) {

		pto_cntrl |= MAC_PTOCR_ASYNCEN | MAC_PTOCR_APDREQEN;
		mac_tcr |= MAC_TCR_SNAPTYPSEL_1;
		mac_tcr |= MAC_TCR_TSEVENTENA;
		mac_tcr |= MAC_TCR_TSMASTERENA;
		pdata->ptp_offloading_mode =
			EQOS_PTP_TRASPARENT_MASTER;

	} else if (l_conf_ptp.mode == EQOS_PTP_PEER_TO_PEER_TRANSPARENT) {

		pto_cntrl |= MAC_PTOCR_APDREQEN;
		mac_tcr |= MAC_TCR_SNAPTYPSEL_3;
		pdata->ptp_offloading_mode =
			EQOS_PTP_PEER_TO_PEER_TRANSPARENT;
	}

	pdata->ptp_offload = 1;
	if (l_conf_ptp.en_dis == EQOS_PTP_OFFLOADING_DISABLE) {
		pto_cntrl = 0;
		mac_tcr = 0;
		pdata->ptp_offload = 0;
	}

	pto_cntrl |= (l_conf_ptp.domain_num << 8);
	hw_if->config_hw_time_stamping(mac_tcr);
	eqos_config_timer_registers(pdata);
	hw_if->config_ptpoffload_engine(pto_cntrl, l_conf_ptp.mc_uc);

	printk(KERN_ALERT"<--eqos_config_ptpoffload\n");

	return Y_SUCCESS;
}









/*!
 * \details This function is invoked by ioctl function when user issues
 * an ioctl command to enable/disable pfc.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] flags – flag to indicate whether pfc to be enabled/disabled.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int eqos_config_pfc(struct net_device *dev,
		unsigned int flags)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	int ret = 0;

	DBGPR("-->eqos_config_pfc\n");

	if (!pdata->hw_feat.dcb_en) {
		printk(KERN_ALERT "PFC is not supported\n");
		return EQOS_NO_HW_SUPPORT;
	}

	hw_if->config_pfc(flags);

	printk(KERN_ALERT "Succesfully %s PFC(Priority Based Flow Control)\n",
		(flags ? "enabled" : "disabled"));

	DBGPR("<--eqos_config_pfc\n");

	return ret;
}

/*!
 * \brief Driver IOCTL routine
 *
 * \details This function is invoked by main ioctl function when
 * users request to configure various device features like,
 * PMT module, TX and RX PBL, TX and RX FIFO threshold level,
 * TX and RX OSF mode, SA insert/replacement, L2/L3/L4 and
 * VLAN filtering, AVB/DCB algorithm etc.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] req – pointer to ioctl structure.
 *
 * \return int
 *
 * \retval 0 - success
 * \retval negative - failure
 */

static int eqos_handle_prv_ioctl(struct eqos_prv_data *pdata,
					struct ifr_data_struct *req)
{
	unsigned int qinx = req->qinx;
	struct eqos_tx_wrapper_descriptor *tx_desc_data =
	    GET_TX_WRAPPER_DESC(qinx);
	struct eqos_rx_wrapper_descriptor *rx_desc_data =
	    GET_RX_WRAPPER_DESC(qinx);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct net_device *dev = pdata->dev;
	int ret = 0;

	DBGPR("-->eqos_handle_prv_ioctl\n");

	if (qinx > EQOS_QUEUE_CNT) {
		printk(KERN_ALERT "Queue number %d is invalid\n" \
				"Hardware has only %d Tx/Rx Queues\n",
				qinx, EQOS_QUEUE_CNT);
		ret = EQOS_NO_HW_SUPPORT;
		return ret;
	}

	switch (req->cmd) {
	case EQOS_POWERUP_MAGIC_CMD:
		if (pdata->hw_feat.mgk_sel) {
			ret = eqos_powerup(dev, EQOS_IOCTL_CONTEXT);
			if (ret == 0)
				ret = EQOS_CONFIG_SUCCESS;
			else
				ret = EQOS_CONFIG_FAIL;
		} else {
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_POWERDOWN_MAGIC_CMD:
		if (pdata->hw_feat.mgk_sel) {
			ret =
			  eqos_powerdown(dev,
			    EQOS_MAGIC_WAKEUP, EQOS_IOCTL_CONTEXT);
			if (ret == 0)
				ret = EQOS_CONFIG_SUCCESS;
			else
				ret = EQOS_CONFIG_FAIL;
		} else {
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_POWERUP_REMOTE_WAKEUP_CMD:
		if (pdata->hw_feat.rwk_sel) {
			ret = eqos_powerup(dev, EQOS_IOCTL_CONTEXT);
			if (ret == 0)
				ret = EQOS_CONFIG_SUCCESS;
			else
				ret = EQOS_CONFIG_FAIL;
		} else {
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_POWERDOWN_REMOTE_WAKEUP_CMD:
		if (pdata->hw_feat.rwk_sel) {
			ret = eqos_configure_remotewakeup(dev, req);
			if (ret == 0)
				ret = EQOS_CONFIG_SUCCESS;
			else
				ret = EQOS_CONFIG_FAIL;
		} else {
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_RX_THRESHOLD_CMD:
		rx_desc_data->rx_threshold_val = req->flags;
		hw_if->config_rx_threshold(qinx,
					rx_desc_data->rx_threshold_val);
		printk(KERN_ALERT "Configured Rx threshold with %d\n",
		       rx_desc_data->rx_threshold_val);
		break;

	case EQOS_TX_THRESHOLD_CMD:
		tx_desc_data->tx_threshold_val = req->flags;
		hw_if->config_tx_threshold(qinx,
					tx_desc_data->tx_threshold_val);
		printk(KERN_ALERT "Configured Tx threshold with %d\n",
		       tx_desc_data->tx_threshold_val);
		break;

	case EQOS_RSF_CMD:
		rx_desc_data->rsf_on = req->flags;
		hw_if->config_rsf_mode(qinx, rx_desc_data->rsf_on);
		printk(KERN_ALERT "Receive store and forward mode %s\n",
		       (rx_desc_data->rsf_on) ? "enabled" : "disabled");
		break;

	case EQOS_TSF_CMD:
		tx_desc_data->tsf_on = req->flags;
		hw_if->config_tsf_mode(qinx, tx_desc_data->tsf_on);
		printk(KERN_ALERT "Transmit store and forward mode %s\n",
		       (tx_desc_data->tsf_on) ? "enabled" : "disabled");
		break;

	case EQOS_OSF_CMD:
		tx_desc_data->osf_on = req->flags;
		hw_if->config_osf_mode(qinx, tx_desc_data->osf_on);
		printk(KERN_ALERT "Transmit DMA OSF mode is %s\n",
		       (tx_desc_data->osf_on) ? "enabled" : "disabled");
		break;

	case EQOS_INCR_INCRX_CMD:
		pdata->incr_incrx = req->flags;
		hw_if->config_incr_incrx_mode(pdata->incr_incrx);
		printk(KERN_ALERT "%s mode is enabled\n",
		       (pdata->incr_incrx) ? "INCRX" : "INCR");
		break;

	case EQOS_RX_PBL_CMD:
		rx_desc_data->rx_pbl = req->flags;
		eqos_config_rx_pbl(pdata, rx_desc_data->rx_pbl, qinx);
		break;

	case EQOS_TX_PBL_CMD:
		tx_desc_data->tx_pbl = req->flags;
		eqos_config_tx_pbl(pdata, tx_desc_data->tx_pbl, qinx);
		break;

#ifdef EQOS_ENABLE_DVLAN
	case EQOS_DVLAN_TX_PROCESSING_CMD:
		if (pdata->hw_feat.sa_vlan_ins) {
			ret = eqos_config_tx_dvlan_processing(pdata, req);
		} else {
			printk(KERN_ALERT "No HW support for Single/Double VLAN\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;
	case EQOS_DVLAN_RX_PROCESSING_CMD:
		if (pdata->hw_feat.sa_vlan_ins) {
			ret = eqos_config_rx_dvlan_processing(pdata, req->flags);
		} else {
			printk(KERN_ALERT "No HW support for Single/Double VLAN\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;
	case EQOS_SVLAN_CMD:
		if (pdata->hw_feat.sa_vlan_ins) {
			ret = eqos_config_svlan(pdata, req->flags);
		} else {
			printk(KERN_ALERT "No HW support for Single/Double VLAN\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;
#endif /* end of EQOS_ENABLE_DVLAN */
	case EQOS_PTPOFFLOADING_CMD:
		if (pdata->hw_feat.tsstssel) {
			ret = eqos_config_ptpoffload(pdata,
					req->ptr);
		} else {
			printk(KERN_ALERT "No HW support for PTP\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_SA0_DESC_CMD:
		if (pdata->hw_feat.sa_vlan_ins) {
			pdata->tx_sa_ctrl_via_desc = req->flags;
			pdata->tx_sa_ctrl_via_reg = EQOS_SA0_NONE;
			if (req->flags == EQOS_SA0_NONE) {
				memcpy(pdata->mac_addr, pdata->dev->dev_addr,
				       EQOS_MAC_ADDR_LEN);
			} else {
				memcpy(pdata->mac_addr, mac_addr0,
				       EQOS_MAC_ADDR_LEN);
			}
			hw_if->configure_mac_addr0_reg(pdata->mac_addr);
			hw_if->configure_sa_via_reg(pdata->tx_sa_ctrl_via_reg);
			printk(KERN_ALERT
			       "SA will use MAC0 with descriptor for configuration %d\n",
			       pdata->tx_sa_ctrl_via_desc);
		} else {
			printk(KERN_ALERT
			       "Device doesn't supports SA Insertion/Replacement\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_SA1_DESC_CMD:
		if (pdata->hw_feat.sa_vlan_ins) {
			pdata->tx_sa_ctrl_via_desc = req->flags;
			pdata->tx_sa_ctrl_via_reg = EQOS_SA1_NONE;
			if (req->flags == EQOS_SA1_NONE) {
				memcpy(pdata->mac_addr, pdata->dev->dev_addr,
				       EQOS_MAC_ADDR_LEN);
			} else {
				memcpy(pdata->mac_addr, mac_addr1,
				       EQOS_MAC_ADDR_LEN);
			}
			hw_if->configure_mac_addr1_reg(pdata->mac_addr);
			hw_if->configure_sa_via_reg(pdata->tx_sa_ctrl_via_reg);
			printk(KERN_ALERT
			       "SA will use MAC1 with descriptor for configuration %d\n",
			       pdata->tx_sa_ctrl_via_desc);
		} else {
			printk(KERN_ALERT
			       "Device doesn't supports SA Insertion/Replacement\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_SA0_REG_CMD:
		if (pdata->hw_feat.sa_vlan_ins) {
			pdata->tx_sa_ctrl_via_reg = req->flags;
			pdata->tx_sa_ctrl_via_desc = EQOS_SA0_NONE;
			if (req->flags == EQOS_SA0_NONE) {
				memcpy(pdata->mac_addr, pdata->dev->dev_addr,
				       EQOS_MAC_ADDR_LEN);
			} else {
				memcpy(pdata->mac_addr, mac_addr0,
				       EQOS_MAC_ADDR_LEN);
			}
			hw_if->configure_mac_addr0_reg(pdata->mac_addr);
			hw_if->configure_sa_via_reg(pdata->tx_sa_ctrl_via_reg);
			printk(KERN_ALERT
			       "SA will use MAC0 with register for configuration %d\n",
			       pdata->tx_sa_ctrl_via_desc);
		} else {
			printk(KERN_ALERT
			       "Device doesn't supports SA Insertion/Replacement\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_SA1_REG_CMD:
		if (pdata->hw_feat.sa_vlan_ins) {
			pdata->tx_sa_ctrl_via_reg = req->flags;
			pdata->tx_sa_ctrl_via_desc = EQOS_SA1_NONE;
			if (req->flags == EQOS_SA1_NONE) {
				memcpy(pdata->mac_addr, pdata->dev->dev_addr,
				       EQOS_MAC_ADDR_LEN);
			} else {
				memcpy(pdata->mac_addr, mac_addr1,
				       EQOS_MAC_ADDR_LEN);
			}
			hw_if->configure_mac_addr1_reg(pdata->mac_addr);
			hw_if->configure_sa_via_reg(pdata->tx_sa_ctrl_via_reg);
			printk(KERN_ALERT
			       "SA will use MAC1 with register for configuration %d\n",
			       pdata->tx_sa_ctrl_via_desc);
		} else {
			printk(KERN_ALERT
			       "Device doesn't supports SA Insertion/Replacement\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_SETUP_CONTEXT_DESCRIPTOR:
		if (pdata->hw_feat.sa_vlan_ins) {
			tx_desc_data->context_setup = req->context_setup;
			if (tx_desc_data->context_setup == 1) {
				printk(KERN_ALERT "Context descriptor will be transmitted"\
						" with every normal descriptor on %d DMA Channel\n",
						qinx);
			}
			else {
				printk(KERN_ALERT "Context descriptor will be setup"\
						" only if VLAN id changes %d\n", qinx);
			}
		}
		else {
			printk(KERN_ALERT
			       "Device doesn't support VLAN operations\n");
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;

	case EQOS_GET_RX_QCNT:
		req->qinx = EQOS_RX_QUEUE_CNT;
		break;

	case EQOS_GET_TX_QCNT:
		req->qinx = EQOS_TX_QUEUE_CNT;
		break;

	case EQOS_GET_CONNECTED_SPEED:
		req->connected_speed = pdata->speed;
		break;

	case EQOS_DCB_ALGORITHM:
		eqos_program_dcb_algorithm(pdata, req);
		break;

	case EQOS_AVB_ALGORITHM:
		eqos_program_avb_algorithm(pdata, req);
		break;

	case EQOS_RX_SPLIT_HDR_CMD:
		if (pdata->hw_feat.sph_en) {
			ret = eqos_config_rx_split_hdr_mode(dev, req->flags);
			if (ret == 0)
				ret = EQOS_CONFIG_SUCCESS;
			else
				ret = EQOS_CONFIG_FAIL;
		} else {
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;
	case EQOS_L3_L4_FILTER_CMD:
		if (pdata->hw_feat.l3l4_filter_num > 0) {
			ret = eqos_config_l3_l4_filtering(dev, req->flags);
			if (ret == 0)
				ret = EQOS_CONFIG_SUCCESS;
			else
				ret = EQOS_CONFIG_FAIL;
		} else {
			ret = EQOS_NO_HW_SUPPORT;
		}
		break;
	case EQOS_IPV4_FILTERING_CMD:
		ret = eqos_config_ip4_filters(dev, req);
		break;
	case EQOS_IPV6_FILTERING_CMD:
		ret = eqos_config_ip6_filters(dev, req);
		break;
	case EQOS_UDP_FILTERING_CMD:
		ret = eqos_config_tcp_udp_filters(dev, req, 1);
		break;
	case EQOS_TCP_FILTERING_CMD:
		ret = eqos_config_tcp_udp_filters(dev, req, 0);
		break;
	case EQOS_VLAN_FILTERING_CMD:
		ret = eqos_config_vlan_filter(dev, req);
		break;
	case EQOS_L2_DA_FILTERING_CMD:
		ret = eqos_confing_l2_da_filter(dev, req);
		break;
	case EQOS_ARP_OFFLOAD_CMD:
		ret = eqos_config_arp_offload(dev, req);
		break;
	case EQOS_AXI_PBL_CMD:
		pdata->axi_pbl = req->flags;
		hw_if->config_axi_pbl_val(pdata->axi_pbl);
		printk(KERN_ALERT "AXI PBL value: %d\n", pdata->axi_pbl);
		break;
	case EQOS_AXI_WORL_CMD:
		pdata->axi_worl = req->flags;
		hw_if->config_axi_worl_val(pdata->axi_worl);
		printk(KERN_ALERT "AXI WORL value: %d\n", pdata->axi_worl);
		break;
	case EQOS_AXI_RORL_CMD:
		pdata->axi_rorl = req->flags;
		hw_if->config_axi_rorl_val(pdata->axi_rorl);
		printk(KERN_ALERT "AXI RORL value: %d\n", pdata->axi_rorl);
		break;
	case EQOS_MAC_LOOPBACK_MODE_CMD:
		ret = eqos_config_mac_loopback_mode(dev, req->flags);
		if (ret == 0)
			ret = EQOS_CONFIG_SUCCESS;
		else
			ret = EQOS_CONFIG_FAIL;
		break;
	case EQOS_PFC_CMD:
		ret = eqos_config_pfc(dev, req->flags);
		break;
#ifdef EQOS_CONFIG_PGTEST
	case EQOS_PG_TEST:
		ret = eqos_handle_pg_ioctl(pdata, (void *)req);
		break;
#endif /* end of EQOS_CONFIG_PGTEST */
	case EQOS_PHY_LOOPBACK:
		ret = eqos_handle_phy_loopback(pdata, (void *)req);
		break;
	case EQOS_MEM_ISO_TEST:
		ret = eqos_handle_mem_iso_ioctl(pdata, (void *)req);
		break;
	case EQOS_CSR_ISO_TEST:
		ret = eqos_handle_csr_iso_ioctl(pdata, (void *)req);
		break;
	default:
		ret = -EOPNOTSUPP;
		printk(KERN_ALERT "Unsupported command call\n");
	}

	DBGPR("<--eqos_handle_prv_ioctl\n");

	return ret;
}


/*!
 * \brief control hw timestamping.
 *
 * \details This function is used to configure the MAC to enable/disable both
 * outgoing(Tx) and incoming(Rx) packets time stamping based on user input.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] ifr – pointer to IOCTL specific structure.
 *
 * \return int
 *
 * \retval 0 - success
 * \retval negative - failure
 */

static int eqos_handle_hwtstamp_ioctl(struct eqos_prv_data *pdata,
	struct ifreq *ifr)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	struct hwtstamp_config config;
	u32 ptp_v2 = 0;
	u32 tstamp_all = 0;
	u32 ptp_over_ipv4_udp = 0;
	u32 ptp_over_ipv6_udp = 0;
	u32 ptp_over_ethernet = 0;
	u32 snap_type_sel = 0;
	u32 ts_master_en = 0;
	u32 ts_event_en = 0;
	u32 av_8021asm_en = 0;
	u32 mac_tcr = 0;
	u64 temp = 0;
	struct timespec now;

	DBGPR_PTP("-->eqos_handle_hwtstamp_ioctl\n");

	if (!pdata->hw_feat.tsstssel) {
		printk(KERN_ALERT "No hw timestamping is available in this core\n");
		return -EOPNOTSUPP;
	}

	if (copy_from_user(&config, ifr->ifr_data,
		sizeof(struct hwtstamp_config)))
		return -EFAULT;

	DBGPR_PTP("config.flags = %#x, tx_type = %#x, rx_filter = %#x\n",
		config.flags, config.tx_type, config.rx_filter);

	/* reserved for future extensions */
	if (config.flags)
		return -EINVAL;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		pdata->hwts_tx_en = 0;
		break;
	case HWTSTAMP_TX_ON:
		pdata->hwts_tx_en = 1;
		break;
	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	/* time stamp no incoming packet at all */
	case HWTSTAMP_FILTER_NONE:
		config.rx_filter = HWTSTAMP_FILTER_NONE;
		break;

	/* PTP v1, UDP, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_EVENT;
		/* take time stamp for all event messages */
		snap_type_sel = MAC_TCR_SNAPTYPSEL_1;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v1, UDP, Sync packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_SYNC;
		/* take time stamp for SYNC messages only */
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v1, UDP, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ;
		/* take time stamp for Delay_Req messages only */
		ts_master_en = MAC_TCR_TSMASTERENA;
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v2, UDP, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for all event messages */
		snap_type_sel = MAC_TCR_SNAPTYPSEL_1;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v2, UDP, Sync packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_SYNC;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for SYNC messages only */
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v2, UDP, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for Delay_Req messages only */
		ts_master_en = MAC_TCR_TSMASTERENA;
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v2/802.AS1, any layer, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for all event messages */
		snap_type_sel = MAC_TCR_SNAPTYPSEL_1;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		ptp_over_ethernet = MAC_TCR_TSIPENA;
		/* for VLAN tagged PTP, AV8021ASMEN bit should not be set */
#ifdef DWC_1588_VLAN_UNTAGGED
		av_8021asm_en = MAC_TCR_AV8021ASMEN;
#endif
		break;

	/* PTP v2/802.AS1, any layer, Sync packet */
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_SYNC;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for SYNC messages only */
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		ptp_over_ethernet = MAC_TCR_TSIPENA;
		av_8021asm_en = MAC_TCR_AV8021ASMEN;
		break;

	/* PTP v2/802.AS1, any layer, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_DELAY_REQ;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for Delay_Req messages only */
		ts_master_en = MAC_TCR_TSMASTERENA;
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		ptp_over_ethernet = MAC_TCR_TSIPENA;
		av_8021asm_en = MAC_TCR_AV8021ASMEN;
		break;

	/* time stamp any incoming packet */
	case HWTSTAMP_FILTER_ALL:
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		tstamp_all = MAC_TCR_TSENALL;
		break;

	default:
		return -ERANGE;
	}
	pdata->hwts_rx_en = ((config.rx_filter == HWTSTAMP_FILTER_NONE) ? 0 : 1);

	if (!pdata->hwts_tx_en && !pdata->hwts_rx_en) {
		/* disable hw time stamping */
		hw_if->config_hw_time_stamping(mac_tcr);
	} else {
		mac_tcr = (MAC_TCR_TSENA | MAC_TCR_TSCFUPDT | MAC_TCR_TSCTRLSSR |
				tstamp_all | ptp_v2 | ptp_over_ethernet | ptp_over_ipv6_udp |
				ptp_over_ipv4_udp | ts_event_en | ts_master_en |
				snap_type_sel | av_8021asm_en);

		if (!pdata->one_nsec_accuracy)
			mac_tcr &= ~MAC_TCR_TSCTRLSSR;

		hw_if->config_hw_time_stamping(mac_tcr);

		/* program Sub Second Increment Reg */
		hw_if->config_sub_second_increment(EQOS_SYSCLOCK);

		/* formula is :
		 * addend = 2^32/freq_div_ratio;
		 *
		 * where, freq_div_ratio = EQOS_SYSCLOCK/50MHz
		 *
		 * hence, addend = ((2^32) * 50MHz)/EQOS_SYSCLOCK;
		 *
		 * NOTE: EQOS_SYSCLOCK should be >= 50MHz to
		 *       achive 20ns accuracy.
		 *
		 * 2^x * y == (y << x), hence
		 * 2^32 * 6250000 ==> (6250000 << 32)
		 * */
		temp = (u64)(62500000ULL << 32);
		pdata->default_addend = div_u64(temp, 125000000);

		hw_if->config_addend(pdata->default_addend);

		/* initialize system time */
		getnstimeofday(&now);
		hw_if->init_systime(now.tv_sec, now.tv_nsec);
	}

	DBGPR_PTP("config.flags = %#x, tx_type = %#x, rx_filter = %#x\n",
		config.flags, config.tx_type, config.rx_filter);

	DBGPR_PTP("<--eqos_handle_hwtstamp_ioctl\n");

	return (copy_to_user(ifr->ifr_data, &config,
		sizeof(struct hwtstamp_config))) ? -EFAULT : 0;
}


/*!
 * \brief Driver IOCTL routine
 *
 * \details This function is invoked by kernel when a user request an ioctl
 * which can't be handled by the generic interface code. Following operations
 * are performed in this functions.
 * - Configuring the PMT module.
 * - Configuring TX and RX PBL.
 * - Configuring the TX and RX FIFO threshold level.
 * - Configuring the TX and RX OSF mode.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] ifr – pointer to IOCTL specific structure.
 * \param[in] cmd – IOCTL command.
 *
 * \return int
 *
 * \retval 0 - success
 * \retval negative - failure
 */

static int eqos_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct ifr_data_struct *req = ifr->ifr_ifru.ifru_data;
	struct mii_ioctl_data *data = if_mii(ifr);
	unsigned int reg_val = 0;
	int ret = 0;

	DBGPR("-->eqos_ioctl\n");

#ifndef EQOS_CONFIG_PGTEST
	if ((!netif_running(dev)) || (!pdata->phydev)) {
		DBGPR("<--eqos_ioctl - error\n");
		return -EINVAL;
	}
#endif

	spin_lock(&pdata->lock);
	switch (cmd) {
	case SIOCGMIIPHY:
		data->phy_id = pdata->phyaddr;
		printk(KERN_ALERT "PHY ID: SIOCGMIIPHY\n");
		break;

	case SIOCGMIIREG:
		ret =
		    eqos_mdio_read_direct(pdata, pdata->phyaddr,
				(data->reg_num & 0x1F), &reg_val);
		if (ret)
			ret = -EIO;

		data->val_out = reg_val;
		printk(KERN_ALERT "PHY ID: SIOCGMIIREG reg:%#x reg_val:%#x\n",
		       (data->reg_num & 0x1F), reg_val);
		break;

	case SIOCSMIIREG:
		printk(KERN_ALERT "PHY ID: SIOCSMIIPHY\n");
		break;

	case EQOS_PRV_IOCTL:
		ret = eqos_handle_prv_ioctl(pdata, req);
		req->command_error = ret;
		break;

	case SIOCSHWTSTAMP:
		ret = eqos_handle_hwtstamp_ioctl(pdata, ifr);
		break;

	default:
		ret = -EOPNOTSUPP;
		printk(KERN_ALERT "Unsupported IOCTL call\n");
	}
	spin_unlock(&pdata->lock);

	DBGPR("<--eqos_ioctl\n");

	return ret;
}

/*!
* \brief API to change MTU.
*
* \details This function is invoked by upper layer when user changes
* MTU (Maximum Transfer Unit). The MTU is used by the Network layer
* to driver packet transmission. Ethernet has a default MTU of
* 1500Bytes. This value can be changed with ifconfig -
* ifconfig <interface_name> mtu <new_mtu_value>
*
* \param[in] dev - pointer to net_device structure
* \param[in] new_mtu - the new MTU for the device.
*
* \return integer
*
* \retval 0 - on success and -ve on failure.
*/

static INT eqos_change_mtu(struct net_device *dev, INT new_mtu)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	int max_frame = (new_mtu + ETH_HLEN + ETH_FCS_LEN + VLAN_HLEN);

	DBGPR("-->eqos_change_mtu: new_mtu:%d\n", new_mtu);

#ifdef EQOS_CONFIG_PGTEST
	printk(KERN_ALERT "jumbo frames not supported with PG test\n");
	return -EOPNOTSUPP;
#endif
	if (dev->mtu == new_mtu) {
		printk(KERN_ALERT "%s: is already configured to %d mtu\n",
		       dev->name, new_mtu);
		return 0;
	}

	/* Supported frame sizes */
	if ((new_mtu < EQOS_MIN_SUPPORTED_MTU) ||
	    (max_frame > EQOS_MAX_SUPPORTED_MTU)) {
		printk(KERN_ALERT
		       "%s: invalid MTU, min %d and max %d MTU are supported\n",
		       dev->name, EQOS_MIN_SUPPORTED_MTU,
		       EQOS_MAX_SUPPORTED_MTU);
		return -EINVAL;
	}

	printk(KERN_ALERT "changing MTU from %d to %d\n", dev->mtu, new_mtu);

	eqos_stop_dev(pdata);

	if (max_frame <= 2048)
		pdata->rx_buffer_len = 2048;
	else
		pdata->rx_buffer_len = PAGE_SIZE; /* in case of JUMBO frame,
						max buffer allocated is
						PAGE_SIZE */

	if ((max_frame == ETH_FRAME_LEN + ETH_FCS_LEN) ||
	    (max_frame == ETH_FRAME_LEN + ETH_FCS_LEN + VLAN_HLEN))
		pdata->rx_buffer_len =
		    EQOS_ETH_FRAME_LEN;

	dev->mtu = new_mtu;

	eqos_start_dev(pdata);

	DBGPR("<--eqos_change_mtu\n");

	return 0;
}

#ifdef EQOS_QUEUE_SELECT_ALGO
u16 eqos_select_queue(struct net_device *dev,
		struct sk_buff *skb, void *accel_priv,
		select_queue_fallback_t fallback)
{
	static u16 txqueue_select = 0;
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct eqos_cfg *pdt_cfg = (struct eqos_cfg *)&pdata->dt_cfg;
	UINT i;

	DBGPR("-->eqos_select_queue\n");

	for (i = 0; i <= EQOS_TX_QUEUE_CNT; i++) {
		if (pdt_cfg->q_prio[i] == skb->priority) {
			txqueue_select = i;
			break;
		}
	}

	DBGPR("<--eqos_select_queue txqueue-select:%d\n",
		txqueue_select);

	return txqueue_select;
}
#endif

unsigned int crc32_snps_le(unsigned int initval, unsigned char *data, unsigned int size)
{
	unsigned int crc = initval;
	unsigned int poly = 0x04c11db7;
	unsigned int temp = 0;
	unsigned char my_data = 0;
	int bit_count;
	for(bit_count = 0; bit_count < size; bit_count++) {
		if((bit_count % 8) == 0) my_data = data[bit_count/8];
		DBGPR_FILTER("%s my_data = %x crc=%x\n", __func__, my_data,crc);
		temp = ((crc >> 31) ^  my_data) &  0x1;
		crc <<= 1;
		if(temp != 0) crc ^= poly;
		my_data >>=1;
	}
		DBGPR_FILTER("%s my_data = %x crc=%x\n", __func__, my_data,crc);
	return ~crc;
}



/*!
* \brief API to delete vid to HW filter.
*
* \details This function is invoked by upper layer when a VLAN id is removed.
* This function deletes the VLAN id from the HW filter.
* vlan id can be removed with vconfig -
* vconfig rem <interface_name > <vlan_id>
*
* \param[in] dev - pointer to net_device structure
* \param[in] vid - vlan id to be removed.
*
* \return void
*/
static int eqos_vlan_rx_kill_vid(struct net_device *dev, __be16 proto, u16 vid)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	unsigned short new_index, old_index;
	int crc32_val = 0;
	unsigned int enb_12bit_vhash;

	printk(KERN_ALERT "-->eqos_vlan_rx_kill_vid: vid = %d\n",
		vid);

	if (pdata->vlan_hash_filtering) {
		crc32_val = (bitrev32(~crc32_le(~0, (unsigned char *)&vid, 2)) >> 28);

		enb_12bit_vhash = hw_if->get_vlan_tag_comparison();
		if (enb_12bit_vhash) {
			/* neget 4-bit crc value for 12-bit VLAN hash comparison */
			new_index = (1 << (~crc32_val & 0xF));
		} else {
			new_index = (1 << (crc32_val & 0xF));
		}

		old_index = hw_if->get_vlan_hash_table_reg();
		old_index &= ~new_index;
		hw_if->update_vlan_hash_table_reg(old_index);
		pdata->vlan_ht_or_id = old_index;
	} else {
		/* By default, receive only VLAN pkt with VID = 1
		 * becasue writting 0 will pass all VLAN pkt */
		hw_if->update_vlan_id(1);
		pdata->vlan_ht_or_id = 1;
	}

	printk(KERN_ALERT "<--eqos_vlan_rx_kill_vid\n");

	//FIXME: Check if any errors need to be returned in case of a failure.
	return 0;
}


static int eqos_set_mac_address(struct net_device *dev, void *p)
{
	if (is_valid_ether_addr(dev->dev_addr))
		return -EOPNOTSUPP;
	else
		return eth_mac_addr(dev, p);
}

/*!
* \brief API to add vid to HW filter.
*
* \details This function is invoked by upper layer when a new VALN id is
* registered. This function updates the HW filter with new VLAN id.
* New vlan id can be added with vconfig -
* vconfig add <interface_name > <vlan_id>
*
* \param[in] dev - pointer to net_device structure
* \param[in] vid - new vlan id.
*
* \return void
*/
static int eqos_vlan_rx_add_vid(struct net_device *dev, __be16 proto, u16 vid)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	unsigned short new_index, old_index;
	int crc32_val = 0;
	unsigned int enb_12bit_vhash;

	printk(KERN_ALERT "-->eqos_vlan_rx_add_vid: vid = %d\n",
		vid);

	if (pdata->vlan_hash_filtering) {
		/* The upper 4 bits of the calculated CRC are used to
		 * index the content of the VLAN Hash Table Reg.
		 * */
		crc32_val = (bitrev32(~crc32_le(~0, (unsigned char *)&vid, 2)) >> 28);

		/* These 4(0xF) bits determines the bit within the
		 * VLAN Hash Table Reg 0
		 * */
		enb_12bit_vhash = hw_if->get_vlan_tag_comparison();
		if (enb_12bit_vhash) {
			/* neget 4-bit crc value for 12-bit VLAN hash comparison */
			new_index = (1 << (~crc32_val & 0xF));
		} else {
			new_index = (1 << (crc32_val & 0xF));
		}

		old_index = hw_if->get_vlan_hash_table_reg();
		old_index |= new_index;
		hw_if->update_vlan_hash_table_reg(old_index);
		pdata->vlan_ht_or_id = old_index;
	} else {
		hw_if->update_vlan_id(vid);
		pdata->vlan_ht_or_id = vid;
	}

	printk(KERN_ALERT "<--eqos_vlan_rx_add_vid\n");

	//FIXME: Check if any errors need to be returned in case of a failure.
	return 0;
}

/*!
 * \brief API called to put device in powerdown mode
 *
 * \details This function is invoked by ioctl function when the user issues an
 * ioctl command to move the device to power down state. Following operations
 * are performed in this function.
 * - stop the phy.
 * - stop the queue.
 * - Disable napi.
 * - Stop DMA TX and RX process.
 * - Enable power down mode using PMT module.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] wakeup_type – remote wake-on-lan or magic packet.
 * \param[in] caller – netif_detach gets called conditionally based
 *                     on caller, IOCTL or DRIVER-suspend
 *
 * \return int
 *
 * \retval zero on success and -ve number on failure.
 */

INT eqos_powerdown(struct net_device *dev, UINT wakeup_type,
		UINT caller)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	ULONG flags;

	DBGPR(KERN_ALERT "-->eqos_powerdown\n");

	if (!dev || !netif_running(dev) ||
	    (caller == EQOS_IOCTL_CONTEXT && pdata->power_down)) {
		printk(KERN_ALERT
		       "Device is already powered down and will powerup for %s\n",
		       EQOS_POWER_DOWN_TYPE(pdata));
		DBGPR("<--eqos_powerdown\n");
		return -EINVAL;
	}

	if (pdata->phydev)
		phy_stop(pdata->phydev);

	spin_lock_irqsave(&pdata->pmt_lock, flags);

	if (caller == EQOS_DRIVER_CONTEXT)
		netif_device_detach(dev);

	netif_tx_disable(dev);
	eqos_all_ch_napi_disable(pdata);

	/* stop DMA TX/RX */
	eqos_stop_all_ch_tx_dma(pdata);
	eqos_stop_all_ch_rx_dma(pdata);

	/* enable power down mode by programming the PMT regs */
	if (wakeup_type & EQOS_REMOTE_WAKEUP)
		hw_if->enable_remote_pmt();
	if (wakeup_type & EQOS_MAGIC_WAKEUP)
		hw_if->enable_magic_pmt();
	pdata->power_down_type = wakeup_type;

	if (caller == EQOS_IOCTL_CONTEXT)
		pdata->power_down = 1;

	spin_unlock_irqrestore(&pdata->pmt_lock, flags);

	DBGPR("<--eqos_powerdown\n");

	return 0;
}

/*!
 * \brief API to powerup the device
 *
 * \details This function is invoked by ioctl function when the user issues an
 * ioctl command to move the device to out of power down state. Following
 * operations are performed in this function.
 * - Wakeup the device using PMT module if supported.
 * - Starts the phy.
 * - Enable MAC and DMA TX and RX process.
 * - Enable napi.
 * - Starts the queue.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] caller – netif_attach gets called conditionally based
 *                     on caller, IOCTL or DRIVER-suspend
 *
 * \return int
 *
 * \retval zero on success and -ve number on failure.
 */

INT eqos_powerup(struct net_device *dev, UINT caller)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	ULONG flags;

	DBGPR("-->eqos_powerup\n");

	if (!dev || !netif_running(dev) ||
	    (caller == EQOS_IOCTL_CONTEXT && !pdata->power_down)) {
		printk(KERN_ALERT "Device is already powered up\n");
		DBGPR(KERN_ALERT "<--eqos_powerup\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&pdata->pmt_lock, flags);

	if (pdata->power_down_type & EQOS_MAGIC_WAKEUP) {
		hw_if->disable_magic_pmt();
		pdata->power_down_type &= ~EQOS_MAGIC_WAKEUP;
	}

	if (pdata->power_down_type & EQOS_REMOTE_WAKEUP) {
		hw_if->disable_remote_pmt();
		pdata->power_down_type &= ~EQOS_REMOTE_WAKEUP;
	}

	pdata->power_down = 0;

	if (pdata->phydev)
		phy_start(pdata->phydev);

	/* enable MAC TX/RX */
	hw_if->start_mac_tx_rx();

	/* enable DMA TX/RX */
	eqos_start_all_ch_tx_dma(pdata);
	eqos_start_all_ch_rx_dma(pdata);

	if (caller == EQOS_DRIVER_CONTEXT)
		netif_device_attach(dev);

	eqos_napi_enable_mq(pdata);

	netif_tx_start_all_queues(dev);

	spin_unlock_irqrestore(&pdata->pmt_lock, flags);

	DBGPR("<--eqos_powerup\n");

	return 0;
}

/*!
 * \brief API to configure remote wakeup
 *
 * \details This function is invoked by ioctl function when the user issues an
 * ioctl command to move the device to power down state using remote wakeup.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] req – pointer to ioctl data structure.
 *
 * \return int
 *
 * \retval zero on success and -ve number on failure.
 */

INT eqos_configure_remotewakeup(struct net_device *dev,
				       struct ifr_data_struct *req)
{
	struct eqos_prv_data *pdata = netdev_priv(dev);
	struct hw_if_struct *hw_if = &(pdata->hw_if);

	if (!dev || !netif_running(dev) || !pdata->hw_feat.rwk_sel
	    || pdata->power_down) {
		printk(KERN_ALERT
		       "Device is already powered down and will powerup for %s\n",
		       EQOS_POWER_DOWN_TYPE(pdata));
		return -EINVAL;
	}

	hw_if->configure_rwk_filter(req->rwk_filter_values,
				    req->rwk_filter_length);

	eqos_powerdown(dev, EQOS_REMOTE_WAKEUP,
			EQOS_IOCTL_CONTEXT);

	return 0;
}

/*!
 * \details This function is invoked by ioctl function when the user issues an
 * ioctl command to change the RX DMA PBL value. This function will program
 * the device to configure the user specified RX PBL value.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] rx_pbl – RX DMA pbl value to be programmed.
 *
 * \return void
 *
 * \retval none
 */

static void eqos_config_rx_pbl(struct eqos_prv_data *pdata,
				      UINT rx_pbl,
				      UINT qinx)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT pblx8_val = 0;

	DBGPR("-->eqos_config_rx_pbl: %d\n", rx_pbl);

	switch (rx_pbl) {
	case EQOS_PBL_1:
	case EQOS_PBL_2:
	case EQOS_PBL_4:
	case EQOS_PBL_8:
	case EQOS_PBL_16:
	case EQOS_PBL_32:
		hw_if->config_rx_pbl_val(qinx, rx_pbl);
		hw_if->config_pblx8(qinx, 0);
		break;
	case EQOS_PBL_64:
	case EQOS_PBL_128:
	case EQOS_PBL_256:
		hw_if->config_rx_pbl_val(qinx, rx_pbl / 8);
		hw_if->config_pblx8(qinx, 1);
		pblx8_val = 1;
		break;
	}

	switch (pblx8_val) {
		case 0:
			printk(KERN_ALERT "Tx PBL[%d] value: %d\n",
					qinx, hw_if->get_tx_pbl_val(qinx));
			printk(KERN_ALERT "Rx PBL[%d] value: %d\n",
					qinx, hw_if->get_rx_pbl_val(qinx));
			break;
		case 1:
			printk(KERN_ALERT "Tx PBL[%d] value: %d\n",
					qinx, (hw_if->get_tx_pbl_val(qinx) * 8));
			printk(KERN_ALERT "Rx PBL[%d] value: %d\n",
					qinx, (hw_if->get_rx_pbl_val(qinx) * 8));
			break;
	}

	DBGPR("<--eqos_config_rx_pbl\n");
}

/*!
 * \details This function is invoked by ioctl function when the user issues an
 * ioctl command to change the TX DMA PBL value. This function will program
 * the device to configure the user specified TX PBL value.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] tx_pbl – TX DMA pbl value to be programmed.
 *
 * \return void
 *
 * \retval none
 */

static void eqos_config_tx_pbl(struct eqos_prv_data *pdata,
				      UINT tx_pbl,
				      UINT qinx)
{
	struct hw_if_struct *hw_if = &(pdata->hw_if);
	UINT pblx8_val = 0;

	DBGPR("-->eqos_config_tx_pbl: %d\n", tx_pbl);

	switch (tx_pbl) {
	case EQOS_PBL_1:
	case EQOS_PBL_2:
	case EQOS_PBL_4:
	case EQOS_PBL_8:
	case EQOS_PBL_16:
	case EQOS_PBL_32:
		hw_if->config_tx_pbl_val(qinx, tx_pbl);
		hw_if->config_pblx8(qinx, 0);
		break;
	case EQOS_PBL_64:
	case EQOS_PBL_128:
	case EQOS_PBL_256:
		hw_if->config_tx_pbl_val(qinx, tx_pbl / 8);
		hw_if->config_pblx8(qinx, 1);
		pblx8_val = 1;
		break;
	}

	switch (pblx8_val) {
		case 0:
			printk(KERN_ALERT "Tx PBL[%d] value: %d\n",
					qinx, hw_if->get_tx_pbl_val(qinx));
			printk(KERN_ALERT "Rx PBL[%d] value: %d\n",
					qinx, hw_if->get_rx_pbl_val(qinx));
			break;
		case 1:
			printk(KERN_ALERT "Tx PBL[%d] value: %d\n",
					qinx, (hw_if->get_tx_pbl_val(qinx) * 8));
			printk(KERN_ALERT "Rx PBL[%d] value: %d\n",
					qinx, (hw_if->get_rx_pbl_val(qinx) * 8));
			break;
	}

	DBGPR("<--eqos_config_tx_pbl\n");
}


/*!
 * \details This function is invoked by ioctl function when the user issues an
 * ioctl command to select the DCB algorithm.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] req – pointer to ioctl data structure.
 *
 * \return void
 *
 * \retval none
 */

static void eqos_program_dcb_algorithm(struct eqos_prv_data *pdata,
		struct ifr_data_struct *req)
{
	struct eqos_dcb_algorithm l_dcb_struct, *u_dcb_struct =
		(struct eqos_dcb_algorithm *)req->ptr;
	struct hw_if_struct *hw_if = &pdata->hw_if;

	DBGPR("-->eqos_program_dcb_algorithm\n");

	if(copy_from_user(&l_dcb_struct, u_dcb_struct,
				sizeof(struct eqos_dcb_algorithm)))
		printk(KERN_ALERT "Failed to fetch DCB Struct info from user\n");

	hw_if->set_tx_queue_operating_mode(l_dcb_struct.qinx,
		(UINT)l_dcb_struct.op_mode);
	hw_if->set_dcb_algorithm(l_dcb_struct.algorithm);
	hw_if->set_dcb_queue_weight(l_dcb_struct.qinx, l_dcb_struct.weight);

	DBGPR("<--eqos_program_dcb_algorithm\n");

	return;
}


/*!
 * \details This function is invoked by ioctl function when the user issues an
 * ioctl command to select the AVB algorithm. This function also configures other
 * parameters like send and idle slope, high and low credit.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] req – pointer to ioctl data structure.
 *
 * \return void
 *
 * \retval none
 */

static void eqos_program_avb_algorithm(struct eqos_prv_data *pdata,
		struct ifr_data_struct *req)
{
	struct eqos_avb_algorithm l_avb_struct, *u_avb_struct =
		(struct eqos_avb_algorithm *)req->ptr;
	struct hw_if_struct *hw_if = &pdata->hw_if;

	DBGPR("-->eqos_program_avb_algorithm\n");

	if(copy_from_user(&l_avb_struct, u_avb_struct,
				sizeof(struct eqos_avb_algorithm)))
		printk(KERN_ALERT "Failed to fetch AVB Struct info from user\n");

	hw_if->set_tx_queue_operating_mode(l_avb_struct.qinx,
		(UINT)l_avb_struct.op_mode);
	hw_if->set_avb_algorithm(l_avb_struct.qinx, l_avb_struct.algorithm);
	hw_if->config_credit_control(l_avb_struct.qinx, l_avb_struct.cc);
	hw_if->config_send_slope(l_avb_struct.qinx, l_avb_struct.send_slope);
	hw_if->config_idle_slope(l_avb_struct.qinx, l_avb_struct.idle_slope);
	hw_if->config_high_credit(l_avb_struct.qinx, l_avb_struct.hi_credit);
	hw_if->config_low_credit(l_avb_struct.qinx, l_avb_struct.low_credit);

	DBGPR("<--eqos_program_avb_algorithm\n");

	return;
}

/*!
* \brief API to read the registers & prints the value.
* \details This function will read all the device register except
* data register & prints the values.
*
* \return none
*/
#if 0
void dbgpr_regs(void)
{
	UINT val0;
	UINT val1;
	UINT val2;
	UINT val3;
	UINT val4;
	UINT val5;

	MAC_PMTCSR_RD(val0);
	MMC_RXICMP_ERR_OCTETS_RD(val1);
	MMC_RXICMP_GD_OCTETS_RD(val2);
	MMC_RXTCP_ERR_OCTETS_RD(val3);
	MMC_RXTCP_GD_OCTETS_RD(val4);
	MMC_RXUDP_ERR_OCTETS_RD(val5);

	DBGPR("dbgpr_regs: MAC_PMTCSR:%#x\n"
	      "dbgpr_regs: MMC_RXICMP_ERR_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXICMP_GD_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXTCP_ERR_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXTCP_GD_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXUDP_ERR_OCTETS:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RXUDP_GD_OCTETS_RD(val0);
	MMC_RXIPV6_NOPAY_OCTETS_RD(val1);
	MMC_RXIPV6_HDRERR_OCTETS_RD(val2);
	MMC_RXIPV6_GD_OCTETS_RD(val3);
	MMC_RXIPV4_UDSBL_OCTETS_RD(val4);
	MMC_RXIPV4_FRAG_OCTETS_RD(val5);

	DBGPR("dbgpr_regs: MMC_RXUDP_GD_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV6_NOPAY_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV6_HDRERR_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV6_GD_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV4_UDSBL_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV4_FRAG_OCTETS:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RXIPV4_NOPAY_OCTETS_RD(val0);
	MMC_RXIPV4_HDRERR_OCTETS_RD(val1);
	MMC_RXIPV4_GD_OCTETS_RD(val2);
	MMC_RXICMP_ERR_PKTS_RD(val3);
	MMC_RXICMP_GD_PKTS_RD(val4);
	MMC_RXTCP_ERR_PKTS_RD(val5);

	DBGPR("dbgpr_regs: MMC_RXIPV4_NOPAY_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV4_HDRERR_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV4_GD_OCTETS:%#x\n"
	      "dbgpr_regs: MMC_RXICMP_ERR_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXICMP_GD_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXTCP_ERR_PKTS:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RXTCP_GD_PKTS_RD(val0);
	MMC_RXUDP_ERR_PKTS_RD(val1);
	MMC_RXUDP_GD_PKTS_RD(val2);
	MMC_RXIPV6_NOPAY_PKTS_RD(val3);
	MMC_RXIPV6_HDRERR_PKTS_RD(val4);
	MMC_RXIPV6_GD_PKTS_RD(val5);

	DBGPR("dbgpr_regs: MMC_RXTCP_GD_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXUDP_ERR_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXUDP_GD_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV6_NOPAY_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV6_HDRERR_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV6_GD_PKTS:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RXIPV4_UBSBL_PKTS_RD(val0);
	MMC_RXIPV4_FRAG_PKTS_RD(val1);
	MMC_RXIPV4_NOPAY_PKTS_RD(val2);
	MMC_RXIPV4_HDRERR_PKTS_RD(val3);
	MMC_RXIPV4_GD_PKTS_RD(val4);
	MMC_RXCTRLPACKETS_G_RD(val5);

	DBGPR("dbgpr_regs: MMC_RXIPV4_UBSBL_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV4_FRAG_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV4_NOPAY_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV4_HDRERR_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXIPV4_GD_PKTS:%#x\n"
	      "dbgpr_regs: MMC_RXCTRLPACKETS_G:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RXRCVERROR_RD(val0);
	MMC_RXWATCHDOGERROR_RD(val1);
	MMC_RXVLANPACKETS_GB_RD(val2);
	MMC_RXFIFOOVERFLOW_RD(val3);
	MMC_RXPAUSEPACKETS_RD(val4);
	MMC_RXOUTOFRANGETYPE_RD(val5);

	DBGPR("dbgpr_regs: MMC_RXRCVERROR:%#x\n"
	      "dbgpr_regs: MMC_RXWATCHDOGERROR:%#x\n"
	      "dbgpr_regs: MMC_RXVLANPACKETS_GB:%#x\n"
	      "dbgpr_regs: MMC_RXFIFOOVERFLOW:%#x\n"
	      "dbgpr_regs: MMC_RXPAUSEPACKETS:%#x\n"
	      "dbgpr_regs: MMC_RXOUTOFRANGETYPE:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RXLENGTHERROR_RD(val0);
	MMC_RXUNICASTPACKETS_G_RD(val1);
	MMC_RX1024TOMAXOCTETS_GB_RD(val2);
	MMC_RX512TO1023OCTETS_GB_RD(val3);
	MMC_RX256TO511OCTETS_GB_RD(val4);
	MMC_RX128TO255OCTETS_GB_RD(val5);

	DBGPR("dbgpr_regs: MMC_RXLENGTHERROR:%#x\n"
	      "dbgpr_regs: MMC_RXUNICASTPACKETS_G:%#x\n"
	      "dbgpr_regs: MMC_RX1024TOMAXOCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_RX512TO1023OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_RX256TO511OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_RX128TO255OCTETS_GB:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RX65TO127OCTETS_GB_RD(val0);
	MMC_RX64OCTETS_GB_RD(val1);
	MMC_RXOVERSIZE_G_RD(val2);
	MMC_RXUNDERSIZE_G_RD(val3);
	MMC_RXJABBERERROR_RD(val4);
	MMC_RXRUNTERROR_RD(val5);

	DBGPR("dbgpr_regs: MMC_RX65TO127OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_RX64OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_RXOVERSIZE_G:%#x\n"
	      "dbgpr_regs: MMC_RXUNDERSIZE_G:%#x\n"
	      "dbgpr_regs: MMC_RXJABBERERROR:%#x\n"
	      "dbgpr_regs: MMC_RXRUNTERROR:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RXALIGNMENTERROR_RD(val0);
	MMC_RXCRCERROR_RD(val1);
	MMC_RXMULTICASTPACKETS_G_RD(val2);
	MMC_RXBROADCASTPACKETS_G_RD(val3);
	MMC_RXOCTETCOUNT_G_RD(val4);
	MMC_RXOCTETCOUNT_GB_RD(val5);

	DBGPR("dbgpr_regs: MMC_RXALIGNMENTERROR:%#x\n"
	      "dbgpr_regs: MMC_RXCRCERROR:%#x\n"
	      "dbgpr_regs: MMC_RXMULTICASTPACKETS_G:%#x\n"
	      "dbgpr_regs: MMC_RXBROADCASTPACKETS_G:%#x\n"
	      "dbgpr_regs: MMC_RXOCTETCOUNT_G:%#x\n"
	      "dbgpr_regs: MMC_RXOCTETCOUNT_GB:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_RXPACKETCOUNT_GB_RD(val0);
	MMC_TXOVERSIZE_G_RD(val1);
	MMC_TXVLANPACKETS_G_RD(val2);
	MMC_TXPAUSEPACKETS_RD(val3);
	MMC_TXEXCESSDEF_RD(val4);
	MMC_TXPACKETSCOUNT_G_RD(val5);

	DBGPR("dbgpr_regs: MMC_RXPACKETCOUNT_GB:%#x\n"
	      "dbgpr_regs: MMC_TXOVERSIZE_G:%#x\n"
	      "dbgpr_regs: MMC_TXVLANPACKETS_G:%#x\n"
	      "dbgpr_regs: MMC_TXPAUSEPACKETS:%#x\n"
	      "dbgpr_regs: MMC_TXEXCESSDEF:%#x\n"
	      "dbgpr_regs: MMC_TXPACKETSCOUNT_G:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_TXOCTETCOUNT_G_RD(val0);
	MMC_TXCARRIERERROR_RD(val1);
	MMC_TXEXESSCOL_RD(val2);
	MMC_TXLATECOL_RD(val3);
	MMC_TXDEFERRED_RD(val4);
	MMC_TXMULTICOL_G_RD(val5);

	DBGPR("dbgpr_regs: MMC_TXOCTETCOUNT_G:%#x\n"
	      "dbgpr_regs: MMC_TXCARRIERERROR:%#x\n"
	      "dbgpr_regs: MMC_TXEXESSCOL:%#x\n"
	      "dbgpr_regs: MMC_TXLATECOL:%#x\n"
	      "dbgpr_regs: MMC_TXDEFERRED:%#x\n"
	      "dbgpr_regs: MMC_TXMULTICOL_G:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_TXSINGLECOL_G_RD(val0);
	MMC_TXUNDERFLOWERROR_RD(val1);
	MMC_TXBROADCASTPACKETS_GB_RD(val2);
	MMC_TXMULTICASTPACKETS_GB_RD(val3);
	MMC_TXUNICASTPACKETS_GB_RD(val4);
	MMC_TX1024TOMAXOCTETS_GB_RD(val5);

	DBGPR("dbgpr_regs: MMC_TXSINGLECOL_G:%#x\n"
	      "dbgpr_regs: MMC_TXUNDERFLOWERROR:%#x\n"
	      "dbgpr_regs: MMC_TXBROADCASTPACKETS_GB:%#x\n"
	      "dbgpr_regs: MMC_TXMULTICASTPACKETS_GB:%#x\n"
	      "dbgpr_regs: MMC_TXUNICASTPACKETS_GB:%#x\n"
	      "dbgpr_regs: MMC_TX1024TOMAXOCTETS_GB:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_TX512TO1023OCTETS_GB_RD(val0);
	MMC_TX256TO511OCTETS_GB_RD(val1);
	MMC_TX128TO255OCTETS_GB_RD(val2);
	MMC_TX65TO127OCTETS_GB_RD(val3);
	MMC_TX64OCTETS_GB_RD(val4);
	MMC_TXMULTICASTPACKETS_G_RD(val5);

	DBGPR("dbgpr_regs: MMC_TX512TO1023OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_TX256TO511OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_TX128TO255OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_TX65TO127OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_TX64OCTETS_GB:%#x\n"
	      "dbgpr_regs: MMC_TXMULTICASTPACKETS_G:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_TXBROADCASTPACKETS_G_RD(val0);
	MMC_TXPACKETCOUNT_GB_RD(val1);
	MMC_TXOCTETCOUNT_GB_RD(val2);
	MMC_IPC_INTR_RX_RD(val3);
	MMC_IPC_INTR_MASK_RX_RD(val4);
	MMC_INTR_MASK_TX_RD(val5);

	DBGPR("dbgpr_regs: MMC_TXBROADCASTPACKETS_G:%#x\n"
	      "dbgpr_regs: MMC_TXPACKETCOUNT_GB:%#x\n"
	      "dbgpr_regs: MMC_TXOCTETCOUNT_GB:%#x\n"
	      "dbgpr_regs: MMC_IPC_INTR_RX:%#x\n"
	      "dbgpr_regs: MMC_IPC_INTR_MASK_RX:%#x\n"
	      "dbgpr_regs: MMC_INTR_MASK_TX:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MMC_INTR_MASK_RX_RD(val0);
	MMC_INTR_TX_RD(val1);
	MMC_INTR_RX_RD(val2);
	MMC_CNTRL_RD(val3);
	MAC_MA1LR_RD(val4);
	MAC_MA1HR_RD(val5);

	DBGPR("dbgpr_regs: MMC_INTR_MASK_RX:%#x\n"
	      "dbgpr_regs: MMC_INTR_TX:%#x\n"
	      "dbgpr_regs: MMC_INTR_RX:%#x\n"
	      "dbgpr_regs: MMC_CNTRL:%#x\n"
	      "dbgpr_regs: MAC_MA1LR:%#x\n"
	      "dbgpr_regs: MAC_MA1HR:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MAC_MA0LR_RD(val0);
	MAC_MA0HR_RD(val1);
	MAC_GPIOR_RD(val2);
	MAC_GMIIDR_RD(val3);
	MAC_GMIIAR_RD(val4);
	MAC_HFR2_RD(val5);

	DBGPR("dbgpr_regs: MAC_MA0LR:%#x\n"
	      "dbgpr_regs: MAC_MA0HR:%#x\n"
	      "dbgpr_regs: MAC_GPIOR:%#x\n"
	      "dbgpr_regs: MAC_GMIIDR:%#x\n"
	      "dbgpr_regs: MAC_GMIIAR:%#x\n"
	      "dbgpr_regs: MAC_HFR2:%#x\n", val0, val1, val2, val3, val4, val5);

	MAC_HFR1_RD(val0);
	MAC_HFR0_RD(val1);
	MAC_MDR_RD(val2);
	MAC_VR_RD(val3);
	MAC_HTR7_RD(val4);
	MAC_HTR6_RD(val5);

	DBGPR("dbgpr_regs: MAC_HFR1:%#x\n"
	      "dbgpr_regs: MAC_HFR0:%#x\n"
	      "dbgpr_regs: MAC_MDR:%#x\n"
	      "dbgpr_regs: MAC_VR:%#x\n"
	      "dbgpr_regs: MAC_HTR7:%#x\n"
	      "dbgpr_regs: MAC_HTR6:%#x\n", val0, val1, val2, val3, val4, val5);

	MAC_HTR5_RD(val0);
	MAC_HTR4_RD(val1);
	MAC_HTR3_RD(val2);
	MAC_HTR2_RD(val3);
	MAC_HTR1_RD(val4);
	MAC_HTR0_RD(val5);

	DBGPR("dbgpr_regs: MAC_HTR5:%#x\n"
	      "dbgpr_regs: MAC_HTR4:%#x\n"
	      "dbgpr_regs: MAC_HTR3:%#x\n"
	      "dbgpr_regs: MAC_HTR2:%#x\n"
	      "dbgpr_regs: MAC_HTR1:%#x\n"
	      "dbgpr_regs: MAC_HTR0:%#x\n", val0, val1, val2, val3, val4, val5);

	DMA_RIWTR7_RD(val0);
	DMA_RIWTR6_RD(val1);
	DMA_RIWTR5_RD(val2);
	DMA_RIWTR4_RD(val3);
	DMA_RIWTR3_RD(val4);
	DMA_RIWTR2_RD(val5);

	DBGPR("dbgpr_regs: DMA_RIWTR7:%#x\n"
	      "dbgpr_regs: DMA_RIWTR6:%#x\n"
	      "dbgpr_regs: DMA_RIWTR5:%#x\n"
	      "dbgpr_regs: DMA_RIWTR4:%#x\n"
	      "dbgpr_regs: DMA_RIWTR3:%#x\n"
	      "dbgpr_regs: DMA_RIWTR2:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_RIWTR1_RD(val0);
	DMA_RIWTR0_RD(val1);
	DMA_RDRLR7_RD(val2);
	DMA_RDRLR6_RD(val3);
	DMA_RDRLR5_RD(val4);
	DMA_RDRLR4_RD(val5);

	DBGPR("dbgpr_regs: DMA_RIWTR1:%#x\n"
	      "dbgpr_regs: DMA_RIWTR0:%#x\n"
	      "dbgpr_regs: DMA_RDRLR7:%#x\n"
	      "dbgpr_regs: DMA_RDRLR6:%#x\n"
	      "dbgpr_regs: DMA_RDRLR5:%#x\n"
	      "dbgpr_regs: DMA_RDRLR4:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_RDRLR3_RD(val0);
	DMA_RDRLR2_RD(val1);
	DMA_RDRLR1_RD(val2);
	DMA_RDRLR0_RD(val3);
	DMA_TDRLR7_RD(val4);
	DMA_TDRLR6_RD(val5);

	DBGPR("dbgpr_regs: DMA_RDRLR3:%#x\n"
	      "dbgpr_regs: DMA_RDRLR2:%#x\n"
	      "dbgpr_regs: DMA_RDRLR1:%#x\n"
	      "dbgpr_regs: DMA_RDRLR0:%#x\n"
	      "dbgpr_regs: DMA_TDRLR7:%#x\n"
	      "dbgpr_regs: DMA_TDRLR6:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_TDRLR5_RD(val0);
	DMA_TDRLR4_RD(val1);
	DMA_TDRLR3_RD(val2);
	DMA_TDRLR2_RD(val3);
	DMA_TDRLR1_RD(val4);
	DMA_TDRLR0_RD(val5);

	DBGPR("dbgpr_regs: DMA_TDRLR5:%#x\n"
	      "dbgpr_regs: DMA_TDRLR4:%#x\n"
	      "dbgpr_regs: DMA_TDRLR3:%#x\n"
	      "dbgpr_regs: DMA_TDRLR2:%#x\n"
	      "dbgpr_regs: DMA_TDRLR1:%#x\n"
	      "dbgpr_regs: DMA_TDRLR0:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_RDTP_RPDR7_RD(val0);
	DMA_RDTP_RPDR6_RD(val1);
	DMA_RDTP_RPDR5_RD(val2);
	DMA_RDTP_RPDR4_RD(val3);
	DMA_RDTP_RPDR3_RD(val4);
	DMA_RDTP_RPDR2_RD(val5);

	DBGPR("dbgpr_regs: DMA_RDTP_RPDR7:%#x\n"
	      "dbgpr_regs: DMA_RDTP_RPDR6:%#x\n"
	      "dbgpr_regs: DMA_RDTP_RPDR5:%#x\n"
	      "dbgpr_regs: DMA_RDTP_RPDR4:%#x\n"
	      "dbgpr_regs: DMA_RDTP_RPDR3:%#x\n"
	      "dbgpr_regs: DMA_RDTP_RPDR2:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_RDTP_RPDR1_RD(val0);
	DMA_RDTP_RPDR0_RD(val1);
	DMA_TDTP_TPDR7_RD(val2);
	DMA_TDTP_TPDR6_RD(val3);
	DMA_TDTP_TPDR5_RD(val4);
	DMA_TDTP_TPDR4_RD(val5);

	DBGPR("dbgpr_regs: DMA_RDTP_RPDR1:%#x\n"
	      "dbgpr_regs: DMA_RDTP_RPDR0:%#x\n"
	      "dbgpr_regs: DMA_TDTP_TPDR7:%#x\n"
	      "dbgpr_regs: DMA_TDTP_TPDR6:%#x\n"
	      "dbgpr_regs: DMA_TDTP_TPDR5:%#x\n"
	      "dbgpr_regs: DMA_TDTP_TPDR4:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_TDTP_TPDR3_RD(val0);
	DMA_TDTP_TPDR2_RD(val1);
	DMA_TDTP_TPDR1_RD(val2);
	DMA_TDTP_TPDR0_RD(val3);
	DMA_RDLAR7_RD(val4);
	DMA_RDLAR6_RD(val5);

	DBGPR("dbgpr_regs: DMA_TDTP_TPDR3:%#x\n"
	      "dbgpr_regs: DMA_TDTP_TPDR2:%#x\n"
	      "dbgpr_regs: DMA_TDTP_TPDR1:%#x\n"
	      "dbgpr_regs: DMA_TDTP_TPDR0:%#x\n"
	      "dbgpr_regs: DMA_RDLAR7:%#x\n"
	      "dbgpr_regs: DMA_RDLAR6:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_RDLAR5_RD(val0);
	DMA_RDLAR4_RD(val1);
	DMA_RDLAR3_RD(val2);
	DMA_RDLAR2_RD(val3);
	DMA_RDLAR1_RD(val4);
	DMA_RDLAR0_RD(val5);

	DBGPR("dbgpr_regs: DMA_RDLAR5:%#x\n"
	      "dbgpr_regs: DMA_RDLAR4:%#x\n"
	      "dbgpr_regs: DMA_RDLAR3:%#x\n"
	      "dbgpr_regs: DMA_RDLAR2:%#x\n"
	      "dbgpr_regs: DMA_RDLAR1:%#x\n"
	      "dbgpr_regs: DMA_RDLAR0:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_TDLAR7_RD(val0);
	DMA_TDLAR6_RD(val1);
	DMA_TDLAR5_RD(val2);
	DMA_TDLAR4_RD(val3);
	DMA_TDLAR3_RD(val4);
	DMA_TDLAR2_RD(val5);

	DBGPR("dbgpr_regs: DMA_TDLAR7:%#x\n"
	      "dbgpr_regs: DMA_TDLAR6:%#x\n"
	      "dbgpr_regs: DMA_TDLAR5:%#x\n"
	      "dbgpr_regs: DMA_TDLAR4:%#x\n"
	      "dbgpr_regs: DMA_TDLAR3:%#x\n"
	      "dbgpr_regs: DMA_TDLAR2:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_TDLAR1_RD(val0);
	DMA_TDLAR0_RD(val1);
	DMA_IER7_RD(val2);
	DMA_IER6_RD(val3);
	DMA_IER5_RD(val4);
	DMA_IER4_RD(val5);

	DBGPR("dbgpr_regs: DMA_TDLAR1:%#x\n"
	      "dbgpr_regs: DMA_TDLAR0:%#x\n"
	      "dbgpr_regs: DMA_IER7:%#x\n"
	      "dbgpr_regs: DMA_IER6:%#x\n"
	      "dbgpr_regs: DMA_IER5:%#x\n"
	      "dbgpr_regs: DMA_IER4:%#x\n", val0, val1, val2, val3, val4, val5);

	DMA_IER3_RD(val0);
	DMA_IER2_RD(val1);
	DMA_IER1_RD(val2);
	DMA_IER0_RD(val3);
	MAC_IMR_RD(val4);
	MAC_ISR_RD(val5);

	DBGPR("dbgpr_regs: DMA_IER3:%#x\n"
	      "dbgpr_regs: DMA_IER2:%#x\n"
	      "dbgpr_regs: DMA_IER1:%#x\n"
	      "dbgpr_regs: DMA_IER0:%#x\n"
	      "dbgpr_regs: MAC_IMR:%#x\n"
	      "dbgpr_regs: MAC_ISR:%#x\n", val0, val1, val2, val3, val4, val5);

	MTL_ISR_RD(val0);
	DMA_SR7_RD(val1);
	DMA_SR6_RD(val2);
	DMA_SR5_RD(val3);
	DMA_SR4_RD(val4);
	DMA_SR3_RD(val5);

	DBGPR("dbgpr_regs: MTL_ISR:%#x\n"
	      "dbgpr_regs: DMA_SR7:%#x\n"
	      "dbgpr_regs: DMA_SR6:%#x\n"
	      "dbgpr_regs: DMA_SR5:%#x\n"
	      "dbgpr_regs: DMA_SR4:%#x\n"
	      "dbgpr_regs: DMA_SR3:%#x\n", val0, val1, val2, val3, val4, val5);

	DMA_SR2_RD(val0);
	DMA_SR1_RD(val1);
	DMA_SR0_RD(val2);
	DMA_ISR_RD(val3);
	DMA_DSR2_RD(val4);
	DMA_DSR1_RD(val5);

	DBGPR("dbgpr_regs: DMA_SR2:%#x\n"
	      "dbgpr_regs: DMA_SR1:%#x\n"
	      "dbgpr_regs: DMA_SR0:%#x\n"
	      "dbgpr_regs: DMA_ISR:%#x\n"
	      "dbgpr_regs: DMA_DSR2:%#x\n"
	      "dbgpr_regs: DMA_DSR1:%#x\n", val0, val1, val2, val3, val4, val5);

	DMA_DSR0_RD(val0);
	MTL_Q0RDR_RD(val1);
	MTL_Q0ESR_RD(val2);
	MTL_Q0TDR_RD(val3);
	DMA_CHRBAR7_RD(val4);
	DMA_CHRBAR6_RD(val5);

	DBGPR("dbgpr_regs: DMA_DSR0:%#x\n"
	      "dbgpr_regs: MTL_Q0RDR:%#x\n"
	      "dbgpr_regs: MTL_Q0ESR:%#x\n"
	      "dbgpr_regs: MTL_Q0TDR:%#x\n"
	      "dbgpr_regs: DMA_CHRBAR7:%#x\n"
	      "dbgpr_regs: DMA_CHRBAR6:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_CHRBAR5_RD(val0);
	DMA_CHRBAR4_RD(val1);
	DMA_CHRBAR3_RD(val2);
	DMA_CHRBAR2_RD(val3);
	DMA_CHRBAR1_RD(val4);
	DMA_CHRBAR0_RD(val5);

	DBGPR("dbgpr_regs: DMA_CHRBAR5:%#x\n"
	      "dbgpr_regs: DMA_CHRBAR4:%#x\n"
	      "dbgpr_regs: DMA_CHRBAR3:%#x\n"
	      "dbgpr_regs: DMA_CHRBAR2:%#x\n"
	      "dbgpr_regs: DMA_CHRBAR1:%#x\n"
	      "dbgpr_regs: DMA_CHRBAR0:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_CHTBAR7_RD(val0);
	DMA_CHTBAR6_RD(val1);
	DMA_CHTBAR5_RD(val2);
	DMA_CHTBAR4_RD(val3);
	DMA_CHTBAR3_RD(val4);
	DMA_CHTBAR2_RD(val5);

	DBGPR("dbgpr_regs: DMA_CHTBAR7:%#x\n"
	      "dbgpr_regs: DMA_CHTBAR6:%#x\n"
	      "dbgpr_regs: DMA_CHTBAR5:%#x\n"
	      "dbgpr_regs: DMA_CHTBAR4:%#x\n"
	      "dbgpr_regs: DMA_CHTBAR3:%#x\n"
	      "dbgpr_regs: DMA_CHTBAR2:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_CHTBAR1_RD(val0);
	DMA_CHTBAR0_RD(val1);
	DMA_CHRDR7_RD(val2);
	DMA_CHRDR6_RD(val3);
	DMA_CHRDR5_RD(val4);
	DMA_CHRDR4_RD(val5);

	DBGPR("dbgpr_regs: DMA_CHTBAR1:%#x\n"
	      "dbgpr_regs: DMA_CHTBAR0:%#x\n"
	      "dbgpr_regs: DMA_CHRDR7:%#x\n"
	      "dbgpr_regs: DMA_CHRDR6:%#x\n"
	      "dbgpr_regs: DMA_CHRDR5:%#x\n"
	      "dbgpr_regs: DMA_CHRDR4:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_CHRDR3_RD(val0);
	DMA_CHRDR2_RD(val1);
	DMA_CHRDR1_RD(val2);
	DMA_CHRDR0_RD(val3);
	DMA_CHTDR7_RD(val4);
	DMA_CHTDR6_RD(val5);

	DBGPR("dbgpr_regs: DMA_CHRDR3:%#x\n"
	      "dbgpr_regs: DMA_CHRDR2:%#x\n"
	      "dbgpr_regs: DMA_CHRDR1:%#x\n"
	      "dbgpr_regs: DMA_CHRDR0:%#x\n"
	      "dbgpr_regs: DMA_CHTDR7:%#x\n"
	      "dbgpr_regs: DMA_CHTDR6:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_CHTDR5_RD(val0);
	DMA_CHTDR4_RD(val1);
	DMA_CHTDR3_RD(val2);
	DMA_CHTDR2_RD(val3);
	DMA_CHTDR1_RD(val4);
	DMA_CHTDR0_RD(val5);

	DBGPR("dbgpr_regs: DMA_CHTDR5:%#x\n"
	      "dbgpr_regs: DMA_CHTDR4:%#x\n"
	      "dbgpr_regs: DMA_CHTDR3:%#x\n"
	      "dbgpr_regs: DMA_CHTDR2:%#x\n"
	      "dbgpr_regs: DMA_CHTDR1:%#x\n"
	      "dbgpr_regs: DMA_CHTDR0:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_SFCSR7_RD(val0);
	DMA_SFCSR6_RD(val1);
	DMA_SFCSR5_RD(val2);
	DMA_SFCSR4_RD(val3);
	DMA_SFCSR3_RD(val4);
	DMA_SFCSR2_RD(val5);

	DBGPR("dbgpr_regs: DMA_SFCSR7:%#x\n"
	      "dbgpr_regs: DMA_SFCSR6:%#x\n"
	      "dbgpr_regs: DMA_SFCSR5:%#x\n"
	      "dbgpr_regs: DMA_SFCSR4:%#x\n"
	      "dbgpr_regs: DMA_SFCSR3:%#x\n"
	      "dbgpr_regs: DMA_SFCSR2:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_SFCSR1_RD(val0);
	DMA_SFCSR0_RD(val1);
	MAC_IVLANTIRR_RD(val2);
	MAC_VLANTIRR_RD(val3);
	MAC_VLANHTR_RD(val4);
	MAC_VLANTR_RD(val5);

	DBGPR("dbgpr_regs: DMA_SFCSR1:%#x\n"
	      "dbgpr_regs: DMA_SFCSR0:%#x\n"
	      "dbgpr_regs: MAC_IVLANTIRR:%#x\n"
	      "dbgpr_regs: MAC_VLANTIRR:%#x\n"
	      "dbgpr_regs: MAC_VLANHTR:%#x\n"
	      "dbgpr_regs: MAC_VLANTR:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_SBUS_RD(val0);
	DMA_BMR_RD(val1);
	MTL_Q0RCR_RD(val2);
	MTL_Q0OCR_RD(val3);
	MTL_Q0ROMR_RD(val4);
	MTL_Q0QR_RD(val5);

	DBGPR("dbgpr_regs: DMA_SBUS:%#x\n"
	      "dbgpr_regs: DMA_BMR:%#x\n"
	      "dbgpr_regs: MTL_Q0RCR:%#x\n"
	      "dbgpr_regs: MTL_Q0OCR:%#x\n"
	      "dbgpr_regs: MTL_Q0ROMR:%#x\n"
	      "dbgpr_regs: MTL_Q0QR:%#x\n", val0, val1, val2, val3, val4, val5);

	MTL_Q0ECR_RD(val0);
	MTL_Q0UCR_RD(val1);
	MTL_Q0TOMR_RD(val2);
	MTL_RQDCM1R_RD(val3);
	MTL_RQDCM0R_RD(val4);
	MTL_FDDR_RD(val5);

	DBGPR("dbgpr_regs: MTL_Q0ECR:%#x\n"
	      "dbgpr_regs: MTL_Q0UCR:%#x\n"
	      "dbgpr_regs: MTL_Q0TOMR:%#x\n"
	      "dbgpr_regs: MTL_RQDCM1R:%#x\n"
	      "dbgpr_regs: MTL_RQDCM0R:%#x\n"
	      "dbgpr_regs: MTL_FDDR:%#x\n", val0, val1, val2, val3, val4, val5);

	MTL_FDACS_RD(val0);
	MTL_OMR_RD(val1);
	MAC_RQC1R_RD(val2);
	MAC_RQC0R_RD(val3);
	MAC_TQPM1R_RD(val4);
	MAC_TQPM0R_RD(val5);

	DBGPR("dbgpr_regs: MTL_FDACS:%#x\n"
	      "dbgpr_regs: MTL_OMR:%#x\n"
	      "dbgpr_regs: MAC_RQC1R:%#x\n"
	      "dbgpr_regs: MAC_RQC0R:%#x\n"
	      "dbgpr_regs: MAC_TQPM1R:%#x\n"
	      "dbgpr_regs: MAC_TQPM0R:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MAC_RFCR_RD(val0);
	MAC_QTFCR7_RD(val1);
	MAC_QTFCR6_RD(val2);
	MAC_QTFCR5_RD(val3);
	MAC_QTFCR4_RD(val4);
	MAC_QTFCR3_RD(val5);

	DBGPR("dbgpr_regs: MAC_RFCR:%#x\n"
	      "dbgpr_regs: MAC_QTFCR7:%#x\n"
	      "dbgpr_regs: MAC_QTFCR6:%#x\n"
	      "dbgpr_regs: MAC_QTFCR5:%#x\n"
	      "dbgpr_regs: MAC_QTFCR4:%#x\n"
	      "dbgpr_regs: MAC_QTFCR3:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	MAC_QTFCR2_RD(val0);
	MAC_QTFCR1_RD(val1);
	MAC_Q0TFCR_RD(val2);
	DMA_AXI4CR7_RD(val3);
	DMA_AXI4CR6_RD(val4);
	DMA_AXI4CR5_RD(val5);

	DBGPR("dbgpr_regs: MAC_QTFCR2:%#x\n"
	      "dbgpr_regs: MAC_QTFCR1:%#x\n"
	      "dbgpr_regs: MAC_Q0TFCR:%#x\n"
	      "dbgpr_regs: DMA_AXI4CR7:%#x\n"
	      "dbgpr_regs: DMA_AXI4CR6:%#x\n"
	      "dbgpr_regs: DMA_AXI4CR5:%#x\n",
	      val0, val1, val2, val3, val4, val5);

	DMA_AXI4CR4_RD(val0);
	DMA_AXI4CR3_RD(val1);
	DMA_AXI4CR2_RD(val2);
	DMA_AXI4CR1_RD(val3);
	DMA_AXI4CR0_RD(val4);
	DMA_RCR7_RD(val5);

	DBGPR("dbgpr_regs: DMA_AXI4CR4:%#x\n"
	      "dbgpr_regs: DMA_AXI4CR3:%#x\n"
	      "dbgpr_regs: DMA_AXI4CR2:%#x\n"
	      "dbgpr_regs: DMA_AXI4CR1:%#x\n"
	      "dbgpr_regs: DMA_AXI4CR0:%#x\n"
	      "dbgpr_regs: DMA_RCR7:%#x\n", val0, val1, val2, val3, val4, val5);

	DMA_RCR6_RD(val0);
	DMA_RCR5_RD(val1);
	DMA_RCR4_RD(val2);
	DMA_RCR3_RD(val3);
	DMA_RCR2_RD(val4);
	DMA_RCR1_RD(val5);

	DBGPR("dbgpr_regs: DMA_RCR6:%#x\n"
	      "dbgpr_regs: DMA_RCR5:%#x\n"
	      "dbgpr_regs: DMA_RCR4:%#x\n"
	      "dbgpr_regs: DMA_RCR3:%#x\n"
	      "dbgpr_regs: DMA_RCR2:%#x\n"
	      "dbgpr_regs: DMA_RCR1:%#x\n", val0, val1, val2, val3, val4, val5);

	DMA_RCR0_RD(val0);
	DMA_TCR7_RD(val1);
	DMA_TCR6_RD(val2);
	DMA_TCR5_RD(val3);
	DMA_TCR4_RD(val4);
	DMA_TCR3_RD(val5);

	DBGPR("dbgpr_regs: DMA_RCR0:%#x\n"
	      "dbgpr_regs: DMA_TCR7:%#x\n"
	      "dbgpr_regs: DMA_TCR6:%#x\n"
	      "dbgpr_regs: DMA_TCR5:%#x\n"
	      "dbgpr_regs: DMA_TCR4:%#x\n"
	      "dbgpr_regs: DMA_TCR3:%#x\n", val0, val1, val2, val3, val4, val5);

	DMA_TCR2_RD(val0);
	DMA_TCR1_RD(val1);
	DMA_TCR0_RD(val2);
	DMA_CR7_RD(val3);
	DMA_CR6_RD(val4);
	DMA_CR5_RD(val5);

	DBGPR("dbgpr_regs: DMA_TCR2:%#x\n"
	      "dbgpr_regs: DMA_TCR1:%#x\n"
	      "dbgpr_regs: DMA_TCR0:%#x\n"
	      "dbgpr_regs: DMA_CR7:%#x\n"
	      "dbgpr_regs: DMA_CR6:%#x\n"
	      "dbgpr_regs: DMA_CR5:%#x\n", val0, val1, val2, val3, val4, val5);

	DMA_CR4_RD(val0);
	DMA_CR3_RD(val1);
	DMA_CR2_RD(val2);
	DMA_CR1_RD(val3);
	DMA_CR0_RD(val4);
	MAC_WTR_RD(val5);

	DBGPR("dbgpr_regs: DMA_CR4:%#x\n"
	      "dbgpr_regs: DMA_CR3:%#x\n"
	      "dbgpr_regs: DMA_CR2:%#x\n"
	      "dbgpr_regs: DMA_CR1:%#x\n"
	      "dbgpr_regs: DMA_CR0:%#x\n"
	      "dbgpr_regs: MAC_WTR:%#x\n", val0, val1, val2, val3, val4, val5);

	MAC_MPFR_RD(val0);
	MAC_MECR_RD(val1);
	MAC_MCR_RD(val2);

	DBGPR("dbgpr_regs: MAC_MPFR:%#x\n"
	      "dbgpr_regs: MAC_MECR:%#x\n"
	      "dbgpr_regs: MAC_MCR:%#x\n", val0, val1, val2);

	return;
}
#endif

/*!
 * \details This function is invoked by eqos_start_xmit and
 * eqos_tx_interrupt function for dumping the TX descriptor contents
 * which are prepared for packet transmission and which are transmitted by
 * device. It is mainly used during development phase for debug purpose. Use
 * of these function may affect the performance during normal operation.
 *
 * \param[in] pdata – pointer to private data structure.
 * \param[in] first_desc_idx – first descriptor index for the current
 *		transfer.
 * \param[in] last_desc_idx – last descriptor index for the current transfer.
 * \param[in] flag – to indicate from which function it is called.
 *
 * \return void
 */

void dump_tx_desc(struct eqos_prv_data *pdata, int first_desc_idx,
		  int last_desc_idx, int flag, UINT qinx)
{
	int i;
	struct s_tx_normal_desc *desc = NULL;
	UINT ctxt;

	if (first_desc_idx == last_desc_idx) {
		desc = GET_TX_DESC_PTR(qinx, first_desc_idx);

		TX_NORMAL_DESC_TDES3_CTXT_RD(desc->TDES3, ctxt);

		printk(KERN_ALERT "\n%s[%02d %4p %03d %s] = %#x:%#x:%#x:%#x\n",
		       (ctxt == 1) ? "TX_CONTXT_DESC" : "TX_NORMAL_DESC",
		       qinx, desc, first_desc_idx,
		       ((flag == 1) ? "QUEUED FOR TRANSMISSION" :
			((flag == 0) ? "FREED/FETCHED BY DEVICE" : "DEBUG DESC DUMP")),
			desc->TDES0, desc->TDES1,
			desc->TDES2, desc->TDES3);
	} else {
		int lp_cnt;
		if (first_desc_idx > last_desc_idx)
			lp_cnt = last_desc_idx + TX_DESC_CNT - first_desc_idx;
		else
			lp_cnt = last_desc_idx - first_desc_idx;

		for (i = first_desc_idx; lp_cnt >= 0; lp_cnt--) {
			desc = GET_TX_DESC_PTR(qinx, i);

			TX_NORMAL_DESC_TDES3_CTXT_RD(desc->TDES3, ctxt);

			printk(KERN_ALERT "\n%s[%02d %4p %03d %s] = %#x:%#x:%#x:%#x\n",
			       (ctxt == 1) ? "TX_CONTXT_DESC" : "TX_NORMAL_DESC",
			       qinx, desc, i,
			       ((flag == 1) ? "QUEUED FOR TRANSMISSION" :
				"FREED/FETCHED BY DEVICE"), desc->TDES0,
			       desc->TDES1, desc->TDES2, desc->TDES3);
			INCR_TX_DESC_INDEX(i, 1);
		}
	}
}

/*!
 * \details This function is invoked by poll function for dumping the
 * RX descriptor contents. It is mainly used during development phase for
 * debug purpose. Use of these function may affect the performance during
 * normal operation
 *
 * \param[in] pdata – pointer to private data structure.
 *
 * \return void
 */

void dump_rx_desc(UINT qinx, struct s_rx_normal_desc *desc, int desc_idx)
{
	printk(KERN_ALERT "\nRX_NORMAL_DESC[%02d %4p %03d RECEIVED FROM DEVICE]"\
		" = %#x:%#x:%#x:%#x",
		qinx, desc, desc_idx, desc->RDES0, desc->RDES1,
		desc->RDES2, desc->RDES3);
}

/*!
 * \details This function is invoked by start_xmit and poll function for
 * dumping the content of packet to be transmitted by device or received
 * from device. It is mainly used during development phase for debug purpose.
 * Use of these functions may affect the performance during normal operation.
 *
 * \param[in] skb – pointer to socket buffer structure.
 * \param[in] len – length of packet to be transmitted/received.
 * \param[in] tx_rx – packet to be transmitted or received.
 * \param[in] desc_idx – descriptor index to be used for transmission or
 *			reception of packet.
 *
 * \return void
 */

void print_pkt(struct sk_buff *skb, int len, bool tx_rx, int desc_idx)
{
	int i, j = 0;
	unsigned char *buf = skb->data;

	printk(KERN_ALERT
	       "\n\n/***********************************************************/\n");

	printk(KERN_ALERT "%s pkt of %d Bytes [DESC index = %d]\n\n",
	       (tx_rx ? "TX" : "RX"), len, desc_idx);
	printk(KERN_ALERT "Dst MAC addr(6 bytes)\n");
	for (i = 0; i < 6; i++)
		printk("%#.2x%s", buf[i], (((i == 5) ? "" : ":")));
	printk(KERN_ALERT "\nSrc MAC addr(6 bytes)\n");
	for (i = 6; i <= 11; i++)
		printk("%#.2x%s", buf[i], (((i == 11) ? "" : ":")));
	i = (buf[12] << 8 | buf[13]);
	printk(KERN_ALERT "\nType/Length(2 bytes)\n%#x", i);

	printk(KERN_ALERT "\nPay Load : %d bytes\n", (len - 14));
	for (i = 14, j = 1; i < len; i++, j++) {
		printk("%#.2x%s", buf[i], (((i == (len - 1)) ? "" : ":")));
		if ((j % 16) == 0)
			printk(KERN_ALERT "");
	}

	printk(KERN_ALERT
	       "/*************************************************************/\n\n");
}


/*!
 * \details This function is invoked by probe function. This function will
 * initialize default receive coalesce parameters and sw timer value and store
 * it in respective receive data structure.
 *
 * \param[in] pdata – pointer to private data structure.
 *
 * \return void
 */

void eqos_init_rx_coalesce(struct eqos_prv_data *pdata)
{
	struct eqos_rx_wrapper_descriptor *rx_desc_data = NULL;
	UINT i;

	DBGPR("-->eqos_init_rx_coalesce\n");

	for (i = 0; i < EQOS_RX_QUEUE_CNT; i++) {
		rx_desc_data = GET_RX_WRAPPER_DESC(i);

		rx_desc_data->use_riwt = 1;
		rx_desc_data->rx_coal_frames = EQOS_RX_MAX_FRAMES;
		rx_desc_data->rx_riwt =
			eqos_usec2riwt(EQOS_OPTIMAL_DMA_RIWT_USEC, pdata);
	}

	DBGPR("<--eqos_init_rx_coalesce\n");
}


/*!
 * \details This function is invoked by open() function. This function will
 * clear MMC structure.
 *
 * \param[in] pdata – pointer to private data structure.
 *
 * \return void
 */

static void eqos_mmc_setup(struct eqos_prv_data *pdata)
{
	DBGPR("-->eqos_mmc_setup\n");

	if (pdata->hw_feat.mmc_sel) {
		memset(&pdata->mmc, 0, sizeof(struct eqos_mmc_counters));
	} else
		printk(KERN_ALERT "No MMC/RMON module available in the HW\n");

	DBGPR("<--eqos_mmc_setup\n");
}

inline unsigned int eqos_reg_read(volatile ULONG *ptr)
{
		return ioread32((void *)ptr);
}


/*!
 * \details This function is invoked by ethtool function when user wants to
 * read MMC counters. This function will read the MMC if supported by core
 * and store it in eqos_mmc_counters structure. By default all the
 * MMC are programmed "read on reset" hence all the fields of the
 * eqos_mmc_counters are incremented.
 *
 * open() function. This function will
 * initialize MMC control register ie it disable all MMC interrupt and all
 * MMC register are configured to clear on read.
 *
 * \param[in] pdata – pointer to private data structure.
 *
 * \return void
 */

void eqos_mmc_read(struct eqos_mmc_counters *mmc)
{
	DBGPR("-->eqos_mmc_read\n");

	/* MMC TX counter registers */
	mmc->mmc_tx_octetcount_gb += eqos_reg_read(MMC_TXOCTETCOUNT_GB_OFFSET);
	mmc->mmc_tx_framecount_gb += eqos_reg_read(MMC_TXPACKETCOUNT_GB_OFFSET);
	mmc->mmc_tx_broadcastframe_g += eqos_reg_read(MMC_TXBROADCASTPACKETS_G_OFFSET);
	mmc->mmc_tx_multicastframe_g += eqos_reg_read(MMC_TXMULTICASTPACKETS_G_OFFSET);
	mmc->mmc_tx_64_octets_gb += eqos_reg_read(MMC_TX64OCTETS_GB_OFFSET);
	mmc->mmc_tx_65_to_127_octets_gb += eqos_reg_read(MMC_TX65TO127OCTETS_GB_OFFSET);
	mmc->mmc_tx_128_to_255_octets_gb += eqos_reg_read(MMC_TX128TO255OCTETS_GB_OFFSET);
	mmc->mmc_tx_256_to_511_octets_gb += eqos_reg_read(MMC_TX256TO511OCTETS_GB_OFFSET);
	mmc->mmc_tx_512_to_1023_octets_gb += eqos_reg_read(MMC_TX512TO1023OCTETS_GB_OFFSET);
	mmc->mmc_tx_1024_to_max_octets_gb += eqos_reg_read(MMC_TX1024TOMAXOCTETS_GB_OFFSET);
	mmc->mmc_tx_unicast_gb += eqos_reg_read(MMC_TXUNICASTPACKETS_GB_OFFSET);
	mmc->mmc_tx_multicast_gb += eqos_reg_read(MMC_TXMULTICASTPACKETS_GB_OFFSET);
	mmc->mmc_tx_broadcast_gb += eqos_reg_read(MMC_TXBROADCASTPACKETS_GB_OFFSET);
	mmc->mmc_tx_underflow_error += eqos_reg_read(MMC_TXUNDERFLOWERROR_OFFSET);
	mmc->mmc_tx_singlecol_g += eqos_reg_read(MMC_TXSINGLECOL_G_OFFSET);
	mmc->mmc_tx_multicol_g += eqos_reg_read(MMC_TXMULTICOL_G_OFFSET);
	mmc->mmc_tx_deferred += eqos_reg_read(MMC_TXDEFERRED_OFFSET);
	mmc->mmc_tx_latecol += eqos_reg_read(MMC_TXLATECOL_OFFSET);
	mmc->mmc_tx_exesscol += eqos_reg_read(MMC_TXEXESSCOL_OFFSET);
	mmc->mmc_tx_carrier_error += eqos_reg_read(MMC_TXCARRIERERROR_OFFSET);
	mmc->mmc_tx_octetcount_g += eqos_reg_read(MMC_TXOCTETCOUNT_G_OFFSET);
	mmc->mmc_tx_framecount_g += eqos_reg_read(MMC_TXPACKETSCOUNT_G_OFFSET);
	mmc->mmc_tx_excessdef += eqos_reg_read(MMC_TXEXCESSDEF_OFFSET);
	mmc->mmc_tx_pause_frame += eqos_reg_read(MMC_TXPAUSEPACKETS_OFFSET);
	mmc->mmc_tx_vlan_frame_g += eqos_reg_read(MMC_TXVLANPACKETS_G_OFFSET);
	mmc->mmc_tx_osize_frame_g += eqos_reg_read(MMC_TXOVERSIZE_G_OFFSET);

	/* MMC RX counter registers */
	mmc->mmc_rx_framecount_gb += eqos_reg_read(MMC_RXPACKETCOUNT_GB_OFFSET);
	mmc->mmc_rx_octetcount_gb += eqos_reg_read(MMC_RXOCTETCOUNT_GB_OFFSET);
	mmc->mmc_rx_octetcount_g += eqos_reg_read(MMC_RXOCTETCOUNT_G_OFFSET);
	mmc->mmc_rx_broadcastframe_g += eqos_reg_read(MMC_RXBROADCASTPACKETS_G_OFFSET);
	mmc->mmc_rx_multicastframe_g += eqos_reg_read(MMC_RXMULTICASTPACKETS_G_OFFSET);
	mmc->mmc_rx_crc_errror += eqos_reg_read(MMC_RXCRCERROR_OFFSET);
	mmc->mmc_rx_align_error += eqos_reg_read(MMC_RXALIGNMENTERROR_OFFSET);
	mmc->mmc_rx_run_error += eqos_reg_read(MMC_RXRUNTERROR_OFFSET);
	mmc->mmc_rx_jabber_error += eqos_reg_read(MMC_RXJABBERERROR_OFFSET);
	mmc->mmc_rx_undersize_g += eqos_reg_read(MMC_RXUNDERSIZE_G_OFFSET);
	mmc->mmc_rx_oversize_g += eqos_reg_read(MMC_RXOVERSIZE_G_OFFSET);
	mmc->mmc_rx_64_octets_gb += eqos_reg_read(MMC_RX64OCTETS_GB_OFFSET);
	mmc->mmc_rx_65_to_127_octets_gb += eqos_reg_read(MMC_RX65TO127OCTETS_GB_OFFSET);
	mmc->mmc_rx_128_to_255_octets_gb += eqos_reg_read(MMC_RX128TO255OCTETS_GB_OFFSET);
	mmc->mmc_rx_256_to_511_octets_gb += eqos_reg_read(MMC_RX256TO511OCTETS_GB_OFFSET);
	mmc->mmc_rx_512_to_1023_octets_gb += eqos_reg_read(MMC_RX512TO1023OCTETS_GB_OFFSET);
	mmc->mmc_rx_1024_to_max_octets_gb += eqos_reg_read(MMC_RX1024TOMAXOCTETS_GB_OFFSET);
	mmc->mmc_rx_unicast_g += eqos_reg_read(MMC_RXUNICASTPACKETS_G_OFFSET);
	mmc->mmc_rx_length_error += eqos_reg_read(MMC_RXLENGTHERROR_OFFSET);
	mmc->mmc_rx_outofrangetype += eqos_reg_read(MMC_RXOUTOFRANGETYPE_OFFSET);
	mmc->mmc_rx_pause_frames += eqos_reg_read(MMC_RXPAUSEPACKETS_OFFSET);
	mmc->mmc_rx_fifo_overflow += eqos_reg_read(MMC_RXFIFOOVERFLOW_OFFSET);
	mmc->mmc_rx_vlan_frames_gb += eqos_reg_read(MMC_RXVLANPACKETS_GB_OFFSET);
	mmc->mmc_rx_watchdog_error += eqos_reg_read(MMC_RXWATCHDOGERROR_OFFSET);
	mmc->mmc_rx_receive_error += eqos_reg_read(MMC_RXRCVERROR_OFFSET);
	mmc->mmc_rx_ctrl_frames_g += eqos_reg_read(MMC_RXCTRLPACKETS_G_OFFSET);

	/* IPC */
	mmc->mmc_rx_ipc_intr_mask += eqos_reg_read(MMC_IPC_INTR_MASK_RX_OFFSET);
	mmc->mmc_rx_ipc_intr += eqos_reg_read(MMC_IPC_INTR_RX_OFFSET);

	/* IPv4 */
	mmc->mmc_rx_ipv4_gd += eqos_reg_read(MMC_RXIPV4_GD_PKTS_OFFSET);
	mmc->mmc_rx_ipv4_hderr += eqos_reg_read(MMC_RXIPV4_HDRERR_PKTS_OFFSET);
	mmc->mmc_rx_ipv4_nopay += eqos_reg_read(MMC_RXIPV4_NOPAY_PKTS_OFFSET);
	mmc->mmc_rx_ipv4_frag += eqos_reg_read(MMC_RXIPV4_FRAG_PKTS_OFFSET);
	mmc->mmc_rx_ipv4_udsbl += eqos_reg_read(MMC_RXIPV4_UBSBL_PKTS_OFFSET);

	/* IPV6 */
	mmc->mmc_rx_ipv6_gd += eqos_reg_read(MMC_RXIPV6_GD_PKTS_OFFSET);
	mmc->mmc_rx_ipv6_hderr += eqos_reg_read(MMC_RXIPV6_HDRERR_PKTS_OFFSET);
	mmc->mmc_rx_ipv6_nopay += eqos_reg_read(MMC_RXIPV6_NOPAY_PKTS_OFFSET);

	/* Protocols */
	mmc->mmc_rx_udp_gd += eqos_reg_read(MMC_RXUDP_GD_PKTS_OFFSET);
	mmc->mmc_rx_udp_err += eqos_reg_read(MMC_RXUDP_ERR_PKTS_OFFSET);
	mmc->mmc_rx_tcp_gd += eqos_reg_read(MMC_RXTCP_GD_PKTS_OFFSET);
	mmc->mmc_rx_tcp_err += eqos_reg_read(MMC_RXTCP_ERR_PKTS_OFFSET);
	mmc->mmc_rx_icmp_gd += eqos_reg_read(MMC_RXICMP_GD_PKTS_OFFSET);
	mmc->mmc_rx_icmp_err += eqos_reg_read(MMC_RXICMP_ERR_PKTS_OFFSET);

	/* IPv4 */
	mmc->mmc_rx_ipv4_gd_octets += eqos_reg_read(MMC_RXIPV4_GD_OCTETS_OFFSET);
	mmc->mmc_rx_ipv4_hderr_octets += eqos_reg_read(MMC_RXIPV4_HDRERR_OCTETS_OFFSET);
	mmc->mmc_rx_ipv4_nopay_octets += eqos_reg_read(MMC_RXIPV4_NOPAY_OCTETS_OFFSET);
	mmc->mmc_rx_ipv4_frag_octets += eqos_reg_read(MMC_RXIPV4_FRAG_OCTETS_OFFSET);
	mmc->mmc_rx_ipv4_udsbl_octets += eqos_reg_read(MMC_RXIPV4_UDSBL_OCTETS_OFFSET);

	/* IPV6 */
	mmc->mmc_rx_ipv6_gd_octets += eqos_reg_read(MMC_RXIPV6_GD_OCTETS_OFFSET);
	mmc->mmc_rx_ipv6_hderr_octets += eqos_reg_read(MMC_RXIPV6_HDRERR_OCTETS_OFFSET);
	mmc->mmc_rx_ipv6_nopay_octets += eqos_reg_read(MMC_RXIPV6_NOPAY_OCTETS_OFFSET);

	/* Protocols */
	mmc->mmc_rx_udp_gd_octets += eqos_reg_read(MMC_RXUDP_GD_OCTETS_OFFSET);
	mmc->mmc_rx_udp_err_octets += eqos_reg_read(MMC_RXUDP_ERR_OCTETS_OFFSET);
	mmc->mmc_rx_tcp_gd_octets += eqos_reg_read(MMC_RXTCP_GD_OCTETS_OFFSET);
	mmc->mmc_rx_tcp_err_octets += eqos_reg_read(MMC_RXTCP_ERR_OCTETS_OFFSET);
	mmc->mmc_rx_icmp_gd_octets += eqos_reg_read(MMC_RXICMP_GD_OCTETS_OFFSET);
	mmc->mmc_rx_icmp_err_octets += eqos_reg_read(MMC_RXICMP_ERR_OCTETS_OFFSET);

	DBGPR("<--eqos_mmc_read\n");
}


phy_interface_t eqos_get_phy_interface(struct eqos_prv_data *pdata)
{
	phy_interface_t ret = PHY_INTERFACE_MODE_MII;

	DBGPR("-->eqos_get_phy_interface\n");

	if (pdata->hw_feat.act_phy_sel == EQOS_GMII_MII) {
		if (pdata->hw_feat.gmii_sel)
			ret = PHY_INTERFACE_MODE_GMII;
		else if (pdata->hw_feat.mii_sel)
			ret = PHY_INTERFACE_MODE_MII;
	} else if (pdata->hw_feat.act_phy_sel == EQOS_RGMII) {
		ret = PHY_INTERFACE_MODE_RGMII;
	} else if (pdata->hw_feat.act_phy_sel == EQOS_SGMII) {
		ret = PHY_INTERFACE_MODE_SGMII;
	} else if (pdata->hw_feat.act_phy_sel == EQOS_TBI) {
		ret = PHY_INTERFACE_MODE_TBI;
	} else if (pdata->hw_feat.act_phy_sel == EQOS_RMII) {
		ret = PHY_INTERFACE_MODE_RMII;
	} else if (pdata->hw_feat.act_phy_sel == EQOS_RTBI) {
		ret = PHY_INTERFACE_MODE_RTBI;
	} else if (pdata->hw_feat.act_phy_sel == EQOS_SMII) {
		ret = PHY_INTERFACE_MODE_SMII;
	} else if (pdata->hw_feat.act_phy_sel == EQOS_REV_MII) {
		//what to return ?
	} else {
		printk(KERN_ALERT "Missing interface support between"\
		    "PHY and MAC\n\n");
		ret = PHY_INTERFACE_MODE_NA;
	}

	DBGPR("<--eqos_get_phy_interface\n");

	return ret;
}

static const struct net_device_ops eqos_netdev_ops = {
	.ndo_open = eqos_open,
	.ndo_stop = eqos_close,
	.ndo_start_xmit = eqos_start_xmit,
	.ndo_get_stats = eqos_get_stats,
	.ndo_set_rx_mode = eqos_set_rx_mode,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = eqos_poll_controller,
#endif				/*end of CONFIG_NET_POLL_CONTROLLER */
	.ndo_set_features = eqos_set_features,
	.ndo_fix_features = eqos_fix_features,
	.ndo_do_ioctl = eqos_ioctl,
	.ndo_change_mtu = eqos_change_mtu,
#ifdef EQOS_QUEUE_SELECT_ALGO
	.ndo_select_queue = eqos_select_queue,
#endif
	.ndo_vlan_rx_add_vid = eqos_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid = eqos_vlan_rx_kill_vid,
	.ndo_set_mac_address = eqos_set_mac_address,
};

struct net_device_ops *eqos_get_netdev_ops(void)
{
	return (struct net_device_ops *)&eqos_netdev_ops;
}


/* Wrapper timer handler.  Only used when configured for multi irq and at least
 * one channel is being polled.
 */
static void eqos_poll_timer(unsigned long data)
{
	struct chan_data *pchinfo = (struct chan_data *) data;

	DBGPR("-->%s(): chan=%d\n", __func__, pchinfo->chan_num);

	eqos_poll_chan_mq_nv(pchinfo);
	add_timer(&pchinfo->poll_timer);

	DBGPR("<--%s():\n", __func__);
}


/* Wrapper timer handler.  Only used when configured for single irq
 * and all chans are being polled
 */
static void eqos_all_chans_timer(unsigned long data)
{
	struct eqos_prv_data *pdata =
		(struct eqos_prv_data *)data;

	eqos_poll_mq_all_chans(pdata);

	pdata->all_chans_timer.expires = jiffies +
					msecs_to_jiffies(1000);  /* 1s */
	add_timer(&pdata->all_chans_timer);
}

static void eqos_disable_all_irqs(struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &pdata->hw_if;
	int	i;

	DBGPR("-->%s()\n", __func__);

	/* disable all interrupts */
	if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ) {
		for (i = 0; i < MAX_CHANS; i++)
			hw_if->disable_chan_interrupts(i, pdata);
	} else {
		for (i = 0; i < MAX_CHANS; i++) {
			DMA_IER_WR(i, 0);
			DMA_SR_WR(i, 0xffffffff);
		}
	}
	/* disable mac interrupts */
	MAC_IMR_WR(0);

	/* ensure irqs are not executing */
	if (pdata->dt_cfg.intr_mode == MODE_MULTI_IRQ) {
		synchronize_irq(pdata->common_irq);
		for (i = 0; i < MAX_CHANS; i++) {
			if (pdata->rx_irq_alloc_mask & (1 << i))
				synchronize_irq(pdata->rx_irqs[i]);
			if (pdata->tx_irq_alloc_mask & (1 << i))
				synchronize_irq(pdata->tx_irqs[i]);
		}
	} else {
		if (pdata->irq_number != 0)
			synchronize_irq(pdata->irq_number);
	}

	DBGPR("<--%s()\n", __func__);
}

static void eqos_stop_dev(struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &pdata->hw_if;
	struct desc_if_struct *desc_if = &pdata->desc_if;

	DBGPR("-->%s()\n", __func__);

	/* turn off sources of data into dev */
	netif_tx_disable(pdata->dev);
	hw_if->stop_mac_rx();

	eqos_disable_all_irqs(pdata);
	stop_txrx_poll_timers(pdata);

	eqos_all_ch_napi_disable(pdata);

	/* stop DMA TX */
	eqos_stop_all_ch_tx_dma(pdata);

	/* disable MAC TX */
	hw_if->stop_mac_tx();

	/* stop the PHY */
	if (pdata->phydev)
		phy_stop(pdata->phydev);

	/* stop DMA RX */
	eqos_stop_all_ch_rx_dma(pdata);

	if (pdata->eee_enabled)
		del_timer_sync(&pdata->eee_ctrl_timer);

	/* return tx skbs */
	desc_if->tx_skb_free_mem(pdata, MAX_CHANS);

	/* free rx skb's */
	desc_if->rx_skb_free_mem(pdata, MAX_CHANS);

	DBGPR("<--%s()\n", __func__);
}

static void eqos_start_dev(struct eqos_prv_data *pdata)
{
	struct hw_if_struct *hw_if = &pdata->hw_if;
	struct desc_if_struct *desc_if = &pdata->desc_if;

	DBGPR("-->%s()\n", __func__);

	/* issue CAR reset to device */
	hw_if->car_reset(pdata);
	hw_if->pad_calibrate(pdata);

	/* default configuration */
	eqos_default_common_confs(pdata);
	eqos_default_tx_confs(pdata);
	eqos_default_rx_confs(pdata);
	eqos_configure_rx_fun_ptr(pdata);

	desc_if->wrapper_tx_desc_init(pdata);
	desc_if->wrapper_rx_desc_init(pdata);

	eqos_napi_enable_mq(pdata);

	eqos_set_rx_mode(pdata->dev);
	eqos_mmc_setup(pdata);

	start_txrx_poll_timers(pdata);

	/* initializes MAC and DMA */
	hw_if->init(pdata);

	MAC_1US_TIC_WR(pdata->csr_clock_speed - 1);

	if (pdata->hw_feat.pcs_sel)
		hw_if->control_an(1, 0);

	if (pdata->phydev) {
		pdata->oldlink = 0;
		pdata->speed = 0;
		pdata->oldduplex = -1;

		phy_start(pdata->phydev);
		phy_start_machine(pdata->phydev);
	}

#ifdef DWC_ETH_QOS_ENABLE_EEE
	pdata->eee_enabled = DWC_ETH_QOS_eee_init(pdata);
#else
	pdata->eee_enabled = false;
#endif

	if (pdata->phydev)
		netif_tx_start_all_queues(pdata->dev);

	DBGPR("<--%s()\n", __func__);
}

void eqos_fbe_work(struct work_struct *work)
{
	struct eqos_prv_data *pdata =
			container_of(work, struct eqos_prv_data, fbe_work);
	int i;
	u32 dma_sr_reg;

	DBGPR("-->%s()\n", __func__);

	i = 0;
	while (pdata->fbe_chan_mask) {
		if (pdata->fbe_chan_mask & 1) {
			DMA_SR_RD(i, dma_sr_reg);

			dev_err(&pdata->pdev->dev,
				"Fatal Bus Error on chan %d, SRreg=0x%.8x\n",
				i, dma_sr_reg);
		}
		pdata->fbe_chan_mask >>= 1;
		i++;
	}

	eqos_stop_dev(pdata);
	eqos_start_dev(pdata);

	DBGPR("<--%s()\n", __func__);
}
