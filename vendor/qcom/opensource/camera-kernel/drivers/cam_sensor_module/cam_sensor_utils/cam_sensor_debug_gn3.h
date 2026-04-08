#ifndef _CAM_SENSOR_DEBUG_GN3_H_
#define _CAM_SENSOR_DEBUG_GN3_H_

const struct cam_sensor_i2c_reg_array gn3_page_4000_reg_array[] = {
	{0xFCFC, 0x4000, 0, 0},
};

const struct cam_sensor_i2c_reg_array gn3_page_2000_reg_array[] = {
	{0xFCFC, 0x2000, 0, 0},
};

const struct st_exposure_reg_dump_addr gn3_dump_addr_arr[] = {
	{ 0x034c, CAMERA_SENSOR_I2C_TYPE_WORD, "width" },
	{ 0x034e, CAMERA_SENSOR_I2C_TYPE_WORD, "height" },
};

#endif /* _CAM_SENSOR_DEBUG_GN3_H_ */