/*
 * arch/arm/mach-tegra/board-evitareul-camera.c
 *
 * Copyright (c) 2011, NVIDIA CORPORATION, All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of NVIDIA CORPORATION nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <mach/gpio.h>
#include <media/ov8838.h>
#include <media/s5k6a1gx03.h>
#include <media/ad5823.h>
#include <mach/board_htc.h>
#include "cpu-tegra.h"
#include "gpio-names.h"
#include "board-evitareul.h"
#include "board.h"

#include <linux/bma250.h> //get g-sensor info
#include <linux/tps61310_flashlight.h> //set flash light brightness

#include <media/rawchip/Yushan_Platform_Specific.h>
#include <media/rawchip/Yushan_HTC_Functions.h>

static struct regulator *cam_vcm2v85_en = NULL;
static struct regulator *cam_a2v85_en = NULL;

typedef enum
{
	OV8838 = 0,
} camera_module_t;

static const char* camera_module_name[] =
{
	[OV8838] = "ov8838",
};

static camera_module_t cam_src;

static inline void ENR_msleep(u32 t)
{
	/*
	 If timer value is between ( 10us - 20ms),
	 ENR_usleep_range() is recommended.
	 Please read Documentation/timers/timers-howto.txt.
	 */
	usleep_range(t * 1000, t * 1000 + 500);
}

static inline void ENR_usleep(u32 t)
{
	usleep_range(t, t + 500);
}

static int evitareul_power_state = 0;
static int evitareul_get_power(void)
{
	return evitareul_power_state;
}

static int enrc2u_set_regulator(void)
{
	int ret = 0;

	/* vcm */
	if (!cam_vcm2v85_en) {
		cam_vcm2v85_en = regulator_get(NULL, "v_cam_vcm2v85");
		if (IS_ERR_OR_NULL(cam_vcm2v85_en))
		{
			ret = PTR_ERR(cam_vcm2v85_en);
			pr_err("[CAM] couldn't get regulator v_cam_vcm2v85\n");
			cam_vcm2v85_en = NULL;
			return ret;
		}
	}

	/* analog */
	if (!cam_a2v85_en) {
		cam_a2v85_en = regulator_get(NULL, "v_cam_a2v85");
		if (IS_ERR_OR_NULL(cam_a2v85_en))
		{
			ret = PTR_ERR(cam_a2v85_en);
			pr_err("[CAM] couldn't get regulator v_cam_a2v85\n");
			cam_a2v85_en = NULL;
			return ret;
		}
    }

	return ret;
}

static int evitareul_main_power_on(void)
{
	int ret;

	pr_info("[CAM] %s ++: %s", __func__, camera_module_name[cam_src]);

	if (evitareul_power_state) {
		pr_info("[CAM] %s already power on, return 0", __func__);
		return 0;
	}

	enrc2u_set_regulator();

	gpio_direction_output(CAM_MCLK, 0);

	tegra_gpio_disable(CAM_I2C_SCL);
	tegra_gpio_disable(CAM_I2C_SDA);

	tegra_gpio_disable(MCAM_SPI_CLK);
	tegra_gpio_disable(MCAM_SPI_DO);
	tegra_gpio_disable(MCAM_SPI_DI);
	tegra_gpio_disable(MCAM_SPI_CS0);
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_ROW0, TEGRA_PUPD_NORMAL);
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_CLK3_REQ, TEGRA_PUPD_NORMAL);

	gpio_direction_output(CAM1_PWDN, 0);

	gpio_direction_output(FRONT_CAM_RST, 0);

	gpio_direction_output(CAM1_VCM_PD, 0);

#if defined(CONFIG_RAWCHIP_ENABLE)
	/* RAW_RSTN */
	ret = gpio_direction_output(RAW_RSTN, 0);
#endif

	/* CAM SEL */
	gpio_direction_output(CAM_SEL, 0);

#if defined(CONFIG_RAWCHIP_ENABLE)
	/* RAW_1V8_EN */
	gpio_direction_output(RAW_1V8_EN, 1);
	ENR_usleep(200);
#endif

	/* VCM */
	ret = regulator_enable(cam_vcm2v85_en);
	if (ret < 0)
	{
		pr_err("[CAM] couldn't enable regulator v_cam1_vcm_2v85\n");
		regulator_put(cam_vcm2v85_en);
		cam_vcm2v85_en = NULL;
		return ret;
	}

	/* main/front cam analog*/
	ret = regulator_enable(cam_a2v85_en);
	if (ret < 0)
	{
		pr_err("[CAM] couldn't enable regulator cam_a2v85_en\n");
		regulator_put(cam_a2v85_en);
		cam_a2v85_en = NULL;
		return ret;
	}
	ENR_usleep(200);

	/* core */
	gpio_direction_output(CAM_D1V2_EN, 1);
	ENR_usleep(200);

	/* IO */
	gpio_direction_output(CAMIO_1V8_EN, 1);
	ENR_usleep(200);
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_ROW6, TEGRA_PUPD_NORMAL);

	tegra_gpio_disable(CAM_MCLK);
	ENR_msleep(3);

#if defined(CONFIG_RAWCHIP_ENABLE)
	/* RAW_RSTN */
	ret = gpio_direction_output(RAW_RSTN, 1);
	ENR_msleep(1);

	pr_info("[CAM] %s, rawchip power on, send SPI command", __func__);
	rawchip_spi_clock_control(1);
	/* SPI send command to configure RAWCHIP here! */
	yushan_spi_write(0x0008, 0x7f);
	pr_info("[CAM] %s, rawchip power on done", __func__);
	ENR_msleep(1);
#endif

	/* XSHUTDOWM */
	gpio_direction_output(CAM1_PWDN, 1);
	ENR_msleep(1);

	gpio_direction_output(CAM1_VCM_PD, 1);

	evitareul_power_state = 1;
	pr_info("[CAM] %s --", __func__);
	return 0;
}

static int evitareul_main_power_off(void)
{
	pr_info("[CAM] %s ++: %s", __func__, camera_module_name[cam_src]);

	if (!evitareul_power_state) {
		pr_info("[CAM] %s already power off, return 0", __func__);
		return 0;
	}
	evitareul_power_state = 0;

	/* VCM PD*/
	gpio_direction_output(CAM1_VCM_PD, 0);
	ENR_msleep(1);

	/* XSHUTDOWN */
	gpio_direction_output(CAM1_PWDN, 0);
	ENR_msleep(1);

	tegra_gpio_enable(MCAM_SPI_CLK);
	ENR_msleep(1);

	tegra_gpio_enable(MCAM_SPI_CS0);
	tegra_gpio_enable(MCAM_SPI_DI);
	tegra_gpio_enable(MCAM_SPI_DO);
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_ROW0, TEGRA_PUPD_PULL_DOWN);
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_CLK3_REQ, TEGRA_PUPD_PULL_DOWN);
	ENR_msleep(1);

#if defined(CONFIG_RAWCHIP_ENABLE)
	/* RAW RSTN*/
	gpio_direction_output(RAW_RSTN, 0);
	ENR_msleep(3);
#endif

	tegra_gpio_enable(CAM_MCLK);
	ENR_msleep(1);

	/* VCM */
	regulator_disable(cam_vcm2v85_en);
	ENR_msleep(1);

	/* digital */
	gpio_direction_output(CAM_D1V2_EN, 0);
	ENR_msleep(1);

#if defined(CONFIG_RAWCHIP_ENABLE)
	/* RAW_1V8_EN */
	gpio_direction_output(RAW_1V8_EN, 0);
	ENR_msleep(1);
	rawchip_spi_clock_control(0);
#endif

	/* analog */
	regulator_disable(cam_a2v85_en);
	ENR_msleep(5);

	/* IO */
	tegra_gpio_enable(CAM_I2C_SCL);
	tegra_gpio_enable(CAM_I2C_SDA);
	gpio_direction_output(CAMIO_1V8_EN, 0);
	ENR_msleep(10);
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_ROW6, TEGRA_PUPD_PULL_DOWN);

	pr_info("[CAM] %s --", __func__);
	return 0;
}

struct ov8838_platform_data evitareul_ov8838_data =
{
	.sensor_name = "ov8838",
	.data_lane = DATA_LANE_4,
	.get_power_state = evitareul_get_power,
	.power_on = evitareul_main_power_on,
	.power_off = evitareul_main_power_off,
	.rawchip_need_powercycle = 1,
	.mirror_flip = 3,
	.use_rawchip = RAWCHIP_ENABLE,
};

struct ad5823_platform_data evitareul_ad5823_data =
{
	.focal_length = 3.03f,
	.fnumber = 2.0f,
	.pos_low = 96,
	.pos_high = 496,
	.settle_time = 55,
	.get_power_state = evitareul_get_power,
	.set_gsensor_mode = GSensor_set_mode,
	.get_gsensor_data = GSensorReadData,
	.set_flashlight = tps61310_flashlight_control,
};

static int evitareul_s5k6a1gx03_power_on(void)
{
	int ret;
	int pcbid = htc_get_pcbid_info();
	pr_info("[CAM] %s ++", __func__);

	enrc2u_set_regulator();

	gpio_direction_output(CAM_MCLK, 0);

#if defined(CONFIG_RAWCHIP_ENABLE)
	if ((pcbid >= PROJECT_PHASE_XB) || (pcbid == PROJECT_PHASE_EVM))
	{
		tegra_gpio_disable(MCAM_SPI_CLK);
		tegra_gpio_disable(MCAM_SPI_DO);
		tegra_gpio_disable(MCAM_SPI_DI);
		tegra_gpio_disable(MCAM_SPI_CS0);
		tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_ROW0, TEGRA_PUPD_NORMAL);
		tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_CLK3_REQ, TEGRA_PUPD_NORMAL);
	}
#endif

	tegra_gpio_disable(CAM_I2C_SCL);
	tegra_gpio_disable(CAM_I2C_SDA);

	gpio_direction_output(CAM1_PWDN, 0);

	gpio_direction_output(FRONT_CAM_RST, 0);

	gpio_direction_output(CAM1_VCM_PD, 0);

#if defined(CONFIG_RAWCHIP_ENABLE)
	if ((pcbid >= PROJECT_PHASE_XB) || (pcbid == PROJECT_PHASE_EVM))
	{
		gpio_direction_output(RAW_RSTN, 0);
	}
#endif

	gpio_direction_output(CAM_SEL, 0);

#if defined(CONFIG_RAWCHIP_ENABLE)
	if ((pcbid >= PROJECT_PHASE_XB) || (pcbid == PROJECT_PHASE_EVM))
	{
		/* RAW_1V8_EN */
		gpio_direction_output(RAW_1V8_EN, 1);
		ENR_usleep(200);
	}
#endif

	/* vcm */
	ret = regulator_enable(cam_vcm2v85_en);
	if (ret < 0)
	{
		pr_err("[CAM] couldn't enable regulator cam_vcm2v85_en\n");
		regulator_put(cam_vcm2v85_en);
		cam_vcm2v85_en = NULL;
		return ret;
	}
	ENR_usleep(200);

	/* analog */
	ret = regulator_enable(cam_a2v85_en);
	if (ret < 0)
	{
		pr_err("[CAM] couldn't enable regulator cam_a2v85_en\n");
		regulator_put(cam_a2v85_en);
		cam_a2v85_en = NULL;
		return ret;
	}
	ENR_usleep(200);

#if defined(CONFIG_RAWCHIP_ENABLE)
	if ((pcbid >= PROJECT_PHASE_XB) || (pcbid == PROJECT_PHASE_EVM))
	{
		/* main camera core */
		gpio_direction_output(CAM_D1V2_EN, 1);
		ENR_usleep(200);
	}
#endif

	/* IO */
	gpio_direction_output(CAMIO_1V8_EN, 1);
	ENR_usleep(200);
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_ROW6, TEGRA_PUPD_NORMAL);

	/* RSTN */
	gpio_direction_output(FRONT_CAM_RST, 1);

	/* digital */
	gpio_direction_output(CAM2_D1V2_EN, 1);
	ENR_usleep(200);

	/* CAM SEL */
	gpio_direction_output(CAM_SEL, 1);
	ENR_msleep(1);

	tegra_gpio_disable(CAM_MCLK);
	ENR_msleep(1);

#if defined(CONFIG_RAWCHIP_ENABLE)
	if ((pcbid >= PROJECT_PHASE_XB) || (pcbid == PROJECT_PHASE_EVM))
	{
		/* RAW_RSTN */
		ret = gpio_direction_output(RAW_RSTN, 1);
		ENR_msleep(1);
		rawchip_spi_clock_control(1);
		/* SPI send command to configure RAWCHIP here! */
		yushan_spi_write(0x0008, 0x7f);
		ENR_msleep(1);
	}
#endif

	pr_info("[CAM] %s --", __func__);
	return 0;
}

static int evitareul_s5k6a1gx03_power_off(void)
{
	pr_info("[CAM] %s ++", __func__);
	int pcbid = htc_get_pcbid_info();

#if defined(CONFIG_RAWCHIP_ENABLE)
	if ((pcbid >= PROJECT_PHASE_XB) || (pcbid == PROJECT_PHASE_EVM))
	{
		/* set gpio output low : O(L) */
		tegra_gpio_enable(MCAM_SPI_CLK);
		ENR_msleep(1);

		tegra_gpio_enable(MCAM_SPI_DO);
		tegra_gpio_enable(MCAM_SPI_DI);
		tegra_gpio_enable(MCAM_SPI_CS0);
		tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_ROW0, TEGRA_PUPD_PULL_DOWN);
		tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_CLK3_REQ, TEGRA_PUPD_PULL_DOWN);
		ENR_msleep(1);

		/* RAW RSTN*/
		gpio_direction_output(RAW_RSTN, 0);
		ENR_msleep(3);
	}
#endif

	tegra_gpio_enable(CAM_MCLK);
	ENR_msleep(1);

	/* CAM SEL */
	gpio_direction_output(CAM_SEL, 0);
	ENR_msleep(1);

	/* vcm */
	regulator_disable(cam_vcm2v85_en);
	ENR_msleep(1);

	/* digital */
	gpio_direction_output(CAM2_D1V2_EN, 0);
	ENR_msleep(1);

	/* RSTN */
	gpio_direction_output(FRONT_CAM_RST, 0);
	ENR_msleep(1);

#if defined(CONFIG_RAWCHIP_ENABLE)
	if ((pcbid >= PROJECT_PHASE_XB) || (pcbid == PROJECT_PHASE_EVM))
	{
		/* core */
		gpio_direction_output(CAM_D1V2_EN, 0);
		ENR_msleep(1);

		/* RAW_1V8_EN */
		gpio_direction_output(RAW_1V8_EN, 0);
		ENR_msleep(1);
		rawchip_spi_clock_control(0);
	}
#endif

	/* analog */
	regulator_disable(cam_a2v85_en);
	ENR_msleep(5);

	/* IO */
	tegra_gpio_enable(CAM_I2C_SCL);
	tegra_gpio_enable(CAM_I2C_SDA);
	gpio_direction_output(CAMIO_1V8_EN, 0);
	ENR_msleep(10);
	tegra_pinmux_set_pullupdown(TEGRA_PINGROUP_KB_ROW6, TEGRA_PUPD_PULL_DOWN);

	pr_info("[CAM] %s --", __func__);
	return 0;
}

struct s5k6a1gx03_platform_data evitareul_s5k6a1gx03_data =
{
	.sensor_name = "s5k6a2gx",
	.data_lane = 1,
	.csi_if = 0,
	.power_on = evitareul_s5k6a1gx03_power_on,
	.power_off = evitareul_s5k6a1gx03_power_off,
	.mirror_flip = 0,
	.use_rawchip = RAWCHIP_DISABLE,
};

static struct i2c_board_info evitareul_i2c3_board_info_main[] =
{
	{ I2C_BOARD_INFO("ov8838", 0x10), .platform_data = &evitareul_ov8838_data, },
	{ I2C_BOARD_INFO("ad5823", 0x0C), .platform_data = &evitareul_ad5823_data, }
};

static struct i2c_board_info evitareul_i2c4_board_info[] =
{
	{ I2C_BOARD_INFO("s5k6a1gx03", 0x36), .platform_data = &evitareul_s5k6a1gx03_data, },
};

struct evitareul_cam_gpio
{
	int gpio;
	const char *label;
	int value;
};

#define TEGRA_CAMERA_GPIO(_gpio, _label, _value)	\
	{						\
		.gpio = _gpio,				\
		.label = _label,			\
		.value = _value,			\
	}

static struct evitareul_cam_gpio evitareul_cam_gpio_output_data[] =
{
	[0] = TEGRA_CAMERA_GPIO(CAM_SEL, "cam_sel", 0),
	[1] = TEGRA_CAMERA_GPIO(FRONT_CAM_RST, "front_cam_rst", 0),
	[2] = TEGRA_CAMERA_GPIO(CAM1_PWDN, "cam1_pwdn", 0),
	[3] = TEGRA_CAMERA_GPIO(CAM_D1V2_EN, "cam_d1v2_en", 0),
	[4] = TEGRA_CAMERA_GPIO(CAM2_D1V2_EN, "cam2_d1v2_en", 0),
	[5] = TEGRA_CAMERA_GPIO(CAMIO_1V8_EN, "camIO_1v8_en", 0),
	[6] = TEGRA_CAMERA_GPIO(CAM1_VCM_PD, "cam1_vcm_PD", 0),
	[7] = TEGRA_CAMERA_GPIO(CAM_I2C_SCL, "cam_i2c_SCL", 0),
	[8] = TEGRA_CAMERA_GPIO(CAM_I2C_SDA, "cam_i2c_SDA", 0),
	[9] = TEGRA_CAMERA_GPIO(CAM_MCLK, "cam_mclk", 0),
	/*for rawchip */
	[10] = TEGRA_CAMERA_GPIO(RAW_1V8_EN, "raw_1v8_en", 0),
	[11] = TEGRA_CAMERA_GPIO(RAW_RSTN, "raw_rstn", 0),
	[12] = TEGRA_CAMERA_GPIO(MCAM_SPI_CLK, "mcam_spi_clk", 0),
	[13] = TEGRA_CAMERA_GPIO(MCAM_SPI_CS0, "mcam_spi_cs0", 0),
	[14] = TEGRA_CAMERA_GPIO(MCAM_SPI_DI, "mcam_spi_di", 0),
	[15] = TEGRA_CAMERA_GPIO(MCAM_SPI_DO, "mcam_spi_do", 0),
};

static struct evitareul_cam_gpio evitareul_cam_gpio_input_data[] =
{
	[0] = TEGRA_CAMERA_GPIO(CAM1_ID, "cam1_id", 0),
	[1] = TEGRA_CAMERA_GPIO(RAW_INTR0, "raw_intr0", 0),
	[2] = TEGRA_CAMERA_GPIO(RAW_INTR1, "raw_intr1", 0),
};

int evitareul_cam_init(void)
{
	int ret;
	int i = 0, j = 0;
	pr_info("[CAM] %s ++", __func__);
	int pcbid = htc_get_pcbid_info();
	for (i = 0; i < ARRAY_SIZE(evitareul_cam_gpio_output_data); i++)
	{
		ret = gpio_request(evitareul_cam_gpio_output_data[i].gpio,
				evitareul_cam_gpio_output_data[i].label);
		if (ret < 0)
		{
			pr_err("[CAM] %s: gpio_request failed for gpio #%d\n", __func__, i);
			goto fail_free_gpio;
		}
		gpio_direction_output(evitareul_cam_gpio_output_data[i].gpio,
				evitareul_cam_gpio_output_data[i].value);
		gpio_export(evitareul_cam_gpio_output_data[i].gpio, false);
		tegra_gpio_enable(evitareul_cam_gpio_output_data[i].gpio);
	}

	for (j = 0; j < ARRAY_SIZE(evitareul_cam_gpio_input_data); j++)
	{
		ret = gpio_request(evitareul_cam_gpio_input_data[j].gpio,
				evitareul_cam_gpio_input_data[j].label);
		if (ret < 0)
		{
			pr_err("[CAM] %s: gpio_request failed for gpio #%d\n", __func__, j);
			goto fail_free_gpio;
		}
		gpio_direction_input(evitareul_cam_gpio_input_data[j].gpio);
		gpio_export(evitareul_cam_gpio_input_data[j].gpio, false);
		tegra_gpio_enable(evitareul_cam_gpio_input_data[j].gpio);
	}

	/* get camera is main or second source */
	cam_src = OV8838;
	pr_info("[CAM] main camera uses %s", camera_module_name[cam_src]);

	/* change mipi port/lane here if needed */
	if (pcbid >= PROJECT_PHASE_XB || pcbid == PROJECT_PHASE_EVM) {
		evitareul_s5k6a1gx03_data.use_rawchip = RAWCHIP_ENABLE;
	}

	if (cam_src == OV8838) {
		i2c_register_board_info(2, evitareul_i2c3_board_info_main,
				ARRAY_SIZE(evitareul_i2c3_board_info_main));
	} else {
		pr_err("[CAM] %s: sensor not found\n", __func__);
	}

	i2c_register_board_info(2, evitareul_i2c4_board_info,
			ARRAY_SIZE(evitareul_i2c4_board_info));

	pr_info("[CAM] %s --", __func__);
	return 0;

fail_free_gpio:
	pr_err("[CAM] %s --: failed!", __func__);
	while (i--)
		gpio_free(evitareul_cam_gpio_output_data[i].gpio);
	while (j--)
		gpio_free(evitareul_cam_gpio_input_data[j].gpio);
	return ret;
}
