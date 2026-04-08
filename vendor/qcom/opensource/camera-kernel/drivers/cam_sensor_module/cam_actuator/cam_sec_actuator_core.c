// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <cam_sensor_cmn_header.h>
#include "cam_sec_actuator_core.h"
#include "cam_sensor_util.h"
#include "cam_trace.h"
#include "cam_common_util.h"
#include "cam_packet_util.h"

#if defined(CONFIG_SAMSUNG_ACTUATOR_READ_HALL_VALUE)
#define ACTUATOR_STATUS_REGISTER_ADDR (0x02)
#define ACTUATOR_HALL_REGISTER_ADDR (0x84)
#define ACTUATOR_RUMBA_Z1_HALL_REGISTER_ADDR (0x60B4) // Tele 5x Joint type

uint8_t ois_5x_vendor_id = 0;

static int32_t cam_sec_actuator_i2c_read(struct cam_actuator_ctrl_t *a_ctrl, uint32_t addr,
		uint32_t *data,
        enum camera_sensor_i2c_type addr_type,
        enum camera_sensor_i2c_type data_type)
{
	int rc = 0;

	if (a_ctrl == NULL) {
		CAM_ERR(CAM_ACTUATOR, "failed. a_ctrl is NULL");
		return -EINVAL;
	}

	rc = camera_io_dev_read(&a_ctrl->io_master_info, addr,
		(uint32_t *)data, addr_type, data_type, false);
	if (rc < 0) {
		CAM_ERR(CAM_ACTUATOR, "Failed to read 0x%x", addr);
	}

	return rc;
}

static int32_t cam_sec_actuator_get_status_for_hall_value(struct cam_actuator_ctrl_t *a_ctrl, uint16_t *info)
{
	int32_t rc = 0;
	uint32_t val = 0;

	rc = cam_sec_actuator_i2c_read(a_ctrl,
		ACTUATOR_STATUS_REGISTER_ADDR, &val,
		CAMERA_SENSOR_I2C_TYPE_BYTE, CAMERA_SENSOR_I2C_TYPE_BYTE);
	if (rc < 0) {
		CAM_ERR(CAM_ACTUATOR, "get status i2c read fail:%d", rc);
		return -EINVAL;
	}

	CAM_INFO(CAM_ACTUATOR, "[AF] val = 0x%x", val);

	*info = (val & 0x60);

	return rc;
}

static void cam_sec_actuator_busywait_for_hall_value(struct cam_actuator_ctrl_t *a_ctrl)
{
	uint16_t info = 0, status_check_count = 0;
	int32_t rc = 0;

	do {
		rc = cam_sec_actuator_get_status_for_hall_value(a_ctrl, &info);
		if (rc < 0) {
			CAM_ERR(CAM_ACTUATOR, "cam_actuator_get_status failed:%d", rc);
		}
		if  (info) {
			CAM_INFO(CAM_ACTUATOR, "[AF] Not Active");
			msleep(10);
		}

		status_check_count++;
	} while (info && status_check_count < 8);

	if (status_check_count == 8)
		CAM_ERR(CAM_ACTUATOR, "[AF] status check failed");
	else
		CAM_INFO(CAM_ACTUATOR, "[AF] Active");
}

int32_t cam_sec_actuator_read_hall_value(struct cam_actuator_ctrl_t *a_ctrl, uint16_t* af_hall_value)
{
	int32_t rc = 0;
	uint8_t value[2];
	uint16_t hallValue = 0;
	uint32_t addr = ACTUATOR_HALL_REGISTER_ADDR;
	uint32_t addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
	bool is_tele_5x_joint_type = false;

	if (a_ctrl == NULL) {
		CAM_ERR(CAM_ACTUATOR, "failed. a_ctrl is NULL");
		return -EINVAL;
	}

	if (a_ctrl->soc_info.index == SEC_TELE2_SENSOR) {
		is_tele_5x_joint_type = (ois_5x_vendor_id == 0x01) ? false : true;
		if (is_tele_5x_joint_type) {
			addr = ACTUATOR_RUMBA_Z1_HALL_REGISTER_ADDR;
			addr_type = CAMERA_SENSOR_I2C_TYPE_WORD;
		}
		else {
			addr = ACTUATOR_HALL_REGISTER_ADDR;
			addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
		}
		CAM_DBG(CAM_ACTUATOR,
			"Tele 5x OIS D.IC Vendor ID 0x%02X, addr 0x%04X, addr_type 0x%02X",
			ois_5x_vendor_id, addr, addr_type);
	}

	if (!is_tele_5x_joint_type) // joint type
		cam_sec_actuator_busywait_for_hall_value(a_ctrl);

#if defined(CONFIG_SEC_FACTORY)
	msleep(50);
#endif

	rc = camera_io_dev_read_seq(&a_ctrl->io_master_info,
		addr, value, addr_type, CAMERA_SENSOR_I2C_TYPE_BYTE, 2);

	if (is_tele_5x_joint_type) {
		hallValue = (uint16_t)value[1] << 6;
		hallValue |= (uint16_t)value[0] >> 2;
	}
	else
	{
		hallValue = (uint16_t)value[0] << 4;
		hallValue |= (uint16_t)value[1] >> 4;
	}

	if (rc < 0) {
		CAM_ERR(CAM_ACTUATOR, "get status i2c read fail:%d", rc);
		return -EINVAL;
	}

	CAM_INFO(CAM_ACTUATOR, "[AF] RAW data = %u (0x%02X, 0x%02X)",
		hallValue, value[0], value[1]);

	*af_hall_value = hallValue;

	return rc;
}
#endif
