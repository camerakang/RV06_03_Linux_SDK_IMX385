// SPDX-License-Identifier: GPL-2.0
/*
 * sc130gs driver
 *
 * Copyright (C) 2017 Fuzhou Rockchip Electronics Co., Ltd.
 * V0.1.0: MIPI is ok.
 * V0.0X01.0X02 fix mclk issue when probe multiple camera.
 * V0.0X01.0X03 add enum_frame_interval function.
 * V0.0X01.0X04 add quick stream on/off
 * V0.0X01.0X05 add function g_mbus_config
 * V0.0X01.0X06 add function reset gpio control
 * V0.0X01.0X07 adapt SC130GS 1lane MIPI 10bit 1280x1024
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <linux/pinctrl/consumer.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x07)
#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

/* SC130GS: 2lane MIPI 10bit, board onboard 27MHz crystal
 * HTS=750(0x02ee), VTS=532(0x0214), 1280x1024 @30fps
 * MIPI link freq = 120MHz (datarate 240Mbps/lane, 2lane)
 */
#define MIPI_FREQ_120M			120000000

#define PIXEL_RATE_WITH_120M		(MIPI_FREQ_120M * 2 / 10 * 2)

/* SC130GS: 4lane MIPI 10bit, board onboard 27MHz crystal
 * HTS=780(0x030c), VTS=527(0x020f), 1280x1024 @240fps
 * MIPI link freq = 495MHz (datarate 990Mbps/lane, 4lane)
 */
#define MIPI_FREQ_495M			495000000

#define PIXEL_RATE_WITH_495M		(MIPI_FREQ_495M * 2 / 10 * 4)

#define SC130GS_XVCLK_FREQ		27000000

#define CHIP_ID				0x0130
#define SC130GS_REG_CHIP_ID		0x3107

#define SC130GS_REG_CTRL_MODE		0x0100
#define SC130GS_MODE_SW_STANDBY		0x0
#define SC130GS_MODE_STREAMING		BIT(0)

#define SC130GS_REG_EXPOSURE		0x3e01
#define	SC130GS_EXPOSURE_MIN		6
#define	SC130GS_EXPOSURE_STEP		1
#define SC130GS_VTS_MAX			0xffff

#define SC130GS_REG_COARSE_AGAIN	0x3e08
#define SC130GS_REG_FINE_AGAIN		0x3e09
#define	ANALOG_GAIN_MIN			0x20
#define	ANALOG_GAIN_MAX			0x391
#define	ANALOG_GAIN_STEP		1
#define	ANALOG_GAIN_DEFAULT		0x20

#define SC130GS_REG_TEST_PATTERN	0x4501
#define	SC130GS_TEST_PATTERN_ENABLE	0xcc
#define	SC130GS_TEST_PATTERN_DISABLE	0xc4

#define SC130GS_REG_VTS			0x320e

#define REG_NULL			0xFFFF

#define SC130GS_REG_VALUE_08BIT		1
#define SC130GS_REG_VALUE_16BIT		2
#define SC130GS_REG_VALUE_24BIT		3

#define SC130GS_NAME			"sc130gs"

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

static const char * const sc130gs_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define SC130GS_NUM_SUPPLIES ARRAY_SIZE(sc130gs_supply_names)

enum {
	LINK_FREQ_120M_INDEX,
	LINK_FREQ_495M_INDEX,
};

struct regval {
	u16 addr;
	u8 val;
};

struct sc130gs_mode {
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	u32 link_freq_index;
	u64 pixel_rate;
	const struct regval *reg_list;
	u32 lanes;
	u32 bus_fmt;
};

struct sc130gs {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[SC130GS_NUM_SUPPLIES];
	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;
	struct pinctrl_state	*pins_sleep;
	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*test_pattern;
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;
	struct mutex		mutex;
	struct v4l2_fract	cur_fps;
	u32			cur_vts;
	bool			streaming;
	bool			power_on;
	const struct sc130gs_mode *cur_mode;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
};

#define to_sc130gs(sd) container_of(sd, struct sc130gs, subdev)

/*
 * Onboard 27MHz crystal
 * 1280x1024, MIPI 2lane 10bit, 240Mbps/lane
 * HTS=750(0x02ee), VTS=532(0x0214), 30fps
 */
static const struct regval sc130gs_2lane_10bit_regs[] = {
	{0x0100, 0x00},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3018, 0x30},
	{0x3019, 0x00},
	{0x3022, 0x10},
	{0x302b, 0x80},
	{0x3030, 0x04},
	{0x3031, 0x0a},
	{0x3034, 0x0d},
	{0x3035, 0x2a},
	{0x3038, 0x44},
	{0x3039, 0x24},
	{0x303a, 0x36},
	{0x303b, 0x06},
	{0x303c, 0x04},
	{0x303f, 0x11},
	{0x3202, 0x00},
	{0x3203, 0x00},
	{0x3205, 0x8b},
	{0x3206, 0x02},
	{0x3207, 0x04},
	{0x320a, 0x04},
	{0x320b, 0x00},
	{0x320c, 0x02},
	{0x320d, 0xee},
	{0x320e, 0x02},
	{0x320f, 0x14},
	{0x3211, 0x0c},
	{0x3213, 0x04},
	{0x3300, 0x20},
	{0x3302, 0x0c},
	{0x3306, 0x28},
	{0x3308, 0x50},
	{0x330a, 0x00},
	{0x330b, 0x40},
	{0x330e, 0x1a},
	{0x3310, 0xf0},
	{0x3311, 0x10},
	{0x3319, 0xe8},
	{0x3333, 0x90},
	{0x3334, 0x30},
	{0x3348, 0x02},
	{0x3349, 0xee},
	{0x334a, 0x02},
	{0x334b, 0xe8},
	{0x335d, 0x00},
	{0x3380, 0xff},
	{0x3382, 0xe0},
	{0x3383, 0x0a},
	{0x3384, 0xe4},
	{0x3400, 0x53},
	{0x3416, 0x31},
	{0x3518, 0x07},
	{0x3519, 0xc8},
	{0x3620, 0x23},
	{0x3621, 0x0a},
	{0x3622, 0x06},
	{0x3623, 0x14},
	{0x3624, 0x40},
	{0x3625, 0x00},
	{0x3626, 0x00},
	{0x3627, 0x01},
	{0x3630, 0x63},
	{0x3632, 0x74},
	{0x3633, 0x63},
	{0x3634, 0xff},
	{0x3635, 0x44},
	{0x3638, 0x82},
	{0x3639, 0x74},
	{0x363a, 0x24},
	{0x363b, 0x00},
	{0x3640, 0x02},
	{0x3663, 0x88},
	{0x3664, 0x07},
	{0x3c00, 0x41},
	{0x3d08, 0x00},
	{0x3e01, 0x1a},
	{0x3e02, 0x00},
	{0x3e03, 0x0b},
	{0x3e08, 0x03},
	{0x3e09, 0x20},
	{0x3e0e, 0x00},
	{0x3e0f, 0x14},
	{0x3e14, 0xb0},
	{0x3f08, 0x04},
	{0x4501, 0xc0},
	{0x4502, 0x16},
	{0x4837, 0x43},
	{0x5000, 0x01},
	{0x5b00, 0x02},
	{0x5b01, 0x03},
	{0x5b02, 0x01},
	{0x5b03, 0x01},
	{0x0100, 0x01},
	{REG_NULL, 0x00},
};

/*
 * 4lane MIPI 10bit 1280x1024 @240fps
 * pixel_rate = 495M*2/10*4 = 396MHz
 * hts_def = pixel_rate / (fps * vts) = 396M / (240 * 527) = 3131
 */
static const struct regval sc130gs_4lane_10bit_regs[] = {
	{0x0100, 0x00},
	{0x3039, 0x80},
	{0x3034, 0x80},
	{0x3001, 0x00},
	{0x3018, 0x70},
	{0x3019, 0x00},
	{0x301f, 0x47},
	{0x3022, 0x10},
	{0x302b, 0x80},
	{0x3030, 0x01},
	{0x3000, 0x00},
	{0x3031, 0x0a},
	{0x3035, 0xd2},
	{0x3036, 0x00},
	{0x3038, 0x4b},
	{0x303a, 0x35},
	{0x303b, 0x0e},
	{0x303c, 0x06},
	{0x303d, 0x03},
	{0x303f, 0x11},
	{0x3202, 0x00},
	{0x3203, 0x00},
	{0x3205, 0x8b},
	{0x3206, 0x02},
	{0x3207, 0x04},
	{0x320a, 0x04},
	{0x320b, 0x00},
	{0x320c, 0x03},
	{0x320d, 0x0c},
	{0x320e, 0x02},
	{0x320f, 0x0f},
	{0x3211, 0x08},
	{0x3213, 0x04},
	{0x3300, 0x20},
	{0x3302, 0x0c},
	{0x3306, 0x48},
	{0x3308, 0x50},
	{0x330a, 0x01},
	{0x330b, 0x20},
	{0x330e, 0x1a},
	{0x3310, 0xf0},
	{0x3311, 0x10},
	{0x3319, 0xe8},
	{0x3333, 0x90},
	{0x3334, 0x30},
	{0x3348, 0x02},
	{0x3349, 0xee},
	{0x334a, 0x02},
	{0x334b, 0xe0},
	{0x335d, 0x00},
	{0x3380, 0xff},
	{0x3382, 0xe0},
	{0x3383, 0x0a},
	{0x3384, 0xe4},
	{0x3400, 0x53},
	{0x3416, 0x31},
	{0x3518, 0x07},
	{0x3519, 0xc8},
	{0x3620, 0x24},
	{0x3621, 0x0a},
	{0x3622, 0x06},
	{0x3623, 0x14},
	{0x3624, 0x20},
	{0x3625, 0x00},
	{0x3626, 0x00},
	{0x3627, 0x01},
	{0x3630, 0x63},
	{0x3632, 0x74},
	{0x3633, 0x63},
	{0x3634, 0xff},
	{0x3635, 0x44},
	{0x3638, 0x82},
	{0x3639, 0x74},
	{0x363a, 0x24},
	{0x363b, 0x00},
	{0x3640, 0x03},
	{0x3658, 0x9a},
	{0x3663, 0x88},
	{0x3664, 0x06},
	{0x3c00, 0x41},
	{0x3d08, 0x00},
	{0x3e01, 0x20},
	{0x3e02, 0x50},
	{0x3e03, 0x0b},
	{0x3e08, 0x02},
	{0x3e09, 0x20},
	{0x3e0e, 0x00},
	{0x3e0f, 0x15},
	{0x3e14, 0xb0},
	{0x3f08, 0x04},
	{0x4501, 0xc0},
	{0x4502, 0x16},
	{0x5000, 0x01},
	{0x5050, 0x0c},
	{0x5b00, 0x02},
	{0x5b01, 0x03},
	{0x5b02, 0x01},
	{0x5b03, 0x01},
	{0x3039, 0x44},
	{0x3034, 0x01},
	{0x0100, 0x01},
	{REG_NULL, 0x00},
};

static const struct sc130gs_mode supported_modes[] = {
	{
		.width = 1280,
		.height = 1024,
		.max_fps = {
			.numerator = 10000,
			.denominator = 300000,
		},
		.exp_def = 0x001a,
		.hts_def = 1500,
		.vts_def = 1067,
		.link_freq_index = LINK_FREQ_120M_INDEX,
		.pixel_rate      = PIXEL_RATE_WITH_120M,
		.reg_list = sc130gs_2lane_10bit_regs,
		.lanes    = 2,
		.bus_fmt  = MEDIA_BUS_FMT_SBGGR10_1X10,
	},
	{
		.width = 1280,
		.height = 1024,
		.max_fps = {
			.numerator = 10000,
			.denominator = 2400000,
		},
		.exp_def = 0x0020,
		.hts_def = 3131,
		.vts_def = 527,
		.link_freq_index = LINK_FREQ_495M_INDEX,
		.pixel_rate      = PIXEL_RATE_WITH_495M,
		.reg_list = sc130gs_4lane_10bit_regs,
		.lanes    = 4,
		.bus_fmt  = MEDIA_BUS_FMT_SBGGR10_1X10,
	},
};

static const char * const sc130gs_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4"
};

static const s64 link_freq_menu_items[] = {
	MIPI_FREQ_120M,
	MIPI_FREQ_495M,
};

/* Write registers up to 4 at a time */
static int sc130gs_write_reg(struct i2c_client *client,
	u16 reg, u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;
	u32 ret;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	ret = i2c_master_send(client, buf, len + 2);
	if (ret != len + 2)
		return -EIO;

	return 0;
}

static int sc130gs_write_array(struct i2c_client *client,
	const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++) {
		ret = sc130gs_write_reg(client, regs[i].addr,
					SC130GS_REG_VALUE_08BIT, regs[i].val);
	}

	return ret;
}

/* Read registers up to 4 at a time */
static int sc130gs_read_reg(struct i2c_client *client,
	u16 reg, unsigned int len, u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);

	return 0;
}

static int sc130gs_get_reso_dist(const struct sc130gs_mode *mode,
	struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct sc130gs_mode *
	sc130gs_find_best_fit(struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		dist = sc130gs_get_reso_dist(&supported_modes[i], framefmt);
		if ((cur_best_fit_dist == -1 || dist < cur_best_fit_dist) &&
		    (supported_modes[i].bus_fmt == framefmt->code)) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}
	return &supported_modes[cur_best_fit];
}

static int sc130gs_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct sc130gs *sc130gs = to_sc130gs(sd);
	const struct sc130gs_mode *mode;
	s64 h_blank, vblank_def;

	mutex_lock(&sc130gs->mutex);

	mode = sc130gs_find_best_fit(fmt);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
#else
		mutex_unlock(&sc130gs->mutex);
		return -ENOTTY;
#endif
	} else {
		sc130gs->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(sc130gs->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(sc130gs->vblank, vblank_def,
					 SC130GS_VTS_MAX - mode->height,
					 1, vblank_def);
		__v4l2_ctrl_s_ctrl_int64(sc130gs->pixel_rate, mode->pixel_rate);
		__v4l2_ctrl_s_ctrl(sc130gs->link_freq, mode->link_freq_index);
		sc130gs->cur_fps = mode->max_fps;
		sc130gs->cur_vts = mode->vts_def;
	}

	mutex_unlock(&sc130gs->mutex);

	return 0;
}

static int sc130gs_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct sc130gs *sc130gs = to_sc130gs(sd);
	const struct sc130gs_mode *mode = sc130gs->cur_mode;

	mutex_lock(&sc130gs->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#else
		mutex_unlock(&sc130gs->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&sc130gs->mutex);

	return 0;
}

static int sc130gs_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct sc130gs *sc130gs = to_sc130gs(sd);

	if (code->index != 0)
		return -EINVAL;
	code->code = sc130gs->cur_mode->bus_fmt;

	return 0;
}

static int sc130gs_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != supported_modes[fse->index].bus_fmt)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int sc130gs_enable_test_pattern(struct sc130gs *sc130gs, u32 pattern)
{
	u32 val;

	if (pattern)
		val = (pattern - 1) | SC130GS_TEST_PATTERN_ENABLE;
	else
		val = SC130GS_TEST_PATTERN_DISABLE;

	return sc130gs_write_reg(sc130gs->client, SC130GS_REG_TEST_PATTERN,
				 SC130GS_REG_VALUE_08BIT, val);
}

static void sc130gs_get_module_inf(struct sc130gs *sc130gs,
				   struct rkmodule_inf *inf)
{
	memset(inf, 0, sizeof(*inf));
	strlcpy(inf->base.sensor, SC130GS_NAME, sizeof(inf->base.sensor));
	strlcpy(inf->base.module, sc130gs->module_name,
		sizeof(inf->base.module));
	strlcpy(inf->base.lens, sc130gs->len_name, sizeof(inf->base.lens));
}

static long sc130gs_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct sc130gs *sc130gs = to_sc130gs(sd);
	long ret = 0;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		sc130gs_get_module_inf(sc130gs, (struct rkmodule_inf *)arg);
		break;
	case RKMODULE_SET_QUICK_STREAM:

		stream = *((u32 *)arg);

		if (stream)
			ret = sc130gs_write_reg(sc130gs->client, SC130GS_REG_CTRL_MODE,
				SC130GS_REG_VALUE_08BIT, SC130GS_MODE_STREAMING);
		else
			ret = sc130gs_write_reg(sc130gs->client, SC130GS_REG_CTRL_MODE,
				SC130GS_REG_VALUE_08BIT, SC130GS_MODE_SW_STANDBY);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long sc130gs_compat_ioctl32(struct v4l2_subdev *sd,
				   unsigned int cmd, unsigned long arg)
{
	void __user *up = compat_ptr(arg);
	struct rkmodule_inf *inf;
	long ret = 0;
	u32 stream = 0;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		inf = kzalloc(sizeof(*inf), GFP_KERNEL);
		if (!inf) {
			ret = -ENOMEM;
			return ret;
		}

		ret = sc130gs_ioctl(sd, cmd, inf);
		if (!ret) {
			ret = copy_to_user(up, inf, sizeof(*inf));
			if (ret)
				ret = -EFAULT;
		}
		kfree(inf);
		break;
	case RKMODULE_SET_QUICK_STREAM:
		if (copy_from_user(&stream, up, sizeof(u32)))
			return -EFAULT;

		ret = sc130gs_ioctl(sd, cmd, &stream);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
#endif

static int sc130gs_set_ctrl_gain(struct sc130gs *sc130gs, u32 a_gain)
{
	int ret = 0;
	u32 coarse_again, fine_again, fine_again_reg, coarse_again_reg;

	if (a_gain < 0x20)
		a_gain = 0x20;
	if (a_gain > 0x391)
		a_gain = 0x391;

	if (a_gain < 0x3a) {/*1x~1.813*/
		fine_again = a_gain;
		coarse_again = 0x03;
		fine_again_reg = fine_again & 0x3f;
		coarse_again_reg = coarse_again & 0x3F;
		if (fine_again_reg >= 0x39)
			fine_again_reg = 0x39;
	} else if (a_gain < 0x72) {/*1.813~3.568x*/
		fine_again = (a_gain - 0x3a) * 1000 / 1755 + 0x20;
		coarse_again = 0x23;
		if (fine_again > 0x3f)
			fine_again = 0x3f;
		fine_again_reg = fine_again & 0x3f;
		coarse_again_reg = coarse_again & 0x3F;
	} else if (a_gain < 0xe8) { /*3.568x~7.250x*/
		fine_again = (a_gain - 0x72) * 1000 / 3682 + 0x20;
		coarse_again = 0x27;
		if (fine_again > 0x3f)
			fine_again = 0x3f;
		fine_again_reg = fine_again & 0x3f;
		coarse_again_reg = coarse_again & 0x3F;
	} else if (a_gain < 0x1d0) { /*7.250x~14.5x*/
		fine_again = (a_gain - 0xe8) * 100 / 725 + 0x20;
		coarse_again = 0x2f;
		if (fine_again > 0x3f)
			fine_again = 0x3f;
		fine_again_reg = fine_again & 0x3f;
		coarse_again_reg = coarse_again & 0x3F;
	} else { /*14.5x~28.547*/
		fine_again = (a_gain - 0x1d0) * 1000 / 14047 + 0x20;
		coarse_again = 0x3f;
		if (fine_again > 0x3f)
			fine_again = 0x3f;
		fine_again_reg = fine_again & 0x3f;
		coarse_again_reg = coarse_again & 0x3F;
	}
	ret |= sc130gs_write_reg(sc130gs->client,
		SC130GS_REG_COARSE_AGAIN,
		SC130GS_REG_VALUE_08BIT,
		coarse_again_reg);
	ret |= sc130gs_write_reg(sc130gs->client,
		SC130GS_REG_FINE_AGAIN,
		SC130GS_REG_VALUE_08BIT,
		fine_again_reg);
	return ret;
}

static int __sc130gs_start_stream(struct sc130gs *sc130gs)
{
	int ret;

	ret = sc130gs_write_array(sc130gs->client, sc130gs->cur_mode->reg_list);
	if (ret)
		return ret;

	/* In case these controls are set before streaming */
	mutex_unlock(&sc130gs->mutex);
	ret = v4l2_ctrl_handler_setup(&sc130gs->ctrl_handler);
	mutex_lock(&sc130gs->mutex);
	if (ret)
		return ret;

	return sc130gs_write_reg(sc130gs->client, SC130GS_REG_CTRL_MODE,
			SC130GS_REG_VALUE_08BIT, SC130GS_MODE_STREAMING);
}

static int __sc130gs_stop_stream(struct sc130gs *sc130gs)
{
	return sc130gs_write_reg(sc130gs->client, SC130GS_REG_CTRL_MODE,
			SC130GS_REG_VALUE_08BIT, SC130GS_MODE_SW_STANDBY);
}

static int sc130gs_s_stream(struct v4l2_subdev *sd, int on)
{
	struct sc130gs *sc130gs = to_sc130gs(sd);
	struct i2c_client *client = sc130gs->client;
	unsigned int fps;
	int ret = 0;

	mutex_lock(&sc130gs->mutex);
	on = !!on;
	if (on == sc130gs->streaming)
		goto unlock_and_return;

	fps = DIV_ROUND_CLOSEST(sc130gs->cur_mode->max_fps.denominator,
				sc130gs->cur_mode->max_fps.numerator);

	dev_info(&sc130gs->client->dev, "%s: on: %d, %dx%d@%d\n", __func__, on,
				sc130gs->cur_mode->width,
				sc130gs->cur_mode->height,
				fps);

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __sc130gs_start_stream(sc130gs);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		__sc130gs_stop_stream(sc130gs);
		pm_runtime_put(&client->dev);
	}

	sc130gs->streaming = on;

unlock_and_return:
	mutex_unlock(&sc130gs->mutex);

	return ret;
}

static int sc130gs_s_power(struct v4l2_subdev *sd, int on)
{
	struct sc130gs *sc130gs = to_sc130gs(sd);
	struct i2c_client *client = sc130gs->client;
	int ret = 0;

	mutex_lock(&sc130gs->mutex);

	/* If the power state is not modified - no work to do. */
	if (sc130gs->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}
		sc130gs->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		sc130gs->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&sc130gs->mutex);

	return ret;
}

static int sc130gs_g_frame_interval(struct v4l2_subdev *sd,
				    struct v4l2_subdev_frame_interval *fi)
{
	struct sc130gs *sc130gs = to_sc130gs(sd);
	const struct sc130gs_mode *mode = sc130gs->cur_mode;

	if (sc130gs->streaming)
		fi->interval = sc130gs->cur_fps;
	else
		fi->interval = mode->max_fps;

	return 0;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 sc130gs_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, SC130GS_XVCLK_FREQ / 1000 / 1000);
}

static int __sc130gs_power_on(struct sc130gs *sc130gs)
{
	int ret;
	u32 delay_us;
	struct device *dev = &sc130gs->client->dev;

	if (!IS_ERR_OR_NULL(sc130gs->pins_default)) {
		ret = pinctrl_select_state(sc130gs->pinctrl,
					   sc130gs->pins_default);
		if (ret < 0)
			dev_err(dev, "could not set pins\n");
	}

	ret = clk_set_rate(sc130gs->xvclk, SC130GS_XVCLK_FREQ);
	if (ret < 0)
		dev_warn(dev, "Failed to set xvclk rate (27MHz)\n");
	if (clk_get_rate(sc130gs->xvclk) != SC130GS_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched, modes are based on 27MHz\n");
	ret = clk_prepare_enable(sc130gs->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}

	ret = regulator_bulk_enable(SC130GS_NUM_SUPPLIES, sc130gs->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	if (!IS_ERR(sc130gs->reset_gpio))
		gpiod_set_value_cansleep(sc130gs->reset_gpio, 0); /* assert reset (LOW) */

	usleep_range(1000, 2000);

	if (!IS_ERR(sc130gs->pwdn_gpio))
		gpiod_set_value_cansleep(sc130gs->pwdn_gpio, 1); /* power on (HIGH) */

	usleep_range(500, 1000);

	if (!IS_ERR(sc130gs->reset_gpio))
		gpiod_set_value_cansleep(sc130gs->reset_gpio, 1); /* release reset (HIGH) */

	/* 8192 cycles prior to first SCCB transaction */
	delay_us = sc130gs_cal_delay(8192);
	usleep_range(delay_us, delay_us * 2);

	return 0;

disable_clk:
	clk_disable_unprepare(sc130gs->xvclk);

	return ret;
}

static void __sc130gs_power_off(struct sc130gs *sc130gs)
{
	int ret;

	if (!IS_ERR(sc130gs->reset_gpio))
		gpiod_set_value_cansleep(sc130gs->reset_gpio, 0); /* assert reset (LOW) */

	if (!IS_ERR(sc130gs->pwdn_gpio))
		gpiod_set_value_cansleep(sc130gs->pwdn_gpio, 0); /* power off (LOW) */
	clk_disable_unprepare(sc130gs->xvclk);
	if (!IS_ERR_OR_NULL(sc130gs->pins_sleep)) {
		ret = pinctrl_select_state(sc130gs->pinctrl,
					   sc130gs->pins_sleep);
		if (ret < 0)
			dev_dbg(&sc130gs->client->dev, "could not set pins\n");
	}
	regulator_bulk_disable(SC130GS_NUM_SUPPLIES, sc130gs->supplies);
}

static int sc130gs_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sc130gs *sc130gs = to_sc130gs(sd);

	return __sc130gs_power_on(sc130gs);
}

static int sc130gs_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sc130gs *sc130gs = to_sc130gs(sd);

	__sc130gs_power_off(sc130gs);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int sc130gs_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct sc130gs *sc130gs = to_sc130gs(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
	const struct sc130gs_mode *def_mode = &supported_modes[0];

	mutex_lock(&sc130gs->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = def_mode->bus_fmt;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&sc130gs->mutex);
	/* No crop or compose */

	return 0;
}
#endif

static int sc130gs_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_pad_config *cfg,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	fie->code = supported_modes[fie->index].bus_fmt;
	fie->width = supported_modes[fie->index].width;
	fie->height = supported_modes[fie->index].height;
	fie->interval = supported_modes[fie->index].max_fps;
	return 0;
}

static int sc130gs_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	u32 val = 0;
	struct sc130gs *sc130gs = to_sc130gs(sd);

	val = 1 << (sc130gs->cur_mode->lanes - 1) |
	      V4L2_MBUS_CSI2_CHANNEL_0 |
	      V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->flags = val;

	return 0;
}

static const struct dev_pm_ops sc130gs_pm_ops = {
	SET_RUNTIME_PM_OPS(sc130gs_runtime_suspend,
			   sc130gs_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops sc130gs_internal_ops = {
	.open = sc130gs_open,
};
#endif

static const struct v4l2_subdev_core_ops sc130gs_core_ops = {
	.s_power = sc130gs_s_power,
	.ioctl = sc130gs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = sc130gs_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops sc130gs_video_ops = {
	.s_stream = sc130gs_s_stream,
	.g_frame_interval = sc130gs_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops sc130gs_pad_ops = {
	.enum_mbus_code = sc130gs_enum_mbus_code,
	.enum_frame_size = sc130gs_enum_frame_sizes,
	.enum_frame_interval = sc130gs_enum_frame_interval,
	.get_fmt = sc130gs_get_fmt,
	.set_fmt = sc130gs_set_fmt,
	.get_mbus_config = sc130gs_g_mbus_config,
};

static const struct v4l2_subdev_ops sc130gs_subdev_ops = {
	.core	= &sc130gs_core_ops,
	.video	= &sc130gs_video_ops,
	.pad	= &sc130gs_pad_ops,
};

static void sc130gs_modify_fps_info(struct sc130gs *sc130gs)
{
	const struct sc130gs_mode *mode = sc130gs->cur_mode;

	sc130gs->cur_fps.denominator = mode->max_fps.denominator * mode->vts_def /
				       sc130gs->cur_vts;
}

static int sc130gs_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sc130gs *sc130gs = container_of(ctrl->handler,
					       struct sc130gs, ctrl_handler);
	struct i2c_client *client = sc130gs->client;
	s64 max;
	int ret = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = sc130gs->cur_mode->height + ctrl->val - 6;
		__v4l2_ctrl_modify_range(sc130gs->exposure,
					 sc130gs->exposure->minimum, max,
					 sc130gs->exposure->step,
					 sc130gs->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		/* 4 least significant bits of expsoure are fractional part */
		ret = sc130gs_write_reg(sc130gs->client, SC130GS_REG_EXPOSURE,
			SC130GS_REG_VALUE_16BIT, ctrl->val << 4);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = sc130gs_set_ctrl_gain(sc130gs, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = sc130gs_write_reg(sc130gs->client, SC130GS_REG_VTS,
					SC130GS_REG_VALUE_16BIT,
					ctrl->val + sc130gs->cur_mode->height);
		if (!ret)
			sc130gs->cur_vts = ctrl->val + sc130gs->cur_mode->height;
		sc130gs_modify_fps_info(sc130gs);
		break;
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = sc130gs_enable_test_pattern(sc130gs, ctrl->val);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops sc130gs_ctrl_ops = {
	.s_ctrl = sc130gs_set_ctrl,
};

static int sc130gs_initialize_controls(struct sc130gs *sc130gs)
{
	const struct sc130gs_mode *mode;
	struct v4l2_ctrl_handler *handler;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;

	handler = &sc130gs->ctrl_handler;
	mode = sc130gs->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &sc130gs->mutex;

	sc130gs->link_freq = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
						    ARRAY_SIZE(link_freq_menu_items) - 1, 0,
						    link_freq_menu_items);

	sc130gs->pixel_rate = v4l2_ctrl_new_std(handler, NULL,
						V4L2_CID_PIXEL_RATE,
						0, PIXEL_RATE_WITH_120M,
						1, mode->pixel_rate);

	__v4l2_ctrl_s_ctrl(sc130gs->link_freq, mode->link_freq_index);

	h_blank = mode->hts_def - mode->width;
	sc130gs->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (sc130gs->hblank)
		sc130gs->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	sc130gs->vblank = v4l2_ctrl_new_std(handler, &sc130gs_ctrl_ops,
				V4L2_CID_VBLANK, vblank_def,
				SC130GS_VTS_MAX - mode->height,
				1, vblank_def);

	exposure_max = mode->vts_def - 6;
	sc130gs->exposure = v4l2_ctrl_new_std(handler, &sc130gs_ctrl_ops,
				V4L2_CID_EXPOSURE, SC130GS_EXPOSURE_MIN,
				exposure_max, SC130GS_EXPOSURE_STEP,
				mode->exp_def);

	sc130gs->anal_gain = v4l2_ctrl_new_std(handler, &sc130gs_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, ANALOG_GAIN_MIN,
				ANALOG_GAIN_MAX, ANALOG_GAIN_STEP,
				ANALOG_GAIN_DEFAULT);

	sc130gs->test_pattern = v4l2_ctrl_new_std_menu_items(handler,
				&sc130gs_ctrl_ops, V4L2_CID_TEST_PATTERN,
				ARRAY_SIZE(sc130gs_test_pattern_menu) - 1,
				0, 0, sc130gs_test_pattern_menu);

	if (handler->error) {
		ret = handler->error;
		dev_err(&sc130gs->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}
	sc130gs->cur_fps = mode->max_fps;
	sc130gs->cur_vts = mode->vts_def;
	sc130gs->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int sc130gs_check_sensor_id(struct sc130gs *sc130gs,
				  struct i2c_client *client)
{
	struct device *dev = &sc130gs->client->dev;
	u32 id = 0;
	int ret;

	ret = sc130gs_read_reg(client, SC130GS_REG_CHIP_ID,
			      SC130GS_REG_VALUE_16BIT, &id);
	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%04x), ret(%d)\n", id, ret);
		return -ENODEV;
	}

	dev_info(dev, "Detected SC130GS CHIP ID = 0x%04x sensor\n", CHIP_ID);

	return 0;
}

static int sc130gs_configure_regulators(struct sc130gs *sc130gs)
{
	unsigned int i;

	for (i = 0; i < SC130GS_NUM_SUPPLIES; i++)
		sc130gs->supplies[i].supply = sc130gs_supply_names[i];

	return devm_regulator_bulk_get(&sc130gs->client->dev,
				       SC130GS_NUM_SUPPLIES,
				       sc130gs->supplies);
}

static int sc130gs_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct sc130gs *sc130gs;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	sc130gs = devm_kzalloc(dev, sizeof(*sc130gs), GFP_KERNEL);
	if (!sc130gs)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				   &sc130gs->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
				       &sc130gs->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
				       &sc130gs->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
				       &sc130gs->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}
	sc130gs->client = client;
	sc130gs->cur_mode = &supported_modes[0];

	sc130gs->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(sc130gs->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	sc130gs->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sc130gs->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	sc130gs->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(sc130gs->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");
	ret = sc130gs_configure_regulators(sc130gs);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	sc130gs->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(sc130gs->pinctrl)) {
		sc130gs->pins_default =
			pinctrl_lookup_state(sc130gs->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(sc130gs->pins_default))
			dev_err(dev, "could not get default pinstate\n");

		sc130gs->pins_sleep =
			pinctrl_lookup_state(sc130gs->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(sc130gs->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	}
	mutex_init(&sc130gs->mutex);

	sd = &sc130gs->subdev;
	v4l2_i2c_subdev_init(sd, client, &sc130gs_subdev_ops);
	ret = sc130gs_initialize_controls(sc130gs);
	if (ret)
		goto err_destroy_mutex;

	ret = __sc130gs_power_on(sc130gs);
	if (ret)
		goto err_free_handler;

	ret = sc130gs_check_sensor_id(sc130gs, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &sc130gs_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
		     V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	sc130gs->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &sc130gs->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(sc130gs->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 sc130gs->module_index, facing,
		 SC130GS_NAME, dev_name(sd->dev));
	ret = v4l2_async_register_subdev_sensor_common(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_clean_entity:
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
err_power_off:
	__sc130gs_power_off(sc130gs);
err_free_handler:
	v4l2_ctrl_handler_free(&sc130gs->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&sc130gs->mutex);

	return ret;
}

static int sc130gs_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sc130gs *sc130gs = to_sc130gs(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&sc130gs->ctrl_handler);
	mutex_destroy(&sc130gs->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__sc130gs_power_off(sc130gs);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id sc130gs_of_match[] = {
	{ .compatible = "smartsens,sc130gs" },
	{},
};
MODULE_DEVICE_TABLE(of, sc130gs_of_match);
#endif

static const struct i2c_device_id sc130gs_match_id[] = {
	{ "smartsens,sc130gs", 0 },
	{ },
};

static struct i2c_driver sc130gs_i2c_driver = {
	.driver = {
		.name = SC130GS_NAME,
		.pm = &sc130gs_pm_ops,
		.of_match_table = of_match_ptr(sc130gs_of_match),
	},
	.probe		= &sc130gs_probe,
	.remove		= &sc130gs_remove,
	.id_table	= sc130gs_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&sc130gs_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&sc130gs_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("Smartsens sc130gs sensor driver");
MODULE_LICENSE("GPL v2");
