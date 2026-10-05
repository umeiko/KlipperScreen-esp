# KlipperScreen-esp Android 原生库：与 src/ports/desktop 同一套 UI/核心代码，
# 编出 libmain.so（SDLActivity 的 getLibraries() 加载 "SDL2" + "main"）。
# SDL2 以源码子工程构建（2.30.9，tools/fetch-android-deps.sh 拉到 third_party/SDL2）。
cmake_minimum_required(VERSION 3.16)
project(KlipperScreenEspAndroid C)

set(CMAKE_C_STANDARD 99)

# bionic 的 aligned_alloc 是 API 28 才引入的；minSdk 21 下 LVGL SDL 驱动
#（lv_sdl_window.c 的 texture_resize）编译报 implicit-function-declaration
#（clang>=16 默认当成 error）。third_party/lvgl 是 vanilla 不能改，这里用
# 宏把它映射到上古 API 就有的 memalign，并强制预引 <malloc.h> 提供声明。
# 只影响 Android 目标编译；memalign 不要求是 size 的整数倍，语义兼容。
# 必须用 COMPILE_LANGUAGE 限定 C——LVGL 还带 .S 汇编（helium/neon blend），
# -include C 头进汇编单元会直接语法爆炸。
add_compile_options("$<$<COMPILE_LANGUAGE:C>:-include>" "$<$<COMPILE_LANGUAGE:C>:malloc.h>")
add_compile_definitions("$<$<COMPILE_LANGUAGE:C>:aligned_alloc=memalign>")

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

# ---------- mbedTLS（拓竹后端 TLS 传输；Linux 桌面共用同一实现） ----------
set(MBEDTLS_ROOT "${REPO_ROOT}/third_party/mbedtls")
set(CACERT_PEM "${REPO_ROOT}/third_party/cacert.pem")
if(NOT EXISTS "${MBEDTLS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "mbedtls not found at ${MBEDTLS_ROOT}\nRun tools/fetch-android-deps.sh first.")
endif()
if(NOT EXISTS "${CACERT_PEM}")
    message(FATAL_ERROR
        "cacert.pem not found at ${CACERT_PEM}\nRun tools/fetch-android-deps.sh first.")
endif()
# Mozilla CA bundle → C 数组（尾部补 NUL：mbedtls PEM 解析按含 NUL 长度计）
file(READ ${CACERT_PEM} BAMBU_CA_HEX HEX)
string(REGEX REPLACE "(..)" "0x\\1," BAMBU_CA_HEX "${BAMBU_CA_HEX}")
set(BAMBU_CA_BUNDLE_C "${CMAKE_BINARY_DIR}/bambu_ca_bundle.c")
file(WRITE "${BAMBU_CA_BUNDLE_C}"
    "const unsigned char bambu_ca_bundle[]={${BAMBU_CA_HEX}0x00};\n"
    "const unsigned int bambu_ca_bundle_len=sizeof(bambu_ca_bundle);\n")
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory("${MBEDTLS_ROOT}" "${CMAKE_BINARY_DIR}/mbedtls" EXCLUDE_FROM_ALL)

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
# POSIX sockets 传输；拓竹走 POSIX+mbedTLS 后端（与 Linux 上位机同一实现）；
# WiFi 用空实现（Android 无 netsh/nmcli，WiFi 由系统管理）。
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
    "${SRC_ROOT}/core/self_update_android.c"
    "${DESKTOP_PORT}/bambu_monitor_posix.c"
    "${DESKTOP_PORT}/bambu_cloud_posix.c"
    "${DESKTOP_PORT}/bambu_tls_mbedtls.c"
    "${BAMBU_CA_BUNDLE_C}"
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
    "${MBEDTLS_ROOT}/include"
)

target_link_libraries(main PRIVATE lvgl cjson SDL2::SDL2 mbedtls mbedx509 mbedcrypto)
