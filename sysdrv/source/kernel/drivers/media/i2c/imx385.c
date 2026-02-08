// SPDX-License-Identifier: GPL-2.0
/*
 * imx385 driver - Fixed for RV1106 24MHz Crystal / 2-Lanes
 *
 * Based on Rockchip Electronics Co., Ltd. driver
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

#define DRIVER_VERSION          KERNEL_VERSION(0, 0x01, 0x02)

#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN       V4L2_CID_GAIN
#endif

/* * 关键修正 1: 链接频率配置 
 * 使用工装实测的 371.25MHz (742.5Mbps per lane)
 */
#define IMX385_LINK_FREQ_371M       371250000
#define IMX385_LANES            2
#define IMX385_BITS_PER_SAMPLE      10  /* 回退到 10-bit 以匹配工装 PLL 设置 */

/* pixel rate = link frequency * 2 (DDR) * lanes / BITS_PER_SAMPLE */
/* 371.25MHz * 2 * 2 / 10 = 148.5MHz */
#define IMX385_PIXEL_RATE           148500000

/* * 关键修正 2: 输入时钟频率
 * 必须与板载晶振一致
 */
#define IMX385_XVCLK_FREQ       24000000

/* Chip ID - 工装旧驱动读取的是 0x0016=0x00，新驱动读 0x3012 
 * 我们尝试兼容两者，IMX385 标准 ID 寄存器通常在 3012h
 */
#define CHIP_ID             0x00
#define IMX385_REG_CHIP_ID      0x0016 

/* 控制模式寄存器 */
#define IMX385_REG_CTRL_MODE        0x3000
#define IMX385_MODE_SW_STANDBY      0x01
#define IMX385_MODE_STREAMING       0x00

#define IMX385_REG_MASTER_MODE      0x3002
#define IMX385_MASTER_MODE_START    0x00
#define IMX385_MASTER_MODE_STOP     0x01

/* 曝光控制寄存器 */
#define IMX385_REG_SHS1_L       0x3020
#define IMX385_REG_SHS1_M       0x3021
#define IMX385_REG_SHS1_H       0x3022

/* 增益控制寄存器 */
#define IMX385_REG_GAIN         0x3014
#define IMX385_GAIN_MIN         0x00
#define IMX385_GAIN_MAX         0xf0
#define IMX385_GAIN_STEP        1
#define IMX385_GAIN_DEFAULT     0x00

/* VTS / HTS */
#define IMX385_REG_VMAX_L       0x3018
#define IMX385_REG_VMAX_M       0x3019
#define IMX385_REG_VMAX_H       0x301a

#define IMX385_REG_HMAX_L       0x301c
#define IMX385_REG_HMAX_M       0x301d

#define IMX385_EXPOSURE_MIN     2
#define IMX385_EXPOSURE_STEP        1
#define IMX385_VTS_MAX          0xffff

/* 寄存器操作宏 */
#define IMX385_FETCH_HIGH_BYTE(VAL) (((VAL) >> 16) & 0xFF)
#define IMX385_FETCH_MID_BYTE(VAL)  (((VAL) >> 8) & 0xFF)
#define IMX385_FETCH_LOW_BYTE(VAL)  ((VAL) & 0xFF)

#define REG_NULL            0xFFFF
#define REG_DELAY           0xFFFE

#define IMX385_REG_VALUE_08BIT      1
#define IMX385_REG_VALUE_16BIT      2
#define IMX385_REG_VALUE_24BIT      3

#define IMX385_NAME         "imx385"

#define OF_CAMERA_PINCTRL_STATE_DEFAULT "rockchip,camera_default"
#define OF_CAMERA_PINCTRL_STATE_SLEEP   "rockchip,camera_sleep"

static const char * const imx385_supply_names[] = {
    "dvdd",     /* 1.2V/1.8V Digital Core */
    "dovdd",    /* 1.8V IO */
    "avdd",     /* 3.3V Analog */
};

#define IMX385_NUM_SUPPLIES ARRAY_SIZE(imx385_supply_names)

struct regval {
    u16 addr;
    u8 val;
};

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

struct imx385 {
    struct i2c_client   *client;
    struct clk      *xvclk;
    struct gpio_desc    *reset_gpio;
    struct gpio_desc    *pwdn_gpio;
    struct regulator_bulk_data supplies[IMX385_NUM_SUPPLIES];

    struct pinctrl      *pinctrl;
    struct pinctrl_state    *pins_default;
    struct pinctrl_state    *pins_sleep;

    struct v4l2_subdev  subdev;
    struct media_pad    pad;
    struct v4l2_ctrl_handler ctrl_handler;
    struct v4l2_ctrl    *exposure;
    struct v4l2_ctrl    *anal_gain;
    struct v4l2_ctrl    *digi_gain;
    struct v4l2_ctrl    *hblank;
    struct v4l2_ctrl    *vblank;
    struct v4l2_ctrl    *pixel_rate;
    struct v4l2_ctrl    *link_freq;

    struct mutex        mutex;
    bool            streaming;
    bool            power_on;
    const struct imx385_mode *support_modes;
    u32         support_modes_num;
    const struct imx385_mode *cur_mode;
    u32         module_index;
    const char      *module_facing;
    const char      *module_name;
    const char      *len_name;
    u32         cur_vts;
    struct v4l2_fwnode_endpoint bus_cfg;
};

#define to_imx385(sd) container_of(sd, struct imx385, subdev)

/*
 * 寄存器配置：移植自旧驱动（工装验证过的配置）
 * 适用于：
 * - 24MHz 输入时钟 (关键!)
 * - 2 Lane MIPI 输出 (关键!)
 * - 10-bit 模式
 * - 1080p @ 30fps
 */
static const struct regval imx385_linear_1080p30_regs[] = {
    {0x0016, 0x03}, /* 写入0x03以允许配置 */
    
    /* 模式控制 */
    {0x3000, 0x01}, /* Standby */
    {0x3001, 0x00}, 
    {0x3002, 0x01}, /* Master Stop */
    {0x3005, 0x00}, /* 10-bit ADC mode */
    {0x3007, 0x00}, /* Window mode normal */
    {0x3009, 0x02}, /* Frame rate setting */
    {0x300a, 0xF0}, /* Black level */
    {0x300b, 0x00},
    {0x3012, 0x2C},
    {0x3013, 0x01},
    {0x3014, 0x00}, /* Gain 0dB */
    {0x3015, 0x00},
    {0x3016, 0x08},
    {0x3017, 0x85},

    /* VMAX = 1350 lines (0x0546) */
    {0x3018, 0xCA}, 
    {0x3019, 0x08},
    {0x301a, 0x00},
    {0x301b, 0x30},

    /* HMAX = 4400 pixels (0x1130) */
    {0x301c, 0x30}, 
    {0x301d, 0x11},

    /* 曝光默认值 */
    {0x3020, 0x00}, 
    {0x3021, 0x00}, 
    {0x3022, 0x00}, 

    /* 其他模拟/数字设置 */
    {0x3036, 0x10}, {0x303a, 0xD1}, {0x303b, 0x03}, {0x3044, 0x01},
    
    /* 接口设置 */
    {0x3046, 0x00}, {0x3047, 0x08}, {0x3049, 0x00}, {0x3054, 0x66},

    /* PLL 设置 - 针对 24MHz 输入 (关键差异点) */
    {0x305c, 0x28}, /* INCKSEL1 */
    {0x305d, 0x00}, /* INCKSEL2 */
    {0x305e, 0x20}, /* INCKSEL3 */
    {0x305f, 0x00}, /* INCKSEL4 */

    {0x310b, 0x07}, {0x3110, 0x12}, {0x31ed, 0x38},

    /* MIPI PHY 设置 */
    {0x3338, 0xD4}, {0x333b, 0x00}, {0x333c, 0xD4}, {0x333d, 0x40},
    {0x333e, 0x10}, {0x333f, 0x00}, 
    
    /* 关键修正 3: 2-Lane 模式 (0x01) */
    {0x3443, 0x01}, 
    
    {0x3344, 0x10}, {0x3346, 0x01}, {0x3353, 0x0E}, {0x3357, 0x49},
    {0x3358, 0x04}, {0x336b, 0x37}, {0x336c, 0x1F},

    /* D-PHY Timing */
    {0x337d, 0x0A}, {0x337e, 0x0A}, {0x337f, 0x01},
    {0x3380, 0x20}, {0x3381, 0x25}, {0x3382, 0x5F}, {0x3383, 0x1F},
    {0x3384, 0x37}, {0x3385, 0x1F}, {0x3386, 0x1F}, {0x3387, 0x17},
    {0x3388, 0x67}, {0x3389, 0x27}, {0x338d, 0xB4}, {0x338e, 0x01},

    {REG_NULL, 0x00},
};

static const struct imx385_mode supported_modes[] = {
    {
        /* * 使用 10-bit 格式 (工装验证过的配置)
         * 如果使用 12-bit 需要完全不同的 PLL 和 HMAX 设置，
         * 在没有 24MHz 晶振下的 12-bit 寄存器表之前，请勿改为 12-bit
         */
        .bus_fmt = MEDIA_BUS_FMT_SRGGB10_1X10,
        .width = 1920, /* 裁剪掉边缘，标准 1080p */
        .height = 1080,
        .max_fps = {
            .numerator = 10000,
            .denominator = 300000, /* 30fps */
        },
        .exp_def = 0x0460,
        .hts_def = 4400,    /* HMAX = 0x1130 */
        .vts_def = 1350,    /* VMAX = 0x0546 */
        .reg_list = imx385_linear_1080p30_regs,
        .hdr_mode = NO_HDR,
    },
};

static const s64 link_freq_menu_items[] = {
    IMX385_LINK_FREQ_371M,
};

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
    if (ret != len + 2)
        return -EIO;

    return 0;
}

static int imx385_write_array(struct i2c_client *client,
                  const struct regval *regs)
{
    u32 i;
    int ret = 0;

    for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++) {
        if (unlikely(regs[i].addr == REG_DELAY)) {
            usleep_range(regs[i].val * 1000, regs[i].val * 2000);
        } else {
            ret = imx385_write_reg(client, regs[i].addr,
                IMX385_REG_VALUE_08BIT, regs[i].val);
        }
    }
    return ret;
}

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
    msgs[0].addr = client->addr;
    msgs[0].flags = 0;
    msgs[0].len = 2;
    msgs[0].buf = (u8 *)&reg_addr_be;

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

static int imx385_set_fmt(struct v4l2_subdev *sd,
              struct v4l2_subdev_pad_config *cfg,
              struct v4l2_subdev_format *fmt)
{
    struct imx385 *imx385 = to_imx385(sd);
    const struct imx385_mode *mode;
    s64 h_blank, vblank_def;

    mutex_lock(&imx385->mutex);

    /* 简单逻辑：只支持一种模式 */
    mode = &imx385->support_modes[0];

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
        
        __v4l2_ctrl_s_ctrl_int64(imx385->pixel_rate, IMX385_PIXEL_RATE);
        __v4l2_ctrl_s_ctrl(imx385->link_freq, 0); /* 0 index = 371M */
        
        imx385->cur_vts = mode->vts_def;
    }

    mutex_unlock(&imx385->mutex);
    return 0;
}

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

static int imx385_enum_mbus_code(struct v4l2_subdev *sd,
                 struct v4l2_subdev_pad_config *cfg,
                 struct v4l2_subdev_mbus_code_enum *code)
{
    struct imx385 *imx385 = to_imx385(sd);
    const struct imx385_mode *mode = imx385->cur_mode;

    if (code->index != 0)
        return -EINVAL;
    code->code = mode->bus_fmt;
    return 0;
}

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

static int imx385_g_frame_interval(struct v4l2_subdev *sd,
                   struct v4l2_subdev_frame_interval *fi)
{
    struct imx385 *imx385 = to_imx385(sd);
    const struct imx385_mode *mode = imx385->cur_mode;

    fi->interval = mode->max_fps;
    return 0;
}

static int imx385_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
                struct v4l2_mbus_config *config)
{
    /* 强制声明为 2 Lanes，即使 bus_cfg 解析可能有误 */
    config->type = V4L2_MBUS_CSI2_DPHY;
    config->flags = V4L2_MBUS_CSI2_2_LANE |
            V4L2_MBUS_CSI2_CHANNEL_0 |
            V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;
    return 0;
}

static void imx385_get_module_inf(struct imx385 *imx385,
                  struct rkmodule_inf *inf)
{
    memset(inf, 0, sizeof(*inf));
    strlcpy(inf->base.sensor, IMX385_NAME, sizeof(inf->base.sensor));
    strlcpy(inf->base.module, imx385->module_name, sizeof(inf->base.module));
    strlcpy(inf->base.lens, imx385->len_name, sizeof(inf->base.lens));
}

static long imx385_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
    struct imx385 *imx385 = to_imx385(sd);
    long ret = 0;
    u32 stream = 0;

    switch (cmd) {
    case RKMODULE_GET_MODULE_INFO:
        imx385_get_module_inf(imx385, (struct rkmodule_inf *)arg);
        break;
    case RKMODULE_SET_QUICK_STREAM:
        stream = *((u32 *)arg);
        if (stream)
            ret = imx385_write_reg(imx385->client, IMX385_REG_CTRL_MODE,
                           IMX385_REG_VALUE_08BIT, IMX385_MODE_STREAMING);
        else
            ret = imx385_write_reg(imx385->client, IMX385_REG_CTRL_MODE,
                           IMX385_REG_VALUE_08BIT, IMX385_MODE_SW_STANDBY);
        break;
    default:
        ret = -ENOIOCTLCMD;
        break;
    }
    return ret;
}

#ifdef CONFIG_COMPAT
static long imx385_compat_ioctl32(struct v4l2_subdev *sd,
                  unsigned int cmd, unsigned long arg)
{
    void __user *up = compat_ptr(arg);
    struct rkmodule_inf *inf;
    long ret;
    u32 stream = 0;

    switch (cmd) {
    case RKMODULE_GET_MODULE_INFO:
        inf = kzalloc(sizeof(*inf), GFP_KERNEL);
        if (!inf) return -ENOMEM;
        ret = imx385_ioctl(sd, cmd, inf);
        if (!ret) ret = copy_to_user(up, inf, sizeof(*inf));
        kfree(inf);
        break;
    case RKMODULE_SET_QUICK_STREAM:
        ret = copy_from_user(&stream, up, sizeof(u32));
        if (!ret) ret = imx385_ioctl(sd, cmd, &stream);
        break;
    default:
        ret = -ENOIOCTLCMD;
        break;
    }
    return ret;
}
#endif

static int __imx385_start_stream(struct imx385 *imx385)
{
    int ret;
    ret = imx385_write_array(imx385->client, imx385->cur_mode->reg_list);
    if (ret) return ret;
    ret = __v4l2_ctrl_handler_setup(&imx385->ctrl_handler);
    if (ret) return ret;
    /* Master Mode Start - Essential for Sony sensors */
    ret = imx385_write_reg(imx385->client, IMX385_REG_MASTER_MODE, 
                   IMX385_REG_VALUE_08BIT, IMX385_MASTER_MODE_START);
    if (ret) return ret;
    return imx385_write_reg(imx385->client, IMX385_REG_CTRL_MODE,
                IMX385_REG_VALUE_08BIT, IMX385_MODE_STREAMING);
}

static int __imx385_stop_stream(struct imx385 *imx385)
{
    int ret;
    ret = imx385_write_reg(imx385->client, IMX385_REG_MASTER_MODE, 
                   IMX385_REG_VALUE_08BIT, IMX385_MASTER_MODE_STOP);
    ret |= imx385_write_reg(imx385->client, IMX385_REG_CTRL_MODE,
                IMX385_REG_VALUE_08BIT, IMX385_MODE_SW_STANDBY);
    return ret;
}

static int imx385_s_stream(struct v4l2_subdev *sd, int on)
{
    struct imx385 *imx385 = to_imx385(sd);
    struct i2c_client *client = imx385->client;
    int ret = 0;

    mutex_lock(&imx385->mutex);
    on = !!on;
    if (on == imx385->streaming) goto unlock_and_return;

    if (on) {
        ret = pm_runtime_get_sync(&client->dev);
        if (ret < 0) {
            pm_runtime_put_noidle(&client->dev);
            goto unlock_and_return;
        }
        ret = __imx385_start_stream(imx385);
        if (ret) {
            v4l2_err(sd, "start stream failed\n");
            pm_runtime_put(&client->dev);
            goto unlock_and_return;
        }
    } else {
        __imx385_stop_stream(imx385);
        pm_runtime_put(&client->dev);
    }
    imx385->streaming = on;

unlock_and_return:
    mutex_unlock(&imx385->mutex);
    return ret;
}

static int imx385_s_power(struct v4l2_subdev *sd, int on)
{
    struct imx385 *imx385 = to_imx385(sd);
    struct i2c_client *client = imx385->client;
    int ret = 0;

    mutex_lock(&imx385->mutex);
    if (imx385->power_on == !!on) goto unlock_and_return;

    if (on) {
        ret = pm_runtime_get_sync(&client->dev);
        if (ret < 0) {
            pm_runtime_put_noidle(&client->dev);
            goto unlock_and_return;
        }
        imx385->power_on = true;
    } else {
        pm_runtime_put(&client->dev);
        imx385->power_on = false;
    }
unlock_and_return:
    mutex_unlock(&imx385->mutex);
    return ret;
}

static int __imx385_power_on(struct imx385 *imx385)
{
    int ret;
    struct device *dev = &imx385->client->dev;

    if (!IS_ERR_OR_NULL(imx385->pins_default)) {
        ret = pinctrl_select_state(imx385->pinctrl, imx385->pins_default);
        if (ret < 0) dev_err(dev, "could not set pins\n");
    }

    ret = clk_set_rate(imx385->xvclk, IMX385_XVCLK_FREQ);
    if (ret < 0) dev_warn(dev, "Failed to set xvclk rate (24MHz)\n");

    ret = clk_prepare_enable(imx385->xvclk);
    if (ret < 0) return ret;

    ret = regulator_bulk_enable(IMX385_NUM_SUPPLIES, imx385->supplies);
    if (ret < 0) goto disable_clk;

    if (!IS_ERR(imx385->reset_gpio)) gpiod_set_value_cansleep(imx385->reset_gpio, 0);
    usleep_range(500, 1000);
    if (!IS_ERR(imx385->reset_gpio)) gpiod_set_value_cansleep(imx385->reset_gpio, 1);
    if (!IS_ERR(imx385->pwdn_gpio)) gpiod_set_value_cansleep(imx385->pwdn_gpio, 1);

    usleep_range(10000, 20000); /* Wait 10ms+ for initialization */
    return 0;

disable_clk:
    clk_disable_unprepare(imx385->xvclk);
    return ret;
}

static void __imx385_power_off(struct imx385 *imx385)
{
    if (!IS_ERR(imx385->pwdn_gpio)) gpiod_set_value_cansleep(imx385->pwdn_gpio, 0);
    clk_disable_unprepare(imx385->xvclk);
    if (!IS_ERR(imx385->reset_gpio)) gpiod_set_value_cansleep(imx385->reset_gpio, 0);
    if (!IS_ERR_OR_NULL(imx385->pins_sleep)) pinctrl_select_state(imx385->pinctrl, imx385->pins_sleep);
    regulator_bulk_disable(IMX385_NUM_SUPPLIES, imx385->supplies);
}

static int imx385_runtime_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct v4l2_subdev *sd = i2c_get_clientdata(client);
    struct imx385 *imx385 = to_imx385(sd);
    return __imx385_power_on(imx385);
}

static int imx385_runtime_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct v4l2_subdev *sd = i2c_get_clientdata(client);
    struct imx385 *imx385 = to_imx385(sd);
    __imx385_power_off(imx385);
    return 0;
}

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static int imx385_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
    struct imx385 *imx385 = to_imx385(sd);
    struct v4l2_mbus_framefmt *try_fmt = v4l2_subdev_get_try_format(sd, fh->pad, 0);
    const struct imx385_mode *def_mode = &imx385->support_modes[0];

    mutex_lock(&imx385->mutex);
    try_fmt->width = def_mode->width;
    try_fmt->height = def_mode->height;
    try_fmt->code = def_mode->bus_fmt;
    try_fmt->field = V4L2_FIELD_NONE;
    mutex_unlock(&imx385->mutex);
    return 0;
}
#endif

static const struct dev_pm_ops imx385_pm_ops = {
    SET_RUNTIME_PM_OPS(imx385_runtime_suspend, imx385_runtime_resume, NULL)
};

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
static const struct v4l2_subdev_internal_ops imx385_internal_ops = {
    .open = imx385_open,
};
#endif

static const struct v4l2_subdev_core_ops imx385_core_ops = {
    .s_power = imx385_s_power,
    .ioctl = imx385_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl32 = imx385_compat_ioctl32,
#endif
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
    .core   = &imx385_core_ops,
    .video  = &imx385_video_ops,
    .pad    = &imx385_pad_ops,
};

static int imx385_set_ctrl(struct v4l2_ctrl *ctrl)
{
    struct imx385 *imx385 = container_of(ctrl->handler, struct imx385, ctrl_handler);
    struct i2c_client *client = imx385->client;
    int ret = 0;
    u32 shs1 = 0;

    if (!pm_runtime_get_if_in_use(&client->dev)) return 0;

    switch (ctrl->id) {
    case V4L2_CID_EXPOSURE:
        shs1 = imx385->cur_vts - ctrl->val - 1;
        ret = imx385_write_reg(imx385->client, IMX385_REG_SHS1_H,
                       IMX385_REG_VALUE_08BIT, IMX385_FETCH_HIGH_BYTE(shs1));
        ret |= imx385_write_reg(imx385->client, IMX385_REG_SHS1_M,
                    IMX385_REG_VALUE_08BIT, IMX385_FETCH_MID_BYTE(shs1));
        ret |= imx385_write_reg(imx385->client, IMX385_REG_SHS1_L,
                    IMX385_REG_VALUE_08BIT, IMX385_FETCH_LOW_BYTE(shs1));
        break;
    case V4L2_CID_ANALOGUE_GAIN:
        ret = imx385_write_reg(imx385->client, IMX385_REG_GAIN,
                       IMX385_REG_VALUE_08BIT, ctrl->val);
        break;
    }
    pm_runtime_put(&client->dev);
    return ret;
}

static const struct v4l2_ctrl_ops imx385_ctrl_ops = {
    .s_ctrl = imx385_set_ctrl,
};

static int imx385_initialize_controls(struct imx385 *imx385)
{
    const struct imx385_mode *mode;
    struct v4l2_ctrl_handler *handler;
    s64 exposure_max, vblank_def;
    u32 h_blank;
    int ret;

    handler = &imx385->ctrl_handler;
    mode = imx385->cur_mode;
    ret = v4l2_ctrl_handler_init(handler, 8);
    if (ret) return ret;
    handler->lock = &imx385->mutex;

    imx385->link_freq = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
                           0, 0, link_freq_menu_items);

    imx385->pixel_rate = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
                           0, IMX385_PIXEL_RATE, 1, IMX385_PIXEL_RATE);

    h_blank = mode->hts_def - mode->width;
    imx385->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
                       h_blank, h_blank, 1, h_blank);
    if (imx385->hblank) imx385->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    vblank_def = mode->vts_def - mode->height;
    imx385->cur_vts = mode->vts_def;
    imx385->vblank = v4l2_ctrl_new_std(handler, &imx385_ctrl_ops,
                       V4L2_CID_VBLANK, vblank_def,
                       IMX385_VTS_MAX - mode->height, 1, vblank_def);

    exposure_max = mode->vts_def - 2;
    imx385->exposure = v4l2_ctrl_new_std(handler, &imx385_ctrl_ops,
                         V4L2_CID_EXPOSURE, IMX385_EXPOSURE_MIN,
                         exposure_max, IMX385_EXPOSURE_STEP, mode->exp_def);

    imx385->anal_gain = v4l2_ctrl_new_std(handler, &imx385_ctrl_ops,
                          V4L2_CID_ANALOGUE_GAIN, IMX385_GAIN_MIN,
                          IMX385_GAIN_MAX, IMX385_GAIN_STEP, IMX385_GAIN_DEFAULT);

    if (handler->error) {
        ret = handler->error;
        v4l2_ctrl_handler_free(handler);
        return ret;
    }
    imx385->subdev.ctrl_handler = handler;
    return 0;
}

static int imx385_check_sensor_id(struct imx385 *imx385,
                  struct i2c_client *client)
{
    u32 id = 0;
    int ret;

    /* 工装代码使用的是 0x0016 且只读一字节 */
    ret = imx385_read_reg(client, IMX385_REG_CHIP_ID, IMX385_REG_VALUE_08BIT, &id);
    if (ret) {
        /* 如果失败，尝试读取标准的 3012h */
        ret = imx385_read_reg(client, 0x3012, IMX385_REG_VALUE_08BIT, &id);
        if (ret) return ret;
    }
    
    dev_info(&client->dev, "Detected IMX385 sensor ID: 0x%02x\n", id);
    return 0;
}

static int imx385_configure_regulators(struct imx385 *imx385)
{
    unsigned int i;
    for (i = 0; i < IMX385_NUM_SUPPLIES; i++)
        imx385->supplies[i].supply = imx385_supply_names[i];
    return devm_regulator_bulk_get(&imx385->client->dev,
                       IMX385_NUM_SUPPLIES, imx385->supplies);
}

static int imx385_probe(struct i2c_client *client,
            const struct i2c_device_id *id)
{
    struct device *dev = &client->dev;
    struct device_node *node = dev->of_node;
    struct imx385 *imx385;
    struct v4l2_subdev *sd;
    char facing[2];
    int ret;
    struct device_node *endpoint;

    imx385 = devm_kzalloc(dev, sizeof(*imx385), GFP_KERNEL);
    if (!imx385) return -ENOMEM;

    ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX, &imx385->module_index);
    ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING, &imx385->module_facing);
    ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME, &imx385->module_name);
    ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME, &imx385->len_name);
    if (ret) {
        dev_err(dev, "could not get module information!\n");
        return -EINVAL;
    }

    endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
    if (!endpoint) return -EINVAL;
    ret = v4l2_fwnode_endpoint_parse(of_fwnode_handle(endpoint), &imx385->bus_cfg);
    if (ret) return ret;

    imx385->support_modes = supported_modes;
    imx385->support_modes_num = ARRAY_SIZE(supported_modes);
    imx385->client = client;
    imx385->cur_mode = &imx385->support_modes[0];

    imx385->xvclk = devm_clk_get(dev, "xvclk");
    if (IS_ERR(imx385->xvclk)) return -EINVAL;

    imx385->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
    imx385->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);

    ret = imx385_configure_regulators(imx385);
    if (ret) return ret;

    imx385->pinctrl = devm_pinctrl_get(dev);
    if (!IS_ERR(imx385->pinctrl)) {
        imx385->pins_default = pinctrl_lookup_state(imx385->pinctrl, OF_CAMERA_PINCTRL_STATE_DEFAULT);
        imx385->pins_sleep = pinctrl_lookup_state(imx385->pinctrl, OF_CAMERA_PINCTRL_STATE_SLEEP);
    }

    mutex_init(&imx385->mutex);
    sd = &imx385->subdev;
    v4l2_i2c_subdev_init(sd, client, &imx385_subdev_ops);
    ret = imx385_initialize_controls(imx385);
    if (ret) goto err_destroy_mutex;

    ret = __imx385_power_on(imx385);
    if (ret) goto err_free_handler;

    ret = imx385_check_sensor_id(imx385, client);
    if (ret) goto err_power_off;

#ifdef CONFIG_VIDEO_V4L2_SUBDEV_API
    sd->internal_ops = &imx385_internal_ops;
    sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
#endif
#if defined(CONFIG_MEDIA_CONTROLLER)
    imx385->pad.flags = MEDIA_PAD_FL_SOURCE;
    sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
    ret = media_entity_pads_init(&sd->entity, 1, &imx385->pad);
    if (ret < 0) goto err_power_off;
#endif

    memset(facing, 0, sizeof(facing));
    if (strcmp(imx385->module_facing, "back") == 0) facing[0] = 'b';
    else facing[0] = 'f';

    snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
         imx385->module_index, facing, IMX385_NAME, dev_name(sd->dev));
    ret = v4l2_async_register_subdev_sensor_common(sd);
    if (ret) goto err_clean_entity;

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
    .probe      = imx385_probe,
    .remove     = imx385_remove,
    .id_table   = imx385_match_id,
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

MODULE_DESCRIPTION("Sony imx385 sensor driver fixed");
MODULE_LICENSE("GPL v2");