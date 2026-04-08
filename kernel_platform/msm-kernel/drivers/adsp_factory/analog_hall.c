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

static ssize_t ahall_status_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;

	mutex_lock(&data->analog_hall_mutex);
	adsp_unicast(NULL, 0, MSG_SEQ_FOLD_MON, 0, MSG_TYPE_GET_RAW_DATA);

	while (!(data->ready_flag[MSG_TYPE_GET_RAW_DATA] & 1 << MSG_SEQ_FOLD_MON) &&
		cnt++ < TIMEOUT_CNT)
		msleep(20);

	data->ready_flag[MSG_TYPE_GET_RAW_DATA] &= ~(1 << MSG_SEQ_FOLD_MON);
	mutex_unlock(&data->analog_hall_mutex);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return snprintf(buf, PAGE_SIZE, "-1\n");
	}

	pr_info("[FACTORY] %s - ahall_status %d\n",
		__func__, data->msg_buf[MSG_SEQ_FOLD_MON][0]);

	return snprintf(buf, PAGE_SIZE, "%d\n",
		data->msg_buf[MSG_SEQ_FOLD_MON][0]);
}

static ssize_t ahall_int_cnt_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;
	bool success = false;
	int32_t interrupt_count = 0;

	mutex_lock(&data->analog_hall_mutex);
	adsp_unicast(NULL, 0, MSG_SEQ_FOLD_MON, 0, MSG_TYPE_ST_SHOW_DATA);

	while (!(data->ready_flag[MSG_TYPE_ST_SHOW_DATA] & 1 << MSG_SEQ_FOLD_MON) &&
		cnt++ < TIMEOUT_CNT)
		usleep_range(500, 550);

	data->ready_flag[MSG_TYPE_ST_SHOW_DATA] &= ~(1 << MSG_SEQ_FOLD_MON);
	mutex_unlock(&data->analog_hall_mutex);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return snprintf(buf, PAGE_SIZE, "%d\n", (int)success);
	}

	interrupt_count = data->msg_buf[MSG_SEQ_FOLD_MON][0];
	pr_info("[FACTORY]: %s - %d\n", __func__, interrupt_count);

	return snprintf(buf, PAGE_SIZE, "%d\n", interrupt_count);
}

static ssize_t ahall_int_cnt_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t msg_type = MSG_TYPE_FACTORY_DISABLE;
	uint8_t cnt = 0;

	if (sysfs_streq(buf, "1")) {
		pr_info("[FACTORY]: %s - on\n", __func__);
		msg_type = MSG_TYPE_FACTORY_ENABLE;
	} else if (sysfs_streq(buf, "0")) {
		pr_info("[FACTORY]: %s - off\n", __func__);
		msg_type = MSG_TYPE_FACTORY_DISABLE;
	}

	mutex_lock(&data->analog_hall_mutex);
	adsp_unicast(NULL, 0, MSG_SEQ_FOLD_MON, 0, msg_type);

	while (!(data->ready_flag[msg_type] & 1 << MSG_SEQ_FOLD_MON) &&
		cnt++ < TIMEOUT_CNT)
		usleep_range(500, 550);

	data->ready_flag[msg_type] &= ~(1 << MSG_SEQ_FOLD_MON);
	mutex_unlock(&data->analog_hall_mutex);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return size;
	}

	pr_info("[FACTORY]: %s - %d\n", __func__, data->msg_buf[MSG_SEQ_FOLD_MON][0]);

	return size;
}
static DEVICE_ATTR(ahall_status, 0444, ahall_status_show, NULL);
static DEVICE_ATTR(ahall_int_cnt, 0664,
	ahall_int_cnt_show, ahall_int_cnt_store);

static struct device_attribute *acc_attrs[] = {
	&dev_attr_ahall_status,
	&dev_attr_ahall_int_cnt,
	NULL,
};

int __init analog_hall_factory_init(void)
{
	adsp_factory_register(MSG_ANALOG_HALL, acc_attrs);
	pr_info("[FACTORY] %s\n", __func__);

	return 0;
}

void __exit analog_hall_factory_exit(void)
{
	adsp_factory_unregister(MSG_ANALOG_HALL);
	pr_info("[FACTORY] %s\n", __func__);
}
