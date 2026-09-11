cmake_minimum_required(VERSION 3.16)
project(klipper_remote_desktop C)

# SDL2 desktop ports share every UI source. On Windows the primary target is
# a real Moonraker controller; a separately named simulator keeps mock data an
# explicit development tool.
set(CMAKE_C_STANDARD 99)

set(SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(REPO_ROOT ${SRC_ROOT}/..)

add_subdirectory(${REPO_ROOT}/third_party/lvgl lvgl)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SDL2 REQUIRED)

# LVGL 的 SDL 驱动源直接 include <SDL2/SDL.h>；brew 前缀不在 clang 默认搜索路径，
# 头文件目录要显式给 lvgl 目标（MSYS2 能过是因为 /ucrt64/include 是编译器默认路径）
target_include_directories(lvgl PRIVATE ${SDL2_INCLUDE_DIRS})

file(GLOB UI_SRCS
    ${SRC_ROOT}/ui/*.c
    ${SRC_ROOT}/ui/panels/*.c
    ${SRC_ROOT}/ui/widgets/*.c
    ${SRC_ROOT}/ui/assets/*.c
)

set(DESKTOP_COMMON_SRCS
    main.c
    ${SRC_ROOT}/bsp/bsp_screen_power.c
    ${SRC_ROOT}/bsp/desktop/bsp_sdl2.c
    ${SRC_ROOT}/bsp/desktop/bsp_wifi_windows.c
    ${SRC_ROOT}/bsp/desktop/bsp_wifi_linux.c
    ${SRC_ROOT}/bsp/desktop/bsp_conf_file.c
    ${SRC_ROOT}/core/app_settings.c
    ${UI_SRCS}
)

# macOS 没有桌面 WiFi 后端（windows=netsh / linux=nmcli 都带 OS 守卫，mac 全编空），
# 编一个空实现补符号：WiFi 页图标常灰、扫描报失败。
if(APPLE)
    list(APPEND DESKTOP_COMMON_SRCS bsp_wifi_stub.c)
endif()

function(configure_desktop_target target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SRC_ROOT}/bsp
        ${SRC_ROOT}/core
        ${SRC_ROOT}/ui
        ${SRC_ROOT}/ui/panels
        ${SRC_ROOT}/ui/widgets
        ${SDL2_INCLUDE_DIRS}
    )
    target_link_libraries(${target} PRIVATE lvgl ${SDL2_LIBRARIES})
    if(WIN32)
        target_link_libraries(${target} PRIVATE mingw32 SDL2::SDL2main)
    elseif(APPLE AND TARGET SDL2::SDL2main)
        # SDL 官方建议 macOS 也链 SDL2main（Cocoa 启动/事件循环初始化）
        target_link_libraries(${target} PRIVATE SDL2::SDL2main)
    endif()
endfunction()

if(WIN32)
    find_package(cJSON CONFIG REQUIRED)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(OpenSSL REQUIRED)

    # 产品版使用 Windows GUI 子系统，双击启动时不创建控制台窗口。
    # simulator 保持默认 Console 子系统，便于查看 stdout 调试信息。
    add_executable(klipper_remote_desktop WIN32
        ${DESKTOP_COMMON_SRCS}
        ${SRC_ROOT}/core/printer_model.c
        ${SRC_ROOT}/core/klipper_api.c
        ${SRC_ROOT}/core/moonraker_client_winhttp.c
        ${SRC_ROOT}/core/bambu_status.c
        bambu_cloud_winhttp.c
        bambu_monitor_openssl.c
    )
    configure_desktop_target(klipper_remote_desktop)
    target_include_directories(klipper_remote_desktop PRIVATE ${CJSON_INCLUDE_DIR}/cjson)
    target_link_libraries(klipper_remote_desktop PRIVATE cjson-static winhttp crypt32
        OpenSSL::SSL OpenSSL::Crypto ws2_32)

    add_executable(klipper_remote_simulator
        ${DESKTOP_COMMON_SRCS}
        ${SRC_ROOT}/core/printer_mock.c
        ${SRC_ROOT}/core/moonraker_client_stub.c
        ${SRC_ROOT}/core/bambu_status.c
        ${SRC_ROOT}/core/bambu_monitor_stub.c
        bambu_cloud_winhttp.c
    )
    configure_desktop_target(klipper_remote_simulator)
    target_compile_definitions(klipper_remote_simulator PRIVATE KLIPPER_DESKTOP_SIMULATOR=1)
    target_include_directories(klipper_remote_simulator PRIVATE ${CJSON_INCLUDE_DIR}/cjson)
    target_link_libraries(klipper_remote_simulator PRIVATE cjson-static winhttp crypt32)
else()
    # macOS / Linux：POSIX sockets 原生传输（moonraker_client_posix.c），
    # 另保留 mock 数据的 simulator 目标作开发/布局工具。
    if(APPLE)
        # brew 前缀（arm64=/opt/homebrew，intel=/usr/local）不在部分
        # CMake 版本的默认搜索路径里，显式补上
        list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
    endif()

    find_package(cJSON CONFIG QUIET)
    if(TARGET cjson)
        # brew / 发行版 CMake 配置包：shared 目标自带 include 目录（.../include/cjson）
        set(CJSON_LINK cjson)
    elseif(TARGET cjson-static)
        set(CJSON_LINK cjson-static)
    else()
        # 无 CMake 配置包时的兜底（手工编译安装 / 散装头文件+库）
        find_path(CJSON_INCLUDE_DIR NAMES cJSON.h PATH_SUFFIXES cjson)
        find_library(CJSON_LIBRARY NAMES cjson)
        if(NOT CJSON_INCLUDE_DIR OR NOT CJSON_LIBRARY)
            message(FATAL_ERROR "cJSON not found (brew install cjson / apt install libcjson-dev)")
        endif()
        set(CJSON_LINK ${CJSON_LIBRARY})
    endif()

    find_package(Threads REQUIRED)

    add_executable(klipper_remote_desktop
        ${DESKTOP_COMMON_SRCS}
        ${SRC_ROOT}/core/printer_model.c
        ${SRC_ROOT}/core/klipper_api.c
        ${SRC_ROOT}/core/moonraker_client_posix.c
        ${SRC_ROOT}/core/bambu_status.c
        ${SRC_ROOT}/core/bambu_monitor_stub.c
        ${SRC_ROOT}/core/bambu_cloud_stub.c
    )
    configure_desktop_target(klipper_remote_desktop)
    # 代码写的是 #include "cJSON.h"，而头文件实际装在 include/cjson/ 子目录。
    # config 包会把 CJSON_INCLUDE_DIR 预设成 .../include 本体（不含子目录），
    # 所以换个变量名强制真实搜索（brew 实测踩坑）
    find_path(CJSON_HEADER_DIR NAMES cJSON.h PATH_SUFFIXES cjson)
    if(NOT CJSON_HEADER_DIR)
        message(FATAL_ERROR "cJSON headers not found (brew install cjson / apt install libcjson-dev)")
    endif()
    target_include_directories(klipper_remote_desktop PRIVATE ${CJSON_HEADER_DIR})
    target_link_libraries(klipper_remote_desktop PRIVATE ${CJSON_LINK} Threads::Threads)

    add_executable(klipper_remote_simulator
        ${DESKTOP_COMMON_SRCS}
        ${SRC_ROOT}/core/printer_mock.c
        ${SRC_ROOT}/core/moonraker_client_stub.c
        ${SRC_ROOT}/core/bambu_status.c
        ${SRC_ROOT}/core/bambu_monitor_stub.c
        ${SRC_ROOT}/core/bambu_cloud_stub.c
    )
    configure_desktop_target(klipper_remote_simulator)
    target_compile_definitions(klipper_remote_simulator PRIVATE KLIPPER_DESKTOP_SIMULATOR=1)
    target_include_directories(klipper_remote_simulator PRIVATE ${CJSON_HEADER_DIR})
    target_link_libraries(klipper_remote_simulator PRIVATE ${CJSON_LINK})
endif()
