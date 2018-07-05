/*
 *   Nokia n810 battery management
 *
 *   WARNING: This driver is based on unconfirmed documentation.
 *            It is possibly dangerous to use this software.
 *            Use this software at your own risk!
 *
 *   Copyright (c) 2010-2011 Michael Buesch <mb@bu3sch.de>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#define DEBUG

#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/firmware.h>
#include <linux/bitops.h>
#include <linux/workqueue.h>
#include <linux/delay.h>

#include "cbus.h"
#include "retu.h"
#include "tahvo.h"
#include "lipocharge.h"


#define N810BM_PMM_BLOCK_FILENAME	"n810-cal-bme-pmm.fw"
#define N810BM_PMM_BLOCK_SIZE		0x600
#define N810BM_PMM_GROUP_SIZE		0x200
#define N810BM_PMM_ELEM_SIZE		0x10

#define N810BM_CHECK_INTERVAL		(HZ * 2)
#define N810BM_MIN_VOLTAGE_THRES	3200 /* Absolute minimum voltage threshold */


/* RETU_ADC_BSI
 * The battery size indicator ADC measures the resistance between
 * the battery BSI pin and ground. This is used to detect the battery
 * capacity, as the BSI resistor is related to capacity.
 *
 * Manually measured lookup table.
 * Hard to measure, thus not very accurate.
 *
 * Resistance  |  ADC value
 * ========================
 * 120k        |  0x3AC
 * 110k        |  0x37C
 * 100k        |  0x351
 *  90k        |  0x329
 */

/* RETU_ADC_BATTVOLT
 * Manually measured lookup table.
 * Hard to measure, thus not very accurate.
 *
 * Voltage  |  ADC value
 * =====================
 * 2.80V    |  0x037
 * 2.90V    |  0x05E
 * 3.00V    |  0x090
 * 3.10V    |  0x0A4
 * 3.20V    |  0x0CC
 * 3.30V    |  0x0EF
 * 3.40V    |  0x115
 * 3.50V    |  0x136
 * 3.60V    |  0x15C
 * 3.70V    |  0x187
 * 3.80V    |  0x1A5
 * 3.90V    |  0x1C9
 * 4.00V    |  0x1ED
 * 4.10V    |  0x212
 * 4.20V    |  0x236
 */


/* PMM block ADC IDs */
enum n810bm_pmm_adc_id {
	N810BM_PMM_ADC_BATVOLT		= 0x01,	/* Battery voltage */
	N810BM_PMM_ADC_CHGVOLT		= 0x02,	/* Charger voltage */
	N810BM_PMM_ADC_GND2		= 0x03,	/* Ground 0V */
	N810BM_PMM_ADC_BSI		= 0x04,	/* Battery size indicator */
	N810BM_PMM_ADC_BATTEMP		= 0x05,	/* Battery temperature */
	N810BM_PMM_ADC_HEADSET		= 0x06,	/* Headset detection */
	N810BM_PMM_ADC_HOOKDET		= 0x07,	/* Hook detection */
	N810BM_PMM_ADC_LIGHTSENS	= 0x08,	/* Light sensor */
	N810BM_PMM_ADC_BATCURR		= 0x0E,	/* Battery current */
	N810BM_PMM_ADC_BKUPVOLT		= 0x13,	/* Backup battery voltage */
	N810BM_PMM_ADC_LIGHTTEMP	= 0x14,	/* Light sensor temperature */
	N810BM_PMM_ADC_RFGP		= 0x15,	/* RF GP */
	N810BM_PMM_ADC_WBTX		= 0x16,	/* Wideband TX detection */
	N810BM_PMM_ADC_RETUTEMP		= 0x17,	/* RETU chip temperature */
	N810BM_PMM_ADC_0xFE		= 0xFE,
};

struct n810bm_adc_calib {
	enum n810bm_pmm_adc_id id;
	u8 flags;
	u8 adc_groupnr;
	u32 field1;
	u32 field2;
	u16 field3;
	u16 field4;
};

struct n810bm_calib {
	struct n810bm_adc_calib adc[25];
};

enum n810bm_capacity {
	N810BM_CAP_UNKNOWN	= -1,
	N810BM_CAP_NONE		= 0,
	N810BM_CAP_1500MAH	= 1500,	/* 1500 mAh battery */
};

enum n810bm_notify_flags {
	N810BM_NOTIFY_charger_present,
	N810BM_NOTIFY_charger_state,
	N810BM_NOTIFY_charger_pwm,
};

struct n810bm {
	bool battery_present;			/* A battery is inserted */
	bool charger_present;			/* The charger is connected */
	enum n810bm_capacity capacity;		/* The capacity of the inserted battery (if any) */

	bool charger_enabled;			/* Want to charge? */
	struct lipocharge charger;		/* Charger subsystem */
	unsigned int active_current_pwm;	/* Active value of TAHVO_REG_CHGCURR */
	int current_measure_enabled;		/* Current measure enable refcount */

	struct platform_device *pdev;
	struct n810bm_calib calib;		/* Calibration data */

	bool verbose_charge_log;		/* Verbose charge logging */

	unsigned long notify_flags;
	struct work_struct notify_work;
	struct work_struct currmeas_irq_work;
	struct delayed_work periodic_check_work;

	bool initialized;			/* The hardware was initialized */
	struct mutex mutex;
};

static void n810bm_notify_charger_present(struct n810bm *bm);
static void n810bm_notify_charger_state(struct n810bm *bm);
static void n810bm_notify_charger_pwm(struct n810bm *bm);


static struct platform_device *n810bm_retu_device;
static struct platform_device *n810bm_tahvo_device;


static inline struct n810bm * device_to_n810bm(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct n810bm *bm = platform_get_drvdata(pdev);

	return bm;
}

static inline bool n810bm_known_battery_present(struct n810bm *bm)
{
	return bm->battery_present &&
	       bm->capacity != N810BM_CAP_UNKNOWN &&
	       bm->capacity != N810BM_CAP_NONE;
}

static NORET_TYPE void n810bm_emergency(struct n810bm *bm, const char *message) ATTRIB_NORET;
static void n810bm_emergency(struct n810bm *bm, const char *message)
{
	printk(KERN_EMERG "n810 battery management fatal fault: %s\n", message);
	cbus_emergency();
}

static u16 tahvo_read(struct n810bm *bm, unsigned int reg)
{
	return tahvo_read_reg(reg);
}

static void tahvo_maskset(struct n810bm *bm, unsigned int reg, u16 mask, u16 set)
{
	tahvo_set_clear_reg_bits(reg, set, mask);
}

static inline void tahvo_write(struct n810bm *bm, unsigned int reg, u16 value)
{
	unsigned long flags;

	spin_lock_irqsave(&tahvo_lock, flags);
	tahvo_write_reg(reg, value);
	spin_unlock_irqrestore(&tahvo_lock, flags);
}

static inline void tahvo_set(struct n810bm *bm, unsigned int reg, u16 mask)
{
	tahvo_set_clear_reg_bits(reg, mask, mask);
}

static inline void tahvo_clear(struct n810bm *bm, unsigned int reg, u16 mask)
{
	tahvo_set_clear_reg_bits(reg, 0, mask);
}

static u16 retu_read(struct n810bm *bm, unsigned int reg)
{
	return retu_read_reg(&n810bm_retu_device->dev, reg);
}

static void retu_maskset(struct n810bm *bm, unsigned int reg, u16 mask, u16 set)
{
	retu_set_clear_reg_bits(&n810bm_retu_device->dev, reg, set, mask);
}

static inline void retu_write(struct n810bm *bm, unsigned int reg, u16 value)
{
	retu_write_reg(&n810bm_retu_device->dev, reg, value);
}

static int retu_adc_average(struct n810bm *bm, unsigned int chan,
			    unsigned int nr_passes)
{
	unsigned int i, value = 0;
	int ret;

	if (WARN_ON(!nr_passes))
		return 0;
	for (i = 0; i < nr_passes; i++) {
		ret = retu_read_adc(&n810bm_retu_device->dev, chan);
		if (ret < 0)
			return ret;
		value += ret;
	}
	value /= nr_passes;

	return value;
}

static struct n810bm_adc_calib * n810bm_get_adc_calib(struct n810bm *bm,
						enum n810bm_pmm_adc_id id)
{
	unsigned int index = 0;
	struct n810bm_adc_calib *cal;

	if (id != N810BM_PMM_ADC_0xFE)
		index = (unsigned int)id + 1;
	if (index >= ARRAY_SIZE(bm->calib.adc))
		return NULL;

	cal = &bm->calib.adc[index];
	WARN_ON(cal->id && cal->id != id);

	return cal;
}

static int pmm_record_get(struct n810bm *bm,
			  const struct firmware *pmm_block,
			  void *buffer, size_t length,
			  unsigned int group, unsigned int element, unsigned int offset)
{
	const u8 *pmm_area = pmm_block->data;
	u8 active_group_mask;

	if (pmm_block->size != N810BM_PMM_BLOCK_SIZE)
		return -EINVAL;
	if (group >= N810BM_PMM_BLOCK_SIZE / N810BM_PMM_GROUP_SIZE)
		return -EINVAL;
	if (element >= N810BM_PMM_GROUP_SIZE / N810BM_PMM_ELEM_SIZE)
		return -EINVAL;
	if (offset >= N810BM_PMM_ELEM_SIZE || length > N810BM_PMM_ELEM_SIZE ||
	    length + offset > N810BM_PMM_ELEM_SIZE)
		return -EINVAL;

	active_group_mask = pmm_area[16];
	if (!(active_group_mask & (1 << group))) {
		dev_dbg(&bm->pdev->dev, "pwm_record_get: Requested group %u, "
			"but group is not active", group);
		return -ENOENT;
	}

	memcpy(buffer,
	       pmm_area + group * N810BM_PMM_GROUP_SIZE
			+ element * N810BM_PMM_ELEM_SIZE
			+ offset,
	       length);

	return 0;
}

/* PMM block group 1 element */
struct group1_element {
	u8 id;
	u8 flags;
	u8 adc_groupnr;
	u8 _padding;
	__le32 field1;
	__le32 field2;
} __packed;

static int extract_group1_elem(struct n810bm *bm,
			       const struct firmware *pmm_block,
			       const enum n810bm_pmm_adc_id *pmm_adc_ids, size_t nr_pmm_adc_ids,
			       u32 field1_mask, u32 field2_mask)
{
	struct group1_element elem;
	int err;
	unsigned int i, element_nr;
	struct n810bm_adc_calib *adc_calib;

	for (i = 0; i < nr_pmm_adc_ids; i++) {
		element_nr = (unsigned int)(pmm_adc_ids[i]) + 3;

		err = pmm_record_get(bm, pmm_block, &elem, sizeof(elem),
				     1, element_nr, 0);
		if (err)
			continue;
		adc_calib = n810bm_get_adc_calib(bm, elem.id);
		if (!adc_calib) {
			dev_err(&bm->pdev->dev, "extract_group1_elem: "
				"Could not get calib element for 0x%02X",
				elem.id);
			return -EINVAL;
		}

		if (adc_calib->flags == elem.flags) {
			adc_calib->field1 = le32_to_cpu(elem.field1) & field1_mask;
			adc_calib->field2 = le32_to_cpu(elem.field2) & field2_mask;
		} else {
			dev_dbg(&bm->pdev->dev, "extract_group1_elem: "
				"Not extracting fields due to flags mismatch: "
				"0x%02X vs 0x%02X",
				adc_calib->flags, elem.flags);
		}
	}

	return 0;
}

static int n810bm_parse_pmm_group1(struct n810bm *bm,
				   const struct firmware *pmm_block)
{
	struct n810bm_adc_calib *adc_calib;
	struct group1_element elem;
	int err;

	static const enum n810bm_pmm_adc_id pmm_adc_ids_1[] = {
		N810BM_PMM_ADC_BATVOLT,
		N810BM_PMM_ADC_CHGVOLT,
		N810BM_PMM_ADC_BKUPVOLT,
		N810BM_PMM_ADC_BATCURR,
	};
	static const enum n810bm_pmm_adc_id pmm_adc_ids_2[] = {
		N810BM_PMM_ADC_BSI,
	};
	static const enum n810bm_pmm_adc_id pmm_adc_ids_3[] = {
		N810BM_PMM_ADC_BATTEMP,
	};

	/* Parse element 2 */
	err = pmm_record_get(bm, pmm_block, &elem, sizeof(elem),
			     1, 2, 0);
	if (err) {
		dev_err(&bm->pdev->dev,
			"PMM: Failed to get group 1 / element 2");
		return err;
	}
	if (elem.id == N810BM_PMM_ADC_0xFE && elem.flags == 0x05) {
		adc_calib = n810bm_get_adc_calib(bm, elem.id);
		if (!adc_calib) {
			dev_err(&bm->pdev->dev,
				"calib extract: Failed to get 0xFE calib");
			return -EINVAL;
		}
		adc_calib->id = elem.id;
		adc_calib->flags = elem.flags;
		adc_calib->field1 = le32_to_cpu(elem.field1);
		adc_calib->field2 = le32_to_cpu(elem.field2);
	}

	err = extract_group1_elem(bm, pmm_block,
				  pmm_adc_ids_1, ARRAY_SIZE(pmm_adc_ids_1),
				  0xFFFFFFFF, 0xFFFFFFFF);
	if (err)
		return err;
	err = extract_group1_elem(bm, pmm_block,
				  pmm_adc_ids_2, ARRAY_SIZE(pmm_adc_ids_2),
				  0xFFFFFFFF, 0);
	if (err)
		return err;
	err = extract_group1_elem(bm, pmm_block,
				  pmm_adc_ids_3, ARRAY_SIZE(pmm_adc_ids_3),
				  0xFFFFFFFF, 0x0000FFFF);
	if (err)
		return err;

	return 0;
}

static int n810bm_parse_pmm_group2(struct n810bm *bm,
				   const struct firmware *pmm_block)
{
	dev_err(&bm->pdev->dev, "TODO: CAL BME PMM group 2 parser not implemented, yet");
	return -EOPNOTSUPP;
}

static void n810bm_adc_calib_set_defaults(struct n810bm *bm)
{
	struct n810bm_adc_calib *adc_calib;
	unsigned int i;

	static const struct n810bm_adc_calib defaults[] = {
		/* ADC group-nr 0 */
		{
			.id		= N810BM_PMM_ADC_HEADSET,
			.flags		= 0x00,
			.adc_groupnr	= 0,
		}, {
			.id		= N810BM_PMM_ADC_HOOKDET,
			.flags		= 0x00,
			.adc_groupnr	= 0,
		}, {
			.id		= N810BM_PMM_ADC_RFGP,
			.flags		= 0x00,
			.adc_groupnr	= 0,
		}, {
			.id		= N810BM_PMM_ADC_LIGHTSENS,
			.flags		= 0x00,
			.adc_groupnr	= 0,
		}, {
			.id		= N810BM_PMM_ADC_WBTX,
			.flags		= 0x00,
			.adc_groupnr	= 0,
		}, {
			.id		= N810BM_PMM_ADC_RETUTEMP,
			.flags		= 0x00,
			.adc_groupnr	= 0,
		}, {
			.id		= N810BM_PMM_ADC_GND2,
			.flags		= 0x00,
			.adc_groupnr	= 0,
		},
		/* ADC group-nr 1 */
		{
			.id		= N810BM_PMM_ADC_0xFE,
			.flags		= 0x05,
			.adc_groupnr	= 1,
			.field1		= (u32)-2,
			.field2		= 13189,
		}, {
			.id		= N810BM_PMM_ADC_BATVOLT,
			.flags		= 0x01,
			.adc_groupnr	= 1,
			.field1		= 2527,
			.field2		= 21373,
		}, {
			.id		= N810BM_PMM_ADC_CHGVOLT,
			.flags		= 0x01,
			.adc_groupnr	= 1,
			.field1		= 0,
			.field2		= 129848,
		}, {
			.id		= N810BM_PMM_ADC_BKUPVOLT,
			.flags		= 0x01,
			.adc_groupnr	= 1,
			.field1		= 0,
			.field2		= 20000,
		}, {
			.id		= N810BM_PMM_ADC_BATCURR,
			.flags		= 0x06,
			.adc_groupnr	= 1,
			.field1		= 0,
			.field2		= 9660,
		},
		/* ADC group-nr 2 */
		{
			.id		= N810BM_PMM_ADC_BSI,
			.flags		= 0x02,
			.adc_groupnr	= 2,
			.field1		= 1169,
			.field2		= 0,
		},
		/* ADC group-nr 3 */
		{
			.id		= N810BM_PMM_ADC_BATTEMP,
			.flags		= 0x03,
			.adc_groupnr	= 3,
			.field1		= 265423000,
			.field2		= 298,
		},
		/* ADC group-nr 4 */
		{
			.id		= N810BM_PMM_ADC_LIGHTTEMP,
			.flags		= 0x04,
			.adc_groupnr	= 4,
			.field1		= 19533778,
			.field2		= 308019670,
			.field3		= 4700,
			.field4		= 2500,
		},
	};

	/* Clear the array */
	memset(&bm->calib.adc, 0, sizeof(bm->calib.adc));
	for (i = 0; i < ARRAY_SIZE(bm->calib.adc); i++)
		bm->calib.adc[i].flags = 0xFF;

	/* Copy the defaults */
	for (i = 0; i < ARRAY_SIZE(defaults); i++) {
		adc_calib = n810bm_get_adc_calib(bm, defaults[i].id);
		if (WARN_ON(!adc_calib))
			continue;
		*adc_calib = defaults[i];
	}
}

static int n810bm_parse_pmm_block(struct n810bm *bm,
				  const struct firmware *pmm_block)
{
	u8 byte;
	int err;
	unsigned int i, count;
	struct n810bm_adc_calib *adc_calib;

	/* Initialize to defaults */
	n810bm_adc_calib_set_defaults(bm);

	/* Parse the PMM data */
	err = pmm_record_get(bm, pmm_block, &byte, sizeof(byte),
			     1, 0, 0); /* group 1 / element 0 */
	err |= (byte != 0x01);
	err |= pmm_record_get(bm, pmm_block, &byte, sizeof(byte),
			      1, 1, 0); /* group 1 / element 1 */
	err |= (byte != 0x01);
	if (err)
		err = n810bm_parse_pmm_group2(bm, pmm_block);
	else
		err = n810bm_parse_pmm_group1(bm, pmm_block);
	if (err)
		return err;

	/* Sanity checks */
	for (i = 0, count = 0; i < ARRAY_SIZE(bm->calib.adc); i++) {
		adc_calib = &bm->calib.adc[i];
		if (adc_calib->flags == 0xFF)
			continue;
		switch (adc_calib->id) {
		case N810BM_PMM_ADC_BATVOLT:
			if (adc_calib->field1 < 2400 ||
			    adc_calib->field1 > 2700)
				goto value_check_fail;
			if (adc_calib->field2 < 20000 ||
			    adc_calib->field2 > 23000)
				goto value_check_fail;
			count++;
			break;
		case N810BM_PMM_ADC_BSI:
			if (adc_calib->field1 < 1100 ||
			    adc_calib->field1 > 1300)
				goto value_check_fail;
			count++;
			break;
		case N810BM_PMM_ADC_BATCURR:
			if (adc_calib->field2 < 7000 ||
			    adc_calib->field2 > 12000)
				goto value_check_fail;
			count++;
			break;
		case N810BM_PMM_ADC_0xFE:
			if ((s32)adc_calib->field1 > 14 ||
			    (s32)adc_calib->field1 < -14)
				goto value_check_fail;
			if (adc_calib->field2 < 13000 ||
			    adc_calib->field2 > 13350)
				goto value_check_fail;
			count++;
			break;
		case N810BM_PMM_ADC_CHGVOLT:
		case N810BM_PMM_ADC_BATTEMP:
		case N810BM_PMM_ADC_BKUPVOLT:
			count++;
			break;
		case N810BM_PMM_ADC_GND2:
		case N810BM_PMM_ADC_HOOKDET:
		case N810BM_PMM_ADC_LIGHTSENS:
		case N810BM_PMM_ADC_HEADSET:
		case N810BM_PMM_ADC_LIGHTTEMP:
		case N810BM_PMM_ADC_RFGP:
		case N810BM_PMM_ADC_WBTX:
		case N810BM_PMM_ADC_RETUTEMP:
			break;
		}
		dev_dbg(&bm->pdev->dev,
			"ADC 0x%02X calib: 0x%02X 0x%02X 0x%08X 0x%08X 0x%04X 0x%04X",
			adc_calib->id, adc_calib->flags, adc_calib->adc_groupnr,
			adc_calib->field1, adc_calib->field2,
			adc_calib->field3, adc_calib->field4);
	}
	if (count != 7) {
		dev_err(&bm->pdev->dev, "PMM sanity check: Did not find "
			"all required values (count=%u)", count);
		goto check_fail;
	}

	return 0;

value_check_fail:
	dev_err(&bm->pdev->dev, "PMM image sanity check failed "
		"(id=%02X, field1=%08X, field2=%08X)",
		adc_calib->id, adc_calib->field1, adc_calib->field2);
check_fail:
	return -EILSEQ;
}

/* Set the current measure timer that triggers on Tahvo IRQ 7
 * An interval of zero disables the timer. */
static void n810bm_set_current_measure_timer(struct n810bm *bm,
					     u16 millisec_interval)
{
	u16 value = millisec_interval;

	if (value <= 0xF905) {
		value = ((u64)0x10624DD3 * (u64)(value + 0xF9)) >> 32;
		value /= 16;
	} else
		value = 0xFF;

	tahvo_write(bm, TAHVO_REG_BATCURRTIMER, value & 0xFF);

	tahvo_set(bm, TAHVO_REG_CHGCTL,
		  TAHVO_REG_CHGCTL_CURTIMRST);
	tahvo_clear(bm, TAHVO_REG_CHGCTL,
		    TAHVO_REG_CHGCTL_CURTIMRST);

	if (millisec_interval)
		tahvo_enable_irq(TAHVO_INT_BATCURR);
	else
		tahvo_disable_irq(TAHVO_INT_BATCURR);

	//TODO also do a software timer for safety.
}

static void n810bm_enable_current_measure(struct n810bm *bm)
{
	WARN_ON(bm->current_measure_enabled < 0);
	if (!bm->current_measure_enabled) {
		/* Enable the current measurement circuitry */
		tahvo_set(bm, TAHVO_REG_CHGCTL,
			  TAHVO_REG_CHGCTL_CURMEAS);
		dev_dbg(&bm->pdev->dev,
			"Current measurement circuitry enabled");
	}
	bm->current_measure_enabled++;
}

static void n810bm_disable_current_measure(struct n810bm *bm)
{
	bm->current_measure_enabled--;
	WARN_ON(bm->current_measure_enabled < 0);
	if (!bm->current_measure_enabled) {
		/* Disable the current measurement circuitry */
		tahvo_clear(bm, TAHVO_REG_CHGCTL,
			    TAHVO_REG_CHGCTL_CURMEAS);
		dev_dbg(&bm->pdev->dev,
			"Current measurement circuitry disabled");
	}
}

/* Measure the actual battery current. Returns a signed value in mA.
 * Does only work, if current measurement was enabled. */
static int n810bm_measure_batt_current(struct n810bm *bm)
{
	u16 retval;
	int adc = 0, ma, i;

	if (WARN_ON(bm->current_measure_enabled <= 0))
		return 0;
	for (i = 0; i < 3; i++) {
		retval = tahvo_read(bm, TAHVO_REG_BATCURR);
		adc += (s16)retval; /* Value is signed */
	}
	adc /= 3;

	//TODO convert to mA
	ma = adc;

	return ma;
}

/* Requires bm->mutex locked */
static int n810bm_measure_batt_current_async(struct n810bm *bm)
{
	int ma;
	bool charging = lipocharge_is_charging(&bm->charger);

	n810bm_enable_current_measure(bm);
	if (!charging)
		WARN_ON(bm->active_current_pwm != 0);
	tahvo_maskset(bm, TAHVO_REG_CHGCTL,
		      TAHVO_REG_CHGCTL_EN |
		      TAHVO_REG_CHGCTL_PWMOVR |
		      TAHVO_REG_CHGCTL_PWMOVRZERO,
		      TAHVO_REG_CHGCTL_EN |
		      TAHVO_REG_CHGCTL_PWMOVR |
		      (charging ? 0 : TAHVO_REG_CHGCTL_PWMOVRZERO));
	ma = n810bm_measure_batt_current(bm);
	tahvo_maskset(bm, TAHVO_REG_CHGCTL,
		      TAHVO_REG_CHGCTL_EN |
		      TAHVO_REG_CHGCTL_PWMOVR |
		      TAHVO_REG_CHGCTL_PWMOVRZERO,
		      (charging ? TAHVO_REG_CHGCTL_EN : 0));
	n810bm_disable_current_measure(bm);

	return ma;
}

static int adc_sanity_check(struct n810bm *bm, unsigned int channel)
{
	int value;

	value = retu_read_adc(&n810bm_retu_device->dev, channel);
	if (value < 0) {
		dev_err(&bm->pdev->dev, "Failed to read GND ADC channel %u",
			channel);
		return -EIO;
	}
	dev_dbg(&bm->pdev->dev,
		"GND ADC channel %u sanity check got value: %d",
		channel, value);
	if (value > 5) {
		n810bm_emergency(bm, "GND ADC sanity check failed");
		return -EIO;
	}

	return 0;
}

static int n810bm_check_adc_sanity(struct n810bm *bm)
{
	int err;

	/* Discard one conversion */
	retu_write(bm, RETU_REG_ADCSCR, 0);
	retu_read_adc(&n810bm_retu_device->dev, RETU_ADC_GND2);

	err = adc_sanity_check(bm, RETU_ADC_GND2);
	if (err)
		return err;

	return 0;
}

/* Measure the battery voltage. Returns the value in mV (or negative value on error). */
static int n810bm_measure_batt_voltage(struct n810bm *bm)
{
	int adc;
	unsigned int mv;
	const unsigned int scale = 1000;

	adc = retu_adc_average(bm, RETU_ADC_BATTVOLT, 5);
	if (adc < 0)
		return adc;
	if (adc <= 0x37)
		return 2800;
	mv = 2800 + ((adc - 0x37) * (((4200 - 2800) * scale) / (0x236 - 0x37))) / scale;

	//TODO compensate for power consumption
	//TODO honor calibration values

	return mv;
}

/* Measure the charger voltage. Returns the value in mV (or negative value on error). */
static int n810bm_measure_charger_voltage(struct n810bm *bm)
{
	int adc;
	unsigned int mv;

	adc = retu_adc_average(bm, RETU_ADC_CHGVOLT, 5);
	if (adc < 0)
		return adc;
	//TODO convert to mV
	mv = adc;

	return mv;
}

/* Measure backup battery voltage. Returns the value in mV (or negative value on error). */
static int n810bm_measure_backup_batt_voltage(struct n810bm *bm)
{
	int adc;
	unsigned int mv;

	adc = retu_adc_average(bm, RETU_ADC_BKUPVOLT, 3);
	if (adc < 0)
		return adc;
	//TODO convert to mV
	mv = adc;

	return mv;
}

/* Measure the battery temperature. Returns the value in K (or negative value on error). */
static int n810bm_measure_batt_temp(struct n810bm *bm)
{
	int adc;
	unsigned int k;

	adc = retu_adc_average(bm, RETU_ADC_BATTEMP, 3);
	if (adc < 0)
		return adc;
	//TODO convert to K
	k = adc;

	return k;
}

/* Read the battery capacity via BSI pin. */
static enum n810bm_capacity n810bm_read_batt_capacity(struct n810bm *bm)
{
	int adc;
	const unsigned int hyst = 20;

	adc = retu_adc_average(bm, RETU_ADC_BSI, 5);
	if (adc < 0) {
		dev_err(&bm->pdev->dev, "Failed to read BSI ADC");
		return N810BM_CAP_UNKNOWN;
	}

	if (adc >= 0x3B5 - hyst && adc <= 0x3B5 + hyst)
		return N810BM_CAP_1500MAH;

	dev_err(&bm->pdev->dev, "Capacity indicator 0x%X unknown", adc);

	return N810BM_CAP_UNKNOWN;
}

/* Convert a battery voltage (in mV) to percentage. */
static unsigned int n810bm_mvolt2percent(unsigned int mv)
{
	const unsigned int minv = 3700;
	const unsigned int maxv = 4150;
	unsigned int percent;

	mv = clamp(mv, minv, maxv);
	percent = (mv - minv) * 100 / (maxv - minv);

	return percent;
}

static void n810bm_start_charge(struct n810bm *bm)
{
	int err;

	WARN_ON(!bm->battery_present);
	WARN_ON(!bm->charger_present);

	/* Set PWM to zero */
	bm->active_current_pwm = 0;
	tahvo_write(bm, TAHVO_REG_CHGCURR, bm->active_current_pwm);

	/* Charge global enable */
	tahvo_maskset(bm, TAHVO_REG_CHGCTL,
		      TAHVO_REG_CHGCTL_EN |
		      TAHVO_REG_CHGCTL_PWMOVR |
		      TAHVO_REG_CHGCTL_PWMOVRZERO,
		      TAHVO_REG_CHGCTL_EN);

	WARN_ON((int)bm->capacity <= 0);
	bm->charger.capacity = bm->capacity;
	err = lipocharge_start(&bm->charger);
	WARN_ON(err);

	/* Initialize current measurement circuitry */
	n810bm_enable_current_measure(bm);
	n810bm_set_current_measure_timer(bm, 250);

	dev_info(&bm->pdev->dev, "Charging battery");
	n810bm_notify_charger_state(bm);
	n810bm_notify_charger_pwm(bm);
}

static void n810bm_stop_charge(struct n810bm *bm)
{
	if (lipocharge_is_charging(&bm->charger)) {
		n810bm_set_current_measure_timer(bm, 0);
		n810bm_disable_current_measure(bm);
	}
	lipocharge_stop(&bm->charger);

	/* Set PWM to zero */
	bm->active_current_pwm = 0;
	tahvo_write(bm, TAHVO_REG_CHGCURR, bm->active_current_pwm);

	/* Charge global disable */
	tahvo_maskset(bm, TAHVO_REG_CHGCTL,
		      TAHVO_REG_CHGCTL_EN |
		      TAHVO_REG_CHGCTL_PWMOVR |
		      TAHVO_REG_CHGCTL_PWMOVRZERO,
		      0);

	dev_info(&bm->pdev->dev, "Not charging battery");
	n810bm_notify_charger_state(bm);
	n810bm_notify_charger_pwm(bm);
}

/* Periodic check */
static void n810bm_periodic_check_work(struct work_struct *work)
{
	struct n810bm *bm = container_of(to_delayed_work(work),
					 struct n810bm, periodic_check_work);
	u16 status;
	bool battery_was_present, charger_was_present;
	int mv;

	mutex_lock(&bm->mutex);

	status = retu_read(bm, RETU_REG_STATUS);
	battery_was_present = bm->battery_present;
	charger_was_present = bm->charger_present;
	bm->battery_present = !!(status & RETU_REG_STATUS_BATAVAIL);
	bm->charger_present = !!(status & RETU_REG_STATUS_CHGPLUG);

	if (bm->battery_present != battery_was_present) {
		/* Battery state changed */
		if (bm->battery_present) {
			bm->capacity = n810bm_read_batt_capacity(bm);
			if (bm->capacity == N810BM_CAP_UNKNOWN) {
				dev_err(&bm->pdev->dev, "Unknown battery detected");
			} else {
				dev_info(&bm->pdev->dev, "Detected %u mAh battery",
					 (unsigned int)bm->capacity);
			}
		} else {
			bm->capacity = N810BM_CAP_NONE;
			dev_info(&bm->pdev->dev, "The main battery was removed");
			//TODO disable charging
		}
	}

	if (bm->charger_present != charger_was_present) {
		/* Charger state changed */
		dev_info(&bm->pdev->dev, "The charger was %s",
			 bm->charger_present ? "plugged in" : "removed");
		n810bm_notify_charger_present(bm);
	}

	if ((bm->battery_present && !bm->charger_present) ||
	    !n810bm_known_battery_present(bm)){
		/* We're draining the battery */
		mv = n810bm_measure_batt_voltage(bm);
		if (mv < 0) {
			n810bm_emergency(bm,
				"check: Failed to measure voltage");
		}
		if (mv < N810BM_MIN_VOLTAGE_THRES) {
			n810bm_emergency(bm,
				"check: Minimum voltage threshold reached");
		}
	}

	if (bm->charger_present && n810bm_known_battery_present(bm)) {
		/* Known battery and charger are connected */
		if (bm->charger_enabled) {
			/* Charger is enabled */
			if (!lipocharge_is_charging(&bm->charger)) {
				//TODO start charging, if battery is below some threshold
				n810bm_start_charge(bm);
			}
		}
	}

	if (lipocharge_is_charging(&bm->charger) && !bm->charger_present) {
		/* Charger was unplugged. */
		n810bm_stop_charge(bm);
	}

	mutex_unlock(&bm->mutex);
	schedule_delayed_work(&bm->periodic_check_work,
			      round_jiffies_relative(N810BM_CHECK_INTERVAL));
}

/*XXX
static void n810bm_adc_irq_handler(unsigned long data)
{
	struct n810bm *bm = (struct n810bm *)data;

	retu_ack_irq(RETU_INT_ADCS);
	//TODO
dev_info(&bm->pdev->dev, "ADC interrupt triggered\n");
}
*/

static void n810bm_tahvo_current_measure_work(struct work_struct *work)
{
	struct n810bm *bm = container_of(work, struct n810bm, currmeas_irq_work);
	int res, ma, mv, temp;

	mutex_lock(&bm->mutex);
	if (!lipocharge_is_charging(&bm->charger))
		goto out_unlock;

	tahvo_maskset(bm, TAHVO_REG_CHGCTL,
		      TAHVO_REG_CHGCTL_PWMOVR |
		      TAHVO_REG_CHGCTL_PWMOVRZERO,
		      TAHVO_REG_CHGCTL_PWMOVR);
	ma = n810bm_measure_batt_current(bm);
	tahvo_maskset(bm, TAHVO_REG_CHGCTL,
		      TAHVO_REG_CHGCTL_PWMOVR |
		      TAHVO_REG_CHGCTL_PWMOVRZERO,
		      TAHVO_REG_CHGCTL_PWMOVR |
		      TAHVO_REG_CHGCTL_PWMOVRZERO);
	msleep(10);
	mv = n810bm_measure_batt_voltage(bm);
	tahvo_maskset(bm, TAHVO_REG_CHGCTL,
		      TAHVO_REG_CHGCTL_PWMOVR |
		      TAHVO_REG_CHGCTL_PWMOVRZERO,
		      0);
	temp = n810bm_measure_batt_temp(bm);
	if (WARN_ON(mv < 0))
		goto out_unlock;
	if (WARN_ON(temp < 0))
		goto out_unlock;

	if (bm->verbose_charge_log) {
		dev_info(&bm->pdev->dev,
			 "Battery charge state: %d mV, %d mA (%s)",
			 mv, ma,
			 (ma <= 0) ? "discharging" : "charging");
	}
	res = lipocharge_update_state(&bm->charger, mv, ma, temp);
	if (res) {
		if (res > 0)
			dev_info(&bm->pdev->dev, "Battery fully charged");
		n810bm_stop_charge(bm);
	}
out_unlock:
	mutex_unlock(&bm->mutex);
}

static void n810bm_tahvo_current_measure_irq_handler(unsigned long data)
{
	struct n810bm *bm = (struct n810bm *)data;

	tahvo_ack_irq(TAHVO_INT_BATCURR);
	schedule_work(&bm->currmeas_irq_work);
}

#define DEFINE_ATTR_NOTIFY(attr_name)						\
	void n810bm_notify_##attr_name(struct n810bm *bm)			\
	{									\
		set_bit(N810BM_NOTIFY_##attr_name, &bm->notify_flags);		\
		wmb();								\
		schedule_work(&bm->notify_work);				\
	}

#define DEFINE_SHOW_INT_FUNC(name, member)					\
	static ssize_t n810bm_attr_##name##_show(struct device *dev,		\
						 struct device_attribute *attr,	\
						 char *buf)			\
	{									\
		struct n810bm *bm = device_to_n810bm(dev);			\
		ssize_t count;							\
										\
		mutex_lock(&bm->mutex);						\
		count = snprintf(buf, PAGE_SIZE, "%d\n", (int)(bm->member));	\
		mutex_unlock(&bm->mutex);					\
										\
		return count;							\
	}

#define DEFINE_STORE_INT_FUNC(name, member)					\
	static ssize_t n810bm_attr_##name##_store(struct device *dev,		\
						  struct device_attribute *attr,\
						  const char *buf, size_t count)\
	{									\
		struct n810bm *bm = device_to_n810bm(dev);			\
		long val;							\
		int err;							\
										\
		mutex_lock(&bm->mutex);						\
		err = strict_strtol(buf, 0, &val);				\
		if (!err)							\
			bm->member = (typeof(bm->member))val;			\
		mutex_unlock(&bm->mutex);					\
										\
		return err ? err : count;					\
	}

#define DEFINE_ATTR_SHOW_INT(name, member)					\
	DEFINE_SHOW_INT_FUNC(name, member)					\
	static DEVICE_ATTR(name, S_IRUGO,					\
			   n810bm_attr_##name##_show, NULL);

#define DEFINE_ATTR_SHOW_STORE_INT(name, member)				\
	DEFINE_SHOW_INT_FUNC(name, member)					\
	DEFINE_STORE_INT_FUNC(name, member)					\
	static DEVICE_ATTR(name, S_IRUGO | S_IWUSR,				\
			   n810bm_attr_##name##_show,				\
			   n810bm_attr_##name##_store);

DEFINE_ATTR_SHOW_INT(battery_present, battery_present);
DEFINE_ATTR_SHOW_INT(charger_present, charger_present);
static DEFINE_ATTR_NOTIFY(charger_present);
DEFINE_ATTR_SHOW_INT(charger_state, charger.state);
static DEFINE_ATTR_NOTIFY(charger_state);
DEFINE_ATTR_SHOW_INT(charger_pwm, active_current_pwm);
static DEFINE_ATTR_NOTIFY(charger_pwm);
DEFINE_ATTR_SHOW_STORE_INT(charger_enable, charger_enabled);
DEFINE_ATTR_SHOW_STORE_INT(charger_verbose, verbose_charge_log);

static ssize_t n810bm_attr_battery_level_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	struct n810bm *bm = device_to_n810bm(dev);
	ssize_t count = -ENODEV;
	int millivolt;

	mutex_lock(&bm->mutex);
	if (!bm->battery_present || lipocharge_is_charging(&bm->charger))
		millivolt = 0;
	else
		millivolt = n810bm_measure_batt_voltage(bm);
	if (millivolt >= 0) {
		count = snprintf(buf, PAGE_SIZE, "%u\n",
				 n810bm_mvolt2percent(millivolt));
	}
	mutex_unlock(&bm->mutex);

	return count;
}
static DEVICE_ATTR(battery_level, S_IRUGO,
		   n810bm_attr_battery_level_show, NULL);

static ssize_t n810bm_attr_battery_capacity_show(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	struct n810bm *bm = device_to_n810bm(dev);
	ssize_t count;
	int capacity = 0;

	mutex_lock(&bm->mutex);
	if (n810bm_known_battery_present(bm))
		capacity = (int)bm->capacity;
	count = snprintf(buf, PAGE_SIZE, "%d\n", capacity);
	mutex_unlock(&bm->mutex);

	return count;
}
static DEVICE_ATTR(battery_capacity, S_IRUGO,
		   n810bm_attr_battery_capacity_show, NULL);

static ssize_t n810bm_attr_battery_temp_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct n810bm *bm = device_to_n810bm(dev);
	ssize_t count = -ENODEV;
	int k;

	mutex_lock(&bm->mutex);
	k = n810bm_measure_batt_temp(bm);
	if (k >= 0)
		count = snprintf(buf, PAGE_SIZE, "%d\n", k);
	mutex_unlock(&bm->mutex);

	return count;
}
static DEVICE_ATTR(battery_temp, S_IRUGO,
		   n810bm_attr_battery_temp_show, NULL);

static ssize_t n810bm_attr_charger_voltage_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct n810bm *bm = device_to_n810bm(dev);
	ssize_t count = -ENODEV;
	int mv = 0;

	mutex_lock(&bm->mutex);
	if (bm->charger_present)
		mv = n810bm_measure_charger_voltage(bm);
	if (mv >= 0)
		count = snprintf(buf, PAGE_SIZE, "%d\n", mv);
	mutex_unlock(&bm->mutex);

	return count;
}
static DEVICE_ATTR(charger_voltage, S_IRUGO,
		   n810bm_attr_charger_voltage_show, NULL);

static ssize_t n810bm_attr_backup_battery_voltage_show(struct device *dev,
						       struct device_attribute *attr,
						       char *buf)
{
	struct n810bm *bm = device_to_n810bm(dev);
	ssize_t count = -ENODEV;
	int mv;

	mutex_lock(&bm->mutex);
	mv = n810bm_measure_backup_batt_voltage(bm);
	if (mv >= 0)
		count = snprintf(buf, PAGE_SIZE, "%d\n", mv);
	mutex_unlock(&bm->mutex);

	return count;
}
static DEVICE_ATTR(backup_battery_voltage, S_IRUGO,
		   n810bm_attr_backup_battery_voltage_show, NULL);

static ssize_t n810bm_attr_battery_current_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	struct n810bm *bm = device_to_n810bm(dev);
	ssize_t count = -ENODEV;
	int ma = 0;

	mutex_lock(&bm->mutex);
	if (bm->battery_present)
		ma = n810bm_measure_batt_current_async(bm);
	count = snprintf(buf, PAGE_SIZE, "%d\n", ma);
	mutex_unlock(&bm->mutex);

	return count;
}
static DEVICE_ATTR(battery_current, S_IRUGO,
		   n810bm_attr_battery_current_show, NULL);

static const struct device_attribute *n810bm_attrs[] = {
	&dev_attr_battery_present,
	&dev_attr_battery_level,
	&dev_attr_battery_current,
	&dev_attr_battery_capacity,
	&dev_attr_battery_temp,
	&dev_attr_backup_battery_voltage,
	&dev_attr_charger_present,
	&dev_attr_charger_state,
	&dev_attr_charger_verbose,
	&dev_attr_charger_voltage,
	&dev_attr_charger_enable,
	&dev_attr_charger_pwm,
};

static void n810bm_notify_work(struct work_struct *work)
{
	struct n810bm *bm = container_of(work, struct n810bm, notify_work);
	unsigned long notify_flags;

	notify_flags = xchg(&bm->notify_flags, 0);
	mb();

#define do_notify(attr_name)						\
	do {								\
		if (notify_flags & (1 << N810BM_NOTIFY_##attr_name)) {	\
			sysfs_notify(&bm->pdev->dev.kobj, NULL,		\
				     dev_attr_##attr_name.attr.name);	\
		}							\
	} while (0)

	do_notify(charger_present);
	do_notify(charger_state);
	do_notify(charger_pwm);
}

static int n810bm_charger_set_current_pwm(struct lipocharge *c,
					  unsigned int duty_cycle)
{
	struct n810bm *bm = container_of(c, struct n810bm, charger);
	int err = -EINVAL;

	WARN_ON(!mutex_is_locked(&bm->mutex));
	if (WARN_ON(duty_cycle > 0xFF))
		goto out;
	if (WARN_ON(!bm->charger_enabled))
		goto out;
	if (WARN_ON(!bm->battery_present || !bm->charger_present))
		goto out;

	if (duty_cycle != bm->active_current_pwm) {
		bm->active_current_pwm = duty_cycle;
		tahvo_write(bm, TAHVO_REG_CHGCURR, duty_cycle);
		n810bm_notify_charger_pwm(bm);
	}

	err = 0;
out:

	return err;
}

static void n810bm_charger_emergency(struct lipocharge *c)
{
	struct n810bm *bm = container_of(c, struct n810bm, charger);

	n810bm_emergency(bm, "Battery charger fault");
}

static void n810bm_hw_exit(struct n810bm *bm)
{
	n810bm_stop_charge(bm);
	retu_write(bm, RETU_REG_ADCSCR, 0);
}

static int n810bm_hw_init(struct n810bm *bm)
{
	int err;

	err = n810bm_check_adc_sanity(bm);
	if (err)
		return err;

	n810bm_stop_charge(bm);

	return 0;
}

static void n810bm_cancel_and_flush_work(struct n810bm *bm)
{
	cancel_delayed_work_sync(&bm->periodic_check_work);
	cancel_work_sync(&bm->notify_work);
	cancel_work_sync(&bm->currmeas_irq_work);
	flush_scheduled_work();
}

static int n810bm_device_init(struct n810bm *bm)
{
	int attr_index;
	int err;

	bm->charger.rate = LIPORATE_p6C;
	bm->charger.top_voltage = 4100;
	bm->charger.duty_cycle_max = 0xFF;
	bm->charger.set_current_pwm = n810bm_charger_set_current_pwm;
	bm->charger.emergency = n810bm_charger_emergency;
	lipocharge_init(&bm->charger, &bm->pdev->dev);

	err = n810bm_hw_init(bm);
	if (err)
		goto error;
	for (attr_index = 0; attr_index < ARRAY_SIZE(n810bm_attrs); attr_index++) {
		err = device_create_file(&bm->pdev->dev, n810bm_attrs[attr_index]);
		if (err)
			goto err_unwind_attrs;
	}
/*XXX
	err = retu_request_irq(RETU_INT_ADCS,
			       n810bm_adc_irq_handler,
			       (unsigned long)bm, "n810bm");
	if (err)
		goto err_unwind_attrs;
*/
	err = tahvo_request_irq(TAHVO_INT_BATCURR,
				n810bm_tahvo_current_measure_irq_handler,
				(unsigned long)bm, "n810bm");
	if (err)
		goto err_free_retu_irq;
	tahvo_disable_irq(TAHVO_INT_BATCURR);

	schedule_delayed_work(&bm->periodic_check_work,
			      round_jiffies_relative(N810BM_CHECK_INTERVAL));

	bm->initialized = 1;
	dev_info(&bm->pdev->dev, "Battery management initialized");

	return 0;

err_free_retu_irq:
//XXX	retu_free_irq(RETU_INT_ADCS);
err_unwind_attrs:
	for (attr_index--; attr_index >= 0; attr_index--)
		device_remove_file(&bm->pdev->dev, n810bm_attrs[attr_index]);
/*err_exit:*/
	n810bm_hw_exit(bm);
error:
	n810bm_cancel_and_flush_work(bm);

	return err;
}

static void n810bm_device_exit(struct n810bm *bm)
{
	int i;

	if (!bm->initialized)
		return;

	lipocharge_exit(&bm->charger);
	tahvo_free_irq(TAHVO_INT_BATCURR);
//XXX	retu_free_irq(RETU_INT_ADCS);
	for (i = 0; i < ARRAY_SIZE(n810bm_attrs); i++)
		device_remove_file(&bm->pdev->dev, n810bm_attrs[i]);

	n810bm_cancel_and_flush_work(bm);

	n810bm_hw_exit(bm);

	bm->initialized = 0;
}

static void n810bm_pmm_block_found(const struct firmware *fw, void *context)
{
	struct n810bm *bm = context;
	int err;

	if (!fw) {
		dev_err(&bm->pdev->dev,
			"CAL PMM block image file not found");
		goto err_release;
	}
	if (fw->size != N810BM_PMM_BLOCK_SIZE ||
	    memcmp(fw->data, "BME-PMM-BLOCK01", 15) != 0) {
		dev_err(&bm->pdev->dev,
			"CAL PMM block image file has an invalid format");
		goto err_release;
	}

	err = n810bm_parse_pmm_block(bm, fw);
	if (err)
		goto err_release;
	release_firmware(fw);

	err = n810bm_device_init(bm);
	if (err) {
		dev_err(&bm->pdev->dev,
			"Failed to initialized battery management (%d)", err);
		goto error;
	}

	return;
err_release:
	release_firmware(fw);
error:
	return;
}

static int __devinit n810bm_probe(void)
{
	struct n810bm *bm;
	int err;

	if (!n810bm_retu_device || !n810bm_tahvo_device)
		return 0;

	bm = kzalloc(sizeof(*bm), GFP_KERNEL);
	if (!bm)
		return -ENOMEM;
	bm->pdev = n810bm_retu_device;
	platform_set_drvdata(n810bm_retu_device, bm);
	platform_set_drvdata(n810bm_tahvo_device, bm);
	mutex_init(&bm->mutex);
	INIT_DELAYED_WORK(&bm->periodic_check_work, n810bm_periodic_check_work);
	INIT_WORK(&bm->notify_work, n810bm_notify_work);
	INIT_WORK(&bm->currmeas_irq_work, n810bm_tahvo_current_measure_work);

	dev_info(&bm->pdev->dev, "Requesting CAL BME PMM block firmware file "
		 N810BM_PMM_BLOCK_FILENAME);
	err = request_firmware_nowait(THIS_MODULE, 1,
				      N810BM_PMM_BLOCK_FILENAME,
				      &bm->pdev->dev, GFP_KERNEL,
				      bm, n810bm_pmm_block_found);
	if (err) {
		dev_err(&bm->pdev->dev,
			"Failed to request CAL PMM block image file (%d)", err);
		goto err_free;
	}

	return 0;

err_free:
	kfree(bm);

	return err;
}

static void __devexit n810bm_remove(void)
{
	struct n810bm *bm;

	if (!n810bm_retu_device || !n810bm_tahvo_device)
		return;
	bm = platform_get_drvdata(n810bm_retu_device);

	n810bm_device_exit(bm);

	kfree(bm);
	platform_set_drvdata(n810bm_retu_device, NULL);
	platform_set_drvdata(n810bm_tahvo_device, NULL);
}

static int __devinit n810bm_retu_probe(struct platform_device *pdev)
{
	n810bm_retu_device = pdev;
	return n810bm_probe();
}

static int __devexit n810bm_retu_remove(struct platform_device *pdev)
{
	n810bm_remove();
	n810bm_retu_device = NULL;
	return 0;
}

static int __devinit n810bm_tahvo_probe(struct platform_device *pdev)
{
	n810bm_tahvo_device = pdev;
	return n810bm_probe();
}

static int __devexit n810bm_tahvo_remove(struct platform_device *pdev)
{
	n810bm_remove();
	n810bm_tahvo_device = NULL;
	return 0;
}

static struct platform_driver n810bm_retu_driver = {
	.remove		= __devexit_p(n810bm_retu_remove),
	.driver		= {
		.name	= "retu-n810bm",
	}
};

static struct platform_driver n810bm_tahvo_driver = {
	.remove		= __devexit_p(n810bm_tahvo_remove),
	.driver		= {
		.name	= "tahvo-n810bm",
	}
};

/* FIXME: for now alloc the device here... */
static struct platform_device n810bm_tahvo_dev = {
	.name	= "tahvo-n810bm",
	.id	= -1,
};

static int __init n810bm_modinit(void)
{
	int err;

	//FIXME
	err = platform_device_register(&n810bm_tahvo_dev);
	if (err)
		return err;

	err = platform_driver_probe(&n810bm_retu_driver, n810bm_retu_probe);
	if (err)
		return err;
	err = platform_driver_probe(&n810bm_tahvo_driver, n810bm_tahvo_probe);
	if (err) {
		platform_driver_unregister(&n810bm_retu_driver);
		return err;
	}

	return 0;
}
module_init(n810bm_modinit);

static void __exit n810bm_modexit(void)
{
	//FIXME
	platform_device_unregister(&n810bm_tahvo_dev);

	platform_driver_unregister(&n810bm_tahvo_driver);
	platform_driver_unregister(&n810bm_retu_driver);
}
module_exit(n810bm_modexit);

MODULE_DESCRIPTION("Nokia n810 battery management");
MODULE_FIRMWARE(N810BM_PMM_BLOCK_FILENAME);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Buesch");
