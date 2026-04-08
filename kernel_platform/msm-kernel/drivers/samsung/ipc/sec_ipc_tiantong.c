#include <linux/platform_device.h>
#include <linux/refcount.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/idr.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/termios.h>
#include <linux/string.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>

#include "sec_ipc_tiantong.h"

static struct class *tiantong_class;
static struct cdev tiantong_cdev;
static dev_t dev_num;
static struct tiantong_regulator tt_reg;
static struct tiantong_gpio tt_gpio;
static int gpio_chn_ht;
static bool gpio_chn_ht_exists;
static bool tiantong_ldo_dio8018;
static bool cp_active;
static int sleep_pin_mode;

#define FOUR_PIN_MODE 4
#define TWO_PIN_MODE 2

static const struct of_device_id tiantong_control_match_table[] = {
	{ .compatible = "sylin,tiantong-control"},
	{},
};

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = tiantong_open,
	.release = tiantong_close,
	.unlocked_ioctl = tiantong_ioctl,
};

static inline void tiantong_print_pin_status(void)
{
	if (tiantong_ldo_dio8018)
		pr_info("%s: b:%d r:%d io:%d avdd_rx:%d avdd_tx:%d f:%d 0p8:%d 2p8:%d\n",
			__func__, gpio_get_value(tt_gpio.bootmode1), gpio_get_value(tt_gpio.reset),
			regulator_is_enabled(tt_reg.vrf_tiantong_io_1p8),
			regulator_is_enabled(tt_reg.vrf_tiantong_avdd_rx_1p8),
			regulator_is_enabled(tt_reg.vrf_tiantong_avdd_tx_1p8),
			regulator_is_enabled(tt_reg.vcc_flash_1v8),
			regulator_is_enabled(tt_reg.vrf_tiantong_0p8),
			regulator_is_enabled(tt_reg.vdd_nla_tiantong_2p8));
	else
		pr_info("%s: bootmode1: %d, reset: %d, vrf_1p8: %d, vrf_1p8_2: %d, vrf_0p8: %d\n",
			__func__, gpio_get_value(tt_gpio.bootmode1), gpio_get_value(tt_gpio.reset),
			gpio_get_value(tt_gpio.vrf_1p8), gpio_get_value(tt_gpio.vrf_1p8_2),
			gpio_get_value(tt_gpio.vrf_0p8));
}

static int tiantong_set_regulator_disable(struct regulator *reg)
{
	int ret = 0;

	ret = regulator_is_enabled(reg);
	if (ret < 0) {
		pr_err("%s: regulator_is_enabled error:%d\n", __func__, ret);
		return ret;
	}

	if (ret) {
		ret = regulator_disable(reg);
		if (ret < 0) {
			pr_err("%s: regulator_disable error:%d\n", __func__, ret);
			return ret;
		}
	}
	return ret;
}

static int tiantong_set_regulator_enable(struct regulator *reg)
{
	int ret = 0;

	ret = regulator_is_enabled(reg);
	if (ret < 0) {
		pr_err("%s: regulator_is_enabled error:%d\n", __func__, ret);
		return ret;
	}

	if (ret != 1) {
		ret = regulator_enable(reg);
		if (ret < 0) {
			pr_err("%s: regulator_enable error:%d\n", __func__, ret);
			return ret;
		}
	}

	return ret;
}

static int tiantong_set_power_32k(int val)
{
	int ret;

	if (tiantong_ldo_dio8018) {
		if (val) {
			ret = tiantong_set_regulator_enable(tt_reg.vrf_tiantong_io_1p8);
			if (ret < 0)
				pr_err("%s: regulator enable for vrf_tiantong_io_1p8 error:%d\n", __func__, ret);

			ret = tiantong_set_regulator_enable(tt_reg.vrf_tiantong_avdd_rx_1p8);
			if (ret < 0)
				pr_err("%s: regulator enable for vrf_tiantong_avdd_rx_1p8 error:%d\n", __func__, ret);

			ret = tiantong_set_regulator_enable(tt_reg.vrf_tiantong_avdd_tx_1p8);
			if (ret < 0)
				pr_err("%s: regulator enable for vrf_tiantong_avdd_tx_1p8 error:%d\n", __func__, ret);

			ret = tiantong_set_regulator_enable(tt_reg.vcc_flash_1v8);
			if (ret < 0)
				pr_err("%s: regulator enable for vcc_flash_1v8 error:%d\n", __func__, ret);
		} else {
			ret = tiantong_set_regulator_disable(tt_reg.vrf_tiantong_io_1p8);
			if (ret < 0)
				pr_err("%s: regulator disable for vrf_tiantong_io_1p8 error:%d\n", __func__, ret);

			ret = tiantong_set_regulator_disable(tt_reg.vrf_tiantong_avdd_rx_1p8);
			if (ret < 0)
				pr_err("%s: regulator disable for vrf_tiantong_avdd_rx_1p8 error:%d\n", __func__, ret);

			ret = tiantong_set_regulator_disable(tt_reg.vrf_tiantong_avdd_tx_1p8);
			if (ret < 0)
				pr_err("%s: regulator disable for vrf_tiantong_avdd_tx_1p8 error:%d\n", __func__, ret);

			ret = tiantong_set_regulator_disable(tt_reg.vcc_flash_1v8);
			if (ret < 0)
				pr_err("%s: regulator disable for vcc_flash_1v8 error:%d\n", __func__, ret);
		}
	} else {
		gpio_set_value(tt_gpio.vrf_1p8, val);
		gpio_set_value(tt_gpio.vrf_1p8_2, val);
	}

	return 0;
}

static int tiantong_set_power_19p2m(int val)
{
	int ret;

	if (tiantong_ldo_dio8018) {
		if (val) {
			ret = tiantong_set_regulator_enable(tt_reg.vrf_tiantong_0p8);
			if (ret < 0)
				pr_err("%s: regulator enable for vrf_tiantong_0p8 error:%d\n", __func__, ret);

			ret = tiantong_set_regulator_enable(tt_reg.vdd_nla_tiantong_2p8);
			if (ret < 0)
				pr_err("%s: regulator enable for vdd_nla_tiantong_2p8 error:%d\n", __func__, ret);
		} else {
			ret = tiantong_set_regulator_disable(tt_reg.vrf_tiantong_0p8);
			if (ret < 0)
				pr_err("%s: regulator disable for vrf_tiantong_0p8 error:%d\n", __func__, ret);

			ret = tiantong_set_regulator_disable(tt_reg.vdd_nla_tiantong_2p8);
			if (ret < 0)
				pr_err("%s: regulator disable for vdd_nla_tiantong_2p8 error:%d\n", __func__, ret);
		}
	} else
		gpio_set_value(tt_gpio.vrf_0p8, val);

	return 0;
}

static int tiantong_init_cdev(void)
{
	struct device *dev_struct;
	int ret = 0;

	pr_info("%s: ++\n", __func__);

	ret = alloc_chrdev_region(&dev_num, MINOR_BASE, MINOR_NUM, DEVICE_NAME);
	if (ret < 0) {
		pr_err("%s: failed to allocate device num for %s, error:%d\n", __func__, DEVICE_NAME, ret);
		return ret;
	}

	cdev_init(&tiantong_cdev, &fops);

	ret = cdev_add(&tiantong_cdev, dev_num, MINOR_NUM);
	if (ret < 0) {
		pr_err("%s: failed to add a cdev struct. error:%d\n", __func__, ret);
		goto unreg_device_num;
	}

	tiantong_class = class_create(DEVICE_NAME);
	if (IS_ERR(tiantong_class)) {
		pr_err("%s: failed to create a class struct\n", __func__);
		ret = -1;
		goto unreg_cdev;
	}

	dev_struct = device_create(tiantong_class, NULL, dev_num, NULL, DEVICE_NAME);
	if (IS_ERR(dev_struct)) {
		pr_err("%s: failed to create a device file\n", __func__);
		ret = -2;
		goto unreg_class;
	}

	pr_info("%s: Major: %d, Minor:%d\n", __func__, MAJOR(dev_num), MINOR(dev_num));
	pr_info("%s: --\n", __func__);
	return 0;

unreg_class:
	class_destroy(tiantong_class);

unreg_cdev:
	cdev_del(&tiantong_cdev);

unreg_device_num:
	unregister_chrdev_region(MKDEV(dev_num, MINOR_BASE), MINOR_NUM);

	pr_info("%s: init cdev failed --\n", __func__);

	return ret;
}

void tiantong_set_ap2cp_wakeup(int value)
{
	gpio_set_value(tt_gpio.ap2cp_wakeup, value);
}
EXPORT_SYMBOL(tiantong_set_ap2cp_wakeup);

int tiantong_get_ap2cp_wakeup(void)
{
	return gpio_get_value(tt_gpio.ap2cp_wakeup);
}
EXPORT_SYMBOL(tiantong_get_ap2cp_wakeup);

int tiantong_get_ap2cp_status(void)
{
	if (sleep_pin_mode == FOUR_PIN_MODE)
		return gpio_get_value(tt_gpio.ap2cp_status);
	else
		return -1;
}
EXPORT_SYMBOL(tiantong_get_ap2cp_status);

int tiantong_get_cp2ap_wakeup(void)
{
	return gpio_get_value(tt_gpio.cp2ap_wakeup);
}
EXPORT_SYMBOL(tiantong_get_cp2ap_wakeup);

int tiantong_get_cp2ap_status(void)
{
	if (sleep_pin_mode == FOUR_PIN_MODE)
		return gpio_get_value(tt_gpio.cp2ap_status);
	else
		return -1;
}
EXPORT_SYMBOL(tiantong_get_cp2ap_status);

int tiantong_active(void)
{
	return cp_active;
}
EXPORT_SYMBOL(tiantong_active);

static int tiantong_init_dio8018(struct platform_device *pdev)
{
	pr_info("%s: init dio8018 regulator\n", __func__);

	tt_reg.vrf_tiantong_io_1p8 = devm_regulator_get(&pdev->dev, "vrf_tiantong_io_1p8");
	if (IS_ERR(tt_reg.vrf_tiantong_io_1p8)) {
		pr_err("%s: get vrf_tiantong_io_1p8 failed\n", __func__);
		return -EINVAL;
	}

	tt_reg.vrf_tiantong_avdd_rx_1p8 = devm_regulator_get(&pdev->dev, "vrf_tiantong_avdd_rx_1p8");
	if (IS_ERR(tt_reg.vrf_tiantong_avdd_rx_1p8)) {
		pr_err("%s: get vrf_tiantong_avdd_rx_1p8 failed\n", __func__);
		return -EINVAL;
	}

	tt_reg.vrf_tiantong_avdd_tx_1p8 = devm_regulator_get(&pdev->dev, "vrf_tiantong_avdd_tx_1p8");
	if (IS_ERR(tt_reg.vrf_tiantong_avdd_tx_1p8)) {
		pr_err("%s: get vrf_tiantong_avdd_tx_1p8 failed\n", __func__);
		return -EINVAL;
	}

	tt_reg.vrf_tiantong_0p8 = devm_regulator_get(&pdev->dev, "vrf_tiantong_0p8");
	if (IS_ERR(tt_reg.vrf_tiantong_0p8)) {
		pr_err("%s: get vrf_tiantong_0p8 failed\n", __func__);
		return -EINVAL;
	}

	tt_reg.vcc_flash_1v8 = devm_regulator_get(&pdev->dev, "vcc_flash_1v8");
	if (IS_ERR(tt_reg.vcc_flash_1v8)) {
		pr_err("%s: get vcc_flash_1v8 failed\n", __func__);
		return -EINVAL;
	}

	tt_reg.vdd_nla_tiantong_2p8 = devm_regulator_get(&pdev->dev, "vdd_nla_tiantong_2p8");
	if (IS_ERR(tt_reg.vdd_nla_tiantong_2p8)) {
		pr_err("%s: get vdd_nla_tiantong_2p8 failed\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static int tiantong_init_gpio(struct platform_device *pdev)
{
	struct device_node *np;
	int ret = 0;

	pr_info("%s ++\n", __func__);
	np = pdev->dev.of_node;

	tt_gpio.bootmode1 = of_get_named_gpio(np, "sylin,bootmode-gpio", 0);
	if (!gpio_is_valid(tt_gpio.bootmode1)) {
		pr_err("%s: bootmode-gpio is not valid: %d\n", __func__, tt_gpio.bootmode1);
		return -EINVAL;
	}

	tt_gpio.reset = of_get_named_gpio(np, "sylin,reset-gpio", 0);
	if (!gpio_is_valid(tt_gpio.reset)) {
		pr_err("%s: reset_gpio is not valid: %d\n", __func__, tt_gpio.reset);
		return -EINVAL;
	}

	tt_gpio.ap2cp_wakeup = of_get_named_gpio(np, "sylin,ap2cp-wakeup-gpio", 0);
	if (!gpio_is_valid(tt_gpio.ap2cp_wakeup)) {
		pr_err("%s: ap2cp_wakeup_gpio is not valid: %d\n", __func__, tt_gpio.ap2cp_wakeup);
		return -EINVAL;
	}

	tt_gpio.cp2ap_wakeup = of_get_named_gpio(np, "sylin,cp2ap-wakeup-gpio", 0);
	if (!gpio_is_valid(tt_gpio.cp2ap_wakeup)) {
		pr_err("%s: cp2ap_wakeup_gpio is not valid: %d\n", __func__, tt_gpio.cp2ap_wakeup);
		return -EINVAL;
	}
	tt_gpio.irq_cp2ap_wakeup = gpio_to_irq(tt_gpio.cp2ap_wakeup);

	if (sleep_pin_mode == FOUR_PIN_MODE) {
		tt_gpio.ap2cp_status = of_get_named_gpio(np, "sylin,ap2cp-status-gpio", 0);
		if (!gpio_is_valid(tt_gpio.ap2cp_status)) {
			pr_err("%s: ap2cp_status_gpio is not valid: %d\n", __func__, tt_gpio.ap2cp_status);
			return -EINVAL;
		}

		tt_gpio.cp2ap_status = of_get_named_gpio(np, "sylin,cp2ap-status-gpio", 0);
		if (!gpio_is_valid(tt_gpio.cp2ap_status)) {
			pr_err("%s: cp2ap_status_gpio is not valid: %d\n", __func__, tt_gpio.cp2ap_status);
			return -EINVAL;
		}
		tt_gpio.irq_cp2ap_status = gpio_to_irq(tt_gpio.cp2ap_status);
	}

	if (tiantong_ldo_dio8018) {
		ret = tiantong_init_dio8018(pdev);
		if (ret != 0) {
			pr_err("%s: init dio8018 regulator failed\n", __func__);
			return -EINVAL;
		}
	} else {
		tt_gpio.vrf_1p8 = of_get_named_gpio(np, "sylin,vrf-tiantong-1p8", 0);
		if (!gpio_is_valid(tt_gpio.vrf_1p8)) {
			pr_err("%s: vrf-tiantong-1p8 is not valid: %d\n", __func__, tt_gpio.vrf_1p8);
			return -EINVAL;
		}

		tt_gpio.vrf_1p8_2 = of_get_named_gpio(np, "sylin,vrf-tiantong-1p8-2", 0);
		if (!gpio_is_valid(tt_gpio.vrf_1p8_2)) {
			pr_err("%s: sylin,vrf-tiantong-1p8-2 is not valid: %d\n", __func__, tt_gpio.vrf_1p8_2);
			return -EINVAL;
		}

		tt_gpio.vrf_0p8 = of_get_named_gpio(np, "sylin,vrf-tiantong-0p8", 0);
		if (!gpio_is_valid(tt_gpio.vrf_0p8)) {
			pr_err("%s: vrf-tiantong-0p8 is not valid: %d\n", __func__, tt_gpio.vrf_0p8);
			return -EINVAL;
		}
	}

	gpio_chn_ht = of_get_named_gpio(np, "sylin,gpio-chn-ht", 0);
	if (!gpio_is_valid(gpio_chn_ht)) {
		pr_info("%s: gpio_chn_ht is not valid: %d\n", __func__, gpio_chn_ht);
		gpio_chn_ht_exists = 0;
	} else
		gpio_chn_ht_exists = 1;

	ret = gpio_request_one(tt_gpio.bootmode1, GPIOF_OUT_INIT_LOW, tiantong_bootmode1_str);
	if (ret < 0) {
		pr_err("%s: request bootmode failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.bootmode1);
		return ret;
	}

	ret = gpio_request_one(tt_gpio.reset, GPIOF_OUT_INIT_LOW, tiantong_reset_str);
	if (ret < 0) {
		pr_err("%s: request reset_gpio failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.reset);
		return ret;
	}

	ret = gpio_request_one(tt_gpio.ap2cp_wakeup, GPIOF_OUT_INIT_LOW, tiantong_ap2cp_wakeup_str);
	if (ret < 0) {
		pr_err("%s: request ap2cp_wakeup_gpio failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.ap2cp_wakeup);
		return ret;
	}

	ret = gpio_request_one(tt_gpio.cp2ap_wakeup, GPIOF_IN, tiantong_cp2ap_wakeup_str);
	if (ret < 0) {
		pr_err("%s: request cp2ap_wakeup_gpio failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.cp2ap_wakeup);
		return ret;
	}

	if (sleep_pin_mode == FOUR_PIN_MODE) {
		ret = gpio_request_one(tt_gpio.ap2cp_status, GPIOF_OUT_INIT_LOW, tiantong_ap2cp_status_str);
		if (ret < 0) {
			pr_err("%s: request ap2cp_status_gpio failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.ap2cp_status);
			return ret;
		}

		ret = gpio_request_one(tt_gpio.cp2ap_status, GPIOF_IN, tiantong_cp2ap_status_str);
		if (ret < 0) {
			pr_err("%s: request cp2ap_status_gpio failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.cp2ap_status);
			return ret;
		}
	}

	if (!tiantong_ldo_dio8018) {
		ret = gpio_request_one(tt_gpio.vrf_1p8, GPIOF_OUT_INIT_LOW, tiantong_vrf_1p8_str);
		if (ret < 0) {
			pr_err("%s: request vrf_1p8 failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.vrf_1p8);
			return ret;
		}

		ret = gpio_request_one(tt_gpio.vrf_1p8_2, GPIOF_OUT_INIT_LOW, tiantong_vrf_1p8_2_str);
		if (ret < 0) {
			pr_err("%s: request vrf_1p8_2 failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.vrf_1p8_2);
			return ret;
		}

		ret = gpio_request_one(tt_gpio.vrf_0p8, GPIOF_OUT_INIT_LOW, tiantong_vrf_0p8_str);
		if (ret < 0) {
			pr_err("%s: request vrf_0p8 failed:%d. gpio num:%d\n", __func__, ret, tt_gpio.vrf_0p8);
			return ret;
		}
	}

	if (gpio_chn_ht_exists) {
		ret = gpio_request(gpio_chn_ht, gpio_chn_ht_str);
		if (ret < 0) {
			pr_err("%s: request gpio_chn_ht failed:%d. gpio num:%d\n", __func__, ret, gpio_chn_ht);
			return ret;
		}
	}

	pr_info("%s: --\n", __func__);

	return 0;
}

static int tiantong_power_on(void)
{
	pr_info("%s: ++ power on tiantong modem\n", __func__);

	tiantong_print_pin_status();

	tiantong_set_power_32k(1);
	gpio_set_value(tt_gpio.bootmode1, 1);

	mdelay(2);
	tiantong_set_power_19p2m(1);

	mdelay(1);
	gpio_set_value(tt_gpio.reset, 0);
	mdelay(2);
	gpio_set_value(tt_gpio.reset, 1);

	mdelay(1);
	tiantong_print_pin_status();

	if (sleep_pin_mode == FOUR_PIN_MODE)
		gpio_set_value(tt_gpio.ap2cp_status, 1);

	pr_info("%s: -- power on tiantong modem done\n", __func__);

	return 0;
}

static int tiantong_power_off(void)
{
	pr_info("%s: ++ power off tiantong modem\n", __func__);

	cp_active = false;

	tiantong_print_pin_status();

	gpio_set_value(tt_gpio.reset, 0);
	mdelay(2);

	tiantong_set_power_19p2m(0);
	mdelay(2);

	tiantong_set_power_32k(0);
	gpio_set_value(tt_gpio.bootmode1, 0);

	mdelay(1);
	tiantong_print_pin_status();

	if (sleep_pin_mode == FOUR_PIN_MODE)
		gpio_set_value(tt_gpio.ap2cp_status, 0);

	pr_info("%s: -- power off tiantong modem done\n", __func__);

	return 0;
}

static int tiantong_reset(void)
{
	pr_info("%s: ++ reset tiantong modem\n", __func__);

	cp_active = false;

	tiantong_print_pin_status();

	gpio_set_value(tt_gpio.reset, 0);
	mdelay(1);
	gpio_set_value(tt_gpio.reset, 1);

	mdelay(1);
	tiantong_print_pin_status();

	if (sleep_pin_mode == FOUR_PIN_MODE)
		gpio_set_value(tt_gpio.ap2cp_status, 1);

	pr_info("%s: -- reset tiantong modem done\n", __func__);

	return 0;
}

static long tiantong_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int val = 0;
	int ret;

	switch (cmd) {
	case TT_BOOT_MODE:
		ret = copy_from_user(&val, (void __user *)arg, sizeof(int));
		if (ret != 0) {
			pr_err("%s: cmd:TT_BOOT_MODE ret:%d\n", __func__, ret);
			return -EFAULT;
		}
		pr_info("%s: cmd TT_BOOT_MODE, val:%d\n", __func__, val);
		gpio_set_value(tt_gpio.bootmode1, val);
		break;
	case TT_RESET_N:
		ret = copy_from_user(&val, (void __user *)arg, sizeof(int));
		if (ret != 0) {
			pr_err("%s: cmd: TT_RESET_N ret:%d\n", __func__, ret);
			return -EFAULT;
		}
		pr_info("%s: cmd TT_RESET_N, val:%d\n", __func__, val);
		gpio_set_value(tt_gpio.reset, val);
		break;
	case TT_AP2CP_WAKE:
		ret = copy_from_user(&val, (void __user *)arg, sizeof(int));
		if (ret != 0) {
			pr_err("%s: cmd:TT_AP2CP_WAKE ret:%d\n", __func__, ret);
			return -EFAULT;
		}
		pr_info("%s: cmd TT_AP2CP_WAKE, val:%d\n", __func__, val);
		gpio_set_value(tt_gpio.ap2cp_wakeup, val);
		break;
	case TT_POWER_32K:
		ret = copy_from_user(&val, (void __user *)arg, sizeof(int));
		if (ret != 0) {
			pr_err("%s: cmd:TT_POWER_32K ret:%d\n", __func__, ret);
			return -EFAULT;
		}
		pr_info("%s: cmd TT_POWER_32K, val:%d\n", __func__, val);
		tiantong_set_power_32k(val);

		mdelay(1);
		tiantong_print_pin_status();
		break;
	case TT_POWER_19P2M:
		ret = copy_from_user(&val, (void __user *)arg, sizeof(int));
		if (ret != 0) {
			pr_err("%s: cmd:TT_POWER_19P2M ret:%d\n", __func__, ret);
			return -EFAULT;
		}
		pr_info("%s: cmd TT_POWER_19P2M, val:%d\n", __func__, val);
		tiantong_set_power_19p2m(val);

		mdelay(1);
		tiantong_print_pin_status();
		break;
	case IOCTL_CHECK_REGION:
		if (gpio_chn_ht_exists)
			val = gpio_get_value(gpio_chn_ht);
		else
			val = 1;

		pr_info("%s: cmd IOCTL_CHECK_REGION :%d\n", __func__, val);
		if (copy_to_user((int __user *)arg, &val, sizeof(int)))
			return -EFAULT;
		break;
	case IOCTL_POWER_ON:
		pr_info("%s: cmd IOCTL_POWER_ON\n", __func__);
		tiantong_power_on();
		break;
	case IOCTL_POWER_OFF:
		pr_info("%s: cmd IOCTL_POWER_OFF\n", __func__);
		tiantong_power_off();
		break;
	case IOCTL_POWER_RESET:
		pr_info("%s: cmd IOCTL_POWER_RESET\n", __func__);
		tiantong_reset();
		break;
	default:
		pr_info("%s: UNKNOWN CMD:%d\n", __func__, cmd);
		return -EFAULT;
	}

	return 0;
}

static irqreturn_t tiantong_wakeup_irq(int isr, void *dev)
{
	struct tian_device *tian_dev = platform_get_drvdata(dev);

	pr_info("%s cmd cp2ap_wakeup, val:%d\n", __func__, tiantong_get_cp2ap_wakeup());
	if (tiantong_get_cp2ap_wakeup()) {
		__pm_relax(tian_dev->tian_wake);
		__pm_stay_awake(tian_dev->tian_wake);
	} else {
		__pm_relax(tian_dev->tian_wake);
	}

	return IRQ_HANDLED;
}

static irqreturn_t tiantong_status_irq(int isr, void *dev)
{
	pr_info("%s cmd cp2ap_status, val:%d\n", __func__, tiantong_get_cp2ap_status());

	if (tiantong_get_cp2ap_status())
		cp_active = true;

	return IRQ_HANDLED;
}

static int tiantong_irq_init(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "%s ++\n", __func__);

	if (sleep_pin_mode == FOUR_PIN_MODE) {
		int ret = devm_request_irq(&pdev->dev, tt_gpio.irq_cp2ap_wakeup, tiantong_wakeup_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "tiantong_wakeup", pdev);
		if (ret) {
			pr_info("%s failed, %d\n", __func__, ret);
			return ret;
		}
		ret = irq_set_irq_wake(tt_gpio.irq_cp2ap_wakeup, 1);
		if (unlikely(ret))
			pr_info("%s: failed to set IRQ wake:%d\n", __func__, ret);

		ret = devm_request_irq(&pdev->dev, tt_gpio.irq_cp2ap_status, tiantong_status_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "tiantong_status", pdev);
		if (ret) {
			pr_info("%s failed, %d\n", __func__, ret);
			return ret;
		}
	}

	dev_info(&pdev->dev, "%s --\n", __func__);

	return 0;
}

static int tiantong_open(struct inode *inode, struct file *file)
{
	pr_info("%s: open /dev/%s\n", __func__, DEVICE_NAME);
	return 0;
}

static int tiantong_close(struct inode *inode, struct file *file)
{
	pr_info("%s: close /dev/%s\n", __func__, DEVICE_NAME);
	return 0;
}

static int tiantong_control_probe(struct platform_device *pdev)
{
	struct tian_device *tian_dev = NULL;
	int ret = 0;

	pr_info("%s: ++\n", __func__);

	tian_dev = kzalloc(sizeof(struct tian_device), GFP_KERNEL);
	if (!tian_dev) {
		pr_err("%s: failed to alloc tian_dev\n", __func__);
		return -ENOMEM;
	}
	tian_dev->pdev = pdev;
	platform_set_drvdata(pdev, tian_dev);

	tian_dev->tian_wake = wakeup_source_register(&pdev->dev,
			dev_name(&pdev->dev));
	if (!tian_dev->tian_wake) {
		dev_err(&pdev->dev,
				"Failed to register wakeup_source\n");
		return -ENOMEM;
	}
	cp_active = false;

	tiantong_ldo_dio8018 = of_property_read_bool(pdev->dev.of_node, "tiantong_ldo_dio8018");
	pr_info("%s: tiantong_ldo_dio8018:%d\n", __func__, tiantong_ldo_dio8018);

	of_property_read_u32(pdev->dev.of_node, "sleep_pin_mode", &sleep_pin_mode);
	pr_info("%s: sleep_pin_mode:%d\n", __func__, sleep_pin_mode);

	ret = tiantong_init_gpio(pdev);
	if (ret < 0)
		pr_err("%s: init tiantong gpio error:%d\n", __func__, ret);

	ret = tiantong_init_cdev();
	if (ret < 0)
		pr_err("%s: init tiantong cdev error:%d\n", __func__, ret);

	ret = tiantong_irq_init(pdev);
	if (ret < 0)
		pr_err("%s: init tiantong irq error:%d\n", __func__, ret);

	pr_info("%s: --\n", __func__);

	return 0;
}

static int tiantong_control_remove(struct platform_device *pdev)
{
	struct tian_device *tian_dev = platform_get_drvdata(pdev);

	dev_t dev = MKDEV(dev_num, MINOR_BASE);

	pr_info("%s: ++\n", __func__);

	gpio_free(tt_gpio.bootmode1);
	gpio_free(tt_gpio.reset);
	gpio_free(tt_gpio.ap2cp_wakeup);
	gpio_free(tt_gpio.cp2ap_wakeup);
	if (sleep_pin_mode == FOUR_PIN_MODE) {
		gpio_free(tt_gpio.ap2cp_status);
		gpio_free(tt_gpio.cp2ap_status);
	}
	if (!tiantong_ldo_dio8018) {
		gpio_free(tt_gpio.vrf_1p8);
		gpio_free(tt_gpio.vrf_1p8_2);
		gpio_free(tt_gpio.vrf_0p8);
	}
	if (gpio_chn_ht_exists)
		gpio_free(gpio_chn_ht);

	device_destroy(tiantong_class, dev_num);
	class_destroy(tiantong_class);
	cdev_del(&tiantong_cdev);
	unregister_chrdev_region(dev, MINOR_NUM);

	pr_info("%s: --\n", __func__);
	platform_set_drvdata(pdev, NULL);

	wakeup_source_unregister(tian_dev->tian_wake);
	tian_dev->tian_wake = NULL;

	pr_info("%s --\n", __func__);

	return 0;
}

static int tiantong_suspend(struct device *dev)
{
	if (sleep_pin_mode == FOUR_PIN_MODE) {
		if (cp_active) {
			pr_info("%s cmd ap2cp_wakeup: %d, cp2ap_wakeup:%d, ap2cp_status: %d, cp2ap_status: %d\n", __func__,
					gpio_get_value(tt_gpio.ap2cp_wakeup), gpio_get_value(tt_gpio.cp2ap_wakeup),
					gpio_get_value(tt_gpio.ap2cp_status), gpio_get_value(tt_gpio.cp2ap_status));
			gpio_set_value(tt_gpio.ap2cp_status, 0);
			msleep(10);
			pr_info("%s cmd ap2cp_wakeup: %d, cp2ap_wakeup:%d, ap2cp_status: %d, cp2ap_status: %d\n", __func__,
					gpio_get_value(tt_gpio.ap2cp_wakeup), gpio_get_value(tt_gpio.cp2ap_wakeup),
					gpio_get_value(tt_gpio.ap2cp_status), gpio_get_value(tt_gpio.cp2ap_status));
		}
	}
	return 0;
}

static int tiantong_resume(struct device *dev)
{
	if (sleep_pin_mode == FOUR_PIN_MODE) {
		if (cp_active) {
			gpio_set_value(tt_gpio.ap2cp_status, 1);
			pr_info("%s cmd ap2cp_wakeup: %d, cp2ap_wakeup:%d, ap2cp_status: %d, cp2ap_status: %d\n", __func__,
					gpio_get_value(tt_gpio.ap2cp_wakeup), gpio_get_value(tt_gpio.cp2ap_wakeup),
					gpio_get_value(tt_gpio.ap2cp_status), gpio_get_value(tt_gpio.cp2ap_status));
		}
	}
	return 0;
}

static const struct dev_pm_ops tiantong_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(tiantong_suspend, tiantong_resume)
};

static struct platform_driver tiantong_control_driver = {
	.probe = tiantong_control_probe,
	.remove = tiantong_control_remove,
	.driver = {
		.name = "sec_ipc_tiantong",
		.of_match_table = tiantong_control_match_table,
		.pm		= &tiantong_pm_ops,
	 },
};

static int __init tiantong_control_init(void)
{
	int ret;

	pr_info("%s ++\n", __func__);

	ret = platform_driver_register(&tiantong_control_driver);
	if (ret) {
		pr_err("%s: platform register failed %d\n",
			__func__, ret);
		return ret;
	}
	pr_info("%s: --\n", __func__);

	return 0;
}

static void __exit tiantong_control_exit(void)
{
	platform_driver_unregister(&tiantong_control_driver);
}

module_init(tiantong_control_init);
module_exit(tiantong_control_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SAMSUNG Electronics");
MODULE_DESCRIPTION("Tiantong Control Driver");
