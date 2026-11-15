# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Modified by the Timmy contributors, 2026
# SPDX-License-Identifier: Apache-2.0

# Timmy: the voice companion UI for the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
# Push-to-talk turns go to Timmy over a WebSocket (timmy_chat_server.c).
# Managed components (LVGL, codecs, BSP) come from idf_component.yml.
set(srcs
    "timmy_app.c"
    "timmy_board.c"
    "timmy_console.c"
    "timmy_state.c"
    "timmy_battery.c"
    "timmy_audio.c"
    "timmy_pmu.c"
    "timmy_input.c"
    "timmy_voice.c"
    "timmy_adpcm.c"
    "timmy_settings.c"
    "timmy_text.c"
    "timmy_lang.c"
    "timmy_link.c"
    "timmy_ble.c"
    "timmy_ui.c"
    "timmy_menu.c"
    "timmy_settings_ui.c"
    "timmy_keypad.c"
    "timmy_lv_mem.c"
    "timmy_chat_server.c"
    "timmy_chat_text.c"
    "timmy_imu.c"
    "timmy_avatar.c"
    "../../avatar/timmy_eyes.c"
    "boards/board_waveshare_s3_175c.c"
    "boards/timmy_lcd_bands.c")

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    LDFRAGMENTS "linker.lf"   # CONFIG_TIMMY_STATIC_BUFFERS_IN_PSRAM
    WHOLE_ARCHIVE   # timmy_lv_mem.c provides symbols only LVGL references
    REQUIRES esp_driver_gpio esp_driver_i2c
    PRIV_REQUIRES
        esp_timer esp_driver_usb_serial_jtag esp_psram nvs_flash esp_pm esp_event esp_netif
        bt esp-tls mbedtls esp_app_format esp_http_client espressif__esp_websocket_client
        noise_core minimp3
        esp_lcd esp_adc esp_driver_i2s esp_driver_ledc esp_driver_spi esp_driver_uart esp_wifi
        esp_driver_pcnt efuse
)

# LVGL spins forever on a failed assert unless LV_ASSERT_HANDLER is set,
# which its Kconfig can't do. Abort, so the panic handler prints a backtrace
# and reboots. PUBLIC reaches LVGL and every file that includes lvgl.h.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${lvgl_lib} PUBLIC
    "LV_ASSERT_HANDLER_INCLUDE=<stdlib.h>"
    "LV_ASSERT_HANDLER=abort()\;")

# Timmy's eyes implement timmy_pixel.h; built with the functions renamed so
# timmy_avatar.c can offer them beside faces an add-on registers.
set(eyes_renames "")
foreach(fn IN ITEMS render scale set_size accent working_accent full_frame)
    list(APPEND eyes_renames "timmy_pixel_${fn}=timmy_avatar_eyes_${fn}")
endforeach()
set_source_files_properties("../../avatar/timmy_eyes.c" PROPERTIES COMPILE_DEFINITIONS "${eyes_renames}")

# timmy_chat_server.c: the WebSocket client's task gets a PSRAM stack.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xTaskCreatePinnedToCore")
