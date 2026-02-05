/*
 * Copyright (C) 2018-2023 Variscite Ltd.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <dm.h>
#include <i2c.h>
#include <asm/io.h>
#include <cpu_func.h>
#include <u-boot/crc.h>

#ifdef CONFIG_ARCH_IMX8M
#include <asm/arch-imx8m/ddr.h>
#endif

#ifdef CONFIG_ARCH_IMX8
#include <asm/arch/sci/sci.h>
#endif

#include "sciaps_eeprom.h"

#if CONFIG_IS_ENABLED(DM_I2C)
static int	sciaps_eeprom_get_dev(struct udevice **devp)
{
	int ret;
	struct udevice *bus;

	ret = uclass_get_device_by_seq(UCLASS_I2C, SCIAPS_EEPROM_I2C_BUS, &bus);
	if (ret) {
		debug("%s: No EEPROM I2C bus %d\n", __func__, SCIAPS_EEPROM_I2C_BUS);
		return ret;
	}

	ret = dm_i2c_probe(bus, SCIAPS_EEPROM_I2C_ADDR, 0, devp);
	if (ret) {
		debug("%s: I2C EEPROM probe failed\n", __func__);
		return ret;
	}

	return 0;
}

int sciaps_eeprom_read_header(struct sciaps_eeprom *e)
{
	int ret;
	struct udevice *dev;

	ret = sciaps_eeprom_get_dev(&dev);
	if (ret) {
		debug("%s: Failed to detect I2C EEPROM\n", __func__);
		return ret;
	}

	/* Read EEPROM header to memory */
	ret = dm_i2c_read(dev, 0, (void *)e, sizeof(*e));
	if (ret) {
		debug("%s: EEPROM read failed, ret=%d\n", __func__, ret);
		return ret;
	}

	return 0;
}
#else
int sciaps_eeprom_read_header(struct sciaps_eeprom *e)
{
	int ret;
	/* Probe EEPROM */
	i2c_set_bus_num(SCIAPS_EEPROM_I2C_BUS);
	ret = i2c_probe(SCIAPS_EEPROM_I2C_ADDR);
	if (ret) {
		debug("%s: I2C EEPROM probe failed\n", __func__);
		return ret;
	}

	/* Read EEPROM header to memory */
	ret = i2c_read(SCIAPS_EEPROM_I2C_ADDR, 0, 1, (uint8_t *)e, sizeof(*e));
	if (ret) {
		debug("%s: EEPROM read failed ret=%d\n", __func__, ret);
		return ret;
	}

	return 0;
}
#endif /* CONFIG_DM_I2C */

static struct sciaps_eeprom s_sciaps_eeprom = {0xff};

uint8_t sciaps_eeprom_get_display(void)
{
	int ret;

	ret = sciaps_eeprom_read_header(&s_sciaps_eeprom);

    if (ret == 0) {
		if (sciaps_eeprom_is_valid(&s_sciaps_eeprom)) {
			return s_sciaps_eeprom.display;
		}
    } else {
        debug("I2C read failed: %d\n", ret);
    }

    return 0xff;
}
