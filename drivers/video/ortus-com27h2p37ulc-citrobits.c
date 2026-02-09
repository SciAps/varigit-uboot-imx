// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 SciAps - All Rights Reserved
 * Author(s): Andre Doudkin ,adoudkin@gmail.com>
 *            Philippe Cornu <philippe.cornu@st.com> for STMicroelectronics.
 *
 * This ortus-com27h2p37ulc citrobits panel driver is inspired from the Linux Kernel driver
 * drivers/gpu/drm/panel/panel-ortus-com27h2p37ulc-citrobits.c
 */

#include <common.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <asm/gpio.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <backlight.h>

/* Write Manufacture Command Set Control */
#define WRMAUCCTR 0xFE

#define PANEL_RESET_ENABLE 0

struct ortus_com27h2p37ulc_citrobits_panel_priv {
	struct udevice *backlight;
	struct gpio_desc reset;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
	unsigned long mode_flags;
};

struct ortus_com27h2p37ulc_citrobits_platform_data {
	int (*enable)(struct udevice *dev);
};
/*
	.clock			= 5941,					// kHz htotal * vtotal * 60 FPS
	.hdisplay		= 240,
	.hsync_start	= 240 + 17,				// active + front porch
	.hsync_end		= 240 + 17 + 10,		// active + front porch + sync
	.htotal			= 240 + 17 + 10 + 20,	// active + front porch + sync + back porch
	.vdisplay		= 320,
	.vsync_start	= 320 + 10,
	.vsync_end		= 320 + 10 + 5,
	.vtotal			= 320 + 10 + 5 + 10,
	.width_mm		= 43,
	.height_mm		= 56,
	.flags = DRM_MODE_FLAG_NHSYNC |
		 DRM_MODE_FLAG_NVSYNC,
*/
static const struct display_timing default_timing = {
	.pixelclock.typ		= 5941000,
	.hactive.typ		= 240,
	.hfront_porch.typ	= 17,
	.hsync_len.typ		= 10,
	.hback_porch.typ	= 20,
	.vactive.typ		= 320,
	.vfront_porch.typ	= 10,
	.vsync_len.typ		= 5,
	.vback_porch.typ	= 10,
	.flags = DISPLAY_FLAGS_HSYNC_LOW |
		 DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_HIGH |
		 DISPLAY_FLAGS_PIXDATA_NEGEDGE,
};

static int ortus_com27h2p37ulc_citrobits_enable(struct udevice *dev)
{
	struct ortus_com27h2p37ulc_citrobits_panel_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct mipi_dsi_device *dsi = plat->device;
	//u8 color_format = color_format_from_dsi_format(priv->format);
	u16 brightness;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	///* Select User Command Set table (CMD1) */
	//ret = mipi_dsi_generic_write(dsi, (u8[]){ WRMAUCCTR, 0x00 }, 2);
	//if (ret < 0)
	//	return -EIO;

	/* Software reset */
	ret = mipi_dsi_dcs_soft_reset(dsi);
	if (ret < 0) {
		printf("Failed to do Software Reset (%d)\n", ret);
		return -EIO;
	}

	/* Wait 80ms for panel out of reset */
	mdelay(80);
#if 0
	/* Set DSI mode */
	ret = mipi_dsi_generic_write(dsi, (u8[]){ 0xC2, 0x0B }, 2);
	if (ret < 0) {
		printf("Failed to set DSI mode (%d)\n", ret);
		return -EIO;
	}

	/* Set tear ON */
	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		printf("Failed to set tear ON (%d)\n", ret);
		return -EIO;
	}

	/* Set tear scanline */
	ret = mipi_dsi_dcs_set_tear_scanline(dsi, 0x380);
	if (ret < 0) {
		printf("Failed to set tear scanline (%d)\n", ret);
		return -EIO;
	}

	/* Set pixel format */
	ret = mipi_dsi_dcs_set_pixel_format(dsi, color_format);
	if (ret < 0) {
		printf("Failed to set pixel format (%d)\n", ret);
		return -EIO;
	}
#endif

	if (priv->backlight) {
		ret = backlight_enable(priv->backlight);
		if (ret)
			return ret;
	}

	/* Set display brightness */
	brightness = 255; /* Max brightness */
	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, &brightness, 2);
	if (ret < 0) {
		printf("Failed to set display brightness (%d)\n",
				  ret);
		return -EIO;
	}


	/* Exit sleep mode */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		printf("Failed to exit sleep mode (%d)\n", ret);
		return -EIO;
	}

	mdelay(5);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		printf("Failed to set display ON (%d)\n", ret);
		return -EIO;
	}

	return 0;
}

static int ortus_com27h2p37ulc_citrobits_panel_enable_backlight(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct ortus_com27h2p37ulc_citrobits_platform_data *data = (struct ortus_com27h2p37ulc_citrobits_platform_data *)dev_get_driver_data(dev);
	struct mipi_dsi_device *device = plat->device;
	int ret;

	ret = mipi_dsi_attach(device);
	if (ret < 0)
		return ret;

	return data->enable(dev);
}

static int ortus_com27h2p37ulc_citrobits_panel_get_display_timing(struct udevice *dev,
					    struct display_timing *timings)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct mipi_dsi_device *device = plat->device;
	struct ortus_com27h2p37ulc_citrobits_panel_priv *priv = dev_get_priv(dev);

	memcpy(timings, &default_timing, sizeof(*timings));

	/* fill characteristics of DSI data link */
	if (device) {
		device->lanes = priv->lanes;
		device->format = priv->format;
		device->mode_flags = priv->mode_flags;
	}

	return 0;
}

static int ortus_com27h2p37ulc_citrobits_panel_probe(struct udevice *dev)
{
	struct ortus_com27h2p37ulc_citrobits_panel_priv *priv = dev_get_priv(dev);
	int ret;
	u32 video_mode;

	priv->format = MIPI_DSI_FMT_RGB888;
	priv->mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO;

	ret = uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev,
						   "backlight", &priv->backlight);
	if (ret) {
		printf("%s: Cannot get backlight: ret=%d\n", __func__, ret);
		if (ret != -ENOENT)
			return log_ret(ret);

		priv->backlight = NULL;
	}

	ret = dev_read_u32(dev, "video-mode", &video_mode);
	if (!ret) {
		switch (video_mode) {
		case 0:
			/* burst mode */
			priv->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST;
			break;
		case 1:
			/* non-burst mode with sync event */
			break;
		case 2:
			/* non-burst mode with sync pulse */
			priv->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
			break;
		default:
			dev_warn(dev, "invalid video mode %d\n", video_mode);
			break;
		}
	}

	ret = dev_read_u32(dev, "dsi-lanes", &priv->lanes);
	if (ret) {
		printf("Failed to get dsi-lanes property (%d)\n", ret);
		return ret;
	}

	ret = gpio_request_by_name(dev, "reset-gpio", 0, &priv->reset,
				   GPIOD_IS_OUT);
	if (ret) {
		printf("Warning: cannot get reset GPIO\n");
		if (ret != -ENOENT)
			return ret;
	}

#if PANEL_RESET_ENABLE
	/* reset panel */
	ret = dm_gpio_set_value(&priv->reset, true);
	if (ret)
		printf("reset gpio fails to set true\n");
	mdelay(100);
#endif
	ret = dm_gpio_set_value(&priv->reset, false);
	if (ret)
		printf("reset gpio fails to set true\n");

#if PANEL_RESET_ENABLE
	mdelay(100);
#endif

	return 0;
}

static int ortus_com27h2p37ulc_citrobits_panel_disable(struct udevice *dev)
{
#if PANEL_RESET_ENABLE
	struct ortus_com27h2p37ulc_citrobits_panel_priv *priv = dev_get_priv(dev);

	printf("----> %s: Enter\n", __func__);
	dm_gpio_set_value(&priv->reset, true);
#else
	(void)dev;
#endif

	return 0;
}

static const struct panel_ops ortus_com27h2p37ulc_citrobits_panel_ops = {
	.enable_backlight = ortus_com27h2p37ulc_citrobits_panel_enable_backlight,
	.get_display_timing = ortus_com27h2p37ulc_citrobits_panel_get_display_timing,
};

static const struct ortus_com27h2p37ulc_citrobits_platform_data pd_ortus_com27h2p37ulc_citrobits = {
	.enable = &ortus_com27h2p37ulc_citrobits_enable,
};

static const struct udevice_id ortus_com27h2p37ulc_citrobits_panel_ids[] = {
	{ .compatible = "ortus,com27h2p37ulc-citrobits", .data = (ulong)&pd_ortus_com27h2p37ulc_citrobits },
	{ }
};

U_BOOT_DRIVER(ortus_com27h2p37ulc_citrobits_panel) = {
	.name			  = "ortus_com27h2p37ulc_citrobits_panel",
	.id				  = UCLASS_PANEL,
	.of_match		  = ortus_com27h2p37ulc_citrobits_panel_ids,
	.ops			  = &ortus_com27h2p37ulc_citrobits_panel_ops,
	.probe			  = ortus_com27h2p37ulc_citrobits_panel_probe,
	.remove			  = ortus_com27h2p37ulc_citrobits_panel_disable,
	.plat_auto = sizeof(struct mipi_dsi_panel_plat),
	.priv_auto = sizeof(struct ortus_com27h2p37ulc_citrobits_panel_priv),
};
