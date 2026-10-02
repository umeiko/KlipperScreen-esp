# KlipperScreen-esp Android 原生库：与 src/ports/desktop 同一套 UI/核心代码，
# 编出 libmain.so（SDLActivity 的 getLibraries() 加载 "SDL2" + "main"）。
# SDL2 以源码子工程构建（2.30.9，tools/fetch-android-deps.sh 拉到 third_party/SDL2）。
cmake_minimum_required(VERSION 3.16)
project(KlipperScreenEspAndroid C)

set(CMAKE_C_STANDARD 99)

# app/jni → 仓库根（jni → app → android → ports → src → 根）
get_filename_component(SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
get_filename_component(REPO_ROOT "${SRC_ROOT}/.." ABSOLUTE)

# ---------- 外部依赖（均不入库，fetch-android-deps.sh / CI 拉取） ----------
if(NOT SDL2_ROOT)
    if(DEFINED ENV{SDL2_ROOT})
        set(SDL2_ROOT "$ENV{SDL2_ROOT}")
    else()
        set(SDL2_ROOT "${REPO_ROOT}/third_party/SDL2")
    endif()
endif()
if(NOT EXISTS "${SDL2_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "SDL2 source tree not found at ${SDL2_ROOT}\n"
        "Run tools/fetch-android-deps.sh first (or pass -DSDL2_ROOT=...).")
endif()

set(CJSON_ROOT "${REPO_ROOT}/third_party/cjson" CACHE PATH "cJSON source tree")
if(NOT EXISTS "${CJSON_ROOT}/cJSON.c")
    message(FATAL_ERROR
        "cJSON not found at ${CJSON_ROOT}\nRun tools/fetch-android-deps.sh first.")
endif()

if(NOT EXISTS "${REPO_ROOT}/third_party/lvgl/CMakeLists.txt")
    message(FATAL_ERROR
        "LVGL not found at third_party/lvgl (fetch v9.3.0, same as desktop port).")
endif()

# ---------- SDL2（只要共享库） ----------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL2_ROOT}" "${CMAKE_BINARY_DIR}/sdl2" EXCLUDE_FROM_ALL)

# ---------- cJSON（单文件静态库） ----------
add_library(cjson STATIC "${CJSON_ROOT}/cJSON.c")
target_include_directories(cjson PUBLIC "${CJSON_ROOT}")

# ---------- LVGL（与 desktop 端口同版本同 lv_conf） ----------
set(DESKTOP_PORT "${SRC_ROOT}/ports/desktop")
# LVGL os_desktop.cmake 未指定配置时在「顶层工程目录」找 lv_conf.h——desktop
# 端口顶层恰好是 src/ports/desktop 所以能过；本工程顶层是 app/jni，必须显式
# 用 LV_BUILD_CONF_PATH 指到与 desktop 端口共用的那份配置
set(LV_BUILD_CONF_PATH "${DESKTOP_PORT}/lv_conf.h" CACHE PATH "" FORCE)
add_subdirectory("${REPO_ROOT}/third_party/lvgl" "${CMAKE_BINARY_DIR}/lvgl")
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC "${DESKTOP_PORT}")      # lv_conf.h
# LVGL 的 SDL 驱动源直接 include <SDL2/SDL.h>；源码树 include/ 下有全部公共头
#（SDL_config.h 由 SDL 构建生成并随 SDL2 目标的接口目录传播，见下 target_link_libraries）
target_include_directories(lvgl PRIVATE "${SDL2_ROOT}/include")
target_link_libraries(lvgl PRIVATE SDL2::SDL2)

# ---------- 应用本体：libmain.so ----------
file(GLOB UI_SRCS
    "${SRC_ROOT}/ui/*.c"
    "${SRC_ROOT}/ui/panels/*.c"
    "${SRC_ROOT}/ui/widgets/*.c"
    "${SRC_ROOT}/ui/assets/*.c"
)

# 与 desktop 端口 Linux/macOS 分支的产品目标同源：
# POSIX sockets 传输 + bambu stub；WiFi 用空实现（Android 无 netsh/nmcli，WiFi 由系统管理）。
add_library(main SHARED
    "${DESKTOP_PORT}/main.c"
    "${SRC_ROOT}/bsp/bsp_screen_power.c"
    "${SRC_ROOT}/bsp/bsp_display_color.c"
    "${SRC_ROOT}/bsp/desktop/bsp_sdl2.c"
    "${SRC_ROOT}/bsp/desktop/bsp_linux_host.c"
    "${SRC_ROOT}/bsp/desktop/bsp_conf_file.c"
    "${SRC_ROOT}/core/file_list_stream.c"
    "${SRC_ROOT}/core/app_settings.c"
    "${SRC_ROOT}/core/gcode_thumbnail.c"
    "${SRC_ROOT}/core/self_update.c"
    "${SRC_ROOT}/core/printer_model.c"
    "${SRC_ROOT}/core/klipper_api.c"
    "${SRC_ROOT}/core/moonraker_client_posix.c"
    "${SRC_ROOT}/core/bambu_status.c"
    "${SRC_ROOT}/core/bambu_monitor_stub.c"
    "${SRC_ROOT}/core/bambu_cloud_stub.c"
    "${DESKTOP_PORT}/bsp_wifi_stub.c"
    ${UI_SRCS}
)

target_include_directories(main PRIVATE
    "${DESKTOP_PORT}"
    "${SRC_ROOT}/bsp"
    "${SRC_ROOT}/core"
    "${SRC_ROOT}/ui"
    "${SRC_ROOT}/ui/panels"
    "${SRC_ROOT}/ui/widgets"
    "${SDL2_ROOT}/include"
)

target_link_libraries(main PRIVATE lvgl cjson SDL2::SDL2)
