// SPDX-License-Identifier: GPL-2.0+
/*
 * TI SN65DSI83/SN65DSI84 MIPI-DSI to LVDS bridge
 *
 * Ported from the Linux DRM bridge driver:
 *	 drivers/gpu/drm/bridge/ti-sn65dsi83.c
 *
 * This U-Boot port currently supports:
 *	 - SN65DSI83 and SN65DSI84 in single-link LVDS mode
 *	 - single-link DSI input
 *	 - single-link LVDS output
 *	 - RGB888 DSI input (24 bpp)
 *	 - SPWG24 or JEIDA24 LVDS output
 *
 * Intended for U-Boot DM + VIDEO_BRIDGE + DM_I2C.
 */

#include <dm.h>
#include <dm/device_compat.h>
#include <i2c.h>
#include <panel.h>
#include <video_bridge.h>
#include <video_link.h>
#include <asm/gpio.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <string.h>

/* ID registers */
#define REG_ID(n)								(0x00 + (n))

/* Reset and clock registers */
#define REG_RC_RESET							0x09
#define REG_RC_RESET_SOFT_RESET					BIT(0)

#define REG_RC_LVDS_PLL							0x0a
#define REG_RC_LVDS_PLL_PLL_EN_STAT				BIT(7)
#define REG_RC_LVDS_PLL_LVDS_CLK_RANGE(n)		(((n) & 0x7) << 1)
#define REG_RC_LVDS_PLL_HS_CLK_SRC_DPHY			BIT(0)

#define REG_RC_DSI_CLK							0x0b
#define REG_RC_DSI_CLK_DSI_CLK_DIVIDER(n)		(((n) & 0x1f) << 3)
#define REG_RC_DSI_CLK_REFCLK_MULTIPLIER(n)		((n) & 0x3)

#define REG_RC_PLL_EN							0x0d
#define REG_RC_PLL_EN_PLL_EN					BIT(0)

/* DSI registers */
#define REG_DSI_LANE							0x10
#define REG_DSI_LANE_DSI_CHANNEL_MODE_SINGLE	BIT(5)
#define REG_DSI_LANE_CHA_DSI_LANES(n)			(((n) & 0x3) << 3)
#define REG_DSI_LANE_CHB_DSI_LANES(n)			(((n) & 0x3) << 1)
#define REG_DSI_LANE_SOT_ERR_TOL_DIS			BIT(0)

#define REG_DSI_EQ								0x11
#define REG_DSI_CLK								0x12
#define REG_DSI_CLK_CHA_DSI_CLK_RANGE(n)		((n) & 0xff)

/* LVDS registers */
#define REG_LVDS_FMT							0x18
#define REG_LVDS_FMT_DE_NEG_POLARITY			BIT(7)
#define REG_LVDS_FMT_HS_NEG_POLARITY			BIT(6)
#define REG_LVDS_FMT_VS_NEG_POLARITY			BIT(5)
#define REG_LVDS_FMT_LVDS_LINK_CFG				BIT(4)
#define REG_LVDS_FMT_CHA_24BPP_MODE				BIT(3)
#define REG_LVDS_FMT_CHB_24BPP_MODE				BIT(2)
#define REG_LVDS_FMT_CHA_24BPP_FORMAT1			BIT(1)
#define REG_LVDS_FMT_CHB_24BPP_FORMAT1			BIT(0)

#define REG_LVDS_VCOM							0x19
#define REG_LVDS_LANE							0x1a
#define REG_LVDS_LANE_CHA_LVDS_TERM				BIT(1)
#define REG_LVDS_LANE_CHB_LVDS_TERM				BIT(0)
#define REG_LVDS_CM								0x1b

/* Video registers */
#define REG_VID_CHA_ACTIVE_LINE_LENGTH_LOW		0x20
#define REG_VID_CHA_ACTIVE_LINE_LENGTH_HIGH		0x21
#define REG_VID_CHA_VERTICAL_DISPLAY_SIZE_LOW	0x24
#define REG_VID_CHA_VERTICAL_DISPLAY_SIZE_HIGH	0x25
#define REG_VID_CHA_SYNC_DELAY_LOW				0x28
#define REG_VID_CHA_SYNC_DELAY_HIGH				0x29
#define REG_VID_CHA_HSYNC_PULSE_WIDTH_LOW		0x2c
#define REG_VID_CHA_HSYNC_PULSE_WIDTH_HIGH		0x2d
#define REG_VID_CHA_VSYNC_PULSE_WIDTH_LOW		0x30
#define REG_VID_CHA_VSYNC_PULSE_WIDTH_HIGH		0x31
#define REG_VID_CHA_HORIZONTAL_BACK_PORCH		0x34
#define REG_VID_CHA_VERTICAL_BACK_PORCH			0x36
#define REG_VID_CHA_HORIZONTAL_FRONT_PORCH		0x38
#define REG_VID_CHA_VERTICAL_FRONT_PORCH		0x3a
#define REG_VID_CHA_TEST_PATTERN				0x3c

/* IRQ/status registers */
#define REG_IRQ_STAT							0xe5
#define REG_IRQ_STAT_CHA_SYNCH_ERR				BIT(7)
#define REG_IRQ_STAT_CHA_CRC_ERR				BIT(6)
#define REG_IRQ_STAT_CHA_UNC_ECC_ERR			BIT(5)
#define REG_IRQ_STAT_CHA_COR_ECC_ERR			BIT(4)
#define REG_IRQ_STAT_CHA_LLP_ERR				BIT(3)
#define REG_IRQ_STAT_CHA_SOT_BIT_ERR			BIT(2)
#define REG_IRQ_STAT_CHA_PLL_UNLOCK				 BIT(0)

#define SN65DSI83_DSI_BPP						24
#define SN65DSI83_MIN_LVDS_CLK_HZ				25000000U
#define SN65DSI83_MAX_LVDS_CLK_HZ				154000000U

enum sn65dsi8x_model {
	SN65DSI83,
    SN65DSI84,
};

struct sn65dsi8x_priv {
	struct gpio_desc enable_gpio;
	unsigned int dsi_lanes;
	bool lvds_format_24bpp;
	bool lvds_format_jeida;
	enum sn65dsi8x_model model;
	bool enabled;
};

static int sn65dsi8x_get_dsi_config(struct udevice *dev, struct mipi_dsi_device *device)
{
	struct sn65dsi8x_priv *priv = dev_get_priv(dev);

	device->lanes = priv->dsi_lanes;
	device->format = MIPI_DSI_FMT_RGB888;
	device->mode_flags =
		MIPI_DSI_MODE_VIDEO |
		MIPI_DSI_MODE_VIDEO_BURST;

	return 0;
}

static int sn65dsi8x_write(struct udevice *dev, u8 reg, u8 val)
{
	int ret;

	ret = dm_i2c_reg_write(dev, reg, val);
	if (ret)
		dev_err(dev, "I2C write reg 0x%02x failed: %d\n", reg, ret);

	return ret;
}

static int sn65dsi8x_read(struct udevice *dev, u8 reg, u8 *val)
{
	int ret;

	ret = dm_i2c_reg_read(dev, reg);
	if (ret < 0) {
		dev_err(dev, "I2C read reg 0x%02x failed: %d\n", reg, ret);
		return ret;
	}

	*val = (u8)ret;
	return 0;
}

static int sn65dsi8x_write16(struct udevice *dev, u8 reg, u16 val)
{
	int ret;

	ret = sn65dsi8x_write(dev, reg, val & 0xff);
	if (ret)
		return ret;

	return sn65dsi8x_write(dev, reg + 1, (val >> 8) & 0xff);
}

/*
 * Linux driver:
 *	 return (mode_clock_kHz - 12500) / 25000;
 *
 * U-Boot display_timing.pixelclock is in Hz.
 */
static u8 sn65dsi8x_get_lvds_range(const struct display_timing *timing)
{
	unsigned int pixelclock_khz = timing->pixelclock.typ / 1000U;

	return (pixelclock_khz - 12500U) / 25000U;
}

/*
 * DSI_CLK = pixel clock * bpp / lanes / 2
 *
 * CHA_DSI_CLK_RANGE encoding is the DSI clock in 5 MHz units,
 * clamped to 40..500 MHz.
 */
static u8 sn65dsi8x_get_dsi_range(struct sn65dsi8x_priv *priv,
				  const struct display_timing *timing)
{
	unsigned long long dsi_clk;
	unsigned int dsi_clk_khz;

	dsi_clk = (unsigned long long)timing->pixelclock.typ *
		  SN65DSI83_DSI_BPP;
	dsi_clk /= priv->dsi_lanes;
	dsi_clk /= 2;

	dsi_clk_khz = (unsigned int)(dsi_clk / 1000ULL);
	dsi_clk_khz = clamp(dsi_clk_khz, 40000U, 500000U);

	return DIV_ROUND_UP(dsi_clk_khz, 5000U);
}

/*
 * Divider = (DSI_CLK / LVDS_CLK) - 1
 *
 * For SN65DSI83 single-link LVDS:
 *	   divider = (bpp / lanes / 2) - 1
 */
static u8 sn65dsi8x_get_dsi_div(struct sn65dsi8x_priv *priv)
{
	unsigned int div = SN65DSI83_DSI_BPP;

	div /= priv->dsi_lanes;
	div /= 2;

	return div - 1;
}

static int sn65dsi8x_wait_pll_lock(struct udevice *dev)
{
	unsigned int timeout_us = 100000;
	u8 val;
	int ret;

	while (timeout_us) {
		ret = sn65dsi8x_read(dev, REG_RC_LVDS_PLL, &val);
		if (ret)
			return ret;

		if (val & REG_RC_LVDS_PLL_PLL_EN_STAT)
			return 0;

		udelay(1000);
		timeout_us -= 1000;
	}

	dev_err(dev, "PLL failed to lock\n");
	return -ETIMEDOUT;
}

static int sn65dsi8x_dump_id(struct udevice *dev)
{
	char id[10];
	int i, ret;
	u8 val;

	for (i = 0; i < 9; i++) {
		ret = sn65dsi8x_read(dev, REG_ID(i), &val);
		if (ret)
			return ret;

		id[i] = (val >= 0x20 && val <= 0x7e) ? val : '.';
	}

	id[9] = '\0';
	dev_info(dev, "ID: %s\n", id);

	return 0;
}

/*
 * Parse Linux-style:
 *
 * ports {
 *	   port@0 {
 *		   reg = <0>;
 *		   endpoint {
 *			   data-lanes = <1 2 3 4>;
 *		   };
 *	   };
 * };
 *
 * Fall back to 4 lanes if no data-lanes property is present.
 */
static unsigned int sn65dsi8x_get_dsi_lanes(struct udevice *dev)
{
	ofnode ports, port, ep;
	u32 lanes[4];
	int count, ret;

	ports = dev_read_subnode(dev, "ports");
	if (!ofnode_valid(ports))
		return 4;

	ofnode_for_each_subnode(port, ports) {
		u32 reg;

		ret = ofnode_read_u32(port, "reg", &reg);
		if (ret || reg != 0)
			continue;

		ofnode_for_each_subnode(ep, port) {
			count = ofnode_read_size(ep, "data-lanes") / sizeof(u32);
			if (count < 1 || count > 4)
				continue;

			ret = ofnode_read_u32_array(ep, "data-lanes",
						   lanes, count);
			if (!ret)
				return count;
		}
	}

	return 4;
}

static int sn65dsi8x_configure(struct udevice *dev,
				   const struct display_timing *timing)
{
	struct sn65dsi8x_priv *priv = dev_get_priv(dev);
	unsigned int hactive = timing->hactive.typ;
	unsigned int vactive = timing->vactive.typ;
	unsigned int hsync = timing->hsync_len.typ;
	unsigned int vsync = timing->vsync_len.typ;
	unsigned int hbp = timing->hback_porch.typ;
	unsigned int vbp = timing->vback_porch.typ;
	unsigned int hfp = timing->hfront_porch.typ;
	unsigned int vfp = timing->vfront_porch.typ;
	u8 val, irq;
	int ret;

	if (timing->pixelclock.typ < SN65DSI83_MIN_LVDS_CLK_HZ ||
		timing->pixelclock.typ > SN65DSI83_MAX_LVDS_CLK_HZ) {
		dev_err(dev, "unsupported LVDS pixel clock %u Hz\n",
			timing->pixelclock.typ);
		return -EINVAL;
	}

	if (priv->dsi_lanes < 1 || priv->dsi_lanes > 4) {
		dev_err(dev, "invalid DSI lane count %u\n", priv->dsi_lanes);
		return -EINVAL;
	}

	dev_info(dev,
		 "%ux%u pclk=%u Hz, lanes=%u, hfp=%u hsync=%u hbp=%u, vfp=%u vsync=%u vbp=%u\n",
		 hactive, vactive, timing->pixelclock.typ, priv->dsi_lanes,
		 hfp, hsync, hbp, vfp, vsync, vbp);

	/* Clear reset and disable PLL before programming CSR registers. */
	ret = sn65dsi8x_write(dev, REG_RC_RESET, 0x00);
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_RC_PLL_EN, 0x00);
	if (ret)
		return ret;

	/* Reference clock derived from DSI link clock. */
	val = REG_RC_LVDS_PLL_LVDS_CLK_RANGE(
			sn65dsi8x_get_lvds_range(timing)) |
		  REG_RC_LVDS_PLL_HS_CLK_SRC_DPHY;

	ret = sn65dsi8x_write(dev, REG_RC_LVDS_PLL, val);
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_DSI_CLK,
				  REG_DSI_CLK_CHA_DSI_CLK_RANGE(
					  sn65dsi8x_get_dsi_range(priv, timing)));
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_RC_DSI_CLK,
				  REG_RC_DSI_CLK_DSI_CLK_DIVIDER(
					  sn65dsi8x_get_dsi_div(priv)));
	if (ret)
		return ret;

	/*
	 * Linux uses ~(lanes - 1) because the SN65 register encoding is:
	 *
	 *	 00 -> 4 lanes
	 *	 01 -> 3 lanes
	 *	 10 -> 2 lanes
	 *	 11 -> 1 lane
	 */
	val = REG_DSI_LANE_DSI_CHANNEL_MODE_SINGLE |
		  REG_DSI_LANE_CHA_DSI_LANES(~(priv->dsi_lanes - 1)) |
		  REG_DSI_LANE_CHB_DSI_LANES(3);

	ret = sn65dsi8x_write(dev, REG_DSI_LANE, val);
	if (ret)
		return ret;

	/* No DSI equalization. */
	ret = sn65dsi8x_write(dev, REG_DSI_EQ, 0x00);
	if (ret)
		return ret;

	/* Sync polarity. */
	val = 0;

	if (timing->flags & DISPLAY_FLAGS_HSYNC_LOW)
		val |= REG_LVDS_FMT_HS_NEG_POLARITY;

	if (timing->flags & DISPLAY_FLAGS_VSYNC_LOW)
		val |= REG_LVDS_FMT_VS_NEG_POLARITY;

	/* SN65DSI83 is single-link LVDS, Channel A only. */
	val |= REG_LVDS_FMT_LVDS_LINK_CFG;

	/* Default to 24 bpp. */
	if (priv->lvds_format_24bpp)
		val |= REG_LVDS_FMT_CHA_24BPP_MODE;

	/* JEIDA = Format 1; SPWG/VESA = Format 2. */
	if (priv->lvds_format_jeida)
		val |= REG_LVDS_FMT_CHA_24BPP_FORMAT1;

	ret = sn65dsi8x_write(dev, REG_LVDS_FMT, val);
	if (ret)
		return ret;

	/* Preserve values used by the Linux driver. */
	ret = sn65dsi8x_write(dev, REG_LVDS_VCOM, 0x05);
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_LVDS_LANE,
				  REG_LVDS_LANE_CHA_LVDS_TERM |
				  REG_LVDS_LANE_CHB_LVDS_TERM);
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_LVDS_CM, 0x00);
	if (ret)
		return ret;

	/* Video timing registers are little-endian low-byte/high-byte pairs. */
	ret = sn65dsi8x_write16(dev, REG_VID_CHA_ACTIVE_LINE_LENGTH_LOW,
				hactive);
	if (ret)
		return ret;

	ret = sn65dsi8x_write16(dev, REG_VID_CHA_VERTICAL_DISPLAY_SIZE_LOW,
				vactive);
	if (ret)
		return ret;

	/* Linux driver uses 32 + 1 pixel clocks. */
	ret = sn65dsi8x_write16(dev, REG_VID_CHA_SYNC_DELAY_LOW, 33);
	if (ret)
		return ret;

	ret = sn65dsi8x_write16(dev, REG_VID_CHA_HSYNC_PULSE_WIDTH_LOW,
				hsync);
	if (ret)
		return ret;

	ret = sn65dsi8x_write16(dev, REG_VID_CHA_VSYNC_PULSE_WIDTH_LOW,
				vsync);
	if (ret)
		return ret;

	if (hbp > 0xff || vbp > 0xff || hfp > 0xff || vfp > 0xff) {
		dev_err(dev,
			"porch exceeds SN65DSI83 8-bit timing register range\n");
		return -EINVAL;
	}

	ret = sn65dsi8x_write(dev, REG_VID_CHA_HORIZONTAL_BACK_PORCH, hbp);
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_VID_CHA_VERTICAL_BACK_PORCH, vbp);
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_VID_CHA_HORIZONTAL_FRONT_PORCH, hfp);
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_VID_CHA_VERTICAL_FRONT_PORCH, vfp);
	if (ret)
		return ret;

	ret = sn65dsi8x_write(dev, REG_VID_CHA_TEST_PATTERN, 0x00);
	if (ret)
		return ret;

	/* Enable PLL. */
	ret = sn65dsi8x_write(dev, REG_RC_PLL_EN, REG_RC_PLL_EN_PLL_EN);
	if (ret)
		return ret;

	udelay(3000);

	ret = sn65dsi8x_wait_pll_lock(dev);
	if (ret) {
		sn65dsi8x_write(dev, REG_RC_PLL_EN, 0x00);
		return ret;
	}

	/* Trigger reset after CSR update. */
	ret = sn65dsi8x_write(dev, REG_RC_RESET,
				  REG_RC_RESET_SOFT_RESET);
	if (ret)
		return ret;

	/*
	 * Clear errors asserted during initialization.
	 * REG_IRQ_STAT is write-1-to-clear.
	 */
	ret = sn65dsi8x_read(dev, REG_IRQ_STAT, &irq);
	if (ret)
		return ret;

	if (irq) {
		ret = sn65dsi8x_write(dev, REG_IRQ_STAT, irq);
		if (ret)
			return ret;
	}

	mdelay(10);

	ret = sn65dsi8x_read(dev, REG_IRQ_STAT, &irq);
	if (ret)
		return ret;

	if (irq)
		dev_warn(dev, "unexpected link status 0x%02x\n", irq);

	priv->enabled = true;

	return 0;
}

static int sn65dsi8x_attach(struct udevice *dev)
{
	struct sn65dsi8x_priv *priv = dev_get_priv(dev);
	struct display_timing timing;
	struct udevice *next;
	int ret;

	/*
	 * EN low >= 10 ms, then high >= 10 ms before I2C accesses.
	 */
	if (dm_gpio_is_valid(&priv->enable_gpio)) {
		dm_gpio_set_value(&priv->enable_gpio, 0);
		mdelay(10);

		dm_gpio_set_value(&priv->enable_gpio, 1);
		mdelay(10);
	}

	ret = sn65dsi8x_dump_id(dev);
	if (ret)
		return ret;

	ret = video_link_get_display_timings(&timing);
	if (ret) {
		dev_err(dev, "failed to get display timing: %d\n", ret);
		return ret;
	}

	ret = sn65dsi8x_configure(dev, &timing);
	if (ret)
		return ret;

	/*
	 * If another bridge follows this one, attach it as part of the graph
	 * chain. A panel is enabled later through set_backlight().
	 */
	next = video_link_get_next_device(dev);
	if (next && device_get_uclass_id(next) == UCLASS_VIDEO_BRIDGE) {
		ret = video_bridge_attach(next);
		if (ret)
			return ret;
	}

	return 0;
}

static int sn65dsi8x_set_backlight(struct udevice *dev, int percent)
{
	struct udevice *next;
	int ret;

	next = video_link_get_next_device(dev);
	if (!next)
		return 0;

	if (device_get_uclass_id(next) == UCLASS_VIDEO_BRIDGE)
		return video_bridge_set_backlight(next, percent);

	if (device_get_uclass_id(next) == UCLASS_PANEL) {
		if (percent <= 0)
			return 0;

		ret = panel_enable_backlight(next);
		if (ret)
			dev_err(dev, "failed to enable downstream panel: %d\n",
				ret);
		return ret;
	}

	return 0;
}

static int sn65dsi8x_check_timing(struct udevice *dev,
				  struct display_timing *timing)
{
	if (timing->pixelclock.typ < SN65DSI83_MIN_LVDS_CLK_HZ ||
		timing->pixelclock.typ > SN65DSI83_MAX_LVDS_CLK_HZ) {
		dev_err(dev, "LVDS pixel clock %u Hz outside 25..154 MHz\n",
			timing->pixelclock.typ);
		return -EINVAL;
	}

	return 0;
}

static int sn65dsi8x_probe(struct udevice *dev)
{
	struct sn65dsi8x_priv *priv = dev_get_priv(dev);
	const char *mapping;
	int ret;

	/*
	 * Keep the bridge in reset while probing.
	 *
	 * Linux binding:
	 *	   enable-gpios = <... GPIO_ACTIVE_HIGH>;
	 */
	ret = gpio_request_by_name(dev, "enable-gpios", 0,
				   &priv->enable_gpio, GPIOD_IS_OUT);
	if (ret && ret != -ENOENT) {
		dev_err(dev, "failed to get enable GPIO: %d\n", ret);
		return ret;
	}

	if (!ret) {
		dm_gpio_set_value(&priv->enable_gpio, 0);
		mdelay(10);
	}

	priv->dsi_lanes = sn65dsi8x_get_dsi_lanes(dev);

	/*
	 * Defaults match the Linux driver's fallback:
	 *	   RGB888 SPWG/VESA 24-bit.
	 *
	 * Optional U-Boot DT property:
	 *
	 *	   ti,lvds-data-mapping = "jeida-24";
	 *
	 * Accepted:
	 *	   "spwg-24"
	 *	   "vesa-24"
	 *	   "jeida-24"
	 */
	priv->lvds_format_24bpp = true;
	priv->lvds_format_jeida = false;

	mapping = dev_read_string(dev, "ti,lvds-data-mapping");
	if (mapping) {
		if (!strcmp(mapping, "jeida-24"))
			priv->lvds_format_jeida = true;
		else if (!strcmp(mapping, "spwg-24") ||
			 !strcmp(mapping, "vesa-24"))
			priv->lvds_format_jeida = false;
		else {
			dev_warn(dev, "unknown LVDS mapping '%s', using SPWG24\n",
				 mapping);
		}
	}

	dev_info(dev, "DSI lanes=%u, LVDS=%s\n",
		priv->dsi_lanes,
		priv->lvds_format_jeida ? "JEIDA24" : "SPWG24");

	return 0;
}

static const struct video_bridge_ops sn65dsi8x_ops = {
	.attach = sn65dsi8x_attach,
	.set_backlight = sn65dsi8x_set_backlight,
	.check_timing = sn65dsi8x_check_timing,
	.get_dsi_config = sn65dsi8x_get_dsi_config,
};

static const struct udevice_id sn65dsi8x_ids[] = {
	{
		.compatible = "ti,sn65dsi83",
		.data = SN65DSI83,
	},
	{
		.compatible = "ti,sn65dsi84",
		.data = SN65DSI84,
	},
	{ }
};

U_BOOT_DRIVER(sn65dsi8x) = {
	.name = "sn65dsi8x",
	.id = UCLASS_VIDEO_BRIDGE,
	.of_match = sn65dsi8x_ids,
	.probe = sn65dsi8x_probe,
	.ops = &sn65dsi8x_ops,
	.priv_auto = sizeof(struct sn65dsi8x_priv),
};
