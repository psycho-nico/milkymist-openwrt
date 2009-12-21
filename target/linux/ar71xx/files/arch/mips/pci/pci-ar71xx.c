/*
 *  Atheros AR71xx PCI host controller driver
 *
 *  Copyright (C) 2008-2009 Gabor Juhos <juhosg@openwrt.org>
 *  Copyright (C) 2008 Imre Kaloz <kaloz@openwrt.org>
 *
 *  Parts of this file are based on Atheros' 2.6.15 BSP
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include <linux/resource.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>

#include <asm/mach-ar71xx/ar71xx.h>
#include <asm/mach-ar71xx/pci.h>

#undef DEBUG
#ifdef DEBUG
#define DBG(fmt, args...)	printk(KERN_DEBUG fmt, ## args)
#else
#define DBG(fmt, args...)
#endif

#define AR71XX_PCI_DELAY	100 /* msecs */

#if 0
#define PCI_IDSEL_BASE	PCI_IDSEL_ADL_START
#else
#define PCI_IDSEL_BASE	0
#endif

static void __iomem *ar71xx_pcicfg_base;
static DEFINE_SPINLOCK(ar71xx_pci_lock);
static int ar71xx_pci_fixup_enable;

static inline void ar71xx_pci_delay(void)
{
	mdelay(AR71XX_PCI_DELAY);
}

static inline u32 ar71xx_pcicfg_rr(unsigned int reg)
{
	return __raw_readl(ar71xx_pcicfg_base + reg);
}

static inline void ar71xx_pcicfg_wr(unsigned int reg, u32 val)
{
	__raw_writel(val, ar71xx_pcicfg_base + reg);
}

/* Byte lane enable bits */
static u8 ble_table[4][4] = {
	{0x0, 0xf, 0xf, 0xf},
	{0xe, 0xd, 0xb, 0x7},
	{0xc, 0xf, 0x3, 0xf},
	{0xf, 0xf, 0xf, 0xf},
};

static inline u32 ar71xx_pci_get_ble(int where, int size, int local)
{
	u32 t;

	t = ble_table[size & 3][where & 3];
	BUG_ON(t == 0xf);
	t <<= (local) ? 20 : 4;
	return t;
}

static inline u32 ar71xx_pci_bus_addr(struct pci_bus *bus, unsigned int devfn,
					int where)
{
	u32 ret;

	if (!bus->number) {
		/* type 0 */
		ret = (1 << (PCI_IDSEL_BASE + PCI_SLOT(devfn)))
		    | (PCI_FUNC(devfn) << 8) | (where & ~3);
	} else {
		/* type 1 */
		ret = (bus->number << 16) | (PCI_SLOT(devfn) << 11)
		    | (PCI_FUNC(devfn) << 8) | (where & ~3) | 1;
	}

	return ret;
}

int ar71xx_pci_be_handler(int is_fixup)
{
	u32 pci_err;
	u32 ahb_err;

	pci_err = ar71xx_pcicfg_rr(PCI_REG_PCI_ERR) & 3;
	if (pci_err) {
		if (!is_fixup)
			printk(KERN_ALERT "PCI error %d at PCI addr 0x%x\n",
				pci_err,
				ar71xx_pcicfg_rr(PCI_REG_PCI_ERR_ADDR));

		ar71xx_pcicfg_wr(PCI_REG_PCI_ERR, pci_err);
	}

	ahb_err = ar71xx_pcicfg_rr(PCI_REG_AHB_ERR) & 1;
	if (ahb_err) {
		if (!is_fixup)
			printk(KERN_ALERT "AHB error at AHB address 0x%x\n",
				ar71xx_pcicfg_rr(PCI_REG_AHB_ERR_ADDR));

		ar71xx_pcicfg_wr(PCI_REG_AHB_ERR, ahb_err);
	}

	return ((ahb_err | pci_err) ? 1 : 0);
}

static inline int ar71xx_pci_set_cfgaddr(struct pci_bus *bus,
			unsigned int devfn, int where, int size, u32 cmd)
{
	u32 addr;

	addr = ar71xx_pci_bus_addr(bus, devfn, where);

	DBG("PCI: set cfgaddr: %02x:%02x.%01x/%02x:%01d, addr=%08x\n",
		bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn),
		where, size, addr);

	ar71xx_pcicfg_wr(PCI_REG_CFG_AD, addr);
	ar71xx_pcicfg_wr(PCI_REG_CFG_CBE,
			cmd | ar71xx_pci_get_ble(where, size, 0));

	return ar71xx_pci_be_handler(1);
}

static int ar71xx_pci_read_config(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *value)
{
	static u32 mask[8] = {0, 0xff, 0xffff, 0, 0xffffffff, 0, 0, 0};
	unsigned long flags;
	u32 data;
	int ret;

	ret = PCIBIOS_SUCCESSFUL;

	DBG("PCI: read config: %02x:%02x.%01x/%02x:%01d\n", bus->number,
			PCI_SLOT(devfn), PCI_FUNC(devfn), where, size);

	spin_lock_irqsave(&ar71xx_pci_lock, flags);

	if (bus->number == 0 && devfn == 0) {
		u32 t;

		t = PCI_CRP_CMD_READ | (where & ~3);

		ar71xx_pcicfg_wr(PCI_REG_CRP_AD_CBE, t);
		data = ar71xx_pcicfg_rr(PCI_REG_CRP_RDDATA);

		DBG("PCI: rd local cfg, ad_cbe:%08x, data:%08x\n", t, data);

	} else {
		int err;

		err = ar71xx_pci_set_cfgaddr(bus, devfn, where, size,
						PCI_CFG_CMD_READ);

		if (err == 0) {
			data = ar71xx_pcicfg_rr(PCI_REG_CFG_RDDATA);
		} else {
			ret = PCIBIOS_DEVICE_NOT_FOUND;
			data = ~0;
		}
	}

	spin_unlock_irqrestore(&ar71xx_pci_lock, flags);

	DBG("PCI: read config: data=%08x raw=%08x\n",
		(data >> (8 * (where & 3))) & mask[size & 7], data);

	*value = (data >> (8 * (where & 3))) & mask[size & 7];

	return ret;
}

static int ar71xx_pci_write_config(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 value)
{
	unsigned long flags;
	int ret;

	DBG("PCI: write config: %02x:%02x.%01x/%02x:%01d value=%08x\n",
		bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn),
		where, size, value);

	value = value << (8 * (where & 3));
	ret = PCIBIOS_SUCCESSFUL;

	spin_lock_irqsave(&ar71xx_pci_lock, flags);
	if (bus->number == 0 && devfn == 0) {
		u32 t;

		t = PCI_CRP_CMD_WRITE | (where & ~3);
		t |= ar71xx_pci_get_ble(where, size, 1);

		DBG("PCI: wr local cfg, ad_cbe:%08x, value:%08x\n", t, value);

		ar71xx_pcicfg_wr(PCI_REG_CRP_AD_CBE, t);
		ar71xx_pcicfg_wr(PCI_REG_CRP_WRDATA, value);
	} else {
		int err;

		err = ar71xx_pci_set_cfgaddr(bus, devfn, where, size,
						PCI_CFG_CMD_WRITE);

		if (err == 0)
			ar71xx_pcicfg_wr(PCI_REG_CFG_WRDATA, value);
		else
			ret = PCIBIOS_DEVICE_NOT_FOUND;
	}
	spin_unlock_irqrestore(&ar71xx_pci_lock, flags);

	return ret;
}

static void ar71xx_pci_fixup(struct pci_dev *dev)
{
	u32 t;

	if (!ar71xx_pci_fixup_enable)
		return;

	if (dev->bus->number != 0 || dev->devfn != 0)
		return;

	DBG("PCI: fixup host controller %s (%04x:%04x)\n", pci_name(dev),
		dev->vendor, dev->device);

	/* setup COMMAND register */
	t = PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INVALIDATE
	  | PCI_COMMAND_PARITY | PCI_COMMAND_SERR | PCI_COMMAND_FAST_BACK;

	pci_write_config_word(dev, PCI_COMMAND, t);
}
DECLARE_PCI_FIXUP_EARLY(PCI_ANY_ID, PCI_ANY_ID, ar71xx_pci_fixup);

int __init ar71xx_pcibios_map_irq(const struct pci_dev *dev, uint8_t slot,
				  uint8_t pin)
{
	int irq = -1;
	int i;

	slot -= PCI_IDSEL_ADL_START - PCI_IDSEL_BASE;

	for (i = 0; i < ar71xx_pci_nr_irqs; i++) {
		struct ar71xx_pci_irq *entry;

		entry = &ar71xx_pci_irq_map[i];
		if (entry->slot == slot && entry->pin == pin) {
			irq = entry->irq;
			break;
		}
	}

	if (irq < 0) {
		printk(KERN_ALERT "PCI: no irq found for pin%u@%s\n",
				pin, pci_name((struct pci_dev *)dev));
	} else {
		printk(KERN_INFO "PCI: mapping irq %d to pin%u@%s\n",
				irq, pin, pci_name((struct pci_dev *)dev));
	}

	return irq;
}

static struct pci_ops ar71xx_pci_ops = {
	.read	= ar71xx_pci_read_config,
	.write	= ar71xx_pci_write_config,
};

static struct resource ar71xx_pci_io_resource = {
	.name		= "PCI IO space",
	.start		= 0,
	.end		= 0,
	.flags		= IORESOURCE_IO,
};

static struct resource ar71xx_pci_mem_resource = {
	.name		= "PCI memory space",
	.start		= AR71XX_PCI_MEM_BASE,
	.end		= AR71XX_PCI_MEM_BASE + AR71XX_PCI_MEM_SIZE - 1,
	.flags		= IORESOURCE_MEM
};

static struct pci_controller ar71xx_pci_controller = {
	.pci_ops	= &ar71xx_pci_ops,
	.mem_resource	= &ar71xx_pci_mem_resource,
	.io_resource	= &ar71xx_pci_io_resource,
};

int __init ar71xx_pcibios_init(void)
{
	ar71xx_device_stop(RESET_MODULE_PCI_BUS | RESET_MODULE_PCI_CORE);
	ar71xx_pci_delay();

	ar71xx_device_start(RESET_MODULE_PCI_BUS | RESET_MODULE_PCI_CORE);
	ar71xx_pci_delay();

	ar71xx_pcicfg_base = ioremap_nocache(AR71XX_PCI_CFG_BASE,
						AR71XX_PCI_CFG_SIZE);

	ar71xx_ddr_wr(AR71XX_DDR_REG_PCI_WIN0, PCI_WIN0_OFFS);
	ar71xx_ddr_wr(AR71XX_DDR_REG_PCI_WIN1, PCI_WIN1_OFFS);
	ar71xx_ddr_wr(AR71XX_DDR_REG_PCI_WIN2, PCI_WIN2_OFFS);
	ar71xx_ddr_wr(AR71XX_DDR_REG_PCI_WIN3, PCI_WIN3_OFFS);
	ar71xx_ddr_wr(AR71XX_DDR_REG_PCI_WIN4, PCI_WIN4_OFFS);
	ar71xx_ddr_wr(AR71XX_DDR_REG_PCI_WIN5, PCI_WIN5_OFFS);
	ar71xx_ddr_wr(AR71XX_DDR_REG_PCI_WIN6, PCI_WIN6_OFFS);
	ar71xx_ddr_wr(AR71XX_DDR_REG_PCI_WIN7, PCI_WIN7_OFFS);

	ar71xx_pci_delay();

	/* clear bus errors */
	(void)ar71xx_pci_be_handler(1);

	ar71xx_pci_fixup_enable = 1;
	register_pci_controller(&ar71xx_pci_controller);

	return 0;
}
