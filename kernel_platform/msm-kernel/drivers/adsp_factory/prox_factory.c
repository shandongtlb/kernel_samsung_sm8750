/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include "adsp.h"
#if IS_ENABLED(CONFIG_SUPPORT_CONTROL_PROX_LED_GPIO)
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#define PROX_LED_EN_GPIO 113
#endif
#define PROX_AVG_COUNT 40
#define PROX_ALERT_THRESHOLD 200
#define PROX_TH_READ 0
#define PROX_TH_WRITE 1
#define BUFFER_MAX 128
#define PROX_REG_START 0x80
#define PROX_DETECT_HIGH_TH 16368
#define PROX_DETECT_LOW_TH 1000

enum {
	MAIN_DISPLAY_IDX,
	SUB_DISPLAY_IDX,
	MAX_DISPLAY_IDX,
};

struct prox_data {
	struct hrtimer prox_timer;
	struct work_struct work_prox;
	struct workqueue_struct *prox_wq;
	struct adsp_data *dev_data;
	int32_t min;
	int32_t max;
	int32_t avg;
	int32_t val;
	int32_t offset;
	int32_t reg_backup[2];
	int32_t debug_info_cmd;
	short avgwork_check;
	short avgtimer_enabled;
};

enum {
	PRX_THRESHOLD_DETECT_H,
	PRX_THRESHOLD_HIGH_DETECT_L,
	PRX_THRESHOLD_HIGH_DETECT_H,
	PRX_THRESHOLD_RELEASE_L,
};

enum {
	PROX_CMD_TYPE_GET_TRIM_CHECK,
	PROX_CMD_TYPE_GET_CAL_DATA,
	PROX_CMD_TYPE_INIT_CAL_DATA,
	PROX_CMD_TYPE_LED_CONTROL,
	PROX_CMD_TYPE_SAVE_CAL_DATA,
	PROX_CMD_TYPE_TOUCH_PROX,
	PROX_CMD_TYPE_MAX
};

static struct prox_data *pdata;

static int32_t get_prox_sidx(struct adsp_data *data)
{
	int32_t ret = MSG_PROX;
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	switch (data->fac_fstate) {
	case FSTATE_INACTIVE:
	case FSTATE_FAC_INACTIVE:
		ret = MSG_PROX;
		break;
	case FSTATE_ACTIVE:
	case FSTATE_FAC_ACTIVE:
	case FSTATE_FAC_INACTIVE_2:
		ret = MSG_PROX_SUB;
		break;
	default:
		break;
	}

#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC_BUT_SUPPORT_SINGLE_PROX)
	ret = MSG_PROX_SUB;
#endif
#endif /* CONFIG_SUPPORT_DUAL_OPTIC */
	return ret;
}

static int32_t prox_get_sidx_from_display_idx(int32_t idx)
{
	int32_t ret = MSG_PROX;
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	ret = idx == MAIN_DISPLAY_IDX ? MSG_PROX : MSG_PROX_SUB;
#endif
	return ret;
}

static int32_t prox_get_display_idx_from_sidx(int32_t idx)
{
	int32_t ret = MAIN_DISPLAY_IDX;
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	ret = idx == MSG_PROX ? MAIN_DISPLAY_IDX : SUB_DISPLAY_IDX;
#endif
	return ret;
}

static bool prox_send_message(struct adsp_data *data, int32_t *msg_buf, int32_t msg_size,
	int32_t display_idx, int32_t cmd, bool wait_for_response)
{
	int32_t prox_idx = prox_get_sidx_from_display_idx(display_idx), cnt = 0;

	mutex_lock(&data->prox_factory_mutex);
	adsp_unicast(msg_buf, msg_size, prox_idx, 0, cmd);
	if (wait_for_response) {
		while (!(data->ready_flag[cmd] & 1 << prox_idx) && cnt++ < TIMEOUT_CNT)
			usleep_range(500, 550);
		data->ready_flag[cmd] &= ~(1 << prox_idx);
	}
	mutex_unlock(&data->prox_factory_mutex);

	return (cnt < TIMEOUT_CNT) ? true : false;
}

#if IS_ENABLED(CONFIG_SUPPORT_PROX_CALIBRATION)
#if IS_ENABLED(CONFIG_SUPPORT_CONTROL_PROX_LED_GPIO)
static void prox_set_led_en(int32_t prox_idx)
{
	if (prox_idx == MSG_PROX) {
		int32_t led_gpio, ret;
		struct device_node *np = of_find_node_by_name(NULL, "ssc_prox_led_en_gpio");

		if (np == NULL) {
			pr_info("[SSC_FAC] %s: ssc_prox_led_en_gpio is NULL\n", __func__);
		} else {
			led_gpio = of_get_named_gpio_flags(np, "qcom,prox_led-en-gpio",	0, NULL);
			if (led_gpio >= 0) {
				ret = gpio_request(led_gpio, NULL);
				if (ret >= 0) {
					pr_info("[SSC_FAC] %s: prox_led_en_gpio set\n", __func__);
					gpio_direction_output(led_gpio, 1);
					gpio_free(led_gpio);
				} else {
					pr_err("[SSC_FAC] %s - gpio_request fail(%d)\n", __func__, ret);
				}
			} else {
				pr_err("[SSC_FAC] %s: prox_led_en_gpio fail(%d)\n", __func__, led_gpio);
			}
		}
	}
}
#endif

void prox_send_cal_data(struct adsp_data *data, uint16_t prox_idx, bool fac_cal)
{
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx), msg_buf[1], prox_cal;
	bool ret;

#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC_BUT_SUPPORT_SINGLE_PROX)
	if (prox_idx == MSG_PROX)
		return;
#endif
#if IS_ENABLED(CONFIG_SUPPORT_CONTROL_PROX_LED_GPIO)
	prox_set_led_en(prox_idx);
#endif
	if (prox_idx == MSG_PROX)
		prox_cal = data->prox_cal;
	else
		prox_cal = data->prox_sub_cal;

	if (!fac_cal || (prox_cal == 0)) {
#if IS_ENABLED(CONFIG_SEC_FACTORY)
		pr_info("[SSC_FAC] %s[%d]: No cal data (%d)\n", __func__, display_idx, prox_cal);
#else
		msg_buf[0] = -1;
		ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_SET_CAL_DATA, true);
		if (!ret)
			pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);

		pr_info("[SSC_FAC] %s[%d]: Excute in-use cal\n", __func__, display_idx);
#endif
	} else if (prox_cal > 0) {
		msg_buf[0] = prox_cal;

		ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_SET_CAL_DATA, true);
		if (!ret)
			pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
		else
			pdata->offset = data->msg_buf[prox_idx][0];

		pr_info("[SSC_FAC] %s[%d]: Cal data: %d(%d)\n", __func__, display_idx, msg_buf[0], pdata->offset);
	} else {
		pr_info("[SSC_FAC] %s[%d]: No cal data\n", __func__, display_idx);
	}
}

void prox_cal_init_work(struct adsp_data *data)
{
	data->prox_cal = 0;
	data->prox_sub_cal = 0;
}
#endif

static ssize_t prox_vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
#if IS_ENABLED(CONFIG_LIGHT_FACTORY)
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t display_idx = prox_get_display_idx_from_sidx(get_prox_sidx(data));

	return snprintf(buf, PAGE_SIZE, "%s\n",	data->light_device_vendor[display_idx]);
#else
	return snprintf(buf, PAGE_SIZE, "UNKNOWN\n");
#endif
}

static ssize_t prox_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
#if IS_ENABLED(CONFIG_LIGHT_FACTORY)
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t display_idx = prox_get_display_idx_from_sidx(get_prox_sidx(data));

	return snprintf(buf, PAGE_SIZE, "%s\n",	data->light_device_name[display_idx]);
#else
	return snprintf(buf, PAGE_SIZE, "UNKNOWN\n");
#endif
}

int32_t get_prox_raw_data(struct adsp_data *data, int32_t *raw_data, int32_t *offset)
{
	int32_t prox_idx = get_prox_sidx(data);
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	ret = prox_send_message(data, NULL, 0, display_idx, MSG_TYPE_GET_RAW_DATA, true);
	if (!ret) {
		pr_err("[FACTORY] %s[%d]: Timeout!!!\n", __func__, display_idx);
		return -1;
	}

	*raw_data = data->msg_buf[prox_idx][0];
	*offset = data->msg_buf[prox_idx][1];

	return 0;
}

static ssize_t prox_raw_data_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);

	if (pdata->avgwork_check == 0)
		get_prox_raw_data(data, &pdata->val, &pdata->offset);

	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->val);
}

static ssize_t prox_avg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n", pdata->min, pdata->avg, pdata->max);
}

static ssize_t prox_avg_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t new_value;

	if (sysfs_streq(buf, "0"))
		new_value = 0;
	else
		new_value = 1;

	if (new_value == pdata->avgtimer_enabled)
		return size;

	if (new_value == 0) {
		pdata->avgtimer_enabled = 0;
		hrtimer_cancel(&pdata->prox_timer);
		cancel_work_sync(&pdata->work_prox);
	} else {
		pdata->avgtimer_enabled = 1;
		pdata->dev_data = data;
		hrtimer_start(&pdata->prox_timer, ns_to_ktime(2000 * NSEC_PER_MSEC), HRTIMER_MODE_REL);
	}

	return size;
}

static void prox_work_func(struct work_struct *work)
{
	int32_t min = 0, max = 0, avg = 0;
	int32_t i;

	pdata->avgwork_check = 1;
	for (i = 0; i < PROX_AVG_COUNT; i++) {
		msleep(20);

		get_prox_raw_data(pdata->dev_data, &pdata->val, &pdata->offset);
		avg += pdata->val;

		if (!i)
			min = pdata->val;
		else if (pdata->val < min)
			min = pdata->val;

		if (pdata->val > max)
			max = pdata->val;
	}
	avg /= PROX_AVG_COUNT;

	pdata->min = min;
	pdata->avg = avg;
	pdata->max = max;
	pdata->avgwork_check = 0;
}

static enum hrtimer_restart prox_timer_func(struct hrtimer *timer)
{
	queue_work(pdata->prox_wq, &pdata->work_prox);
	hrtimer_forward_now(&pdata->prox_timer,	ns_to_ktime(2000 * NSEC_PER_MSEC));

	return HRTIMER_RESTART;
}

static void prox_led_control(struct adsp_data *data, int32_t led_number)
{
	int32_t prox_idx = get_prox_sidx(data), msg_buf[2];
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	msg_buf[0] = PROX_CMD_TYPE_LED_CONTROL;
	msg_buf[1] = led_number;

	ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_GET_CAL_DATA, true);
	if (!ret)
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
}

static ssize_t prox_led_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t data_buf, offset = 0, ret = 0, result = 1;

	prox_led_control(data, 0);
	msleep(200);
	ret = get_prox_raw_data(data, &data_buf, &offset);
	prox_led_control(data, 4);

	if (ret != 0)
		result = -1;

	pr_info("[SSC_FAC] %s: [%d] %d\n", __func__, result, data_buf);

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d,%d\n", result, data_buf, data_buf, data_buf, data_buf);
}

static int32_t prox_get_threshold(struct adsp_data *data, int32_t type)
{
	int32_t prox_idx = get_prox_sidx(data), msg_buf[2];
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	msg_buf[0] = type;
	msg_buf[1] = 0;

	ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_GET_THRESHOLD, true);
	if (!ret) {
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
		return 0;
	}

	return data->msg_buf[prox_idx][0];
}

static void prox_set_threshold(struct adsp_data *data, int32_t type, int32_t val)
{
	int32_t prox_idx = get_prox_sidx(data), msg_buf[2];
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	msg_buf[0] = type;
	msg_buf[1] = val;

	ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_SET_THRESHOLD, true);
	if (!ret)
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
}

static ssize_t prox_cal_show(struct device *dev, struct device_attribute *attr, char *buf)
{
#if IS_ENABLED(CONFIG_SUPPORT_PROX_CALIBRATION)
	struct adsp_data *data = dev_get_drvdata(dev);

	if (get_prox_sidx(data) == MSG_PROX)
		return snprintf(buf, PAGE_SIZE, "%d,0,0\n", data->prox_cal);
	else
		return snprintf(buf, PAGE_SIZE, "%d,0,0\n", data->prox_sub_cal);
#else
	return snprintf(buf, PAGE_SIZE, "0,0,0\n");
#endif
}

static ssize_t prox_cal_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
#if IS_ENABLED(CONFIG_SUPPORT_PROX_CALIBRATION)
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t prox_idx = get_prox_sidx(data), msg_buf[2], cmd;
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	if (sysfs_streq(buf, "1")) {
		cmd = PROX_CMD_TYPE_GET_CAL_DATA;
	} else if (sysfs_streq(buf, "0")) {
		cmd = PROX_CMD_TYPE_INIT_CAL_DATA;
	} else {
		pr_err("[SSC_FAC] %s[%d]: wrong value\n", __func__, display_idx);
		return size;
	}

	pr_info("[SSC_FAC] %s[%d]: msg %d\n", __func__, display_idx, cmd);

	ret = prox_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true);
	if (!ret) {
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
		return size;
	} else if (data->msg_buf[prox_idx][0] < 0) {
		pr_err("[SSC_FAC] %s[%d]: fail! %d\n", __func__, display_idx, data->msg_buf[prox_idx][0]);
		return size;
	}

	msg_buf[0] = PROX_CMD_TYPE_SAVE_CAL_DATA;
	msg_buf[1] = data->msg_buf[prox_idx][0];

	if (prox_idx == MSG_PROX)
		data->prox_cal = data->msg_buf[prox_idx][0];
	else
		data->prox_sub_cal = data->msg_buf[prox_idx][0];

	ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_SET_TEMPORARY_MSG, true);
	if (!ret)
		pr_err("[SSC_FAC] %s[%d]: SAVE_CAL_DATA Timeout!!\n", __func__, display_idx);

	if ((prox_idx == MSG_PROX) && (data->prox_cal > 0))
		prox_send_cal_data(data, prox_idx, true);
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	else if ((prox_idx == MSG_PROX_SUB) && (data->prox_sub_cal > 0))
		prox_send_cal_data(data, prox_idx, true);
#endif
#else
	pr_info("[SSC_FAC] %s: unsupported prox cal!\n", __func__);
#endif /* CONFIG_SUPPORT_PROX_CALIBRATION */
	return size;
}

static ssize_t prox_thresh_high_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t thd;

	thd = prox_get_threshold(data, PRX_THRESHOLD_DETECT_H);
	pr_info("[SSC_FAC] %s: %d\n", __func__, thd);

	return snprintf(buf, PAGE_SIZE, "%d\n",	thd);
}

static ssize_t prox_thresh_high_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t thd = 0;

	if (kstrtoint(buf, 10, &thd)) {
		pr_err("[SSC_FAC] %s: kstrtoint fail\n", __func__);
		return size;
	}

	prox_set_threshold(data, PRX_THRESHOLD_DETECT_H, thd);
	pr_info("[SSC_FAC] %s: %d\n", __func__, thd);

	return size;
}

static ssize_t prox_thresh_low_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t thd;

	thd = prox_get_threshold(data, PRX_THRESHOLD_RELEASE_L);
	pr_info("[SSC_FAC] %s: %d\n", __func__, thd);

	return snprintf(buf, PAGE_SIZE, "%d\n",	thd);
}

static ssize_t prox_thresh_low_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t thd = 0;

	if (kstrtoint(buf, 10, &thd)) {
		pr_err("[SSC_FAC] %s: kstrtoint fail\n", __func__);
		return size;
	}

	prox_set_threshold(data, PRX_THRESHOLD_RELEASE_L, thd);
	pr_info("[SSC_FAC] %s: %d\n", __func__, thd);

	return size;
}

static ssize_t prox_thresh_detect_high_show(struct device *dev,	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t thd;

	thd = prox_get_threshold(data, PRX_THRESHOLD_HIGH_DETECT_H);
	pr_info("[SSC_FAC] %s: %d\n", __func__, thd);

	return snprintf(buf, PAGE_SIZE, "%d\n",	thd);
}

static ssize_t prox_thresh_detect_high_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t thd = 0;

	if (kstrtoint(buf, 10, &thd)) {
		pr_err("[SSC_FAC] %s: kstrtoint fail\n", __func__);
		return size;
	}

	prox_set_threshold(data, PRX_THRESHOLD_HIGH_DETECT_H, thd);
	pr_info("[SSC_FAC] %s: %d\n", __func__, thd);

	return size;
}

static ssize_t prox_thresh_detect_low_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t thd;

	thd = prox_get_threshold(data, PRX_THRESHOLD_HIGH_DETECT_L);
	pr_info("[SSC_FAC] %s: %d\n", __func__, thd);

	return snprintf(buf, PAGE_SIZE, "%d\n",	thd);
}

static ssize_t prox_thresh_detect_low_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t thd = 0;

	if (kstrtoint(buf, 10, &thd)) {
		pr_err("[SSC_FAC] %s: kstrtoint fail\n", __func__);
		return size;
	}

	prox_set_threshold(data, PRX_THRESHOLD_HIGH_DETECT_L, thd);
	pr_info("[SSC_FAC] %s: %d\n", __func__, thd);

	return size;
}

static ssize_t prox_cancel_pass_show(struct device *dev, struct device_attribute *attr, char *buf)
{
#if IS_ENABLED(CONFIG_SUPPORT_PROX_CALIBRATION)
	struct adsp_data *data = dev_get_drvdata(dev);

	if (get_prox_sidx(data) == MSG_PROX)
		return snprintf(buf, PAGE_SIZE, "%d\n",	(data->prox_cal > 0) ? 1 : 0);
	else
		return snprintf(buf, PAGE_SIZE, "%d\n", (data->prox_sub_cal > 0) ? 1 : 0);
#else
	return snprintf(buf, PAGE_SIZE, "1\n");
#endif
}

static ssize_t prox_default_trim_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->offset);
}

static ssize_t prox_alert_thresh_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", PROX_ALERT_THRESHOLD);
}

static ssize_t prox_register_read_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t prox_idx = get_prox_sidx(data), msg_buf[1];
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	msg_buf[0] = pdata->reg_backup[0];

	ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_GET_REGISTER, true);
	if (!ret)
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
	else
		pdata->reg_backup[1] = data->msg_buf[prox_idx][0];

	pr_info("[SSC_FAC] %s[%d]: [0x%x]: %d\n", __func__, display_idx, pdata->reg_backup[0], pdata->reg_backup[1]);

	return snprintf(buf, PAGE_SIZE, "%d\n", pdata->reg_backup[1]);
}

static ssize_t prox_register_read_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int32_t reg = 0;

	if (sscanf(buf, "%3d", &reg) != 1) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	pdata->reg_backup[0] = reg;
	pr_info("[SSC_FAC] %s: [0x%x]\n", __func__, pdata->reg_backup[0]);

	return size;
}

static ssize_t prox_register_write_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t prox_idx = get_prox_sidx(data), msg_buf[2];
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	if (sscanf(buf, "%3d,%5d", &msg_buf[0], &msg_buf[1]) != 2) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_SET_REGISTER, true);
	if (!ret)
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
	else
		pdata->reg_backup[0] = msg_buf[0];

	pr_info("[SSC_FAC] %s: 0x%x - %d\n", __func__, msg_buf[0], data->msg_buf[prox_idx][0]);

	return size;
}

static ssize_t prox_touch_prox_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t prox_idx = get_prox_sidx(data), msg_buf[2];
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	if (sscanf(buf, "%2d", &msg_buf[1]) != 1) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	pr_info("[SSC_FAC] %s[%d]: event: %d\n", __func__, display_idx, msg_buf[1]);

	msg_buf[0] = PROX_CMD_TYPE_TOUCH_PROX;

	ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_GET_CAL_DATA, true);
	if (!ret)
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);

	return size;
}

static ssize_t prox_debug_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t prox_idx = get_prox_sidx(data), msg_buf[1];
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	msg_buf[0] = pdata->debug_info_cmd;

	ret = prox_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_GET_DUMP_REGISTER, true);
	if (!ret)
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		data->msg_buf[prox_idx][0], data->msg_buf[prox_idx][1],
		data->msg_buf[prox_idx][2], data->msg_buf[prox_idx][3],
		data->msg_buf[prox_idx][4], data->msg_buf[prox_idx][5],
		data->msg_buf[prox_idx][6], data->msg_buf[prox_idx][7],
		data->msg_buf[prox_idx][8], data->msg_buf[prox_idx][9],
		data->msg_buf[prox_idx][10], data->msg_buf[prox_idx][11]);
}

static ssize_t prox_debug_info_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	int32_t reg = 0;

	if (sscanf(buf, "%3d", &reg) != 1) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	pdata->debug_info_cmd = reg;

	return size;
}

static ssize_t prox_light_get_dhr_sensor_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t prox_idx = get_prox_sidx(data), offset = 0;
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	int32_t *info = data->msg_buf[prox_idx];
	bool ret;

	ret = prox_send_message(data, NULL, 0, display_idx, MSG_TYPE_GET_DHR_INFO, true);
	if (!ret)
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
	else
		pr_info("[SSC_FAC] %d,%d,%d,%d,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%d\n",
			info[0], info[1], info[2], info[3], info[4], info[5],
			info[6], info[7], info[8], info[9], info[10], info[11]);

	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"THD\":\"%d %d %d %d\",", info[0], info[1], info[2], info[3]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"PDRIVE_CURRENT\":\"%02x\",", info[4]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"PERSIST_TIME\":\"%02x\",", info[5]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"PPULSE\":\"%02x\",", info[6]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"PGAIN\":\"%02x\",", info[7]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"PTIME\":\"%02x\",", info[8]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"PPLUSE_LEN\":\"%02x\",", info[9]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"ATIME\":\"%02x\",", info[10]);
	offset += snprintf(buf + offset, PAGE_SIZE - offset,
		"\"POFFSET\":\"%d\"\n", info[11]);

	return offset;
}

static ssize_t prox_wakelock_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	return size;
}

static ssize_t prox_trim_check_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t prox_idx = get_prox_sidx(data), cmd;
	int32_t display_idx = prox_get_display_idx_from_sidx(prox_idx);
	bool ret;

	cmd = PROX_CMD_TYPE_GET_TRIM_CHECK;

	ret = prox_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true);
	if (!ret) {
		pr_err("[SSC_FAC] %s[%d]: Timeout!!!\n", __func__, display_idx);
		return snprintf(buf, PAGE_SIZE, "NG\n");
	}

	pr_info("[SSC_FAC] %s[%d]: [%s]: 0x%x, 0x%x\n", __func__, display_idx,
		(data->msg_buf[prox_idx][0] > 0) ? "TRIM" : "UNTRIM",
		(uint16_t)data->msg_buf[prox_idx][1], (uint16_t)data->msg_buf[prox_idx][2]);

	return snprintf(buf, PAGE_SIZE, "%s\n",	(data->msg_buf[prox_idx][0] > 0) ? "TRIM" : "UNTRIM");
}

static DEVICE_ATTR(vendor, 0444, prox_vendor_show, NULL);
static DEVICE_ATTR(name, 0444, prox_name_show, NULL);
static DEVICE_ATTR(state, 0444, prox_raw_data_show, NULL);
static DEVICE_ATTR(raw_data, 0444, prox_raw_data_show, NULL);
static DEVICE_ATTR(prox_led_test, 0444, prox_led_test_show, NULL);
static DEVICE_ATTR(prox_avg, 0664, prox_avg_show, prox_avg_store);
static DEVICE_ATTR(prox_cal, 0664, prox_cal_show, prox_cal_store);
static DEVICE_ATTR(thresh_high, 0664, prox_thresh_high_show, prox_thresh_high_store);
static DEVICE_ATTR(thresh_low, 0664, prox_thresh_low_show, prox_thresh_low_store);
static DEVICE_ATTR(register_write, 0220, NULL, prox_register_write_store);
static DEVICE_ATTR(register_read, 0664, prox_register_read_show, prox_register_read_store);
static DEVICE_ATTR(prox_offset_pass, 0444, prox_cancel_pass_show, NULL);
static DEVICE_ATTR(prox_trim, 0444, prox_default_trim_show, NULL);
static DEVICE_ATTR(thresh_detect_high, 0664, prox_thresh_detect_high_show, prox_thresh_detect_high_store);
static DEVICE_ATTR(thresh_detect_low, 0664, prox_thresh_detect_low_show, prox_thresh_detect_low_store);
static DEVICE_ATTR(prox_alert_thresh, 0444, prox_alert_thresh_show, NULL);
static DEVICE_ATTR(dhr_sensor_info, 0440, prox_light_get_dhr_sensor_info_show, NULL);
static DEVICE_ATTR(prox_wakelock, 0220, NULL, prox_wakelock_store);
static DEVICE_ATTR(trim_check, 0444, prox_trim_check_show, NULL);
static DEVICE_ATTR(debug_info, 0664, prox_debug_info_show, prox_debug_info_store);
static DEVICE_ATTR(touch_prox, 0220, NULL, prox_touch_prox_store);

static struct device_attribute *prox_attrs[] = {
	&dev_attr_vendor,
	&dev_attr_name,
	&dev_attr_state,
	&dev_attr_raw_data,
	&dev_attr_prox_led_test,
	&dev_attr_prox_avg,
	&dev_attr_prox_cal,
	&dev_attr_thresh_high,
	&dev_attr_thresh_low,
	&dev_attr_prox_offset_pass,
	&dev_attr_prox_trim,
	&dev_attr_thresh_detect_high,
	&dev_attr_thresh_detect_low,
	&dev_attr_prox_alert_thresh,
	&dev_attr_dhr_sensor_info,
	&dev_attr_register_write,
	&dev_attr_register_read,
	&dev_attr_prox_wakelock,
	&dev_attr_trim_check,
	&dev_attr_debug_info,
	&dev_attr_touch_prox,
	NULL,
};

int prox_factory_init(void)
{
	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	adsp_factory_register(MSG_PROX, prox_attrs);
	pr_info("[SSC_FAC] %s\n", __func__);

	hrtimer_init(&pdata->prox_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	pdata->prox_timer.function = prox_timer_func;
	pdata->prox_wq = create_singlethread_workqueue("prox_wq");

	/* this is the thread function we run on the work queue */
	INIT_WORK(&pdata->work_prox, prox_work_func);

	pdata->avgwork_check = 0;
	pdata->avgtimer_enabled = 0;
	pdata->avg = 0;
	pdata->min = 0;
	pdata->max = 0;
	pdata->offset = 0;
	pdata->debug_info_cmd = 0;

	return 0;
}

void prox_factory_exit(void)
{
	if (pdata->avgtimer_enabled == 1) {
		hrtimer_cancel(&pdata->prox_timer);
		cancel_work_sync(&pdata->work_prox);
	}
	destroy_workqueue(pdata->prox_wq);
	adsp_factory_unregister(MSG_PROX);
	kfree(pdata);
	pr_info("[SSC_FAC] %s\n", __func__);
}
