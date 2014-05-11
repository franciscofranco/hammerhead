/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/ratelimit.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/qpnp-misc.h>
#include <linux/usb/msm_hsusb.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/slimport.h>

#include <mach/rpm-regulator.h>
#include <mach/rpm-regulator-smd.h>
#include <mach/msm_bus.h>
#include <mach/clk.h>

#include "dwc3_otg.h"
#include "core.h"
#include "gadget.h"

/* ADC threshold values */
static int adc_low_threshold = 700;
module_param(adc_low_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_low_threshold, "ADC ID Low voltage threshold");

static int adc_high_threshold = 950;
module_param(adc_high_threshold, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_high_threshold, "ADC ID High voltage threshold");

static int adc_meas_interval = ADC_MEAS1_INTERVAL_1S;
module_param(adc_meas_interval, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(adc_meas_interval, "ADC ID polling period");

static int override_phy_init;
module_param(override_phy_init, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(override_phy_init, "Override HSPHY Init Seq");

static int override_phy_host_init;
module_param(override_phy_host_init, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(override_phy_host_init, "Override HSPHY HOST Init Seq");

/* Enable Proprietary charger detection */
static bool prop_chg_detect = true;
module_param(prop_chg_detect, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(prop_chg_detect, "Enable Proprietary charger detection");

/**
 *  USB DBM Hardware registers.
 *
 */
#define DBM_BASE		0x000F8000
#define DBM_EP_CFG(n)		(DBM_BASE + (0x00 + 4 * (n)))
#define DBM_DATA_FIFO(n)	(DBM_BASE + (0x10 + 4 * (n)))
#define DBM_DATA_FIFO_SIZE(n)	(DBM_BASE + (0x20 + 4 * (n)))
#define DBM_DATA_FIFO_EN	(DBM_BASE + (0x30))
#define DBM_GEVNTADR		(DBM_BASE + (0x34))
#define DBM_GEVNTSIZ		(DBM_BASE + (0x38))
#define DBM_DBG_CNFG		(DBM_BASE + (0x3C))
#define DBM_HW_TRB0_EP(n)	(DBM_BASE + (0x40 + 4 * (n)))
#define DBM_HW_TRB1_EP(n)	(DBM_BASE + (0x50 + 4 * (n)))
#define DBM_HW_TRB2_EP(n)	(DBM_BASE + (0x60 + 4 * (n)))
#define DBM_HW_TRB3_EP(n)	(DBM_BASE + (0x70 + 4 * (n)))
#define DBM_PIPE_CFG		(DBM_BASE + (0x80))
#define DBM_SOFT_RESET		(DBM_BASE + (0x84))
#define DBM_GEN_CFG		(DBM_BASE + (0x88))

/**
 *  USB DBM  Hardware registers bitmask.
 *
 */
/* DBM_EP_CFG */
#define DBM_EN_EP		0x00000001
#define USB3_EPNUM		0x0000003E
#define DBM_BAM_PIPE_NUM	0x000000C0
#define DBM_PRODUCER		0x00000100
#define DBM_DISABLE_WB		0x00000200
#define DBM_INT_RAM_ACC		0x00000400

/* DBM_DATA_FIFO_SIZE */
#define DBM_DATA_FIFO_SIZE_MASK	0x0000ffff

/* DBM_GEVNTSIZ */
#define DBM_GEVNTSIZ_MASK	0x0000ffff

/* DBM_DBG_CNFG */
#define DBM_ENABLE_IOC_MASK	0x0000000f

/* DBM_SOFT_RESET */
#define DBM_SFT_RST_EP0		0x00000001
#define DBM_SFT_RST_EP1		0x00000002
#define DBM_SFT_RST_EP2		0x00000004
#define DBM_SFT_RST_EP3		0x00000008
#define DBM_SFT_RST_EPS_MASK	0x0000000F
#define DBM_SFT_RST_MASK	0x80000000
#define DBM_EN_MASK		0x00000002

#define DBM_MAX_EPS		4

/* DBM TRB configurations */
#define DBM_TRB_BIT		0x80000000
#define DBM_TRB_DATA_SRC	0x40000000
#define DBM_TRB_DMA		0x20000000
#define DBM_TRB_EP_NUM(ep)	(ep<<24)

#define USB3_PORTSC		(0x430)
#define PORT_PE			(0x1 << 1)
/**
 *  USB QSCRATCH Hardware registers
 *
 */
#define QSCRATCH_REG_OFFSET	(0x000F8800)
#define QSCRATCH_GENERAL_CFG	(QSCRATCH_REG_OFFSET + 0x08)
#define HS_PHY_CTRL_REG		(QSCRATCH_REG_OFFSET + 0x10)
#define PARAMETER_OVERRIDE_X_REG (QSCRATCH_REG_OFFSET + 0x14)
#define CHARGING_DET_CTRL_REG	(QSCRATCH_REG_OFFSET + 0x18)
#define CHARGING_DET_OUTPUT_REG	(QSCRATCH_REG_OFFSET + 0x1C)
#define ALT_INTERRUPT_EN_REG	(QSCRATCH_REG_OFFSET + 0x20)
#define HS_PHY_IRQ_STAT_REG	(QSCRATCH_REG_OFFSET + 0x24)
#define CGCTL_REG		(QSCRATCH_REG_OFFSET + 0x28)
#define SS_PHY_CTRL_REG		(QSCRATCH_REG_OFFSET + 0x30)
#define SS_PHY_PARAM_CTRL_1	(QSCRATCH_REG_OFFSET + 0x34)
#define SS_PHY_PARAM_CTRL_2	(QSCRATCH_REG_OFFSET + 0x38)
#define SS_CR_PROTOCOL_DATA_IN_REG  (QSCRATCH_REG_OFFSET + 0x3C)
#define SS_CR_PROTOCOL_DATA_OUT_REG (QSCRATCH_REG_OFFSET + 0x40)
#define SS_CR_PROTOCOL_CAP_ADDR_REG (QSCRATCH_REG_OFFSET + 0x44)
#define SS_CR_PROTOCOL_CAP_DATA_REG (QSCRATCH_REG_OFFSET + 0x48)
#define SS_CR_PROTOCOL_READ_REG     (QSCRATCH_REG_OFFSET + 0x4C)
#define SS_CR_PROTOCOL_WRITE_REG    (QSCRATCH_REG_OFFSET + 0x50)

struct dwc3_msm_req_complete {
	struct list_head list_item;
	struct usb_request *req;
	void (*orig_complete)(struct usb_ep *ep,
			      struct usb_request *req);
};

struct dwc3_msm {
	struct device *dev;
	void __iomem *base;
	u32 resource_size;
	int dbm_num_eps;
	u8 ep_num_mapping[DBM_MAX_EPS];
	const struct usb_ep_ops *original_ep_ops[DWC3_ENDPOINTS_NUM];
	struct list_head req_complete_list;
	struct clk		*xo_clk;
	struct clk		*ref_clk;
	struct clk		*core_clk;
	struct clk		*iface_clk;
	struct clk		*sleep_clk;
	struct clk		*hsphy_sleep_clk;
	struct clk		*utmi_clk;
	struct regulator	*hsusb_3p3;
	struct regulator	*hsusb_1p8;
	struct regulator	*hsusb_vddcx;
	struct regulator	*ssusb_1p8;
	struct regulator	*ssusb_vddcx;
	struct regulator	*dwc3_gdsc;

	/* VBUS regulator if no OTG and running in host only mode */
	struct regulator	*vbus_otg;
	struct dwc3_ext_xceiv	ext_xceiv;
	bool			resume_pending;
	atomic_t                pm_suspended;
	atomic_t		in_lpm;
	int			hs_phy_irq;
	unsigned long		dwc3_irq_enabled;
	int			hsphy_init_seq;
	int			hsphy_host_init_seq;
	struct delayed_work	resume_work;
	struct work_struct	restart_usb_work;
	struct dwc3_charger	charger;
	struct usb_phy		*otg_xceiv;
	struct delayed_work	chg_work;
	enum usb_chg_state	chg_state;
	int			pmic_id_irq;
	struct work_struct	id_work;
	struct qpnp_adc_tm_btm_param	adc_param;
	struct delayed_work	init_adc_work;
	bool			id_adc_detect;
	u8			dcd_retries;
	u32			bus_perf_client;
	struct msm_bus_scale_pdata	*bus_scale_table;
	struct power_supply	usb_psy;
	struct power_supply	*ext_vbus_psy;
	unsigned int		online;
	unsigned int		host_mode;
	unsigned int		current_max;
	unsigned int		vdd_no_vol_level;
	unsigned int		vdd_low_vol_level;
	unsigned int		vdd_high_vol_level;
	bool			vbus_active;
	bool			ext_inuse;
	enum dwc3_id_state	id_state;
	unsigned long		lpm_flags;
#define MDWC3_CORECLK_OFF		BIT(0)
#define MDWC3_TCXO_SHUTDOWN		BIT(1)
};

#define USB_HSPHY_3P3_VOL_MIN		3050000 /* uV */
#define USB_HSPHY_3P3_VOL_MAX		3300000 /* uV */
#define USB_HSPHY_3P3_HPM_LOAD		16000	/* uA */

#define USB_HSPHY_1P8_VOL_MIN		1800000 /* uV */
#define USB_HSPHY_1P8_VOL_MAX		1800000 /* uV */
#define USB_HSPHY_1P8_HPM_LOAD		19000	/* uA */

#define USB_SSPHY_1P8_VOL_MIN		1800000 /* uV */
#define USB_SSPHY_1P8_VOL_MAX		1800000 /* uV */
#define USB_SSPHY_1P8_HPM_LOAD		23000	/* uA */

static struct dwc3_msm *context;

static struct usb_ext_notification *usb_ext;

/**
 *
 * Read register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 *
 * @return u32
 */
static inline u32 dwc3_msm_read_reg(void *base, u32 offset)
{
	u32 val = ioread32(base + offset);
	return val;
}

/**
 * Read register masked field with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask.
 *
 * @return u32
 */
static inline u32 dwc3_msm_read_reg_field(void *base,
					  u32 offset,
					  const u32 mask)
{
	u32 shift = find_first_bit((void *)&mask, 32);
	u32 val = ioread32(base + offset);
	val &= mask;		/* clear other bits */
	val >>= shift;
	return val;
}

/**
 *
 * Write register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_reg(void *base, u32 offset, u32 val)
{
	iowrite32(val, base + offset);
}

/**
 * Write register masked field with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask.
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_reg_field(void *base, u32 offset,
					    const u32 mask, u32 val)
{
	u32 shift = find_first_bit((void *)&mask, 32);
	u32 tmp = ioread32(base + offset);

	tmp &= ~mask;		/* clear written bits */
	val = tmp | (val << shift);
	iowrite32(val, base + offset);
}

/**
 * Write register and read back masked value to confirm it is written
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask specifying what should be updated
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_readback(void *base, u32 offset,
					    const u32 mask, u32 val)
{
	u32 write_val, tmp = ioread32(base + offset);

	tmp &= ~mask;		/* retain other bits */
	write_val = tmp | val;

	iowrite32(write_val, base + offset);

	/* Read back to see if val was written */
	tmp = ioread32(base + offset);
	tmp &= mask;		/* clear other bits */

	if (tmp != val)
		dev_err(context->dev, "%s: write: %x to QSCRATCH: %x FAILED\n",
						__func__, val, offset);
}

/**
 *
 * Write SSPHY register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @addr - SSPHY address to write.
 * @val - value to write.
 *
 */
static void dwc3_msm_ssusb_write_phycreg(void *base, u32 addr, u32 val)
{
	iowrite32(addr, base + SS_CR_PROTOCOL_DATA_IN_REG);
	iowrite32(0x1, base + SS_CR_PROTOCOL_CAP_ADDR_REG);
	while (ioread32(base + SS_CR_PROTOCOL_CAP_ADDR_REG))
		cpu_relax();

	iowrite32(val, base + SS_CR_PROTOCOL_DATA_IN_REG);
	iowrite32(0x1, base + SS_CR_PROTOCOL_CAP_DATA_REG);
	while (ioread32(base + SS_CR_PROTOCOL_CAP_DATA_REG))
		cpu_relax();

	iowrite32(0x1, base + SS_CR_PROTOCOL_WRITE_REG);
	while (ioread32(base + SS_CR_PROTOCOL_WRITE_REG))
		cpu_relax();
}

/**
 *
 * Read SSPHY register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @addr - SSPHY address to read.
 *
 */
static u32 dwc3_msm_ssusb_read_phycreg(void *base, u32 addr)
{
	iowrite32(addr, base + SS_CR_PROTOCOL_DATA_IN_REG);
	iowrite32(0x1, base + SS_CR_PROTOCOL_CAP_ADDR_REG);
	while (ioread32(base + SS_CR_PROTOCOL_CAP_ADDR_REG))
		cpu_relax();

	iowrite32(0x1, base + SS_CR_PROTOCOL_READ_REG);
	while (ioread32(base + SS_CR_PROTOCOL_READ_REG))
		cpu_relax();

	return ioread32(base + SS_CR_PROTOCOL_DATA_OUT_REG);
}

/**
 * Return DBM EP number according to usb endpoint number.
 *
 */
static int dwc3_msm_find_matching_dbm_ep(struct dwc3_msm *mdwc, u8 usb_ep)
{
	int i;

	for (i = 0; i < mdwc->dbm_num_eps; i++)
		if (mdwc->ep_num_mapping[i] == usb_ep)
			return i;

	return -ENODEV; /* Not found */
}

/**
 * Return number of configured DBM endpoints.
 *
 */
static int dwc3_msm_configured_dbm_ep_num(struct dwc3_msm *mdwc)
{
	int i;
	int count = 0;

	for (i = 0; i < mdwc->dbm_num_eps; i++)
		if (mdwc->ep_num_mapping[i])
			count++;

	return count;
}

/**
 * Configure the DBM with the USB3 core event buffer.
 * This function is called by the SNPS UDC upon initialization.
 *
 * @addr - address of the event buffer.
 * @size - size of the event buffer.
 *
 */
static int dwc3_msm_event_buffer_config(struct dwc3_msm *mdwc,
					u32 addr, u16 size)
{
	dev_dbg(mdwc->dev, "%s\n", __func__);

	dwc3_msm_write_reg(mdwc->base, DBM_GEVNTADR, addr);
	dwc3_msm_write_reg_field(mdwc->base, DBM_GEVNTSIZ,
		DBM_GEVNTSIZ_MASK, size);

	return 0;
}

/**
 * Reset the DBM registers upon initialization.
 *
 */
static int dwc3_msm_dbm_soft_reset(struct dwc3_msm *mdwc, int enter_reset)
{
	dev_dbg(mdwc->dev, "%s\n", __func__);
	if (enter_reset) {
		dev_dbg(mdwc->dev, "enter DBM reset\n");
		dwc3_msm_write_reg_field(mdwc->base, DBM_SOFT_RESET,
			DBM_SFT_RST_MASK, 1);
	} else {
		dev_dbg(mdwc->dev, "exit DBM reset\n");
		dwc3_msm_write_reg_field(mdwc->base, DBM_SOFT_RESET,
			DBM_SFT_RST_MASK, 0);
		/*enable DBM*/
		dwc3_msm_write_reg_field(mdwc->base, QSCRATCH_GENERAL_CFG,
			DBM_EN_MASK, 0x1);
	}

	return 0;
}

/**
 * Soft reset specific DBM ep.
 * This function is called by the function driver upon events
 * such as transfer aborting, USB re-enumeration and USB
 * disconnection.
 *
 * @dbm_ep - DBM ep number.
 * @enter_reset - should we enter a reset state or get out of it.
 *
 */
static int dwc3_msm_dbm_ep_soft_reset(struct dwc3_msm *mdwc,
					u8 dbm_ep, bool enter_reset)
{
	dev_dbg(mdwc->dev, "%s\n", __func__);

	if (dbm_ep >= mdwc->dbm_num_eps) {
		dev_err(mdwc->dev, "%s: Invalid DBM ep index\n", __func__);
		return -ENODEV;
	}

	if (enter_reset) {
		dwc3_msm_write_reg_field(mdwc->base, DBM_SOFT_RESET,
			DBM_SFT_RST_EPS_MASK & 1 << dbm_ep, 1);
	} else {
		dwc3_msm_write_reg_field(mdwc->base, DBM_SOFT_RESET,
			DBM_SFT_RST_EPS_MASK & 1 << dbm_ep, 0);
	}

	return 0;
}

/**
 * Configure a USB DBM ep to work in BAM mode.
 *
 *
 * @usb_ep - USB physical EP number.
 * @producer - producer/consumer.
 * @disable_wb - disable write back to system memory.
 * @internal_mem - use internal USB memory for data fifo.
 * @ioc - enable interrupt on completion.
 *
 * @return int - DBM ep number.
 */
static int dwc3_msm_dbm_ep_config(struct dwc3_msm *mdwc, u8 usb_ep, u8 bam_pipe,
				  bool producer, bool disable_wb,
				  bool internal_mem, bool ioc)
{
	u8 dbm_ep;
	u32 ep_cfg;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	dbm_ep = dwc3_msm_find_matching_dbm_ep(mdwc, usb_ep);

	if (dbm_ep < 0) {
		dev_err(mdwc->dev,
				"%s: Invalid usb ep index\n", __func__);
		return -ENODEV;
	}
	/* First, reset the dbm endpoint */
	dwc3_msm_dbm_ep_soft_reset(mdwc, dbm_ep, 0);

	/* Set ioc bit for dbm_ep if needed */
	dwc3_msm_write_reg_field(mdwc->base, DBM_DBG_CNFG,
		DBM_ENABLE_IOC_MASK & 1 << dbm_ep, ioc ? 1 : 0);

	ep_cfg = (producer ? DBM_PRODUCER : 0) |
		(disable_wb ? DBM_DISABLE_WB : 0) |
		(internal_mem ? DBM_INT_RAM_ACC : 0);

	dwc3_msm_write_reg_field(mdwc->base, DBM_EP_CFG(dbm_ep),
		DBM_PRODUCER | DBM_DISABLE_WB | DBM_INT_RAM_ACC, ep_cfg >> 8);

	dwc3_msm_write_reg_field(mdwc->base, DBM_EP_CFG(dbm_ep), USB3_EPNUM,
		usb_ep);
	dwc3_msm_write_reg_field(mdwc->base, DBM_EP_CFG(dbm_ep),
		DBM_BAM_PIPE_NUM, bam_pipe);
	dwc3_msm_write_reg_field(mdwc->base, DBM_PIPE_CFG, 0x000000ff,
		0xe4);
	dwc3_msm_write_reg_field(mdwc->base, DBM_EP_CFG(dbm_ep), DBM_EN_EP,
		1);

	return dbm_ep;
}

/**
 * Configure a USB DBM ep to work in normal mode.
 *
 * @usb_ep - USB ep number.
 *
 */
static int dwc3_msm_dbm_ep_unconfig(struct dwc3_msm *mdwc, u8 usb_ep)
{
	u8 dbm_ep;
	u32 data;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	dbm_ep = dwc3_msm_find_matching_dbm_ep(mdwc, usb_ep);

	if (dbm_ep < 0) {
		dev_err(mdwc->dev, "%s: Invalid usb ep index\n", __func__);
		return -ENODEV;
	}

	mdwc->ep_num_mapping[dbm_ep] = 0;

	data = dwc3_msm_read_reg(mdwc->base, DBM_EP_CFG(dbm_ep));
	data &= (~0x1);
	dwc3_msm_write_reg(mdwc->base, DBM_EP_CFG(dbm_ep), data);

	/* Reset the dbm endpoint */
	dwc3_msm_dbm_ep_soft_reset(mdwc, dbm_ep, true);
	/*
	 * 10 usec delay is required before deasserting DBM endpoint reset
	 * according to hardware programming guide.
	 */
	udelay(10);
	dwc3_msm_dbm_ep_soft_reset(mdwc, dbm_ep, false);

	return 0;
}

/**
 * Configure the DBM with the BAM's data fifo.
 * This function is called by the USB BAM Driver
 * upon initialization.
 *
 * @ep - pointer to usb endpoint.
 * @addr - address of data fifo.
 * @size - size of data fifo.
 *
 */
int msm_data_fifo_config(struct usb_ep *ep, u32 addr, u32 size, u8 dst_pipe_idx)
{
	u8 dbm_ep;
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	u8 bam_pipe = dst_pipe_idx;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	dbm_ep = bam_pipe;
	mdwc->ep_num_mapping[dbm_ep] = dep->number;

	dwc3_msm_write_reg(mdwc->base, DBM_DATA_FIFO(dbm_ep), addr);
	dwc3_msm_write_reg_field(mdwc->base, DBM_DATA_FIFO_SIZE(dbm_ep),
		DBM_DATA_FIFO_SIZE_MASK, size);

	return 0;
}

/**
* Cleanups for msm endpoint on request complete.
*
* Also call original request complete.
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to usb_request instance.
*
* @return int - 0 on success, negetive on error.
*/
static void dwc3_msm_req_complete_func(struct usb_ep *ep,
				       struct usb_request *request)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_msm_req_complete *req_complete = NULL;

	/* Find original request complete function and remove it from list */
	list_for_each_entry(req_complete, &mdwc->req_complete_list, list_item) {
		if (req_complete->req == request)
			break;
	}
	if (!req_complete || req_complete->req != request) {
		dev_err(dep->dwc->dev, "%s: could not find the request\n",
					__func__);
		return;
	}
	list_del(&req_complete->list_item);

	/*
	 * Release another one TRB to the pool since DBM queue took 2 TRBs
	 * (normal and link), and the dwc3/gadget.c :: dwc3_gadget_giveback
	 * released only one.
	 */
	dep->busy_slot++;

	/* Unconfigure dbm ep */
	dwc3_msm_dbm_ep_unconfig(mdwc, dep->number);

	/*
	 * If this is the last endpoint we unconfigured, than reset also
	 * the event buffers.
	 */
	if (0 == dwc3_msm_configured_dbm_ep_num(mdwc))
		dwc3_msm_event_buffer_config(mdwc, 0, 0);

	/*
	 * Call original complete function, notice that dwc->lock is already
	 * taken by the caller of this function (dwc3_gadget_giveback()).
	 */
	request->complete = req_complete->orig_complete;
	if (request->complete)
		request->complete(ep, request);

	kfree(req_complete);
}

/**
* Helper function.
* See the header of the dwc3_msm_ep_queue function.
*
* @dwc3_ep - pointer to dwc3_ep instance.
* @req - pointer to dwc3_request instance.
*
* @return int - 0 on success, negetive on error.
*/
static int __dwc3_msm_ep_queue(struct dwc3_ep *dep, struct dwc3_request *req)
{
	struct dwc3_trb *trb;
	struct dwc3_trb *trb_link;
	struct dwc3_gadget_ep_cmd_params params;
	u32 cmd;
	int ret = 0;

	/* We push the request to the dep->req_queued list to indicate that
	 * this request is issued with start transfer. The request will be out
	 * from this list in 2 cases. The first is that the transfer will be
	 * completed (not if the transfer is endless using a circular TRBs with
	 * with link TRB). The second case is an option to do stop stransfer,
	 * this can be initiated by the function driver when calling dequeue.
	 */
	req->queued = true;
	list_add_tail(&req->list, &dep->req_queued);

	/* First, prepare a normal TRB, point to the fake buffer */
	trb = &dep->trb_pool[dep->free_slot & DWC3_TRB_MASK];
	dep->free_slot++;
	memset(trb, 0, sizeof(*trb));

	req->trb = trb;
	trb->bph = DBM_TRB_BIT | DBM_TRB_DMA | DBM_TRB_EP_NUM(dep->number);
	trb->size = DWC3_TRB_SIZE_LENGTH(req->request.length);
	trb->ctrl = DWC3_TRBCTL_NORMAL | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_CHN;
	req->trb_dma = dwc3_trb_dma_offset(dep, trb);

	/* Second, prepare a Link TRB that points to the first TRB*/
	trb_link = &dep->trb_pool[dep->free_slot & DWC3_TRB_MASK];
	dep->free_slot++;
	memset(trb_link, 0, sizeof *trb_link);

	trb_link->bpl = lower_32_bits(req->trb_dma);
	trb_link->bph = DBM_TRB_BIT |
			DBM_TRB_DMA | DBM_TRB_EP_NUM(dep->number);
	trb_link->size = 0;
	trb_link->ctrl = DWC3_TRBCTL_LINK_TRB | DWC3_TRB_CTRL_HWO;

	/*
	 * Now start the transfer
	 */
	memset(&params, 0, sizeof(params));
	params.param0 = 0; /* TDAddr High */
	params.param1 = lower_32_bits(req->trb_dma); /* DAddr Low */

	/* DBM requires IOC to be set */
	cmd = DWC3_DEPCMD_STARTTRANSFER | DWC3_DEPCMD_CMDIOC;
	ret = dwc3_send_gadget_ep_cmd(dep->dwc, dep->number, cmd, &params);
	if (ret < 0) {
		dev_dbg(dep->dwc->dev,
			"%s: failed to send STARTTRANSFER command\n",
			__func__);

		list_del(&req->list);
		return ret;
	}
	dep->flags |= DWC3_EP_BUSY;

	return ret;
}

/**
* Queue a usb request to the DBM endpoint.
* This function should be called after the endpoint
* was enabled by the ep_enable.
*
* This function prepares special structure of TRBs which
* is familier with the DBM HW, so it will possible to use
* this endpoint in DBM mode.
*
* The TRBs prepared by this function, is one normal TRB
* which point to a fake buffer, followed by a link TRB
* that points to the first TRB.
*
* The API of this function follow the regular API of
* usb_ep_queue (see usb_ep_ops in include/linuk/usb/gadget.h).
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to usb_request instance.
* @gfp_flags - possible flags.
*
* @return int - 0 on success, negetive on error.
*/
static int dwc3_msm_ep_queue(struct usb_ep *ep,
			     struct usb_request *request, gfp_t gfp_flags)
{
	struct dwc3_request *req = to_dwc3_request(request);
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_msm_req_complete *req_complete;
	unsigned long flags;
	int ret = 0;
	u8 bam_pipe;
	bool producer;
	bool disable_wb;
	bool internal_mem;
	bool ioc;
	u8 speed;

	if (!(request->udc_priv & MSM_SPS_MODE)) {
		/* Not SPS mode, call original queue */
		dev_vdbg(mdwc->dev, "%s: not sps mode, use regular queue\n",
					__func__);

		return (mdwc->original_ep_ops[dep->number])->queue(ep,
								request,
								gfp_flags);
	}

	if (!dep->endpoint.desc) {
		dev_err(mdwc->dev,
			"%s: trying to queue request %p to disabled ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	}

	if (dep->number == 0 || dep->number == 1) {
		dev_err(mdwc->dev,
			"%s: trying to queue dbm request %p to control ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	}


	if (dep->busy_slot != dep->free_slot || !list_empty(&dep->request_list)
					 || !list_empty(&dep->req_queued)) {
		dev_err(mdwc->dev,
			"%s: trying to queue dbm request %p tp ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	} else {
		dep->busy_slot = 0;
		dep->free_slot = 0;
	}

	/*
	 * Override req->complete function, but before doing that,
	 * store it's original pointer in the req_complete_list.
	 */
	req_complete = kzalloc(sizeof(*req_complete), GFP_KERNEL);
	if (!req_complete) {
		dev_err(mdwc->dev, "%s: not enough memory\n", __func__);
		return -ENOMEM;
	}
	req_complete->req = request;
	req_complete->orig_complete = request->complete;
	list_add_tail(&req_complete->list_item, &mdwc->req_complete_list);
	request->complete = dwc3_msm_req_complete_func;

	/*
	 * Configure the DBM endpoint
	 */
	bam_pipe = request->udc_priv & MSM_PIPE_ID_MASK;
	producer = ((request->udc_priv & MSM_PRODUCER) ? true : false);
	disable_wb = ((request->udc_priv & MSM_DISABLE_WB) ? true : false);
	internal_mem = ((request->udc_priv & MSM_INTERNAL_MEM) ? true : false);
	ioc = ((request->udc_priv & MSM_ETD_IOC) ? true : false);

	ret = dwc3_msm_dbm_ep_config(mdwc, dep->number,
					bam_pipe, producer,
					disable_wb, internal_mem, ioc);
	if (ret < 0) {
		dev_err(mdwc->dev,
			"error %d after calling dwc3_msm_dbm_ep_config\n",
			ret);
		return ret;
	}

	dev_vdbg(dwc->dev, "%s: queing request %p to ep %s length %d\n",
			__func__, request, ep->name, request->length);

	/*
	 * We must obtain the lock of the dwc3 core driver,
	 * including disabling interrupts, so we will be sure
	 * that we are the only ones that configure the HW device
	 * core and ensure that we queuing the request will finish
	 * as soon as possible so we will release back the lock.
	 */
	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_msm_ep_queue(dep, req);
	spin_unlock_irqrestore(&dwc->lock, flags);
	if (ret < 0) {
		dev_err(mdwc->dev,
			"error %d after calling __dwc3_msm_ep_queue\n", ret);
		return ret;
	}

	speed = dwc3_readl(dwc->regs, DWC3_DSTS) & DWC3_DSTS_CONNECTSPD;
	dwc3_msm_write_reg(mdwc->base, DBM_GEN_CFG, speed >> 2);

	return 0;
}

/**
 * Configure MSM endpoint.
 * This function do specific configurations
 * to an endpoint which need specific implementaion
 * in the MSM architecture.
 *
 * This function should be called by usb function/class
 * layer which need a support from the specific MSM HW
 * which wrap the USB3 core. (like DBM specific endpoints)
 *
 * @ep - a pointer to some usb_ep instance
 *
 * @return int - 0 on success, negetive on error.
 */
int msm_ep_config(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_ep_ops *new_ep_ops;

	dwc3_msm_event_buffer_config(mdwc,
			dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTADRLO(0)),
			dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTSIZ(0)));

	/* Save original ep ops for future restore*/
	if (mdwc->original_ep_ops[dep->number]) {
		dev_err(mdwc->dev,
			"ep [%s,%d] already configured as msm endpoint\n",
			ep->name, dep->number);
		return -EPERM;
	}
	mdwc->original_ep_ops[dep->number] = ep->ops;

	/* Set new usb ops as we like */
	new_ep_ops = kzalloc(sizeof(struct usb_ep_ops), GFP_KERNEL);
	if (!new_ep_ops) {
		dev_err(mdwc->dev,
			"%s: unable to allocate mem for new usb ep ops\n",
			__func__);
		return -ENOMEM;
	}
	(*new_ep_ops) = (*ep->ops);
	new_ep_ops->queue = dwc3_msm_ep_queue;
	new_ep_ops->disable = ep->ops->disable;

	ep->ops = new_ep_ops;

	/*
	 * Do HERE more usb endpoint configurations
	 * which are specific to MSM.
	 */

	return 0;
}
EXPORT_SYMBOL(msm_ep_config);

/**
 * Un-configure MSM endpoint.
 * Tear down configurations done in the
 * dwc3_msm_ep_config function.
 *
 * @ep - a pointer to some usb_ep instance
 *
 * @return int - 0 on success, negetive on error.
 */
int msm_ep_unconfig(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_ep_ops *old_ep_ops;

	/* Restore original ep ops */
	if (!mdwc->original_ep_ops[dep->number]) {
		dev_err(mdwc->dev,
			"ep [%s,%d] was not configured as msm endpoint\n",
			ep->name, dep->number);
		return -EINVAL;
	}
	old_ep_ops = (struct usb_ep_ops	*)ep->ops;
	ep->ops = mdwc->original_ep_ops[dep->number];
	mdwc->original_ep_ops[dep->number] = NULL;
	kfree(old_ep_ops);

	/*
	 * Do HERE more usb endpoint un-configurations
	 * which are specific to MSM.
	 */

	return 0;
}
EXPORT_SYMBOL(msm_ep_unconfig);

static void dwc3_restart_usb_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
						restart_usb_work);

	dev_dbg(mdwc->dev, "%s\n", __func__);

	if (atomic_read(&mdwc->in_lpm) || !mdwc->otg_xceiv) {
		dev_err(mdwc->dev, "%s failed!!!\n", __func__);
		return;
	}

	if (!mdwc->ext_xceiv.bsv) {
		dev_dbg(mdwc->dev, "%s bailing out in disconnect\n", __func__);
		return;
	}

	/* Reset active USB connection */
	mdwc->ext_xceiv.bsv = false;
	queue_delayed_work(system_nrt_wq, &mdwc->resume_work, 0);
	/* Make sure disconnect is processed before sending connect */
	flush_delayed_work(&mdwc->resume_work);

	mdwc->ext_xceiv.bsv = true;
	queue_delayed_work(system_nrt_wq, &mdwc->resume_work, 0);
}

/**
 * Reset USB peripheral connection
 * Inform OTG for Vbus LOW followed by Vbus HIGH notification.
 * This performs full hardware reset and re-initialization which
 * might be required by some DBM client driver during uninit/cleanup.
 */
void msm_dwc3_restart_usb_session(struct usb_gadget *gadget)
{
	struct dwc3 *dwc = container_of(gadget, struct dwc3, gadget);
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	if (mdwc)
		return;

	dev_dbg(mdwc->dev, "%s\n", __func__);
	queue_work(system_nrt_wq, &mdwc->restart_usb_work);
}
EXPORT_SYMBOL(msm_dwc3_restart_usb_session);

/**
 * msm_register_usb_ext_notification: register for event notification
 * @info: pointer to client usb_ext_notification structure. May be NULL.
 *
 * @return int - 0 on success, negative on error
 */
int msm_register_usb_ext_notification(struct usb_ext_notification *info)
{
	pr_debug("%s usb_ext: %p\n", __func__, info);

	if (info) {
		if (usb_ext) {
			pr_err("%s: already registered\n", __func__);
			return -EEXIST;
		}

		if (!info->notify) {
			pr_err("%s: notify is NULL\n", __func__);
			return -EINVAL;
		}
	}

	usb_ext = info;
	return 0;
}
EXPORT_SYMBOL(msm_register_usb_ext_notification);

/* HSPHY */
static int dwc3_hsusb_config_vddcx(int high)
{
	int min_vol, max_vol, ret;
	struct dwc3_msm *dwc = context;

	max_vol = dwc->vdd_high_vol_level;
	min_vol = high ? dwc->vdd_low_vol_level : dwc->vdd_no_vol_level;
	ret = regulator_set_voltage(dwc->hsusb_vddcx, min_vol, max_vol);
	if (ret) {
		dev_err(dwc->dev, "unable to set voltage for HSUSB_VDDCX\n");
		return ret;
	}

	dev_dbg(dwc->dev, "%s: min_vol:%d max_vol:%d\n", __func__,
							min_vol, max_vol);

	return ret;
}

static int dwc3_hsusb_ldo_init(int init)
{
	int rc = 0;
	struct dwc3_msm *dwc = context;

	if (!init) {
		regulator_set_voltage(dwc->hsusb_1p8, 0, USB_HSPHY_1P8_VOL_MAX);
		regulator_set_voltage(dwc->hsusb_3p3, 0, USB_HSPHY_3P3_VOL_MAX);
		return 0;
	}

	dwc->hsusb_3p3 = devm_regulator_get(dwc->dev, "HSUSB_3p3");
	if (IS_ERR(dwc->hsusb_3p3)) {
		dev_err(dwc->dev, "unable to get hsusb 3p3\n");
		return PTR_ERR(dwc->hsusb_3p3);
	}

	rc = regulator_set_voltage(dwc->hsusb_3p3,
			USB_HSPHY_3P3_VOL_MIN, USB_HSPHY_3P3_VOL_MAX);
	if (rc) {
		dev_err(dwc->dev, "unable to set voltage for hsusb 3p3\n");
		return rc;
	}
	dwc->hsusb_1p8 = devm_regulator_get(dwc->dev, "HSUSB_1p8");
	if (IS_ERR(dwc->hsusb_1p8)) {
		dev_err(dwc->dev, "unable to get hsusb 1p8\n");
		rc = PTR_ERR(dwc->hsusb_1p8);
		goto devote_3p3;
	}
	rc = regulator_set_voltage(dwc->hsusb_1p8,
			USB_HSPHY_1P8_VOL_MIN, USB_HSPHY_1P8_VOL_MAX);
	if (rc) {
		dev_err(dwc->dev, "unable to set voltage for hsusb 1p8\n");
		goto devote_3p3;
	}

	return 0;

devote_3p3:
	regulator_set_voltage(dwc->hsusb_3p3, 0, USB_HSPHY_3P3_VOL_MAX);

	return rc;
}

static int dwc3_hsusb_ldo_enable(int on)
{
	int rc = 0;
	struct dwc3_msm *dwc = context;

	dev_dbg(dwc->dev, "reg (%s)\n", on ? "HPM" : "LPM");

	if (!on)
		goto disable_regulators;


	rc = regulator_set_optimum_mode(dwc->hsusb_1p8, USB_HSPHY_1P8_HPM_LOAD);
	if (rc < 0) {
		dev_err(dwc->dev, "Unable to set HPM of regulator HSUSB_1p8\n");
		return rc;
	}

	rc = regulator_enable(dwc->hsusb_1p8);
	if (rc) {
		dev_err(dwc->dev, "Unable to enable HSUSB_1p8\n");
		goto put_1p8_lpm;
	}

	rc = regulator_set_optimum_mode(dwc->hsusb_3p3,	USB_HSPHY_3P3_HPM_LOAD);
	if (rc < 0) {
		dev_err(dwc->dev, "Unable to set HPM of regulator HSUSB_3p3\n");
		goto disable_1p8;
	}

	rc = regulator_enable(dwc->hsusb_3p3);
	if (rc) {
		dev_err(dwc->dev, "Unable to enable HSUSB_3p3\n");
		goto put_3p3_lpm;
	}

	return 0;

disable_regulators:
	rc = regulator_disable(dwc->hsusb_3p3);
	if (rc)
		dev_err(dwc->dev, "Unable to disable HSUSB_3p3\n");

put_3p3_lpm:
	rc = regulator_set_optimum_mode(dwc->hsusb_3p3, 0);
	if (rc < 0)
		dev_err(dwc->dev, "Unable to set LPM of regulator HSUSB_3p3\n");

disable_1p8:
	rc = regulator_disable(dwc->hsusb_1p8);
	if (rc)
		dev_err(dwc->dev, "Unable to disable HSUSB_1p8\n");

put_1p8_lpm:
	rc = regulator_set_optimum_mode(dwc->hsusb_1p8, 0);
	if (rc < 0)
		dev_err(dwc->dev, "Unable to set LPM of regulator HSUSB_1p8\n");

	return rc < 0 ? rc : 0;
}

/* SSPHY */
static int dwc3_ssusb_config_vddcx(int high)
{
	int min_vol, max_vol, ret;
	struct dwc3_msm *dwc = context;

	max_vol = dwc->vdd_high_vol_level;
	min_vol = high ? dwc->vdd_low_vol_level : dwc->vdd_no_vol_level;
	ret = regulator_set_voltage(dwc->ssusb_vddcx, min_vol, max_vol);
	if (ret) {
		dev_err(dwc->dev, "unable to set voltage for SSUSB_VDDCX\n");
		return ret;
	}

	dev_dbg(dwc->dev, "%s: min_vol:%d max_vol:%d\n", __func__,
							min_vol, max_vol);
	return ret;
}

/* 3.3v supply not needed for SS PHY */
static int dwc3_ssusb_ldo_init(int init)
{
	int rc = 0;
	struct dwc3_msm *dwc = context;

	if (!init) {
		regulator_set_voltage(dwc->ssusb_1p8, 0, USB_SSPHY_1P8_VOL_MAX);
		return 0;
	}

	dwc->ssusb_1p8 = devm_regulator_get(dwc->dev, "SSUSB_1p8");
	if (IS_ERR(dwc->ssusb_1p8)) {
		dev_err(dwc->dev, "unable to get ssusb 1p8\n");
		return PTR_ERR(dwc->ssusb_1p8);
	}
	rc = regulator_set_voltage(dwc->ssusb_1p8,
			USB_SSPHY_1P8_VOL_MIN, USB_SSPHY_1P8_VOL_MAX);
	if (rc)
		dev_err(dwc->dev, "unable to set voltage for ssusb 1p8\n");

	return rc;
}

static int dwc3_ssusb_ldo_enable(int on)
{
	int rc = 0;
	struct dwc3_msm *dwc = context;

	dev_dbg(context->dev, "reg (%s)\n", on ? "HPM" : "LPM");

	if (!on)
		goto disable_regulators;


	rc = regulator_set_optimum_mode(dwc->ssusb_1p8, USB_SSPHY_1P8_HPM_LOAD);
	if (rc < 0) {
		dev_err(dwc->dev, "Unable to set HPM of SSUSB_1p8\n");
		return rc;
	}

	rc = regulator_enable(dwc->ssusb_1p8);
	if (rc) {
		dev_err(dwc->dev, "Unable to enable SSUSB_1p8\n");
		goto put_1p8_lpm;
	}

	return 0;

disable_regulators:
	rc = regulator_disable(dwc->ssusb_1p8);
	if (rc)
		dev_err(dwc->dev, "Unable to disable SSUSB_1p8\n");

put_1p8_lpm:
	rc = regulator_set_optimum_mode(dwc->ssusb_1p8, 0);
	if (rc < 0)
		dev_err(dwc->dev, "Unable to set LPM of SSUSB_1p8\n");

	return rc < 0 ? rc : 0;
}

/*
 * Config Global Distributed Switch Controller (GDSC)
 * to support controller power collapse
 */
static int dwc3_msm_config_gdsc(struct dwc3_msm *msm, int on)
{
	int ret = 0;

	if (IS_ERR(msm->dwc3_gdsc))
		return 0;

	if (!msm->dwc3_gdsc) {
		msm->dwc3_gdsc = devm_regulator_get(msm->dev,
			"USB3_GDSC");
		if (IS_ERR(msm->dwc3_gdsc))
			return 0;
	}

	if (on) {
		ret = regulator_enable(msm->dwc3_gdsc);
		if (ret) {
			dev_err(msm->dev, "unable to enable usb3 gdsc\n");
			return ret;
		}
	} else {
		regulator_disable(msm->dwc3_gdsc);
	}

	return 0;
}

static int dwc3_msm_link_clk_reset(bool assert)
{
	int ret = 0;
	struct dwc3_msm *mdwc = context;

	if (assert) {
		/* Using asynchronous block reset to the hardware */
		dev_dbg(mdwc->dev, "block_reset ASSERT\n");
		clk_disable_unprepare(mdwc->ref_clk);
		clk_disable_unprepare(mdwc->iface_clk);
		clk_disable_unprepare(mdwc->core_clk);
		ret = clk_reset(mdwc->core_clk, CLK_RESET_ASSERT);
		if (ret)
			dev_err(mdwc->dev, "dwc3 core_clk assert failed\n");
	} else {
		dev_dbg(mdwc->dev, "block_reset DEASSERT\n");
		ret = clk_reset(mdwc->core_clk, CLK_RESET_DEASSERT);
		ndelay(200);
		clk_prepare_enable(mdwc->core_clk);
		clk_prepare_enable(mdwc->ref_clk);
		clk_prepare_enable(mdwc->iface_clk);
		if (ret)
			dev_err(mdwc->dev, "dwc3 core_clk deassert failed\n");
	}

	return ret;
}

/* Reinitialize SSPHY parameters by overriding using QSCRATCH CR interface */
static void dwc3_msm_ss_phy_reg_init(struct dwc3_msm *msm)
{
	u32 data = 0;

	/*
	 * WORKAROUND: There is SSPHY suspend bug due to which USB enumerates
	 * in HS mode instead of SS mode. Workaround it by asserting
	 * LANE0.TX_ALT_BLOCK.EN_ALT_BUS to enable TX to use alt bus mode
	 */
	data = dwc3_msm_ssusb_read_phycreg(msm->base, 0x102D);
	data |= (1 << 7);
	dwc3_msm_ssusb_write_phycreg(msm->base, 0x102D, data);

	data = dwc3_msm_ssusb_read_phycreg(msm->base, 0x1010);
	data &= ~0xFF0;
	data |= 0x20;
	dwc3_msm_ssusb_write_phycreg(msm->base, 0x1010, data);

	/*
	 * Fix RX Equalization setting as follows
	 * LANE0.RX_OVRD_IN_HI. RX_EQ_EN set to 0
	 * LANE0.RX_OVRD_IN_HI.RX_EQ_EN_OVRD set to 1
	 * LANE0.RX_OVRD_IN_HI.RX_EQ set to 3
	 * LANE0.RX_OVRD_IN_HI.RX_EQ_OVRD set to 1
	 */
	data = dwc3_msm_ssusb_read_phycreg(msm->base, 0x1006);
	data &= ~(1 << 6);
	data |= (1 << 7);
	data &= ~(0x7 << 8);
	data |= (0x3 << 8);
	data |= (0x1 << 11);
	dwc3_msm_ssusb_write_phycreg(msm->base, 0x1006, data);

	/*
	 * Set EQ and TX launch amplitudes as follows
	 * LANE0.TX_OVRD_DRV_LO.PREEMPH set to 22
	 * LANE0.TX_OVRD_DRV_LO.AMPLITUDE set to 127
	 * LANE0.TX_OVRD_DRV_LO.EN set to 1.
	 */
	data = dwc3_msm_ssusb_read_phycreg(msm->base, 0x1002);
	data &= ~0x3F80;
	data |= (0x16 << 7);
	data &= ~0x7F;
	data |= (0x7F | (1 << 14));
	dwc3_msm_ssusb_write_phycreg(msm->base, 0x1002, data);

	/*
	 * Set the QSCRATCH SS_PHY_PARAM_CTRL1 parameters as follows
	 * TX_FULL_SWING [26:20] amplitude to 127
	 * TX_DEEMPH_3_5DB [13:8] to 22
	 * LOS_BIAS [2:0] to 0x5
	 */
	dwc3_msm_write_readback(msm->base, SS_PHY_PARAM_CTRL_1,
				0x07f03f07, 0x07f01605);
}

/* Initialize QSCRATCH registers for HSPHY and SSPHY operation */
static void dwc3_msm_qscratch_reg_init(struct dwc3_msm *msm)
{
	/* SSPHY Initialization: Use ref_clk from pads and set its parameters */
	dwc3_msm_write_reg(msm->base, SS_PHY_CTRL_REG, 0x10210002);
	msleep(30);
	/* Assert SSPHY reset */
	dwc3_msm_write_reg(msm->base, SS_PHY_CTRL_REG, 0x10210082);
	usleep_range(2000, 2200);
	/* De-assert SSPHY reset - power and ref_clock must be ON */
	dwc3_msm_write_reg(msm->base, SS_PHY_CTRL_REG, 0x10210002);
	usleep_range(2000, 2200);
	/* Ref clock must be stable now, enable ref clock for HS mode */
	dwc3_msm_write_reg(msm->base, SS_PHY_CTRL_REG, 0x10210102);
	usleep_range(2000, 2200);
	/*
	 * HSPHY Initialization: Enable UTMI clock and clamp enable HVINTs,
	 * and disable RETENTION (power-on default is ENABLED)
	 */
	dwc3_msm_write_reg(msm->base, HS_PHY_CTRL_REG, 0x5220bb2);
	usleep_range(2000, 2200);
	/* Disable (bypass) VBUS and ID filters */
	dwc3_msm_write_reg(msm->base, QSCRATCH_GENERAL_CFG, 0x78);
	/*
	 * write HSPHY init value to QSCRATCH reg to set HSPHY parameters like
	 * VBUS valid threshold, disconnect valid threshold, DC voltage level,
	 * preempasis and rise/fall time.
	 */
	if (override_phy_init)
		msm->hsphy_init_seq = override_phy_init;
	if (msm->hsphy_init_seq)
		dwc3_msm_write_readback(msm->base,
					PARAMETER_OVERRIDE_X_REG, 0x03FFFFFF,
					msm->hsphy_init_seq & 0x03FFFFFF);

	/* Enable master clock for RAMs to allow BAM to access RAMs when
	 * RAM clock gating is enabled via DWC3's GCTL. Otherwise, issues
	 * are seen where RAM clocks get turned OFF in SS mode
	 */
	dwc3_msm_write_reg(msm->base, CGCTL_REG,
		dwc3_msm_read_reg(msm->base, CGCTL_REG) | 0x18);

	dwc3_msm_ss_phy_reg_init(msm);
}

static void dwc3_msm_hsphy_host_init_seq(void)
{
	struct dwc3_msm *msm = context;

	if (!msm) {
		pr_err("%s: No device\n", __func__);
		return;
	}

	if (override_phy_host_init)
		msm->hsphy_host_init_seq = override_phy_host_init;
	if (msm->hsphy_host_init_seq)
		dwc3_msm_write_readback(msm->base,
					PARAMETER_OVERRIDE_X_REG, 0x03FFFFFF,
					msm->hsphy_host_init_seq & 0x03FFFFFF);
}

static void dwc3_msm_block_reset(bool core_reset)
{

	struct dwc3_msm *mdwc = context;
	int ret  = 0;

	if (core_reset) {
		ret = dwc3_msm_link_clk_reset(1);
		if (ret)
			return;

		usleep_range(1000, 1200);
		ret = dwc3_msm_link_clk_reset(0);
		if (ret)
			return;

		usleep_range(10000, 12000);

		/* Reinitialize QSCRATCH registers after block reset */
		dwc3_msm_qscratch_reg_init(mdwc);
	}

	/* Reset the DBM */
	dwc3_msm_dbm_soft_reset(mdwc, 1);
	usleep_range(1000, 1200);
	dwc3_msm_dbm_soft_reset(mdwc, 0);
}

static void dwc3_chg_enable_secondary_det(struct dwc3_msm *mdwc)
{
	u32 chg_ctrl;

	/* Turn off VDP_SRC */
	dwc3_msm_write_reg(mdwc->base, CHARGING_DET_CTRL_REG, 0x0);
	msleep(20);

	/* Before proceeding make sure VDP_SRC is OFF */
	chg_ctrl = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_CTRL_REG);
	if (chg_ctrl & 0x3F)
		dev_err(mdwc->dev, "%s Unable to reset chg_det block: %x\n",
						 __func__, chg_ctrl);
	/*
	 * Configure DM as current source, DP as current sink
	 * and enable battery charging comparators.
	 */
	dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG, 0x3F, 0x34);
}

static bool dwc3_chg_det_check_linestate(struct dwc3_msm *mdwc)
{
	u32 chg_det;

	if (!prop_chg_detect)
		return false;

	chg_det = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_OUTPUT_REG);
	return chg_det & (3 << 8);
}

static bool dwc3_chg_det_check_output(struct dwc3_msm *mdwc)
{
	u32 chg_det;
	bool ret = false;

	chg_det = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_OUTPUT_REG);
	ret = chg_det & 1;

	return ret;
}

static void dwc3_chg_enable_primary_det(struct dwc3_msm *mdwc)
{
	/*
	 * Configure DP as current source, DM as current sink
	 * and enable battery charging comparators.
	 */
	dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG, 0x3F, 0x30);
}

static inline bool dwc3_chg_check_dcd(struct dwc3_msm *mdwc)
{
	u32 chg_state;
	bool ret = false;

	chg_state = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_OUTPUT_REG);
	ret = chg_state & 2;

	return ret;
}

static inline void dwc3_chg_disable_dcd(struct dwc3_msm *mdwc)
{
	dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG, 0x3F, 0x0);
}

static inline void dwc3_chg_enable_dcd(struct dwc3_msm *mdwc)
{
	/* Data contact detection enable, DCDENB */
	dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG, 0x3F, 0x2);
}

static void dwc3_chg_block_reset(struct dwc3_msm *mdwc)
{
	u32 chg_ctrl;

	/* Clear charger detecting control bits */
	dwc3_msm_write_reg(mdwc->base, CHARGING_DET_CTRL_REG, 0x0);

	/* Clear alt interrupt latch and enable bits */
	dwc3_msm_write_reg(mdwc->base, HS_PHY_IRQ_STAT_REG, 0xFFF);
	dwc3_msm_write_reg(mdwc->base, ALT_INTERRUPT_EN_REG, 0x0);

	udelay(100);

	/* Before proceeding make sure charger block is RESET */
	chg_ctrl = dwc3_msm_read_reg(mdwc->base, CHARGING_DET_CTRL_REG);
	if (chg_ctrl & 0x3F)
		dev_err(mdwc->dev, "%s Unable to reset chg_det block: %x\n",
						 __func__, chg_ctrl);
}

static const char *chg_to_string(enum dwc3_chg_type chg_type)
{
	switch (chg_type) {
	case DWC3_SDP_CHARGER:		return "USB_SDP_CHARGER";
	case DWC3_DCP_CHARGER:		return "USB_DCP_CHARGER";
	case DWC3_CDP_CHARGER:		return "USB_CDP_CHARGER";
	case DWC3_PROPRIETARY_CHARGER:	return "USB_PROPRIETARY_CHARGER";
	default:			return "INVALID_CHARGER";
	}
}

#define DWC3_CHG_DCD_POLL_TIME		(100 * HZ/1000) /* 100 msec */
#define DWC3_CHG_DCD_MAX_RETRIES	15 /* Tdcd_tmout = 15 * 100 msec */
#define DWC3_CHG_PRIMARY_DET_TIME	(50 * HZ/1000) /* TVDPSRC_ON */
#define DWC3_CHG_SECONDARY_DET_TIME	(50 * HZ/1000) /* TVDMSRC_ON */

static void dwc3_chg_detect_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, chg_work.work);
	bool is_dcd = false, tmout, vout;
	unsigned long delay;

	dev_dbg(mdwc->dev, "chg detection work\n");
	switch (mdwc->chg_state) {
	case USB_CHG_STATE_UNDEFINED:
		dwc3_chg_block_reset(mdwc);
		dwc3_chg_enable_dcd(mdwc);
		mdwc->chg_state = USB_CHG_STATE_WAIT_FOR_DCD;
		mdwc->dcd_retries = 0;
		delay = DWC3_CHG_DCD_POLL_TIME;
		break;
	case USB_CHG_STATE_WAIT_FOR_DCD:
		if (slimport_is_connected()) {
			dwc3_chg_block_reset(mdwc);
			mdwc->charger.chg_type = USB_SDP_CHARGER;
			mdwc->charger.notify_detection_complete(mdwc->otg_xceiv->otg,
								&mdwc->charger);
			return;
		}

		is_dcd = dwc3_chg_check_dcd(mdwc);
		tmout = ++mdwc->dcd_retries == DWC3_CHG_DCD_MAX_RETRIES;
		if (is_dcd || tmout) {
			dwc3_chg_disable_dcd(mdwc);
			if (dwc3_chg_det_check_linestate(mdwc)) {
				dev_dbg(mdwc->dev, "proprietary charger\n");
				mdwc->charger.chg_type =
						DWC3_PROPRIETARY_CHARGER;
				mdwc->chg_state = USB_CHG_STATE_DETECTED;
				delay = 0;
				break;
			}
			dwc3_chg_enable_primary_det(mdwc);
			delay = DWC3_CHG_PRIMARY_DET_TIME;
			mdwc->chg_state = USB_CHG_STATE_DCD_DONE;
		} else {
			delay = DWC3_CHG_DCD_POLL_TIME;
		}
		break;
	case USB_CHG_STATE_DCD_DONE:
		vout = dwc3_chg_det_check_output(mdwc);
		if (vout) {
			dwc3_chg_enable_secondary_det(mdwc);
			delay = DWC3_CHG_SECONDARY_DET_TIME;
			mdwc->chg_state = USB_CHG_STATE_PRIMARY_DONE;
		} else {
			mdwc->charger.chg_type = DWC3_SDP_CHARGER;
			mdwc->chg_state = USB_CHG_STATE_DETECTED;
			delay = 0;
		}
		break;
	case USB_CHG_STATE_PRIMARY_DONE:
		vout = dwc3_chg_det_check_output(mdwc);
		if (vout)
			mdwc->charger.chg_type = DWC3_DCP_CHARGER;
		else
			mdwc->charger.chg_type = DWC3_CDP_CHARGER;
		mdwc->chg_state = USB_CHG_STATE_SECONDARY_DONE;
		/* fall through */
	case USB_CHG_STATE_SECONDARY_DONE:
		mdwc->chg_state = USB_CHG_STATE_DETECTED;
		/* fall through */
	case USB_CHG_STATE_DETECTED:
		dwc3_chg_block_reset(mdwc);
		/* Enable VDP_SRC */
		if (mdwc->charger.chg_type == DWC3_DCP_CHARGER)
			dwc3_msm_write_readback(mdwc->base,
					CHARGING_DET_CTRL_REG, 0x1F, 0x10);
		dev_dbg(mdwc->dev, "chg_type = %s\n",
			chg_to_string(mdwc->charger.chg_type));
		mdwc->charger.notify_detection_complete(mdwc->otg_xceiv->otg,
								&mdwc->charger);
		return;
	default:
		return;
	}

	queue_delayed_work(system_nrt_wq, &mdwc->chg_work, delay);
}

static void dwc3_start_chg_det(struct dwc3_charger *charger, bool start)
{
	struct dwc3_msm *mdwc = context;

	if (start == false) {
		dev_dbg(mdwc->dev, "canceling charging detection work\n");
		cancel_delayed_work_sync(&mdwc->chg_work);
		mdwc->chg_state = USB_CHG_STATE_UNDEFINED;
		charger->chg_type = DWC3_INVALID_CHARGER;
		return;
	}

	mdwc->chg_state = USB_CHG_STATE_UNDEFINED;
	charger->chg_type = DWC3_INVALID_CHARGER;
	queue_delayed_work(system_nrt_wq, &mdwc->chg_work, 0);
}

static int dwc3_msm_suspend(struct dwc3_msm *mdwc)
{
	int ret;
	bool dcp;
	bool host_bus_suspend;
	bool host_ss_active;

	dev_dbg(mdwc->dev, "%s: entering lpm\n", __func__);

	if (atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: Already suspended\n", __func__);
		return 0;
	}

	host_ss_active = dwc3_msm_read_reg(mdwc->base, USB3_PORTSC) & PORT_PE;

	if (cancel_delayed_work_sync(&mdwc->chg_work))
		dev_dbg(mdwc->dev, "%s: chg_work was pending\n", __func__);
	if (mdwc->chg_state != USB_CHG_STATE_DETECTED) {
		/* charger detection wasn't complete; re-init flags */
		mdwc->chg_state = USB_CHG_STATE_UNDEFINED;
		mdwc->charger.chg_type = DWC3_INVALID_CHARGER;
		dwc3_msm_write_readback(mdwc->base, CHARGING_DET_CTRL_REG,
								0x37, 0x0);
	}

	dcp = ((mdwc->charger.chg_type == DWC3_DCP_CHARGER) ||
	      (mdwc->charger.chg_type == DWC3_PROPRIETARY_CHARGER));
	host_bus_suspend = mdwc->host_mode == 1;

	/* Sequence to put SSPHY in low power state:
	 * 1. Clear REF_SS_PHY_EN in SS_PHY_CTRL_REG
	 * 2. Clear REF_USE_PAD in SS_PHY_CTRL_REG
	 * 3. Set TEST_POWERED_DOWN in SS_PHY_CTRL_REG to enable PHY retention
	 * 4. Disable SSPHY ref clk
	 */
	dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 8), 0x0);
	dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 28), 0x0);
	dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 26),
								(1 << 26));

	usleep_range(1000, 1200);
	clk_disable_unprepare(mdwc->ref_clk);

	if (host_bus_suspend) {
		/* Sequence for host bus suspend case:
		 * 1. Set suspend and sleep bits in GUSB2PHYCONFIG reg
		 * 2. Clear interrupt latch register and enable BSV, ID HV intr
		 * 3. Enable DP and DM HV interrupts in ALT_INTERRUPT_EN_REG
		 */
		dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
			dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0)) |
								0x00000140);
		dwc3_msm_write_reg(mdwc->base, HS_PHY_IRQ_STAT_REG, 0xFFF);
		if (mdwc->otg_xceiv && (!mdwc->ext_xceiv.otg_capability))
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
							 0x18000, 0x18000);
		dwc3_msm_write_reg(mdwc->base, ALT_INTERRUPT_EN_REG, 0xFC0);
		udelay(5);
	} else {
		/* Sequence to put hardware in low power state:
		 * 1. Set OTGDISABLE to disable OTG block in HSPHY (saves power)
		 * 2. Clear charger detection control fields (performed above)
		 * 3. SUSPEND PHY and turn OFF core clock after some delay
		 * 4. Clear interrupt latch register and enable BSV, ID HV intr
		 * 5. Enable PHY retention
		 */
		dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG, 0x1000,
									0x1000);
		dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
							0xC00000, 0x800000);
		dwc3_msm_write_reg(mdwc->base, HS_PHY_IRQ_STAT_REG, 0xFFF);
		if (mdwc->otg_xceiv && (!mdwc->ext_xceiv.otg_capability))
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
							0x18000, 0x18000);
		if (!dcp)
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
								0x2, 0x0);
	}

	/* make sure above writes are completed before turning off clocks */
	wmb();

	/* remove vote for controller power collapse */
	if (!host_bus_suspend)
		dwc3_msm_config_gdsc(mdwc, 0);

	if (!host_bus_suspend || !host_ss_active) {
		clk_disable_unprepare(mdwc->core_clk);
		mdwc->lpm_flags |= MDWC3_CORECLK_OFF;
	}
	clk_disable_unprepare(mdwc->iface_clk);

	if (!host_bus_suspend)
		clk_disable_unprepare(mdwc->utmi_clk);

	if (!host_bus_suspend) {
		/* USB PHY no more requires TCXO */
		clk_disable_unprepare(mdwc->xo_clk);
		mdwc->lpm_flags |= MDWC3_TCXO_SHUTDOWN;
	}

	if (mdwc->bus_perf_client) {
		ret = msm_bus_scale_client_update_request(
						mdwc->bus_perf_client, 0);
		if (ret)
			dev_err(mdwc->dev, "Failed to reset bus bw vote\n");
	}

	if (mdwc->otg_xceiv && mdwc->ext_xceiv.otg_capability && !dcp &&
							!host_bus_suspend)
		dwc3_hsusb_ldo_enable(0);

	dwc3_ssusb_ldo_enable(0);
	dwc3_ssusb_config_vddcx(0);
	if (!host_bus_suspend && !dcp)
		dwc3_hsusb_config_vddcx(0);

	/* arm the interrupt only for host mode lpm */
	if (host_bus_suspend && mdwc->hs_phy_irq &&
			!test_and_set_bit(0, &mdwc->dwc3_irq_enabled)) {
		enable_irq(mdwc->hs_phy_irq);
		enable_irq_wake(mdwc->hs_phy_irq);
	}

	pm_relax(mdwc->dev);
	atomic_set(&mdwc->in_lpm, 1);

	dev_info(mdwc->dev, "DWC3 in low power mode\n");

	return 0;
}

static int dwc3_msm_resume(struct dwc3_msm *mdwc)
{
	int ret;
	bool dcp;
	bool host_bus_suspend;

	dev_dbg(mdwc->dev, "%s: exiting lpm\n", __func__);

	if (!atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: Already resumed\n", __func__);
		return 0;
	}

	pm_stay_awake(mdwc->dev);

	if (mdwc->bus_perf_client) {
		ret = msm_bus_scale_client_update_request(
						mdwc->bus_perf_client, 1);
		if (ret)
			dev_err(mdwc->dev, "Failed to vote for bus scaling\n");
	}

	dcp = ((mdwc->charger.chg_type == DWC3_DCP_CHARGER) ||
	      (mdwc->charger.chg_type == DWC3_PROPRIETARY_CHARGER));
	host_bus_suspend = mdwc->host_mode == 1;

	if (mdwc->lpm_flags & MDWC3_TCXO_SHUTDOWN) {
		/* Vote for TCXO while waking up USB HSPHY */
		ret = clk_prepare_enable(mdwc->xo_clk);
		if (ret)
			dev_err(mdwc->dev, "%s failed to vote TCXO buffer%d\n",
						__func__, ret);
		mdwc->lpm_flags &= ~MDWC3_TCXO_SHUTDOWN;
	}

	/* add vote for controller power collapse */
	if (!host_bus_suspend)
		dwc3_msm_config_gdsc(mdwc, 1);

	if (!host_bus_suspend)
		clk_prepare_enable(mdwc->utmi_clk);

	if (mdwc->otg_xceiv && mdwc->ext_xceiv.otg_capability && !dcp &&
							!host_bus_suspend)
		dwc3_hsusb_ldo_enable(1);

	dwc3_ssusb_ldo_enable(1);
	dwc3_ssusb_config_vddcx(1);

	if (!host_bus_suspend && !dcp)
		dwc3_hsusb_config_vddcx(1);

	clk_prepare_enable(mdwc->ref_clk);
	usleep_range(1000, 1200);

	clk_prepare_enable(mdwc->iface_clk);
	if (mdwc->lpm_flags & MDWC3_CORECLK_OFF) {
		clk_prepare_enable(mdwc->core_clk);
		mdwc->lpm_flags &= ~MDWC3_CORECLK_OFF;
	}

	if (host_bus_suspend) {
		/* Disable HV interrupt */
		if (mdwc->otg_xceiv && (!mdwc->ext_xceiv.otg_capability))
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
							0x18000, 0x0);
		/* Clear interrupt latch register */
		dwc3_msm_write_reg(mdwc->base, HS_PHY_IRQ_STAT_REG, 0x000);

		/* Disable DP and DM HV interrupt */
		dwc3_msm_write_reg(mdwc->base, ALT_INTERRUPT_EN_REG, 0x000);

		/* Clear suspend bit in GUSB2PHYCONFIG register */
		dwc3_msm_write_readback(mdwc->base, DWC3_GUSB2PHYCFG(0),
								0x40, 0x0);
	} else {
		/* Disable HV interrupt */
		if (mdwc->otg_xceiv && (!mdwc->ext_xceiv.otg_capability))
			dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG,
								0x18000, 0x0);
		/* Disable Retention */
		dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG, 0x2, 0x2);

		dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
			dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0)) |
								 0xF0000000);
		/* 10usec delay required before de-asserting PHY RESET */
		udelay(10);
		dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
		      dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0)) &
								0x7FFFFFFF);

		/* Bring PHY out of suspend */
		dwc3_msm_write_readback(mdwc->base, HS_PHY_CTRL_REG, 0xC00000,
									0x0);

	}

	/* Assert SS PHY RESET */
	dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 7),
								(1 << 7));
	dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 28),
								(1 << 28));
	dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 8),
								(1 << 8));
	dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 26), 0x0);
	/* 10usec delay required before de-asserting SS PHY RESET */
	udelay(10);
	dwc3_msm_write_readback(mdwc->base, SS_PHY_CTRL_REG, (1 << 7), 0x0);

	/*
	 * Reinitilize SSPHY parameters as SS_PHY RESET will reset
	 * the internal registers to default values.
	 */
	dwc3_msm_ss_phy_reg_init(mdwc);

	/* Disarm the interrupt once the controller is out of lpm */
	if (test_and_clear_bit(0, &mdwc->dwc3_irq_enabled)) {
		disable_irq_wake(mdwc->hs_phy_irq);
		disable_irq(mdwc->hs_phy_irq);
	}

	atomic_set(&mdwc->in_lpm, 0);

	dev_info(mdwc->dev, "DWC3 exited from low power mode\n");

	return 0;
}

static void dwc3_resume_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
							resume_work.work);

	dev_dbg(mdwc->dev, "%s: dwc3 resume work\n", __func__);
	/* handle any event that was queued while work was already running */
	if (!atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: notifying xceiv event\n", __func__);
		if (mdwc->otg_xceiv)
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_XCEIV_STATE);
		return;
	}

	/* bail out if system resume in process, else initiate RESUME */
	if (atomic_read(&mdwc->pm_suspended)) {
		mdwc->resume_pending = true;
	} else {
		pm_runtime_get_sync(mdwc->dev);
		if (mdwc->otg_xceiv)
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_PHY_RESUME);
		pm_runtime_put_noidle(mdwc->dev);
		if (mdwc->otg_xceiv && (mdwc->ext_xceiv.otg_capability))
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_XCEIV_STATE);
	}
}

static u32 debug_id = true, debug_bsv, debug_connect;

static int dwc3_connect_show(struct seq_file *s, void *unused)
{
	if (debug_connect)
		seq_printf(s, "true\n");
	else
		seq_printf(s, "false\n");

	return 0;
}

static int dwc3_connect_open(struct inode *inode, struct file *file)
{
	return single_open(file, dwc3_connect_show, inode->i_private);
}

static ssize_t dwc3_connect_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct dwc3_msm *mdwc = s->private;
	char buf[8];

	memset(buf, 0x00, sizeof(buf));

	if (copy_from_user(&buf, ubuf, min_t(size_t, sizeof(buf) - 1, count)))
		return -EFAULT;

	if (!strncmp(buf, "enable", 6) || !strncmp(buf, "true", 4)) {
		debug_connect = true;
	} else {
		debug_connect = debug_bsv = false;
		debug_id = true;
	}

	mdwc->ext_xceiv.bsv = debug_bsv;
	mdwc->ext_xceiv.id = debug_id ? DWC3_ID_FLOAT : DWC3_ID_GROUND;

	if (atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: calling resume_work\n", __func__);
		dwc3_resume_work(&mdwc->resume_work.work);
	} else {
		dev_dbg(mdwc->dev, "%s: notifying xceiv event\n", __func__);
		if (mdwc->otg_xceiv)
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_XCEIV_STATE);
	}

	return count;
}

const struct file_operations dwc3_connect_fops = {
	.open = dwc3_connect_open,
	.read = seq_read,
	.write = dwc3_connect_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct dentry *dwc3_debugfs_root;

static void dwc3_debugfs_init(struct dwc3_msm *mdwc)
{
	dwc3_debugfs_root = debugfs_create_dir("msm_dwc3", NULL);

	if (!dwc3_debugfs_root || IS_ERR(dwc3_debugfs_root))
		return;

	if (!debugfs_create_bool("id", S_IRUGO | S_IWUSR, dwc3_debugfs_root,
				 &debug_id))
		goto error;

	if (!debugfs_create_bool("bsv", S_IRUGO | S_IWUSR, dwc3_debugfs_root,
				 &debug_bsv))
		goto error;

	if (!debugfs_create_file("connect", S_IRUGO | S_IWUSR,
				dwc3_debugfs_root, mdwc, &dwc3_connect_fops))
		goto error;

	return;

error:
	debugfs_remove_recursive(dwc3_debugfs_root);
}

static irqreturn_t msm_dwc3_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;

	if (atomic_read(&mdwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s received in LPM\n", __func__);
		queue_delayed_work(system_nrt_wq, &mdwc->resume_work, 0);
	} else {
		/* With current implementation should never end up here */
		pr_info_ratelimited("%s: IRQ outside LPM\n", __func__);
	}

	return IRQ_HANDLED;
}

static int dwc3_msm_power_get_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);
	switch (psp) {
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = mdwc->host_mode;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = mdwc->current_max;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = mdwc->vbus_active;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = mdwc->online;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = psy->type;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int dwc3_msm_power_set_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  const union power_supply_propval *val)
{
	static bool init;
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_SCOPE:
		mdwc->host_mode = val->intval;
		break;
	/* Process PMIC notification in PRESENT prop */
	case POWER_SUPPLY_PROP_PRESENT:
		dev_dbg(mdwc->dev, "%s: notify xceiv event\n", __func__);
		if (mdwc->otg_xceiv && !mdwc->ext_inuse &&
		    (mdwc->ext_xceiv.otg_capability || !init)) {
			mdwc->ext_xceiv.bsv = val->intval;
			/*
			 * set debouncing delay to 120msec. Otherwise battery
			 * charging CDP complaince test fails if delay > 120ms.
			 */
			queue_delayed_work(system_nrt_wq,
							&mdwc->resume_work, 12);

			if (!init)
				init = true;
		}
		mdwc->vbus_active = val->intval;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		mdwc->online = val->intval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		mdwc->current_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		/*
		 * Since setting POWER_SUPPLY_PROP_TYPE doesn't
		 * do anything bail out here, it's not necessary
		 * to generate a power supply event.
		 */
		return 0;
	default:
		return -EINVAL;
	}

	power_supply_changed(&mdwc->usb_psy);
	return 0;
}

static void dwc3_msm_external_power_changed(struct power_supply *psy)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm, usb_psy);
	union power_supply_propval ret = {0,};

	if (!mdwc->ext_vbus_psy)
		mdwc->ext_vbus_psy = power_supply_get_by_name("ext-vbus");

	if (!mdwc->ext_vbus_psy) {
		pr_err("%s: Unable to get ext_vbus power_supply\n", __func__);
		return;
	}

	mdwc->ext_vbus_psy->get_property(mdwc->ext_vbus_psy,
					POWER_SUPPLY_PROP_ONLINE, &ret);
	if (ret.intval) {
		dwc3_start_chg_det(&mdwc->charger, false);
		mdwc->ext_vbus_psy->get_property(mdwc->ext_vbus_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &ret);
		power_supply_set_current_limit(&mdwc->usb_psy, ret.intval);
	}

	power_supply_set_online(&mdwc->usb_psy, ret.intval);
	power_supply_changed(&mdwc->usb_psy);
}


static char *dwc3_msm_pm_power_supplied_to[] = {
	"ac",
#ifdef CONFIG_TOUCHSCREEN_CHARGER_NOTIFY
	"touch",
#endif
};

static enum power_supply_property dwc3_msm_pm_power_props_usb[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_SCOPE,
};

static void dwc3_init_adc_work(struct work_struct *w);

static void dwc3_ext_notify_online(void *ctx, int on)
{
	struct dwc3_msm *mdwc = ctx;
	bool notify_otg = false;

	if (!mdwc) {
		pr_err("%s: DWC3 driver already removed\n", __func__);
		return;
	}

	dev_dbg(mdwc->dev, "notify %s%s\n", on ? "" : "dis", "connected");

	if (!mdwc->ext_vbus_psy)
		mdwc->ext_vbus_psy = power_supply_get_by_name("ext-vbus");

	mdwc->ext_inuse = on;
	if (on) {
		/* force OTG to exit B-peripheral state */
		mdwc->ext_xceiv.bsv = false;
		notify_otg = true;
		dwc3_start_chg_det(&mdwc->charger, false);
	} else {
		/* external client offline; tell OTG about cached ID/BSV */
		if (mdwc->ext_xceiv.id != mdwc->id_state) {
			mdwc->ext_xceiv.id = mdwc->id_state;
			notify_otg = true;
		}

		mdwc->ext_xceiv.bsv = mdwc->vbus_active;
		notify_otg |= mdwc->vbus_active;
	}

	if (mdwc->ext_vbus_psy)
		power_supply_set_present(mdwc->ext_vbus_psy, on);

	if (notify_otg)
		queue_delayed_work(system_nrt_wq, &mdwc->resume_work, 0);
}

static void dwc3_id_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, id_work);
	int ret;

	/* Give external client a chance to handle */
	if (!mdwc->ext_inuse && usb_ext) {
		if (mdwc->pmic_id_irq)
			disable_irq(mdwc->pmic_id_irq);

		ret = usb_ext->notify(usb_ext->ctxt, mdwc->id_state,
				      dwc3_ext_notify_online, mdwc);
		dev_dbg(mdwc->dev, "%s: external handler returned %d\n",
			__func__, ret);

		if (mdwc->pmic_id_irq) {
			unsigned long flags;
			local_irq_save(flags);
			/* ID may have changed while IRQ disabled; update it */
			mdwc->id_state = !!irq_read_line(mdwc->pmic_id_irq);
			local_irq_restore(flags);
			enable_irq(mdwc->pmic_id_irq);
		}

		mdwc->ext_inuse = (ret == 0);
	}

	if (!mdwc->ext_inuse) { /* notify OTG */
		mdwc->ext_xceiv.id = mdwc->id_state;
		dwc3_resume_work(&mdwc->resume_work.work);
	}
}

static irqreturn_t dwc3_pmic_id_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;
	enum dwc3_id_state id;

	/* If we can't read ID line state for some reason, treat it as float */
	id = !!irq_read_line(irq);
	if (mdwc->id_state != id) {
		mdwc->id_state = id;
		queue_work(system_nrt_wq, &mdwc->id_work);
	}

	return IRQ_HANDLED;
}

static void dwc3_adc_notification(enum qpnp_tm_state state, void *ctx)
{
	struct dwc3_msm *mdwc = ctx;

	if (state >= ADC_TM_STATE_NUM) {
		pr_err("%s: invalid notification %d\n", __func__, state);
		return;
	}

	dev_dbg(mdwc->dev, "%s: state = %s\n", __func__,
			state == ADC_TM_HIGH_STATE ? "high" : "low");

	/* save ID state, but don't necessarily notify OTG */
	if (state == ADC_TM_HIGH_STATE) {
		mdwc->id_state = DWC3_ID_FLOAT;
		mdwc->adc_param.state_request = ADC_TM_LOW_THR_ENABLE;
	} else {
		mdwc->id_state = DWC3_ID_GROUND;
		mdwc->adc_param.state_request = ADC_TM_HIGH_THR_ENABLE;
	}

	dwc3_id_work(&mdwc->id_work);

	/* re-arm ADC interrupt */
	qpnp_adc_tm_usbid_configure(&mdwc->adc_param);
}

static void dwc3_init_adc_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
							init_adc_work.work);
	int ret;

	ret = qpnp_adc_tm_is_ready();
	if (ret == -EPROBE_DEFER) {
		queue_delayed_work(system_nrt_wq, to_delayed_work(w),
					msecs_to_jiffies(100));
		return;
	}

	mdwc->adc_param.low_thr = adc_low_threshold;
	mdwc->adc_param.high_thr = adc_high_threshold;
	mdwc->adc_param.timer_interval = adc_meas_interval;
	mdwc->adc_param.state_request = ADC_TM_HIGH_LOW_THR_ENABLE;
	mdwc->adc_param.btm_ctx = mdwc;
	mdwc->adc_param.threshold_notification = dwc3_adc_notification;

	ret = qpnp_adc_tm_usbid_configure(&mdwc->adc_param);
	if (ret) {
		dev_err(mdwc->dev, "%s: request ADC error %d\n", __func__, ret);
		return;
	}

	mdwc->id_adc_detect = true;
}

static ssize_t adc_enable_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%s\n", context->id_adc_detect ?
						"enabled" : "disabled");
}

static ssize_t adc_enable_store(struct device *dev,
		struct device_attribute *attr, const char
		*buf, size_t size)
{
	if (!strnicmp(buf, "enable", 6)) {
		if (!context->id_adc_detect)
			dwc3_init_adc_work(&context->init_adc_work.work);
		return size;
	} else if (!strnicmp(buf, "disable", 7)) {
		qpnp_adc_tm_usbid_end();
		context->id_adc_detect = false;
		return size;
	}

	return -EINVAL;
}

static DEVICE_ATTR(adc_enable, S_IRUGO | S_IWUSR, adc_enable_show,
		adc_enable_store);

static int __devinit dwc3_msm_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct dwc3_msm *msm;
	struct resource *res;
	void __iomem *tcsr;
	unsigned long flags;
	int ret = 0;
	int len = 0;
	u32 tmp[3];

	msm = devm_kzalloc(&pdev->dev, sizeof(*msm), GFP_KERNEL);
	if (!msm) {
		dev_err(&pdev->dev, "not enough memory\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, msm);
	context = msm;
	msm->dev = &pdev->dev;

	INIT_LIST_HEAD(&msm->req_complete_list);
	INIT_DELAYED_WORK(&msm->chg_work, dwc3_chg_detect_work);
	INIT_DELAYED_WORK(&msm->resume_work, dwc3_resume_work);
	INIT_WORK(&msm->restart_usb_work, dwc3_restart_usb_work);
	INIT_WORK(&msm->id_work, dwc3_id_work);
	INIT_DELAYED_WORK(&msm->init_adc_work, dwc3_init_adc_work);

	ret = dwc3_msm_config_gdsc(msm, 1);
	if (ret) {
		dev_err(&pdev->dev, "unable to configure usb3 gdsc\n");
		return ret;
	}

	msm->xo_clk = clk_get(&pdev->dev, "xo");
	if (IS_ERR(msm->xo_clk)) {
		dev_err(&pdev->dev, "%s unable to get TCXO buffer handle\n",
								__func__);
		ret = PTR_ERR(msm->xo_clk);
		goto disable_dwc3_gdsc;
	}

	ret = clk_prepare_enable(msm->xo_clk);
	if (ret) {
		dev_err(&pdev->dev, "%s failed to vote for TCXO buffer%d\n",
						__func__, ret);
		goto put_xo;
	}

	/*
	 * DWC3 Core requires its CORE CLK (aka master / bus clk) to
	 * run at 125Mhz in SSUSB mode and >60MHZ for HSUSB mode.
	 */
	msm->core_clk = devm_clk_get(&pdev->dev, "core_clk");
	if (IS_ERR(msm->core_clk)) {
		dev_err(&pdev->dev, "failed to get core_clk\n");
		ret = PTR_ERR(msm->core_clk);
		goto disable_xo;
	}
	clk_set_rate(msm->core_clk, 125000000);
	clk_prepare_enable(msm->core_clk);

	msm->iface_clk = devm_clk_get(&pdev->dev, "iface_clk");
	if (IS_ERR(msm->iface_clk)) {
		dev_err(&pdev->dev, "failed to get iface_clk\n");
		ret = PTR_ERR(msm->iface_clk);
		goto disable_core_clk;
	}
	clk_prepare_enable(msm->iface_clk);

	msm->sleep_clk = devm_clk_get(&pdev->dev, "sleep_clk");
	if (IS_ERR(msm->sleep_clk)) {
		dev_err(&pdev->dev, "failed to get sleep_clk\n");
		ret = PTR_ERR(msm->sleep_clk);
		goto disable_iface_clk;
	}
	clk_prepare_enable(msm->sleep_clk);

	msm->hsphy_sleep_clk = devm_clk_get(&pdev->dev, "sleep_a_clk");
	if (IS_ERR(msm->hsphy_sleep_clk)) {
		dev_err(&pdev->dev, "failed to get sleep_a_clk\n");
		ret = PTR_ERR(msm->hsphy_sleep_clk);
		goto disable_sleep_clk;
	}
	clk_prepare_enable(msm->hsphy_sleep_clk);

	msm->utmi_clk = devm_clk_get(&pdev->dev, "utmi_clk");
	if (IS_ERR(msm->utmi_clk)) {
		dev_err(&pdev->dev, "failed to get utmi_clk\n");
		ret = PTR_ERR(msm->utmi_clk);
		goto disable_sleep_a_clk;
	}
	clk_prepare_enable(msm->utmi_clk);

	msm->ref_clk = devm_clk_get(&pdev->dev, "ref_clk");
	if (IS_ERR(msm->ref_clk)) {
		dev_err(&pdev->dev, "failed to get ref_clk\n");
		ret = PTR_ERR(msm->ref_clk);
		goto disable_utmi_clk;
	}
	clk_prepare_enable(msm->ref_clk);

	of_get_property(node, "qcom,vdd-voltage-level", &len);
	if (len == sizeof(tmp)) {
		of_property_read_u32_array(node, "qcom,vdd-voltage-level",
							tmp, len/sizeof(*tmp));
		msm->vdd_no_vol_level = tmp[0];
		msm->vdd_low_vol_level = tmp[1];
		msm->vdd_high_vol_level = tmp[2];
	} else {
		dev_err(&pdev->dev, "no qcom,vdd-voltage-level property\n");
		ret = -EINVAL;
		goto disable_ref_clk;
	}

	/* SS PHY */
	msm->ssusb_vddcx = devm_regulator_get(&pdev->dev, "ssusb_vdd_dig");
	if (IS_ERR(msm->ssusb_vddcx)) {
		dev_err(&pdev->dev, "unable to get ssusb vddcx\n");
		ret = PTR_ERR(msm->ssusb_vddcx);
		goto disable_ref_clk;
	}

	ret = dwc3_ssusb_config_vddcx(1);
	if (ret) {
		dev_err(&pdev->dev, "ssusb vddcx configuration failed\n");
		goto disable_ref_clk;
	}

	ret = regulator_enable(context->ssusb_vddcx);
	if (ret) {
		dev_err(&pdev->dev, "unable to enable the ssusb vddcx\n");
		goto unconfig_ss_vddcx;
	}

	ret = dwc3_ssusb_ldo_init(1);
	if (ret) {
		dev_err(&pdev->dev, "ssusb vreg configuration failed\n");
		goto disable_ss_vddcx;
	}

	ret = dwc3_ssusb_ldo_enable(1);
	if (ret) {
		dev_err(&pdev->dev, "ssusb vreg enable failed\n");
		goto free_ss_ldo_init;
	}

	/* HS PHY */
	msm->hsusb_vddcx = devm_regulator_get(&pdev->dev, "hsusb_vdd_dig");
	if (IS_ERR(msm->hsusb_vddcx)) {
		dev_err(&pdev->dev, "unable to get hsusb vddcx\n");
		ret = PTR_ERR(msm->hsusb_vddcx);
		goto disable_ss_ldo;
	}

	ret = dwc3_hsusb_config_vddcx(1);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vddcx configuration failed\n");
		goto disable_ss_ldo;
	}

	ret = regulator_enable(context->hsusb_vddcx);
	if (ret) {
		dev_err(&pdev->dev, "unable to enable the hsusb vddcx\n");
		goto unconfig_hs_vddcx;
	}

	ret = dwc3_hsusb_ldo_init(1);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vreg configuration failed\n");
		goto disable_hs_vddcx;
	}

	ret = dwc3_hsusb_ldo_enable(1);
	if (ret) {
		dev_err(&pdev->dev, "hsusb vreg enable failed\n");
		goto free_hs_ldo_init;
	}

	msm->id_state = msm->ext_xceiv.id = DWC3_ID_FLOAT;
	msm->ext_xceiv.otg_capability = of_property_read_bool(node,
				"qcom,otg-capability");
	msm->charger.charging_disabled = of_property_read_bool(node,
				"qcom,charging-disabled");

	msm->charger.skip_chg_detect = of_property_read_bool(node,
				"qcom,skip-charger-detection");
	/*
	 * DWC3 has separate IRQ line for OTG events (ID/BSV) and for
	 * DP and DM linestate transitions during low power mode.
	 */
	msm->hs_phy_irq = platform_get_irq_byname(pdev, "hs_phy_irq");
	if (msm->hs_phy_irq < 0) {
		dev_dbg(&pdev->dev, "pget_irq for hs_phy_irq failed\n");
		msm->hs_phy_irq = 0;
	} else {
		ret = devm_request_irq(&pdev->dev, msm->hs_phy_irq,
				msm_dwc3_irq, IRQF_TRIGGER_RISING,
			       "msm_dwc3", msm);
		if (ret) {
			dev_err(&pdev->dev, "irqreq HSPHYINT failed\n");
			goto disable_hs_ldo;
		}
		/* Leave the irq line disabled. It is only used for USB host
		   mode suspend. i.e device plug in to the OTG cable
		*/
		disable_irq(msm->hs_phy_irq);
	}

	if (msm->ext_xceiv.otg_capability) {
		msm->pmic_id_irq = platform_get_irq_byname(pdev, "pmic_id_irq");
		if (msm->pmic_id_irq > 0) {
			/* check if PMIC ID IRQ is supported */
			ret = qpnp_misc_irqs_available(&pdev->dev);

			if (ret == -EPROBE_DEFER) {
				/* qpnp hasn't probed yet; defer dwc probe */
				goto disable_hs_ldo;
			} else if (ret == 0) {
				msm->pmic_id_irq = 0;
			} else {
				ret = devm_request_irq(&pdev->dev,
						       msm->pmic_id_irq,
						       dwc3_pmic_id_irq,
						       IRQF_TRIGGER_RISING |
						       IRQF_TRIGGER_FALLING,
						       "dwc3_msm_pmic_id", msm);
				if (ret) {
					dev_err(&pdev->dev, "irqreq IDINT failed\n");
					goto disable_hs_ldo;
				}

				local_irq_save(flags);
				/* Update initial ID state */
				msm->id_state =
					!!irq_read_line(msm->pmic_id_irq);
				if (msm->id_state == DWC3_ID_GROUND)
					queue_work(system_nrt_wq,
							&msm->id_work);
				local_irq_restore(flags);
				enable_irq_wake(msm->pmic_id_irq);
			}
		}

		if (msm->pmic_id_irq <= 0) {
			/* If no PMIC ID IRQ, use ADC for ID pin detection */
			queue_work(system_nrt_wq, &msm->init_adc_work.work);
			device_create_file(&pdev->dev, &dev_attr_adc_enable);
			msm->pmic_id_irq = 0;
		}
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_dbg(&pdev->dev, "missing TCSR memory resource\n");
	} else {
		tcsr = devm_ioremap_nocache(&pdev->dev, res->start,
			resource_size(res));
		if (!tcsr) {
			dev_dbg(&pdev->dev, "tcsr ioremap failed\n");
		} else {
			/* Enable USB3 on the primary USB port. */
			writel_relaxed(0x1, tcsr);
			/*
			 * Ensure that TCSR write is completed before
			 * USB registers initialization.
			 */
			mb();
		}
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing memory base resource\n");
		ret = -ENODEV;
		goto disable_hs_ldo;
	}

	msm->base = devm_ioremap_nocache(&pdev->dev, res->start,
		resource_size(res));
	if (!msm->base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENODEV;
		goto disable_hs_ldo;
	}

	msm->resource_size = resource_size(res);

	if (of_property_read_u32(node, "qcom,dwc-hsphy-init",
						&msm->hsphy_init_seq))
		dev_dbg(&pdev->dev, "unable to read hsphy init seq\n");
	else if (!msm->hsphy_init_seq)
		dev_warn(&pdev->dev, "incorrect hsphyinitseq.Using PORvalue\n");

	if (of_property_read_u32(node, "qcom,dwc-hsphy-host-init",
						&msm->hsphy_host_init_seq))
		dev_dbg(&pdev->dev, "Unable to read hsphy host init seq\n");

	dwc3_msm_qscratch_reg_init(msm);

	pm_runtime_set_active(msm->dev);
	pm_runtime_enable(msm->dev);

	if (of_property_read_u32(node, "qcom,dwc-usb3-msm-dbm-eps",
				 &msm->dbm_num_eps)) {
		dev_err(&pdev->dev,
			"unable to read platform data num of dbm eps\n");
		msm->dbm_num_eps = DBM_MAX_EPS;
	}

	if (msm->dbm_num_eps > DBM_MAX_EPS) {
		dev_err(&pdev->dev,
			"Driver doesn't support number of DBM EPs. "
			"max: %d, dbm_num_eps: %d\n",
			DBM_MAX_EPS, msm->dbm_num_eps);
		ret = -ENODEV;
		goto disable_hs_ldo;
	}

	/* usb_psy required only for vbus_notifications or charging support */
	if (msm->ext_xceiv.otg_capability || !msm->charger.charging_disabled) {
		if (!of_property_read_u32(node,
					"qcom,dwc-usb3-msm-adc-low-threshold",
					&adc_low_threshold)) {
			dev_info(&pdev->dev,
				"Read platform data for adc low threshold\n");
		}

		if (!of_property_read_u32(node,
					"qcom,dwc-usb3-msm-adc-high-threshold",
					&adc_high_threshold)) {
			dev_info(&pdev->dev,
				"Read platform data for adc high threshold\n");
		}

		msm->usb_psy.name = "usb";
		msm->usb_psy.type = POWER_SUPPLY_TYPE_USB;
		msm->usb_psy.supplied_to = dwc3_msm_pm_power_supplied_to;
		msm->usb_psy.num_supplicants = ARRAY_SIZE(
						dwc3_msm_pm_power_supplied_to);
		msm->usb_psy.properties = dwc3_msm_pm_power_props_usb;
		msm->usb_psy.num_properties =
					ARRAY_SIZE(dwc3_msm_pm_power_props_usb);
		msm->usb_psy.get_property = dwc3_msm_power_get_property_usb;
		msm->usb_psy.set_property = dwc3_msm_power_set_property_usb;
		msm->usb_psy.external_power_changed =
					dwc3_msm_external_power_changed;

		ret = power_supply_register(&pdev->dev, &msm->usb_psy);
		if (ret < 0) {
			dev_err(&pdev->dev,
					"%s:power_supply_register usb failed\n",
						__func__);
			goto disable_hs_ldo;
		}
	}

	if (node) {
		ret = of_platform_populate(node, NULL, NULL, &pdev->dev);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to add create dwc3 core\n");
			goto put_psupply;
		}
	}

	msm->bus_scale_table = msm_bus_cl_get_pdata(pdev);
	if (!msm->bus_scale_table) {
		dev_err(&pdev->dev, "bus scaling is disabled\n");
	} else {
		msm->bus_perf_client =
			msm_bus_scale_register_client(msm->bus_scale_table);
		ret = msm_bus_scale_client_update_request(
						msm->bus_perf_client, 1);
		if (ret)
			dev_err(&pdev->dev, "Failed to vote for bus scaling\n");
	}

	msm->otg_xceiv = usb_get_transceiver();
	/* Register with OTG if present, ignore USB2 OTG using other PHY */
	if (msm->otg_xceiv && !(msm->otg_xceiv->flags & ENABLE_SECONDARY_PHY)) {
		/* Skip charger detection for simulator targets */
		if (!msm->charger.skip_chg_detect) {
			msm->charger.start_detection = dwc3_start_chg_det;
			ret = dwc3_set_charger(msm->otg_xceiv->otg,
					&msm->charger);
			if (ret || !msm->charger.notify_detection_complete) {
				dev_err(&pdev->dev,
					"failed to register charger: %d\n",
					ret);
				goto put_xcvr;
			}
		}

		if (msm->ext_xceiv.otg_capability)
			msm->ext_xceiv.ext_block_reset = dwc3_msm_block_reset;
		if (msm->hsphy_host_init_seq)
			msm->ext_xceiv.ext_hsphy_host_init_seq =
				dwc3_msm_hsphy_host_init_seq;
		ret = dwc3_set_ext_xceiv(msm->otg_xceiv->otg, &msm->ext_xceiv);
		if (ret || !msm->ext_xceiv.notify_ext_events) {
			dev_err(&pdev->dev, "failed to register xceiver: %d\n",
									ret);
			goto put_xcvr;
		}
	} else {
		dev_dbg(&pdev->dev, "No OTG, DWC3 running in host only mode\n");
		msm->host_mode = 1;
		msm->vbus_otg = devm_regulator_get(&pdev->dev, "vbus_dwc3");
		if (IS_ERR(msm->vbus_otg)) {
			dev_dbg(&pdev->dev, "Failed to get vbus regulator\n");
			msm->vbus_otg = 0;
		} else {
			ret = regulator_enable(msm->vbus_otg);
			if (ret) {
				msm->vbus_otg = 0;
				dev_err(&pdev->dev, "Failed to enable vbus_otg\n");
			}
		}
		msm->otg_xceiv = NULL;
	}

	device_init_wakeup(msm->dev, 1);
	pm_stay_awake(msm->dev);
	dwc3_debugfs_init(msm);

	return 0;

put_xcvr:
	usb_put_transceiver(msm->otg_xceiv);
put_psupply:
	if (msm->usb_psy.dev)
		power_supply_unregister(&msm->usb_psy);
disable_hs_ldo:
	dwc3_hsusb_ldo_enable(0);
free_hs_ldo_init:
	dwc3_hsusb_ldo_init(0);
disable_hs_vddcx:
	regulator_disable(context->hsusb_vddcx);
unconfig_hs_vddcx:
	dwc3_hsusb_config_vddcx(0);
disable_ss_ldo:
	dwc3_ssusb_ldo_enable(0);
free_ss_ldo_init:
	dwc3_ssusb_ldo_init(0);
disable_ss_vddcx:
	regulator_disable(context->ssusb_vddcx);
unconfig_ss_vddcx:
	dwc3_ssusb_config_vddcx(0);
disable_ref_clk:
	clk_disable_unprepare(msm->ref_clk);
disable_utmi_clk:
	clk_disable_unprepare(msm->utmi_clk);
disable_sleep_a_clk:
	clk_disable_unprepare(msm->hsphy_sleep_clk);
disable_sleep_clk:
	clk_disable_unprepare(msm->sleep_clk);
disable_iface_clk:
	clk_disable_unprepare(msm->iface_clk);
disable_core_clk:
	clk_disable_unprepare(msm->core_clk);
disable_xo:
	clk_disable_unprepare(msm->xo_clk);
put_xo:
	clk_put(msm->xo_clk);
disable_dwc3_gdsc:
	dwc3_msm_config_gdsc(msm, 0);

	return ret;
}

static int __devexit dwc3_msm_remove(struct platform_device *pdev)
{
	struct dwc3_msm	*msm = platform_get_drvdata(pdev);

	if (msm->id_adc_detect)
		qpnp_adc_tm_usbid_end();
	if (dwc3_debugfs_root)
		debugfs_remove_recursive(dwc3_debugfs_root);
	if (msm->otg_xceiv) {
		dwc3_start_chg_det(&msm->charger, false);
		usb_put_transceiver(msm->otg_xceiv);
	}
	if (msm->usb_psy.dev)
		power_supply_unregister(&msm->usb_psy);
	if (msm->vbus_otg)
		regulator_disable(msm->vbus_otg);

	pm_runtime_disable(msm->dev);
	device_init_wakeup(msm->dev, 0);

	dwc3_hsusb_ldo_enable(0);
	dwc3_hsusb_ldo_init(0);
	regulator_disable(msm->hsusb_vddcx);
	dwc3_hsusb_config_vddcx(0);
	dwc3_ssusb_ldo_enable(0);
	dwc3_ssusb_ldo_init(0);
	regulator_disable(msm->ssusb_vddcx);
	dwc3_ssusb_config_vddcx(0);
	clk_disable_unprepare(msm->core_clk);
	clk_disable_unprepare(msm->iface_clk);
	clk_disable_unprepare(msm->sleep_clk);
	clk_disable_unprepare(msm->hsphy_sleep_clk);
	clk_disable_unprepare(msm->ref_clk);
	clk_disable_unprepare(msm->xo_clk);
	clk_put(msm->xo_clk);

	dwc3_msm_config_gdsc(msm, 0);

	return 0;
}

static int dwc3_msm_pm_suspend(struct device *dev)
{
	int ret = 0;
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "dwc3-msm PM suspend\n");

	flush_delayed_work_sync(&mdwc->resume_work);
	if (!atomic_read(&mdwc->in_lpm)) {
		dev_err(mdwc->dev, "Abort PM suspend!! (USB is outside LPM)\n");
		return -EBUSY;
	}

	ret = dwc3_msm_suspend(mdwc);
	if (!ret)
		atomic_set(&mdwc->pm_suspended, 1);

	return ret;
}

static int dwc3_msm_pm_resume(struct device *dev)
{
	int ret = 0;
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "dwc3-msm PM resume\n");

	atomic_set(&mdwc->pm_suspended, 0);
	if (mdwc->resume_pending) {
		mdwc->resume_pending = false;

		ret = dwc3_msm_resume(mdwc);
		/* Update runtime PM status */
		pm_runtime_disable(dev);
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);

		/* Let OTG know about resume event and update pm_count */
		if (mdwc->otg_xceiv) {
			mdwc->ext_xceiv.notify_ext_events(mdwc->otg_xceiv->otg,
							DWC3_EVENT_PHY_RESUME);
			if (mdwc->ext_xceiv.otg_capability)
				mdwc->ext_xceiv.notify_ext_events(
							mdwc->otg_xceiv->otg,
							DWC3_EVENT_XCEIV_STATE);
		}
	}

	return ret;
}

static int dwc3_msm_runtime_idle(struct device *dev)
{
	dev_dbg(dev, "DWC3-msm runtime idle\n");

	return 0;
}

static int dwc3_msm_runtime_suspend(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "DWC3-msm runtime suspend\n");

	return dwc3_msm_suspend(mdwc);
}

static int dwc3_msm_runtime_resume(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "DWC3-msm runtime resume\n");

	return dwc3_msm_resume(mdwc);
}

static const struct dev_pm_ops dwc3_msm_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_msm_pm_suspend, dwc3_msm_pm_resume)
	SET_RUNTIME_PM_OPS(dwc3_msm_runtime_suspend, dwc3_msm_runtime_resume,
				dwc3_msm_runtime_idle)
};

static const struct of_device_id of_dwc3_matach[] = {
	{
		.compatible = "qcom,dwc-usb3-msm",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_dwc3_matach);

static struct platform_driver dwc3_msm_driver = {
	.probe		= dwc3_msm_probe,
	.remove		= __devexit_p(dwc3_msm_remove),
	.driver		= {
		.name	= "msm-dwc3",
		.pm	= &dwc3_msm_dev_pm_ops,
		.of_match_table	= of_dwc3_matach,
	},
};

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 MSM Glue Layer");

static int __devinit dwc3_msm_init(void)
{
	return platform_driver_register(&dwc3_msm_driver);
}
module_init(dwc3_msm_init);

static void __exit dwc3_msm_exit(void)
{
	platform_driver_unregister(&dwc3_msm_driver);
}
module_exit(dwc3_msm_exit);
