cmake_minimum_required(VERSION 3.16)

# ESP32 后端工程（ESP-IDF v5.5.5）
# 共享源码在 src/ 下，以 IDF 组件形式引入
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/entry
    ${CMAKE_CURRENT_SOURCE_DIR}/bambu
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ui
    ${CMAKE_CURRENT_SOURCE_DIR}/../../bsp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(klipper_remote_display)
