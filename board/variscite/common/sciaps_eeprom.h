/*
 * Copyright (C) 2018-2023 Variscite Ltd.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef _SCIAPS_EEPROM_H_
#define _SCIAPS_EEPROM_H_

#ifdef CONFIG_ARCH_IMX8M
#include <asm/arch-imx8m/ddr.h>
#endif

#define SCIAPS_EEPROM_MAGIC	0x5341 /* == HEX("SA") */

#define SCIAPS_EEPROM_I2C_BUS     1
#define SCIAPS_EEPROM_I2C_ADDR   0x54

#define SCIAPS_EEPROM_DISPLAY_OTD_35in				35
#define SCIAPS_EEPROM_DISPLAY_OTD_27in_Citrobits	27
#define SCIAPS_EEPROM_DISPLAY_NH_70in				70

#define SCIAPS_EEPROM_PLATFORM_BASE_MASK			0xf0
#define SCIAPS_EEPROM_PLATFORM_BASE_NIR				0x50
#define SCIAPS_EEPROM_PLATFORM_BASE_NGX				0x30
#define SCIAPS_EEPROM_PLATFORM_BASE_NGL				0x40
#define SCIAPS_EEPROM_PLATFORM_BASE_NGSR			0x60
#define SCIAPS_EEPROM_PLATFORM_BASE_CHECK(p, b) ((p & SCIAPS_EEPROM_PLATFORM_BASE_MASK) == b)

struct __attribute__((packed)) sciaps_eeprom {
	uint16_t	magic;
	uint8_t		platform;
	uint8_t		display;
};


static inline bool sciaps_eeprom_is_valid(struct sciaps_eeprom *ep)
{
	if (htons(ep->magic) != SCIAPS_EEPROM_MAGIC) {
		debug("Invalid EEPROM magic 0x%hx, expected 0x%hx\n",
			htons(ep->magic), SCIAPS_EEPROM_MAGIC);
		return false;
	}

	return true;
}

int sciaps_eeprom_read_header(struct sciaps_eeprom *e);
uint8_t sciaps_eeprom_get_display(void);
uint8_t sciaps_eeprom_get_platform(void);

#endif /* _SCIAPS_EEPROM_H_ */
