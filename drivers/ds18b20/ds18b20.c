// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO bit-banged DS18B20 driver.
 *
 * The device node returns one native IEEE-754 double for every read(2).
 * The line is released (input mode) for the bus high level; an external
 * pull-up is required, as specified by the 1-Wire electrical interface.
 */

#include <linux/cdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#define DS18B20_CLASS_NAME          "ds18b20"
#define DS18B20_DEVICE_NAME        "ds18b20"
#define DS18B20_SCRATCHPAD_SIZE    9
#define DS18B20_CONVERSION_TIME_MS 750

struct ds18b20 {
	struct device *dev;
	struct gpio_desc *data;
	struct mutex lock;
	struct cdev cdev;
	dev_t devt;
	struct device *char_device;
};

static struct class *ds18b20_class;
static DEFINE_MUTEX(ds18b20_class_lock);
static unsigned int ds18b20_users;

static int ds18b20_release_line(struct ds18b20 *sensor)
{
	/* Input mode is the open-drain release state. */
	return gpiod_direction_input(sensor->data);
}

static int ds18b20_drive_low(struct ds18b20 *sensor)
{
	return gpiod_direction_output(sensor->data, 0);
}

static int ds18b20_read_level(struct ds18b20 *sensor)
{
	return gpiod_get_value_cansleep(sensor->data);
}

static int ds18b20_reset(struct ds18b20 *sensor)
{
	int level;
	int ret;

	ret = ds18b20_drive_low(sensor);
	if (ret)
		return ret;
	udelay(480);

	ret = ds18b20_release_line(sensor);
	if (ret)
		return ret;
	udelay(70);
	level = ds18b20_read_level(sensor);
	udelay(410);

	if (level < 0)
		return level;
	return level ? -ENODEV : 0;
}

static int ds18b20_write_bit(struct ds18b20 *sensor, bool bit)
{
	int ret;

	ret = ds18b20_drive_low(sensor);
	if (ret)
		return ret;

	if (bit) {
		udelay(6);
		ret = ds18b20_release_line(sensor);
		if (ret)
			return ret;
		udelay(64);
	} else {
		udelay(60);
		ret = ds18b20_release_line(sensor);
		if (ret)
			return ret;
		udelay(10);
	}

	return 0;
}

static int ds18b20_read_bit(struct ds18b20 *sensor, bool *bit)
{
	int ret;
	int level;

	ret = ds18b20_drive_low(sensor);
	if (ret)
		return ret;
	udelay(3);
	ret = ds18b20_release_line(sensor);
	if (ret)
		return ret;
	udelay(10);

	level = ds18b20_read_level(sensor);
	if (level < 0)
		return level;
	*bit = level != 0;

	udelay(53);
	return 0;
}

static int ds18b20_write_byte(struct ds18b20 *sensor, u8 value)
{
	unsigned int bit;
	int ret;

	for (bit = 0; bit < 8; ++bit) {
		ret = ds18b20_write_bit(sensor, value & BIT(bit));
		if (ret)
			return ret;
	}
	return 0;
}

static int ds18b20_read_byte(struct ds18b20 *sensor, u8 *value)
{
	unsigned int bit;
	bool sample;
	int ret;

	*value = 0;
	for (bit = 0; bit < 8; ++bit) {
		ret = ds18b20_read_bit(sensor, &sample);
		if (ret)
			return ret;
		if (sample)
			*value |= BIT(bit);
	}
	return 0;
}

static u8 ds18b20_crc8(const u8 *data, size_t length)
{
	u8 crc = 0;
	size_t index;

	for (index = 0; index < length; ++index) {
		u8 byte = data[index];
		unsigned int bit;

		for (bit = 0; bit < 8; ++bit) {
			u8 mix = (crc ^ byte) & 0x01;
			crc >>= 1;
			if (mix)
				crc ^= 0x8c;
			byte >>= 1;
		}
	}
	return crc;
}

static int ds18b20_read_temperature(struct ds18b20 *sensor, u64 *temperature_bits)
{
	u8 scratchpad[DS18B20_SCRATCHPAD_SIZE];
	s16 raw;
	u32 magnitude;
	u32 exponent;
	u32 highest_bit;
	u64 significand;
	u8 calculated_crc;
	unsigned int index;
	int ret;

	ret = ds18b20_reset(sensor);
	if (ret)
		return ret;
	ret = ds18b20_write_byte(sensor, 0xcc); /* Skip ROM: one device on bus. */
	if (ret)
		return ret;
	ret = ds18b20_write_byte(sensor, 0x44); /* Convert T. */
	if (ret)
		return ret;

	/* 750 ms is the worst-case conversion time at 12-bit resolution. */
	msleep(DS18B20_CONVERSION_TIME_MS);

	ret = ds18b20_reset(sensor);
	if (ret)
		return ret;
	ret = ds18b20_write_byte(sensor, 0xcc);
	if (ret)
		return ret;
	ret = ds18b20_write_byte(sensor, 0xbe); /* Read scratchpad. */
	if (ret)
		return ret;

	for (index = 0; index < DS18B20_SCRATCHPAD_SIZE; ++index) {
		ret = ds18b20_read_byte(sensor, &scratchpad[index]);
		if (ret)
			return ret;
	}

	calculated_crc = ds18b20_crc8(scratchpad,
		DS18B20_SCRATCHPAD_SIZE - 1);
	if (calculated_crc != scratchpad[DS18B20_SCRATCHPAD_SIZE - 1]) {
		dev_err_ratelimited(sensor->dev,
			"scratchpad CRC mismatch: "
			"%02x %02x %02x %02x %02x %02x %02x %02x %02x "
			"(calculated=%02x received=%02x)\n",
			scratchpad[0], scratchpad[1], scratchpad[2],
			scratchpad[3], scratchpad[4], scratchpad[5],
			scratchpad[6], scratchpad[7], scratchpad[8],
			calculated_crc, scratchpad[DS18B20_SCRATCHPAD_SIZE - 1]);
		return -EIO;
	}

	raw = (s16)((u16)scratchpad[1] << 8 | scratchpad[0]);
	/*
	 * Kernel code must not use floating-point instructions on arm64. Build
	 * the exact IEEE-754 representation of raw / 16 in integer arithmetic;
	 * the user-space read ABI is still one native little-endian double.
	 */
	if (raw == 0) {
		*temperature_bits = 0;
		return 0;
	}
	magnitude = raw < 0 ? (u32)(-(s32)raw) : (u32)raw;
	highest_bit = fls(magnitude) - 1;
	exponent = highest_bit + 1023 - 4;
	significand = (u64)magnitude << (52 - highest_bit);
	*temperature_bits = ((u64)(raw < 0) << 63) |
		((u64)exponent << 52) |
		(significand & ((1ULL << 52) - 1));
	return 0;
}

static int ds18b20_open(struct inode *inode, struct file *file)
{
	struct ds18b20 *sensor = container_of(inode->i_cdev, struct ds18b20, cdev);

	file->private_data = sensor;
	return 0;
}

static ssize_t ds18b20_read(struct file *file, char __user *buffer,
			    size_t count, loff_t *position)
{
	struct ds18b20 *sensor = file->private_data;
	u64 temperature_bits;
	int ret;

	if (*position != 0)
		return 0;
	if (count < sizeof(temperature_bits))
		return -EINVAL;
	if (mutex_lock_interruptible(&sensor->lock))
		return -ERESTARTSYS;

	ret = ds18b20_read_temperature(sensor, &temperature_bits);
	if (!ret && copy_to_user(buffer, &temperature_bits, sizeof(temperature_bits)))
		ret = -EFAULT;
	mutex_unlock(&sensor->lock);
	if (ret)
		return ret;

	*position = sizeof(temperature_bits);
	return sizeof(temperature_bits);
}

static const struct file_operations ds18b20_fops = {
	.owner = THIS_MODULE,
	.open = ds18b20_open,
	.read = ds18b20_read,
	.llseek = no_llseek,
};

static int ds18b20_probe(struct platform_device *pdev)
{
	struct ds18b20 *sensor;
	int ret;

	sensor = devm_kzalloc(&pdev->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = &pdev->dev;
	mutex_init(&sensor->lock);
	sensor->data = devm_gpiod_get(&pdev->dev, "ds18b20", GPIOD_IN);
	if (IS_ERR(sensor->data)) {
		ret = PTR_ERR(sensor->data);
		dev_err(&pdev->dev, "failed to acquire ds18b20-gpios: %d\n", ret);
		return ret;
	}

	ret = alloc_chrdev_region(&sensor->devt, 0, 1, DS18B20_DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&sensor->cdev, &ds18b20_fops);
	sensor->cdev.owner = THIS_MODULE;
	ret = cdev_add(&sensor->cdev, sensor->devt, 1);
	if (ret)
		goto unregister_chrdev;

	mutex_lock(&ds18b20_class_lock);
	if (ds18b20_users++ == 0) {
		ds18b20_class = class_create(THIS_MODULE, DS18B20_CLASS_NAME);
		if (IS_ERR(ds18b20_class)) {
			ret = PTR_ERR(ds18b20_class);
			ds18b20_class = NULL;
			ds18b20_users--;
		}
	}
	mutex_unlock(&ds18b20_class_lock);
	if (ret)
		goto del_cdev;

	sensor->char_device = device_create(ds18b20_class, &pdev->dev,
					    sensor->devt, sensor,
					    DS18B20_DEVICE_NAME);
	if (IS_ERR(sensor->char_device)) {
			ret = PTR_ERR(sensor->char_device);
			goto destroy_class_ref;
	}

	platform_set_drvdata(pdev, sensor);
	dev_info(&pdev->dev, "registered /dev/%s (binary double read)\n",
		 DS18B20_DEVICE_NAME);
	return 0;

destroy_class_ref:
	mutex_lock(&ds18b20_class_lock);
	if (--ds18b20_users == 0) {
		class_destroy(ds18b20_class);
		ds18b20_class = NULL;
	}
	mutex_unlock(&ds18b20_class_lock);
del_cdev:
	cdev_del(&sensor->cdev);
unregister_chrdev:
	unregister_chrdev_region(sensor->devt, 1);
	return ret;
}

static int ds18b20_remove(struct platform_device *pdev)
{
	struct ds18b20 *sensor = platform_get_drvdata(pdev);

	device_destroy(ds18b20_class, sensor->devt);
	cdev_del(&sensor->cdev);
	unregister_chrdev_region(sensor->devt, 1);

	mutex_lock(&ds18b20_class_lock);
	if (--ds18b20_users == 0) {
		class_destroy(ds18b20_class);
		ds18b20_class = NULL;
	}
	mutex_unlock(&ds18b20_class_lock);
	return 0;
}

static const struct of_device_id ds18b20_of_match[] = {
	{ .compatible = "ds-temperature-collector" },
	{ }
};
MODULE_DEVICE_TABLE(of, ds18b20_of_match);

static struct platform_driver ds18b20_driver = {
	.probe = ds18b20_probe,
	.remove = ds18b20_remove,
	.driver = {
		.name = DS18B20_DEVICE_NAME,
		.of_match_table = ds18b20_of_match,
	},
};
module_platform_driver(ds18b20_driver);

MODULE_AUTHOR("rkGateway");
MODULE_DESCRIPTION("GPIO bit-banged DS18B20 character device driver");
MODULE_LICENSE("GPL");
