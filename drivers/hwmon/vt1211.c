/*
 * vt1211.c - driver for the VIA VT1211 Super-I/O chip integrated hardware
 *            monitoring features
 * Copyright (C) 2006 Juerg Haefliger <juergh@gmail.com>
 *
 * This driver is based on the driver for kernel 2.4 by Mark D. Studebaker
 * and its port to kernel 2.6 by Lars Ekman.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon-vid.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/ioport.h>
#include <linux/acpi.h>
#include <linux/io.h>

static int uch_config = -1;
module_param(uch_config, int, 0);
MODULE_PARM_DESC(uch_config, "Initialize the universal channel configuration");

static int int_mode = -1;
module_param(int_mode, int, 0);
MODULE_PARM_DESC(int_mode, "Force the temperature interrupt mode");

static unsigned short force_id;
module_param(force_id, ushort, 0);
MODULE_PARM_DESC(force_id, "Override the detected device ID");

static struct platform_device *pdev;

#define DRVNAME "vt1211"


#define VT1211_REG_IN(ix)		(0x21 + (ix))
#define VT1211_REG_IN_MIN(ix)		((ix) == 0 ? 0x3e : 0x2a + 2 * (ix))
#define VT1211_REG_IN_MAX(ix)		((ix) == 0 ? 0x3d : 0x29 + 2 * (ix))

static u8 regtemp[]	= {0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
static u8 regtempmax[]	= {0x39, 0x1d, 0x3d, 0x2b, 0x2d, 0x2f, 0x31};
static u8 regtemphyst[]	= {0x3a, 0x1e, 0x3e, 0x2c, 0x2e, 0x30, 0x32};

#define VT1211_REG_FAN(ix)		(0x29 + (ix))
#define VT1211_REG_FAN_MIN(ix)		(0x3b + (ix))
#define VT1211_REG_FAN_DIV		 0x47

#define VT1211_REG_PWM(ix)		(0x60 + (ix))
#define VT1211_REG_PWM_CLK		 0x50
#define VT1211_REG_PWM_CTL		 0x51
#define VT1211_REG_PWM_AUTO_TEMP(ap)	(0x55 - (ap))
#define VT1211_REG_PWM_AUTO_PWM(ix, ap)	(0x58 + 2 * (ix) - (ap))

#define VT1211_REG_CONFIG		0x40
#define VT1211_REG_ALARM1		0x41
#define VT1211_REG_ALARM2		0x42
#define VT1211_REG_VID			0x45
#define VT1211_REG_UCH_CONFIG		0x4a
#define VT1211_REG_TEMP1_CONFIG		0x4b
#define VT1211_REG_TEMP2_CONFIG		0x4c

static const u8 bitalarmin[]	= {11, 0, 1, 3, 8, 2, 9};
static const u8 bitalarmtemp[]	= {4, 15, 11, 0, 1, 3, 8};
static const u8 bitalarmfan[]	= {6, 7};


struct vt1211_data {
	unsigned short addr;
	const char *name;
	struct device *hwmon_dev;

	struct mutex update_lock;
	char valid;			
	unsigned long last_updated;	

	
	u8  in[6];
	u8  in_max[6];
	u8  in_min[6];
	u8  temp[7];
	u8  temp_max[7];
	u8  temp_hyst[7];
	u8  fan[2];
	u8  fan_min[2];
	u8  fan_div[2];
	u8  fan_ctl;
	u8  pwm[2];
	u8  pwm_ctl[2];
	u8  pwm_clk;
	u8  pwm_auto_temp[4];
	u8  pwm_auto_pwm[2][4];
	u8  vid;		
	u8  vrm;
	u8  uch_config;		
	u16 alarms;
};

#define ISVOLT(ix, uch_config)	((ix) > 4 ? 1 : \
				 !(((uch_config) >> ((ix) + 2)) & 1))

#define ISTEMP(ix, uch_config)	((ix) < 2 ? 1 : \
				 ((uch_config) >> (ix)) & 1)

#define IN_FROM_REG(ix, reg)	((reg) < 3 ? 0 : (ix) == 5 ? \
				 (((reg) - 3) * 15882 + 479) / 958 : \
				 (((reg) - 3) * 10000 + 479) / 958)
#define IN_TO_REG(ix, val)	(SENSORS_LIMIT((ix) == 5 ? \
				 ((val) * 958 + 7941) / 15882 + 3 : \
				 ((val) * 958 + 5000) / 10000 + 3, 0, 255))

#define TEMP_FROM_REG(ix, reg)	((ix) == 0 ? (reg) * 1000 : \
				 (ix) == 1 ? (reg) < 51 ? 0 : \
				 ((reg) - 51) * 1000 : \
				 ((253 - (reg)) * 2200 + 105) / 210)
#define TEMP_TO_REG(ix, val)	SENSORS_LIMIT( \
				 ((ix) == 0 ? ((val) + 500) / 1000 : \
				  (ix) == 1 ? ((val) + 500) / 1000 + 51 : \
				  253 - ((val) * 210 + 1100) / 2200), 0, 255)

#define DIV_FROM_REG(reg)	(1 << (reg))

#define RPM_FROM_REG(reg, div)	(((reg) == 0) || ((reg) == 255) ? 0 : \
				 1310720 / (reg) / DIV_FROM_REG(div))
#define RPM_TO_REG(val, div)	((val) == 0 ? 255 : \
				 SENSORS_LIMIT((1310720 / (val) / \
				 DIV_FROM_REG(div)), 1, 254))


#define SIO_REG_CIP1		0x2e
#define SIO_REG_CIP2		0x4e

#define SIO_VT1211_LDN		0x07	
#define SIO_VT1211_DEVID	0x20	
#define SIO_VT1211_DEVREV	0x21	
#define SIO_VT1211_ACTIVE	0x30	
#define SIO_VT1211_BADDR	0x60	
#define SIO_VT1211_ID		0x3c	

#define SIO_VT1211_LDN_HWMON	0x0b	

static inline void superio_outb(int sio_cip, int reg, int val)
{
	outb(reg, sio_cip);
	outb(val, sio_cip + 1);
}

static inline int superio_inb(int sio_cip, int reg)
{
	outb(reg, sio_cip);
	return inb(sio_cip + 1);
}

static inline void superio_select(int sio_cip, int ldn)
{
	outb(SIO_VT1211_LDN, sio_cip);
	outb(ldn, sio_cip + 1);
}

static inline void superio_enter(int sio_cip)
{
	outb(0x87, sio_cip);
	outb(0x87, sio_cip);
}

static inline void superio_exit(int sio_cip)
{
	outb(0xaa, sio_cip);
}


static inline u8 vt1211_read8(struct vt1211_data *data, u8 reg)
{
	return inb(data->addr + reg);
}

static inline void vt1211_write8(struct vt1211_data *data, u8 reg, u8 val)
{
	outb(val, data->addr + reg);
}

static struct vt1211_data *vt1211_update_device(struct device *dev)
{
	struct vt1211_data *data = dev_get_drvdata(dev);
	int ix, val;

	mutex_lock(&data->update_lock);

	
	if (time_after(jiffies, data->last_updated + HZ) || !data->valid) {
		
		data->vid = vt1211_read8(data, VT1211_REG_VID) & 0x1f;

		
		for (ix = 0; ix < ARRAY_SIZE(data->in); ix++) {
			if (ISVOLT(ix, data->uch_config)) {
				data->in[ix] = vt1211_read8(data,
						VT1211_REG_IN(ix));
				data->in_min[ix] = vt1211_read8(data,
						VT1211_REG_IN_MIN(ix));
				data->in_max[ix] = vt1211_read8(data,
						VT1211_REG_IN_MAX(ix));
			}
		}

		
		for (ix = 0; ix < ARRAY_SIZE(data->temp); ix++) {
			if (ISTEMP(ix, data->uch_config)) {
				data->temp[ix] = vt1211_read8(data,
						regtemp[ix]);
				data->temp_max[ix] = vt1211_read8(data,
						regtempmax[ix]);
				data->temp_hyst[ix] = vt1211_read8(data,
						regtemphyst[ix]);
			}
		}

		
		for (ix = 0; ix < ARRAY_SIZE(data->fan); ix++) {
			data->fan[ix] = vt1211_read8(data,
						VT1211_REG_FAN(ix));
			data->fan_min[ix] = vt1211_read8(data,
						VT1211_REG_FAN_MIN(ix));
			data->pwm[ix] = vt1211_read8(data,
						VT1211_REG_PWM(ix));
		}
		val = vt1211_read8(data, VT1211_REG_FAN_DIV);
		data->fan_div[0] = (val >> 4) & 3;
		data->fan_div[1] = (val >> 6) & 3;
		data->fan_ctl = val & 0xf;

		val = vt1211_read8(data, VT1211_REG_PWM_CTL);
		data->pwm_ctl[0] = val & 0xf;
		data->pwm_ctl[1] = (val >> 4) & 0xf;

		data->pwm_clk = vt1211_read8(data, VT1211_REG_PWM_CLK);

		
		data->pwm_auto_pwm[0][1] = vt1211_read8(data,
						VT1211_REG_PWM_AUTO_PWM(0, 1));
		data->pwm_auto_pwm[0][2] = vt1211_read8(data,
						VT1211_REG_PWM_AUTO_PWM(0, 2));
		data->pwm_auto_pwm[1][1] = vt1211_read8(data,
						VT1211_REG_PWM_AUTO_PWM(1, 1));
		data->pwm_auto_pwm[1][2] = vt1211_read8(data,
						VT1211_REG_PWM_AUTO_PWM(1, 2));
		for (ix = 0; ix < ARRAY_SIZE(data->pwm_auto_temp); ix++) {
			data->pwm_auto_temp[ix] = vt1211_read8(data,
						VT1211_REG_PWM_AUTO_TEMP(ix));
		}

		
		data->alarms = (vt1211_read8(data, VT1211_REG_ALARM2) << 8) |
				vt1211_read8(data, VT1211_REG_ALARM1);

		data->last_updated = jiffies;
		data->valid = 1;
	}

	mutex_unlock(&data->update_lock);

	return data;
}


#define SHOW_IN_INPUT	0
#define SHOW_SET_IN_MIN	1
#define SHOW_SET_IN_MAX	2
#define SHOW_IN_ALARM	3

static ssize_t show_in(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct vt1211_data *data = vt1211_update_device(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int fn = sensor_attr_2->nr;
	int res;

	switch (fn) {
	case SHOW_IN_INPUT:
		res = IN_FROM_REG(ix, data->in[ix]);
		break;
	case SHOW_SET_IN_MIN:
		res = IN_FROM_REG(ix, data->in_min[ix]);
		break;
	case SHOW_SET_IN_MAX:
		res = IN_FROM_REG(ix, data->in_max[ix]);
		break;
	case SHOW_IN_ALARM:
		res = (data->alarms >> bitalarmin[ix]) & 1;
		break;
	default:
		res = 0;
		dev_dbg(dev, "Unknown attr fetch (%d)\n", fn);
	}

	return sprintf(buf, "%d\n", res);
}

static ssize_t set_in(struct device *dev, struct device_attribute *attr,
		      const char *buf, size_t count)
{
	struct vt1211_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int fn = sensor_attr_2->nr;
	long val;
	int err;

	err = kstrtol(buf, 10, &val);
	if (err)
		return err;

	mutex_lock(&data->update_lock);
	switch (fn) {
	case SHOW_SET_IN_MIN:
		data->in_min[ix] = IN_TO_REG(ix, val);
		vt1211_write8(data, VT1211_REG_IN_MIN(ix), data->in_min[ix]);
		break;
	case SHOW_SET_IN_MAX:
		data->in_max[ix] = IN_TO_REG(ix, val);
		vt1211_write8(data, VT1211_REG_IN_MAX(ix), data->in_max[ix]);
		break;
	default:
		dev_dbg(dev, "Unknown attr fetch (%d)\n", fn);
	}
	mutex_unlock(&data->update_lock);

	return count;
}


#define SHOW_TEMP_INPUT		0
#define SHOW_SET_TEMP_MAX	1
#define SHOW_SET_TEMP_MAX_HYST	2
#define SHOW_TEMP_ALARM		3

static ssize_t show_temp(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct vt1211_data *data = vt1211_update_device(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int fn = sensor_attr_2->nr;
	int res;

	switch (fn) {
	case SHOW_TEMP_INPUT:
		res = TEMP_FROM_REG(ix, data->temp[ix]);
		break;
	case SHOW_SET_TEMP_MAX:
		res = TEMP_FROM_REG(ix, data->temp_max[ix]);
		break;
	case SHOW_SET_TEMP_MAX_HYST:
		res = TEMP_FROM_REG(ix, data->temp_hyst[ix]);
		break;
	case SHOW_TEMP_ALARM:
		res = (data->alarms >> bitalarmtemp[ix]) & 1;
		break;
	default:
		res = 0;
		dev_dbg(dev, "Unknown attr fetch (%d)\n", fn);
	}

	return sprintf(buf, "%d\n", res);
}

static ssize_t set_temp(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct vt1211_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int fn = sensor_attr_2->nr;
	long val;
	int err;

	err = kstrtol(buf, 10, &val);
	if (err)
		return err;

	mutex_lock(&data->update_lock);
	switch (fn) {
	case SHOW_SET_TEMP_MAX:
		data->temp_max[ix] = TEMP_TO_REG(ix, val);
		vt1211_write8(data, regtempmax[ix],
			      data->temp_max[ix]);
		break;
	case SHOW_SET_TEMP_MAX_HYST:
		data->temp_hyst[ix] = TEMP_TO_REG(ix, val);
		vt1211_write8(data, regtemphyst[ix],
			      data->temp_hyst[ix]);
		break;
	default:
		dev_dbg(dev, "Unknown attr fetch (%d)\n", fn);
	}
	mutex_unlock(&data->update_lock);

	return count;
}


#define SHOW_FAN_INPUT		0
#define SHOW_SET_FAN_MIN	1
#define SHOW_SET_FAN_DIV	2
#define SHOW_FAN_ALARM		3

static ssize_t show_fan(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct vt1211_data *data = vt1211_update_device(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int fn = sensor_attr_2->nr;
	int res;

	switch (fn) {
	case SHOW_FAN_INPUT:
		res = RPM_FROM_REG(data->fan[ix], data->fan_div[ix]);
		break;
	case SHOW_SET_FAN_MIN:
		res = RPM_FROM_REG(data->fan_min[ix], data->fan_div[ix]);
		break;
	case SHOW_SET_FAN_DIV:
		res = DIV_FROM_REG(data->fan_div[ix]);
		break;
	case SHOW_FAN_ALARM:
		res = (data->alarms >> bitalarmfan[ix]) & 1;
		break;
	default:
		res = 0;
		dev_dbg(dev, "Unknown attr fetch (%d)\n", fn);
	}

	return sprintf(buf, "%d\n", res);
}

static ssize_t set_fan(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	struct vt1211_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int fn = sensor_attr_2->nr;
	int reg;
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	mutex_lock(&data->update_lock);

	
	reg = vt1211_read8(data, VT1211_REG_FAN_DIV);
	data->fan_div[0] = (reg >> 4) & 3;
	data->fan_div[1] = (reg >> 6) & 3;
	data->fan_ctl = reg & 0xf;

	switch (fn) {
	case SHOW_SET_FAN_MIN:
		data->fan_min[ix] = RPM_TO_REG(val, data->fan_div[ix]);
		vt1211_write8(data, VT1211_REG_FAN_MIN(ix),
			      data->fan_min[ix]);
		break;
	case SHOW_SET_FAN_DIV:
		switch (val) {
		case 1:
			data->fan_div[ix] = 0;
			break;
		case 2:
			data->fan_div[ix] = 1;
			break;
		case 4:
			data->fan_div[ix] = 2;
			break;
		case 8:
			data->fan_div[ix] = 3;
			break;
		default:
			count = -EINVAL;
			dev_warn(dev, "fan div value %ld not supported. "
				 "Choose one of 1, 2, 4, or 8.\n", val);
			goto EXIT;
		}
		vt1211_write8(data, VT1211_REG_FAN_DIV,
			      ((data->fan_div[1] << 6) |
			       (data->fan_div[0] << 4) |
				data->fan_ctl));
		break;
	default:
		dev_dbg(dev, "Unknown attr fetch (%d)\n", fn);
	}

EXIT:
	mutex_unlock(&data->update_lock);
	return count;
}


#define SHOW_PWM			0
#define SHOW_SET_PWM_ENABLE		1
#define SHOW_SET_PWM_FREQ		2
#define SHOW_SET_PWM_AUTO_CHANNELS_TEMP	3

static ssize_t show_pwm(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct vt1211_data *data = vt1211_update_device(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int fn = sensor_attr_2->nr;
	int res;

	switch (fn) {
	case SHOW_PWM:
		res = data->pwm[ix];
		break;
	case SHOW_SET_PWM_ENABLE:
		res = ((data->pwm_ctl[ix] >> 3) & 1) ? 2 : 0;
		break;
	case SHOW_SET_PWM_FREQ:
		res = 90000 >> (data->pwm_clk & 7);
		break;
	case SHOW_SET_PWM_AUTO_CHANNELS_TEMP:
		res = (data->pwm_ctl[ix] & 7) + 1;
		break;
	default:
		res = 0;
		dev_dbg(dev, "Unknown attr fetch (%d)\n", fn);
	}

	return sprintf(buf, "%d\n", res);
}

static ssize_t set_pwm(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	struct vt1211_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int fn = sensor_attr_2->nr;
	int tmp, reg;
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	mutex_lock(&data->update_lock);

	switch (fn) {
	case SHOW_SET_PWM_ENABLE:
		
		reg = vt1211_read8(data, VT1211_REG_FAN_DIV);
		data->fan_div[0] = (reg >> 4) & 3;
		data->fan_div[1] = (reg >> 6) & 3;
		data->fan_ctl = reg & 0xf;
		reg = vt1211_read8(data, VT1211_REG_PWM_CTL);
		data->pwm_ctl[0] = reg & 0xf;
		data->pwm_ctl[1] = (reg >> 4) & 0xf;
		switch (val) {
		case 0:
			data->pwm_ctl[ix] &= 7;
			if ((data->pwm_ctl[ix ^ 1] & 1) == 0)
				data->fan_ctl &= 0xe;
			break;
		case 2:
			data->pwm_ctl[ix] |= 8;
			data->fan_ctl |= 1;
			break;
		default:
			count = -EINVAL;
			dev_warn(dev, "pwm mode %ld not supported. "
				 "Choose one of 0 or 2.\n", val);
			goto EXIT;
		}
		vt1211_write8(data, VT1211_REG_PWM_CTL,
			      ((data->pwm_ctl[1] << 4) |
				data->pwm_ctl[0]));
		vt1211_write8(data, VT1211_REG_FAN_DIV,
			      ((data->fan_div[1] << 6) |
			       (data->fan_div[0] << 4) |
				data->fan_ctl));
		break;
	case SHOW_SET_PWM_FREQ:
		val = 135000 / SENSORS_LIMIT(val, 135000 >> 7, 135000);
		
		tmp = 0;
		for (val >>= 1; val > 0; val >>= 1)
			tmp++;
		
		reg = vt1211_read8(data, VT1211_REG_PWM_CLK);
		data->pwm_clk = (reg & 0xf8) | tmp;
		vt1211_write8(data, VT1211_REG_PWM_CLK, data->pwm_clk);
		break;
	case SHOW_SET_PWM_AUTO_CHANNELS_TEMP:
		if (val < 1 || val > 7) {
			count = -EINVAL;
			dev_warn(dev, "temp channel %ld not supported. "
				 "Choose a value between 1 and 7.\n", val);
			goto EXIT;
		}
		if (!ISTEMP(val - 1, data->uch_config)) {
			count = -EINVAL;
			dev_warn(dev, "temp channel %ld is not available.\n",
				 val);
			goto EXIT;
		}
		
		reg = vt1211_read8(data, VT1211_REG_PWM_CTL);
		data->pwm_ctl[0] = reg & 0xf;
		data->pwm_ctl[1] = (reg >> 4) & 0xf;
		data->pwm_ctl[ix] = (data->pwm_ctl[ix] & 8) | (val - 1);
		vt1211_write8(data, VT1211_REG_PWM_CTL,
			      ((data->pwm_ctl[1] << 4) | data->pwm_ctl[0]));
		break;
	default:
		dev_dbg(dev, "Unknown attr fetch (%d)\n", fn);
	}

EXIT:
	mutex_unlock(&data->update_lock);
	return count;
}



static ssize_t show_pwm_auto_point_temp(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct vt1211_data *data = vt1211_update_device(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int ap = sensor_attr_2->nr;

	return sprintf(buf, "%d\n", TEMP_FROM_REG(data->pwm_ctl[ix] & 7,
		       data->pwm_auto_temp[ap]));
}

static ssize_t set_pwm_auto_point_temp(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct vt1211_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int ap = sensor_attr_2->nr;
	int reg;
	long val;
	int err;

	err = kstrtol(buf, 10, &val);
	if (err)
		return err;


	mutex_lock(&data->update_lock);

	
	reg = vt1211_read8(data, VT1211_REG_PWM_CTL);
	data->pwm_ctl[0] = reg & 0xf;
	data->pwm_ctl[1] = (reg >> 4) & 0xf;

	data->pwm_auto_temp[ap] = TEMP_TO_REG(data->pwm_ctl[ix] & 7, val);
	vt1211_write8(data, VT1211_REG_PWM_AUTO_TEMP(ap),
		      data->pwm_auto_temp[ap]);
	mutex_unlock(&data->update_lock);

	return count;
}


static ssize_t show_pwm_auto_point_pwm(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct vt1211_data *data = vt1211_update_device(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int ap = sensor_attr_2->nr;

	return sprintf(buf, "%d\n", data->pwm_auto_pwm[ix][ap]);
}

static ssize_t set_pwm_auto_point_pwm(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct vt1211_data *data = dev_get_drvdata(dev);
	struct sensor_device_attribute_2 *sensor_attr_2 =
						to_sensor_dev_attr_2(attr);
	int ix = sensor_attr_2->index;
	int ap = sensor_attr_2->nr;
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	mutex_lock(&data->update_lock);
	data->pwm_auto_pwm[ix][ap] = SENSORS_LIMIT(val, 0, 255);
	vt1211_write8(data, VT1211_REG_PWM_AUTO_PWM(ix, ap),
		      data->pwm_auto_pwm[ix][ap]);
	mutex_unlock(&data->update_lock);

	return count;
}


static ssize_t show_vrm(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct vt1211_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", data->vrm);
}

static ssize_t set_vrm(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	struct vt1211_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	data->vrm = val;

	return count;
}

static ssize_t show_vid(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct vt1211_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", vid_from_reg(data->vid, data->vrm));
}

static ssize_t show_name(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct vt1211_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", data->name);
}

static ssize_t show_alarms(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct vt1211_data *data = vt1211_update_device(dev);

	return sprintf(buf, "%d\n", data->alarms);
}


#define SENSOR_ATTR_IN(ix) \
{	SENSOR_ATTR_2(in##ix##_input, S_IRUGO, \
		show_in, NULL, SHOW_IN_INPUT, ix), \
	SENSOR_ATTR_2(in##ix##_min, S_IRUGO | S_IWUSR, \
		show_in, set_in, SHOW_SET_IN_MIN, ix), \
	SENSOR_ATTR_2(in##ix##_max, S_IRUGO | S_IWUSR, \
		show_in, set_in, SHOW_SET_IN_MAX, ix), \
	SENSOR_ATTR_2(in##ix##_alarm, S_IRUGO, \
		show_in, NULL, SHOW_IN_ALARM, ix) \
}

static struct sensor_device_attribute_2 vt1211_sysfs_in[][4] = {
	SENSOR_ATTR_IN(0),
	SENSOR_ATTR_IN(1),
	SENSOR_ATTR_IN(2),
	SENSOR_ATTR_IN(3),
	SENSOR_ATTR_IN(4),
	SENSOR_ATTR_IN(5)
};

#define IN_UNIT_ATTRS(X)			\
{	&vt1211_sysfs_in[X][0].dev_attr.attr,	\
	&vt1211_sysfs_in[X][1].dev_attr.attr,	\
	&vt1211_sysfs_in[X][2].dev_attr.attr,	\
	&vt1211_sysfs_in[X][3].dev_attr.attr,	\
	NULL					\
}

static struct attribute *vt1211_in_attr[][5] = {
	IN_UNIT_ATTRS(0),
	IN_UNIT_ATTRS(1),
	IN_UNIT_ATTRS(2),
	IN_UNIT_ATTRS(3),
	IN_UNIT_ATTRS(4),
	IN_UNIT_ATTRS(5)
};

static const struct attribute_group vt1211_in_attr_group[] = {
	{ .attrs = vt1211_in_attr[0] },
	{ .attrs = vt1211_in_attr[1] },
	{ .attrs = vt1211_in_attr[2] },
	{ .attrs = vt1211_in_attr[3] },
	{ .attrs = vt1211_in_attr[4] },
	{ .attrs = vt1211_in_attr[5] }
};

#define SENSOR_ATTR_TEMP(ix) \
{	SENSOR_ATTR_2(temp##ix##_input, S_IRUGO, \
		show_temp, NULL, SHOW_TEMP_INPUT, ix-1), \
	SENSOR_ATTR_2(temp##ix##_max, S_IRUGO | S_IWUSR, \
		show_temp, set_temp, SHOW_SET_TEMP_MAX, ix-1), \
	SENSOR_ATTR_2(temp##ix##_max_hyst, S_IRUGO | S_IWUSR, \
		show_temp, set_temp, SHOW_SET_TEMP_MAX_HYST, ix-1), \
	SENSOR_ATTR_2(temp##ix##_alarm, S_IRUGO, \
		show_temp, NULL, SHOW_TEMP_ALARM, ix-1) \
}

static struct sensor_device_attribute_2 vt1211_sysfs_temp[][4] = {
	SENSOR_ATTR_TEMP(1),
	SENSOR_ATTR_TEMP(2),
	SENSOR_ATTR_TEMP(3),
	SENSOR_ATTR_TEMP(4),
	SENSOR_ATTR_TEMP(5),
	SENSOR_ATTR_TEMP(6),
	SENSOR_ATTR_TEMP(7),
};

#define TEMP_UNIT_ATTRS(X)			\
{	&vt1211_sysfs_temp[X][0].dev_attr.attr,	\
	&vt1211_sysfs_temp[X][1].dev_attr.attr,	\
	&vt1211_sysfs_temp[X][2].dev_attr.attr,	\
	&vt1211_sysfs_temp[X][3].dev_attr.attr,	\
	NULL					\
}

static struct attribute *vt1211_temp_attr[][5] = {
	TEMP_UNIT_ATTRS(0),
	TEMP_UNIT_ATTRS(1),
	TEMP_UNIT_ATTRS(2),
	TEMP_UNIT_ATTRS(3),
	TEMP_UNIT_ATTRS(4),
	TEMP_UNIT_ATTRS(5),
	TEMP_UNIT_ATTRS(6)
};

static const struct attribute_group vt1211_temp_attr_group[] = {
	{ .attrs = vt1211_temp_attr[0] },
	{ .attrs = vt1211_temp_attr[1] },
	{ .attrs = vt1211_temp_attr[2] },
	{ .attrs = vt1211_temp_attr[3] },
	{ .attrs = vt1211_temp_attr[4] },
	{ .attrs = vt1211_temp_attr[5] },
	{ .attrs = vt1211_temp_attr[6] }
};

#define SENSOR_ATTR_FAN(ix) \
	SENSOR_ATTR_2(fan##ix##_input, S_IRUGO, \
		show_fan, NULL, SHOW_FAN_INPUT, ix-1), \
	SENSOR_ATTR_2(fan##ix##_min, S_IRUGO | S_IWUSR, \
		show_fan, set_fan, SHOW_SET_FAN_MIN, ix-1), \
	SENSOR_ATTR_2(fan##ix##_div, S_IRUGO | S_IWUSR, \
		show_fan, set_fan, SHOW_SET_FAN_DIV, ix-1), \
	SENSOR_ATTR_2(fan##ix##_alarm, S_IRUGO, \
		show_fan, NULL, SHOW_FAN_ALARM, ix-1)

#define SENSOR_ATTR_PWM(ix) \
	SENSOR_ATTR_2(pwm##ix, S_IRUGO, \
		show_pwm, NULL, SHOW_PWM, ix-1), \
	SENSOR_ATTR_2(pwm##ix##_enable, S_IRUGO | S_IWUSR, \
		show_pwm, set_pwm, SHOW_SET_PWM_ENABLE, ix-1), \
	SENSOR_ATTR_2(pwm##ix##_auto_channels_temp, S_IRUGO | S_IWUSR, \
		show_pwm, set_pwm, SHOW_SET_PWM_AUTO_CHANNELS_TEMP, ix-1)

#define SENSOR_ATTR_PWM_FREQ(ix) \
	SENSOR_ATTR_2(pwm##ix##_freq, S_IRUGO | S_IWUSR, \
		show_pwm, set_pwm, SHOW_SET_PWM_FREQ, ix-1)

#define SENSOR_ATTR_PWM_FREQ_RO(ix) \
	SENSOR_ATTR_2(pwm##ix##_freq, S_IRUGO, \
		show_pwm, NULL, SHOW_SET_PWM_FREQ, ix-1)

#define SENSOR_ATTR_PWM_AUTO_POINT_TEMP(ix, ap) \
	SENSOR_ATTR_2(pwm##ix##_auto_point##ap##_temp, S_IRUGO | S_IWUSR, \
		show_pwm_auto_point_temp, set_pwm_auto_point_temp, \
		ap-1, ix-1)

#define SENSOR_ATTR_PWM_AUTO_POINT_TEMP_RO(ix, ap) \
	SENSOR_ATTR_2(pwm##ix##_auto_point##ap##_temp, S_IRUGO, \
		show_pwm_auto_point_temp, NULL, \
		ap-1, ix-1)

#define SENSOR_ATTR_PWM_AUTO_POINT_PWM(ix, ap) \
	SENSOR_ATTR_2(pwm##ix##_auto_point##ap##_pwm, S_IRUGO | S_IWUSR, \
		show_pwm_auto_point_pwm, set_pwm_auto_point_pwm, \
		ap-1, ix-1)

#define SENSOR_ATTR_PWM_AUTO_POINT_PWM_RO(ix, ap) \
	SENSOR_ATTR_2(pwm##ix##_auto_point##ap##_pwm, S_IRUGO, \
		show_pwm_auto_point_pwm, NULL, \
		ap-1, ix-1)

static struct sensor_device_attribute_2 vt1211_sysfs_fan_pwm[] = {
	SENSOR_ATTR_FAN(1),
	SENSOR_ATTR_FAN(2),
	SENSOR_ATTR_PWM(1),
	SENSOR_ATTR_PWM(2),
	SENSOR_ATTR_PWM_FREQ(1),
	SENSOR_ATTR_PWM_FREQ_RO(2),
	SENSOR_ATTR_PWM_AUTO_POINT_TEMP(1, 1),
	SENSOR_ATTR_PWM_AUTO_POINT_TEMP(1, 2),
	SENSOR_ATTR_PWM_AUTO_POINT_TEMP(1, 3),
	SENSOR_ATTR_PWM_AUTO_POINT_TEMP(1, 4),
	SENSOR_ATTR_PWM_AUTO_POINT_TEMP_RO(2, 1),
	SENSOR_ATTR_PWM_AUTO_POINT_TEMP_RO(2, 2),
	SENSOR_ATTR_PWM_AUTO_POINT_TEMP_RO(2, 3),
	SENSOR_ATTR_PWM_AUTO_POINT_TEMP_RO(2, 4),
	SENSOR_ATTR_PWM_AUTO_POINT_PWM_RO(1, 1),
	SENSOR_ATTR_PWM_AUTO_POINT_PWM(1, 2),
	SENSOR_ATTR_PWM_AUTO_POINT_PWM(1, 3),
	SENSOR_ATTR_PWM_AUTO_POINT_PWM_RO(1, 4),
	SENSOR_ATTR_PWM_AUTO_POINT_PWM_RO(2, 1),
	SENSOR_ATTR_PWM_AUTO_POINT_PWM(2, 2),
	SENSOR_ATTR_PWM_AUTO_POINT_PWM(2, 3),
	SENSOR_ATTR_PWM_AUTO_POINT_PWM_RO(2, 4),
};

static struct device_attribute vt1211_sysfs_misc[] = {
	__ATTR(vrm, S_IRUGO | S_IWUSR, show_vrm, set_vrm),
	__ATTR(cpu0_vid, S_IRUGO, show_vid, NULL),
	__ATTR(name, S_IRUGO, show_name, NULL),
	__ATTR(alarms, S_IRUGO, show_alarms, NULL),
};


static void __devinit vt1211_init_device(struct vt1211_data *data)
{
	
	data->vrm = vid_which_vrm();

	
	data->uch_config = vt1211_read8(data, VT1211_REG_UCH_CONFIG);
	if (uch_config > -1) {
		data->uch_config = (data->uch_config & 0x83) |
				   (uch_config << 2);
		vt1211_write8(data, VT1211_REG_UCH_CONFIG, data->uch_config);
	}

	if (int_mode == 0) {
		vt1211_write8(data, VT1211_REG_TEMP1_CONFIG, 0);
		vt1211_write8(data, VT1211_REG_TEMP2_CONFIG, 0);
	}

	
	data->pwm_auto_pwm[0][3] = 255;
	data->pwm_auto_pwm[1][3] = 255;
}

static void vt1211_remove_sysfs(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(vt1211_in_attr_group); i++)
		sysfs_remove_group(&dev->kobj, &vt1211_in_attr_group[i]);

	for (i = 0; i < ARRAY_SIZE(vt1211_temp_attr_group); i++)
		sysfs_remove_group(&dev->kobj, &vt1211_temp_attr_group[i]);

	for (i = 0; i < ARRAY_SIZE(vt1211_sysfs_fan_pwm); i++) {
		device_remove_file(dev,
			&vt1211_sysfs_fan_pwm[i].dev_attr);
	}
	for (i = 0; i < ARRAY_SIZE(vt1211_sysfs_misc); i++)
		device_remove_file(dev, &vt1211_sysfs_misc[i]);
}

static int __devinit vt1211_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vt1211_data *data;
	struct resource *res;
	int i, err;

	data = kzalloc(sizeof(struct vt1211_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		dev_err(dev, "Out of memory\n");
		goto EXIT;
	}

	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	if (!request_region(res->start, resource_size(res), DRVNAME)) {
		err = -EBUSY;
		dev_err(dev, "Failed to request region 0x%lx-0x%lx\n",
			(unsigned long)res->start, (unsigned long)res->end);
		goto EXIT_KFREE;
	}
	data->addr = res->start;
	data->name = DRVNAME;
	mutex_init(&data->update_lock);

	platform_set_drvdata(pdev, data);

	
	vt1211_init_device(data);

	
	for (i = 0; i < ARRAY_SIZE(vt1211_in_attr_group); i++) {
		if (ISVOLT(i, data->uch_config)) {
			err = sysfs_create_group(&dev->kobj,
						 &vt1211_in_attr_group[i]);
			if (err)
				goto EXIT_DEV_REMOVE;
		}
	}
	for (i = 0; i < ARRAY_SIZE(vt1211_temp_attr_group); i++) {
		if (ISTEMP(i, data->uch_config)) {
			err = sysfs_create_group(&dev->kobj,
						 &vt1211_temp_attr_group[i]);
			if (err)
				goto EXIT_DEV_REMOVE;
		}
	}
	for (i = 0; i < ARRAY_SIZE(vt1211_sysfs_fan_pwm); i++) {
		err = device_create_file(dev,
			&vt1211_sysfs_fan_pwm[i].dev_attr);
		if (err)
			goto EXIT_DEV_REMOVE;
	}
	for (i = 0; i < ARRAY_SIZE(vt1211_sysfs_misc); i++) {
		err = device_create_file(dev,
		       &vt1211_sysfs_misc[i]);
		if (err)
			goto EXIT_DEV_REMOVE;
	}

	
	data->hwmon_dev = hwmon_device_register(dev);
	if (IS_ERR(data->hwmon_dev)) {
		err = PTR_ERR(data->hwmon_dev);
		dev_err(dev, "Class registration failed (%d)\n", err);
		goto EXIT_DEV_REMOVE_SILENT;
	}

	return 0;

EXIT_DEV_REMOVE:
	dev_err(dev, "Sysfs interface creation failed (%d)\n", err);
EXIT_DEV_REMOVE_SILENT:
	vt1211_remove_sysfs(pdev);
	release_region(res->start, resource_size(res));
EXIT_KFREE:
	platform_set_drvdata(pdev, NULL);
	kfree(data);
EXIT:
	return err;
}

static int __devexit vt1211_remove(struct platform_device *pdev)
{
	struct vt1211_data *data = platform_get_drvdata(pdev);
	struct resource *res;

	hwmon_device_unregister(data->hwmon_dev);
	vt1211_remove_sysfs(pdev);
	platform_set_drvdata(pdev, NULL);
	kfree(data);

	res = platform_get_resource(pdev, IORESOURCE_IO, 0);
	release_region(res->start, resource_size(res));

	return 0;
}

static struct platform_driver vt1211_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = DRVNAME,
	},
	.probe  = vt1211_probe,
	.remove = __devexit_p(vt1211_remove),
};

static int __init vt1211_device_add(unsigned short address)
{
	struct resource res = {
		.start	= address,
		.end	= address + 0x7f,
		.flags	= IORESOURCE_IO,
	};
	int err;

	pdev = platform_device_alloc(DRVNAME, address);
	if (!pdev) {
		err = -ENOMEM;
		pr_err("Device allocation failed (%d)\n", err);
		goto EXIT;
	}

	res.name = pdev->name;
	err = acpi_check_resource_conflict(&res);
	if (err)
		goto EXIT_DEV_PUT;

	err = platform_device_add_resources(pdev, &res, 1);
	if (err) {
		pr_err("Device resource addition failed (%d)\n", err);
		goto EXIT_DEV_PUT;
	}

	err = platform_device_add(pdev);
	if (err) {
		pr_err("Device addition failed (%d)\n", err);
		goto EXIT_DEV_PUT;
	}

	return 0;

EXIT_DEV_PUT:
	platform_device_put(pdev);
EXIT:
	return err;
}

static int __init vt1211_find(int sio_cip, unsigned short *address)
{
	int err = -ENODEV;
	int devid;

	superio_enter(sio_cip);

	devid = force_id ? force_id : superio_inb(sio_cip, SIO_VT1211_DEVID);
	if (devid != SIO_VT1211_ID)
		goto EXIT;

	superio_select(sio_cip, SIO_VT1211_LDN_HWMON);

	if ((superio_inb(sio_cip, SIO_VT1211_ACTIVE) & 1) == 0) {
		pr_warn("HW monitor is disabled, skipping\n");
		goto EXIT;
	}

	*address = ((superio_inb(sio_cip, SIO_VT1211_BADDR) << 8) |
		    (superio_inb(sio_cip, SIO_VT1211_BADDR + 1))) & 0xff00;
	if (*address == 0) {
		pr_warn("Base address is not set, skipping\n");
		goto EXIT;
	}

	err = 0;
	pr_info("Found VT1211 chip at 0x%04x, revision %u\n",
		*address, superio_inb(sio_cip, SIO_VT1211_DEVREV));

EXIT:
	superio_exit(sio_cip);
	return err;
}

static int __init vt1211_init(void)
{
	int err;
	unsigned short address = 0;

	err = vt1211_find(SIO_REG_CIP1, &address);
	if (err) {
		err = vt1211_find(SIO_REG_CIP2, &address);
		if (err)
			goto EXIT;
	}

	if ((uch_config < -1) || (uch_config > 31)) {
		err = -EINVAL;
		pr_warn("Invalid UCH configuration %d. "
			"Choose a value between 0 and 31.\n", uch_config);
		goto EXIT;
	}

	if ((int_mode < -1) || (int_mode > 0)) {
		err = -EINVAL;
		pr_warn("Invalid interrupt mode %d. "
			"Only mode 0 is supported.\n", int_mode);
		goto EXIT;
	}

	err = platform_driver_register(&vt1211_driver);
	if (err)
		goto EXIT;

	
	err = vt1211_device_add(address);
	if (err)
		goto EXIT_DRV_UNREGISTER;

	return 0;

EXIT_DRV_UNREGISTER:
	platform_driver_unregister(&vt1211_driver);
EXIT:
	return err;
}

static void __exit vt1211_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&vt1211_driver);
}

MODULE_AUTHOR("Juerg Haefliger <juergh@gmail.com>");
MODULE_DESCRIPTION("VT1211 sensors");
MODULE_LICENSE("GPL");

module_init(vt1211_init);
module_exit(vt1211_exit);
