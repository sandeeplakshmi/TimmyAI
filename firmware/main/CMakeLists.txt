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

set(GADGET_SRCS
    "main.c"
    "app.c"
    "ble_server.c"
    "ble_basic.c"
    "ble_central.c"
    "ble_client.c"
    "ble_commands.c"
    "ble_coex.c"
    "ghash32.c"
    "fast_gcm.c"
    "wifi_mgr.c"
    "wifi_known.c"
    "identity.c"
    "config_store.c"
    "vm_api.c"
    "diagnostic_log.c"
    "link_events.c"
    "link_probe.c"
    "stack_monitor.c"
    "bug_report.c"
    "ota.c"
    "image_fetch.c"
    "button.c"
    "tunnel_netif.c"
    "net_discovery.c"
    "noise_control.cpp"
    "noise_tunnel.cpp"
    "link_pairing.c"
    "pairing_signer_policy.c"
    "pairing_transcript.c"
    "factory_test.c"
)

set(GADGET_REQUIRES
    nvs_flash
    esp_wifi
    esp_event
    esp_netif
    esp_http_client
    esp_https_ota
    app_update
    esp_app_format
    esp-tls
    noise_core
    product_info
    bt
    esp_psram
    esp_coex
    driver
    esp_adc
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_ledc
    esp_driver_spi
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_lcd
    lwip
    mbedtls
    efuse
    timmy
    esp_driver_i2s
    esp_driver_pcnt
)

list(APPEND GADGET_SRCS "led_status.c" "timmy_glue.c")

idf_component_register(
    SRCS ${GADGET_SRCS}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../avatar"
    REQUIRES ${GADGET_REQUIRES}
)

# device.guide's Markdown, embedded as timmy_device_guide_md (NUL-terminated).
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/device_guide.cmake")
idf_build_get_property(gadget_project_dir PROJECT_DIR)
timmy_device_guide_assemble("${CMAKE_CURRENT_BINARY_DIR}/timmy_device_guide.md" "${gadget_project_dir}")
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/timmy_device_guide.md" TEXT)

# fast_gcm.c takes over ESP-IDF's one-shot AES-GCM entry points (PSA's AES-GCM
# driver calls them across archives, so the linker can redirect them).
if(CONFIG_HOMEHUB_FAST_GCM)
    # -u pulls fast_gcm.o out of libmain: nothing else in main references it,
    # and the references the wrap creates come from later archives.
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=esp_aes_gcm_crypt_and_tag"
        "-Wl,--wrap=esp_aes_gcm_auth_decrypt"
        "-Wl,-u,__wrap_esp_aes_gcm_crypt_and_tag"
        "-Wl,-u,__wrap_esp_aes_gcm_auth_decrypt")
endif()

# ble_coex.c sits between the BLE controller and the coexistence scheduler
# (both in prebuilt archives, so the linker can redirect the calls).
if(CONFIG_HOMEHUB_BLE_COEX)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=coex_schm_status_bit_set"
        "-Wl,--wrap=coex_schm_status_bit_clear"
        "-Wl,-u,__wrap_coex_schm_status_bit_set"
        "-Wl,-u,__wrap_coex_schm_status_bit_clear")
endif()
