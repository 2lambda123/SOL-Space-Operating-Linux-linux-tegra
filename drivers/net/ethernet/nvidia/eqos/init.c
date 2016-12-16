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
 * =========================================================================
 */
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


/*!@file: eqos_init.c
 * @brief: Driver functions.
 */
#include "yheader.h"
#include "init.h"
#include "yregacc.h"
#include "nvregacc.h"
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/tegra-soc.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#define LP_SUPPORTED 0
static const struct of_device_id eqos_of_match[] = {
	{ .compatible = "nvidia,eqos" },
	{},
};
MODULE_DEVICE_TABLE(of, eqos_of_match);

ULONG eqos_base_addr;

void eqos_init_all_fptrs(struct eqos_prv_data *pdata)
{
	eqos_init_function_ptrs_dev(&pdata->hw_if);
	eqos_init_function_ptrs_desc(&pdata->desc_if);
}

/*!
* \brief POWER Interrupt Service Routine
* \details POWER Interrupt Service Routine
*
* \param[in] irq         - interrupt number for particular device
* \param[in] device_id   - pointer to device structure
* \return returns positive integer
* \retval IRQ_HANDLED
*/
irqreturn_t EQOS_ISR_SW_EQOS_POWER(int irq, void *device_id)
{
	struct eqos_prv_data *pdata = (struct eqos_prv_data *)device_id;
	ULONG mac_isr;
	ULONG mac_imr;
	ULONG mac_pmtcsr;
	ULONG clk_ctrl = 0;

	if (tegra_platform_is_unit_fpga())
		CLK_CRTL0_RD(clk_ctrl);

	if (clk_ctrl & BIT(31)) {
		pr_info("power_isr: phy_intr received\n");
		return IRQ_NONE;
	} else {
		MAC_ISR_RD(mac_isr);
		MAC_IMR_RD(mac_imr);
		pr_info("power_isr: power_intr received, MAC_ISR =%#lx, MAC_IMR =%#lx\n",
				mac_isr, mac_imr);

		mac_isr = (mac_isr & mac_imr);

		/* RemoteWake and MagicPacket events will be received by PHY
		 * supporting these features on silicon and can be used to wake
		 * up Tegra. Still let the below code be there in case we ever
		 * get this interrupt.
		 */
		if (GET_VALUE(mac_isr, MAC_ISR_PMTIS_LPOS, MAC_ISR_PMTIS_HPOS)
			& 1) {
			pdata->xstats.pmt_irq_n++;
			MAC_PMTCSR_RD(mac_pmtcsr);
			pr_info("power_isr: PMTCSR : %#lx\n", mac_pmtcsr);
			if (pdata->power_down)
				eqos_powerup(pdata->dev, EQOS_IOCTL_CONTEXT);
		}

		/* RxLPI exit EEE interrupts */
		if (GET_VALUE(mac_isr, MAC_ISR_LPI_LPOS, MAC_ISR_LPI_HPOS)
			& 1) {
			pr_info("power_isr: LPI intr received\n");
			eqos_handle_eee_interrupt(pdata);
#ifdef HWA_NV_1650337
		/* FIXME: remove once root cause of HWA_NV_1650337 is known */
		} else {
			/* We have seen power_intr flood without LPIIS set in
			 * MAC_ISR and need to still read MAC_LPI_CONTROL_STS
			 * register to get rid of interrupt storm issue.
			 */
			pr_info("power_isr: LPIIS not set in MAC_ISR but still"
				" reading MAC_LPI_CONTROL_STS\n");
			eqos_handle_eee_interrupt(pdata);
#endif
		}

		return IRQ_HANDLED;
	}
}

void get_dt_u32(struct eqos_prv_data *pdata, char *pdt_prop, u32 *pval,
		u32 val_def, u32 val_max)
{
	struct device_node *pnode = pdata->pdev->dev.of_node;
	int ret = 0;

	ret = of_property_read_u32(pnode, pdt_prop, pval);

	if (ret < 0)
		dev_err(&pdata->pdev->dev,
			"%s(): \"%s\" read failed %d. Using default\n",
			__func__, pdt_prop, ret);

	if (*pval > val_max) {
		dev_err(&pdata->pdev->dev,
			"%s(): %d is invalid value for \"%s\".  Using default.\n",
			__func__, *pval, pdt_prop);
		*pval = val_def;
	}
}


void get_dt_u32_array(struct eqos_prv_data *pdata, char *pdt_prop, u32 *pval,
			u32 val_def, u32 val_max, u32 num_entries)
{
	struct device_node *pnode = pdata->pdev->dev.of_node;
	int i, ret = 0;

	ret = of_property_read_u32_array(pnode, pdt_prop, pval, num_entries);

	if (ret < 0) {
		pr_err("%s(): \"%s\" read failed %d. Using default\n",
			__func__, pdt_prop, ret);
		for (i = 0; i < num_entries; i++)
			pval[i] = val_def;
	}
	for (i = 0; i < num_entries; i++) {
		if (!strcmp(pdt_prop, "nvidia,queue_prio")) {
			int j;

			if (pval[i] > val_max) {
				dev_err(&pdata->pdev->dev,
					"%d is invalid value for"
					" queue_prio[%d], using default %d\n",
					pval[i], i, i + val_def);
				pval[i] = val_def + i;
			}
			/* q_prio need to be exclusive for each queue */
			for (j = 0; j < i; j++) {
				if (i == j)
					continue;
				/* use default if two priorities are same */
				if (pval[i] == pval[j]) {
					dev_err(&pdata->pdev->dev,
						"queue_prio %d same"
						" for q%d and q%d, using default\n",
						pval[i], i, j);
					pval[i] = val_def + i;
					pval[j] = val_def + j;
				}
			}
		} else if (pval[i] > val_max) {
			dev_err(&pdata->pdev->dev,
				"%d is invalid value for \"%s[%d]\"."
				"  Using default.\n",
				pval[i], pdt_prop, i);
			pval[i] = val_def;
		}
	}
}

static void eqos_clock_deinit(struct eqos_prv_data *pdata)
{
	struct platform_device *pdev = pdata->pdev;

	clk_disable_unprepare(pdata->tx_clk);
	clk_disable_unprepare(pdata->ptp_ref_clk);
	clk_disable_unprepare(pdata->rx_clk);
	clk_disable_unprepare(pdata->axi_clk);
	clk_disable_unprepare(pdata->axi_cbb_clk);

	devm_clk_put(&pdev->dev, pdata->tx_clk);
	devm_clk_put(&pdev->dev, pdata->ptp_ref_clk);
	devm_clk_put(&pdev->dev, pdata->rx_clk);
	devm_clk_put(&pdev->dev, pdata->axi_clk);
	devm_clk_put(&pdev->dev, pdata->axi_cbb_clk);
}

static int eqos_clock_init(struct eqos_prv_data *pdata)
{
	struct platform_device *pdev = pdata->pdev;
	struct device_node *node = pdev->dev.of_node;
	u32 ptp_ref_clock_speed;
	int ret;

	pdata->axi_cbb_clk = devm_clk_get(&pdev->dev, "axi_cbb");
	if (IS_ERR(pdata->axi_cbb_clk)) {
		ret = PTR_ERR(pdata->axi_cbb_clk);
		dev_err(&pdev->dev, "can't get axi_cbb clk (%d)\n", ret);
		return ret;
	}
	pdata->axi_clk = devm_clk_get(&pdev->dev, "eqos_axi");
	if (IS_ERR(pdata->axi_clk)) {
		ret = PTR_ERR(pdata->axi_clk);
		dev_err(&pdev->dev, "can't get eqos_axi clk (%d)\n", ret);
		goto axi_get_fail;
	}
	pdata->rx_clk = devm_clk_get(&pdev->dev, "eqos_rx");
	if (IS_ERR(pdata->rx_clk)) {
		ret = PTR_ERR(pdata->rx_clk);
		dev_err(&pdev->dev, "can't get eqos_rx clk (%d)\n", ret);
		goto rx_get_fail;
	}
	pdata->ptp_ref_clk = devm_clk_get(&pdev->dev, "eqos_ptp_ref");
	if (IS_ERR(pdata->ptp_ref_clk)) {
		ret = PTR_ERR(pdata->ptp_ref_clk);
		dev_err(&pdev->dev, "can't get eqos_ptp_ref clk (%d)\n", ret);
		goto ptp_ref_get_fail;
	}
	pdata->tx_clk = devm_clk_get(&pdev->dev, "eqos_tx");
	if (IS_ERR(pdata->tx_clk)) {
		ret = PTR_ERR(pdata->tx_clk);
		dev_err(&pdev->dev, "can't get eqos_tx clk (%d)\n", ret);
		goto tx_get_fail;
	}

	ret = clk_prepare_enable(pdata->axi_cbb_clk);
	if (ret < 0)
		goto axi_cbb_en_fail;

	ret = clk_prepare_enable(pdata->axi_clk);
	if (ret < 0)
		goto axi_en_fail;

	ret = clk_prepare_enable(pdata->rx_clk);
	if (ret < 0)
		goto rx_en_fail;

	ret = clk_prepare_enable(pdata->ptp_ref_clk);
	if (ret < 0)
		goto ptp_ref_en_fail;

	/* set ptp_ref_clk freq default 62.5Mhz */
	ret = of_property_read_u32(node, "nvidia,ptp_ref_clock_speed",
			&ptp_ref_clock_speed);
	if (ret < 0) {
		dev_err(&pdev->dev,
		"ptp_ref_clk read failed %d, setting default to 125MHz\n", ret);
		/* take default as 125MHz */
		ptp_ref_clock_speed = 125;
	} else if (ptp_ref_clock_speed > 625) { /* max parent clock is 625MHz */
		dev_warn(&pdev->dev,
		"ptp_ref_clk read set to more than 625MHz\n");
		/* take default as 125MHz */
		ptp_ref_clock_speed = 125;
	}

	ret = clk_set_rate(pdata->ptp_ref_clk, ptp_ref_clock_speed * 1000000);
	if (ret) {
		dev_err(&pdev->dev, "ptp_ref clk set rate failed (%d)\n", ret);
		goto ptp_ref_set_rate_failed;
	}

	ret = clk_prepare_enable(pdata->tx_clk);
	if (ret < 0)
		goto tx_en_fail;

	DBGPR("%s(): axi_cbb/axi/rx/ptp/tx = %ld/%ld/%ld/%ld/%ld\n",
		__func__,
		clk_get_rate(pdata->axi_cbb_clk),
		clk_get_rate(pdata->axi_clk), clk_get_rate(pdata->rx_clk),
		clk_get_rate(pdata->ptp_ref_clk), clk_get_rate(pdata->tx_clk));

	return 0;

tx_en_fail:
ptp_ref_set_rate_failed:
	clk_disable_unprepare(pdata->ptp_ref_clk);
ptp_ref_en_fail:
	clk_disable_unprepare(pdata->rx_clk);
rx_en_fail:
	clk_disable_unprepare(pdata->axi_clk);
axi_en_fail:
	clk_disable_unprepare(pdata->axi_cbb_clk);
axi_cbb_en_fail:
	devm_clk_put(&pdev->dev, pdata->tx_clk);
tx_get_fail:
	devm_clk_put(&pdev->dev, pdata->ptp_ref_clk);
ptp_ref_get_fail:
	devm_clk_put(&pdev->dev, pdata->rx_clk);
rx_get_fail:
	devm_clk_put(&pdev->dev, pdata->axi_clk);
axi_get_fail:
	devm_clk_put(&pdev->dev, pdata->axi_cbb_clk);
	return ret;
}

static void eqos_regulator_deinit(struct eqos_prv_data *pdata)
{
	if (!IS_ERR_OR_NULL(pdata->vddio_sys_enet_bias)) {
		regulator_disable(pdata->vddio_sys_enet_bias);
		devm_regulator_put(pdata->vddio_sys_enet_bias);
	}
	if (!IS_ERR_OR_NULL(pdata->vddio_enet)) {
		regulator_disable(pdata->vddio_enet);
		devm_regulator_put(pdata->vddio_enet);
	}
	if (!IS_ERR_OR_NULL(pdata->phy_pllvdd)) {
		regulator_disable(pdata->phy_pllvdd);
		devm_regulator_put(pdata->phy_pllvdd);
	}
	if (!IS_ERR_OR_NULL(pdata->phy_ovdd_rgmii)) {
		regulator_disable(pdata->phy_ovdd_rgmii);
		devm_regulator_put(pdata->phy_ovdd_rgmii);
	}
	if (!IS_ERR_OR_NULL(pdata->phy_vdd_1v8)) {
		regulator_disable(pdata->phy_vdd_1v8);
		devm_regulator_put(pdata->phy_vdd_1v8);
	}
}

static int eqos_regulator_init(struct eqos_prv_data *pdata)
{
	struct platform_device *pdev = pdata->pdev;
	int ret = 0;

	pdata->phy_vdd_1v8 = devm_regulator_get(&pdev->dev, "phy_vdd_1v8");
	if (IS_ERR(pdata->phy_vdd_1v8)) {
		ret = PTR_ERR(pdata->phy_vdd_1v8);
		dev_err(&pdev->dev, "phy_vdd_1v8 get failed %d\n", ret);
		return ret;
	}

	pdata->phy_ovdd_rgmii =
		devm_regulator_get(&pdev->dev, "phy_ovdd_rgmii");
	if (IS_ERR(pdata->phy_ovdd_rgmii)) {
		ret = PTR_ERR(pdata->phy_ovdd_rgmii);
		dev_err(&pdev->dev, "phy_ovdd_rgmii get failed %d\n", ret);
		goto phy_ovdd_rgmii_get_failed;
	}

	pdata->phy_pllvdd = devm_regulator_get(&pdev->dev,
		"phy_pllvdd");
	if (IS_ERR(pdata->phy_pllvdd)) {
		ret = PTR_ERR(pdata->phy_pllvdd);
		dev_err(&pdev->dev, "phy_pllvdd get failed %d\n", ret);
		goto phy_pllvdd_get_failed;
	}

	pdata->vddio_enet = devm_regulator_get(&pdev->dev, "vddio_enet");
	if (IS_ERR(pdata->vddio_enet)) {
		ret = PTR_ERR(pdata->vddio_enet);
		dev_err(&pdev->dev, "vddio_enet get failed %d\n", ret);
		goto vddio_enet_get_failed;
	}

	pdata->vddio_sys_enet_bias = devm_regulator_get(&pdev->dev,
		"vddio_sys_enet_bias");
	if (IS_ERR(pdata->vddio_sys_enet_bias)) {
		ret = PTR_ERR(pdata->vddio_sys_enet_bias);
		dev_err(&pdev->dev, "vddio_sys_enet_bias get failed %d\n", ret);
		goto vddio_sys_enet_bias_get_failed;
	}

	ret = regulator_enable(pdata->phy_vdd_1v8);
	if (ret) {
		dev_err(&pdev->dev, "phy_vdd_1v8 enable failed %d\n", ret);
		goto phy_vdd_1v8_enable_failed;
	}

	ret = regulator_enable(pdata->phy_ovdd_rgmii);
	if (ret) {
		dev_err(&pdev->dev, "phy_ovdd_rgmii enable failed %d\n", ret);
		goto phy_ovdd_rgmii_enable_failed;
	}

	ret = regulator_enable(pdata->phy_pllvdd);
	if (ret) {
		dev_err(&pdev->dev, "phy_pllvdd enable failed %d\n", ret);
		goto phy_pllvdd_enable_failed;
	}

	ret = regulator_enable(pdata->vddio_enet);
	if (ret) {
		dev_err(&pdev->dev, "vddio_enet enable failed %d\n", ret);
		goto vddio_enet_enable_failed;
	}

	ret = regulator_enable(pdata->vddio_sys_enet_bias);
	if (ret) {
		dev_err(&pdev->dev, "vddio_sys_enet_bias enable failed %d\n",
			ret);
		goto vddio_sys_enet_bias_enable_failed;
	}

	return 0;

vddio_sys_enet_bias_enable_failed:
	regulator_disable(pdata->vddio_enet);
vddio_enet_enable_failed:
	regulator_disable(pdata->phy_pllvdd);
phy_pllvdd_enable_failed:
	regulator_disable(pdata->phy_ovdd_rgmii);
phy_ovdd_rgmii_enable_failed:
	regulator_disable(pdata->phy_vdd_1v8);
phy_vdd_1v8_enable_failed:
	devm_regulator_put(pdata->vddio_sys_enet_bias);
vddio_sys_enet_bias_get_failed:
	devm_regulator_put(pdata->vddio_enet);
vddio_enet_get_failed:
	devm_regulator_put(pdata->phy_pllvdd);
phy_pllvdd_get_failed:
	devm_regulator_put(pdata->phy_ovdd_rgmii);
phy_ovdd_rgmii_get_failed:
	devm_regulator_put(pdata->phy_vdd_1v8);
	return ret;
}

static int eqos_get_phyreset_from_gpio(struct eqos_prv_data *pdata)
{
	struct platform_device *pdev = pdata->pdev;
	struct device_node *node = pdev->dev.of_node;
	int ret;

	pdata->phy_reset_gpio =
		of_get_named_gpio(node, "nvidia,phy-reset-gpio", 0);
	if (pdata->phy_reset_gpio < 0) {
		dev_err(&pdev->dev, "failed to read phy_reset_gpio\n");
		return -ENODEV;
	}
	if (gpio_is_valid(pdata->phy_reset_gpio)) {
		ret = devm_gpio_request_one(&pdev->dev, pdata->phy_reset_gpio,
				GPIOF_OUT_INIT_HIGH, "eqos_phy_reset");
		if (ret < 0) {
			dev_err(&pdev->dev, "phy_reset gpio_request failed\n");
			return ret;
		}
	} else {
		dev_err(&pdev->dev, "invalid phy_reset_gpio\n");
		return -ENODEV;
	}
	return 0;
}

static int eqos_get_phyirq_from_gpio(struct eqos_prv_data *pdata)
{
	struct platform_device *pdev = pdata->pdev;
	struct device_node *node = pdev->dev.of_node;
	int ret;

	pdata->phy_intr_gpio =
			of_get_named_gpio(node, "nvidia,phy-intr-gpio", 0);
	if (pdata->phy_intr_gpio < 0) {
		dev_err(&pdev->dev, "failed to read phy_intr_gpio\n");
		return -ENODEV;
	}
	if (gpio_is_valid(pdata->phy_intr_gpio)) {
		ret = devm_gpio_request_one(&pdev->dev, pdata->phy_intr_gpio,
				GPIOF_IN, "eqos_phy_intr");
		if (ret < 0) {
			dev_err(&pdev->dev, "phy_intr gpio_request failed\n");
			return ret;
		}
		ret = gpio_to_irq(pdata->phy_intr_gpio);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"gpio_to_irq failed for phy_intr\n");
			return ret;
		}
	} else {
		dev_err(&pdev->dev, "invalid phy_intr_gpio\n");
		return -ENODEV;
	}
	return ret;
}

/* Get MAC address from the specified DTB path */
static int eqos_get_mac_address_dtb(const char *node_name,
	const char *property_name, unsigned char *mac_addr)
{
	struct device_node *np = of_find_node_by_path(node_name);
	const char *mac_str = NULL;
	int values[6] = {0};
	unsigned char mac_temp[6] = {0};
	int i, ret = 0;

	if (!np)
		return -EADDRNOTAVAIL;

	/* If the property is present but contains an invalid value,
	 * then something is wrong. Log the error in that case.
	 */
	if (of_property_read_string(np, property_name, &mac_str)) {
		ret = -EADDRNOTAVAIL;
		goto err_out;
	}

	/* The DTB property is a string of the form xx:xx:xx:xx:xx:xx
	 * Convert to an array of bytes.
	 */
	if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
		&values[0], &values[1], &values[2],
		&values[3], &values[4], &values[5]) != 6) {
		ret = -EINVAL;
		goto err_out;
	}

	for (i = 0; i < 6; ++i)
		mac_temp[i] = (unsigned char)values[i];

	if (!is_valid_ether_addr(mac_temp)) {
		ret = -EINVAL;
		goto err_out;
	}

	memcpy(mac_addr, mac_temp, 6);

	of_node_put(np);

	return ret;

err_out:
	pr_err("%s: bad mac address at %s/%s: %s.\n",
		__func__, node_name, property_name,
		(mac_str) ? mac_str : "null");

	of_node_put(np);

	return ret;
}

/*!
* \brief API to initialize the device.
*
* \details This probing function gets called (during execution of
* pci_register_driver() for already existing devices or later if a
* new device gets inserted) for all PCI devices which match the ID table
* and are not "owned" by the other drivers yet. This function gets passed
* a "struct pci_dev *" for each device whose entry in the ID table matches
* the device. The probe function returns zero when the driver chooses to take
* "ownership" of the device or an error code (negative number) otherwise.
* The probe function always gets called from process context, so it can sleep.
*
* \param[in] pdev - pointer to pci_dev structure.
* \param[in] id   - pointer to table of device ID/ID's the driver is inerested.
*
* \return integer
*
* \retval 0 on success & -ve number on failure.
*/

int eqos_probe(struct platform_device *pdev)
{

	struct eqos_prv_data *pdata = NULL;
	struct net_device *ndev = NULL;
	int i, j, ret = 0;
	int irq, power_irq;
	int phyirq;
	int rx_irqs[MAX_CHANS];
	int tx_irqs[MAX_CHANS];
	struct hw_if_struct *hw_if = NULL;
	struct desc_if_struct *desc_if = NULL;
	struct resource *res;
	const struct of_device_id *match;
	struct device_node *node = pdev->dev.of_node;
	u8 mac_addr[6];

	struct eqos_cfg *pdt_cfg;
	struct chan_data *pchinfo;

	DBGPR("-->%s()\n", __func__);

	match = of_match_device(eqos_of_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	/* get base addr */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	if (unlikely(res == NULL)) {
		dev_err(&pdev->dev, "invalid resource\n");
		return -EINVAL;
	}

	/* get IRQ */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "invalid irq at index 0\n");
		return irq;
	}

	power_irq = platform_get_irq(pdev, 1);
	if (power_irq < 0) {
		dev_err(&pdev->dev, "invalid irq at index 1\n");
		return power_irq;
	}

	for (i = IRQ_CHAN0_RX_IDX, j = 0; i <= IRQ_MAX_IDX; i += 2, j++) {
		rx_irqs[j] = platform_get_irq(pdev, i);
		if (rx_irqs[j] < 0) {
			dev_err(&pdev->dev, "invalid irq at index %d\n", i);
			return rx_irqs[j];
		}

		tx_irqs[j] = platform_get_irq(pdev, i + 1);
		if (tx_irqs[j] < 0) {
			dev_err(&pdev->dev, "invalid irq at index %d\n", i + 1);
			return tx_irqs[j];
		}
	}

#if defined(CONFIG_PHYS_ADDR_T_64BIT)
	DBGPR("res->start = 0x%lx\n", (unsigned long)res->start);
	DBGPR("res->end = 0x%lx\n", (unsigned long)res->end);
#else
	DBGPR("res->start = 0x%x\n", (unsigned int)res->start);
	DBGPR("res->end = 0x%x\n", (unsigned int)res->end);
#endif
	DBGPR("irq = %d\n", irq);
	DBGPR("power_irq = %d\n", power_irq);

	for (j = 0; j < MAX_CHANS; j++)
		DBGPR("rx_irq[%d]=%d, tx_irq[%d]=%d\n",
			j, rx_irqs[j], j, tx_irqs[j]);

	DBGPR("============================================================\n");
	DBGPR("Sizeof rx context desc %lu\n", sizeof(struct s_rx_context_desc));
	DBGPR("Sizeof tx context desc %lu\n", sizeof(struct s_tx_context_desc));
	DBGPR("Sizeof rx normal desc %lu\n", sizeof(struct s_rx_desc));
	DBGPR("Sizeof tx normal desc %lu\n\n", sizeof(struct s_tx_desc));
	DBGPR("============================================================\n");

	/* remap base address */
	eqos_base_addr = (ULONG) devm_ioremap_nocache(&pdev->dev,
		res->start, (res->end - res->start) + 1);

	/* Set DMA addressing limitations */
	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))) {
		dev_err(&pdev->dev, "dma_set_mask_and_coherent failed\n");
		goto err_out_dev_failed;
	}

	/* allocate and set up the ethernet device */
	ndev = alloc_etherdev_mqs(sizeof(struct eqos_prv_data),
				MAX_CHANS, MAX_CHANS);
	if (ndev == NULL) {
		pr_err("%s:Unable to alloc new net device\n",
		    DEV_NAME);
		ret = -ENOMEM;
		goto err_out_dev_failed;
	}

	ndev->base_addr = eqos_base_addr;
	SET_NETDEV_DEV(ndev, &pdev->dev);
	pdata = netdev_priv(ndev);
	eqos_init_all_fptrs(pdata);
	hw_if = &(pdata->hw_if);
	desc_if = &(pdata->desc_if);

	platform_set_drvdata(pdev, ndev);
	pdata->pdev = pdev;

	pdata->dev = ndev;

	/* PMT and PHY irqs are shared on FPGA system */
	if (tegra_platform_is_unit_fpga()) {
		phyirq = power_irq;
		/* issue sw reset to device */
		hw_if->exit();
	} else {
		/* regulator init */
		ret = eqos_regulator_init(pdata);
		if (ret < 0) {
			dev_err(&pdev->dev,
				"failed to enable regulator %d\n", ret);
			goto err_out_regulator_en_failed;
		}

		/* On silicon the phy_intr line is handled through a wake
		 * capable GPIO input. DMIC4_CLK is the GPIO input port.
		 */
		phyirq = eqos_get_phyirq_from_gpio(pdata);
		if (phyirq < 0) {
			dev_err(&pdev->dev, "get_phyirq_from_gpio failed\n");
			goto err_out_phyirq_failed;
		}

		/* setup PHY reset gpio */
		ret = eqos_get_phyreset_from_gpio(pdata);
		if (ret < 0) {
			dev_err(&pdev->dev, "get_phyreset_from_gpio failed\n");
			goto err_out_phyreset_failed;
		}

		/* reset the PHY Broadcom PHY needs minimum of 2us delay */
		gpio_set_value(pdata->phy_reset_gpio, 0);
		usleep_range(10, 11);
		gpio_set_value(pdata->phy_reset_gpio, 1);

		/* CAR reset */
		pdata->eqos_rst =
			devm_reset_control_get(&pdev->dev, "eqos_rst");
		if (IS_ERR_OR_NULL(pdata->eqos_rst)) {
			ret = PTR_ERR(pdata->eqos_rst);
			dev_err(&pdev->dev,
				"failed to get eqos reset %d\n", ret);
			goto err_out_reset_get_failed;
		}

		/* clock initialization */
		ret = eqos_clock_init(pdata);
		if (ret < 0) {
			dev_err(&pdev->dev, "eqos_clock_init failed\n");
			goto err_out_clock_init_failed;
		}

		/* issue CAR reset to device */
		hw_if->car_reset(pdata);
	}
	DBGPR("phyirq = %d\n", phyirq);

	/* calibrate pad */
	ret = hw_if->pad_calibrate(pdata);
	if (ret < 0)
		goto err_out_pad_calibrate_failed;

	/* queue count */
	pdata->tx_queue_cnt = get_tx_queue_count();
	pdata->rx_queue_cnt = get_rx_queue_count();

#ifdef EQOS_CONFIG_DEBUGFS
	/* to give prv data to debugfs */
	eqos_get_pdata(pdata);
#endif

	ndev->irq = irq;
	pdata->common_irq = irq;

	pdata->power_irq = power_irq;
	pdata->phyirq = phyirq;

	for (j = 0; j < MAX_CHANS; j++) {
		pdata->rx_irqs[j] = rx_irqs[j];
		pdata->tx_irqs[j] = tx_irqs[j];
	}

	eqos_get_all_hw_features(pdata);

#ifdef YDEBUG
	eqos_print_all_hw_features(pdata);
#endif

	ret = desc_if->alloc_queue_struct(pdata);
	if (ret < 0) {
		pr_err("ERROR: Unable to alloc Tx/Rx queue\n");
		goto err_out_q_alloc_failed;
	}

	ndev->netdev_ops = eqos_get_netdev_ops();

	pdata->interface = eqos_get_phy_interface(pdata);
	/* Bypass PHYLIB for TBI, RTBI and SGMII interface */
	if (1 == pdata->hw_feat.sma_sel) {
		ret = eqos_mdio_register(ndev);
		if (ret < 0) {
			pr_err("MDIO bus (id %d) registration failed\n",
			       pdata->bus_id);
			goto err_out_mdio_reg;
		}
	} else {
		pr_err("%s: MDIO is not present\n\n", DEV_NAME);
	}

	if (pdata->phydev)
		phy_stop(pdata->phydev);

	pdata->ptp_cfg.use_tagged_ptp = of_property_read_bool(node,
			"nvidia,use_tagged_ptp");
	get_dt_u32(pdata, "nvidia,ptp_dma_ch",
		&(pdata->ptp_cfg.ptp_dma_ch_id),
		PTP_DMA_CH_DEFAULT, PTP_DMA_CH_MAX);

	pdt_cfg = (struct eqos_cfg *)&pdata->dt_cfg;
	get_dt_u32(pdata, "nvidia,pause_frames", &pdt_cfg->pause_frames,
			PAUSE_FRAMES_DEFAULT, PAUSE_FRAMES_MAX);
	get_dt_u32_array(pdata, "nvidia,chan_napi_quota",
			pdt_cfg->chan_napi_quota,
			CHAN_NAPI_QUOTA_DEFAULT, CHAN_NAPI_QUOTA_MAX, 4);
	get_dt_u32_array(pdata, "nvidia,rxq_enable_ctrl", pdt_cfg->rxq_ctrl,
			RXQ_CTRL_DEFAULT, RXQ_CTRL_MAX, 4);
	get_dt_u32_array(pdata, "nvidia,queue_prio", pdt_cfg->q_prio,
			QUEUE_PRIO_DEFAULT, QUEUE_PRIO_MAX, 4);

	for (i = 0; i < MAX_CHANS; i++) {
		pchinfo = &pdata->chinfo[i];
		pchinfo->chan_num = i;
		pchinfo->int_mask = VIRT_INTR_CH_CRTL_RX_WR_MASK;

		/* enable tx interrupts for all chan */
		pchinfo->int_mask |= VIRT_INTR_CH_CRTL_TX_WR_MASK;
	}

	for (i = 0; i < MAX_CHANS; i++)
		pdata->napi_quota_all_chans += pdt_cfg->chan_napi_quota[i];

	/* csr_clock_speed is axi_cbb_clk rate */
	pdata->csr_clock_speed = clk_get_rate(pdata->axi_cbb_clk) / 1000000;
	if (pdata->csr_clock_speed <= 0) {
		dev_err(&pdev->dev, "fail to read axi_cbb_clk rate\n");
	} else {
		DBGPR("setting MAC_1US_TIC to %d MHz\n",
			pdata->csr_clock_speed);
			MAC_1US_TIC_WR(pdata->csr_clock_speed - 1);
	}

	ret = eqos_get_mac_address_dtb("/chosen", "nvidia,ether-mac", mac_addr);
	if (ret < 0) {
		pr_err("ether-mac read from DT failed %d\n", ret);
	} else {
		pr_err("Setting local MAC: %x %x %x %x %x %x\n",
			mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3],
			mac_addr[4], mac_addr[5]);
		/* Set up MAC address */
		ndev->dev_addr[0] = mac_addr[0];
		ndev->dev_addr[1] = mac_addr[1];
		ndev->dev_addr[2] = mac_addr[2];
		ndev->dev_addr[3] = mac_addr[3];
		ndev->dev_addr[4] = mac_addr[4];
		ndev->dev_addr[5] = mac_addr[5];
	}
	/* enabling and registration of irq with magic wakeup */
	if (1 == pdata->hw_feat.mgk_sel) {
		device_set_wakeup_capable(&pdev->dev, 1);
		pdata->wolopts = WAKE_MAGIC;
		enable_irq_wake(ndev->irq);
	}

	for (i = 0; i < EQOS_RX_QUEUE_CNT; i++) {
		struct eqos_rx_queue *rx_queue = GET_RX_QUEUE_PTR(i);

		netif_napi_add(ndev, &rx_queue->napi,
			       eqos_napi_mq, pdata->napi_quota_all_chans);
		rx_queue->chan_num = i;
	}

	ndev->ethtool_ops = (eqos_get_ethtool_ops());

	if (pdata->hw_feat.tso_en) {
		ndev->hw_features = NETIF_F_TSO;
		ndev->hw_features |= NETIF_F_SG;
		ndev->hw_features |= NETIF_F_IP_CSUM;
		ndev->hw_features |= NETIF_F_IPV6_CSUM;
	} else if (pdata->hw_feat.tx_coe_sel) {
		ndev->hw_features = NETIF_F_IP_CSUM;
		ndev->hw_features |= NETIF_F_IPV6_CSUM;
	}

	if (pdata->hw_feat.rx_coe_sel) {
		ndev->hw_features |= NETIF_F_RXCSUM;
		ndev->hw_features |= NETIF_F_LRO;
	}
#ifdef EQOS_ENABLE_VLAN_TAG
	ndev->vlan_features |= ndev->hw_features;
	ndev->hw_features |= NETIF_F_HW_VLAN_CTAG_RX;
	if (pdata->hw_feat.sa_vlan_ins)
		ndev->hw_features |= NETIF_F_HW_VLAN_CTAG_TX;
	if (pdata->hw_feat.vlan_hash_en)
		ndev->hw_features |= NETIF_F_HW_VLAN_CTAG_FILTER;
#endif /* end of EQOS_ENABLE_VLAN_TAG */
	ndev->features |= ndev->hw_features;
	pdata->dev_state |= ndev->features;

	eqos_init_rx_coalesce(pdata);

#ifdef EQOS_CONFIG_PTP
	eqos_ptp_init(pdata);
#endif	/* end of EQOS_CONFIG_PTP */

	spin_lock_init(&pdata->lock);
	spin_lock_init(&pdata->tx_lock);
	spin_lock_init(&pdata->pmt_lock);

	for (i = 0; i < MAX_CHANS; i++)
		spin_lock_init(&pdata->chinfo[i].chan_lock);

	ret = register_netdev(ndev);
	if (ret) {
		pr_err("%s: Net device registration failed\n",
		    DEV_NAME);
		goto err_out_netdev_failed;
	}

	DBGPR("<-- eqos_probe\n");

	if (pdata->hw_feat.pcs_sel) {
		netif_carrier_off(ndev);
		pr_err("carrier off till LINK is up\n");
	} else
		DBGPR("Net device registration sucessful\n");

	if (tegra_platform_is_unit_fpga()) {
		ret = request_irq(power_irq, EQOS_ISR_SW_EQOS_POWER,
			IRQF_SHARED, DEV_NAME, pdata);

		if (ret != 0) {
			pr_err("Unable to register PMT IRQ %d\n", power_irq);
			ret = -EBUSY;
			goto err_out_pmt_irq_failed;
		}
	}

	pdata->fbe_wq = alloc_workqueue("FBE WQ\n", WQ_HIGHPRI|WQ_UNBOUND, 0);
	if (!pdata->fbe_wq) {
		dev_err(&pdev->dev, "Work Queue Allocation Failed\n");
		goto err_out_fbe_wq_failed;
	}
	INIT_WORK(&pdata->fbe_work, eqos_fbe_work);

	return 0;

 err_out_fbe_wq_failed:
	if ((tegra_platform_is_unit_fpga()) &&
		(pdata->power_irq != 0)) {
		free_irq(pdata->power_irq, pdata);
		pdata->power_irq = 0;
	}

 err_out_pmt_irq_failed:
	unregister_netdev(ndev);

 err_out_netdev_failed:

#ifdef EQOS_CONFIG_PTP
	eqos_ptp_remove(pdata);
#endif	/* end of EQOS_CONFIG_PTP */

	/* remove rx napi */
	for (i = 0; i < EQOS_RX_QUEUE_CNT; i++) {
		struct eqos_rx_queue *rx_queue = GET_RX_QUEUE_PTR(i);
		netif_napi_del(&rx_queue->napi);
	}
	if (1 == pdata->hw_feat.sma_sel)
		eqos_mdio_unregister(ndev);

 err_out_mdio_reg:
	desc_if->free_queue_struct(pdata);

 err_out_q_alloc_failed:
 err_out_pad_calibrate_failed:
	if (!tegra_platform_is_unit_fpga())
		eqos_clock_deinit(pdata);
 err_out_clock_init_failed:
	if (!tegra_platform_is_unit_fpga() &&
		!IS_ERR_OR_NULL(pdata->eqos_rst)) {
		reset_control_assert(pdata->eqos_rst);
	}
 err_out_reset_get_failed:
	if (!tegra_platform_is_unit_fpga())
		devm_gpio_free(&pdev->dev, pdata->phy_reset_gpio);
 err_out_phyreset_failed:
	if (!tegra_platform_is_unit_fpga())
		devm_gpio_free(&pdev->dev, pdata->phy_intr_gpio);
 err_out_phyirq_failed:
	if (!tegra_platform_is_unit_fpga())
		eqos_regulator_deinit(pdata);
 err_out_regulator_en_failed:
	free_netdev(ndev);
	platform_set_drvdata(pdev, NULL);

 err_out_dev_failed:
	devm_iounmap(&pdev->dev, (void *) eqos_base_addr);

	return ret;
}

/*!
* \brief API to release all the resources from the driver.
*
* \details The remove function gets called whenever a device being handled
* by this driver is removed (either during deregistration of the driver or
* when it is manually pulled out of a hot-pluggable slot). This function
* should reverse operations performed at probe time. The remove function
* always gets called from process context, so it can sleep.
*
* \param[in] pdev - pointer to pci_dev structure.
*
* \return void
*/

int eqos_remove(struct platform_device *pdev)
{
	struct net_device *ndev;
	struct eqos_prv_data *pdata;
	struct desc_if_struct *desc_if;
	int i, ret_val = 0;

	DBGPR("--> eqos_remove\n");

	if (pdev == NULL) {
		DBGPR("Remove called on invalid device\n");
		return -1;
	}

	ndev = platform_get_drvdata(pdev);
	pdata = netdev_priv(ndev);
	desc_if = &(pdata->desc_if);

	/* free tx skb's */
	desc_if->tx_skb_free_mem(pdata, EQOS_TX_QUEUE_CNT);

	if ((tegra_platform_is_unit_fpga()) &&
	    (pdata->power_irq != 0)) {
		free_irq(pdata->power_irq, pdata);
		pdata->power_irq = 0;
	}

	unregister_netdev(ndev);

#ifdef EQOS_CONFIG_PTP
	eqos_ptp_remove(pdata);
#endif	/* end of EQOS_CONFIG_PTP */

	/* remove rx napi */
	for (i = 0; i < EQOS_RX_QUEUE_CNT; i++) {
		struct eqos_rx_queue *rx_queue = GET_RX_QUEUE_PTR(i);
		netif_napi_del(&rx_queue->napi);
	}

	if (1 == pdata->hw_feat.sma_sel)
		eqos_mdio_unregister(ndev);

	desc_if->free_queue_struct(pdata);

	if (!tegra_platform_is_unit_fpga()) {
		eqos_clock_deinit(pdata);

		if (!IS_ERR_OR_NULL(pdata->eqos_rst))
			reset_control_assert(pdata->eqos_rst);
		devm_gpio_free(&pdev->dev, pdata->phy_reset_gpio);
		devm_gpio_free(&pdev->dev, pdata->phy_intr_gpio);
		eqos_regulator_deinit(pdata);
	}

	free_netdev(ndev);

	platform_set_drvdata(pdev, NULL);

	devm_iounmap(&pdev->dev, (void *) eqos_base_addr);

	DBGPR("<-- eqos_remove\n");

	return ret_val;
}

static struct platform_driver eqos_driver = {

	.probe = eqos_probe,
	.remove = eqos_remove,
	.shutdown = eqos_shutdown,
#if 0
	.suspend_late = eqos_suspend_late,
	.resume_early = eqos_resume_early,
#endif
#ifdef CONFIG_PM
	.suspend = eqos_suspend,
	.resume = eqos_resume,
#endif
	.driver = {
		   .name = DEV_NAME,
		   .owner = THIS_MODULE,
			 .of_match_table = eqos_of_match,
	},
};

static void eqos_shutdown(struct platform_device *pdev)
{
	pr_err("-->eqos_shutdown\n");
	pr_err("Handle the shutdown\n");
	pr_err(">--eqos_shutdown\n");

	return;
}

#if 0
static INT eqos_suspend_late(struct platform_device *pdev, pm_message_t state)
{
	pr_err("-->eqos_suspend_late\n");
	pr_err("Handle the suspend_late\n");
	pr_err("<--eqos_suspend_late\n");

	return 0;
}

static INT eqos_resume_early(struct platform_device *pdev)
{
	pr_err("-->eqos_resume_early\n");
	pr_err("Handle the resume_early\n");
	pr_err("<--eqos_resume_early\n");

	return 0;
}

#endif

#ifdef CONFIG_PM

/*!
 * \brief Routine to put the device in suspend mode
 *
 * \details This function gets called by PCI core when the device is being
 * suspended. The suspended state is passed as input argument to it.
 * Following operations are performed in this function,
 * - stop the phy.
 * - detach the device from stack.
 * - stop the queue.
 * - Disable napi.
 * - Stop DMA TX and RX process.
 * - Enable power down mode using PMT module or disable MAC TX and RX process.
 * - Save the pci state.
 *
 * \param[in] pdev – pointer to pci device structure.
 * \param[in] state – suspend state of device.
 *
 * \return int
 *
 * \retval 0
 */

static INT eqos_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct eqos_prv_data *pdata = netdev_priv(dev);

	if (pdata->suspended) {
		pr_err("eqos already suspended\n");
		return -EINVAL;
	}
	pdata->suspended = 1;
	eqos_stop_dev(pdata);

	/* disable clocks */
	eqos_clock_deinit(pdata);
	/* disable regulators */
	eqos_regulator_deinit(pdata);

	return 0;
}

/*!
 * \brief Routine to resume device operation
 *
 * \details This function gets called by PCI core when the device is being
 * resumed. It is always called after suspend has been called. These function
 * reverse operations performed at suspend time. Following operations are
 * performed in this function,
 * - restores the saved pci power state.
 * - Wakeup the device using PMT module if supported.
 * - Starts the phy.
 * - Enable MAC and DMA TX and RX process.
 * - Attach the device to stack.
 * - Enable napi.
 * - Starts the queue.
 *
 * \param[in] pdev – pointer to pci device structure.
 *
 * \return int
 *
 * \retval 0
 */

static INT eqos_resume(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct eqos_prv_data *pdata = netdev_priv(dev);

	if (!pdata->suspended) {
		pr_err("eqos already resumed\n");
		return -EINVAL;
	}

	/* start regulators */
	eqos_regulator_init(pdata);

	/* enable clocks */
	eqos_clock_init(pdata);

	eqos_start_dev(pdata);
	pdata->suspended = 0;

	return 0;
}

#endif	/* CONFIG_PM */

/*!
* \brief API to register the driver.
*
* \details This is the first function called when the driver is loaded.
* It register the driver with PCI sub-system
*
* \return void.
*/

static int eqos_init_module(void)
{
	INT ret = 0;

	DBGPR("-->eqos_init_module\n");

	ret = platform_driver_register(&eqos_driver);
	if (ret < 0) {
		DBGPR("eqos:driver registration failed\n");
		return ret;
	}
	DBGPR("eqos:driver registration sucessful\n");

#ifdef EQOS_CONFIG_DEBUGFS
	create_debug_files();
#endif

	DBGPR("<--eqos_init_module\n");

	return ret;
}

/*!
* \brief API to unregister the driver.
*
* \details This is the first function called when the driver is removed.
* It unregister the driver from PCI sub-system
*
* \return void.
*/

static void __exit eqos_exit_module(void)
{
	DBGPR("-->eqos_exit_module\n");

#ifdef EQOS_CONFIG_DEBUGFS
	remove_debug_files();
#endif

	platform_driver_unregister(&eqos_driver);

	DBGPR("<--eqos_exit_module\n");
}

/*!
* \brief Macro to register the driver registration function.
*
* \details A module always begin with either the init_module or the function
* you specify with module_init call. This is the entry function for modules;
* it tells the kernel what functionality the module provides and sets up the
* kernel to run the module's functions when they're needed. Once it does this,
* entry function returns and the module does nothing until the kernel wants
* to do something with the code that the module provides.
*/
module_init(eqos_init_module);

/*!
* \brief Macro to register the driver un-registration function.
*
* \details All modules end by calling either cleanup_module or the function
* you specify with the module_exit call. This is the exit function for modules;
* it undoes whatever entry function did. It unregisters the functionality
* that the entry function registered.
*/
module_exit(eqos_exit_module);

/*!
* \brief Macro to declare the module author.
*
* \details This macro is used to declare the module's authore.
*/
MODULE_AUTHOR("Synopsys India Pvt Ltd");

/*!
* \brief Macro to describe what the module does.
*
* \details This macro is used to describe what the module does.
*/
MODULE_DESCRIPTION("eqos Driver");

/*!
* \brief Macro to describe the module license.
*
* \details This macro is used to describe the module license.
*/
MODULE_LICENSE("GPL");
