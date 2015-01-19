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
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/spmi.h>
#include <linux/of_irq.h>
#include <linux/wakelock.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/hwmon-sysfs.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/platform_device.h>

/* QPNP IADC register definition */
#define QPNP_IADC_REVISION1				0x0
#define QPNP_IADC_REVISION2				0x1
#define QPNP_IADC_REVISION3				0x2
#define QPNP_IADC_REVISION4				0x3
#define QPNP_IADC_PERPH_TYPE				0x4
#define QPNP_IADC_PERH_SUBTYPE				0x5

#define QPNP_IADC_SUPPORTED_REVISION2			1

#define QPNP_STATUS1					0x8
#define QPNP_STATUS1_OP_MODE				4
#define QPNP_STATUS1_MULTI_MEAS_EN			BIT(3)
#define QPNP_STATUS1_MEAS_INTERVAL_EN_STS		BIT(2)
#define QPNP_STATUS1_REQ_STS				BIT(1)
#define QPNP_STATUS1_EOC				BIT(0)
#define QPNP_STATUS1_REQ_STS_EOC_MASK			0x3
#define QPNP_STATUS2					0x9
#define QPNP_STATUS2_CONV_SEQ_STATE_SHIFT		4
#define QPNP_STATUS2_FIFO_NOT_EMPTY_FLAG		BIT(1)
#define QPNP_STATUS2_CONV_SEQ_TIMEOUT_STS		BIT(0)
#define QPNP_CONV_TIMEOUT_ERR				2

#define QPNP_IADC_MODE_CTL				0x40
#define QPNP_OP_MODE_SHIFT				4
#define QPNP_USE_BMS_DATA				BIT(4)
#define QPNP_VADC_SYNCH_EN				BIT(2)
#define QPNP_OFFSET_RMV_EN				BIT(1)
#define QPNP_ADC_TRIM_EN				BIT(0)
#define QPNP_IADC_EN_CTL1				0x46
#define QPNP_IADC_ADC_EN				BIT(7)
#define QPNP_ADC_CH_SEL_CTL				0x48
#define QPNP_ADC_DIG_PARAM				0x50
#define QPNP_ADC_CLK_SEL_MASK				0x3
#define QPNP_ADC_DEC_RATIO_SEL_MASK			0xc
#define QPNP_ADC_DIG_DEC_RATIO_SEL_SHIFT		2

#define QPNP_HW_SETTLE_DELAY				0x51
#define QPNP_CONV_REQ					0x52
#define QPNP_CONV_REQ_SET				BIT(7)
#define QPNP_CONV_SEQ_CTL				0x54
#define QPNP_CONV_SEQ_HOLDOFF_SHIFT			4
#define QPNP_CONV_SEQ_TRIG_CTL				0x55
#define QPNP_FAST_AVG_CTL				0x5a

#define QPNP_M0_LOW_THR_LSB				0x5c
#define QPNP_M0_LOW_THR_MSB				0x5d
#define QPNP_M0_HIGH_THR_LSB				0x5e
#define QPNP_M0_HIGH_THR_MSB				0x5f
#define QPNP_M1_LOW_THR_LSB				0x69
#define QPNP_M1_LOW_THR_MSB				0x6a
#define QPNP_M1_HIGH_THR_LSB				0x6b
#define QPNP_M1_HIGH_THR_MSB				0x6c

#define QPNP_DATA0					0x60
#define QPNP_DATA1					0x61
#define QPNP_CONV_TIMEOUT_ERR				2

#define QPNP_IADC_SEC_ACCESS				0xD0
#define QPNP_IADC_SEC_ACCESS_DATA			0xA5
#define QPNP_IADC_MSB_OFFSET				0xF2
#define QPNP_IADC_LSB_OFFSET				0xF3
#define QPNP_IADC_NOMINAL_RSENSE			0xF4
#define QPNP_IADC_ATE_GAIN_CALIB_OFFSET			0xF5
#define QPNP_INT_TEST_VAL				0xE1

#define QPNP_IADC_ADC_CH_SEL_CTL			0x48
#define QPNP_IADC_ADC_CHX_SEL_SHIFT			3

#define QPNP_IADC_ADC_DIG_PARAM				0x50
#define QPNP_IADC_CLK_SEL_SHIFT				1
#define QPNP_IADC_DEC_RATIO_SEL				3

#define QPNP_IADC_CONV_REQUEST				0x52
#define QPNP_IADC_CONV_REQ				BIT(7)

#define QPNP_IADC_DATA0					0x60
#define QPNP_IADC_DATA1					0x61

#define QPNP_ADC_CONV_TIME_MIN				2000
#define QPNP_ADC_CONV_TIME_MAX				2100
#define QPNP_ADC_ERR_COUNT				20

#define QPNP_ADC_GAIN_NV				17857
#define QPNP_OFFSET_CALIBRATION_SHORT_CADC_LEADS_IDEAL	0
#define QPNP_IADC_INTERNAL_RSENSE_N_OHMS_FACTOR		10000000
#define QPNP_IADC_NANO_VOLTS_FACTOR			1000000
#define QPNP_IADC_CALIB_SECONDS				300000
#define QPNP_IADC_RSENSE_LSB_N_OHMS_PER_BIT		15625
#define QPNP_IADC_DIE_TEMP_CALIB_OFFSET			5000

#define QPNP_RAW_CODE_16_BIT_MSB_MASK			0xff00
#define QPNP_RAW_CODE_16_BIT_LSB_MASK			0xff
#define QPNP_BIT_SHIFT_8				8
#define QPNP_RSENSE_MSB_SIGN_CHECK			0x80
#define QPNP_ADC_COMPLETION_TIMEOUT			HZ

struct qpnp_iadc_comp {
	bool	ext_rsense;
	u8	id;
	u8	sys_gain;
	u8	revision;
};

struct qpnp_iadc_drv {
	struct device				*dev;
	struct qpnp_adc_drv			*adc;
	int32_t					rsense;
	bool					external_rsense;
	struct device				*iadc_hwmon;
	bool					iadc_initialized;
	int64_t					die_temp;
	struct delayed_work			iadc_work;
	struct mutex				iadc_vadc_lock;
	bool					iadc_mode_sel;
	struct qpnp_iadc_comp			iadc_comp;
	bool					iadc_poll_eoc;
	struct sensor_device_attribute		sens_attr[0];
};

struct qpnp_iadc_drv	*qpnp_iadc;

static int32_t qpnp_iadc_read_reg(uint32_t reg, u8 *data)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	int rc;

	rc = spmi_ext_register_readl(iadc->adc->spmi->ctrl, iadc->adc->slave,
		(iadc->adc->offset + reg), data, 1);
	if (rc < 0) {
		pr_err("qpnp iadc read reg %d failed with %d\n", reg, rc);
		return rc;
	}

	return 0;
}

static int32_t qpnp_iadc_write_reg(uint32_t reg, u8 data)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	int rc;
	u8 *buf;

	buf = &data;
	rc = spmi_ext_register_writel(iadc->adc->spmi->ctrl, iadc->adc->slave,
		(iadc->adc->offset + reg), buf, 1);
	if (rc < 0) {
		pr_err("qpnp iadc write reg %d failed with %d\n", reg, rc);
		return rc;
	}

	return 0;
}

static void trigger_iadc_completion(struct work_struct *work)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;

	if (!iadc || !iadc->iadc_initialized)
		return;

	complete(&iadc->adc->adc_rslt_completion);

	return;
}
DECLARE_WORK(trigger_iadc_completion_work, trigger_iadc_completion);

static irqreturn_t qpnp_iadc_isr(int irq, void *dev_id)
{
	schedule_work(&trigger_iadc_completion_work);

	return IRQ_HANDLED;
}

static int32_t qpnp_iadc_enable(bool state)
{
	int rc = 0;
	u8 data = 0;

	data = QPNP_IADC_ADC_EN;
	if (state) {
		rc = qpnp_iadc_write_reg(QPNP_IADC_EN_CTL1,
					data);
		if (rc < 0) {
			pr_err("IADC enable failed\n");
			return rc;
		}
	} else {
		rc = qpnp_iadc_write_reg(QPNP_IADC_EN_CTL1,
					(~data & QPNP_IADC_ADC_EN));
		if (rc < 0) {
			pr_err("IADC disable failed\n");
			return rc;
		}
	}

	return 0;
}

static int32_t qpnp_iadc_status_debug(void)
{
	int rc = 0;
	u8 mode = 0, status1 = 0, chan = 0, dig = 0, en = 0;

	rc = qpnp_iadc_read_reg(QPNP_IADC_MODE_CTL, &mode);
	if (rc < 0) {
		pr_err("mode ctl register read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(QPNP_ADC_DIG_PARAM, &dig);
	if (rc < 0) {
		pr_err("digital param read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(QPNP_IADC_ADC_CH_SEL_CTL, &chan);
	if (rc < 0) {
		pr_err("channel read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(QPNP_STATUS1, &status1);
	if (rc < 0) {
		pr_err("status1 read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(QPNP_IADC_EN_CTL1, &en);
	if (rc < 0) {
		pr_err("en read failed with %d\n", rc);
		return rc;
	}

	pr_debug("EOC not set with status:%x, dig:%x, ch:%x, mode:%x, en:%x\n",
			status1, dig, chan, mode, en);

	rc = qpnp_iadc_enable(false);
	if (rc < 0) {
		pr_err("IADC disable failed with %d\n", rc);
		return rc;
	}

	return 0;
}

static int32_t qpnp_iadc_read_conversion_result(uint16_t *data)
{
	uint8_t rslt_lsb, rslt_msb;
	uint16_t rslt;
	int32_t rc;

	rc = qpnp_iadc_read_reg(QPNP_IADC_DATA0, &rslt_lsb);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(QPNP_IADC_DATA1, &rslt_msb);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		return rc;
	}

	rslt = (rslt_msb << 8) | rslt_lsb;
	*data = rslt;

	rc = qpnp_iadc_enable(false);
	if (rc)
		return rc;

	return 0;
}

static int32_t qpnp_iadc_comp(int64_t *result, struct qpnp_iadc_comp comp,
							int64_t die_temp)
{
	int64_t temp_var = 0, sign_coeff = 0, sys_gain_coeff = 0, old;

	old = *result;
	*result = *result * 1000000;

	if (comp.revision == QPNP_IADC_VER_3_1) {
		/* revision 3.1 */
		if (comp.sys_gain > 127)
			sys_gain_coeff = -QPNP_COEFF_6 * (comp.sys_gain - 128);
		else
			sys_gain_coeff = QPNP_COEFF_6 * comp.sys_gain;
	} else if (comp.revision != QPNP_IADC_VER_3_0) {
		/* unsupported revision, do not compensate */
		*result = old;
		return 0;
	}

	if (!comp.ext_rsense) {
		/* internal rsense */
		switch (comp.id) {
		case COMP_ID_TSMC:
			temp_var = ((QPNP_COEFF_2 * die_temp) -
						QPNP_COEFF_3_TYPEB);
		break;
		case COMP_ID_GF:
		default:
			temp_var = ((QPNP_COEFF_2 * die_temp) -
						QPNP_COEFF_3_TYPEA);
		break;
		}
		temp_var = div64_s64(temp_var, QPNP_COEFF_4);
		if (comp.revision == QPNP_IADC_VER_3_0)
			temp_var = QPNP_COEFF_1 * (1000000 - temp_var);
		else if (comp.revision == QPNP_IADC_VER_3_1)
			temp_var = 1000000 * (1000000 - temp_var);
		*result = div64_s64(*result * 1000000, temp_var);
	}

	sign_coeff = *result < 0 ? QPNP_COEFF_7 : QPNP_COEFF_5;
	if (comp.ext_rsense) {
		/* external rsense and current charging */
		temp_var = div64_s64((-sign_coeff * die_temp) + QPNP_COEFF_8,
						QPNP_COEFF_4);
		temp_var = 1000000000 - temp_var;
		if (comp.revision == QPNP_IADC_VER_3_1) {
			sys_gain_coeff = (1000000 +
				div64_s64(sys_gain_coeff, QPNP_COEFF_4));
			temp_var = div64_s64(temp_var * sys_gain_coeff,
				1000000000);
		}
		*result = div64_s64(*result, temp_var);
	}
	pr_debug("%lld compensated into %lld\n", old, *result);

	return 0;
}

int32_t qpnp_iadc_comp_result(int64_t *result)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;

	return qpnp_iadc_comp(result, iadc->iadc_comp, iadc->die_temp);
}
EXPORT_SYMBOL(qpnp_iadc_comp_result);

static int32_t qpnp_iadc_comp_info(void)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	int rc = 0;

	rc = qpnp_iadc_read_reg(QPNP_INT_TEST_VAL, &iadc->iadc_comp.id);
	if (rc < 0) {
		pr_err("qpnp adc comp id failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(QPNP_IADC_REVISION2, &iadc->iadc_comp.revision);
	if (rc < 0) {
		pr_err("qpnp adc revision read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(QPNP_IADC_ATE_GAIN_CALIB_OFFSET,
						&iadc->iadc_comp.sys_gain);
	if (rc < 0)
		pr_err("full scale read failed with %d\n", rc);

	pr_debug("fab id = %u, revision = %u, sys gain = %u, external_rsense = %d\n",
			iadc->iadc_comp.id,
			iadc->iadc_comp.revision,
			iadc->iadc_comp.sys_gain,
			iadc->iadc_comp.ext_rsense);
	return rc;
}

static int32_t qpnp_iadc_configure(enum qpnp_iadc_channels channel,
					uint16_t *raw_code, uint32_t mode_sel)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	u8 qpnp_iadc_mode_reg = 0, qpnp_iadc_ch_sel_reg = 0;
	u8 qpnp_iadc_conv_req = 0, qpnp_iadc_dig_param_reg = 0;
	u8 status1 = 0;
	uint32_t count = 0;
	int32_t rc = 0;

	qpnp_iadc_ch_sel_reg = channel;

	qpnp_iadc_dig_param_reg |= iadc->adc->amux_prop->decimation <<
					QPNP_IADC_DEC_RATIO_SEL;
	if (iadc->iadc_mode_sel)
		qpnp_iadc_mode_reg |= (QPNP_ADC_TRIM_EN | QPNP_VADC_SYNCH_EN);
	else
		qpnp_iadc_mode_reg |= QPNP_ADC_TRIM_EN;

	qpnp_iadc_conv_req = QPNP_IADC_CONV_REQ;

	rc = qpnp_iadc_write_reg(QPNP_IADC_MODE_CTL, qpnp_iadc_mode_reg);
	if (rc) {
		pr_err("qpnp adc read adc failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_write_reg(QPNP_IADC_ADC_CH_SEL_CTL,
						qpnp_iadc_ch_sel_reg);
	if (rc) {
		pr_err("qpnp adc read adc failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_write_reg(QPNP_ADC_DIG_PARAM,
						qpnp_iadc_dig_param_reg);
	if (rc) {
		pr_err("qpnp adc read adc failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_write_reg(QPNP_HW_SETTLE_DELAY,
				iadc->adc->amux_prop->hw_settle_time);
	if (rc < 0) {
		pr_err("qpnp adc configure error for hw settling time setup\n");
		return rc;
	}

	rc = qpnp_iadc_write_reg(QPNP_FAST_AVG_CTL,
					iadc->adc->amux_prop->fast_avg_setup);
	if (rc < 0) {
		pr_err("qpnp adc fast averaging configure error\n");
		return rc;
	}

	if (!iadc->iadc_poll_eoc)
		INIT_COMPLETION(iadc->adc->adc_rslt_completion);

	rc = qpnp_iadc_enable(true);
	if (rc)
		return rc;

	rc = qpnp_iadc_write_reg(QPNP_CONV_REQ, qpnp_iadc_conv_req);
	if (rc) {
		pr_err("qpnp adc read adc failed with %d\n", rc);
		return rc;
	}

	if (iadc->iadc_poll_eoc) {
		while (status1 != QPNP_STATUS1_EOC) {
			rc = qpnp_iadc_read_reg(QPNP_STATUS1, &status1);
			if (rc < 0)
				return rc;
			status1 &= QPNP_STATUS1_REQ_STS_EOC_MASK;
			usleep_range(QPNP_ADC_CONV_TIME_MIN,
					QPNP_ADC_CONV_TIME_MAX);
			count++;
			if (count > QPNP_ADC_ERR_COUNT) {
				pr_err("retry error exceeded\n");
				rc = qpnp_iadc_status_debug();
				if (rc < 0)
					pr_err("IADC status debug failed\n");
				rc = -EINVAL;
				return rc;
			}
		}
	} else {
		rc = wait_for_completion_timeout(
				&iadc->adc->adc_rslt_completion,
				QPNP_ADC_COMPLETION_TIMEOUT);
		if (!rc) {
			rc = qpnp_iadc_read_reg(QPNP_STATUS1, &status1);
			if (rc < 0)
				return rc;
			status1 &= QPNP_STATUS1_REQ_STS_EOC_MASK;
			if (status1 == QPNP_STATUS1_EOC)
				pr_debug("End of conversion status set\n");
			else {
				rc = qpnp_iadc_status_debug();
				if (rc < 0) {
					pr_err("status debug failed %d\n", rc);
					return rc;
				}
				return -EINVAL;
			}
		}
	}

	rc = qpnp_iadc_read_conversion_result(raw_code);
	if (rc) {
		pr_err("qpnp adc read adc failed with %d\n", rc);
		return rc;
	}

	return 0;
}

#define IADC_CENTER	0xC000
#define IADC_READING_RESOLUTION_N	542535
#define IADC_READING_RESOLUTION_D	100000
static int32_t qpnp_convert_raw_offset_voltage(void)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	s64 numerator;

	if ((iadc->adc->calib.gain_raw - iadc->adc->calib.offset_raw) == 0) {
		pr_err("raw offset errors! raw_gain:0x%x and raw_offset:0x%x\n",
			iadc->adc->calib.gain_raw, iadc->adc->calib.offset_raw);
		return -EINVAL;
	}

	numerator = iadc->adc->calib.offset_raw - IADC_CENTER;
	numerator *= IADC_READING_RESOLUTION_N;
	iadc->adc->calib.offset_uv = div_s64(numerator,
						IADC_READING_RESOLUTION_D);

	numerator = iadc->adc->calib.gain_raw - iadc->adc->calib.offset_raw;
	numerator *= IADC_READING_RESOLUTION_N;

	iadc->adc->calib.gain_uv = div_s64(numerator,
						IADC_READING_RESOLUTION_D);

	pr_debug("gain_uv:%d offset_uv:%d\n",
			iadc->adc->calib.gain_uv, iadc->adc->calib.offset_uv);
	return 0;
}

int32_t qpnp_iadc_calibrate_for_trim(void)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	uint8_t rslt_lsb, rslt_msb;
	int32_t rc = 0;
	uint16_t raw_data;
	uint32_t mode_sel = 0;

	mutex_lock(&iadc->adc->adc_lock);

	if (iadc->iadc_poll_eoc) {
		pr_debug("acquiring iadc eoc wakelock\n");
		pm_stay_awake(iadc->dev);
	}

	rc = qpnp_iadc_configure(GAIN_CALIBRATION_17P857MV,
						&raw_data, mode_sel);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		goto fail;
	}

	iadc->adc->calib.gain_raw = raw_data;

	rc = qpnp_iadc_configure(OFFSET_CALIBRATION_CSP2_CSN2,
						&raw_data, mode_sel);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		goto fail;
	}

	iadc->adc->calib.offset_raw = raw_data;
	if (rc < 0) {
		pr_err("qpnp adc offset/gain calculation failed\n");
		goto fail;
	}

	pr_debug("raw gain:0x%x, raw offset:0x%x\n",
		iadc->adc->calib.gain_raw, iadc->adc->calib.offset_raw);

	rc = qpnp_convert_raw_offset_voltage();
	if (rc < 0) {
		pr_err("qpnp raw_voltage conversion failed\n");
		goto fail;
	}

	rslt_msb = (raw_data & QPNP_RAW_CODE_16_BIT_MSB_MASK) >>
							QPNP_BIT_SHIFT_8;
	rslt_lsb = raw_data & QPNP_RAW_CODE_16_BIT_LSB_MASK;

	pr_debug("trim values:lsb:0x%x and msb:0x%x\n", rslt_lsb, rslt_msb);

	rc = qpnp_iadc_write_reg(QPNP_IADC_SEC_ACCESS,
					QPNP_IADC_SEC_ACCESS_DATA);
	if (rc < 0) {
		pr_err("qpnp iadc configure error for sec access\n");
		goto fail;
	}

	rc = qpnp_iadc_write_reg(QPNP_IADC_MSB_OFFSET,
						rslt_msb);
	if (rc < 0) {
		pr_err("qpnp iadc configure error for MSB write\n");
		goto fail;
	}

	rc = qpnp_iadc_write_reg(QPNP_IADC_SEC_ACCESS,
					QPNP_IADC_SEC_ACCESS_DATA);
	if (rc < 0) {
		pr_err("qpnp iadc configure error for sec access\n");
		goto fail;
	}

	rc = qpnp_iadc_write_reg(QPNP_IADC_LSB_OFFSET,
						rslt_lsb);
	if (rc < 0) {
		pr_err("qpnp iadc configure error for LSB write\n");
		goto fail;
	}
fail:
	if (iadc->iadc_poll_eoc) {
		pr_debug("releasing iadc eoc wakelock\n");
		pm_relax(iadc->dev);
	}
	mutex_unlock(&iadc->adc->adc_lock);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_calibrate_for_trim);

static void qpnp_iadc_work(struct work_struct *work)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	int rc = 0;

	rc = qpnp_iadc_calibrate_for_trim();
	if (rc)
		pr_debug("periodic IADC calibration failed\n");
	else
		schedule_delayed_work(&iadc->iadc_work,
			round_jiffies_relative(msecs_to_jiffies
					(QPNP_IADC_CALIB_SECONDS)));
	return;
}

static int32_t qpnp_iadc_version_check(void)
{
	uint8_t revision;
	int rc;

	rc = qpnp_iadc_read_reg(QPNP_IADC_REVISION2, &revision);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		return rc;
	}

	if (revision < QPNP_IADC_SUPPORTED_REVISION2) {
		pr_err("IADC Version not supported\n");
		return -EINVAL;
	}

	return 0;
}

int32_t qpnp_iadc_is_ready(void)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;

	if (!iadc || !iadc->iadc_initialized)
		return -EPROBE_DEFER;
	else
		return 0;
}
EXPORT_SYMBOL(qpnp_iadc_is_ready);

int32_t qpnp_iadc_get_rsense(int32_t *rsense)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	uint8_t	rslt_rsense;
	int32_t	rc = 0, sign_bit = 0;

	if (!iadc || !iadc->iadc_initialized)
		return -EPROBE_DEFER;

	if (iadc->external_rsense) {
		*rsense = iadc->rsense;
		return rc;
	}

	rc = qpnp_iadc_read_reg(QPNP_IADC_NOMINAL_RSENSE, &rslt_rsense);
	if (rc < 0) {
		pr_err("qpnp adc rsense read failed with %d\n", rc);
		return rc;
	}

	pr_debug("rsense:0%x\n", rslt_rsense);

	if (rslt_rsense & QPNP_RSENSE_MSB_SIGN_CHECK)
		sign_bit = 1;

	rslt_rsense &= ~QPNP_RSENSE_MSB_SIGN_CHECK;

	if (sign_bit)
		*rsense = QPNP_IADC_INTERNAL_RSENSE_N_OHMS_FACTOR -
			(rslt_rsense * QPNP_IADC_RSENSE_LSB_N_OHMS_PER_BIT);
	else
		*rsense = QPNP_IADC_INTERNAL_RSENSE_N_OHMS_FACTOR +
			(rslt_rsense * QPNP_IADC_RSENSE_LSB_N_OHMS_PER_BIT);

	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_get_rsense);

static int32_t qpnp_check_pmic_temp(void)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	struct qpnp_vadc_result result_pmic_therm;
	int64_t die_temp_offset;
	int rc = 0;

	rc = qpnp_vadc_read(DIE_TEMP, &result_pmic_therm);
	if (rc < 0)
		return rc;

	die_temp_offset = result_pmic_therm.physical -
			iadc->die_temp;
	if (die_temp_offset < 0)
		die_temp_offset = -die_temp_offset;

	if (die_temp_offset > QPNP_IADC_DIE_TEMP_CALIB_OFFSET) {
		iadc->die_temp =
			result_pmic_therm.physical;
		rc = qpnp_iadc_calibrate_for_trim();
		if (rc)
			pr_err("periodic IADC calibration failed\n");
	}

	return rc;
}

int32_t qpnp_iadc_read(enum qpnp_iadc_channels channel,
				struct qpnp_iadc_result *result)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	int32_t rc, rsense_n_ohms = 0, sign = 0, num, mode_sel = 0;
	int32_t rsense_u_ohms = 0;
	int64_t result_current;
	uint16_t raw_data;

	if (!iadc || !iadc->iadc_initialized)
		return -EPROBE_DEFER;

	if (!iadc->iadc_mode_sel) {
		rc = qpnp_check_pmic_temp();
		if (rc) {
			pr_err("Error checking pmic therm temp\n");
			return rc;
		}
	}

	mutex_lock(&iadc->adc->adc_lock);

	if (iadc->iadc_poll_eoc) {
		pr_debug("acquiring iadc eoc wakelock\n");
		pm_stay_awake(iadc->dev);
	}

	rc = qpnp_iadc_configure(channel, &raw_data, mode_sel);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		goto fail;
	}

	rc = qpnp_iadc_get_rsense(&rsense_n_ohms);
	pr_debug("current raw:0%x and rsense:%d\n",
			raw_data, rsense_n_ohms);
	rsense_u_ohms = rsense_n_ohms/1000;
	num = raw_data - iadc->adc->calib.offset_raw;
	if (num < 0) {
		sign = 1;
		num = -num;
	}

	result->result_uv = (num * QPNP_ADC_GAIN_NV)/
		(iadc->adc->calib.gain_raw - iadc->adc->calib.offset_raw);
	result_current = result->result_uv;
	result_current *= QPNP_IADC_NANO_VOLTS_FACTOR;
	/* Intentional fall through. Process the result w/o comp */
	do_div(result_current, rsense_u_ohms);

	if (sign) {
		result->result_uv = -result->result_uv;
		result_current = -result_current;
	}
	rc = qpnp_iadc_comp_result(&result_current);
	if (rc < 0)
		pr_err("Error during compensating the IADC\n");
	rc = 0;

	result->result_ua = (int32_t) result_current;
fail:
	if (iadc->iadc_poll_eoc) {
		pr_debug("releasing iadc eoc wakelock\n");
		pm_relax(iadc->dev);
	}
	mutex_unlock(&iadc->adc->adc_lock);

	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_read);

int32_t qpnp_iadc_get_gain_and_offset(struct qpnp_iadc_calib *result)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	int rc;

	if (!iadc || !iadc->iadc_initialized)
		return -EPROBE_DEFER;

	rc = qpnp_check_pmic_temp();
	if (rc) {
		pr_err("Error checking pmic therm temp\n");
		return rc;
	}

	mutex_lock(&iadc->adc->adc_lock);
	result->gain_raw = iadc->adc->calib.gain_raw;
	result->ideal_gain_nv = QPNP_ADC_GAIN_NV;
	result->gain_uv = iadc->adc->calib.gain_uv;
	result->offset_raw = iadc->adc->calib.offset_raw;
	result->ideal_offset_uv =
				QPNP_OFFSET_CALIBRATION_SHORT_CADC_LEADS_IDEAL;
	result->offset_uv = iadc->adc->calib.offset_uv;
	pr_debug("raw gain:0%x, raw offset:0%x\n",
			result->gain_raw, result->offset_raw);
	pr_debug("gain_uv:%d offset_uv:%d\n",
			result->gain_uv, result->offset_uv);
	mutex_unlock(&iadc->adc->adc_lock);

	return 0;
}
EXPORT_SYMBOL(qpnp_iadc_get_gain_and_offset);

int32_t qpnp_iadc_vadc_sync_read(
	enum qpnp_iadc_channels i_channel, struct qpnp_iadc_result *i_result,
	enum qpnp_vadc_channels v_channel, struct qpnp_vadc_result *v_result)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	int rc = 0;

	if (!iadc || !iadc->iadc_initialized)
		return -EPROBE_DEFER;

	mutex_lock(&iadc->iadc_vadc_lock);

	if (iadc->iadc_poll_eoc) {
		pr_debug("acquiring iadc eoc wakelock\n");
		pm_stay_awake(iadc->dev);
	}

	rc = qpnp_check_pmic_temp();
	if (rc) {
		pr_err("PMIC die temp check failed\n");
		goto fail;
	}

	iadc->iadc_mode_sel = true;

	rc = qpnp_vadc_iadc_sync_request(v_channel);
	if (rc) {
		pr_err("Configuring VADC failed\n");
		goto fail;
	}

	rc = qpnp_iadc_read(i_channel, i_result);
	if (rc)
		pr_err("Configuring IADC failed\n");
	/* Intentional fall through to release VADC */

	rc = qpnp_vadc_iadc_sync_complete_request(v_channel,
							v_result);
	if (rc)
		pr_err("Releasing VADC failed\n");
fail:
	iadc->iadc_mode_sel = false;

	if (iadc->iadc_poll_eoc) {
		pr_debug("releasing iadc eoc wakelock\n");
		pm_relax(iadc->dev);
	}

	mutex_unlock(&iadc->iadc_vadc_lock);

	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_vadc_sync_read);

static ssize_t qpnp_iadc_show(struct device *dev,
			struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct qpnp_iadc_result result;
	int rc = -1;

	rc = qpnp_iadc_read(attr->index, &result);

	if (rc)
		return 0;

	return snprintf(buf, QPNP_ADC_HWMON_NAME_LENGTH,
					"Result:%d\n", result.result_ua);
}

static struct sensor_device_attribute qpnp_adc_attr =
	SENSOR_ATTR(NULL, S_IRUGO, qpnp_iadc_show, NULL, 0);

static int32_t qpnp_iadc_init_hwmon(struct spmi_device *spmi)
{
	struct qpnp_iadc_drv *iadc = qpnp_iadc;
	struct device_node *child;
	struct device_node *node = spmi->dev.of_node;
	int rc = 0, i = 0, channel;

	for_each_child_of_node(node, child) {
		channel = iadc->adc->adc_channels[i].channel_num;
		qpnp_adc_attr.index = iadc->adc->adc_channels[i].channel_num;
		qpnp_adc_attr.dev_attr.attr.name =
						iadc->adc->adc_channels[i].name;
		memcpy(&iadc->sens_attr[i], &qpnp_adc_attr,
						sizeof(qpnp_adc_attr));
		sysfs_attr_init(&iadc->sens_attr[i].dev_attr.attr);
		rc = device_create_file(&spmi->dev,
				&iadc->sens_attr[i].dev_attr);
		if (rc) {
			dev_err(&spmi->dev,
				"device_create_file failed for dev %s\n",
				iadc->adc->adc_channels[i].name);
			goto hwmon_err_sens;
		}
		i++;
	}

	return 0;
hwmon_err_sens:
	pr_err("Init HWMON failed for qpnp_iadc with %d\n", rc);
	return rc;
}

static int __devinit qpnp_iadc_probe(struct spmi_device *spmi)
{
	struct qpnp_iadc_drv *iadc;
	struct qpnp_adc_drv *adc_qpnp;
	struct device_node *node = spmi->dev.of_node;
	struct device_node *child;
	int rc, count_adc_channel_list = 0;

	if (!node)
		return -EINVAL;

	if (qpnp_iadc) {
		pr_err("IADC already in use\n");
		return -EBUSY;
	}

	for_each_child_of_node(node, child)
		count_adc_channel_list++;

	if (!count_adc_channel_list) {
		pr_err("No channel listing\n");
		return -EINVAL;
	}

	iadc = devm_kzalloc(&spmi->dev, sizeof(struct qpnp_iadc_drv) +
		(sizeof(struct sensor_device_attribute) *
				count_adc_channel_list), GFP_KERNEL);
	if (!iadc) {
		dev_err(&spmi->dev, "Unable to allocate memory\n");
		return -ENOMEM;
	}

	adc_qpnp = devm_kzalloc(&spmi->dev, sizeof(struct qpnp_adc_drv),
			GFP_KERNEL);
	if (!adc_qpnp) {
		dev_err(&spmi->dev, "Unable to allocate memory\n");
		rc = -ENOMEM;
		goto fail;
	}

	iadc->dev = &(spmi->dev);
	iadc->adc = adc_qpnp;

	rc = qpnp_adc_get_devicetree_data(spmi, iadc->adc);
	if (rc) {
		dev_err(&spmi->dev, "failed to read device tree\n");
		goto fail;
	}

	mutex_init(&iadc->adc->adc_lock);

	rc = of_property_read_u32(node, "qcom,rsense",
			&iadc->rsense);
	if (rc)
		pr_debug("Defaulting to internal rsense\n");
	else {
		pr_debug("Use external rsense\n");
		iadc->external_rsense = true;
	}

	iadc->iadc_poll_eoc = of_property_read_bool(node,
						"qcom,iadc-poll-eoc");
	if (!iadc->iadc_poll_eoc) {
		rc = devm_request_irq(&spmi->dev, iadc->adc->adc_irq_eoc,
				qpnp_iadc_isr, IRQF_TRIGGER_RISING,
				"qpnp_iadc_interrupt", iadc);
		if (rc) {
			dev_err(&spmi->dev, "failed to request adc irq\n");
			return rc;
		} else
			enable_irq_wake(iadc->adc->adc_irq_eoc);
	}

	dev_set_drvdata(&spmi->dev, iadc);
	qpnp_iadc = iadc;

	rc = qpnp_iadc_init_hwmon(spmi);
	if (rc) {
		dev_err(&spmi->dev, "failed to initialize qpnp hwmon adc\n");
		goto fail;
	}
	iadc->iadc_hwmon = hwmon_device_register(&iadc->adc->spmi->dev);

	rc = qpnp_iadc_version_check();
	if (rc) {
		dev_err(&spmi->dev, "IADC version not supported\n");
		goto fail;
	}

	mutex_init(&iadc->iadc_vadc_lock);
	INIT_DELAYED_WORK(&iadc->iadc_work, qpnp_iadc_work);
	rc = qpnp_iadc_comp_info();
	if (rc) {
		dev_err(&spmi->dev, "abstracting IADC comp info failed!\n");
		goto fail;
	}
	iadc->iadc_initialized = true;

	rc = qpnp_iadc_calibrate_for_trim();
	if (rc)
		dev_err(&spmi->dev, "failed to calibrate for USR trim\n");

	if (iadc->iadc_poll_eoc)
		device_init_wakeup(iadc->dev, 1);

	schedule_delayed_work(&iadc->iadc_work,
			round_jiffies_relative(msecs_to_jiffies
					(QPNP_IADC_CALIB_SECONDS)));
	return 0;
fail:
	qpnp_iadc = NULL;
	return rc;
}

static int __devexit qpnp_iadc_remove(struct spmi_device *spmi)
{
	struct qpnp_iadc_drv *iadc = dev_get_drvdata(&spmi->dev);
	struct device_node *node = spmi->dev.of_node;
	struct device_node *child;
	int i = 0;

	cancel_delayed_work(&iadc->iadc_work);
	mutex_destroy(&iadc->iadc_vadc_lock);
	for_each_child_of_node(node, child) {
		device_remove_file(&spmi->dev,
			&iadc->sens_attr[i].dev_attr);
		i++;
	}
	if (iadc->iadc_poll_eoc)
		pm_relax(iadc->dev);
	dev_set_drvdata(&spmi->dev, NULL);

	return 0;
}

static const struct of_device_id qpnp_iadc_match_table[] = {
	{	.compatible = "qcom,qpnp-iadc",
	},
	{}
};

static struct spmi_driver qpnp_iadc_driver = {
	.driver		= {
		.name	= "qcom,qpnp-iadc",
		.of_match_table = qpnp_iadc_match_table,
	},
	.probe		= qpnp_iadc_probe,
	.remove		= qpnp_iadc_remove,
};

static int __init qpnp_iadc_init(void)
{
	return spmi_driver_register(&qpnp_iadc_driver);
}
module_init(qpnp_iadc_init);

static void __exit qpnp_iadc_exit(void)
{
	spmi_driver_unregister(&qpnp_iadc_driver);
}
module_exit(qpnp_iadc_exit);

MODULE_DESCRIPTION("QPNP PMIC current ADC driver");
MODULE_LICENSE("GPL v2");
