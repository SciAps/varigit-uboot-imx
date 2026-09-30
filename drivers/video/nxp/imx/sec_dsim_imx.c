// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2019 NXP
 *
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dsi_host.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <video.h>
#include <video_bridge.h>
#include <video_link.h>
#include <asm/io.h>
#include <asm/arch/gpio.h>
#include <dm/device-internal.h>
#include <linux/iopoll.h>
#include <linux/err.h>
#include <power/regulator.h>

struct imx_sec_dsim_priv {
	struct mipi_dsi_device device;
	void __iomem *base;
	struct udevice *panel;
	struct udevice *dsi_host;
	struct udevice *bridge;
	struct display_timing adj;
};

static int imx_sec_dsim_attach(struct udevice *dev)
{
	struct imx_sec_dsim_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_device *device = &priv->device;
	struct mipi_dsi_panel_plat *mplat;
	struct display_timing timings;
	struct udevice *next;
	int ret;

	next = video_link_get_next_device(dev);
	if (!next) {
		dev_err(dev, "no downstream device\n");
		return -ENODEV;
	}

	if (device_get_uclass_id(next) == UCLASS_VIDEO_BRIDGE) {
		priv->bridge = next;
		priv->panel = video_link_get_next_device(next);

		if (!priv->panel ||
			device_get_uclass_id(priv->panel) != UCLASS_PANEL) {
			dev_err(dev, "failed to get panel behind bridge\n");
			return -ENODEV;
		}

		dev_info(dev, "bridge %s -> panel %s\n", next->name, priv->panel->name);
	}
	else if (device_get_uclass_id(next) == UCLASS_PANEL) {
		priv->bridge = NULL;
		priv->panel = next;

	}
	else {
		dev_err(dev, "unsupported downstream device %s, uclass=%d\n", next->name, device_get_uclass_id(next));
		return -ENODEV;
	}

	mplat = dev_get_plat(priv->panel);
	mplat->device = &priv->device;

	ret = video_link_get_display_timings(&timings);
	if (ret) {
		dev_err(dev, "decode display timing error %d\n", ret);
		return ret;
	}

	priv->adj = timings;

	ret = uclass_get_device(UCLASS_DSI_HOST, 0, &priv->dsi_host);
	if (ret) {
		dev_err(dev, "No video dsi host detected %d\n", ret);
		return ret;
	}

	if (priv->bridge) {
		ret = video_bridge_get_dsi_config(priv->bridge, device);
		if (ret) {
			dev_err(dev, "failed to get DSI config from %s: %d\n", priv->bridge->name, ret);
			return ret;
		}
	}

	ret = dsi_host_init(priv->dsi_host, device, &timings, 4, NULL);
	if (ret) {
		dev_err(dev, "failed to initialize mipi dsi host\n");
		return ret;
	}

	if (priv->bridge) {
		ret = mipi_dsi_attach(device);
		if (ret) {
			dev_err(dev, "mipi_dsi_attach failed: %d\n", ret);
			return ret;
		}

		ret = video_bridge_attach(priv->bridge);
		if (ret) {
			dev_err(dev, "failed to attach downstream bridge %s: %d\n", priv->bridge->name, ret);
			return ret;
		}
	}
	return 0;
}

static int imx_sec_dsim_set_backlight(struct udevice *dev, int percent)
{
	struct imx_sec_dsim_priv *priv = dev_get_priv(dev);
	int ret;

	ret = panel_enable_backlight(priv->panel);
	if (ret) {
		dev_err(dev, "panel %s enable backlight error %d\n",
			priv->panel->name, ret);
		return ret;
	}

	ret = dsi_host_enable(priv->dsi_host);
	if (ret) {
		dev_err(dev, "failed to enable mipi dsi host\n");
		return ret;
	}

	return 0;
}

static int imx_sec_dsim_probe(struct udevice *dev)
{
	struct imx_sec_dsim_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_device *device = &priv->device;

	device->dev = dev;

	return 0;
}

static int imx_sec_dsim_remove(struct udevice *dev)
{
	struct imx_sec_dsim_priv *priv = dev_get_priv(dev);
	int ret;

	if (priv->panel)
		device_remove(priv->panel, DM_REMOVE_NORMAL);

	ret = dsi_host_disable(priv->dsi_host);
	if (ret) {
		dev_err(dev, "failed to enable mipi dsi host\n");
		return ret;
	}

	return 0;
}

static int imx_sec_dsim_check_timing(struct udevice *dev, struct display_timing *timing)
{
	struct imx_sec_dsim_priv *priv = dev_get_priv(dev);

	/* DSI force the Polarities as high */
	priv->adj.flags &= ~(DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW);
	priv->adj.flags |= DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH;

	*timing = priv->adj;

	return 0;
}

struct video_bridge_ops imx_sec_dsim_ops = {
	.attach = imx_sec_dsim_attach,
	.set_backlight = imx_sec_dsim_set_backlight,
	.check_timing = imx_sec_dsim_check_timing,
};

static const struct udevice_id imx_sec_dsim_ids[] = {
	{ .compatible = "fsl,imx8mm-mipi-dsim" },
	{ .compatible = "fsl,imx8mn-mipi-dsim" },
	{ .compatible = "fsl,imx8mp-mipi-dsim" },
	{ }
};

U_BOOT_DRIVER(imx_sec_dsim) = {
	.name				= "imx_sec_dsim",
	.id				= UCLASS_VIDEO_BRIDGE,
	.of_match			= imx_sec_dsim_ids,
	.bind				= dm_scan_fdt_dev,
	.remove				= imx_sec_dsim_remove,
	.probe				= imx_sec_dsim_probe,
	.ops				= &imx_sec_dsim_ops,
	.priv_auto		= sizeof(struct imx_sec_dsim_priv),
};
