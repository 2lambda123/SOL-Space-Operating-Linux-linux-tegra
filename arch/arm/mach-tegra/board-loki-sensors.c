/*
 * arch/arm/mach-tegra/board-loki-sensors.c
 *
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/mpu.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/nct1008.h>
#include <media/ar0261.h>
#include <media/imx135.h>
#include <media/dw9718.h>
#include <media/as364x.h>
#include <mach/gpio-tegra.h>
#include <linux/gpio.h>
#include <linux/therm_est.h>

#include "board.h"
#include "board-common.h"
#include "board-loki.h"
#include "tegra-board-id.h"
#include "dvfs.h"
#include "cpu-tegra.h"

static struct i2c_board_info loki_i2c_board_info_cm32181[] = {
	{
		I2C_BOARD_INFO("cm32181", 0x48),
	},
};

/* MPU board file definition    */
static struct mpu_platform_data mpu9250_gyro_data = {
	.int_config     = 0x10,
	.level_shifter  = 0,
	/* Located in board_[platformname].h */
	.orientation    = MPU_GYRO_ORIENTATION,
	.sec_slave_type = SECONDARY_SLAVE_TYPE_NONE,
	.key            = {0x4E, 0xCC, 0x7E, 0xEB, 0xF6, 0x1E, 0x35, 0x22,
			0x00, 0x34, 0x0D, 0x65, 0x32, 0xE9, 0x94, 0x89},
};

static struct mpu_platform_data mpu_compass_data = {
	.orientation    = MPU_COMPASS_ORIENTATION,
	.config         = NVI_CONFIG_BOOT_MPU,
};

static struct mpu_platform_data mpu_bmp_pdata = {
	.config         = NVI_CONFIG_BOOT_MPU,
};

static struct i2c_board_info __initdata inv_mpu9250_i2c0_board_info[] = {
	{
		I2C_BOARD_INFO(MPU_GYRO_NAME, MPU_GYRO_ADDR),
		.platform_data = &mpu9250_gyro_data,
	},
	{
		/* The actual BMP180 address is 0x77 but because this conflicts
		 * with another device, this address is hacked so Linux will
		 * call the driver.  The conflict is technically okay since the
		 * BMP180 is behind the MPU.  Also, the BMP180 driver uses a
		 * hard-coded address of 0x77 since it can't be changed anyway.
		 */
		I2C_BOARD_INFO(MPU_BMP_NAME, MPU_BMP_ADDR),
		.platform_data = &mpu_bmp_pdata,
	},
	{
		I2C_BOARD_INFO(MPU_COMPASS_NAME, MPU_COMPASS_ADDR),
		.platform_data = &mpu_compass_data,
	},
};

static void mpuirq_init(void)
{
	int ret = 0;
	unsigned gyro_irq_gpio = MPU_GYRO_IRQ_GPIO;
	unsigned gyro_bus_num = MPU_GYRO_BUS_NUM;
	char *gyro_name = MPU_GYRO_NAME;

	pr_info("*** MPU START *** mpuirq_init...\n");

	ret = gpio_request(gyro_irq_gpio, gyro_name);

	if (ret < 0) {
		pr_err("%s: gpio_request failed %d\n", __func__, ret);
		return;
	}

	ret = gpio_direction_input(gyro_irq_gpio);
	if (ret < 0) {
		pr_err("%s: gpio_direction_input failed %d\n", __func__, ret);
		gpio_free(gyro_irq_gpio);
		return;
	}
	pr_info("*** MPU END *** mpuirq_init...\n");

	inv_mpu9250_i2c0_board_info[0].irq = gpio_to_irq(MPU_GYRO_IRQ_GPIO);
	i2c_register_board_info(gyro_bus_num, inv_mpu9250_i2c0_board_info,
		ARRAY_SIZE(inv_mpu9250_i2c0_board_info));
}

static struct regulator *loki_vcmvdd;

static int loki_get_extra_regulators(void)
{
	if (!loki_vcmvdd) {
		loki_vcmvdd = regulator_get(NULL, "avdd_af1_cam");
		if (WARN_ON(IS_ERR(loki_vcmvdd))) {
			pr_err("%s: can't get regulator avdd_af1_cam: %ld\n",
					__func__, PTR_ERR(loki_vcmvdd));
			regulator_put(loki_vcmvdd);
			loki_vcmvdd = NULL;
			return -ENODEV;
		}
	}

	return 0;
}


static int loki_ar0261_power_on(struct ar0261_power_rail *pw)
{
	int err;

	if (unlikely(WARN_ON(!pw || !pw->avdd || !pw->iovdd || !pw->dvdd)))
		return -EFAULT;

	if (loki_get_extra_regulators())
		goto loki_ar0261_poweron_fail;

	gpio_set_value(CAM_RSTN, 0);
	gpio_set_value(CAM_AF_PWDN, 1);


	err = regulator_enable(loki_vcmvdd);
	if (unlikely(err))
		goto ar0261_vcm_fail;

	err = regulator_enable(pw->dvdd);
	if (unlikely(err))
		goto ar0261_dvdd_fail;

	err = regulator_enable(pw->avdd);
	if (unlikely(err))
		goto ar0261_avdd_fail;

	err = regulator_enable(pw->iovdd);
	if (unlikely(err))
		goto ar0261_iovdd_fail;

	usleep_range(1, 2);
	gpio_set_value(CAM2_PWDN, 1);

	gpio_set_value(CAM_RSTN, 1);

	return 0;
ar0261_iovdd_fail:
	regulator_disable(pw->dvdd);

ar0261_dvdd_fail:
	regulator_disable(pw->avdd);

ar0261_avdd_fail:
	regulator_disable(loki_vcmvdd);

ar0261_vcm_fail:
	pr_err("%s vcmvdd failed.\n", __func__);
	return -ENODEV;

loki_ar0261_poweron_fail:
	pr_err("%s failed.\n", __func__);
	return -ENODEV;
}

static int loki_ar0261_power_off(struct ar0261_power_rail *pw)
{
	if (unlikely(WARN_ON(!pw || !pw->avdd || !pw->iovdd || !pw->dvdd ||
					!loki_vcmvdd)))
		return -EFAULT;

	gpio_set_value(CAM_RSTN, 0);

	usleep_range(1, 2);

	regulator_disable(pw->iovdd);
	regulator_disable(pw->dvdd);
	regulator_disable(pw->avdd);


	regulator_disable(loki_vcmvdd);

	return 0;
}

struct ar0261_platform_data loki_ar0261_data = {
	.power_on = loki_ar0261_power_on,
	.power_off = loki_ar0261_power_off,
	.mclk_name = "vi_sensor2",
};

static int loki_imx135_get_extra_regulators(struct imx135_power_rail *pw)
{
	if (!pw->ext_reg1) {
		pw->ext_reg1 = regulator_get(NULL, "imx135_reg1");
		if (WARN_ON(IS_ERR(pw->ext_reg1))) {
			pr_err("%s: can't get regulator imx135_reg1: %ld\n",
				__func__, PTR_ERR(pw->ext_reg1));
			pw->ext_reg1 = NULL;
			return -ENODEV;
		}
	}

	if (!pw->ext_reg2) {
		pw->ext_reg2 = regulator_get(NULL, "imx135_reg2");
		if (WARN_ON(IS_ERR(pw->ext_reg2))) {
			pr_err("%s: can't get regulator imx135_reg2: %ld\n",
				__func__, PTR_ERR(pw->ext_reg2));
			pw->ext_reg2 = NULL;
			return -ENODEV;
		}
	}

	return 0;
}

static int loki_imx135_power_on(struct imx135_power_rail *pw)
{
	int err;

	if (unlikely(WARN_ON(!pw || !pw->iovdd || !pw->avdd)))
		return -EFAULT;

	if (loki_imx135_get_extra_regulators(pw))
		goto imx135_poweron_fail;

	err = regulator_enable(pw->ext_reg1);
	if (unlikely(err))
		goto imx135_ext_reg1_fail;

	err = regulator_enable(pw->ext_reg2);
	if (unlikely(err))
		goto imx135_ext_reg2_fail;


	gpio_set_value(CAM_RSTN, 0);
	gpio_set_value(CAM_AF_PWDN, 1);
	gpio_set_value(CAM1_PWDN, 0);
	usleep_range(10, 20);

	err = regulator_enable(pw->avdd);
	if (err)
		goto imx135_avdd_fail;

	err = regulator_enable(pw->iovdd);
	if (err)
		goto imx135_iovdd_fail;

	usleep_range(1, 2);
	gpio_set_value(CAM_RSTN, 1);
	gpio_set_value(CAM1_PWDN, 1);

	usleep_range(300, 310);

	return 1;


imx135_iovdd_fail:
	regulator_disable(pw->avdd);

imx135_avdd_fail:
	if (pw->ext_reg2)
		regulator_disable(pw->ext_reg2);

imx135_ext_reg2_fail:
	if (pw->ext_reg1)
		regulator_disable(pw->ext_reg1);
	gpio_set_value(CAM_AF_PWDN, 0);

imx135_ext_reg1_fail:
imx135_poweron_fail:
	pr_err("%s failed.\n", __func__);
	return -ENODEV;
}

static int loki_imx135_power_off(struct imx135_power_rail *pw)
{
	if (unlikely(WARN_ON(!pw || !pw->iovdd || !pw->avdd)))
		return -EFAULT;

	usleep_range(1, 2);
	gpio_set_value(CAM_RSTN, 0);
	usleep_range(1, 2);

	regulator_disable(pw->iovdd);
	regulator_disable(pw->avdd);

	regulator_disable(pw->ext_reg1);
	regulator_disable(pw->ext_reg2);

	return 0;
}

struct imx135_platform_data loki_imx135_data = {
	.power_on = loki_imx135_power_on,
	.power_off = loki_imx135_power_off,
};

static int loki_dw9718_power_on(struct dw9718_power_rail *pw)
{
	int err;
	pr_info("%s\n", __func__);

	if (unlikely(!pw || !pw->vdd || !pw->vdd_i2c))
		return -EFAULT;

	err = regulator_enable(pw->vdd);
	if (unlikely(err))
		goto dw9718_vdd_fail;

	err = regulator_enable(pw->vdd_i2c);
	if (unlikely(err))
		goto dw9718_i2c_fail;

	usleep_range(1000, 1020);

	/* return 1 to skip the in-driver power_on sequence */
	pr_debug("%s --\n", __func__);
	return 1;

dw9718_i2c_fail:
	regulator_disable(pw->vdd);

dw9718_vdd_fail:
	pr_err("%s FAILED\n", __func__);
	return -ENODEV;
}

static int loki_dw9718_power_off(struct dw9718_power_rail *pw)
{
	pr_info("%s\n", __func__);

	if (unlikely(!pw || !pw->vdd || !pw->vdd_i2c))
		return -EFAULT;

	regulator_disable(pw->vdd);
	regulator_disable(pw->vdd_i2c);

	return 1;
}

static u16 dw9718_devid;
static int loki_dw9718_detect(void *buf, size_t size)
{
	dw9718_devid = 0x9718;
	return 0;
}

static struct nvc_focus_cap dw9718_cap = {
	.settle_time = 30,
	.slew_rate = 0x3A200C,
	.focus_macro = 450,
	.focus_infinity = 200,
	.focus_hyper = 200,
};

static struct dw9718_platform_data loki_dw9718_data = {
	.cfg = NVC_CFG_NODEV,
	.num = 0,
	.sync = 0,
	.dev_name = "focuser",
	.cap = &dw9718_cap,
	.power_on = loki_dw9718_power_on,
	.power_off = loki_dw9718_power_off,
	.detect = loki_dw9718_detect,
};

static struct as364x_platform_data loki_as3648_data = {
	.config		= {
		.led_mask	= 3,
		.max_total_current_mA = 1000,
		.max_peak_current_mA = 600,
		.vin_low_v_run_mV = 3070,
		.strobe_type = 1,
		},
	.pinstate	= {
		.mask	= 1 << (CAM_FLASH_STROBE - TEGRA_GPIO_PBB0),
		.values	= 1 << (CAM_FLASH_STROBE - TEGRA_GPIO_PBB0)
		},
	.dev_name	= "torch",
	.type		= AS3648,
	.gpio_strobe	= CAM_FLASH_STROBE,
};


static struct i2c_board_info loki_i2c_board_info_e1823[] = {
	{
		I2C_BOARD_INFO("imx135", 0x10),
		.platform_data = &loki_imx135_data,
	},
	{
		I2C_BOARD_INFO("ar0261", 0x36),
		.platform_data = &loki_ar0261_data,
	},
	{
		I2C_BOARD_INFO("dw9718", 0x0c),
		.platform_data = &loki_dw9718_data,
	},
	{
		I2C_BOARD_INFO("as3648", 0x30),
		.platform_data = &loki_as3648_data,
	},
};


static int loki_camera_init(void)
{
	return 0;
}

static struct throttle_table tj_throttle_table[] = {
	/* CPU_THROT_LOW cannot be used by other than CPU */
	/*      CPU,  C2BUS,  C3BUS,   SCLK,    EMC   */
	{ { 1810500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1785000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1759500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1734000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1708500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1683000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1657500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1632000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1606500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1581000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1555500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1530000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1504500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1479000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1453500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1428000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1402500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1377000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1351500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1326000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1300500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1275000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1249500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1224000, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1198500, NO_CAP, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1173000, 636000, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1147500, 636000, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1122000, 636000, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1096500, 636000, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1071000, 636000, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1045500, 636000, NO_CAP, NO_CAP, NO_CAP } },
	{ { 1020000, 636000, NO_CAP, NO_CAP, NO_CAP } },
	{ {  994500, 636000, NO_CAP, NO_CAP, NO_CAP } },
	{ {  969000, 600000, NO_CAP, NO_CAP, NO_CAP } },
	{ {  943500, 600000, NO_CAP, NO_CAP, NO_CAP } },
	{ {  918000, 600000, NO_CAP, NO_CAP, NO_CAP } },
	{ {  892500, 600000, NO_CAP, NO_CAP, NO_CAP } },
	{ {  867000, 600000, NO_CAP, NO_CAP, NO_CAP } },
	{ {  841500, 564000, NO_CAP, NO_CAP, NO_CAP } },
	{ {  816000, 564000, NO_CAP, NO_CAP, 792000 } },
	{ {  790500, 564000, NO_CAP, 372000, 792000 } },
	{ {  765000, 564000, 468000, 372000, 792000 } },
	{ {  739500, 528000, 468000, 372000, 792000 } },
	{ {  714000, 528000, 468000, 336000, 792000 } },
	{ {  688500, 528000, 420000, 336000, 792000 } },
	{ {  663000, 492000, 420000, 336000, 792000 } },
	{ {  637500, 492000, 420000, 336000, 408000 } },
	{ {  612000, 492000, 420000, 300000, 408000 } },
	{ {  586500, 492000, 360000, 336000, 408000 } },
	{ {  561000, 420000, 420000, 300000, 408000 } },
	{ {  535500, 420000, 360000, 228000, 408000 } },
	{ {  510000, 420000, 288000, 228000, 408000 } },
	{ {  484500, 324000, 288000, 228000, 408000 } },
	{ {  459000, 324000, 288000, 228000, 408000 } },
	{ {  433500, 324000, 288000, 228000, 408000 } },
	{ {  408000, 324000, 288000, 228000, 408000 } },
};

static struct balanced_throttle tj_throttle = {
	.throt_tab_size = ARRAY_SIZE(tj_throttle_table),
	.throt_tab = tj_throttle_table,
};

static struct throttle_table tj_hard_throttle_table[] = {
	{ {  204000,  420000,  360000,  208000,  204000 } },
};

static struct balanced_throttle tj_hard_throttle = {
	.throt_tab_size = ARRAY_SIZE(tj_hard_throttle_table),
	.throt_tab = tj_hard_throttle_table,
};

static int __init loki_throttle_init(void)
{
	balanced_throttle_register(&tj_throttle, "tegra-balanced");
	balanced_throttle_register(&tj_hard_throttle, "tegra-hard");
	return 0;
}
module_init(loki_throttle_init);

static struct nct1008_platform_data loki_nct72_pdata = {
	.supported_hwrev = true,
	.ext_range = true,
	.conv_rate = 0x06, /* 4Hz conversion rate */
	.offset = 0,
	.shutdown_ext_limit = 91, /* C */
	.shutdown_local_limit = 120, /* C */

	.passive_delay = 2000,

	.num_trips = 2,
	.trips = {
		/* Thermal Throttling */
		[0] = {
			.cdev_type = "tegra-balanced",
			.trip_temp = 80000,
			.trip_type = THERMAL_TRIP_PASSIVE,
			.upper = THERMAL_NO_LIMIT,
			.lower = THERMAL_NO_LIMIT,
			.hysteresis = 0,
		},
		[1] = {
			.cdev_type = "tegra-hard",
			.trip_temp = 86000, /* shutdown_ext_limit - 2C */
			.trip_type = THERMAL_TRIP_PASSIVE,
			.upper = 1,
			.lower = 1,
			.hysteresis = 6000,
		},
		[2] = {
			.cdev_type = "suspend_soctherm",
			.trip_temp = 50000,
			.trip_type = THERMAL_TRIP_ACTIVE,
			.upper = 1,
			.lower = 1,
			.hysteresis = 5000,
		},
	},
};

static struct i2c_board_info loki_i2c_nct72_board_info[] = {
	{
		I2C_BOARD_INFO("nct72", 0x4c),
		.platform_data = &loki_nct72_pdata,
		.irq = -1,
	}
};

static int loki_nct72_init(void)
{
	int nct72_port = TEGRA_GPIO_PI6;
	int ret = 0;

/*
	tegra_add_cdev_trips(loki_nct72_pdata.trips,
				&loki_nct72_pdata.num_trips);
*/
/*
	tegra_platform_edp_init(loki_nct72_pdata.trips,
				&loki_nct72_pdata.num_trips,
				0);
*/
	 /* edp temperature margin */
	tegra_add_cdev_trips(loki_nct72_pdata.trips,
				&loki_nct72_pdata.num_trips);
	tegra_add_tj_trips(loki_nct72_pdata.trips,
				&loki_nct72_pdata.num_trips);
	loki_i2c_nct72_board_info[0].irq = gpio_to_irq(nct72_port);

	ret = gpio_request(nct72_port, "temp_alert");
	if (ret < 0)
		return ret;

	ret = gpio_direction_input(nct72_port);
	if (ret < 0) {
		pr_info("%s: calling gpio_free(nct72_port)", __func__);
		gpio_free(nct72_port);
	}

	/* loki has thermal sensor on GEN2-I2C i.e. instance 1 */
	i2c_register_board_info(0, loki_i2c_nct72_board_info,
		ARRAY_SIZE(loki_i2c_nct72_board_info));

	return ret;
}

int __init loki_sensors_init(void)
{
	mpuirq_init();
	loki_camera_init();
	loki_nct72_init();

	i2c_register_board_info(0, loki_i2c_board_info_cm32181,
			ARRAY_SIZE(loki_i2c_board_info_cm32181));

	return 0;
}
