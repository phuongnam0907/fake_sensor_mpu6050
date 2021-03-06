/*
 * MPU6050 6-axis gyroscope + accelerometer driver
 *
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
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
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#include <linux/sensors.h>
#include "fake6050.h"
#include <linux/kthread.h>

#define MPU6050_ACCEL_MIN_VALUE	-32768
#define MPU6050_ACCEL_MAX_VALUE	32767
#define MPU6050_GYRO_MIN_VALUE	-32768
#define MPU6050_GYRO_MAX_VALUE	32767

#define MPU6050_MAX_EVENT_CNT	170
/* Limit mininum delay to 10ms as we do not need higher rate so far */
#define MPU6050_ACCEL_MIN_POLL_INTERVAL_MS	10
#define MPU6050_ACCEL_MAX_POLL_INTERVAL_MS	5000
#define MPU6050_ACCEL_DEFAULT_POLL_INTERVAL_MS	200
#define MPU6050_ACCEL_INT_MAX_DELAY			19

#define MPU6050_GYRO_MIN_POLL_INTERVAL_MS	10
#define MPU6050_GYRO_MAX_POLL_INTERVAL_MS	5000
#define MPU6050_GYRO_DEFAULT_POLL_INTERVAL_MS	200
#define MPU6050_GYRO_INT_MAX_DELAY		18

#define MPU6050_RAW_ACCEL_DATA_LEN	6
#define MPU6050_RAW_GYRO_DATA_LEN	6

#define MPU6050_RESET_SLEEP_US	10

#define MPU6050_DEV_NAME_ACCEL	"MPU6050-accel"
#define MPU6050_DEV_NAME_GYRO	"gyroscope"

#define MPU6050_PINCTRL_DEFAULT	"mpu_default"
#define MPU6050_PINCTRL_SUSPEND	"mpu_sleep"

#define CAL_SKIP_COUNT	5
#define MPU_ACC_CAL_COUNT	15
#define MPU_ACC_CAL_NUM	(MPU_ACC_CAL_COUNT - CAL_SKIP_COUNT)
#define MPU_ACC_CAL_BUF_SIZE	22
#define RAW_TO_1G	16384
#define MPU_ACC_CAL_DELAY 100	/* ms */
#define POLL_MS_100HZ 10
#define SNS_TYPE_GYRO 0
#define SNS_TYPE_ACCEL 1

enum mpu6050_place {
	MPU6050_PLACE_PU = 0,
	MPU6050_PLACE_PR = 1,
	MPU6050_PLACE_LD = 2,
	MPU6050_PLACE_LL = 3,
	MPU6050_PLACE_PU_BACK = 4,
	MPU6050_PLACE_PR_BACK = 5,
	MPU6050_PLACE_LD_BACK = 6,
	MPU6050_PLACE_LL_BACK = 7,
	MPU6050_PLACE_UNKNOWN = 8,
	MPU6050_AXIS_REMAP_TAB_SZ = 8
};

struct mpu6050_place_name {
	char name[32];
	enum mpu6050_place place;
};

struct axis_data {
	s16 x;
	s16 y;
	s16 z;
	s16 rx;
	s16 ry;
	s16 rz;
};

/**
 *  struct mpu6050_sensor - Cached chip configuration data
 *  @client:		I2C client
 *  @dev:		device structure
 *  @accel_dev:		accelerometer input device structure
 *  @gyro_dev:		gyroscope input device structure
 *  @accel_cdev:		sensor class device structure for accelerometer
 *  @gyro_cdev:		sensor class device structure for gyroscope
 *  @pdata:	device platform dependent data
 *  @op_lock:	device operation mutex
 *  @chip_type:	sensor hardware model
 *  @fifo_flush_work:	work structure to flush sensor fifo
 *  @reg:		notable slave registers
 *  @cfg:		cached chip configuration data
 *  @axis:	axis data reading
 *  @gyro_poll_ms:	gyroscope polling delay
 *  @accel_poll_ms:	accelerometer polling delay
 *  @accel_latency_ms:	max latency for accelerometer batching
 *  @gyro_latency_ms:	max latency for gyroscope batching
 *  @accel_en:	accelerometer enabling flag
 *  @gyro_en:	gyroscope enabling flag
 *  @use_poll:		use polling mode instead of  interrupt mode
 *  @motion_det_en:	motion detection wakeup is enabled
 *  @batch_accel:	accelerometer is working on batch mode
 *  @batch_gyro:	gyroscope is working on batch mode
 *  @acc_cal_buf:	accelerometer calibration string format bias
 *  @acc_cal_params:	accelerometer calibration bias
 *  @acc_use_cal:	accelerometer use calibration bias to
			compensate raw data
 *  @enable_gpio:	enable GPIO
 *  @power_enabled:	flag of device power state
 *  @pinctrl:	pinctrl struct for interrupt pin
 *  @pin_default:	pinctrl default state
 *  @pin_sleep:	pinctrl sleep state
 *  @flush_count:	number of flush
 *  @fifo_start_ns:		timestamp of first fifo data
 */
struct mpu6050_sensor {
	struct i2c_client *client;
	struct device *dev;
	struct hrtimer gyro_timer;
	struct hrtimer accel_timer;
	struct input_dev *accel_dev;
	struct input_dev *gyro_dev;
	struct sensors_classdev accel_cdev;
	struct sensors_classdev gyro_cdev;
	struct mpu6050_platform_data *pdata;
	struct mutex op_lock;
	enum inv_devices chip_type;
	struct workqueue_struct *data_wq;
	struct work_struct resume_work;
	struct delayed_work fifo_flush_work;
	struct mpu_reg_map reg;
	struct mpu_chip_config cfg;
	struct axis_data axis;
	u32 gyro_poll_ms;
	u32 accel_poll_ms;
	u32 accel_latency_ms;
	u32 gyro_latency_ms;
	atomic_t accel_en;
	atomic_t gyro_en;
	bool use_poll;
	bool motion_det_en;
	bool batch_accel;
	bool batch_gyro;

	/* calibration */
	char acc_cal_buf[MPU_ACC_CAL_BUF_SIZE];
	int acc_cal_params[3];
	bool acc_use_cal;

	/* power control */
	int enable_gpio;
	bool power_enabled;

	/* pinctrl */
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_default;
	struct pinctrl_state *pin_sleep;

	u32 flush_count;
	u64 fifo_start_ns;
	int gyro_wkp_flag;
	int accel_wkp_flag;
	struct task_struct *gyr_task;
	struct task_struct *accel_task;
	bool gyro_delay_change;
	bool accel_delay_change;
	wait_queue_head_t	gyro_wq;
	wait_queue_head_t	accel_wq;
};

/* Accelerometer information read by HAL */
static struct sensors_classdev mpu6050_acc_cdev = {
	.name = "MPU6050-accel",
	.vendor = "Invensense",
	.version = 1,
	.handle = SENSORS_ACCELERATION_HANDLE,
	.type = SENSOR_TYPE_ACCELEROMETER,
	.max_range = "156.8",	/* m/s^2 */
	.resolution = "0.000598144",	/* m/s^2 */
	.sensor_power = "0.5",	/* 0.5 mA */
	.min_delay = MPU6050_ACCEL_MIN_POLL_INTERVAL_MS * 1000,
	.max_delay = MPU6050_ACCEL_MAX_POLL_INTERVAL_MS,
	.delay_msec = MPU6050_ACCEL_DEFAULT_POLL_INTERVAL_MS,
	.fifo_reserved_event_count = 0,
	.fifo_max_event_count = 0,
	.enabled = 0,
	.max_latency = 0,
	.flags = 0, /* SENSOR_FLAG_CONTINUOUS_MODE */
	.sensors_enable = NULL,
	.sensors_poll_delay = NULL,
	.sensors_enable_wakeup = NULL,
	.sensors_set_latency = NULL,
	.sensors_flush = NULL,
};

/* gyroscope information read by HAL */
static struct sensors_classdev mpu6050_gyro_cdev = {
	.name = "MPU6050-gyro",
	.vendor = "Invensense",
	.version = 1,
	.handle = SENSORS_GYROSCOPE_HANDLE,
	.type = SENSOR_TYPE_GYROSCOPE,
	.max_range = "34.906586",	/* rad/s */
	.resolution = "0.0010681152",	/* rad/s */
	.sensor_power = "3.6",	/* 3.6 mA */
	.min_delay = MPU6050_GYRO_MIN_POLL_INTERVAL_MS * 1000,
	.max_delay = MPU6050_GYRO_MAX_POLL_INTERVAL_MS,
	.delay_msec = MPU6050_ACCEL_DEFAULT_POLL_INTERVAL_MS,
	.fifo_reserved_event_count = 0,
	.fifo_max_event_count = 0,
	.enabled = 0,
	.max_latency = 0,
	.flags = 0, /* SENSOR_FLAG_CONTINUOUS_MODE */
	.sensors_enable = NULL,
	.sensors_poll_delay = NULL,
	.sensors_enable_wakeup = NULL,
	.sensors_set_latency = NULL,
	.sensors_flush = NULL,
};

struct sensor_axis_remap {
	/* src means which source will be mapped to target x, y, z axis */
	/* if an target OS axis is remapped from (-)x,
	 * src is 0, sign_* is (-)1 */
	/* if an target OS axis is remapped from (-)y,
	 * src is 1, sign_* is (-)1 */
	/* if an target OS axis is remapped from (-)z,
	 * src is 2, sign_* is (-)1 */
	int src_x:3;
	int src_y:3;
	int src_z:3;

	int sign_x:2;
	int sign_y:2;
	int sign_z:2;
};

static const struct sensor_axis_remap
mpu6050_accel_axis_remap_tab[MPU6050_AXIS_REMAP_TAB_SZ] = {
	/* src_x src_y src_z  sign_x  sign_y  sign_z */
	{  0,    1,    2,     1,      1,      1 }, /* P0 */
	{  1,    0,    2,     1,     -1,      1 }, /* P1 */
	{  0,    1,    2,    -1,     -1,      1 }, /* P2 */
	{  1,    0,    2,    -1,      1,      1 }, /* P3 */

	{  0,    1,    2,    -1,      1,     -1 }, /* P4 */
	{  1,    0,    2,    -1,     -1,     -1 }, /* P5 */
	{  0,    1,    2,     1,     -1,     -1 }, /* P6 */
	{  1,    0,    2,     1,      1,     -1 }, /* P7 */
};

static const struct sensor_axis_remap
mpu6050_gyro_axis_remap_tab[MPU6050_AXIS_REMAP_TAB_SZ] = {
	/* src_x src_y src_z  sign_x  sign_y  sign_z */
	{  0,    1,    2,    -1,      1,     -1 }, /* P0 */
	{  1,    0,    2,    -1,     -1,     -1 }, /* P1*/
	{  0,    1,    2,     1,     -1,     -1 }, /* P2 */
	{  1,    0,    2,     1,      1,     -1 }, /* P3 */

	{  0,    1,    2,     1,      1,      1 }, /* P4 */
	{  1,    0,    2,     1,     -1,      1 }, /* P5 */
	{  0,    1,    2,    -1,     -1,      1 }, /* P6 */
	{  1,    0,    2,    -1,      1,      1 }, /* P7 */
};

static const struct mpu6050_place_name
mpu6050_place_name2num[MPU6050_AXIS_REMAP_TAB_SZ] = {
	{"Portrait Up", MPU6050_PLACE_PU},
	{"Landscape Right", MPU6050_PLACE_PR},
	{"Portrait Down", MPU6050_PLACE_LD},
	{"Landscape Left", MPU6050_PLACE_LL},
	{"Portrait Up Back Side", MPU6050_PLACE_PU_BACK},
	{"Landscape Right Back Side", MPU6050_PLACE_PR_BACK},
	{"Portrait Down Back Side", MPU6050_PLACE_LD_BACK},
	{"Landscape Left Back Side", MPU6050_PLACE_LL_BACK},
};

/* Function declarations */
static int gyro_poll_thread(void *data);
static int accel_poll_thread(void *data);
static void mpu6050_pinctrl_state(struct mpu6050_sensor *sensor,
			bool active);
static int mpu6050_config_sample_rate(struct mpu6050_sensor *sensor);

static int mpu6050_power_ctl(struct mpu6050_sensor *sensor, bool on)
{
	int rc = 0;
	printk("MPU6050 - power ctl\n");
	if (on && (!sensor->power_enabled)) {
		msleep(POWER_UP_TIME_MS);

		mpu6050_pinctrl_state(sensor, true);

		sensor->power_enabled = true;
	} else if (!on && (sensor->power_enabled)) {
		mpu6050_pinctrl_state(sensor, false);

		sensor->power_enabled = false;
	} else {
		printk("MPU6050 - Ignore power status change from %d to %d\n",
				on, sensor->power_enabled);
	}
	printk("MPU6050 - Power report %d\n", rc);
	return rc;
}

static int mpu6050_power_init(struct mpu6050_sensor *sensor)
{
	printk("MPU6050 - Power init\n");

	return 0;
}

static int mpu6050_power_deinit(struct mpu6050_sensor *sensor)
{
	int ret = 0;
	printk("MPU6050 - power deinit\n");
	return ret;
}

static void mpu6050_remap_accel_data(struct axis_data *data, int place)
{
	const struct sensor_axis_remap *remap;
	s16 tmp[3];
	/* sensor with place 0 needs not to be remapped */
	if ((place <= 0) || (place >= MPU6050_AXIS_REMAP_TAB_SZ))
		return;

	remap = &mpu6050_accel_axis_remap_tab[place];

	tmp[0] = data->x;
	tmp[1] = data->y;
	tmp[2] = data->z;
	data->x = tmp[remap->src_x] * remap->sign_x;
	data->y = tmp[remap->src_y] * remap->sign_y;
	data->z = tmp[remap->src_z] * remap->sign_z;

	return;
}

static void mpu6050_remap_gyro_data(struct axis_data *data, int place)
{
	const struct sensor_axis_remap *remap;
	s16 tmp[3];
	/* sensor with place 0 needs not to be remapped */
	if ((place <= 0) || (place >= MPU6050_AXIS_REMAP_TAB_SZ))
		return;

	remap = &mpu6050_gyro_axis_remap_tab[place];
	tmp[0] = data->rx;
	tmp[1] = data->ry;
	tmp[2] = data->rz;
	data->rx = tmp[remap->src_x] * remap->sign_x;
	data->ry = tmp[remap->src_y] * remap->sign_y;
	data->rz = tmp[remap->src_z] * remap->sign_z;

	return;
}

static int mpu6050_manage_polling(int sns_type, struct mpu6050_sensor *sensor)
{
	ktime_t ktime;
	int ret = 0;

	switch (sns_type) {
	case SNS_TYPE_GYRO:
		if (atomic_read(&sensor->gyro_en)) {
			ktime = ktime_set(0,
				sensor->gyro_poll_ms * NSEC_PER_MSEC);
			ret = hrtimer_start(&sensor->gyro_timer,
					ktime,
					HRTIMER_MODE_REL);
		} else
			ret = hrtimer_try_to_cancel(&sensor->gyro_timer);
		break;

	case SNS_TYPE_ACCEL:
		if (atomic_read(&sensor->accel_en)) {
			ktime = ktime_set(0,
				sensor->accel_poll_ms * NSEC_PER_MSEC);
			ret = hrtimer_start(&sensor->accel_timer,
					ktime,
					HRTIMER_MODE_REL);
		} else
			ret = hrtimer_try_to_cancel(&sensor->accel_timer);
		break;

	default:
		printk("MPU6050 - Invalid sensor type\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

static enum hrtimer_restart gyro_timer_handle(struct hrtimer *hrtimer)
{
	struct mpu6050_sensor *sensor;
	sensor = container_of(hrtimer, struct mpu6050_sensor, gyro_timer);
	sensor->gyro_wkp_flag = 1;
	wake_up_interruptible(&sensor->gyro_wq);
	if (mpu6050_manage_polling(SNS_TYPE_GYRO, sensor) < 0)
		printk("MPU6050 - gyr: failed to start/cancel timer\n");
	return HRTIMER_NORESTART;
}

static enum hrtimer_restart accel_timer_handle(struct hrtimer *hrtimer)
{
	struct mpu6050_sensor *sensor;
	sensor = container_of(hrtimer, struct mpu6050_sensor, accel_timer);
	sensor->accel_wkp_flag = 1;
	wake_up_interruptible(&sensor->accel_wq);
	if (mpu6050_manage_polling(SNS_TYPE_ACCEL, sensor) < 0)
		printk("MPU6050 - acc: failed to start/cancel timer\n");
	return HRTIMER_NORESTART;
}

static int gyro_poll_thread(void *data)
{
	struct mpu6050_sensor *sensor = data;
	ktime_t timestamp;

	while (1) {
		wait_event_interruptible(sensor->gyro_wq,
			((sensor->gyro_wkp_flag != 0) ||
				kthread_should_stop()));
		sensor->gyro_wkp_flag = 0;

		if (kthread_should_stop())
			break;

		mutex_lock(&sensor->op_lock);
		if (sensor->gyro_delay_change) {
			if (sensor->gyro_poll_ms <= POLL_MS_100HZ)
				set_wake_up_idle(true);
			else
				set_wake_up_idle(false);
			sensor->gyro_delay_change = false;
		}
		mutex_unlock(&sensor->op_lock);

		timestamp = ktime_get_boottime();
		mpu6050_remap_gyro_data(&sensor->axis, sensor->pdata->place);
		input_report_abs(sensor->gyro_dev, ABS_RX, sensor->axis.rx);
		input_report_abs(sensor->gyro_dev, ABS_RY, sensor->axis.ry);
		input_report_abs(sensor->gyro_dev, ABS_RZ, sensor->axis.rz);
		input_event(sensor->gyro_dev,
				EV_SYN, SYN_TIME_SEC,
				ktime_to_timespec(timestamp).tv_sec);
		input_event(sensor->gyro_dev, EV_SYN,
			SYN_TIME_NSEC,
			ktime_to_timespec(timestamp).tv_nsec);
		input_sync(sensor->gyro_dev);
	}
	return 0;
}

static int accel_poll_thread(void *data)
{
	struct mpu6050_sensor *sensor = data;
	ktime_t timestamp;

	while (1) {
		wait_event_interruptible(sensor->accel_wq,
			((sensor->accel_wkp_flag != 0) ||
				kthread_should_stop()));
		sensor->accel_wkp_flag = 0;

		if (kthread_should_stop())
			break;

		mutex_lock(&sensor->op_lock);
		if (sensor->accel_delay_change) {
			if (sensor->accel_poll_ms <= POLL_MS_100HZ)
				set_wake_up_idle(true);
			else
				set_wake_up_idle(false);
			sensor->accel_delay_change = false;
		}
		mutex_unlock(&sensor->op_lock);

		timestamp = ktime_get_boottime();
		mpu6050_remap_accel_data(&sensor->axis, sensor->pdata->place);
		input_report_abs(sensor->accel_dev, ABS_X, sensor->axis.x);
		input_report_abs(sensor->accel_dev, ABS_Y, sensor->axis.y);
		input_report_abs(sensor->accel_dev, ABS_Z, sensor->axis.z);
		input_event(sensor->accel_dev,
				EV_SYN, SYN_TIME_SEC,
				ktime_to_timespec(timestamp).tv_sec);
		input_event(sensor->accel_dev, EV_SYN,
			SYN_TIME_NSEC,
			ktime_to_timespec(timestamp).tv_nsec);
		input_sync(sensor->accel_dev);
	}

	return 0;
}

/**
 *  mpu6050_set_lpa_freq() - set low power wakeup frequency.
 */
static int mpu6050_set_lpa_freq(struct mpu6050_sensor *sensor, int lpa_freq)
{
	sensor->cfg.lpa_freq = lpa_freq;
	printk("MPU6050 - set lpa-freq_%d REG_108 READ_63 WRITE_127 \n",sensor->cfg.lpa_freq);

	return 0;
}

static int mpu6050_switch_engine(struct mpu6050_sensor *sensor,
				bool en, u32 mask)
{
	struct mpu_reg_map *reg;
	u8 /* data, */ mgmt_1;
	int ret;
	printk("MPU6050 - switch engine\n");
	ret = 0;
	reg = &sensor->reg;
	/*
	 * switch clock needs to be careful. Only when gyro is on, can
	 * clock source be switched to gyro. Otherwise, it must be set to
	 * internal clock
	 */
	mgmt_1 = MPU_CLK_INTERNAL;
	if (BIT_PWR_GYRO_STBY_MASK == mask) {
		ret = 1;
		mgmt_1 = (u8)ret;
		mgmt_1 &= ~BIT_CLK_MASK;
	}

	if ((BIT_PWR_GYRO_STBY_MASK == mask) && (!en)) {
		mgmt_1 |= MPU_CLK_INTERNAL;
	}

	if ((BIT_PWR_GYRO_STBY_MASK == mask) && en) {
		/* wait gyro stable */
		msleep(SENSOR_UP_TIME_MS);
		/* after gyro is on & stable, switch internal clock to PLL */
		mgmt_1 |= MPU_CLK_PLL_X;
	}

	return 0;
}

static int mpu6050_init_engine(struct mpu6050_sensor *sensor)
{
	int ret;

	ret = mpu6050_switch_engine(sensor, false, BIT_PWR_GYRO_STBY_MASK);
	if (ret)
		return ret;

	ret = mpu6050_switch_engine(sensor, false, BIT_PWR_ACCEL_STBY_MASK);
	if (ret)
		return ret;

	return 0;
}

/**
 * mpu6050_set_power_mode() - set the power mode
 * @sensor: sensor data structure
 * @power_on: value to switch on/off of power, 1: normal power,
 *    0: low power
 *
 * Put device to normal-power mode or low-power mode.
 */
static int mpu6050_set_power_mode(struct mpu6050_sensor *sensor,
					bool power_on)
{
	s32 ret;
	u8 val;
	ret = 1;

	if (power_on)
		val = (u8)ret & ~BIT_SLEEP;
	else
		val = (u8)ret | BIT_SLEEP;

	return 0;
}

static int mpu6050_gyro_enable(struct mpu6050_sensor *sensor, bool on)
{
	int ret;
	u8 data;
	ret = 0;
	if (sensor->cfg.is_asleep) {
		printk("MPU6050 - Fail to set gyro state, device is asleep.\n");
		return -EINVAL;
	}

	ret = 1;
	data = (u8)ret;
	if (on) {
		ret = mpu6050_switch_engine(sensor, true,
			BIT_PWR_GYRO_STBY_MASK);
		if (ret)
			return ret;
		sensor->cfg.gyro_enable = 1;

		data &= ~BIT_SLEEP;

		sensor->cfg.enable = 1;
	} else {
		if (work_pending(&sensor->resume_work))
			cancel_work_sync(&sensor->resume_work);
		ret = mpu6050_switch_engine(sensor, false,
			BIT_PWR_GYRO_STBY_MASK);
		if (ret)
			return ret;
		sensor->cfg.gyro_enable = 0;
		if (!sensor->cfg.accel_enable) {
			data |=  BIT_SLEEP;
			sensor->cfg.enable = 0;
		}
	}
	return 0;
}

/**
 * mpu6050_restore_context() - update the sensor register context
 */

static int mpu6050_restore_context(struct mpu6050_sensor *sensor)
{
	int ret;

	ret = mpu6050_set_lpa_freq(sensor, sensor->cfg.lpa_freq);
	if (ret < 0) {
		printk("MPU6050 - set lpa_freq failed.\n");
		goto exit;
	}

	printk("MPU6050 - restore context finished\n");

exit:
	return ret;
}

/**
 * mpu6050_reset_chip() - reset chip to default state
 */
static void mpu6050_reset_chip(struct mpu6050_sensor *sensor)
{
	int ret, i;
	ret = 1;

	for (i = 0; i < MPU6050_RESET_RETRY_CNT; i++) {
		if ((ret & BIT_H_RESET) == 0) {
			printk("MPU6050 - Chip reset success! i=%d\n", i);
			break;
		}

		usleep(MPU6050_RESET_SLEEP_US);
	}

	return;
}

static int mpu6050_gyro_set_enable(struct mpu6050_sensor *sensor, bool enable)
{
	int ret = 0;

	printk("MPU6050 - mpu6050_gyro_set_enable enable=%d\n", enable);
	mutex_lock(&sensor->op_lock);
	if (enable) {
		if (!sensor->power_enabled) {
			ret = mpu6050_power_ctl(sensor, true);
			if (ret < 0) {
				printk("MPU6050 - Failed to power up mpu6050\n");
				goto exit;
			}
			mpu6050_reset_chip(sensor);
			ret = mpu6050_restore_context(sensor);
			if (ret < 0) {
				printk("MPU6050 - Failed to restore context\n");
				goto exit;
			}
		}

		ret = mpu6050_gyro_enable(sensor, true);
		if (ret) {
			printk("MPU6050 - Fail to enable gyro engine ret=%d\n", ret);
			ret = -EBUSY;
			goto exit;
		}

		ret = mpu6050_config_sample_rate(sensor);
		if (ret < 0)
			printk("MPU6050 - Unable to update sampling rate! ret=%d\n",
				ret);

		if (!sensor->batch_gyro)
		{
			ktime_t ktime;
			ktime = ktime_set(0,
					sensor->gyro_poll_ms * NSEC_PER_MSEC);
			hrtimer_start(&sensor->gyro_timer, ktime,
					HRTIMER_MODE_REL);
		}
		atomic_set(&sensor->gyro_en, 1);
	} else {
		atomic_set(&sensor->gyro_en, 0);
		if (!sensor->batch_gyro)
		{
			ret = hrtimer_try_to_cancel(&sensor->gyro_timer);
		}
		ret = mpu6050_gyro_enable(sensor, false);
		if (ret) {
			printk("MPU6050 - Fail to disable gyro engine ret=%d\n", ret);
			ret = -EBUSY;
			goto exit;
		}
		if (!sensor->cfg.accel_enable && !sensor->cfg.gyro_enable)
			mpu6050_power_ctl(sensor, false);
	}

exit:
	mutex_unlock(&sensor->op_lock);
	return ret;
}

/* Update sensor sample rate divider upon accel and gyro polling rate. */
static int mpu6050_config_sample_rate(struct mpu6050_sensor *sensor)
{
	u32 delay_ms;
	u8 div;
	printk("MPU6050 - sample rate\n");
	if (sensor->cfg.is_asleep)
		return -EINVAL;

	if (sensor->accel_poll_ms <= sensor->gyro_poll_ms)
		delay_ms = sensor->accel_poll_ms;
	else
		delay_ms = sensor->gyro_poll_ms;

	/* Sample_rate = internal_ODR/(1+SMPLRT_DIV) */
	if ((sensor->cfg.lpf != MPU_DLPF_256HZ_NOLPF2) &&
		(sensor->cfg.lpf != MPU_DLPF_RESERVED)) {
		if (delay_ms > DELAY_MS_MAX_DLPF)
			delay_ms = DELAY_MS_MAX_DLPF;
		if (delay_ms < DELAY_MS_MIN_DLPF)
			delay_ms = DELAY_MS_MIN_DLPF;

		div = (u8)(((ODR_DLPF_ENA * delay_ms) / MSEC_PER_SEC) - 1);
	} else {
		if (delay_ms > DELAY_MS_MAX_NODLPF)
			delay_ms = DELAY_MS_MAX_NODLPF;
		if (delay_ms < DELAY_MS_MIN_NODLPF)
			delay_ms = DELAY_MS_MIN_NODLPF;
		div = (u8)(((ODR_DLPF_DIS * delay_ms) / MSEC_PER_SEC) - 1);
	}

	if (sensor->cfg.rate_div == div)
		return 0;
	sensor->cfg.rate_div = div;

	return 0;
}

/*
 * Calculate sample interval according to sample rate.
 * Return sample interval in millisecond.
 */
static inline u64 mpu6050_get_sample_interval(struct mpu6050_sensor *sensor)
{
	u64 interval_ns;
	printk("MPU6050 - get sample interval\n");
	if ((sensor->cfg.lpf == MPU_DLPF_256HZ_NOLPF2) ||
		(sensor->cfg.lpf == MPU_DLPF_RESERVED)) {
		interval_ns = (sensor->cfg.rate_div + 1) * NSEC_PER_MSEC;
		interval_ns /= 8;
	} else {
		interval_ns = (sensor->cfg.rate_div + 1) * NSEC_PER_MSEC;
	}

	return interval_ns;
}

static int mpu6050_gyro_set_poll_delay(struct mpu6050_sensor *sensor,
					unsigned long delay)
{
	int ret = 0;
	printk("MPU6050 - mpu6050_gyro_set_poll_delay delay=%ld\n", delay);
	if (delay < MPU6050_GYRO_MIN_POLL_INTERVAL_MS)
		delay = MPU6050_GYRO_MIN_POLL_INTERVAL_MS;
	if (delay > MPU6050_GYRO_MAX_POLL_INTERVAL_MS)
		delay = MPU6050_GYRO_MAX_POLL_INTERVAL_MS;

	mutex_lock(&sensor->op_lock);
	if (sensor->gyro_poll_ms == delay)
		goto exit;

	sensor->gyro_delay_change = true;
	sensor->gyro_poll_ms = delay;

	if (!atomic_read(&sensor->gyro_en))
		goto exit;

exit:
	mutex_unlock(&sensor->op_lock);
	return ret;
}

static int mpu6050_gyro_cdev_enable(struct sensors_classdev *sensors_cdev,
			unsigned int enable)
{
	struct mpu6050_sensor *sensor = container_of(sensors_cdev,
			struct mpu6050_sensor, gyro_cdev);
	return mpu6050_gyro_set_enable(sensor, enable);
}

static int mpu6050_gyro_cdev_poll_delay(struct sensors_classdev *sensors_cdev,
			unsigned int delay_ms)
{
	struct mpu6050_sensor *sensor = container_of(sensors_cdev,
			struct mpu6050_sensor, gyro_cdev);
	return mpu6050_gyro_set_poll_delay(sensor, delay_ms);
}

static ssize_t mpu6050_get_place(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);

	return snprintf(buf, 30, "%s\n", mpu6050_place_name2num[sensor->pdata->place].name);
}

static ssize_t mpu6050_gyro_attr_get_rx(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);

	return snprintf(buf, 4, "%d\n", sensor->axis.rx);
}

static ssize_t mpu6050_gyro_attr_set_rx(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);
	unsigned long enable;
	int ret;

	if (kstrtoul(buf, 10, &enable))
		return -EINVAL;

	sensor->axis.rx = enable;
	return ret ? -EBUSY : count;
}

static ssize_t mpu6050_gyro_attr_get_ry(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);

	return snprintf(buf, 4, "%d\n", sensor->axis.ry);
}

static ssize_t mpu6050_gyro_attr_set_ry(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);
	unsigned long enable;
	int ret;

	if (kstrtoul(buf, 10, &enable))
		return -EINVAL;

	sensor->axis.ry = enable;
	return ret ? -EBUSY : count;
}

static ssize_t mpu6050_gyro_attr_get_rz(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);

	return snprintf(buf, 4, "%d\n", sensor->axis.rz);
}

static ssize_t mpu6050_gyro_attr_set_rz(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);
	unsigned long enable;
	int ret;

	if (kstrtoul(buf, 10, &enable))
		return -EINVAL;

	sensor->axis.rz = enable;
	return ret ? -EBUSY : count;
}

static struct device_attribute gyro_attr[] = {
	__ATTR(valueX, S_IRUGO | S_IWUSR,
		mpu6050_gyro_attr_get_rx,
		mpu6050_gyro_attr_set_rx),
	__ATTR(valueY, S_IRUGO | S_IWUSR,
		mpu6050_gyro_attr_get_ry,
		mpu6050_gyro_attr_set_ry),
	__ATTR(valueZ, S_IRUGO | S_IWUSR,
		mpu6050_gyro_attr_get_rz,
		mpu6050_gyro_attr_set_rz),
	__ATTR(place, S_IRUSR,
		mpu6050_get_place,
		NULL),
};

static int create_gyro_sysfs_interfaces(struct device *dev)
{
	int i;
	int err;
	for (i = 0; i < ARRAY_SIZE(gyro_attr); i++) {
		err = device_create_file(dev, gyro_attr + i);
		if (err)
			goto error;
	}
	return 0;

error:
	for (; i >= 0; i--)
		device_remove_file(dev, gyro_attr + i);
	dev_err(dev, "Unable to create interface\n");
	return err;
}

static int remove_gyro_sysfs_interfaces(struct device *dev)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(gyro_attr); i++)
		device_remove_file(dev, gyro_attr + i);
	return 0;
}

static int mpu6050_accel_enable(struct mpu6050_sensor *sensor, bool on)
{
	int ret;
	u8 data;

	ret = 0;
	if (sensor->cfg.is_asleep)
		return -EINVAL;

	data = (u8)ret;
	if (on) {
		ret = mpu6050_switch_engine(sensor, true,
			BIT_PWR_ACCEL_STBY_MASK);
		if (ret)
			return ret;
		sensor->cfg.accel_enable = 1;

		data &= ~BIT_SLEEP;

		sensor->cfg.enable = 1;
	} else {
		if (work_pending(&sensor->resume_work))
			cancel_work_sync(&sensor->resume_work);
		ret = mpu6050_switch_engine(sensor, false,
			BIT_PWR_ACCEL_STBY_MASK);
		if (ret)
			return ret;
		sensor->cfg.accel_enable = 0;

		if (!sensor->cfg.gyro_enable) {
			data |=  BIT_SLEEP;
			sensor->cfg.enable = 0;
		}
	}
	return 0;
}

static int mpu6050_accel_set_enable(struct mpu6050_sensor *sensor, bool enable)
{
	int ret = 0;
	printk("MPU6050 - accecl set enable\n");
	printk("MPU6050 - mpu6050_accel_set_enable enable=%d\n", enable);
	if (enable) {
		if (!sensor->power_enabled) {
			ret = mpu6050_power_ctl(sensor, true);
			if (ret < 0) {
				printk("MPU6050 - Failed to set power up mpu6050");
				return ret;
			}
			mpu6050_reset_chip(sensor);
			ret = mpu6050_restore_context(sensor);
			if (ret < 0) {
				printk("MPU6050 - Failed to restore context");
				return ret;
			}
		}

		ret = mpu6050_accel_enable(sensor, true);
		if (ret) {
			printk("MPU6050 - Fail to enable accel engine ret=%d\n", ret);
			ret = -EBUSY;
			return ret;
		}

		ret = mpu6050_config_sample_rate(sensor);
		if (ret < 0)
			printk("MPU6050 - Unable to update sampling rate! ret=%d\n",
				ret);

		if (!sensor->batch_accel) 
		{
			ktime_t ktime;
			ktime = ktime_set(0,
					sensor->accel_poll_ms * NSEC_PER_MSEC);
			hrtimer_start(&sensor->accel_timer, ktime,
					HRTIMER_MODE_REL);
		}
		atomic_set(&sensor->accel_en, 1);
	} else {
		atomic_set(&sensor->accel_en, 0);
		if (!sensor->batch_accel)
		{
			ret = hrtimer_try_to_cancel(&sensor->accel_timer);
		}

		ret = mpu6050_accel_enable(sensor, false);
		if (ret) {
			printk("MPU6050 - Fail to disable accel engine ret=%d\n", ret);
			ret = -EBUSY;
			return ret;
		}
		if (!sensor->cfg.accel_enable && !sensor->cfg.gyro_enable)
			mpu6050_power_ctl(sensor, false);
	}

	return ret;
}

static int mpu6050_accel_set_poll_delay(struct mpu6050_sensor *sensor,
					unsigned long delay)
{
	int ret;

	printk("MPU6050 - mpu6050_accel_set_poll_delay delay_ms=%ld\n", delay);
	if (delay < MPU6050_ACCEL_MIN_POLL_INTERVAL_MS)
		delay = MPU6050_ACCEL_MIN_POLL_INTERVAL_MS;
	if (delay > MPU6050_ACCEL_MAX_POLL_INTERVAL_MS)
		delay = MPU6050_ACCEL_MAX_POLL_INTERVAL_MS;

	mutex_lock(&sensor->op_lock);
	if (sensor->accel_poll_ms == delay)
		goto exit;

	sensor->accel_delay_change = true;
	sensor->accel_poll_ms = delay;

	if (!atomic_read(&sensor->accel_en))
		goto exit;

	if (sensor->use_poll) {
		ktime_t ktime;
		ret = hrtimer_try_to_cancel(&sensor->accel_timer);
		ktime = ktime_set(0,
				sensor->accel_poll_ms * NSEC_PER_MSEC);
		hrtimer_start(&sensor->accel_timer, ktime, HRTIMER_MODE_REL);
	} else {
		ret = mpu6050_config_sample_rate(sensor);
		if (ret < 0)
			printk("MPU6050 - Unable to set polling delay for accel!\n");
	}

exit:
	mutex_unlock(&sensor->op_lock);
	return ret;
}

static int mpu6050_accel_cdev_enable(struct sensors_classdev *sensors_cdev,
			unsigned int enable)
{
	struct mpu6050_sensor *sensor = container_of(sensors_cdev,
			struct mpu6050_sensor, accel_cdev);
	int err;

	mutex_lock(&sensor->op_lock);

	err = mpu6050_accel_set_enable(sensor, enable);

	mutex_unlock(&sensor->op_lock);

	return err;
}

static int mpu6050_accel_cdev_poll_delay(struct sensors_classdev *sensors_cdev,
			unsigned int delay_ms)
{
	struct mpu6050_sensor *sensor = container_of(sensors_cdev,
			struct mpu6050_sensor, accel_cdev);

	return mpu6050_accel_set_poll_delay(sensor, delay_ms);
}

static ssize_t mpu6050_accel_attr_get_x(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);

	return snprintf(buf, 4, "%d\n", sensor->axis.x);
}

static ssize_t mpu6050_accel_attr_set_x(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);
	unsigned long enable;
	int ret;

	if (kstrtoul(buf, 10, &enable))
		return -EINVAL;

	sensor->axis.x = enable;
	return ret ? -EBUSY : count;
}

static ssize_t mpu6050_accel_attr_get_y(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);

	return snprintf(buf, 4, "%d\n", sensor->axis.y);
}

static ssize_t mpu6050_accel_attr_set_y(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);
	unsigned long enable;
	int ret;

	if (kstrtoul(buf, 10, &enable))
		return -EINVAL;

	sensor->axis.y = enable;
	return ret ? -EBUSY : count;
}

static ssize_t mpu6050_accel_attr_get_z(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);

	return snprintf(buf, 4, "%d\n", sensor->axis.z);
}

static ssize_t mpu6050_accel_attr_set_z(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct mpu6050_sensor *sensor = dev_get_drvdata(dev);
	unsigned long enable;
	int ret;

	if (kstrtoul(buf, 10, &enable))
		return -EINVAL;

	sensor->axis.z = enable;
	return ret ? -EBUSY : count;
}

static struct device_attribute accel_attr[] = {
	__ATTR(valueX, S_IRUGO | S_IWUSR,
		mpu6050_accel_attr_get_x,
		mpu6050_accel_attr_set_x),
	__ATTR(valueY, S_IRUGO | S_IWUSR,
		mpu6050_accel_attr_get_y,
		mpu6050_accel_attr_set_y),
	__ATTR(valueZ, S_IRUGO | S_IWUSR,
		mpu6050_accel_attr_get_z,
		mpu6050_accel_attr_set_z),
	__ATTR(place, S_IRUSR,
		mpu6050_get_place,
		NULL),
};

static int create_accel_sysfs_interfaces(struct device *dev)
{
	int i;
	int err;
	for (i = 0; i < ARRAY_SIZE(accel_attr); i++) {
		err = device_create_file(dev, accel_attr + i);
		if (err)
			goto error;
	}
	return 0;

error:
	for (; i >= 0; i--)
		device_remove_file(dev, accel_attr + i);
	dev_err(dev, "Unable to create interface\n");
	return err;
}

static int remove_accel_sysfs_interfaces(struct device *dev)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(accel_attr); i++)
		device_remove_file(dev, accel_attr + i);
	return 0;
}

static void setup_mpu6050_reg(struct mpu_reg_map *reg)
{
	reg->sample_rate_div	= REG_SAMPLE_RATE_DIV;
	reg->lpf		= REG_CONFIG;
	reg->fifo_en		= REG_FIFO_EN;
	reg->gyro_config	= REG_GYRO_CONFIG;
	reg->accel_config	= REG_ACCEL_CONFIG;
	reg->mot_thr		= REG_ACCEL_MOT_THR;
	reg->mot_dur		= REG_ACCEL_MOT_DUR;
	reg->fifo_count_h	= REG_FIFO_COUNT_H;
	reg->fifo_r_w		= REG_FIFO_R_W;
	reg->raw_gyro		= REG_RAW_GYRO;
	reg->raw_accel		= REG_RAW_ACCEL;
	reg->temperature	= REG_TEMPERATURE;
	reg->int_pin_cfg	= REG_INT_PIN_CFG;
	reg->int_enable		= REG_INT_ENABLE;
	reg->int_status		= REG_INT_STATUS;
	reg->user_ctrl		= REG_USER_CTRL;
	reg->pwr_mgmt_1		= REG_PWR_MGMT_1;
	reg->pwr_mgmt_2		= REG_PWR_MGMT_2;
};

/**
 * mpu_check_chip_type() - check and setup chip type.
 */
static int mpu_check_chip_type(struct mpu6050_sensor *sensor)
{
	struct mpu_reg_map *reg;
	s32 ret;

	sensor->chip_type = INV_MPU6050;

	reg = &sensor->reg;
	setup_mpu6050_reg(reg);

	/* turn off and turn on power to ensure gyro engine is on */

	ret = mpu6050_set_power_mode(sensor, false);
	if (ret)
		return ret;
	ret = mpu6050_set_power_mode(sensor, true);
	if (ret)
		return ret;

	sensor->chip_type = INV_MPU6050;

	printk("mpu6050 - check chip type INV_MPU6050\n");

	return 0;
}

/**
 *  mpu6050_init_config() - Initialize hardware, disable FIFO.
 *  @indio_dev:	Device driver instance.
 *  Initial configuration:
 *  FSR: +/- 2000DPS
 *  DLPF: 42Hz
 *  FIFO rate: 50Hz
 *  AFS: 2G
 */
static int mpu6050_init_config(struct mpu6050_sensor *sensor)
{
	u8 data;
	
	printk("MPU6050 - init config\n");

	if (sensor->cfg.is_asleep)
		return -EINVAL;

	mpu6050_reset_chip(sensor);

	memset(&sensor->cfg, 0, sizeof(struct mpu_chip_config));

	sensor->cfg.fsr = MPU_FSR_2000DPS;
	sensor->cfg.lpf = MPU_DLPF_42HZ;

	data = (u8)(ODR_DLPF_ENA / INIT_FIFO_RATE - 1);

	sensor->cfg.rate_div = data;
	sensor->cfg.accel_fs = ACCEL_FS_02G;

	if ((sensor->pdata->int_flags & IRQF_TRIGGER_FALLING) ||
		(sensor->pdata->int_flags & IRQF_TRIGGER_LOW))
		data = BIT_INT_CFG_DEFAULT | BIT_INT_ACTIVE_LOW;
	else
		data = BIT_INT_CFG_DEFAULT;

	sensor->cfg.int_pin_cfg = data;
	sensor->cfg.gyro_enable = 0;
	sensor->cfg.gyro_fifo_enable = 0;
	sensor->cfg.accel_enable = 0;
	sensor->cfg.accel_fifo_enable = 0;

	return 0;
}

static void mpu6050_pinctrl_state(struct mpu6050_sensor *sensor,
			bool active)
{
	printk("MPU6050 - Pinctrl state\n");

	return;
}

#ifdef CONFIG_OF
static int mpu6050_dt_get_place(struct device *dev,
			struct mpu6050_platform_data *pdata)
{
	const char *place_name;
	int rc;
	int i;
	printk("MPU6050 - get place\n");
	rc = of_property_read_string(dev->of_node, "invn,place", &place_name);
	if (rc) {
		printk("MPU6050 - Cannot get place configuration!\n");
		return -EINVAL;
	}

	for (i = 0; i < MPU6050_AXIS_REMAP_TAB_SZ; i++) {
		if (!strcmp(place_name, mpu6050_place_name2num[i].name)) {
			pdata->place = mpu6050_place_name2num[i].place;
			break;
		}
	}
	if (i >= MPU6050_AXIS_REMAP_TAB_SZ) {
		printk("MPU6050 - Invalid place parameter, use default value 0\n");
		pdata->place = 0;
	}

	return 0;
}

static int mpu6050_parse_dt(struct device *dev,
			struct mpu6050_platform_data *pdata)
{
	int rc;

	rc = mpu6050_dt_get_place(dev, pdata);
	if (rc)
		return rc;

	/* check gpio_int later, use polling if gpio_int is invalid. */
	pdata->gpio_int = of_get_named_gpio_flags(dev->of_node,
				"invn,gpio-int", 0, &pdata->int_flags);

	pdata->gpio_en = of_get_named_gpio_flags(dev->of_node,
				"invn,gpio-en", 0, NULL);

	pdata->use_int = of_property_read_bool(dev->of_node,
				"invn,use-interrupt");

	return 0;
}
#else
static int mpu6050_parse_dt(struct device *dev,
			struct mpu6050_platform_data *pdata)
{
	return -EINVAL;
}
#endif

/**
 * mpu6050_probe() - device detection callback
 * @client: i2c client of found device
 * @id: id match information
 *
 * The I2C layer calls us when it believes a sensor is present at this
 * address. Probe to see if this is correct and to validate the device.
 *
 * If present install the relevant sysfs interfaces and input device.
 */
static int mpu6050_probe(struct i2c_client *client,const struct i2c_device_id *id)
{
	struct mpu6050_sensor *sensor;
	struct mpu6050_platform_data *pdata;
	int ret;

	sensor = devm_kzalloc(&client->dev, sizeof(struct mpu6050_sensor),
			GFP_KERNEL);
	if (!sensor) {
		printk("MPU6050 - Failed to allocate driver data\n");
		return -ENOMEM;
	}

	sensor->axis.x = 0;
	sensor->axis.y = 0;
	sensor->axis.z = 0;
	sensor->axis.rx = 0;
	sensor->axis.ry = 0;
	sensor->axis.rz = 0;
	
	sensor->client = client;
	sensor->dev = &client->dev;
	i2c_set_clientdata(client, sensor);

	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct mpu6050_platform_data), GFP_KERNEL);
		if (!pdata) {
			printk("MPU6050 - Failed to allcated memory\n");
			ret = -ENOMEM;
			goto err_free_devmem;
		}
		ret = mpu6050_parse_dt(&client->dev, pdata);
		if (ret) {
			printk("MPU6050 - Failed to parse device tree\n");
			ret = -EINVAL;
			goto err_free_devmem;
		}
		printk("MPU6050 - use interrupt is %d\n", pdata->use_int);
		printk("MPU6050 - interrupt flags is %d\n", pdata->int_flags);
	} else {
		printk("MPU6050 - use platform\n");
		pdata = client->dev.platform_data;
	}

	if (!pdata) {
		printk("MPU6050 - Cannot get device platform data\n");
		ret = -EINVAL;
		goto err_free_devmem;
	}

	mutex_init(&sensor->op_lock);
	sensor->pdata = pdata;
	sensor->enable_gpio = sensor->pdata->gpio_en;

	ret = mpu6050_power_init(sensor);
	if (ret) {
		printk("MPU6050 - Failed to init regulator\n");
		goto err_free_enable_gpio;
	}
	ret = mpu6050_power_ctl(sensor, true);
	if (ret) {
		printk("MPU6050 - Failed to power on device\n");
		goto err_deinit_regulator;
	}

	ret = mpu_check_chip_type(sensor);
	if (ret) {
		printk("MPU6050 - Cannot get invalid chip type\n");
		goto err_power_off_device;
	}

	ret = mpu6050_init_engine(sensor);
	if (ret) {
		printk("MPU6050 - Failed to init chip engine\n");
		goto err_power_off_device;
	}

	ret = mpu6050_set_lpa_freq(sensor, MPU6050_LPA_5HZ);
	if (ret) {
		printk("MPU6050 - Failed to set lpa frequency\n");
		goto err_power_off_device;
	}

	sensor->cfg.is_asleep = false;
	atomic_set(&sensor->accel_en, 0);
	atomic_set(&sensor->gyro_en, 0);
	ret = mpu6050_init_config(sensor);
	if (ret) {
		printk("MPU6050 - Failed to set default config\n");
		goto err_power_off_device;
	}

	sensor->accel_dev = devm_input_allocate_device(&client->dev);
	if (!sensor->accel_dev) {
		printk("MPU6050 - Failed to allocate accelerometer input device\n");
		ret = -ENOMEM;
		goto err_power_off_device;
	}

	sensor->gyro_dev = devm_input_allocate_device(&client->dev);
	if (!sensor->gyro_dev) {
		printk("MPU6050 - Failed to allocate gyroscope input device\n");
		ret = -ENOMEM;
		goto err_power_off_device;
	}

	sensor->accel_dev->name = MPU6050_DEV_NAME_ACCEL;
	sensor->gyro_dev->name = MPU6050_DEV_NAME_GYRO;
	sensor->accel_dev->id.bustype = BUS_I2C;
	sensor->gyro_dev->id.bustype = BUS_I2C;
	sensor->accel_poll_ms = MPU6050_ACCEL_DEFAULT_POLL_INTERVAL_MS;
	sensor->gyro_poll_ms = MPU6050_GYRO_DEFAULT_POLL_INTERVAL_MS;
	sensor->acc_use_cal = false;

	input_set_capability(sensor->accel_dev, EV_ABS, ABS_MISC);
	input_set_capability(sensor->gyro_dev, EV_ABS, ABS_MISC);
	input_set_abs_params(sensor->accel_dev, ABS_X,
			MPU6050_ACCEL_MIN_VALUE, MPU6050_ACCEL_MAX_VALUE,
			0, 0);
	input_set_abs_params(sensor->accel_dev, ABS_Y,
			MPU6050_ACCEL_MIN_VALUE, MPU6050_ACCEL_MAX_VALUE,
			0, 0);
	input_set_abs_params(sensor->accel_dev, ABS_Z,
			MPU6050_ACCEL_MIN_VALUE, MPU6050_ACCEL_MAX_VALUE,
			0, 0);
	input_set_abs_params(sensor->gyro_dev, ABS_RX,
			     MPU6050_GYRO_MIN_VALUE, MPU6050_GYRO_MAX_VALUE,
			     0, 0);
	input_set_abs_params(sensor->gyro_dev, ABS_RY,
			     MPU6050_GYRO_MIN_VALUE, MPU6050_GYRO_MAX_VALUE,
			     0, 0);
	input_set_abs_params(sensor->gyro_dev, ABS_RZ,
			     MPU6050_GYRO_MIN_VALUE, MPU6050_GYRO_MAX_VALUE,
			     0, 0);
	sensor->accel_dev->dev.parent = &client->dev;
	sensor->gyro_dev->dev.parent = &client->dev;
	input_set_drvdata(sensor->accel_dev, sensor);
	input_set_drvdata(sensor->gyro_dev, sensor);

	sensor->use_poll = 1;
	printk("MPU6050 - Polling mode is enabled. use_int=%d gpio_int=%d",
		sensor->pdata->use_int, sensor->pdata->gpio_int);

	sensor->data_wq = create_freezable_workqueue("mpu6050_data_work");
	if (!sensor->data_wq) {
		printk("MPU6050 - Cannot create workqueue!\n");
		goto err_free_gpio;
	}

	hrtimer_init(&sensor->gyro_timer, CLOCK_BOOTTIME, HRTIMER_MODE_REL);
	sensor->gyro_timer.function = gyro_timer_handle;
	hrtimer_init(&sensor->accel_timer, CLOCK_BOOTTIME, HRTIMER_MODE_REL);
	sensor->accel_timer.function = accel_timer_handle;

	init_waitqueue_head(&sensor->gyro_wq);
	init_waitqueue_head(&sensor->accel_wq);
	sensor->gyro_wkp_flag = 0;
	sensor->accel_wkp_flag = 0;

	sensor->gyr_task = kthread_run(gyro_poll_thread, sensor, "sns_gyro");
	sensor->accel_task = kthread_run(accel_poll_thread, sensor,
						"sns_accel");

	ret = input_register_device(sensor->accel_dev);
	if (ret) {
		printk("MPU6050 - Failed to register input device\n");
		goto err_destroy_workqueue;
	}
	ret = input_register_device(sensor->gyro_dev);
	if (ret) {
		printk("MPU6050 - Failed to register input device\n");
		goto err_destroy_workqueue;
	}
	ret = create_accel_sysfs_interfaces(&sensor->accel_dev->dev);
	if (ret < 0) {
		dev_err(&client->dev, "failed to create sysfs for accel\n");
		goto err_destroy_workqueue;
	}
	ret = create_gyro_sysfs_interfaces(&sensor->gyro_dev->dev);
	if (ret < 0) {
		dev_err(&client->dev, "failed to create sysfs for gyro\n");
		goto err_remove_accel_sysfs;
	}

	sensor->accel_cdev = mpu6050_acc_cdev;
	sensor->accel_cdev.delay_msec = sensor->accel_poll_ms;
	sensor->accel_cdev.sensors_enable = mpu6050_accel_cdev_enable;
	sensor->accel_cdev.sensors_poll_delay = mpu6050_accel_cdev_poll_delay;
	sensor->accel_cdev.fifo_reserved_event_count = 0;

	ret = sensors_classdev_register(&sensor->accel_dev->dev,
			&sensor->accel_cdev);
	if (ret) {
		printk("MPU6050 - create accel class device file failed!\n");
		ret = -EINVAL;
		goto err_remove_gyro_sysfs;
	}

	sensor->gyro_cdev = mpu6050_gyro_cdev;
	sensor->gyro_cdev.delay_msec = sensor->gyro_poll_ms;
	sensor->gyro_cdev.sensors_enable = mpu6050_gyro_cdev_enable;
	sensor->gyro_cdev.sensors_poll_delay = mpu6050_gyro_cdev_poll_delay;
	sensor->gyro_cdev.fifo_reserved_event_count = 0;

	ret = sensors_classdev_register(&sensor->gyro_dev->dev,
			&sensor->gyro_cdev);
	if (ret) {
		printk("MPU6050 - create accel class device file failed!\n");
		ret = -EINVAL;
		goto err_remove_accel_cdev;
	}

	ret = mpu6050_power_ctl(sensor, false);
	if (ret) {
		printk("MPU6050 - Power off mpu6050 failed\n");
		goto err_remove_gyro_cdev;
	}

	return 0;
err_remove_gyro_cdev:
	sensors_classdev_unregister(&sensor->gyro_cdev);
err_remove_accel_cdev:
	 sensors_classdev_unregister(&sensor->accel_cdev);
err_remove_gyro_sysfs:
	remove_accel_sysfs_interfaces(&sensor->gyro_dev->dev);
err_remove_accel_sysfs:
	remove_accel_sysfs_interfaces(&sensor->accel_dev->dev);
err_destroy_workqueue:
	destroy_workqueue(sensor->data_wq);
	hrtimer_try_to_cancel(&sensor->gyro_timer);
	hrtimer_try_to_cancel(&sensor->accel_timer);
	kthread_stop(sensor->gyr_task);
	kthread_stop(sensor->accel_task);
err_free_gpio:
err_power_off_device:
	mpu6050_power_ctl(sensor, false);
err_deinit_regulator:
	mpu6050_power_deinit(sensor);
err_free_enable_gpio:
err_free_devmem:
	devm_kfree(&client->dev, sensor);
	printk("MPU6050 - Probe device return error%d\n", ret);
	return ret;
}

/**
 * mpu6050_remove() - remove a sensor
 * @client: i2c client of sensor being removed
 *
 * Our sensor is going away, clean up the resources.
 */
static int mpu6050_remove(struct i2c_client *client)
{
	struct mpu6050_sensor *sensor = i2c_get_clientdata(client);

	sensors_classdev_unregister(&sensor->accel_cdev);
	sensors_classdev_unregister(&sensor->gyro_cdev);
	remove_gyro_sysfs_interfaces(&sensor->gyro_dev->dev);
	remove_accel_sysfs_interfaces(&sensor->accel_dev->dev);
	destroy_workqueue(sensor->data_wq);
	hrtimer_try_to_cancel(&sensor->gyro_timer);
	hrtimer_try_to_cancel(&sensor->accel_timer);
	kthread_stop(sensor->gyr_task);
	kthread_stop(sensor->accel_task);
	mpu6050_power_ctl(sensor, false);
	mpu6050_power_deinit(sensor);
	devm_kfree(&client->dev, sensor);

	return 0;
}

static const struct i2c_device_id mpu6050_ids[] = {
	{ "mpu6050", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mpu6050_ids);

static const struct of_device_id mpu6050_of_match[] = {
	{ .compatible = "invn,fake6050", },
	{ },
};
MODULE_DEVICE_TABLE(of, mpu6050_of_match);

static struct i2c_driver mpu6050_i2c_driver = {
	.driver	= {
		.name	= "mpu6050",
		.owner	= THIS_MODULE,
		.of_match_table = mpu6050_of_match,
	},
	.probe		= mpu6050_probe,
	.remove		= mpu6050_remove,
	.id_table	= mpu6050_ids,
};

module_i2c_driver(mpu6050_i2c_driver);

MODULE_DESCRIPTION("MPU6050 Tri-axis gyroscope driver");
MODULE_LICENSE("GPL v2");

/* &i2c_1 {
	mpu6050@68 {
		compatible = "invn,mpu6050";
		reg = <0x68>;
		invn,place = "Portrait Down";
	};
}; */
