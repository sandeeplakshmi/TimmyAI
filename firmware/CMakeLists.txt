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

cmake_minimum_required(VERSION 3.16)
# One board: plain `idf.py build` picks its configuration layers.
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;devices/sdkconfig.timmy;devices/sdkconfig.timmy-waveshare-s3-175c")
endif()
if(NOT IDF_TARGET)
    set(IDF_TARGET "esp32s3")
endif()
# Private add-on components (docs/CUSTOMIZING.md): TIMMY_EXTRA_COMPONENTS (a
# list of component directories or folders of them), else $TIMMY_HOME/firmware-extras
# (TIMMY_HOME defaults to ~/.timmy) when it exists.
if(DEFINED ENV{TIMMY_EXTRA_COMPONENTS})
    set(timmy_extras "$ENV{TIMMY_EXTRA_COMPONENTS}")
else()
    if(DEFINED ENV{TIMMY_HOME})
        set(timmy_home "$ENV{TIMMY_HOME}")
    else()
        set(timmy_home "$ENV{HOME}/.timmy")
    endif()
    if(EXISTS "${timmy_home}/firmware-extras")
        set(timmy_extras "${timmy_home}/firmware-extras")
    endif()
endif()
if(timmy_extras)
    string(REPLACE ":" ";" timmy_extras "${timmy_extras}")
    message(STATUS "Timmy add-on components: ${timmy_extras}")
    list(APPEND EXTRA_COMPONENT_DIRS ${timmy_extras})
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/project.cmake")
project(timmy-gadget)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/validate_config.cmake")
