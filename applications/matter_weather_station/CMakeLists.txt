#
# Copyright (c) 2021 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE ZDebug)
endif()

set(CONF_FILE           ${CMAKE_CURRENT_SOURCE_DIR}/configuration/${BOARD}/prj_${CMAKE_BUILD_TYPE}.conf)
set(PM_STATIC_YML_FILE  ${CMAKE_CURRENT_SOURCE_DIR}/configuration/${BOARD}/pm_static.yml)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(matter-weather-station)

set(COMMON_ROOT ${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common)
set(NLIO_ROOT ${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/third_party/nlio/repo)
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/src/app/chip_data_model.cmake)

# NORDIC SDK APP START
target_include_directories(app PRIVATE
    src
    ${COMMON_ROOT}/src
    ${NLIO_ROOT}/include
    ${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/zzz_generated/app-common
)

target_sources(app PRIVATE
    src/app_task.cpp
    src/main.cpp
    src/zap-generated/attribute-size.cpp
    src/zap-generated/IMClusterCommandHandler.cpp
    src/zap-generated/callback-stub.cpp
    ${COMMON_ROOT}/src/thread_util.cpp
    ${COMMON_ROOT}/src/led_widget.cpp
    ${COMMON_ROOT}/src/dfu_over_smp.cpp
)

chip_configure_data_model(app
    INCLUDE_SERVER
    ZAP_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/weather-station.zap
)
# NORDIC SDK APP END
