# Fake driver for sensor Accelerometer - MPU6050

<b>Direction:</b> {AOSP}/kernel/driver/input/misc/

1. Device tree

```
&i2c_1 {
	fake6050@68 {
		compatible = "invn,fake6050";
		reg = <0x68>;
		invn,place = "Portrait Down";
	};
};
```

2. Kconfig

```
config SENSORS_FAKE6050
	tristate "FAKE6050 6-axix gyroscope + acceleromater combo"
	help
	  Say Y here if you want to support InvenSense FAKE6050
	  connected via an I2C bus.

	  To compile this driver as a module, choose M here: the
	  module will be called fake6050.
```

3. Makefile

```
obj-$(CONFIG_SENSORS_FAKE6050)		+= fake6050.o
```

4. Defconfig

```
#10. fake sensor mpu6050 - combo accel and gyro
CONFIG_SENSORS_FAKE6050=y
```
