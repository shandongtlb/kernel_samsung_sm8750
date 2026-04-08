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
#include <linux/dirent.h>
#include "adsp.h"

#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
#define SUPPORTED_DISPLAY_COUNT 2
#else
#define SUPPORTED_DISPLAY_COUNT 1
#endif

enum {
	MAIN_DISPLAY_IDX,
	SUB_DISPLAY_IDX,
	MAX_DISPLAY_IDX,
};

#define NATUAL_SCREEN_MODE 0
#define VIVID_SCREEN_MODE  4

#define LOG_INTERVAL_SEC   3

#define NO_DEVICE_ID       0xff
#define NO_DEVICE_STR      "No_Device"

#define DEVICE_LIST_NUM    15
static const struct device_id_t device_list[DEVICE_LIST_NUM] = {
	/* ID, Vendor,      Name */
	{0x00, "Unknown",   "Unknown"},
	{0x18, "AMS",       "TCS3701"},
	{0x21, "SensorTek", "STK31610"},
	{0x88, "AMS",       "TMD4913"},
	{0x95, "AMS",       "TMD4914"},
	{0x61, "SensorTek", "STK33911"},
	{0x62, "SensorTek", "STK33917"},
	{0x63, "SensorTek", "STK33910"},
	{0x65, "SensorTek", "STK33915"},
	{0x70, "Capella",   "VEML3235"},
	{0x71, "Capella",   "VEML3328"},
	{0xF0, "SensorTek", "STK33F00"},
	{0xF1, "SensorTek", "STK33F11"},
	{0xF6, "SensorTek", "STK33F15"},
	{NO_DEVICE_ID, NO_DEVICE_STR, NO_DEVICE_STR},
};

#define ASCII_TO_DEC(x) (x - 48)
int brightness;

enum {
	OPTION_TYPE_COPR_ENABLE,
	OPTION_TYPE_BOLED_ENABLE,
	OPTION_TYPE_LCD_ONOFF,
	OPTION_TYPE_GET_COPR,
	OPTION_TYPE_GET_DDI_DEVICE_ID,
	OPTION_TYPE_SET_HALLIC_INFO,
	OPTION_TYPE_GET_LIGHT_CAL,
	OPTION_TYPE_SET_LIGHT_CAL,
	OPTION_TYPE_SET_LCD_VERSION,
	OPTION_TYPE_SET_UB_DISCONNECT,
	OPTION_TYPE_GET_LIGHT_DEBUG_INFO,
	OPTION_TYPE_SET_DEVICE_MODE,
	OPTION_TYPE_SET_PANEL_STATE,
	OPTION_TYPE_SET_PANEL_TEST_STATE,
	OPTION_TYPE_SET_AUTO_BRIGHTNESS_HYST,
	OPTION_TYPE_SET_PANEL_SCREEN_MODE,
	OPTION_TYPE_GET_LIGHT_CIRCLE_COORDINATES,
	OPTION_TYPE_SAVE_LIGHT_CAL,
	OPTION_TYPE_LOAD_LIGHT_CAL,
	OPTION_TYPE_GET_LIGHT_DEVICE_ID,
	OPTION_TYPE_GET_TRIM_CHECK,
	OPTION_TYPE_GET_SUB_ALS_LUX,
	OPTION_TYPE_GET_MAX_BRIGHTNESS,
	OPTION_TYPE_MAX
};

#if IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR) && IS_ENABLED(CONFIG_SEC_PANEL_NOTIFIER_V2)
#include <linux/sec_panel_notifier_v2.h>
#endif

#define LIGHT_CAL_PASS 1
#define LIGHT_CAL_FAIL 0

/*************************************************************************/
/* factory Sysfs							 */
/*************************************************************************/

int get_light_sidx(struct adsp_data *data)
{
	int ret = MSG_LIGHT;
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	switch (data->fac_fstate) {
	case FSTATE_INACTIVE:
		if (data->pre_panel_state[MAIN_DISPLAY_IDX] == 0 &&
			data->pre_panel_state[SUB_DISPLAY_IDX] > 0)
			ret = MSG_LIGHT_SUB;
		else
			ret = MSG_LIGHT;
		break;
	case FSTATE_FAC_INACTIVE:
		ret = MSG_LIGHT;
		break;
	case FSTATE_ACTIVE:
	case FSTATE_FAC_ACTIVE:
	case FSTATE_FAC_INACTIVE_2:
		ret = MSG_LIGHT_SUB;
		break;
	default:
		break;
	}
#endif
	return ret;
}

static int light_get_sidx_from_display_idx(int32_t idx)
{
	int ret = MSG_LIGHT;
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	ret = idx == MAIN_DISPLAY_IDX ? MSG_LIGHT : MSG_LIGHT_SUB;
#endif
	return ret;
}

static int light_get_display_idx_from_sidx(int32_t idx)
{
	int ret = MAIN_DISPLAY_IDX;
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	ret = idx == MSG_LIGHT ? MAIN_DISPLAY_IDX : SUB_DISPLAY_IDX;
#endif
	return ret;
}

static bool light_send_message(struct adsp_data *data, int32_t *msg_buf, int32_t msg_size,
	int32_t display_idx, int32_t cmd, bool wait_for_response, const char *caller)
{
	int32_t light_idx = light_get_sidx_from_display_idx(display_idx), cnt = 0;

	if (!data->light_factory_is_ready) {
		if (caller)
			pr_info("[SSC_FAC] %s[%d]: Factory daemon is not ready(%d).\n", caller, display_idx, cmd);
		return false;
	} else if (!strncmp(data->light_device_vendor[display_idx], NO_DEVICE_STR, DEVICE_INFO_LENGTH)) {
		if (caller)
			pr_err("[SSC_FAC] %s[%d]: No device(%d).\n", caller, display_idx, cmd);
		return false;
	}

	mutex_lock(&data->light_factory_mutex);
	adsp_unicast(msg_buf, msg_size, light_idx, 0, cmd);
	if (wait_for_response) {
		while (!(data->ready_flag[cmd] & 1 << light_idx) && cnt++ < TIMEOUT_CNT)
			usleep_range(500, 550);
		data->ready_flag[cmd] &= ~(1 << light_idx);
	}
	mutex_unlock(&data->light_factory_mutex);

	if (cnt >= TIMEOUT_CNT) {
		if (caller)
			pr_err("[SSC_FAC] %s[%d]: Timeout(%d).\n", caller, display_idx, cmd);

		return true;
	}

	return true;
}

static bool light_get_device_id(struct adsp_data *data, uint16_t light_idx)
{
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);
	int32_t cmd = OPTION_TYPE_GET_LIGHT_DEVICE_ID, i, device_index = UNKNOWN_INDEX;
	uint8_t device_id = 0;
	bool ret;

	memset(data->light_device_vendor[display_idx], 0, sizeof(data->light_device_vendor[display_idx]));

	ret = light_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		device_id = NO_DEVICE_ID;
	else
		device_id = (uint8_t)data->msg_buf[light_idx][0];

	pr_info("[SSC_FAC] %s[%d]: device_id: %d\n", __func__, display_idx, device_id);

	for (i = 0; i < DEVICE_LIST_NUM; i++)
		if (device_id == device_list[i].device_id)
			break;
	if (i >= DEVICE_LIST_NUM)
		pr_err("[SSC_FAC] %s[%d]: Unknown ID - (0x%x)\n", __func__, display_idx, device_id);
	else
		device_index = i;

	memcpy(data->light_device_vendor[display_idx], device_list[device_index].device_vendor,
		sizeof(char) * DEVICE_INFO_LENGTH);
	memcpy(data->light_device_name[display_idx], device_list[device_index].device_name,
		sizeof(char) * DEVICE_INFO_LENGTH);

	pr_info("[SSC_FAC] %s[%d]: Device ID - %s(%s)\n", __func__, display_idx,
		data->light_device_name[display_idx], data->light_device_vendor[display_idx]);

	return (device_id != NO_DEVICE_ID);
}

static ssize_t light_vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);

	if (!strncmp(data->light_device_vendor[display_idx], NO_DEVICE_STR, DEVICE_INFO_LENGTH))
		light_get_device_id(data, light_idx);

	return snprintf(buf, PAGE_SIZE, "%s\n", data->light_device_vendor[display_idx]);
}

static ssize_t light_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);

	if (!strncmp(data->light_device_vendor[display_idx], NO_DEVICE_STR, DEVICE_INFO_LENGTH))
		light_get_device_id(data, light_idx);

	return snprintf(buf, PAGE_SIZE, "%s\n",	data->light_device_name[display_idx]);
}

static ssize_t light_raw_data_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);
	bool ret;

	ret = light_send_message(data, NULL, 0, display_idx, MSG_TYPE_GET_RAW_DATA, true, __func__);
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "0,0,0,0,0,0\n");

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d,%d,%d\n",
		data->msg_buf[light_idx][0], data->msg_buf[light_idx][1],
		data->msg_buf[light_idx][2], data->msg_buf[light_idx][3],
		data->msg_buf[light_idx][4], data->msg_buf[light_idx][5]);
}

static ssize_t light_get_dhr_sensor_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);

	light_send_message(data, NULL, 0, display_idx, MSG_TYPE_GET_DHR_INFO, true, __func__);

	return data->msg_buf[light_idx][0];
}

#if !IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR)
static ssize_t light_brightness_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	pr_info("[SSC_FAC] %s: %d\n", __func__, brightness);
	return snprintf(buf, PAGE_SIZE, "%d\n", brightness);
}

static ssize_t light_brightness_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);

	brightness = ASCII_TO_DEC(buf[0]) * 100 + ASCII_TO_DEC(buf[1]) * 10 + ASCII_TO_DEC(buf[2]);

	light_send_message(data, &brightness, sizeof(int), display_idx, MSG_TYPE_SET_CAL_DATA, false, NULL);

	return size;
}
#endif

static ssize_t light_register_read_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), msg_buf[1];

	msg_buf[0] = data->light_temp_reg;

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_GET_REGISTER, true, __func__);

	pr_info("[SSC_FAC] %s: [0x%x]: 0x%x\n", __func__, msg_buf[0], data->msg_buf[light_idx][0]);

	return snprintf(buf, PAGE_SIZE, "[0x%x]: 0x%x\n", msg_buf[0], data->msg_buf[light_idx][0]);
}

static ssize_t light_register_read_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	int reg = 0;
	struct adsp_data *data = dev_get_drvdata(dev);

	if (sscanf(buf, "%3x", &reg) != 1) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	data->light_temp_reg = reg;
	pr_info("[SSC_FAC] %s: [0x%x]\n", __func__, data->light_temp_reg);

	return size;
}

static ssize_t light_register_write_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), msg_buf[2];

	if (sscanf(buf, "%3x,%3x", &msg_buf[0], &msg_buf[1]) != 2) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_SET_REGISTER, true, __func__);

	data->msg_buf[light_idx][MSG_LIGHT_MAX - 1] = msg_buf[0];
	pr_info("[SSC_FAC] %s: 0x%x - 0x%x\n", __func__, msg_buf[0], data->msg_buf[light_idx][0]);

	return size;
}

static ssize_t light_hyst_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);

	pr_info("[SSC_FAC] %s: %d,%d,%d,%d\n", __func__, data->hyst[0], data->hyst[1], data->hyst[2], data->hyst[3]);

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d\n", data->hyst[0], data->hyst[1], data->hyst[2], data->hyst[3]);
}

static ssize_t light_hyst_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), msg_buf[5];

	if (sscanf(buf, "%11d,%11d,%11d,%11d", &data->hyst[0], &data->hyst[1], &data->hyst[2], &data->hyst[3]) != 4) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	pr_info("[SSC_FAC] %s[%d]: (%d) %d < %d < %d\n", __func__, display_idx,
		data->hyst[0], data->hyst[1], data->hyst[2], data->hyst[3]);

	msg_buf[0] = OPTION_TYPE_SET_AUTO_BRIGHTNESS_HYST;
	msg_buf[1] = data->hyst[0];
	msg_buf[2] = data->hyst[1];
	msg_buf[3] = data->hyst[2];
	msg_buf[4] = data->hyst[3];

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);

	return size;
}

static ssize_t light_screen_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t display_idx = light_get_display_idx_from_sidx(get_light_sidx(data));

	return snprintf(buf, PAGE_SIZE, "%d\n", data->pre_screen_mode[display_idx]);
}

static ssize_t light_screen_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), screen_mode, msg_buf[3];

	if (sscanf(buf, "%2d", &screen_mode) != 1) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	screen_mode = (screen_mode == 0) ? NATUAL_SCREEN_MODE : VIVID_SCREEN_MODE;
	pr_info("[SSC_FAC] %s[%d]: panel screen mode %d\n", __func__, display_idx, screen_mode);

	if (data->pre_screen_mode[0] == screen_mode)
		return size;

	data->pre_screen_mode[0] = data->pre_screen_mode[1] = screen_mode;
	msg_buf[0] = OPTION_TYPE_SET_PANEL_SCREEN_MODE;
	msg_buf[1] = (int32_t)screen_mode;
	msg_buf[2] = display_idx;

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);

	return size;
}

#if IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR) && IS_ENABLED(CONFIG_SEC_PANEL_NOTIFIER_V2)
void light_brightness_work_func(struct work_struct *work)
{
	struct adsp_data *data = container_of((struct work_struct *)work,
		struct adsp_data, light_br_work);
	int32_t display_idx = data->brightness_info[2];

	if (display_idx >= MAIN_DISPLAY_IDX && display_idx < MAX_DISPLAY_IDX)
		data->brightness_info[5] = data->pre_panel_state[display_idx];

	light_send_message(data, data->brightness_info, sizeof(data->brightness_info),
		display_idx, MSG_TYPE_SET_CAL_DATA, true, NULL);
}

static void light_update_brightness_info(struct adsp_data *data, struct panel_notifier_event_data *panel_event)
{
	int32_t display_idx = panel_event->display_index;
	struct panel_event_bl_data bl = panel_event->d.bl;
#if IS_ENABLED(CONFIG_SUPPORT_PANEL_STATE_NOTIFY_FOR_LIGHT_SENSOR)
	static int32_t pre_finger_mask_hbm_on[MAX_DISPLAY_IDX] = {-1, -1};
#endif

	if (bl.level)
		bl.level /= data->brightness_resolution[display_idx];

	data->brightness_info[0] = bl.level;
	data->brightness_info[1] = bl.aor;
#if IS_ENABLED(CONFIG_SUPPORT_PANEL_STATE_NOTIFY_FOR_LIGHT_SENSOR)
	data->brightness_info[2] = display_idx;
	data->brightness_info[3] = bl.finger_mask_hbm_on;
	data->brightness_info[4] = bl.gradual_acl_val;

	if ((data->brightness_info[0] == data->pre_bl_level[display_idx]) &&
		(data->brightness_info[2] == data->pre_display_idx) &&
		(data->brightness_info[3] == pre_finger_mask_hbm_on[display_idx]) &&
		(data->brightness_info[4] == data->pre_acl_mode[display_idx]))
		return;

	pre_finger_mask_hbm_on[display_idx] = data->brightness_info[3];
	if (data->pre_acl_mode[display_idx] != data->brightness_info[4]) {
		pr_info("[SSC_FAC] %s[%d]: change acl status : %d -> %d\n",
			__func__, display_idx, data->pre_acl_mode[display_idx], data->brightness_info[4]);
		data->pre_acl_mode[display_idx] = data->brightness_info[4];
	}
#else
	if (data->brightness_info[0] == data->pre_bl_level[display_idx])
		return;
#endif

	if (data->brightness_info[0] <= 1 || data->pre_bl_level[display_idx] <= 1)
		pr_info("[SSC_FAC] %s[%d]: br: %d\n", __func__, display_idx, data->brightness_info[0]);

	data->pre_bl_level[display_idx] = data->brightness_info[0];
	data->pre_display_idx = data->brightness_info[2];

	schedule_work(&data->light_br_work);
}

static void light_send_ub_disconnect_message(struct adsp_data *data, struct panel_notifier_event_data *panel_event)
{
	enum panel_notifier_event_state_t state = panel_event->state;
	int32_t display_idx = panel_event->display_index;
	int32_t msg_buf[2];
	static int32_t pre_ub_con_state[MAX_DISPLAY_IDX] = {-1, -1};

	if ((int32_t)state == pre_ub_con_state[display_idx])
		return;

	pre_ub_con_state[display_idx] = (int32_t)state;
	msg_buf[0] = OPTION_TYPE_SET_UB_DISCONNECT;
	msg_buf[1] = (int32_t)state;

	pr_info("[SSC_FAC] %s[%d]: ub disconnected %d\n", __func__, display_idx, msg_buf[1]);

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);
}

#if IS_ENABLED(CONFIG_SUPPORT_PANEL_STATE_NOTIFY_FOR_LIGHT_SENSOR)
static void light_send_panel_state_message(struct adsp_data *data, struct panel_notifier_event_data *panel_event)
{
	enum panel_notifier_event_state_t state = panel_event->state;
	uint8_t display_idx = panel_event->display_index;
	int32_t panel_state = (int32_t)state - PANEL_EVENT_PANEL_STATE_OFF;
	int32_t msg_buf[4];

	if ((panel_state >= PANEL_EVENT_PANEL_STATE_LPM) ||
		((data->pre_panel_state[display_idx] == panel_state) &&
		(data->pre_panel_idx == (int32_t)display_idx)))
		return;

	data->pre_panel_state[display_idx] = panel_state;
	data->pre_panel_idx = (int32_t)display_idx;

	msg_buf[0] = OPTION_TYPE_SET_PANEL_STATE;
	msg_buf[1] = panel_state;
	msg_buf[2] = (int32_t)display_idx;
	msg_buf[3] = data->pre_screen_mode[display_idx];

	pr_info("[SSC_FAC] %s[%d]: panel_state %d(mode: %d)\n",
		__func__, display_idx, panel_state, data->pre_screen_mode[display_idx]);

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);
}

static void light_send_test_mode_message(struct adsp_data *data, struct panel_notifier_event_data *panel_event)
{
	enum panel_notifier_event_state_t state = panel_event->state;
	int32_t display_idx = panel_event->display_index;
	int32_t test_state = (int32_t)state - PANEL_EVENT_TEST_MODE_STATE_NONE;
	int32_t msg_buf[3];

	msg_buf[0] = OPTION_TYPE_SET_PANEL_TEST_STATE;
	msg_buf[1] = test_state;
	msg_buf[2] = display_idx;

	pr_info("[SSC_FAC] %s[%d]: panel test state %d\n", __func__, display_idx, test_state);

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);
}

static void light_send_screen_mode_message(struct adsp_data *data, struct panel_notifier_event_data *panel_event)
{
	uint8_t screen_mode = panel_event->d.screen_mode;
	int32_t display_idx = panel_event->display_index;
	int32_t msg_buf[3];

	if (data->pre_screen_mode[display_idx] == (int32_t)screen_mode)
		return;

	data->pre_screen_mode[display_idx] = (int32_t)screen_mode;
	msg_buf[0] = OPTION_TYPE_SET_PANEL_SCREEN_MODE;
	msg_buf[1] = (int32_t)screen_mode;
	msg_buf[2] = display_idx;

	pr_info("[SSC_FAC] %s[%d]: panel screen mode %d\n", __func__, display_idx, screen_mode);

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);
}
#endif /* CONFIG_SUPPORT_PANEL_STATE_NOTIFY_FOR_LIGHT_SENSOR */

#if IS_ENABLED(CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR)
void light_main_copr_work_func(struct work_struct *work)
{
	struct adsp_data *data = container_of((struct delayed_work *)work, struct adsp_data, light_main_copr_work);

	light_send_message(data, data->copr_data[MAIN_DISPLAY_IDX], sizeof(data->copr_data[MAIN_DISPLAY_IDX]),
		MAIN_DISPLAY_IDX, MSG_TYPE_SET_THRESHOLD, false, NULL);
	data->has_pending_main_copr_data = false;
}

void light_sub_copr_work_func(struct work_struct *work)
{
	struct adsp_data *data = container_of((struct delayed_work *)work, struct adsp_data, light_sub_copr_work);

	light_send_message(data, data->copr_data[SUB_DISPLAY_IDX], sizeof(data->copr_data[SUB_DISPLAY_IDX]),
		SUB_DISPLAY_IDX, MSG_TYPE_SET_THRESHOLD, false, NULL);
	data->has_pending_sub_copr_data = false;
}

static void light_update_copr_data(struct adsp_data *data, struct panel_notifier_event_data *panel_event)
{
	struct panel_event_copr_data copr_data = panel_event->d.copr;
	int32_t display_idx = panel_event->display_index;
	int32_t cur_time = (int32_t)(ktime_to_ns(ktime_get_boottime()) / NSEC_PER_SEC);

	mutex_lock(&data->light_factory_mutex);
	for (int i = 0; i < 5; i++)
		for (int j = 1; j <= 3; j++)
			data->copr_data[display_idx][(i * 3) + (j - 1)] = (int32_t)copr_data.stat[i][j];

	if (cur_time - data->last_ap_copr_log_time >= LOG_INTERVAL_SEC) {
		pr_info("[SSC_FAC] AP COPR[%d]: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
			display_idx, data->copr_data[display_idx][0],
			data->copr_data[display_idx][1], data->copr_data[display_idx][2],
			data->copr_data[display_idx][3], data->copr_data[display_idx][4],
			data->copr_data[display_idx][5], data->copr_data[display_idx][6],
			data->copr_data[display_idx][7], data->copr_data[display_idx][8],
			data->copr_data[display_idx][9], data->copr_data[display_idx][10],
			data->copr_data[display_idx][11], data->copr_data[display_idx][12],
			data->copr_data[display_idx][13], data->copr_data[display_idx][14]);
		data->last_ap_copr_log_time = cur_time;
	}

	if (data->light_factory_is_ready) {
		if (display_idx == MAIN_DISPLAY_IDX && !data->has_pending_main_copr_data) {
			data->has_pending_main_copr_data = true;
			schedule_delayed_work(&data->light_main_copr_work, msecs_to_jiffies(30));
		} else if (display_idx == SUB_DISPLAY_IDX && !data->has_pending_sub_copr_data && data->pre_panel_state[MAIN_DISPLAY_IDX] == 0) {
			data->has_pending_sub_copr_data = true;
			schedule_delayed_work(&data->light_sub_copr_work, msecs_to_jiffies(30));
		}
	}

	mutex_unlock(&data->light_factory_mutex);
}
#endif

int light_panel_data_notify(struct notifier_block *nb, unsigned long val, void *v)
{
	struct adsp_data *data = adsp_get_struct_data();
	struct panel_notifier_event_data *panel_event = v;
	int32_t display_idx = panel_event->display_index;

	if (display_idx >= SUPPORTED_DISPLAY_COUNT)
		return 0;

	switch (val) {
	case PANEL_EVENT_BL_STATE_CHANGED:
		light_update_brightness_info(data, panel_event);
		break;
	case PANEL_EVENT_UB_CON_STATE_CHANGED:
		light_send_ub_disconnect_message(data, panel_event);
		break;
#if IS_ENABLED(CONFIG_SUPPORT_PANEL_STATE_NOTIFY_FOR_LIGHT_SENSOR)
	case PANEL_EVENT_PANEL_STATE_CHANGED:
		light_send_panel_state_message(data, panel_event);
		break;
	case PANEL_EVENT_TEST_MODE_STATE_CHANGED:
		light_send_test_mode_message(data, panel_event);
		break;
	case PANEL_EVENT_SCREEN_MODE_STATE_CHANGED:
		light_send_screen_mode_message(data, panel_event);
		break;
#endif
#if IS_ENABLED(CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR)
	case PANEL_EVENT_COPR_STATE_CHANGED:
		light_update_copr_data(data, panel_event);
		break;
#endif
	default:
		break;
	}

	return 0;
}

static struct notifier_block light_panel_data_notifier = {
	.notifier_call = light_panel_data_notify,
	.priority = 1,
};
#endif /* CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR && CONFIG_SEC_PANEL_NOTIFIER_V2 */

static ssize_t light_hallic_info_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
#ifndef CONFIG_SEC_FACTORY
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), msg_buf[2];
#endif
	int32_t new_value;

	if (sysfs_streq(buf, "0"))
		new_value = 0;
	else if (sysfs_streq(buf, "1"))
		new_value = 1;
	else
		return size;

	pr_info("[SSC_FAC] %s: new_value %d\n", __func__, new_value);
#ifndef CONFIG_SEC_FACTORY
	msg_buf[0] = OPTION_TYPE_SET_HALLIC_INFO;
	msg_buf[1] = new_value;

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);
#endif
	return size;
}

static ssize_t light_lcd_onoff_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), msg_buf[3], new_value;

	if (sysfs_streq(buf, "0"))
		new_value = 0;
	else if (sysfs_streq(buf, "1"))
		new_value = 1;
	else
		return size;

#if IS_ENABLED(CONFIG_SUPPORT_PANEL_STATE_NOTIFY_FOR_LIGHT_SENSOR) && IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	if (data->pre_panel_idx >= 0)
		light_idx = light_get_sidx_from_display_idx(data->pre_panel_idx);
#endif
	pr_info("[SSC_FAC] %s[%d]: new_value %d\n", __func__, new_value, display_idx);

	data->pre_bl_level[0] = data->pre_bl_level[1] = -1;
	msg_buf[0] = OPTION_TYPE_LCD_ONOFF;
	msg_buf[1] = new_value;
	msg_buf[2] = data->pre_panel_state[display_idx];

	if (new_value == 1) {
#if IS_ENABLED(CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR)
		schedule_delayed_work(&data->light_copr_debug_work, msecs_to_jiffies(1000));
		data->light_copr_debug_count = 0;
#endif
	} else {
#if IS_ENABLED(CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR)
		cancel_delayed_work_sync(&data->light_copr_debug_work);
		data->light_copr_debug_count = 5;
#endif
	}

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);

	return size;
}

static ssize_t light_circle_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);
	int32_t cmd = OPTION_TYPE_GET_LIGHT_CIRCLE_COORDINATES;
	bool ret;

	ret = light_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "0 0 0 0 0 0\n");

	return snprintf(buf, PAGE_SIZE, "%d.%d %d.%d %d.%d %d.%d %d.%d %d.%d\n",
		data->msg_buf[light_idx][0] / 10,
		abs(data->msg_buf[light_idx][0]) % 10,
		data->msg_buf[light_idx][1] / 10,
		abs(data->msg_buf[light_idx][1]) % 10,
		data->msg_buf[light_idx][4] / 10,
		abs(data->msg_buf[light_idx][4]) % 10,
		data->msg_buf[light_idx][2] / 10,
		abs(data->msg_buf[light_idx][2]) % 10,
		data->msg_buf[light_idx][3] / 10,
		abs(data->msg_buf[light_idx][3]) % 10,
		data->msg_buf[light_idx][4] / 10,
		abs(data->msg_buf[light_idx][4]) % 10);
#else
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "0 0 0\n");
	
	return snprintf(buf, PAGE_SIZE, "%d.%d %d.%d %d.%d\n",
		data->msg_buf[light_idx][0] / 10,
		data->msg_buf[light_idx][0] % 10,
		data->msg_buf[light_idx][1] / 10,
		data->msg_buf[light_idx][1] % 10,
		data->msg_buf[light_idx][2] / 10,
		data->msg_buf[light_idx][2] % 10);
#endif
}

#if IS_ENABLED(CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR) || IS_ENABLED(CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR)
static ssize_t light_read_copr_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), msg_buf[2];
	int new_value;

	if (sysfs_streq(buf, "0"))
		new_value = 0;
	else if (sysfs_streq(buf, "1"))
		new_value = 1;
	else
		return size;

	pr_info("[SSC_FAC] %s[%d]: new_value %d\n", __func__, display_idx, new_value);
	msg_buf[0] = OPTION_TYPE_COPR_ENABLE;
	msg_buf[1] = new_value;

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);

	return size;
}

static ssize_t light_read_copr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), cmd = OPTION_TYPE_GET_COPR;
	bool ret;

	ret = light_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "-1\n");

	pr_info("[SSC_FAC] %s[%d]: %d\n", __func__, display_idx, data->msg_buf[light_idx][4]);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->msg_buf[light_idx][4]);
}

static ssize_t light_copr_roix_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);
	bool ret;

	ret = light_send_message(data, NULL, 0, display_idx, MSG_TYPE_GET_DUMP_REGISTER, true, __func__);
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "-1,-1,-1,-1\n");

	pr_info("[SSC_FAC] %s[%d]: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", __func__, display_idx,
		data->msg_buf[light_idx][0], data->msg_buf[light_idx][1],
		data->msg_buf[light_idx][2], data->msg_buf[light_idx][3],
		data->msg_buf[light_idx][4], data->msg_buf[light_idx][5],
		data->msg_buf[light_idx][6], data->msg_buf[light_idx][7],
		data->msg_buf[light_idx][8], data->msg_buf[light_idx][9] % 1000,
		data->msg_buf[light_idx][10] % 1000, data->msg_buf[light_idx][11] % 1000,
		data->msg_buf[light_idx][9] / 1000, data->msg_buf[light_idx][10] / 1000,
		data->msg_buf[light_idx][11] / 1000);

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		data->msg_buf[light_idx][0], data->msg_buf[light_idx][1],
		data->msg_buf[light_idx][2], data->msg_buf[light_idx][3],
		data->msg_buf[light_idx][4], data->msg_buf[light_idx][5],
		data->msg_buf[light_idx][6], data->msg_buf[light_idx][7],
		data->msg_buf[light_idx][8], data->msg_buf[light_idx][9],
		data->msg_buf[light_idx][10], data->msg_buf[light_idx][11]);
}

static ssize_t light_test_copr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), cmd = OPTION_TYPE_GET_COPR;
	bool ret;

	ret = light_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "-1,-1,-1,-1\n");

	pr_info("[SSC_FAC] %s[%d]: %d,%d,%d,%d\n", __func__, display_idx,
		data->msg_buf[light_idx][0], data->msg_buf[light_idx][1],
		data->msg_buf[light_idx][2], data->msg_buf[light_idx][3]);

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d\n",
		data->msg_buf[light_idx][0], data->msg_buf[light_idx][1],
		data->msg_buf[light_idx][2], data->msg_buf[light_idx][3]);
}

static ssize_t light_boled_enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);
	int32_t msg_buf[2];
	int new_value;

	if (sysfs_streq(buf, "0"))
		new_value = 0;
	else if (sysfs_streq(buf, "1"))
		new_value = 1;
	else
		return size;

	pr_info("[SSC_FAC] %s[%d]: new_value %d\n", __func__, display_idx, new_value);
	msg_buf[0] = OPTION_TYPE_BOLED_ENABLE;
	msg_buf[1] = new_value;

	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_GET_CAL_DATA, false, __func__);

	return size;
}
#endif /* CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR || CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR */

#if IS_ENABLED(CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR)
static ssize_t light_ddi_spi_check_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), cmd = OPTION_TYPE_GET_DDI_DEVICE_ID;
	bool ret;

	ret = light_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "-1\n");

	pr_info("[SSC_FAC] %s[%d]: %d\n", __func__, display_idx, data->msg_buf[light_idx][0]);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->msg_buf[light_idx][0]);
}

void light_copr_debug_work_func(struct work_struct *work)
{
	struct adsp_data *data = container_of((struct delayed_work *)work,
		struct adsp_data, light_copr_debug_work);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);
	bool ret;

#if IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR) && IS_ENABLED(CONFIG_SEC_PANEL_NOTIFIER_V2)
	if (data->pre_panel_state[display_idx] == 0)
		return;
#endif
	if (!strncmp(data->light_device_vendor[display_idx], NO_DEVICE_STR, DEVICE_INFO_LENGTH)) {
		light_get_device_id(data, light_idx);
		return;
	}

	ret = light_send_message(data, NULL, 0, display_idx, MSG_TYPE_GET_DUMP_REGISTER, true, __func__);
	if (!ret)
		return;

	pr_info("[SSC_FAC] %s[%d]: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", __func__, display_idx,
		data->msg_buf[light_idx][0], data->msg_buf[light_idx][1],
		data->msg_buf[light_idx][2], data->msg_buf[light_idx][3],
		data->msg_buf[light_idx][4], data->msg_buf[light_idx][5],
		data->msg_buf[light_idx][6], data->msg_buf[light_idx][7],
		data->msg_buf[light_idx][8], data->msg_buf[light_idx][9] % 1000,
		data->msg_buf[light_idx][10] % 1000, data->msg_buf[light_idx][11] % 1000,
		data->msg_buf[light_idx][9] / 1000, data->msg_buf[light_idx][10] / 1000,
		data->msg_buf[light_idx][11] / 1000);

	if (data->light_copr_debug_count++ < 5)
		schedule_delayed_work(&data->light_copr_debug_work, msecs_to_jiffies(1000));
}
#endif /* CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR */

static ssize_t light_debug_info_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int reg = 0;

	if (sscanf(buf, "%3d", &reg) != 1) {
		pr_err("[SSC_FAC]: %s - The number of data are wrong\n", __func__);
		return -EINVAL;
	}

	data->light_debug_info_cmd = reg;

	return size;
}

static ssize_t light_debug_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t msg_buf[2], display_idx = light_get_display_idx_from_sidx(light_idx);
	bool ret;

	msg_buf[0] = OPTION_TYPE_GET_LIGHT_DEBUG_INFO;
	msg_buf[1] = data->light_debug_info_cmd;

	ret = light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "0,0,0,0,0,0\n");

	return snprintf(buf, PAGE_SIZE, "%d,%d,%d,%d,%d,%d\n",
		data->msg_buf[light_idx][0], data->msg_buf[light_idx][1],
		data->msg_buf[light_idx][2], data->msg_buf[light_idx][3],
		data->msg_buf[light_idx][4] >> 16,
		data->msg_buf[light_idx][4] & 0xffff);
}

void light_get_brightness_resolution(struct adsp_data *data, int max_br, int32_t display_idx)
{
	if (max_br >= 25500)
		data->brightness_resolution[display_idx] = 100;
	else if (max_br >= 2550)
		data->brightness_resolution[display_idx] = 10;
	else if (max_br >= 255)
		data->brightness_resolution[display_idx] = 1;
	else
		data->brightness_resolution[display_idx] = 10;

	data->max_brightness[display_idx] = max_br;
	pr_info("[SSC_FAC] %s[%d]: brightness resolution %d",
		__func__, display_idx, data->brightness_resolution[display_idx]);
}

void light_init_work_func(struct work_struct *work)
{
	struct adsp_data *data = container_of((struct delayed_work *)work,
		struct adsp_data, light_init_work);
	int32_t retries = 3;
#if IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR) && IS_ENABLED(CONFIG_SEC_PANEL_NOTIFIER_V2)
	int32_t msg_buf[4], display_idx, light_idx, cmd = OPTION_TYPE_GET_MAX_BRIGHTNESS;

	for (display_idx = 0; display_idx < SUPPORTED_DISPLAY_COUNT; display_idx++) {
		light_idx = light_get_sidx_from_display_idx(display_idx);
		msg_buf[0] = OPTION_TYPE_SET_PANEL_STATE;
		msg_buf[1] = data->pre_panel_state[display_idx];
		msg_buf[2] = display_idx;
		msg_buf[3] = data->pre_screen_mode[display_idx];

		pr_info("[SSC_FAC] %s[%d]: panel_state %d(mode: %d)\n", __func__,
			display_idx, data->pre_panel_state[display_idx], data->pre_screen_mode[display_idx]);

		light_send_message(data, msg_buf, sizeof(msg_buf),
			display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);
		msleep(25);

		light_send_message(data, &cmd, sizeof(int32_t),
			display_idx, MSG_TYPE_SET_TEMPORARY_MSG, true, __func__);

		light_get_brightness_resolution(data, data->msg_buf[light_idx][0], display_idx);

		pr_info("[SSC_FAC] %s[%d]: max brightness %d\n", __func__, display_idx, data->msg_buf[light_idx][0]);
		msleep(25);
	}
#endif
	while (retries-- > 0) {
		if (light_get_device_id(data, MSG_LIGHT))
			break;
		msleep(25);
	}
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	retries = 3;
	while (retries-- > 0) {
		if (light_get_device_id(data, MSG_LIGHT_SUB))
			break;
		msleep(25);
	}
#endif

#if IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR) && IS_ENABLED(CONFIG_SEC_PANEL_NOTIFIER_V2)
	schedule_work(&data->light_br_work);
#if IS_ENABLED(CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR)
	if (data->pre_panel_state[MAIN_DISPLAY_IDX] && !data->has_pending_main_copr_data) {
		data->has_pending_main_copr_data = true;
		schedule_delayed_work(&data->light_main_copr_work, msecs_to_jiffies(30));
	}
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	else if (data->pre_panel_state[SUB_DISPLAY_IDX] && !data->has_pending_sub_copr_data) {
		data->has_pending_sub_copr_data = true;
		schedule_delayed_work(&data->light_sub_copr_work, msecs_to_jiffies(30));
	}
#endif
#endif /* CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR */
#endif /* CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR && CONFIG_SEC_PANEL_NOTIFIER_V2 */
}

void light_init_work(struct adsp_data *data)
{
	data->pre_bl_level[MAIN_DISPLAY_IDX] = data->pre_bl_level[SUB_DISPLAY_IDX] = -1;
	data->pre_panel_idx = -1;
	data->pre_display_idx = -1;
	data->light_debug_info_cmd = 0;
	data->light_factory_is_ready = true;
	data->brightness_resolution[MAIN_DISPLAY_IDX] = 10;
	data->brightness_resolution[SUB_DISPLAY_IDX] = 10;
	data->max_brightness[MAIN_DISPLAY_IDX] = -1;
	data->max_brightness[SUB_DISPLAY_IDX] = -1;
#if IS_ENABLED(CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR)
	data->has_pending_main_copr_data = false;
	data->has_pending_sub_copr_data = false;
#endif
	schedule_delayed_work(&data->light_init_work, msecs_to_jiffies(1000));
}

#if IS_ENABLED(CONFIG_SUPPORT_LIGHT_CALIBRATION)
void light_cal_read_work_func(struct work_struct *work)
{
	struct adsp_data *data = container_of((struct delayed_work *)work,
		struct adsp_data, light_cal_work);
	uint16_t light_idx;
	int32_t msg_buf[5] = {0, }, cmd = OPTION_TYPE_LOAD_LIGHT_CAL, display_idx;
	bool ret;

	for (display_idx = 0; display_idx < SUPPORTED_DISPLAY_COUNT; display_idx++) {
		light_idx = light_get_sidx_from_display_idx(display_idx);
		memset(msg_buf, 0, sizeof(msg_buf));

		ret = light_send_message(data, &cmd, sizeof(int32_t),
			display_idx, MSG_TYPE_SET_TEMPORARY_MSG, true, __func__);
		if (!ret) {
			return;
		} else if (data->msg_buf[light_idx][0] < 0) {
			pr_err("[SSC_FAC] %s[%d]: UB is not matched!(%d)\n", __func__,
				display_idx, data->msg_buf[light_idx][0]);
#if IS_ENABLED(CONFIG_SUPPORT_PROX_CALIBRATION)
			if (light_idx == MSG_LIGHT)
				prox_send_cal_data(data, MSG_PROX, false);
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
			else if (light_idx == MSG_LIGHT_SUB)
				prox_send_cal_data(data, MSG_PROX_SUB, false);
#endif
#endif /* CONFIG_SUPPORT_PROX_CALIBRATION */
			continue;
		}

		msg_buf[0] = OPTION_TYPE_SET_LIGHT_CAL;
		if (light_idx == MSG_LIGHT) {
			msg_buf[1] = data->light_cal_result = data->msg_buf[light_idx][0];
			msg_buf[2] = data->light_cal1 = data->msg_buf[light_idx][1];
			msg_buf[3] = data->light_cal2 = data->msg_buf[light_idx][2];
			msg_buf[4] = data->copr_w = data->msg_buf[light_idx][3];
		} else {
			msg_buf[1] = data->sub_light_cal_result = data->msg_buf[light_idx][0];
			msg_buf[2] = data->sub_light_cal1 = data->msg_buf[light_idx][1];
			msg_buf[3] = data->sub_light_cal2 = data->msg_buf[light_idx][2];
			msg_buf[4] = data->sub_copr_w = data->msg_buf[light_idx][3];
		}

#if IS_ENABLED(CONFIG_SUPPORT_PROX_CALIBRATION)
		if (light_idx == MSG_LIGHT) {
			data->prox_cal = data->msg_buf[light_idx][4];
			prox_send_cal_data(data, MSG_PROX, true);
		}
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
		else if (light_idx == MSG_LIGHT_SUB) {
			data->prox_sub_cal = data->msg_buf[light_idx][4];
			prox_send_cal_data(data, MSG_PROX_SUB, true);
		}
#endif
#endif /* CONFIG_SUPPORT_PROX_CALIBRATION */
		if (msg_buf[1] == LIGHT_CAL_PASS)
			light_send_message(data, msg_buf, sizeof(msg_buf),
				display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);
	}
}

void light_cal_init_work(struct adsp_data *data)
{
	data->light_cal_result = LIGHT_CAL_FAIL;
	data->light_cal1 = -1;
	data->light_cal2 = -1;
	data->copr_w = -1;

	data->sub_light_cal_result = LIGHT_CAL_FAIL;
	data->sub_light_cal1 = -1;
	data->sub_light_cal2 = -1;
	data->sub_copr_w = -1;

	schedule_delayed_work(&data->light_cal_work, msecs_to_jiffies(8000));
}

static ssize_t light_cal_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), cmd = OPTION_TYPE_GET_LIGHT_CAL, cur_lux;
	bool ret;

	ret = light_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		cur_lux = -1;
	else
		cur_lux = data->msg_buf[light_idx][4];

	if (light_idx == MSG_LIGHT) {
		pr_info("[SSC_FAC] %s[%d]: cal_data (P/F: %d, Cal1: %d, Cal2: %d, COPR_W: %d, ACL: %d, cur lux: %d)\n",
			__func__, display_idx, data->light_cal_result, data->light_cal1,
			data->light_cal2, data->copr_w % 1000, data->copr_w / 1000, cur_lux);

		return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n",
			data->light_cal_result, data->light_cal2, cur_lux);
	} else {
		pr_info("[SSC_FAC] %s[%d]: cal_data (P/F: %d, Cal1: %d, Cal2: %d, COPR_W: %d, ACL: %d, cur lux: %d)\n",
			__func__, display_idx, data->sub_light_cal_result, data->sub_light_cal1,
			data->sub_light_cal2, data->sub_copr_w % 1000, data->sub_copr_w / 1000, cur_lux);

		return snprintf(buf, PAGE_SIZE, "%d,%d,%d\n",
			data->sub_light_cal_result, data->sub_light_cal2, cur_lux);
	}
}

static ssize_t light_cal_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);
	int32_t cmd = OPTION_TYPE_GET_LIGHT_CAL, new_value, msg_buf[5];
	bool ret;

	if (sysfs_streq(buf, "0"))
		new_value = 0;
	else if (sysfs_streq(buf, "1"))
		new_value = 1;
	else
		return size;

	pr_info("[SSC_FAC] %s[%d]: cmd: %d\n", __func__, display_idx, new_value);

	if (new_value == 1) {
		ret = light_send_message(data, &cmd, sizeof(int32_t),
			display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
		if (!ret)
			return size;

		pr_info("[SSC_FAC] %s[%d]: (P/F: %d, Cal1: %d, Cal2: %d, COPR_W: %d, ACL: %d)\n",
			__func__, display_idx, data->msg_buf[light_idx][0],
			data->msg_buf[light_idx][1],
			data->msg_buf[light_idx][2],
			data->msg_buf[light_idx][3],
			data->pre_acl_mode[display_idx]);

		if (data->msg_buf[light_idx][0] == LIGHT_CAL_PASS) {
			if (light_idx == MSG_LIGHT) {
				data->light_cal_result = data->msg_buf[light_idx][0];
				data->light_cal1 = data->msg_buf[light_idx][1];
				data->light_cal2 = data->msg_buf[light_idx][2];
				data->copr_w = data->pre_acl_mode[display_idx] * 1000 + data->msg_buf[light_idx][3];
			} else {
				data->sub_light_cal_result = data->msg_buf[light_idx][0];
				data->sub_light_cal1 = data->msg_buf[light_idx][1];
				data->sub_light_cal2 = data->msg_buf[light_idx][2];
				data->sub_copr_w = data->pre_acl_mode[display_idx] * 1000 + data->msg_buf[light_idx][3];
			}
		} else {
			return size;
		}
	} else {
		if (light_idx == MSG_LIGHT) {
			data->light_cal_result = LIGHT_CAL_FAIL;
			data->light_cal1 = 0;
			data->light_cal2 = 0;
			data->copr_w = 0;
		} else {
			data->sub_light_cal_result = LIGHT_CAL_FAIL;
			data->sub_light_cal1 = 0;
			data->sub_light_cal2 = 0;
			data->sub_copr_w = 0;
		}
	}

	msg_buf[0] = OPTION_TYPE_SAVE_LIGHT_CAL;
	if (light_idx == MSG_LIGHT) {
		msg_buf[1] = data->light_cal_result;
		msg_buf[2] = data->light_cal1;
		msg_buf[3] = data->light_cal2;
		msg_buf[4] = data->copr_w;
	} else {
		msg_buf[1] = data->sub_light_cal_result;
		msg_buf[2] = data->sub_light_cal1;
		msg_buf[3] = data->sub_light_cal2;
		msg_buf[4] = data->sub_copr_w;
	}

	ret = light_send_message(data, msg_buf, sizeof(msg_buf),
		display_idx, MSG_TYPE_SET_TEMPORARY_MSG, true, __func__);
	if (!ret)
		return size;

	msg_buf[0] = OPTION_TYPE_SET_LIGHT_CAL;
	light_send_message(data, msg_buf, sizeof(msg_buf), display_idx, MSG_TYPE_OPTION_DEFINE, false, __func__);

	return size;
}

static ssize_t light_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx);
	int32_t cmd = OPTION_TYPE_GET_LIGHT_CAL, test_value;
	bool ret;

	ret = light_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		test_value = -1;
	else
		test_value = data->msg_buf[light_idx][2];

	pr_info("[SSC_FAC] %s[%d]: test_data (Cal1: %d, Cal2: %d, COPR_W: %d, ACL: %d, 16ms lux: %d)\n",
		__func__, display_idx, data->light_cal1, data->light_cal2,
		data->copr_w % 1000, data->copr_w / 1000, test_value);

	return snprintf(buf, PAGE_SIZE, "%d, %d, %d, %d\n",
		data->light_cal1, data->light_cal2, data->copr_w % 1000, test_value);
}

static DEVICE_ATTR(light_test, 0444, light_test_show, NULL);
#endif /* CONFIG_SUPPORT_LIGHT_CALIBRATION */

static ssize_t light_trim_check_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint16_t light_idx = get_light_sidx(data);
	int32_t display_idx = light_get_display_idx_from_sidx(light_idx), cmd = OPTION_TYPE_GET_TRIM_CHECK;
	bool ret;

	ret = light_send_message(data, &cmd, sizeof(int32_t), display_idx, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "NG\n");

	pr_info("[SSC_FAC] %s[%d]: [%s]: 0x%x, 0x%x\n",
		__func__, display_idx, (data->msg_buf[light_idx][0] > 0) ? "TRIM" : "UNTRIM",
		(uint16_t)data->msg_buf[light_idx][1], (uint16_t)data->msg_buf[light_idx][2]);

	return snprintf(buf, PAGE_SIZE, "%s\n",	(data->msg_buf[light_idx][0] > 0) ? "TRIM" : "UNTRIM");
}

#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
static ssize_t light_sub_als_lux_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	int32_t cmd = OPTION_TYPE_GET_SUB_ALS_LUX;
	bool ret;

	ret = light_send_message(data, &cmd, sizeof(int32_t), SUB_DISPLAY_IDX, MSG_TYPE_GET_CAL_DATA, true, __func__);
	if (!ret)
		return snprintf(buf, PAGE_SIZE, "-1\n");

	return snprintf(buf, PAGE_SIZE, "%d\n", data->msg_buf[MSG_LIGHT_SUB][0]);
}
#endif

#if IS_ENABLED(CONFIG_SUPPORT_LIGHT_CALIBRATION)
static DEVICE_ATTR(light_cal, 0664, light_cal_show, light_cal_store);
#endif
static DEVICE_ATTR(lcd_onoff, 0220, NULL, light_lcd_onoff_store);
static DEVICE_ATTR(hallic_info, 0220, NULL, light_hallic_info_store);
static DEVICE_ATTR(light_circle, 0444, light_circle_show, NULL);
#if IS_ENABLED(CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR) || IS_ENABLED(CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR)
static DEVICE_ATTR(read_copr, 0664, light_read_copr_show, light_read_copr_store);
static DEVICE_ATTR(test_copr, 0444, light_test_copr_show, NULL);
static DEVICE_ATTR(boled_enable, 0220, NULL, light_boled_enable_store);
static DEVICE_ATTR(copr_roix, 0444, light_copr_roix_show, NULL);
#endif
#if IS_ENABLED(CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR)
static DEVICE_ATTR(sensorhub_ddi_spi_check, 0444, light_ddi_spi_check_show, NULL);
#endif
static DEVICE_ATTR(register_write, 0220, NULL, light_register_write_store);
static DEVICE_ATTR(register_read, 0664,	light_register_read_show, light_register_read_store);
static DEVICE_ATTR(vendor, 0444, light_vendor_show, NULL);
static DEVICE_ATTR(name, 0444, light_name_show, NULL);
static DEVICE_ATTR(lux, 0444, light_raw_data_show, NULL);
static DEVICE_ATTR(raw_data, 0444, light_raw_data_show, NULL);
static DEVICE_ATTR(dhr_sensor_info, 0444, light_get_dhr_sensor_info_show, NULL);
static DEVICE_ATTR(debug_info, 0664, light_debug_info_show, light_debug_info_store);
static DEVICE_ATTR(hyst, 0664, light_hyst_show, light_hyst_store);
static DEVICE_ATTR(screen_mode, 0664, light_screen_mode_show, light_screen_mode_store);
#if !IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR)
static DEVICE_ATTR(brightness, 0664, light_brightness_show, light_brightness_store);
#endif
static DEVICE_ATTR(trim_check, 0444, light_trim_check_show, NULL);
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
static DEVICE_ATTR(sub_als_lux, 0444, light_sub_als_lux_show, NULL);
#endif

static struct device_attribute *light_attrs[] = {
	&dev_attr_vendor,
	&dev_attr_name,
	&dev_attr_lux,
	&dev_attr_raw_data,
	&dev_attr_dhr_sensor_info,
	&dev_attr_register_write,
	&dev_attr_register_read,
	&dev_attr_lcd_onoff,
	&dev_attr_hallic_info,
	&dev_attr_light_circle,
#if IS_ENABLED(CONFIG_SUPPORT_AP_COPR_FOR_LIGHT_SENSOR) || IS_ENABLED(CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR)
	&dev_attr_read_copr,
	&dev_attr_test_copr,
	&dev_attr_boled_enable,
	&dev_attr_copr_roix,
#endif
#if IS_ENABLED(CONFIG_SUPPORT_DDI_COPR_FOR_LIGHT_SENSOR)
	&dev_attr_sensorhub_ddi_spi_check,
#endif
#if IS_ENABLED(CONFIG_SUPPORT_LIGHT_CALIBRATION)
	&dev_attr_light_cal,
#endif
#if IS_ENABLED(CONFIG_SUPPORT_LIGHT_CALIBRATION)
	&dev_attr_light_test,
#endif
	&dev_attr_debug_info,
	&dev_attr_hyst,
	&dev_attr_screen_mode,
#if !IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR)
	&dev_attr_brightness,
#endif
	&dev_attr_trim_check,
#if IS_ENABLED(CONFIG_SUPPORT_DUAL_OPTIC)
	&dev_attr_sub_als_lux,
#endif
	NULL,
};

int light_factory_init(void)
{
	adsp_factory_register(MSG_LIGHT, light_attrs);
#if IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR) && IS_ENABLED(CONFIG_SEC_PANEL_NOTIFIER_V2)
	panel_notifier_register(&light_panel_data_notifier);
#endif
	pr_info("[SSC_FAC] %s\n", __func__);

	return 0;
}

void light_factory_exit(void)
{
	adsp_factory_unregister(MSG_LIGHT);
#if IS_ENABLED(CONFIG_SUPPORT_BRIGHTNESS_NOTIFY_FOR_LIGHT_SENSOR) && IS_ENABLED(CONFIG_SEC_PANEL_NOTIFIER_V2)
	panel_notifier_unregister(&light_panel_data_notifier);
#endif
	pr_info("[SSC_FAC] %s\n", __func__);
}
