// SPDX-License-Identifier: GPL-2.0
/*
 * imx385 driver
 *
 * Copyright (C) 2024 Rockchip Electronics Co., Ltd.
 * V0.0X01.0X00 initial version.
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
#include <linux/of_graph.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <linux/pinctrl/consumer.h>

#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x00)

#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN		V4L2_CID_GAIN
#endif

/* IMX385 时钟和数据速率配置 - 针对 37.125MHz 晶振 */
#define IMX385_XVCLK_FREQ		24000000
#define IMX385_LINK_FREQ_371M       371250000
#define IMX385_LANES			2
#define IMX385_BITS_PER_SAMPLE		10  /* 工装使用 10-bit ADC (0x3005=0x00) */

/* pixel rate = link frequency * 2 (DDR) * lanes / BITS_PER_SAMPLE */
/* 144MHz * 2 * 2 / 10 = 57.6MHz */
#define IMX385_PIXEL_RATE_LINEAR	148500000

/* IMX385 芯片 ID */
#define CHIP_ID				0x00  /* 工装实测值 */
#define IMX385_REG_CHIP_ID		0x0016  /* 工装使用的寄存器地址 */

/* IMX385 控制寄存器 */
#define IMX385_REG_CTRL_MODE		0x3000
#define IMX385_MODE_SW_STANDBY		0x01
#define IMX385_MODE_STREAMING		0x00

#define IMX385_REG_MASTER_MODE		0x3002
#define IMX385_MASTER_MODE_START	0x00
#define IMX385_MASTER_MODE_STOP		0x01

/* IMX385 曝光控制寄存器 */
#define IMX385_REG_SHS1_L		0x3020
#define IMX385_REG_SHS1_M		0x3021
#define IMX385_REG_SHS1_H		0x3022

/* IMX385 增益控制寄存器 */
#define IMX385_REG_GAIN			0x3014
#define IMX385_REG_HCG			0x3009
#define IMX385_GAIN_MIN			0x00
#define IMX385_GAIN_MAX			0xf0
#define IMX385_GAIN_STEP		1
#define IMX385_GAIN_DEFAULT		0x00

/* IMX385 垂直时序寄存器 */
#define IMX385_REG_VMAX_L		0x3018
#define IMX385_REG_VMAX_M		0x3019
#define IMX385_REG_VMAX_H		0x301a

/* IMX385 水平时序寄存器 */
#define IMX385_REG_HMAX_L		0x301c
#define IMX385_REG_HMAX_M		0x301d

/* IMX385 曝光和 VTS 限制 */
#define IMX385_EXPOSURE_MIN		2
#define IMX385_EXPOSURE_STEP		1
#define IMX385_VTS_MAX			0xffff
#define IMX385_VMAX_1080P30		0x0465  /* 1125 */

/* 寄存器操作宏 */
#define IMX385_FETCH_HIGH_BYTE(VAL)	(((VAL) >> 16) & 0xFF)
#define IMX385_FETCH_MID_BYTE(VAL)	(((VAL) >> 8) & 0xFF)
#define IMX385_FETCH_LOW_BYTE(VAL)	((VAL) & 0xFF)

#define REG_NULL			0xFFFF
#define REG_DELAY			0xFFFE

#define IMX385_REG_VALUE_08BIT		1
#define IMX385_REG_VALUE_16BIT		2
#define IMX385_REG_VALUE_24BIT		3

#define IMX385_NAME			"imx385"

#define OF_CAMERA_PINCTRL_STATE_DEFAULT	"rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP	"rockchip,camera_sleep"

/* IMX385 电源供应名称（按照上电顺序排列）*/
static const char * const imx385_supply_names[] = {
	"dvdd",		/* 数字核心电源 1.8V - 第一个上电 */
	"dovdd",	/* 数字 I/O 电源 1.8V - 第二个上电 */
	"avdd",		/* 模拟电源 3.3V - 第三个上电 */
};

#define IMX385_NUM_SUPPLIES ARRAY_SIZE(imx385_supply_names)

/**
 * @brief 寄存器值结构体
 */
struct regval {
	u16 addr;
	u8 val;
};

/**
 * @brief IMX385 模式配置结构体
 */
struct imx385_mode {
	u32 bus_fmt;
	u32 width;
	u32 height;
	struct v4l2_fract max_fps;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	const struct regval *reg_list;
	u32 hdr_mode;
};

/**
 * @brief IMX385 设备结构体
 */
struct imx385 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[IMX385_NUM_SUPPLIES];

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
	struct v4l2_ctrl	*pixel_rate;
	struct v4l2_ctrl	*link_freq;

	struct mutex		mutex;
	bool			streaming;
	bool			power_on;
	const struct imx385_mode *support_modes;
	u32			support_modes_num;
	const struct imx385_mode *cur_mode;
	u32			module_index;
	const char		*module_facing;
	const char		*module_name;
	const char		*len_name;
	u32			cur_vts;
};

#define to_imx385(sd) container_of(sd, struct imx385, subdev)

/*
 * Xclk 37.125Mhz
 */
static const struct regval imx385_global_regs[] = {
	{REG_NULL, 0x00},
};

/*
 * 工装实测配置 - 基于 I2C 抓取数据 (2024实测)
 * Xclk 37.125MHz
 * max_framerate 30fps
 * mipi_datarate per lane 742Mbps, 2 lane
 * 
 * 寄存器配置说明：
 * 1. 先进入待机模式 (0x3000=0x01)
 * 2. 配置基本参数（分辨率、帧率、接口等）
 * 3. 配置 MIPI CSI-2 参数
 * 4. 配置时钟参数
 * 5. 最后通过 s_stream 函数启动流
 * 
 * 注意：这些寄存器值是从工装I2C抓取数据中提取的实际工作配置
 */
static const struct regval imx385_linear_1080p30_regs[] = {
	/* 芯片ID读取确认 - 地址0x0016应返回0x00 */
	{0x0016, 0x03},		/* 工装实测：写入0x03用于配置 */
	
	/* 基本配置 - 按工装I2C抓取顺序 */
	{0x3000, 0x01},		/* STANDBY: 进入待机模式 */
	{0x3001, 0x00},		/* 工装配置 */
	{0x3002, 0x01},		/* XTMSTA: Master mode stop */
	{0x3005, 0x00},		/* ADBIT: 10-bit ADC */
	{0x3007, 0x00},		/* WINMODE: All-pixel scan mode (1080p) */
	{0x3009, 0x02},		/* FRSEL: 30fps 模式 */
	{0x300a, 0xF0},		/* BLKLEVEL[7:0]: 黑电平 = 0xF0 (240) */
	{0x300b, 0x00},		/* BLKLEVEL[8]: 黑电平高位 */
	{0x3012, 0x2C},		/* 工装实测值 */
	{0x3013, 0x01},		/* 工装实测值 */
	{0x3014, 0x00},		/* GAIN: 初始增益 = 0dB */
	{0x3015, 0x00},		/* 工装配置 */
	{0x3016, 0x08},		/* 工装实测值 */
	{0x3017, 0x85},		/* 工装配置 */

	/* 垂直时序：VMAX = 0x0546 (1350 行) - 工装实测 */
	{0x3018, 0x46},		/* VMAX[7:0] */
	{0x3019, 0x05},		/* VMAX[15:8] */
	{0x301a, 0x00},		/* VMAX[16] */
	{0x301b, 0x30},		/* 工装配置 */

	/* 水平时序：HMAX = 0x1130 - 工装实测 */
	{0x301c, 0x30},		/* HMAX[7:0] - 工装实测 0x30 */
	{0x301d, 0x11},		/* HMAX[15:8] - 工装实测 0x11 */

	/* 曝光配置：SHS1 = 0x0000 (初始曝光) */
	{0x3020, 0x00},		/* SHS1[7:0] */
	{0x3021, 0x00},		/* SHS1[15:8] */
	{0x3022, 0x00},		/* SHS1[19:16] */

	/* 工装额外配置 */
	{0x3036, 0x10},		/* 工装配置 */
	{0x303a, 0xD1},		/* 工装配置 */
	{0x303b, 0x03},		/* 工装配置 */
	{0x3044, 0x01},		/* 工装配置 */

	/* 输出接口配置 */
	{0x3046, 0x00},		/* ODBIT/OPORTSEL */
	{0x3047, 0x08},		/* 工装配置 */
	{0x3049, 0x00},		/* 工装配置 */
	{0x3054, 0x66},		/* 工装配置 */

	/* 时钟配置 - 工装实测值 (INCK = 37.125MHz) */
	{0x305c, 0x28},		/* INCKSEL1: 工装实测 0x28 */
	{0x305d, 0x00},		/* INCKSEL2 */
	{0x305e, 0x20},		/* INCKSEL3 */
	{0x305f, 0x00},		/* INCKSEL4 */

	/* 工装额外时钟配置 */
	{0x310b, 0x07},		/* 工装配置 */
	{0x3110, 0x12},		/* 工装配置 */
	{0x31ed, 0x38},		/* 工装配置 */

	/* MIPI 相关配置 - 工装实测 */
	{0x3338, 0xD4},		/* 工装配置 */
	{0x333b, 0x00},		/* 工装配置 */
	{0x333c, 0xD4},		/* 工装配置 */
	{0x333d, 0x40},		/* 工装配置 */
	{0x333e, 0x10},		/* 工装配置 */
	{0x333f, 0x00},		/* 工装配置 */
	{0x3443, 0x01},
	{0x3344, 0x10},		/* 工装配置 */
	{0x3346, 0x01},		/* 工装配置 */
	{0x3353, 0x0E},		/* 工装配置 */
	{0x3357, 0x49},		/* 工装配置 */
	{0x3358, 0x04},		/* 工装配置 */
	{0x336b, 0x37},		/* 工装配置 */
	{0x336c, 0x1F},		/* 工装配置 */

	/* Lane 配置 - 关键！工装实测 */
	{0x337d, 0x0A},		/* Lane 配置 - 工装实测 */
	{0x337e, 0x0A},		/* Lane 配置 - 工装实测 */
	{0x337f, 0x01},		/* 工装配置 */

	/* MIPI 时序配置 - 工装实测 */
	{0x3380, 0x20},		/* 工装配置 */
	{0x3381, 0x25},		/* 工装配置 */
	{0x3382, 0x5F},		/* 工装配置 */
	{0x3383, 0x1F},		/* 工装配置 */
	{0x3384, 0x37},		/* 工装配置 */
	{0x3385, 0x1F},		/* 工装配置 */
	{0x3386, 0x1F},		/* 工装配置 */
	{0x3387, 0x17},		/* 工装配置 */
	{0x3388, 0x67},		/* 工装配置 */
	{0x3389, 0x27},		/* 工装配置 */
	{0x338d, 0xB4},		/* 工装配置 */
	{0x338e, 0x01},		/* 工装配置 */

	/* 注意：不在这里启动流，由 s_stream 函数控制 */
	{REG_NULL, 0x00},
};

static const struct imx385_mode supported_modes[] = {
	{
		.bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,  /* 工装使用 10-bit 格式 */
		.width = 1920,
		.height = 1080,
		.max_fps = {
			.numerator = 10000,
			.denominator = 250000,  /* 30fps */
		},
		.exp_def = 0x0460,
		.hts_def = 4400,	/* HMAX = 0x1130 (工装实测) */
		.vts_def = 1350,	/* VMAX = 0x0546 = 1350 (工装实测) */
		.reg_list = imx385_linear_1080p30_regs,
		.hdr_mode = NO_HDR,
	},
};

static const s64 link_freq_menu_items[] = {
	IMX385_LINK_FREQ_371M,
};

/**
 * @brief 写入单个寄存器（最多 4 字节）
 * @param client I2C 客户端
 * @param reg 寄存器地址
 * @param len 数据长度
 * @param val 寄存器值
 * @return 0 成功，负数表示错误码
 * 
 * IMX385 使用 16 位寄存器地址，大端序
 */
static int imx385_write_reg(struct i2c_client *client, u16 reg,
			    u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;
	int ret;

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
	if (ret != len + 2) {
		dev_err(&client->dev,
			"Failed to write reg 0x%04x = 0x%02x: i2c_master_send returned %d\n",
			reg, val, ret);
		return -EIO;
	}

	dev_dbg(&client->dev, "Write reg 0x%04x = 0x%02x\n", reg, val);

	return 0;
}

/**
 * @brief 批量写入寄存器数组
 * @param client I2C 客户端
 * @param regs 寄存器数组
 * @return 0 成功，负数表示错误码
 */
static int imx385_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	dev_info(&client->dev, "开始批量写入寄存器...\n");
	
	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++) {
		if (unlikely(regs[i].addr == REG_DELAY)) {
			dev_info(&client->dev, "  延时 %d ms\n", regs[i].val);
			usleep_range(regs[i].val * 1000, regs[i].val * 2000);
		} else {
			ret = imx385_write_reg(client, regs[i].addr,
				IMX385_REG_VALUE_08BIT, regs[i].val);
			if (ret) {
				dev_err(&client->dev, "写入寄存器 0x%04x = 0x%02x 失败: %d\n",
					regs[i].addr, regs[i].val, ret);
				break;
			}
			/* 每10个寄存器打印一次进度 */
			if (i % 10 == 0)
				dev_info(&client->dev, "  已写入 %d 个寄存器...\n", i);
		}
	}

	if (ret == 0)
		dev_info(&client->dev, "批量写入完成，共 %d 个寄存器\n", i);
	else
		dev_err(&client->dev, "批量写入失败，在第 %d 个寄存器处\n", i);

	return ret;
}

/**
 * @brief 读取单个寄存器（最多 4 字节）
 * @param client I2C 客户端
 * @param reg 寄存器地址
 * @param len 数据长度
 * @param val 读取的值（输出参数）
 * @return 0 成功，负数表示错误码
 * 
 * IMX385 使用 16 位寄存器地址，大端序
 */
static int imx385_read_reg(struct i2c_client *client, u16 reg,
			   unsigned int len, u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* 写入寄存器地址 */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* 从寄存器读取数据 */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs)) {
		dev_err(&client->dev,
			"Failed to read reg 0x%04x: i2c_transfer returned %d\n",
			reg, ret);
		return -EIO;
	}

	*val = be32_to_cpu(data_be);

	dev_dbg(&client->dev, "Read reg 0x%04x = 0x%02x\n", reg, *val);

	return 0;
}

/**
 * @brief 获取寄存器列表
 * @param imx385 设备结构体指针
 * @param regs 寄存器列表（输出参数）
 * @return 0 成功，负数表示错误码
 */
static int imx385_get_reso_dist(const struct imx385_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

/**
 * @brief 查找最匹配的模式
 * @param imx385 设备结构体指针
 * @param mode 模式结构体指针
 * @param fmt 格式结构体指针
 * @return 最匹配的模式指针
 */
static const struct imx385_mode *
imx385_find_best_fit(struct imx385 *imx385, struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < imx385->support_modes_num; i++) {
		dist = imx385_get_reso_dist(&imx385->support_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &imx385->support_modes[cur_best_fit];
}

/**
 * @brief 设置曝光时间
 * @param imx385 设备结构体指针
 * @param val 曝光值（单位：行）
 * @return 0 成功，负数表示错误码
 * 
 * 曝光时间通过 SHS1 寄存器（0x3020-0x3022）设置
 * 计算公式：SHS1 = VMAX - 曝光行数 - 1
 */
static int imx385_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx385 *imx385 = container_of(ctrl->handler,
					     struct imx385, ctrl_handler);
	struct i2c_client *client = imx385->client;
	int ret = 0;
	u32 shs1, again;

	/* 如果设备未上电，延迟到上电后再设置 */
	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		/* 计算 SHS1 值 */
		shs1 = imx385->cur_vts - ctrl->val - 1;
		ret = imx385_write_reg(imx385->client,
			IMX385_REG_SHS1_L,
			IMX385_REG_VALUE_08BIT,
			IMX385_FETCH_LOW_BYTE(shs1));
		ret |= imx385_write_reg(imx385->client,
			IMX385_REG_SHS1_M,
			IMX385_REG_VALUE_08BIT,
			IMX385_FETCH_MID_BYTE(shs1));
		ret |= imx385_write_reg(imx385->client,
			IMX385_REG_SHS1_H,
			IMX385_REG_VALUE_08BIT,
			IMX385_FETCH_HIGH_BYTE(shs1));
		dev_dbg(&client->dev, "set exposure 0x%x, shs1 0x%x\n",
			ctrl->val, shs1);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		/* 设置模拟增益 */
		again = ctrl->val;
		ret = imx385_write_reg(imx385->client,
			IMX385_REG_GAIN,
			IMX385_REG_VALUE_08BIT,
			again);
		dev_dbg(&client->dev, "set analog gain 0x%x\n", again);
		break;
	case V4L2_CID_VBLANK:
		/* 设置 VMAX */
		ret = imx385_write_reg(imx385->client,
			IMX385_REG_VMAX_L,
			IMX385_REG_VALUE_08BIT,
			IMX385_FETCH_LOW_BYTE(ctrl->val));
		ret |= imx385_write_reg(imx385->client,
			IMX385_REG_VMAX_M,
			IMX385_REG_VALUE_08BIT,
			IMX385_FETCH_MID_BYTE(ctrl->val));
		ret |= imx385_write_reg(imx385->client,
			IMX385_REG_VMAX_H,
			IMX385_REG_VALUE_08BIT,
			IMX385_FETCH_HIGH_BYTE(ctrl->val));
		imx385->cur_vts = ctrl->val;
		dev_dbg(&client->dev, "set vblank 0x%x\n", ctrl->val);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx385_ctrl_ops = {
	.s_ctrl = imx385_set_ctrl,
};

/**
 * @brief 检查传感器 ID
 * @param imx385 设备结构体指针
 * @return 0 成功，负数表示错误码
 * 
 * IMX385 的芯片 ID 寄存器位于 0x31dc，值应该为 0x02
 */
static int imx385_check_sensor_id(struct imx385 *imx385,
				  struct i2c_client *client)
{
	struct device *dev = &imx385->client->dev;
	u32 id = 0;
	int ret;

	dev_info(dev, "Reading sensor ID from register 0x%04x...\n", IMX385_REG_CHIP_ID);
	
	/* 在读取前再次确认传感器已就绪 */
	dev_info(dev, "Waiting additional 50ms before first I2C read...\n");
	msleep(50);

	ret = imx385_read_reg(client, IMX385_REG_CHIP_ID,
			     IMX385_REG_VALUE_08BIT, &id);
	if (ret) {
		dev_err(dev, "Failed to read sensor id from 0x%04x, error: %d\n",
			IMX385_REG_CHIP_ID, ret);
		dev_err(dev, "Possible causes:\n");
		dev_err(dev, "  1. I2C communication failure (check bus, address 0x%02x, pull-ups)\n",
			client->addr);
		dev_err(dev, "  2. Power supply not stable (check DVDD=1.8V, DOVDD=1.8V, AVDD=3.3V)\n");
		dev_err(dev, "  3. Clock not running (check XVCLK = 24MHz)\n");
		dev_err(dev, "  4. Reset timing issue (check RESET GPIO sequence)\n");
		dev_err(dev, "  5. Hardware connection problem\n");
		return ret;
	}

	if (id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id: read 0x%02x, expected 0x%02x\n",
			id, CHIP_ID);
		dev_err(dev, "This may indicate:\n");
		dev_err(dev, "  1. Wrong sensor connected (not IMX385)\n");
		dev_err(dev, "  2. Sensor not properly initialized\n");
		dev_err(dev, "  3. I2C address mismatch (current: 0x%02x)\n", client->addr);
		return -ENODEV;
	}

	dev_info(dev, "Successfully detected IMX385 sensor, ID: 0x%02x\n", id);

	return 0;
}

/**
 * @brief 配置寄存器列表
 * @param imx385 设备结构体指针
 * @param mode 模式结构体指针
 * @return 0 成功，负数表示错误码
 */
static int imx385_configure_regulators(struct imx385 *imx385)
{
	unsigned int i;

	for (i = 0; i < IMX385_NUM_SUPPLIES; i++)
		imx385->supplies[i].supply = imx385_supply_names[i];

	return devm_regulator_bulk_get(&imx385->client->dev,
				       IMX385_NUM_SUPPLIES,
				       imx385->supplies);
}

/**
 * @brief 上电序列
 * @param imx385 设备结构体指针
 * @return 0 成功，负数表示错误码
 * 
 * IMX385 上电时序（严格按照数据手册）：
 * T0: 初始状态 - XCLR 和 PWDN 都保持低电平
 * T1: 开启电源 DVDD(1.8V) → DOVDD(1.8V) → AVDD(3.3V)
 * T2: 等待电源稳定（所有电源达到额定电压）- 至少 2ms
 * T3: 开启输入时钟 INCK（24MHz）
 * T4: 等待时钟稳定 - 至少 1ms
 * T5: 将 XCLR 拉高，传感器进入 Standby 模式
 * T6: 等待传感器初始化完成 - 至少 20ms
 * 
 * 关键规则：
 * - 在所有电源达到稳定工作电压（AVDD > 3.15V）之前，XCLR 必须保持低电平
 * - INCK 必须在 XCLR 拉高之前已经稳定开启
 * - 时序违反可能导致传感器无法响应 I2C 通信
 * 
 * GPIO 极性说明：
 * - RESET GPIO (GPIO_ACTIVE_HIGH): 0=复位, 1=正常工作
 * - PWDN GPIO (GPIO_ACTIVE_HIGH): 0=正常工作, 1=关机
 */
static int __imx385_power_on(struct imx385 *imx385)
{
	int ret;
	struct device *dev = &imx385->client->dev;

	dev_info(dev, "Starting IMX385 power-on sequence...\n");

	/* T0: 初始状态 - 确保 RESET 和 PWDN 都为低电平 */
	if (!IS_ERR(imx385->reset_gpio)) {
		gpiod_set_value_cansleep(imx385->reset_gpio, 0);
		dev_info(dev, "  [T0] RESET GPIO set to LOW (sensor in reset)\n");
	}

	if (!IS_ERR(imx385->pwdn_gpio)) {
		gpiod_set_value_cansleep(imx385->pwdn_gpio, 0);
		dev_info(dev, "  [T0] PWDN GPIO set to LOW (power on mode)\n");
	}

	/* T1: 按顺序上电：DVDD(1.8V) → DOVDD(1.8V) → AVDD(3.3V) */
	dev_info(dev, "  [T1] Enabling power supplies: DVDD -> DOVDD -> AVDD\n");
	ret = regulator_bulk_enable(IMX385_NUM_SUPPLIES, imx385->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	/* T2: 等待电源稳定（确保 AVDD > 3.15V）*/
	dev_info(dev, "  [T2] Waiting for power supplies to stabilize (3ms)...\n");
	usleep_range(3000, 4000);

		/* T3: 配置并启动输入时钟 INCK（24MHz） */
	dev_info(dev, "  [T3] Configuring and enabling XVCLK (24MHz)...\n");
	ret = clk_set_rate(imx385->xvclk, IMX385_XVCLK_FREQ);
	if (ret < 0) {
		dev_warn(dev, "Failed to set xvclk rate (24MHz): %d\n", ret);
	}

	if (clk_get_rate(imx385->xvclk) != IMX385_XVCLK_FREQ) {
		dev_warn(dev, "xvclk mismatched: expected %d Hz, got %ld Hz\n",
			 IMX385_XVCLK_FREQ, clk_get_rate(imx385->xvclk));
	} else {
		dev_info(dev, "  XVCLK configured to %ld Hz\n", clk_get_rate(imx385->xvclk));
	}

	ret = clk_prepare_enable(imx385->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk: %d\n", ret);
		goto disable_regulator;
	}

	/* T4: 等待时钟稳定 */
	dev_info(dev, "  [T4] Waiting for clock to stabilize (2ms)...\n");
	usleep_range(2000, 3000);

	/* T5: 拉高 XCLR，释放复位，传感器进入 Standby 模式 */
	if (!IS_ERR(imx385->reset_gpio)) {
		gpiod_set_value_cansleep(imx385->reset_gpio, 1);
		dev_info(dev, "  [T5] RESET GPIO set to HIGH (releasing reset)\n");
	}

	/* T6: 等待传感器内部初始化完成，准备 I2C 通信 */
	dev_info(dev, "  [T6] Waiting for sensor initialization (200ms)...\n");
	msleep(200);

	/* T7: 额外延时确保传感器完全就绪 */
	dev_info(dev, "  [T7] Additional stabilization delay (100ms)...\n");
	msleep(100);

	dev_info(dev, "IMX385 power-on sequence completed successfully\n");
	dev_info(dev, "Total power-on time: ~310ms\n");

	return 0;

disable_regulator:
	regulator_bulk_disable(IMX385_NUM_SUPPLIES, imx385->supplies);

	return ret;
}

/**
 * @brief 下电序列
 * @param imx385 设备结构体指针
 * 
 * 按照相反顺序关闭电源和时钟
 * GPIO 极性说明：
 * - PWDN GPIO (GPIO_ACTIVE_HIGH): 1=关机, 0=正常工作
 * - RESET GPIO (GPIO_ACTIVE_HIGH): 0=复位, 1=正常工作
 */
static void __imx385_power_off(struct imx385 *imx385)
{
	/* 步骤1: 设置 PWDN 为高电平（关机模式）*/
	if (!IS_ERR(imx385->pwdn_gpio))
		gpiod_set_value_cansleep(imx385->pwdn_gpio, 1);
	
	/* 步骤2: 关闭时钟 */
	clk_disable_unprepare(imx385->xvclk);
	
	/* 步骤3: 拉低 RESET（进入复位状态）*/
	if (!IS_ERR(imx385->reset_gpio))
		gpiod_set_value_cansleep(imx385->reset_gpio, 0);
	
	/* 步骤4: 关闭电源 */
	regulator_bulk_disable(IMX385_NUM_SUPPLIES, imx385->supplies);
}

/**
 * @brief 运行时 PM 挂起
 */
static int __maybe_unused imx385_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx385 *imx385 = to_imx385(sd);

	return __imx385_power_on(imx385);
}

/**
 * @brief 运行时 PM 恢复
 */
static int __maybe_unused imx385_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx385 *imx385 = to_imx385(sd);

	__imx385_power_off(imx385);

	return 0;
}

/**
 * @brief 启动/停止视频流
 * @param sd V4L2 子设备
 * @param on 1 启动，0 停止
 * @return 0 成功，负数表示错误码
 */
static int imx385_s_stream(struct v4l2_subdev *sd, int on)
{
	struct imx385 *imx385 = to_imx385(sd);
	struct i2c_client *client = imx385->client;
	int ret = 0;

	mutex_lock(&imx385->mutex);
	on = !!on;
	if (on == imx385->streaming)
		goto unlock_and_return;

	if (on) {
		dev_info(&client->dev, "=== IMX385 启动流 ===\n");
		
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			dev_err(&client->dev, "pm_runtime_get_sync 失败: %d\n", ret);
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}
		dev_info(&client->dev, "步骤1: PM runtime 同步完成\n");

		dev_info(&client->dev, "步骤2: 开始写入寄存器配置表...\n");
		dev_info(&client->dev, "  寄存器表地址: %p\n", imx385->cur_mode->reg_list);
		ret = imx385_write_array(imx385->client,
					imx385->cur_mode->reg_list);
		if (ret) {
			dev_err(&client->dev, "写入寄存器配置表失败: %d\n", ret);
			goto err_rpm_put;
		}
		dev_info(&client->dev, "步骤2: 寄存器配置表写入完成\n");

		dev_info(&client->dev, "步骤3: 设置 V4L2 控制参数...\n");
		ret = __v4l2_ctrl_handler_setup(&imx385->ctrl_handler);
		if (ret) {
			dev_err(&client->dev, "V4L2 控制参数设置失败: %d\n", ret);
			goto err_rpm_put;
		}
		dev_info(&client->dev, "步骤3: V4L2 控制参数设置完成\n");

		/* 步骤4: 退出待机模式 (STANDBY = 0x00) - 按工装顺序 */
		dev_info(&client->dev, "步骤4: 退出待机模式 (写入 0x3000 = 0x00)...\n");
		ret = imx385_write_reg(imx385->client,
				      IMX385_REG_CTRL_MODE,
				      IMX385_REG_VALUE_08BIT,
				      IMX385_MODE_STREAMING);
		if (ret) {
			dev_err(&client->dev, "退出待机模式失败: %d\n", ret);
			goto err_rpm_put;
		}
		dev_info(&client->dev, "步骤4: 已退出待机模式\n");

		/* 步骤5: 启动主时序发生器 (XTMSTA = 0x00) - 按工装顺序 */
		dev_info(&client->dev, "步骤5: 启动主时序发生器 (写入 0x3002 = 0x00)...\n");
		ret = imx385_write_reg(imx385->client,
				      IMX385_REG_MASTER_MODE,
				      IMX385_REG_VALUE_08BIT,
				      IMX385_MASTER_MODE_START);
		if (ret) {
			dev_err(&client->dev, "启动主时序发生器失败: %d\n", ret);
			goto err_rpm_put;
		}
		dev_info(&client->dev, "步骤5: 主时序发生器启动完成\n");

		/* 步骤6: 工装额外的曝光和增益设置 */
		dev_info(&client->dev, "步骤6: 设置曝光和增益 (按工装顺序)...\n");
		ret = imx385_write_reg(imx385->client, 0x3021,
				      IMX385_REG_VALUE_08BIT, 0x00);
		ret |= imx385_write_reg(imx385->client, 0x3020,
				       IMX385_REG_VALUE_08BIT, 0x00);
		ret |= imx385_write_reg(imx385->client, 0x3015,
				       IMX385_REG_VALUE_08BIT, 0x00);
		ret |= imx385_write_reg(imx385->client, 0x3014,
				       IMX385_REG_VALUE_08BIT, 0x00);
		if (ret) {
			dev_err(&client->dev, "设置曝光和增益失败: %d\n", ret);
			goto err_rpm_put;
		}
		dev_info(&client->dev, "步骤6: 曝光和增益设置完成\n");

		/* 步骤7: 等待传感器稳定 */
		dev_info(&client->dev, "步骤7: 等待传感器稳定 (30ms)...\n");
		msleep(30);

		dev_info(&client->dev, "=== IMX385 流启动成功 ===\n");
	} else {
		dev_info(&client->dev, "=== IMX385 停止流 ===\n");
		
		/* 步骤1: 停止主时序发生器 (XTMSTA = 0x01) */
		dev_info(&client->dev, "步骤1: 停止主时序发生器 (写入 0x3002 = 0x01)...\n");
		ret = imx385_write_reg(imx385->client,
				      IMX385_REG_MASTER_MODE,
				      IMX385_REG_VALUE_08BIT,
				      IMX385_MASTER_MODE_STOP);
		if (ret) {
			dev_err(&client->dev, "停止主时序发生器失败: %d\n", ret);
		}
		
		/* 步骤2: 进入待机模式 (STANDBY = 0x01) */
		dev_info(&client->dev, "步骤2: 进入待机模式 (写入 0x3000 = 0x01)...\n");
		ret = imx385_write_reg(imx385->client,
				      IMX385_REG_CTRL_MODE,
				      IMX385_REG_VALUE_08BIT,
				      IMX385_MODE_SW_STANDBY);
		if (ret) {
			dev_err(&client->dev, "进入待机模式失败: %d\n", ret);
		}
		
		pm_runtime_put(&client->dev);
		dev_info(&client->dev, "=== IMX385 流停止完成 ===\n");
	}

	imx385->streaming = on;

	mutex_unlock(&imx385->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
unlock_and_return:
	mutex_unlock(&imx385->mutex);

	return ret;
}

/**
 * @brief 上电操作
 */
static int imx385_s_power(struct v4l2_subdev *sd, int on)
{
	struct imx385 *imx385 = to_imx385(sd);
	struct i2c_client *client = imx385->client;
	int ret = 0;

	mutex_lock(&imx385->mutex);

	if (imx385->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = imx385_check_sensor_id(imx385, client);
		if (ret)
			goto err_rpm_put;

		imx385->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		imx385->power_on = false;
	}

	mutex_unlock(&imx385->mutex);

	return ret;

err_rpm_put:
	pm_runtime_put(&client->dev);
unlock_and_return:
	mutex_unlock(&imx385->mutex);

	return ret;
}

/**
 * @brief 获取格式
 */
static int imx385_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx385 *imx385 = to_imx385(sd);

	if (code->index != 0)
		return -EINVAL;
	code->code = imx385->cur_mode->bus_fmt;

	return 0;
}

/**
 * @brief 枚举帧尺寸
 */
static int imx385_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx385 *imx385 = to_imx385(sd);

	if (fse->index >= imx385->support_modes_num)
		return -EINVAL;

	if (fse->code != imx385->support_modes[fse->index].bus_fmt)
		return -EINVAL;

	fse->min_width  = imx385->support_modes[fse->index].width;
	fse->max_width  = imx385->support_modes[fse->index].width;
	fse->max_height = imx385->support_modes[fse->index].height;
	fse->min_height = imx385->support_modes[fse->index].height;

	return 0;
}

/**
 * @brief 获取当前格式
 */
static int imx385_get_fmt(struct v4l2_subdev *sd,
			 struct v4l2_subdev_pad_config *cfg,
			 struct v4l2_subdev_format *fmt)
{
	struct imx385 *imx385 = to_imx385(sd);
	const struct imx385_mode *mode = imx385->cur_mode;

	mutex_lock(&imx385->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
#else
		mutex_unlock(&imx385->mutex);
		return -ENOTTY;
#endif
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = mode->bus_fmt;
		fmt->format.field = V4L2_FIELD_NONE;
	}
	mutex_unlock(&imx385->mutex);

	return 0;
}

/**
 * @brief 设置格式
 */
static int imx385_set_fmt(struct v4l2_subdev *sd,
			 struct v4l2_subdev_pad_config *cfg,
			 struct v4l2_subdev_format *fmt)
{
	struct imx385 *imx385 = to_imx385(sd);
	const struct imx385_mode *mode;
	s64 h_blank, vblank_def;

	mutex_lock(&imx385->mutex);

	mode = imx385_find_best_fit(imx385, fmt);
	fmt->format.code = mode->bus_fmt;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
#else
		mutex_unlock(&imx385->mutex);
		return -ENOTTY;
#endif
	} else {
		imx385->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(imx385->hblank, h_blank,
					 h_blank, 1, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(imx385->vblank, vblank_def,
					 IMX385_VTS_MAX - mode->height,
					 1, vblank_def);
		imx385->cur_vts = mode->vts_def;
	}

	mutex_unlock(&imx385->mutex);

	return 0;
}

/**
 * @brief 获取传感器信息
 */
static int imx385_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct imx385 *imx385 = to_imx385(sd);
	const struct imx385_mode *mode = imx385->cur_mode;

	mutex_lock(&imx385->mutex);
	fi->interval = mode->max_fps;
	mutex_unlock(&imx385->mutex);

	return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
/**
 * @brief 打开子设备
 * @param sd V4L2 子设备
 * @param fh 文件句柄
 * @return 0 成功
 */
static int imx385_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct imx385 *imx385 = to_imx385(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
	const struct imx385_mode *def_mode = &imx385->support_modes[0];

	mutex_lock(&imx385->mutex);
	/* 初始化 try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = def_mode->bus_fmt;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&imx385->mutex);

	return 0;
}
#endif

static int imx385_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
				struct v4l2_mbus_config *config)
{
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->flags = V4L2_MBUS_CSI2_2_LANE |
			V4L2_MBUS_CSI2_CHANNEL_0 |
			V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;

	return 0;
}

static const struct dev_pm_ops imx385_pm_ops = {
	SET_RUNTIME_PM_OPS(imx385_runtime_suspend,
			   imx385_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops imx385_internal_ops = {
	.open = imx385_open,
};
#endif

static const struct v4l2_subdev_core_ops imx385_core_ops = {
	.s_power = imx385_s_power,
};

static const struct v4l2_subdev_video_ops imx385_video_ops = {
	.s_stream = imx385_s_stream,
	.g_frame_interval = imx385_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops imx385_pad_ops = {
	.enum_mbus_code = imx385_enum_mbus_code,
	.enum_frame_size = imx385_enum_frame_sizes,
	.get_fmt = imx385_get_fmt,
	.set_fmt = imx385_set_fmt,
	.get_mbus_config = imx385_g_mbus_config,
};

static const struct v4l2_subdev_ops imx385_subdev_ops = {
	.core	= &imx385_core_ops,
	.video	= &imx385_video_ops,
	.pad	= &imx385_pad_ops,
};

/**
 * @brief 初始化 V4L2 控制
 * @param imx385 设备结构体指针
 * @return 0 成功，负数表示错误码
 */
static int imx385_initialize_controls(struct imx385 *imx385)
{
	const struct imx385_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;

	handler = &imx385->ctrl_handler;
	mode = imx385->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &imx385->mutex;

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, IMX385_PIXEL_RATE_LINEAR, 1,
			  IMX385_PIXEL_RATE_LINEAR);

	h_blank = mode->hts_def - mode->width;
	imx385->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				h_blank, h_blank, 1, h_blank);
	if (imx385->hblank)
		imx385->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	imx385->vblank = v4l2_ctrl_new_std(handler, &imx385_ctrl_ops,
				V4L2_CID_VBLANK, vblank_def,
				IMX385_VTS_MAX - mode->height,
				1, vblank_def);

	exposure_max = mode->vts_def - 2;
	imx385->exposure = v4l2_ctrl_new_std(handler, &imx385_ctrl_ops,
				V4L2_CID_EXPOSURE, IMX385_EXPOSURE_MIN,
				exposure_max, IMX385_EXPOSURE_STEP,
				mode->exp_def);

	imx385->anal_gain = v4l2_ctrl_new_std(handler, &imx385_ctrl_ops,
				V4L2_CID_ANALOGUE_GAIN, IMX385_GAIN_MIN,
				IMX385_GAIN_MAX, IMX385_GAIN_STEP,
				IMX385_GAIN_DEFAULT);

	if (handler->error) {
		ret = handler->error;
		dev_err(&imx385->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	imx385->subdev.ctrl_handler = handler;
	imx385->cur_vts = mode->vts_def;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

/**
 * @brief 检查传感器 ID
 * @param imx385 设备结构体指针
 * @return 0 成功，负数表示错误码
 */
static int imx385_check_hwcfg(struct device *dev)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "Failed to get endpoint\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &bus_cfg);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "Failed to parse endpoint\n");
		return ret;
	}

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX385_LANES) {
		dev_err(dev, "Unsupported number of data lanes %u, only %u supported\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes, IMX385_LANES);
		ret = -EINVAL;
		goto out_free_bus_cfg;
	}

	dev_info(dev, "Using %u data lanes\n", IMX385_LANES);

out_free_bus_cfg:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

/**
 * @brief I2C 驱动 probe 函数
 * @param client I2C 客户端
 * @param id I2C 设备 ID
 * @return 0 成功，负数表示错误码
 */
static int imx385_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct imx385 *imx385;
	struct v4l2_subdev *sd;
	char facing[2];
	int ret;

	dev_info(dev, "driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);
	dev_info(dev, "DEBUG: PIXEL_RATE = %d\n", IMX385_PIXEL_RATE_LINEAR);
	imx385 = devm_kzalloc(dev, sizeof(*imx385), GFP_KERNEL);
	if (!imx385)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
				    &imx385->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
					&imx385->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
					&imx385->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
					&imx385->len_name);
	if (ret) {
		dev_err(dev, "could not get module information!\n");
		return -EINVAL;
	}

	imx385->client = client;
	imx385->support_modes = supported_modes;
	imx385->support_modes_num = ARRAY_SIZE(supported_modes);
	imx385->cur_mode = &supported_modes[0];

	imx385->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(imx385->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	imx385->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(imx385->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	imx385->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(imx385->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	ret = imx385_configure_regulators(imx385);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	imx385->pinctrl = devm_pinctrl_get(dev);
	if (!IS_ERR(imx385->pinctrl)) {
		imx385->pins_default =
			pinctrl_lookup_state(imx385->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_DEFAULT);
		if (IS_ERR(imx385->pins_default))
			dev_err(dev, "could not get default pinstate\n");

		imx385->pins_sleep =
			pinctrl_lookup_state(imx385->pinctrl,
					     OF_CAMERA_PINCTRL_STATE_SLEEP);
		if (IS_ERR(imx385->pins_sleep))
			dev_err(dev, "could not get sleep pinstate\n");
	}

	ret = imx385_check_hwcfg(dev);
	if (ret)
		return ret;

	mutex_init(&imx385->mutex);

	sd = &imx385->subdev;
	v4l2_i2c_subdev_init(sd, client, &imx385_subdev_ops);
	ret = imx385_initialize_controls(imx385);
	if (ret)
		goto err_destroy_mutex;

	ret = __imx385_power_on(imx385);
	if (ret)
		goto err_free_handler;

	ret = imx385_check_sensor_id(imx385, client);
	if (ret)
		goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
	sd->internal_ops = &imx385_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
	imx385->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &imx385->pad);
	if (ret < 0)
		goto err_power_off;
#endif

	memset(facing, 0, sizeof(facing));
	if (strcmp(imx385->module_facing, "back") == 0)
		facing[0] = 'b';
	else
		facing[0] = 'f';

	snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
		 imx385->module_index, facing,
		 IMX385_NAME, dev_name(sd->dev));
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
	__imx385_power_off(imx385);
err_free_handler:
	v4l2_ctrl_handler_free(&imx385->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&imx385->mutex);

	return ret;
}

/**
 * @brief I2C 驱动 remove 函数
 * @param client I2C 客户端
 * @return 0 成功
 */
static int imx385_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx385 *imx385 = to_imx385(sd);

	v4l2_async_unregister_subdev(sd);
#if defined(CONFIG_MEDIA_CONTROLLER)
	media_entity_cleanup(&sd->entity);
#endif
	v4l2_ctrl_handler_free(&imx385->ctrl_handler);
	mutex_destroy(&imx385->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__imx385_power_off(imx385);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id imx385_of_match[] = {
	{ .compatible = "sony,imx385" },
	{},
};
MODULE_DEVICE_TABLE(of, imx385_of_match);
#endif

static const struct i2c_device_id imx385_match_id[] = {
	{ "sony,imx385", 0 },
	{ },
};

static struct i2c_driver imx385_i2c_driver = {
	.driver = {
		.name = IMX385_NAME,
		.pm = &imx385_pm_ops,
		.of_match_table = of_match_ptr(imx385_of_match),
	},
	.probe		= &imx385_probe,
	.remove		= &imx385_remove,
	.id_table	= imx385_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&imx385_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&imx385_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("Sony IMX385 sensor driver");
MODULE_LICENSE("GPL v2");
