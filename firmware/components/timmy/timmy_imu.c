/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 the Timmy contributors
 */
/*
 * The QMI8658 accelerometer on the Waveshare 1.75C's shared I2C bus, for
 * Timmy's eyes: which way the board leans, and shakes. Probed on first use;
 * without one (other boards, or no answer) timmy_imu_accel() returns false
 * and the eyes simply don't tilt.
 */

#include "timmy_imu.h"

#include "sdkconfig.h"


#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "timmy_imu";

#define REG_WHO_AM_I 0x00   /* reads 0x05 */
#define REG_CTRL1 0x02      /* bit 6: register address auto-increment */
#define REG_CTRL2 0x03      /* accelerometer: full scale [6:4], data rate [3:0] */
#define REG_CTRL7 0x08      /* bit 0: accelerometer on */
#define REG_AX_L 0x35       /* AX, AY, AZ, little-endian int16 */
#define LSB_PER_G 8192.0f   /* +-4 g */

static i2c_master_dev_handle_t s_dev;
static int s_state;         /* 0 not tried, 1 ready, -1 none */

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 20);
}

static bool probe(i2c_master_bus_handle_t bus, uint16_t addr)
{
    if (i2c_master_probe(bus, addr, 20) != ESP_OK) return false;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
    uint8_t reg = REG_WHO_AM_I, who = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &who, 1, 20) == ESP_OK && who == 0x05) {
        ESP_LOGI(TAG, "QMI8658 at 0x%02x", addr);
        return true;
    }
    ESP_LOGW(TAG, "0x%02x answers but isn't a QMI8658 (id 0x%02x)", addr, who);
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

static bool start(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus || !(probe(bus, 0x6B) || probe(bus, 0x6A))) {
        ESP_LOGW(TAG, "no QMI8658 on the I2C bus: tilt and shake off");
        return false;
    }
    if (wr(REG_CTRL1, 0x40) != ESP_OK || wr(REG_CTRL2, 0x16) != ESP_OK || wr(REG_CTRL7, 0x01) != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 setup failed: tilt and shake off");
        return false;
    }
    return true;
}

bool timmy_imu_accel(float g[3])
{
    if (s_state == 0) s_state = start() ? 1 : -1;
    if (s_state < 0) return false;
    uint8_t reg = REG_AX_L, raw[6];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, raw, sizeof(raw), 20) != ESP_OK) return false;
    for (int i = 0; i < 3; i++) {
        g[i] = (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8) / LSB_PER_G;
    }
    return true;
}

