/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by the Timmy contributors, 2026
 * SPDX-License-Identifier: Apache-2.0
 */

#include "timmy_console.h"

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

esp_err_t timmy_console_install(size_t rx_buf)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = rx_buf;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
#if CONFIG_VFS_SUPPORT_IO && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
    /* IDF builds this VFS only for a USB Serial/JTAG console. */
    if (err == ESP_OK) {
        /* stdout/logs must share the driver's TX ring with console writes,
         * rather than writing directly to its hardware FIFO. */
        usb_serial_jtag_vfs_use_driver();
    }
#endif
    return err;
}

bool timmy_console_getc(uint8_t *c)
{
    return usb_serial_jtag_read_bytes(c, 1, portMAX_DELAY) == 1;
}

/* A stalled host must not keep the UI snapshot timer holding its lock.
 * Queue each line in one driver call: the ring takes all of it or none. */
bool timmy_console_write(const void *buf, size_t n)
{
    if (!n) {
        return true;
    }
    int written = usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(200));
    return written > 0 && (size_t)written == n;
}

bool timmy_console_host(void)
{
    return usb_serial_jtag_is_connected();
}

